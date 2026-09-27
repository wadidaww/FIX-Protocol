#pragma once
// =============================================================================
// FIX Protocol Engine - Session State Machine
// =============================================================================
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../core/constants.hpp"
#include "../core/message.hpp"
#include "../core/types.hpp"
#include "../dictionary/data_dictionary.hpp"
#include "../parser/parser.hpp"
#include "../parser/serializer.hpp"
#include "../store/message_store.hpp"

namespace fix {

class IAuditLog; // log/message_log.hpp — included by session.cpp

// ---------------------------------------------------------------------------
// Session configuration
// ---------------------------------------------------------------------------
struct SessionConfig {
    SessionID id;
    bool initiator = true;       // false = acceptor
    int heartbeat_interval = 30; // seconds
    int reconnect_delay = 5;     // seconds (for initiators)
    int logon_timeout = 10;      // seconds
    int logout_timeout = 5;      // seconds
    bool reset_on_logon = false;
    bool reset_on_disconnect = false;
    // Dictionary validation of inbound APPLICATION messages (2.7).
    // Default OFF (Phase 2 review decision F8): the builtin tables are
    // deliberately incomplete (OrdStatus W/X/Y missing, HandlInst still
    // required on NewOrderSingle), so a default-ON switch silently turned
    // valid FIX 4.4 traffic into false 35=j rejects. The implementation is
    // complete and opt-in — set true per session; the default flips back ON
    // as the Phase 4 exit gate once the tables cover the core-trading set.
    bool validate_fields = false;
    std::string username;
    std::string password;
    SeqNum reset_seq_num = 0; // 0 = no override

    // FIXT: default ApplVerID to use for FIX 5.0 SP2 sessions
    std::string default_appl_ver_id;
};

// ---------------------------------------------------------------------------
// Session FSM states
//
// Phase 2 (2.4): `LogoutReceived` is gone — a peer-initiated Logout is
// answered with an echo Logout and the session goes straight to Disconnected
// (the intermediate state only created a window for the timer thread to run
// with a half-torn-down session). `Reconnecting` is gone too: it was never
// written anywhere (grep-verified); reconnection is owned by the transport,
// which calls on_transport_connected() when the byte stream is back.
// ---------------------------------------------------------------------------
enum class SessionState : std::uint8_t {
    NotConnected,
    WaitingLogon, // acceptor waiting for logon
    LogonSent,    // initiator sent logon
    Active,       // session established
    LogoutSent,   // we sent logout
    Disconnected, // terminal
};

std::string_view to_string(SessionState s) noexcept;

// ---------------------------------------------------------------------------
// Application callbacks
// ---------------------------------------------------------------------------
struct SessionCallbacks {
    // Session lifecycle
    std::function<void(const SessionID &)> on_logon;
    std::function<void(const SessionID &, std::string_view reason)> on_logout;
    std::function<void(const SessionID &, SeqNum expected, SeqNum received)> on_sequence_gap;
    std::function<void(const SessionID &)> on_heartbeat_timeout;
    std::function<void(const SessionID &, SeqNum, SeqNum)> on_resend_request;

    // Message received (called for application-level messages)
    std::function<void(const SessionID &, const Message &)> on_message;

    // A session-level Reject (35=3) was received from the counterparty.
    // Optional: no-op when unset. (2.5)
    std::function<void(const SessionID &, SeqNum ref_seq, std::string_view text)> on_reject;

    // Error sink for failures that would otherwise be swallowed (a user
    // callback threw, ...). Optional: when unset the engine has no logging
    // infrastructure, so the error is dropped — this callback is the
    // documented floor for surfacing it. (2.6)
    //
    // Phase 3 (MUST#1): RE-ENTRANCY IS PERMITTED. The sink is never invoked
    // while Session::send_mutex_ is held — a report() made under the lock is
    // buffered and delivered by Session::drain_pending_errors() after the
    // lock scope ends (and from on_timer()), so calling Session::send()/
    // send_raw()/logon() from inside on_error is safe and cannot self-
    // deadlock (std::mutex is non-recursive). It may still be invoked
    // concurrently from several threads; keep it thread-safe, and never
    // throw out of it (throws are swallowed).
    std::function<void(const SessionID &, std::string_view)> on_error;

    // ENGINE WIRING, not a user hook (MUST#1): invoked by
    // Session::drain_pending_errors() after every send-path lock scope and
    // from on_timer(), with NO session lock held. The Engine points it at
    // its deferred transport-send-error queue, so EngineConfig::on_error
    // never runs under send_mutex_ even though the do_send wiring that
    // produces those errors does. Optional: no-op when unset.
    std::function<void()> drain_deferred_errors;

    // Transport send callback (engine calls this to write bytes to the wire)
    std::function<void(const std::string &)> do_send;
    std::function<void()> do_disconnect; // engine → ITransport::disconnect()
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
class Session {
public:
    // Milliseconds since the epoch as used by all session timestamps.
    using Millis = std::chrono::milliseconds::rep;

    explicit Session(SessionConfig cfg, std::unique_ptr<IMessageStore> store,
                     const DataDictionary *dict = nullptr, SessionCallbacks cbs = {});

    // Closes the owned message store explicitly (3.4: flush + fsync +
    // close, idempotent) — the store destructor is only the fallback.
    // Deliberately NOT done in disconnect(): a dropped connection must keep
    // the store usable for the reconnect that follows.
    ~Session();

    // Called by transport layer when bytes arrive
    void on_data(const char *data, std::size_t len);

    // Called by the engine when the underlying transport connection has been
    // established (initiator: reset any stale state and send Logon;
    // acceptor: enter WaitingLogon so an incoming Logon is accepted).
    // Safe to call on every (re)connect.
    void on_transport_connected();

    // Send an application message (seq assigned automatically). Only valid
    // while the session is Active — any other state returns SessionError so
    // a disconnected session can no longer burn phantom sequence numbers.
    Result<void> send(Message msg);

    // Send a raw string (already serialized) – bypasses seq mgmt
    Result<void> send_raw(const std::string &raw);

    // Trigger logon (for initiators)
    Result<void> logon();

    // Initiate graceful logout
    Result<void> logout(std::string_view reason = "");

    // Called periodically by the engine (e.g., from a timer thread)
    void on_timer();

    // Forcibly disconnect (no logout sent)
    void disconnect();

    // Resets session sequence numbers and state
    Result<void> reset();

    // Test seam: replace the time source used for ALL session timing
    // decisions (last_send/last_recv/logon/test-request/logout deadlines).
    // Message SendingTime keeps using the real wall clock so the wire stays
    // spec-conformant. Must be called before the session is driven (no
    // concurrent traffic) — the previous source is dropped immediately.
    void set_time_source_for_test(std::function<Millis()> clock);

    // -- Audit log wiring (3.5) ---------------------------------------------
    // Records EVERY inbound frame as received (before identity/sequence
    // validation — a rejected CompID/BeginString frame is exactly what an
    // audit trail must capture) and every outbound frame handed to the
    // transport. Pass nullptr (default) to disable auditing. The pointer is
    // stored atomically so it is TSAN-clean even if wired slightly late,
    // but the contract is: wire it BEFORE the session is driven (Engine
    // does this immediately after construction, before the transport
    // starts). The Session does NOT own the log — the Engine keeps it
    // alive for the whole engine lifetime (stop() joins all IO threads
    // before the members are destroyed).
    void set_audit_log(IAuditLog *log) noexcept { audit_.store(log, std::memory_order_release); }
    [[nodiscard]] IAuditLog *audit_log() const noexcept {
        return audit_.load(std::memory_order_acquire);
    }

    // Getters
    [[nodiscard]] const SessionID &id() const noexcept { return cfg_.id; }
    [[nodiscard]] SessionState state() const noexcept { return state_.load(); }
    [[nodiscard]] bool is_active() const noexcept { return state_.load() == SessionState::Active; }
    [[nodiscard]] const IMessageStore *store() const noexcept { return store_.get(); }

    // For metrics
    [[nodiscard]] std::uint64_t msgs_sent() const noexcept { return msgs_sent_.load(); }
    [[nodiscard]] std::uint64_t msgs_received() const noexcept { return msgs_received_.load(); }
    // Callback / processing errors surfaced through SessionCallbacks::on_error
    // (plan 2.6: "replace silent catch with on_error callback + counter").
    [[nodiscard]] std::uint64_t error_count() const noexcept { return errors_.load(); }
    // Stored outbound frames that could not be parsed during a ResendRequest
    // replay (2.2): they are never emitted verbatim, only covered by the
    // enclosing gap-fill.
    [[nodiscard]] std::uint64_t resend_corrupt_frames() const noexcept {
        return resend_corrupt_.load();
    }

private:
    SessionConfig cfg_;
    std::unique_ptr<IMessageStore> store_;
    const DataDictionary *dict_;
    SessionCallbacks cbs_;
    StreamParser parser_;
    MessageBuilder builder_;

    std::atomic<SessionState> state_{SessionState::NotConnected};
    std::atomic<std::uint64_t> msgs_sent_{0};
    std::atomic<std::uint64_t> msgs_received_{0};
    std::atomic<std::uint64_t> errors_{0};

    // Audit sink (3.5): see set_audit_log(). Never dereferenced without a
    // prior null check; atomic so wiring races are impossible under TSAN.
    std::atomic<IAuditLog *> audit_{nullptr};
    // Emits one audit entry; never throws into the session path (an audit
    // backend failure is routed to report(), which buffers it when the TX
    // side runs under send_mutex_ — audit_frame itself never calls user
    // code). It cannot break message processing.
    void audit_frame(bool outbound, std::string_view raw) noexcept;

    // -- send_mutex_ + owner tracking (Phase 3, MUST#1) ----------------------
    // Send paths lock through SendLock (never a bare lock_guard) so that
    // report() can tell whether the CALLING THREAD already holds the lock:
    // no user callback may ever run under send_mutex_ (3.1 exit criterion),
    // so a report() made while the lock is held is buffered into
    // pending_errors_ and delivered by drain_pending_errors() after the lock
    // scope ends. Ownership is per-session: a thread holding session A's lock
    // still gets a synchronous on_error for session B.
    mutable std::mutex send_mutex_;
    std::atomic<std::thread::id> send_owner_{std::thread::id{}};

    struct SendLock {
        explicit SendLock(Session &s) noexcept
            : s_(s) {
            s_.send_mutex_.lock();
            s_.send_owner_.store(std::this_thread::get_id(), std::memory_order_release);
        }
        ~SendLock() noexcept {
            s_.send_owner_.store(std::thread::id{}, std::memory_order_release);
            s_.send_mutex_.unlock();
        }
        SendLock(const SendLock &) = delete;
        SendLock &operator=(const SendLock &) = delete;

    private:
        Session &s_;
    };

    // -- Deferred error buffer (Phase 3, MUST#1) ----------------------------
    // report() called WHILE send_mutex_ is held enqueues here instead of
    // invoking SessionCallbacks::on_error (which would run user code under
    // the lock — and re-enter send() → self-deadlock). Guarded by its own
    // tiny leaf mutex, never held across a callback: the lock order is
    // send → pending_err_mtx_, and drain_pending_errors() swaps the batch
    // out before invoking anything (see session_manager.hpp).
    mutable std::mutex pending_err_mtx_;
    std::vector<std::string> pending_errors_;
    // Bounded: a drain that never runs (unit tests that never tick the
    // session) must not grow the buffer without limit. The error COUNT is
    // never affected — errors_ is incremented synchronously in report().
    static constexpr std::size_t kMaxPendingErrors = 64;
    // Re-entrancy gate: on_error → Session::send() → send_message → drain
    // again on the same thread. The outer drain keeps looping until the
    // buffer is empty, so a nested call returns immediately.
    std::atomic<bool> draining_errors_{false};
    // Delivers buffered errors. Called AFTER every send-path lock scope
    // (send_message, send_raw, send_logon, handle_resend_request incl. the
    // resend replay loop) and from on_timer() — never while send_mutex_ is
    // held. Also drains the engine's deferred transport-error queue through
    // SessionCallbacks::drain_deferred_errors.
    void drain_pending_errors() noexcept;

    // -- Shared timing state (F2 / 2.3) -------------------------------------
    // Epoch milliseconds, stored atomically: written on IO/user threads
    // (process_message/send_message/send_logon/send_logout) and read by the
    // engine's timer thread (on_timer). Plain TimePoint members raced under
    // TSAN.
    static Millis now_ms() noexcept;
    // Virtual clock: null → now_ms(). Held via shared_ptr so the test seam
    // can swap it without racing concurrent readers (atomic<shared_ptr>).
    using ClockFn = std::function<Millis()>;
    std::atomic<std::shared_ptr<const ClockFn>> clock_;
    std::atomic<Millis> last_send_time_ms_{0};
    std::atomic<Millis> last_recv_time_ms_{0};
    std::atomic<Millis> logon_sent_time_ms_{0};
    // When the TestRequest we are awaiting was sent (2.3): the 1.2×HeartBtInt
    // deadline is measured from THIS, not from last_recv.
    std::atomic<Millis> test_req_sent_time_ms_{0};
    // When our Logout went out (2.3): logout_timeout while LogoutSent.
    std::atomic<Millis> logout_sent_time_ms_{0};

    // -- TestRequest round-trip bookkeeping (F2 / 2.3) ----------------------
    // The flag is written by the timer thread (send_test_request) and the IO
    // thread (any inbound message clears it — relaxed liveness rule — plus
    // disconnect/reset/on_transport_connected); the ID string is guarded by
    // test_req_mtx_ — the mutex is only ever held for the string itself,
    // never across sends or user callbacks.
    mutable std::mutex test_req_mtx_;
    std::string pending_test_req_id_;
    std::atomic<bool> test_req_pending_{false};

    // -- Sequence-gap throttle (2.1 / 2.2 / F9) ------------------------------
    // Set when a gap triggers a ResendRequest, cleared when a message finally
    // arrives at the expected sequence (or on disconnect/reset/logon). Stops
    // one ResendRequest per out-of-sequence message from flooding the peer.
    // Since F9 it also drives periodic RETRIES from on_timer(): a single-shot
    // request that went unanswered used to leave a zombie session (Active
    // forever, exactly one ResendRequest ever sent).
    std::atomic<bool> gap_open_{false};
    // ResendRequest retry budget for the CURRENT gap (F9): when the last RR
    // went out (epoch ms, session clock) and how many RRs the gap has already
    // produced (initial request = 1). Written by the IO thread (request_gap)
    // and the timer thread (on_timer), so both are atomic.
    // kMaxGapRetries caps the TOTAL ResendRequests per gap: on the attempt
    // beyond it the session reports the stall on on_error and disconnects
    // instead of waiting forever for a peer that is never going to answer.
    static constexpr int kMaxGapRetries = 5;
    std::atomic<Millis> gap_rr_last_ms_{0};
    std::atomic<int> gap_rr_attempts_{0};
    // Close the current gap: resets the retry budget FIRST, then releases the
    // throttle. Every site that clears gap_open_ must call this so the next
    // gap starts a fresh budget instead of inheriting a spent one (F9).
    void clear_gap() noexcept {
        gap_rr_attempts_.store(0, std::memory_order_relaxed);
        gap_rr_last_ms_.store(0, std::memory_order_relaxed);
        gap_open_.store(false, std::memory_order_release);
    }

    // Corrupt stored frames skipped during a ResendRequest replay (2.2).
    std::atomic<std::uint64_t> resend_corrupt_{0};

    // on_logout already delivered for the current connection cycle — see
    // notify_logout(). Re-armed in on_transport_connected/handle_logon/reset.
    std::atomic<bool> logout_notified_{false};

    // -- Heartbeat interval mirror (F2) -------------------------------------
    // cfg_.heartbeat_interval is written by the IO thread (handle_logon) and
    // read by the timer thread (on_timer) and by send_logon (any thread).
    // SessionConfig must stay copyable, so the shared view lives here as an
    // atomic mirror; construction and handle_logon keep both in sync.
    std::atomic<int> heartbeat_sec_{30};

    // -- Parser guard (F10/F14) ---------------------------------------------
    // StreamParser is single-thread-affine (feed/next/reset must not overlap).
    // on_data holds this only across feed()/next() — never across
    // process_message() — so disconnect() (which may run on the timer or IO
    // thread and resets the parser) can never deadlock against it.
    mutable std::mutex parser_mtx_;

    // -- Message dispatch ----------------------------------------------------
    void process_message(const Message &msg);
    void process_admin(const Message &msg);
    void process_app(const Message &msg);

    // -- Admin message handlers ---------------------------------------------
    void handle_logon(const Message &msg);
    void handle_logout(const Message &msg);
    void handle_heartbeat(const Message &msg);
    void handle_test_request(const Message &msg);
    void handle_resend_request(const Message &msg);
    // Returns true when the SequenceReset was consumed (applied or safely
    // ignored): process_message then returns WITHOUT advancing/storing the
    // sequence number. Returns false when it must be dropped as unrecoverable
    // (a gap-fill referencing a future sequence — a ResendRequest was issued).
    bool handle_sequence_reset(const Message &msg);
    void handle_reject(const Message &msg);

    // -- Internal send helpers -----------------------------------------------
    // Sends our Logon. Publishes the handshake state (LogonSent for an
    // initiator, WaitingLogon for an acceptor) BEFORE the frame reaches the
    // wire and rolls it back if the send fails, so a loopback-fast reply can
    // never be mistaken for a stale Logon (F6); with reset_on_logon it also
    // rewinds the store BEFORE sending, so the frame really carries MsgSeqNum
    // 1 instead of a stale N+1 (F1). Returns the send/store failure to the
    // caller — logon() and handle_logon both check it.
    Result<void> send_logon();
    void send_logout(std::string_view reason);
    // Echo Logout for a peer-initiated logout: sends the message WITHOUT
    // entering LogoutSent, so the caller can go straight to Disconnected.
    void send_logout_echo();
    void send_heartbeat(std::string_view test_req_id = "");
    void send_test_request();
    void send_resend_request(SeqNum begin, SeqNum end);
    // RefSeqNum (45) is MANDATORY on both 35=3 and 35=j — always emitted;
    // ref_seq 0 means "the offending MsgSeqNum was itself missing/unknown"
    // and is written as 0 rather than left out (F12).
    void send_reject(SeqNum ref_seq, SessionRejectReason reason, std::string_view ref_msg_type = "",
                     TagNum ref_tag = 0, std::string_view text = "");
    // BusinessMessageReject (35=j) for dictionary-validation failures (2.7).
    // RefMsgType mirrors the rejected message — tag 372 is itself a required
    // field of 35=j in the builtin dictionary.
    void send_business_reject(SeqNum ref_seq, int business_reject_reason,
                              std::string_view ref_msg_type, std::string_view text);

    // internal: sets header, stores, sends. Locks send_mutex_ itself.
    Result<void> send_message(Message &msg);
    // Same as send_message but the caller ALREADY holds send_mutex_ (used by
    // the ResendRequest replay so concurrent sends can't interleave).
    Result<void> send_message_locked(Message &msg);
    // Emit an already-serialized frame (resend replay): no seq consumption,
    // no store write. Caller must hold send_mutex_.
    void emit_wire_locked(std::string_view wire);

    // -- Sequence validation -------------------------------------------------
    bool validate_seq_num(const Message &msg);
    // Gap throttle: sends a ResendRequest at most once per open gap (the
    // retry cadence for an unanswered request lives in on_timer — F9).
    void request_gap(SeqNum begin);
    // The place a processed inbound message consumes its sequence slot,
    // shared by the dispatch advance and the duplicate-header Reject path:
    // a Reject must consume exactly what an equivalent accepted message
    // would, otherwise an in-sequence PossDup with a duplicated header is
    // rejected but never consumed and the reject/resend loop never ends
    // (F3). The pre-validation missing-MsgType Reject deliberately does NOT
    // use this: it keeps its own stricter `seq == expected` gate so a
    // future-sequence message can never fast-forward the counter.
    void consume_inbound(const Message &msg, bool seq_validated);

    // -- State machine (2.4) -------------------------------------------------
    // compare_exchange transition: returns true only if the session was in
    // `expected` and moved to `desired`. Use it wherever a user thread, the
    // IO thread and the timer thread can race. send_logon() also uses it
    // (under send_mutex_) so a concurrent duplicate initiator logon loses
    // the CAS instead of emitting a second Logon with a second store reset.
    bool transition(SessionState expected, SessionState desired);

    // -- ResendRequest replay (2.2) -----------------------------------------
    // Snapshot of store_->get_messages(begin, end): collecting under the
    // store lock and replaying after releasing it keeps the store lock out of
    // any send/callback path. nullopt = the store read failed; the caller
    // must then emit NOTHING rather than gap-fill over messages it could not
    // read (masking real traffic would be worse than a stalled peer).
    struct StoredFrame {
        SeqNum seq;
        std::string wire;
    };
    [[nodiscard]] std::optional<std::vector<StoredFrame>> snapshot_store(SeqNum begin, SeqNum end);

    // -- Exception boundary (2.6) -------------------------------------------
    // Runs a user-callback invocation; any exception (std or not) is funnelled
    // to report() so it can never escape into the IO/timer thread.
    template <typename Fn>
    void guarded(Fn &&fn) {
        try {
            fn();
        } catch (const std::exception &e) {
            report(e.what());
        } catch (...) {
            report("unknown exception from session callback");
        }
    }
    // Routes to SessionCallbacks::on_error; swallows when unset (no logging
    // infra exists — that is the documented floor, see SessionCallbacks).
    // The error COUNTER is always bumped here, immediately. Delivery is
    // synchronous UNLESS the calling thread already holds send_mutex_ —
    // then the message is buffered for drain_pending_errors() so user code
    // never runs under the send lock (MUST#1, see SessionCallbacks).
    void report(std::string_view what) noexcept;
    // Single guarded invocation of SessionCallbacks::on_error (never throws).
    void invoke_on_error(std::string_view what) noexcept;

    // -- Session-end notification -------------------------------------------
    // Single gate for on_logout: fires the callback at most once per
    // connection cycle (mutual logout, peer-initiated logout, logout
    // timeout, rejected handshake). The flag re-arms on transport connect
    // and on a successful logon, so a later reconnect/logout cycle notifies
    // again while any late duplicate (e.g. peer Logout arriving after the
    // timeout already closed us) stays silent.
    void notify_logout(std::string_view reason);

    // -- Dictionary validation (2.7) ----------------------------------------
    // Resolves the DataDictionary for this session's version: the explicitly
    // provided dict first, else the process-wide registry the engine
    // populates via Engine::load_*_dictionary(). May return nullptr (no
    // validation then).
    [[nodiscard]] const DataDictionary *resolve_dictionary() const;

    // -- Helpers -------------------------------------------------------------
    bool is_admin_msg(std::string_view msg_type) const noexcept;
    // Current time through the (possibly virtual) clock.
    [[nodiscard]] Millis clock_now() const;
};

} // namespace fix
