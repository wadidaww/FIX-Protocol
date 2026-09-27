// =============================================================================
// FIX Protocol Engine - FileStore implementation
//
// Phase 3 (3.4): crash-safe sequence persistence (tmp+fsync+rename+dir
// fsync), strict seq-file validation that refuses to start instead of
// silently resetting (C10), checked message appends with optional fsync,
// sanitized collision-free filenames, explicit idempotent close().
// =============================================================================
#include "fix/store/file_store.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#endif

namespace fix {

namespace {

// --- tiny fd portability shim (POSIX vs MSVC CRT) ---------------------------
#ifdef _WIN32
int fd_open(const char *path, int flags, int mode) {
    return _open(path, flags, mode);
}
int fd_write(int fd, const void *buf, std::size_t len) {
    return _write(fd, buf, static_cast<unsigned>(len));
}
int fd_fsync(int fd) {
    return _commit(fd);
}
int fd_close(int fd) {
    return _close(fd);
}
#else
int fd_open(const char *path, int flags, int mode) {
    return ::open(path, flags, mode);
}
ssize_t fd_write(int fd, const void *buf, std::size_t len) {
    return ::write(fd, buf, len);
}
int fd_fsync(int fd) {
    return ::fsync(fd);
}
int fd_close(int fd) {
    return ::close(fd);
}
#endif

Result<void> write_all(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const auto n = fd_write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return make_unexpected(ErrorCode::StoreError);
        }
        if (n == 0)
            return make_unexpected(ErrorCode::StoreError);
        off += static_cast<std::size_t>(n);
    }
    return {};
}

// fsync an already-written file through a side fd (the message logs are
// owned by std::ofstream, which exposes no fd; fsync is inode-scoped, so a
// second handle flushing the same inode is exactly equivalent).
Result<void> fsync_path(const std::filesystem::path &p) {
#ifdef _WIN32
    const int fd = _open(p.string().c_str(), _O_WRONLY | _O_BINARY);
    if (fd < 0)
        return make_unexpected(ErrorCode::StoreError);
    const int rc = _commit(fd);
    _close(fd);
#else
    const int fd = ::open(p.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return make_unexpected(ErrorCode::StoreError);
    const int rc = ::fsync(fd);
    ::close(fd);
#endif
    if (rc != 0)
        return make_unexpected(ErrorCode::StoreError);
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Filenames
// ---------------------------------------------------------------------------
std::string FileStore::sanitize_component(std::string_view raw, std::size_t cap) {
    std::string out;
    out.reserve(raw.size() < cap ? raw.size() : cap);
    for (char c : raw) {
        if (out.size() >= cap)
            break;
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-')
            out.push_back(static_cast<char>(u));
        else
            out.push_back('_'); // '/', ':', '.', spaces, control bytes, ...
    }
    if (out.empty())
        out.push_back('_'); // keep the component visible & non-empty
    return out;
}

std::string FileStore::make_prefix(const SessionID &sid) {
    // '~' is the field separator BECAUSE sanitisation never lets it survive
    // inside a component: "A~B" as a single CompID cannot impersonate the
    // pair (A, B), so distinct SessionIDs always yield distinct filenames.
    std::string prefix = sanitize_component(sid.senderCompID);
    prefix += '~';
    prefix += sanitize_component(sid.targetCompID);
    if (!sid.qualifier.empty()) {
        prefix += '~';
        prefix += sanitize_component(sid.qualifier);
    }
    return prefix;
}

// ---------------------------------------------------------------------------
// Construction / open
// ---------------------------------------------------------------------------
FileStore::FileStore(std::filesystem::path dir, const SessionID &sid)
    : FileStore(std::move(dir), sid, Options{}) {}

FileStore::FileStore(std::filesystem::path dir, const SessionID &sid, Options opts)
    : dir_(std::move(dir)),
      opts_(opts) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec)
        throw std::system_error(ec,
                                "FileStore: cannot create store directory '" + dir_.string() + "'");

    const std::string prefix = make_prefix(sid);
    seqfile_ = dir_ / (prefix + ".seqnums");
    outfile_ = dir_ / (prefix + ".out.log");
    infile_ = dir_ / (prefix + ".in.log");

    // Refuse to start on a corrupt/truncated/unparsable seq file — NEVER a
    // silent reset to 1 (that is the sequence-hijack finding, C10).
    if (auto r = load_seqs(); !r)
        throw std::system_error(r.error(),
                                "FileStore: refusing to start — invalid sequence file '" +
                                    seqfile_.string() + "'");

    rebuild_index();

    out_stream_.open(outfile_, std::ios::binary | std::ios::app);
    in_stream_.open(infile_, std::ios::binary | std::ios::app);
    // A message log that cannot be opened is NOT fatal here: every
    // store_outbound/store_inbound call surfaces ErrorCode::StoreError as a
    // Result, so the failure reaches the session's error path per message.
}

Result<std::unique_ptr<FileStore>> FileStore::open(std::filesystem::path dir, const SessionID &sid,
                                                   Options opts) {
    try {
        return std::make_unique<FileStore>(std::move(dir), sid, opts);
    } catch (const std::system_error &e) {
        return std::unexpected(e.code());
    } catch (const std::exception &) {
        return std::unexpected(make_error_code(ErrorCode::StoreError));
    }
}

FileStore::~FileStore() {
    try {
        (void)close(); // fallback: the explicit close() is the contract
    } catch (...) {
        // A destructor must never throw.
    }
}

// ---------------------------------------------------------------------------
// Sequence persistence (crash-safe)
// ---------------------------------------------------------------------------
Result<void> FileStore::load_seqs() {
    std::error_code ec;
    const bool exists = std::filesystem::exists(seqfile_, ec);
    if (ec)
        return make_unexpected(ErrorCode::StoreError);
    if (!exists) {
        // Fresh session — a missing file is the only legal "start at 1".
        sender_seq_.store(1, std::memory_order_release);
        target_seq_.store(1, std::memory_order_release);
        return {};
    }

    std::ifstream f(seqfile_, std::ios::binary);
    if (!f.is_open())
        return make_unexpected(ErrorCode::StoreError); // exists but unreadable
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());

    // Strict validation: only digits and whitespace may appear at all (this
    // also rejects sign tricks before from_chars ever runs), then exactly
    // two unsigned values separated by whitespace, nothing after the second,
    // both >= 1. Anything else — empty file, one number, trailing garbage,
    // overflow, non-numeric — is corruption and REFUSES to start.
    SeqNum s = 0, t = 0;
    auto parse = [&]() -> bool {
        const char *p = content.data();
        const char *end = p + content.size();
        auto ws = [&] {
            while (p != end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                ++p;
        };
        for (char c : content) {
            const auto u = static_cast<unsigned char>(c);
            if (!std::isdigit(u) && c != ' ' && c != '\t' && c != '\r' && c != '\n')
                return false;
        }
        ws();
        if (p == end)
            return false;
        auto r1 = std::from_chars(p, end, s);
        if (r1.ec != std::errc{} || r1.ptr == p)
            return false;
        p = r1.ptr;
        if (p == end || !(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            return false; // the two numbers must be whitespace-separated
        ws();
        if (p == end)
            return false; // truncated: only one number present
        auto r2 = std::from_chars(p, end, t);
        if (r2.ec != std::errc{} || r2.ptr == p)
            return false;
        p = r2.ptr;
        while (p != end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
        if (p != end)
            return false; // trailing garbage
        return s >= 1 && t >= 1;
    };

    if (!parse())
        return make_unexpected(ErrorCode::StoreError);

    // Commit only after full validation — a failed parse leaves the current
    // values untouched.
    sender_seq_.store(s, std::memory_order_release);
    target_seq_.store(t, std::memory_order_release);
    return {};
}

Result<void> FileStore::persist_seqs() {
    // mutex_ held: both counters are written as ONE consistent snapshot.
    const std::string data = std::to_string(sender_seq_.load(std::memory_order_acquire)) + "\n" +
                             std::to_string(target_seq_.load(std::memory_order_acquire)) + "\n";

    // write-tmp + fsync + atomic rename + parent-dir fsync: the seq file
    // always holds a COMPLETE old or new value, even across kill -9/power.
    auto tmp = seqfile_;
    tmp += ".tmp";

#ifdef _WIN32
    const int fd = fd_open(tmp.string().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    const int fd = fd_open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    if (fd < 0)
        return make_unexpected(ErrorCode::StoreError);

    auto discard_tmp = [&]() {
        fd_close(fd);
        std::error_code ignore;
        std::filesystem::remove(tmp, ignore);
        return make_unexpected(ErrorCode::StoreError);
    };

    if (auto w = write_all(fd, data); !w)
        return discard_tmp();
    if (fd_fsync(fd) != 0)
        return discard_tmp();
    if (fd_close(fd) != 0) {
        std::error_code ignore;
        std::filesystem::remove(tmp, ignore);
        return make_unexpected(ErrorCode::StoreError);
    }

#ifdef _WIN32
    // ReplaceFile-style atomic swap (std::rename cannot overwrite on Win).
    if (!MoveFileExW(tmp.c_str(), seqfile_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignore;
        std::filesystem::remove(tmp, ignore);
        return make_unexpected(ErrorCode::StoreError);
    }
#else
    if (::rename(tmp.c_str(), seqfile_.c_str()) != 0) {
        std::error_code ignore;
        std::filesystem::remove(tmp, ignore);
        return make_unexpected(ErrorCode::StoreError);
    }
    // Parent-dir fsync makes the rename itself durable. Best effort: some
    // filesystems refuse directory fsync (EINVAL/ENOTSUP) and must not brick
    // the store — the atomicity guarantee above already covers kill -9.
    const int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
#endif
    return {};
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

// ---------------------------------------------------------------------------
// Sequence mutations
// ---------------------------------------------------------------------------
Result<void> FileStore::set_next_sender_seq_num(SeqNum n) {
    std::lock_guard lock(mutex_);
    if (closed_)
        return make_unexpected(ErrorCode::StoreError);
    const SeqNum prev = sender_seq_.load(std::memory_order_acquire);
    sender_seq_.store(n, std::memory_order_release);
    if (auto r = persist_seqs(); !r) {
        sender_seq_.store(prev, std::memory_order_release); // nothing changed
        return r;
    }
    return {};
}

Result<void> FileStore::set_next_target_seq_num(SeqNum n) {
    std::lock_guard lock(mutex_);
    if (closed_)
        return make_unexpected(ErrorCode::StoreError);
    const SeqNum prev = target_seq_.load(std::memory_order_acquire);
    target_seq_.store(n, std::memory_order_release);
    if (auto r = persist_seqs(); !r) {
        target_seq_.store(prev, std::memory_order_release);
        return r;
    }
    return {};
}

Result<void> FileStore::incr_sender_seq_num() {
    std::lock_guard lock(mutex_);
    if (closed_)
        return make_unexpected(ErrorCode::StoreError);
    const SeqNum prev = sender_seq_.load(std::memory_order_acquire);
    if (prev == std::numeric_limits<SeqNum>::max())
        return make_unexpected(ErrorCode::StoreError);
    sender_seq_.store(prev + 1, std::memory_order_release);
    if (auto r = persist_seqs(); !r) {
        sender_seq_.store(prev, std::memory_order_release);
        return r;
    }
    return {};
}

Result<void> FileStore::incr_target_seq_num() {
    std::lock_guard lock(mutex_);
    if (closed_)
        return make_unexpected(ErrorCode::StoreError);
    const SeqNum prev = target_seq_.load(std::memory_order_acquire);
    if (prev == std::numeric_limits<SeqNum>::max())
        return make_unexpected(ErrorCode::StoreError);
    target_seq_.store(prev + 1, std::memory_order_release);
    if (auto r = persist_seqs(); !r) {
        target_seq_.store(prev, std::memory_order_release);
        return r;
    }
    return {};
}

Result<SeqNum> FileStore::next_sender_seq_num_incr() {
    // 3.2: claim + persist as ONE step under the store mutex. On failure the
    // advance rolls back before the error is reported, so a failed persist
    // consumes NO sequence number (the caller may safely retry).
    std::lock_guard lock(mutex_);
    if (closed_)
        return std::unexpected(make_error_code(ErrorCode::StoreError));
    const SeqNum claimed = sender_seq_.load(std::memory_order_acquire);
    if (claimed == std::numeric_limits<SeqNum>::max())
        return std::unexpected(make_error_code(ErrorCode::StoreError));
    sender_seq_.store(claimed + 1, std::memory_order_release);
    if (auto r = persist_seqs(); !r) {
        sender_seq_.store(claimed, std::memory_order_release);
        return std::unexpected(r.error());
    }
    return claimed;
}

Result<void> FileStore::advance_next_target_seq_num(SeqNum n) {
    // 3.2: monotonic max-update, atomic with its persist.
    std::lock_guard lock(mutex_);
    if (closed_)
        return make_unexpected(ErrorCode::StoreError);
    const SeqNum cur = target_seq_.load(std::memory_order_acquire);
    if (n <= cur)
        return {}; // no-op: never rewinds
    target_seq_.store(n, std::memory_order_release);
    if (auto r = persist_seqs(); !r) {
        target_seq_.store(cur, std::memory_order_release);
        return r;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Message logs
// ---------------------------------------------------------------------------
Result<void> FileStore::store_outbound(SeqNum seq, const std::string &raw) {
    std::lock_guard lock(mutex_);
    if (closed_ || !out_stream_.is_open() || !out_stream_)
        // The stream latches in the failed state after a write error (ENOSPC,
        // EIO, ...): the error is RETURNED here instead of being swallowed
        // and the corrupt tail is never indexed. reset()/refresh() reopen.
        return make_unexpected(ErrorCode::StoreError);

    const auto pos = out_stream_.tellp();
    // Format: "<seq>|<raw>\n"
    out_stream_ << seq << '|' << raw << '\n';
    out_stream_.flush();
    if (!out_stream_)
        return make_unexpected(ErrorCode::StoreError); // nothing indexed

    if (opts_.fsync_messages) {
        if (auto r = fsync_path(outfile_); !r)
            return r;
    }
    out_index_[seq] = pos;
    return {};
}

Result<void> FileStore::store_inbound(SeqNum seq, const std::string &raw) {
    std::lock_guard lock(mutex_);
    if (closed_ || !in_stream_.is_open() || !in_stream_)
        return make_unexpected(ErrorCode::StoreError);
    in_stream_ << seq << '|' << raw << '\n';
    in_stream_.flush();
    if (!in_stream_)
        return make_unexpected(ErrorCode::StoreError);
    if (opts_.fsync_messages) {
        if (auto r = fsync_path(infile_); !r)
            return r;
    }
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
    if (closed_)
        return make_unexpected(ErrorCode::StoreError);
    sender_seq_.store(1, std::memory_order_release);
    target_seq_.store(1, std::memory_order_release);
    out_index_.clear();

    out_stream_.close();
    in_stream_.close();
    out_stream_.clear();
    in_stream_.clear();

    // Truncate files (checked — a failed truncate must not report success)
    std::error_code ec;
    if (std::filesystem::exists(outfile_, ec))
        std::filesystem::resize_file(outfile_, 0, ec);
    if (ec)
        return make_unexpected(ErrorCode::StoreError);
    if (std::filesystem::exists(infile_, ec))
        std::filesystem::resize_file(infile_, 0, ec);
    if (ec)
        return make_unexpected(ErrorCode::StoreError);

    if (auto r = persist_seqs(); !r)
        return r;

    out_stream_.open(outfile_, std::ios::binary | std::ios::app);
    in_stream_.open(infile_, std::ios::binary | std::ios::app);

    return {};
}

Result<void> FileStore::refresh() {
    std::lock_guard lock(mutex_);
    // Validates before committing: a seq file that went corrupt while we
    // were running yields an error, not a silent rebase (3.4).
    if (auto r = load_seqs(); !r)
        return r;
    rebuild_index();
    // Recover from a latched write error: reopen a stream that is in a
    // failed state so a transient ENOSPC does not permanently wedge sends.
    if (out_stream_.is_open() && !out_stream_) {
        out_stream_.close();
        out_stream_.clear();
        out_stream_.open(outfile_, std::ios::binary | std::ios::app);
    }
    if (in_stream_.is_open() && !in_stream_) {
        in_stream_.close();
        in_stream_.clear();
        in_stream_.open(infile_, std::ios::binary | std::ios::app);
    }
    return {};
}

Result<void> FileStore::close() {
    std::lock_guard lock(mutex_);
    if (closed_)
        return {}; // idempotent

    Result<void> first_error{};

    // Flush + fsync both message logs so nothing accepted is lost...
    if (out_stream_.is_open()) {
        out_stream_.flush();
        if (!out_stream_)
            first_error = make_unexpected(ErrorCode::StoreError);
        else if (auto r = fsync_path(outfile_); !r && !first_error)
            first_error = r;
        out_stream_.close();
        out_stream_.clear();
    }
    if (in_stream_.is_open()) {
        in_stream_.flush();
        if (!in_stream_)
            first_error = make_unexpected(ErrorCode::StoreError);
        else if (auto r = fsync_path(infile_); !r && !first_error)
            first_error = r;
        in_stream_.close();
        in_stream_.clear();
    }

    // ...then the final sequence snapshot (crash-safe protocol above).
    if (auto r = persist_seqs(); !r && !first_error)
        first_error = r;

    closed_ = true;
    return first_error;
}

} // namespace fix
