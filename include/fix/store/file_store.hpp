#pragma once
// =============================================================================
// FIX Protocol Engine - File-Based Message Store (mmap append-only log)
// =============================================================================
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#include "message_store.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// FileStore
//
// Stores sequences in a small header file; raw messages in an append-only log.
// Provides crash-safe recovery: on restart, it reads back seq numbers and
// re-indexes the message log.
// ---------------------------------------------------------------------------
class FileStore : public IMessageStore {
public:
    explicit FileStore(std::filesystem::path dir, const SessionID &sid);
    ~FileStore();

    SeqNum next_sender_seq_num() const noexcept override {
        return sender_seq_.load(std::memory_order_acquire);
    }
    SeqNum next_target_seq_num() const noexcept override {
        return target_seq_.load(std::memory_order_acquire);
    }
    void set_next_sender_seq_num(SeqNum n) override;
    void set_next_target_seq_num(SeqNum n) override;
    void incr_sender_seq_num() override;
    void incr_target_seq_num() override;

    Result<void> store_outbound(SeqNum seq, const std::string &raw) override;
    Result<void> store_inbound(SeqNum seq, const std::string &raw) override;
    Result<void> get_messages(SeqNum begin, SeqNum end, MessageCallback cb) const override;
    Result<void> reset() override;
    Result<void> refresh() override;

private:
    std::filesystem::path dir_;
    std::filesystem::path seqfile_;
    std::filesystem::path outfile_;
    std::filesystem::path infile_;

    std::atomic<SeqNum> sender_seq_{1};
    std::atomic<SeqNum> target_seq_{1};
    mutable std::mutex mutex_;
    std::ofstream out_stream_;
    std::ofstream in_stream_;
    // In-memory index: seq -> byte offset in outfile
    std::map<SeqNum, std::streampos> out_index_;

    void load_seqs();
    void persist_seqs();
    void rebuild_index();
};

} // namespace fix
