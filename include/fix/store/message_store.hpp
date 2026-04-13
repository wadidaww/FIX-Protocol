#pragma once
// =============================================================================
// FIX Protocol Engine - Message Store Interface
// =============================================================================
#include "../core/types.hpp"
#include "../core/message.hpp"
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <filesystem>

namespace fix {

// ---------------------------------------------------------------------------
// Callback type for message replay (ResendRequest recovery)
// ---------------------------------------------------------------------------
using MessageCallback = std::function<void(SeqNum, const std::string& raw)>;

// ---------------------------------------------------------------------------
// IMessageStore - interface for inbound/outbound sequence + raw message store
// ---------------------------------------------------------------------------
class IMessageStore {
public:
    virtual ~IMessageStore() = default;

    // Sequence numbers
    virtual SeqNum next_sender_seq_num() const noexcept = 0;
    virtual SeqNum next_target_seq_num() const noexcept = 0;
    virtual void   set_next_sender_seq_num(SeqNum n) = 0;
    virtual void   set_next_target_seq_num(SeqNum n) = 0;
    virtual void   incr_sender_seq_num() = 0;
    virtual void   incr_target_seq_num() = 0;

    // Store a sent message for potential resend
    virtual Result<void> store_outbound(SeqNum seq, const std::string& raw) = 0;

    // Store a received message (for audit; seq = inbound seq num)
    virtual Result<void> store_inbound(SeqNum seq, const std::string& raw)  = 0;

    // Replay outbound messages in [begin, end] (inclusive; 0 = last)
    virtual Result<void> get_messages(SeqNum begin, SeqNum end,
                                      MessageCallback cb) const = 0;

    // Reset (new session)
    virtual Result<void> reset() = 0;

    // Refresh (re-read from persistent store)
    virtual Result<void> refresh() = 0;
};

} // namespace fix
