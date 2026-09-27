#pragma once
// =============================================================================
// FIX Protocol Engine - Message Store Interface
// =============================================================================
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../core/message.hpp"
#include "../core/types.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// Callback type for message replay (ResendRequest recovery)
// ---------------------------------------------------------------------------
using MessageCallback = std::function<void(SeqNum, const std::string &raw)>;

// ---------------------------------------------------------------------------
// IMessageStore - interface for inbound/outbound sequence + raw message store
//
// Phase 3 (3.2/3.4):
//  * All MUTATIONS that touch persisted state return Result so a failed
//    write (ENOSPC, EIO, corrupt seq file, ...) surfaces instead of being
//    silently swallowed — a persisted sequence number that never reached
//    the disk is a desync waiting to happen after the next restart.
//  * next_X() + incr_X() pairs are NOT atomic: two concurrent senders could
//    read the same value before either increments (the checked-then-
//    increment race). Use the compound primitives below instead — they
//    perform read-and-advance as ONE indivisible step.
//  * close() flushes and releases any persistent handle; it is idempotent
//    and also invoked by the owning Session's destructor (3.4).
// ---------------------------------------------------------------------------
class IMessageStore {
public:
    virtual ~IMessageStore() = default;

    // -- Sequence numbers (reads are always lock-free snapshots) ------------
    virtual SeqNum next_sender_seq_num() const noexcept = 0;
    virtual SeqNum next_target_seq_num() const noexcept = 0;

    // -- Sequence mutations --------------------------------------------------
    // Absolute setters (rewind/advance to an exact value: ResetSeqNumFlag,
    // SequenceReset NewSeqNo, reset()). A failed persist leaves the previous
    // value in place and reports the error.
    virtual Result<void> set_next_sender_seq_num(SeqNum n) = 0;
    virtual Result<void> set_next_target_seq_num(SeqNum n) = 0;
    // Simple +1 advances. Prefer the compound primitives on hot paths that
    // also need to READ the value (they close the check-then-increment race).
    virtual Result<void> incr_sender_seq_num() = 0;
    virtual Result<void> incr_target_seq_num() = 0;

    // -- 3.2: atomic compound primitives ------------------------------------
    // Atomically claim the current NextSenderMsgSeqNum and advance the
    // counter past it in ONE indivisible step; returns the CLAIMED value
    // (the MsgSeqNum the caller must use). On error NOTHING was consumed —
    // the implementation rolls its own advance back before reporting, so a
    // failed persist can never burn a sequence number. The session still
    // serialises its own claims under Session::send_mutex_; this primitive
    // removes the store-level read/increment window for every other caller.
    virtual Result<SeqNum> next_sender_seq_num_incr() = 0;
    // Monotonically advance NextTargetMsgSeqNum to at least `n` as one
    // atomic step (never rewinds — values <= current are a no-op). Replaces
    // the racy `if (n > next_target()) set_next_target(n)` pair on the
    // inbound-consume path. Rewinds still go through set_next_target_seq_num.
    virtual Result<void> advance_next_target_seq_num(SeqNum n) = 0;

    // Store a sent message for potential resend
    virtual Result<void> store_outbound(SeqNum seq, const std::string &raw) = 0;

    // Store a received message (for audit; seq = inbound seq num)
    virtual Result<void> store_inbound(SeqNum seq, const std::string &raw) = 0;

    // Replay outbound messages in [begin, end] (inclusive; 0 = last).
    //
    // MessageCallback contract (NIT#8 — this matters, it is not decoration):
    // the callback runs SYNCHRONOUSLY, in ascending seq order, and
    // implementations are allowed to hold their internal store lock across
    // it (FileStore does — its mutex_ is held for the whole walk). So the
    // callback must be quick, must not throw, and must not re-enter the
    // same store (its mutex is non-recursive: a nested get/append would
    // deadlock). Callers therefore COLLECT inside the callback and do all
    // real work — I/O, sending, user callbacks — after it returns (see
    // Session::snapshot_store / handle_resend_request). Sequences absent
    // from the store are simply not delivered (a gap after purge/reset is
    // legal and the caller's problem to fill).
    virtual Result<void> get_messages(SeqNum begin, SeqNum end, MessageCallback cb) const = 0;

    // Reset (new session)
    virtual Result<void> reset() = 0;

    // Refresh (re-read from persistent store). A persistent implementation
    // MUST validate its state here and return an error for a corrupt store
    // instead of silently rebasing to a fresh session.
    virtual Result<void> refresh() = 0;

    // Flush/fsync/close any persistent handle (3.4). Idempotent: calling it
    // twice (or after the implementation already closed) is safe. The owning
    // Session calls this from its destructor; in-memory stores need no-op.
    virtual Result<void> close() { return {}; }
};

} // namespace fix
