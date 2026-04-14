#pragma once
// =============================================================================
// FIX Protocol Engine - In-Memory Message Store
// =============================================================================
#include <atomic>
#include <map>
#include <mutex>

#include "message_store.hpp"

namespace fix {

class MemoryStore : public IMessageStore {
public:
    MemoryStore() = default;

    SeqNum next_sender_seq_num() const noexcept override {
        return sender_seq_.load(std::memory_order_acquire);
    }
    SeqNum next_target_seq_num() const noexcept override {
        return target_seq_.load(std::memory_order_acquire);
    }
    void set_next_sender_seq_num(SeqNum n) override {
        sender_seq_.store(n, std::memory_order_release);
    }
    void set_next_target_seq_num(SeqNum n) override {
        target_seq_.store(n, std::memory_order_release);
    }
    void incr_sender_seq_num() override { sender_seq_.fetch_add(1, std::memory_order_acq_rel); }
    void incr_target_seq_num() override { target_seq_.fetch_add(1, std::memory_order_acq_rel); }

    Result<void> store_outbound(SeqNum seq, const std::string &raw) override {
        std::lock_guard lock(mutex_);
        outbound_[seq] = raw;
        return {};
    }
    Result<void> store_inbound(SeqNum seq, const std::string &raw) override {
        std::lock_guard lock(mutex_);
        inbound_[seq] = raw;
        return {};
    }

    Result<void> get_messages(SeqNum begin, SeqNum end, MessageCallback cb) const override {
        std::lock_guard lock(mutex_);
        auto it = outbound_.lower_bound(begin);
        SeqNum last = (end == 0) ? (outbound_.empty() ? 0 : outbound_.rbegin()->first) : end;
        for (; it != outbound_.end() && it->first <= last; ++it) {
            cb(it->first, it->second);
        }
        return {};
    }

    Result<void> reset() override {
        std::lock_guard lock(mutex_);
        sender_seq_.store(1, std::memory_order_release);
        target_seq_.store(1, std::memory_order_release);
        outbound_.clear();
        inbound_.clear();
        return {};
    }

    Result<void> refresh() override { return {}; }

private:
    std::atomic<SeqNum> sender_seq_{1};
    std::atomic<SeqNum> target_seq_{1};
    mutable std::mutex mutex_;
    std::map<SeqNum, std::string> outbound_;
    std::map<SeqNum, std::string> inbound_;
};

} // namespace fix
