#pragma once
// =============================================================================
// FIX Protocol Engine - Main Engine API
// =============================================================================
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/message.hpp"
#include "core/types.hpp"
#include "dictionary/data_dictionary.hpp"
#include "log/message_log.hpp"
#include "session/session.hpp"
#include "session/session_manager.hpp"
#include "store/file_store.hpp"
#include "store/memory_store.hpp"
#include "transport/transport.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// Deferred engine error sink (Phase 3, MUST#1b)
//
// EngineConfig::on_error is USER code and must never run while
// Session::send_mutex_ is held — yet the Engine's do_send wiring runs
// exactly there. A transport send failure is therefore PUSHed into this
// queue and drained from places that hold no session lock:
//   * Session::drain_pending_errors() after every send-path lock scope (via
//     SessionCallbacks::drain_deferred_errors, wired in Engine::add_session)
//     — synchronous from the sender's point of view, and
//   * the engine's timer loop, next to the existing deferred backstop report
//     (the ~200 ms backstop for everything else).
// Lock order: send → engine-error-queue (leaf; see the statement at the top
// of session_manager.hpp). Shared by pointer because the queue itself must
// outlive the Engine: a session parked in stop()'s never-destroyed
// quarantine can still drain after the Engine is gone.
// ---------------------------------------------------------------------------
class DeferredErrorSink {
public:
    explicit DeferredErrorSink(std::function<void(std::error_code)> sink)
        : sink_(std::move(sink)) {}

    // Producer side — MAY be called with Session::send_mutex_ held (that is
    // the whole point): it only takes the queue's own leaf mutex.
    void push(std::error_code ec) {
        std::lock_guard lock(mtx_);
        if (queue_.size() >= kMaxQueued) {
            ++dropped_; // bounded like Session::pending_errors_ — count, don't grow
            return;
        }
        queue_.push_back(ec);
    }

    // Consumer side — NEVER call with a session lock held.
    void drain() noexcept {
        std::vector<std::error_code> batch;
        {
            std::lock_guard lock(mtx_);
            batch.swap(queue_);
        }
        if (!sink_)
            return;
        for (std::error_code ec : batch) {
            try {
                sink_(ec);
            } catch (...) {
                // The sink is user code; a throw must not take the timer/
                // session thread down (same contract as report_error()).
            }
        }
    }

    [[nodiscard]] std::size_t dropped() const noexcept {
        std::lock_guard lock(mtx_);
        return dropped_;
    }

private:
    static constexpr std::size_t kMaxQueued = 256;
    const std::function<void(std::error_code)> sink_;
    mutable std::mutex mtx_;
    std::vector<std::error_code> queue_;
    std::size_t dropped_ = 0;
};

// ---------------------------------------------------------------------------
// EngineConfig
// ---------------------------------------------------------------------------
struct EngineConfig {
    std::filesystem::path store_dir = "./fix_store";
    std::filesystem::path log_dir = "./fix_logs";
    bool use_file_store = false;
    bool enable_audit = true;
    // fsync(2) after every FileStore message append (3.4): crash-safe
    // frames at one fsync per stored frame; sequence numbers are always
    // fsynced regardless, so only recent un-fsynced frames can vanish.
    bool fsync_messages = false;
    int timer_interval_ms = 200; // heartbeat/timer resolution
    std::size_t thread_pool_size = 4;

    // Engine-level transport error sink. Every transport error (bind/connect/
    // recv/send failures, poll errors) that used to be discarded fires this,
    // as do transport send failures surfaced from Session's do_send wiring and
    // the exception-backstop reports for exceptions escaping user/session
    // callbacks on the engine's timer, transport-IO and shutdown threads
    // (fix::ErrorCode::SessionError). Called on the transport's IO thread (or
    // the caller's, for start-up errors / synchronous sends) – keep it fast
    // and thread-safe, do not re-enter the Engine/session it came from, and
    // never throw out of it (throws are swallowed by the backstop).
    //
    // Phase 3 (MUST#1b): failures produced BY the do_send wiring run under
    // Session::send_mutex_, so they are queued (DeferredErrorSink) and
    // delivered when the lock is gone — typically still before Session::send
    // returns (Session::drain_pending_errors), at the latest on the next
    // timer tick. Everything else still fires inline.
    std::function<void(std::error_code)> on_error;
};

// ---------------------------------------------------------------------------
// Engine – top-level coordinator
//
// Lock order (Phase 3, task 3.1): the authoritative statement lives at the
// top of include/fix/session/session_manager.hpp —
//     manager → recv → send → store → transport
// — plus the two leaf error buffers taken from under `send`
// (Session::pending_err_mtx_ and this class's DeferredErrorSink queue, both
// documented there). "manager" covers SessionManager::mutex_ and this
// class's conn_mutex_. Both are top-level registry locks: never nested with
// each other, never held across a user callback, a join, or an error-sink
// report (every sweep in src/engine.cpp snapshots under the lock, releases
// it, then iterates/stops/report).
// ---------------------------------------------------------------------------
class Engine {
public:
    explicit Engine(EngineConfig cfg = {});
    ~Engine();

    // Initialise and start background threads
    Result<void> start();

    // Stop everything: logout all sessions, join the timer thread and every
    // transport IO thread, then release all sessions.
    //
    // Calling contract: stop() must NOT be called from a session callback
    // (i.e. from a transport IO thread or the timer thread) on the normal
    // path – it joins those threads. As a backstop against hard termination
    // (~thread aborts) a transport owned by the calling thread itself is
    // parked in a never-destroyed quarantine instead of being joined here;
    // everything else is still stopped and joined synchronously.
    void stop();

    // Create a new session with a given transport. The transport is started
    // immediately if the engine is already running. Returns nullptr when
    // `transport` is null or a session with the same SessionID exists.
    Session *add_session(SessionConfig cfg, std::unique_ptr<ITransport> transport,
                         SessionCallbacks cbs = {}, const DataDictionary *dict = nullptr);

    // Remove a session
    bool remove_session(const SessionID &sid);

    // Get session by ID
    [[nodiscard]] Session *get_session(const SessionID &sid) noexcept;

    // Data dictionaries
    void load_dictionary(FixVersion v, const std::filesystem::path &xml_path);
    void load_builtin_dictionary(FixVersion v);
    [[nodiscard]] const DataDictionary *dictionary(FixVersion v) const noexcept;

    // Audit log
    [[nodiscard]] IAuditLog *audit_log() noexcept { return audit_log_.get(); }
    // Swap the audit sink. Sessions captured the PREVIOUS log by raw pointer
    // in add_session(), so this re-wires every live session to the new sink —
    // swapping the member alone would leave them auditing into a destroyed
    // object (UAF). The rewire-then-swap runs under audit_mtx_, the same
    // lock add_session() holds across its register+capture pair, so the two
    // cannot interleave; for_each() iterates a lock-free snapshot, so no
    // SessionManager lock is held here. Equivalent (and equally correct) to
    // calling this BEFORE add_session, which wires new sessions to whatever
    // is installed.
    void set_audit_log(std::unique_ptr<IAuditLog> log);

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    EngineConfig cfg_;
    SessionManager sessions_;
    DictionaryRegistry dicts_;
    std::unique_ptr<IAuditLog> audit_log_;
    // Guards audit_log_ for the two places that swap/capture it (add_session's
    // register+capture pair and set_audit_log's rewire-then-swap) so a log
    // cannot be destroyed while a session is being wired to it. Registry-level
    // lock: never taken under conn_mutex_/SessionManager, never held across a
    // user callback.
    std::mutex audit_mtx_;
    // MUST#1b: transport-send failures queued by the do_send wiring (which
    // runs under Session::send_mutex_) and drained where no lock is held —
    // see DeferredErrorSink above. Shared so sessions that outlive this
    // Engine (stop()'s quarantine) can still drain safely.
    std::shared_ptr<DeferredErrorSink> deferred_errors_;

    std::atomic<bool> running_{false};
    std::thread timer_thread_;

    // Transport → Session wiring per connection. Both are shared: the session
    // outlives its raw handle while callbacks run, and the transport must stay
    // alive until stop() has joined its IO thread.
    struct Connection {
        std::shared_ptr<ITransport> transport;
        std::shared_ptr<Session> session;
    };
    std::vector<Connection> connections_;
    mutable std::mutex conn_mutex_;

    // Sessions whose transport IO thread is the thread that removed them: the
    // IO thread cannot join itself, so remove_session() parks them here and
    // the timer thread reaps them (stop → join → destroy, off the IO thread).
    // Guarded by conn_mutex_; swapped out wholesale by drain_reap_list().
    std::vector<Connection> reap_list_;

    void drain_reap_list();
};

} // namespace fix
