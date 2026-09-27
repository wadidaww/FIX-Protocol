#pragma once
// =============================================================================
// FIX Protocol Engine - Session Manager
// =============================================================================
//
// ---------------------------------------------------------------------------
// LOCK ORDER (Phase 3, task 3.1) — the authoritative statement
// ---------------------------------------------------------------------------
// Total order across the engine. Acquire in this order only; NEVER acquire a
// lock that appears EARLIER in the chain while holding one that appears LATER:
//
//     manager  →  recv  →  send  →  store  →  transport
//
//   manager   SessionManager::mutex_ (this class), Engine::conn_mutex_ AND
//             Engine::audit_mtx_ (transport/session registry + audit-sink
//             swap). The registry locks are peers: they are never nested
//             with each other either (Engine snapshots under conn_mutex_ and
//             calls into the manager only after releasing it, and vice
//             versa); audit_mtx_ sits above the manager lock (it is taken
//             across create_session's register+capture pair and across
//             set_audit_log's for_each snapshot).
//   recv      Session::parser_mtx_ / Session::test_req_mtx_ — leaf locks: no
//             other mutex is ever acquired while they are held, and the
//             message dispatch (process_message → user callbacks) runs after
//             they have been released.
//   send      Session::send_mutex_ (sequence assignment → serialise → store →
//             do_send).
//   store     IMessageStore internal mutex (MemoryStore/FileStore).
//   transport TcpTransport::send_mutex_ (send queue + conn_fd_) — leaf.
//
//   Two LEAF error buffers may be taken from UNDER `send` and nothing later:
//     errbuf   Session::pending_err_mtx_ and the Engine's DeferredErrorSink
//              queue (see DeferredErrorSink in engine.hpp). Push/swap only —
//              NO callback ever runs while either is held. Errors collected
//              there are drained only after every session lock is released:
//              Session::drain_pending_errors() (owning thread, after each
//              send-path lock scope) and the engine's timer loop / stop().
//
// Rules (audited in 3.1; violations are bugs, not style):
//   * NEVER hold manager, store or send across a USER callback
//     (SessionCallbacks::on_*, EngineConfig hooks). Snapshot before you
//     iterate: for_each() copies the shared_ptr list under the lock, releases
//     the lock, then invokes the callback; Engine::start/stop and the timer
//     pass do the same for their connection/session sweeps, and error-sink
//     reports are deferred until after the pass.
//     MUST#1 closed the last hole in this rule: a report() reached while
//     send_mutex_ is held (same thread — send_owner_ records the holder) no
//     longer invokes on_error; it buffers into pending_err_mtx_ and the
//     error is delivered by drain_pending_errors() once the lock scope ends,
//     and the Engine's do_send wiring pushes transport failures into the
//     engine error queue for exactly the same deferred drain. User error
//     sinks therefore cannot re-enter a locked Session (lock-order
//     violation → deadlock) and cannot touch transport state mid-send.
//   * ACCEPTED BOUNDARY: Session::send_message holds send_mutex_ across
//     cbs_.do_send → ITransport::send. That call is the transport ENQUEUE
//     (bounded, non-blocking, syscall under the transport leaf lock, no user
//     code) — not a user callback — and it is how send → transport ordering is
//     established. Store locks are never held across it: store_outbound() /
//     incr_sender_seq_num() take and release their mutex internally, and the
//     resend path snapshots the store BEFORE taking send_mutex_.
//   * A leaf lock may be held across a syscall into the OS (send/epoll), but
//     never across application code or a blocking join.
//
// See include/fix/engine.hpp for the Engine-side cross-reference.
// ---------------------------------------------------------------------------
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "../dictionary/data_dictionary.hpp"
#include "../store/memory_store.hpp"
#include "session.hpp"

namespace fix {

class SessionManager {
public:
    SessionManager() = default;

    // Create and register a session. Returns nullptr (and registers nothing)
    // when a session with the same ID already exists – sessions are never
    // silently replaced, so transports holding a reference stay valid.
    [[nodiscard]] std::shared_ptr<Session>
    create_session(SessionConfig cfg, std::unique_ptr<IMessageStore> store = nullptr,
                   const DataDictionary *dict = nullptr, SessionCallbacks cbs = {});

    // Look up by session ID
    [[nodiscard]] Session *find(const SessionID &sid) noexcept;
    [[nodiscard]] const Session *find(const SessionID &sid) const noexcept;

    // Look up by sender/target
    [[nodiscard]] Session *find(std::string_view sender, std::string_view target) noexcept;

    // Shared ownership lookup (keeps the session alive while in use)
    [[nodiscard]] std::shared_ptr<Session> find_shared(const SessionID &sid);

    // Remove a session
    bool remove(const SessionID &sid);

    // Iterate over a SNAPSHOT of the session list: the registry lock is taken
    // only long enough to copy the shared_ptrs, never across `fn` (see the
    // lock-order statement above). `fn` may safely call back into this class
    // (create_session/remove); sessions removed mid-pass stay alive for the
    // duration of the snapshot.
    void for_each(std::function<void(Session &)> fn);

    [[nodiscard]] std::size_t count() const noexcept;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;

    static std::string make_key(const SessionID &sid);
};

} // namespace fix
