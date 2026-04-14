// =============================================================================
// FIX Protocol Engine - FileStore implementation
// =============================================================================
#include "fix/store/file_store.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace fix {

FileStore::FileStore(std::filesystem::path dir, const SessionID &sid)
    : dir_(std::move(dir)) {
    std::filesystem::create_directories(dir_);
    std::string prefix = sid.senderCompID + "-" + sid.targetCompID;
    seqfile_ = dir_ / (prefix + ".seqnums");
    outfile_ = dir_ / (prefix + ".out.log");
    infile_ = dir_ / (prefix + ".in.log");

    load_seqs();
    rebuild_index();

    out_stream_.open(outfile_, std::ios::binary | std::ios::app);
    in_stream_.open(infile_, std::ios::binary | std::ios::app);
}

FileStore::~FileStore() {
    try {
        persist_seqs();
    } catch (...) {}
}

void FileStore::load_seqs() {
    std::ifstream f(seqfile_);
    if (!f.is_open())
        return;
    SeqNum s = 1, t = 1;
    f >> s >> t;
    sender_seq_.store(s, std::memory_order_release);
    target_seq_.store(t, std::memory_order_release);
}

void FileStore::persist_seqs() {
    std::ofstream f(seqfile_, std::ios::trunc);
    f << sender_seq_.load(std::memory_order_acquire) << "\n"
      << target_seq_.load(std::memory_order_acquire) << "\n";
    f.flush();
}

void FileStore::rebuild_index() {
    std::ifstream f(outfile_, std::ios::binary);
    if (!f.is_open())
        return;

    out_index_.clear();
    std::streampos pos = 0;
    std::string line;
    while (std::getline(f, line)) {
        // Format: "<seq>|<raw>\n" – we stored it this way in store_outbound
        auto sep = line.find('|');
        if (sep == std::string::npos)
            continue;
        SeqNum seq = 0;
        std::from_chars(line.data(), line.data() + sep, seq);
        out_index_[seq] = pos;
        pos = f.tellg();
    }
}

void FileStore::set_next_sender_seq_num(SeqNum n) {
    sender_seq_.store(n, std::memory_order_release);
    std::lock_guard lock(mutex_);
    persist_seqs();
}

void FileStore::set_next_target_seq_num(SeqNum n) {
    target_seq_.store(n, std::memory_order_release);
    std::lock_guard lock(mutex_);
    persist_seqs();
}

void FileStore::incr_sender_seq_num() {
    sender_seq_.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lock(mutex_);
    persist_seqs();
}

void FileStore::incr_target_seq_num() {
    target_seq_.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lock(mutex_);
    persist_seqs();
}

Result<void> FileStore::store_outbound(SeqNum seq, const std::string &raw) {
    std::lock_guard lock(mutex_);
    if (!out_stream_.is_open())
        return make_unexpected(ErrorCode::StoreError);

    auto pos = out_stream_.tellp();
    // Format: "<seq>|<raw>\n"
    out_stream_ << seq << '|' << raw << '\n';
    out_stream_.flush();
    out_index_[seq] = pos;
    return {};
}

Result<void> FileStore::store_inbound(SeqNum seq, const std::string &raw) {
    std::lock_guard lock(mutex_);
    if (!in_stream_.is_open())
        return make_unexpected(ErrorCode::StoreError);
    in_stream_ << seq << '|' << raw << '\n';
    in_stream_.flush();
    return {};
}

Result<void> FileStore::get_messages(SeqNum begin, SeqNum end, MessageCallback cb) const {
    std::lock_guard lock(mutex_);
    SeqNum last = (end == 0) ? (out_index_.empty() ? 0 : out_index_.rbegin()->first) : end;

    std::ifstream f(outfile_, std::ios::binary);
    if (!f.is_open())
        return make_unexpected(ErrorCode::StoreError);

    auto it = out_index_.lower_bound(begin);
    for (; it != out_index_.end() && it->first <= last; ++it) {
        f.seekg(it->second);
        std::string line;
        std::getline(f, line);
        auto sep = line.find('|');
        if (sep == std::string::npos)
            continue;
        std::string raw = line.substr(sep + 1);
        cb(it->first, raw);
    }
    return {};
}

Result<void> FileStore::reset() {
    std::lock_guard lock(mutex_);
    sender_seq_.store(1, std::memory_order_release);
    target_seq_.store(1, std::memory_order_release);
    out_index_.clear();

    out_stream_.close();
    in_stream_.close();

    // Truncate files
    { std::ofstream f(outfile_, std::ios::trunc | std::ios::binary); }
    { std::ofstream f(infile_, std::ios::trunc | std::ios::binary); }

    persist_seqs();

    out_stream_.open(outfile_, std::ios::binary | std::ios::app);
    in_stream_.open(infile_, std::ios::binary | std::ios::app);

    return {};
}

Result<void> FileStore::refresh() {
    std::lock_guard lock(mutex_);
    load_seqs();
    rebuild_index();
    return {};
}

} // namespace fix
