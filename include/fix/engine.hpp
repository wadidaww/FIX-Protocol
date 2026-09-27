#pragma once
// =============================================================================
// FIX Protocol Engine - Main Engine API
// =============================================================================
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
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
    std::function<void(std::error_code)> on_error;
};

// ---------------------------------------------------------------------------
// Engine – top-level coordinator
//
// Lock order (Phase 3, task 3.1): the authoritative statement lives at the
// top of include/fix/session/session_manager.hpp —
//     manager → recv → send → store → transport
// — where "manager" covers both SessionManager::mutex_ and this class's
// conn_mutex_. Both are top-level registry locks: never nested with each
// other, never held across a user callback, a join, or an error-sink report
// (every sweep in src/engine.cpp snapshots under the lock, releases it, then
// iterates/stops/report).
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
    void set_audit_log(std::unique_ptr<IAuditLog> log) { audit_log_ = std::move(log); }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    EngineConfig cfg_;
    SessionManager sessions_;
    DictionaryRegistry dicts_;
    std::unique_ptr<IAuditLog> audit_log_;

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
