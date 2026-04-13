#pragma once
// =============================================================================
// FIX Protocol Engine - Main Engine API
// =============================================================================
#include "core/types.hpp"
#include "core/message.hpp"
#include "session/session.hpp"
#include "session/session_manager.hpp"
#include "transport/transport.hpp"
#include "dictionary/data_dictionary.hpp"
#include "log/message_log.hpp"
#include "store/memory_store.hpp"
#include "store/file_store.hpp"

#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <filesystem>

namespace fix {

// ---------------------------------------------------------------------------
// EngineConfig
// ---------------------------------------------------------------------------
struct EngineConfig {
    std::filesystem::path store_dir    = "./fix_store";
    std::filesystem::path log_dir      = "./fix_logs";
    bool                  use_file_store = false;
    bool                  enable_audit   = true;
    int                   timer_interval_ms = 200; // heartbeat/timer resolution
    std::size_t           thread_pool_size  = 4;
};

// ---------------------------------------------------------------------------
// Engine – top-level coordinator
// ---------------------------------------------------------------------------
class Engine {
public:
    explicit Engine(EngineConfig cfg = {});
    ~Engine();

    // Initialise and start background threads
    Result<void> start();
    void         stop();

    // Create a new session with a given transport
    Session* add_session(SessionConfig cfg,
                          std::unique_ptr<ITransport> transport,
                          SessionCallbacks cbs      = {},
                          const DataDictionary* dict = nullptr);

    // Remove a session
    bool remove_session(const SessionID& sid);

    // Get session by ID
    [[nodiscard]] Session* get_session(const SessionID& sid) noexcept;

    // Data dictionaries
    void load_dictionary(FixVersion v, const std::filesystem::path& xml_path);
    void load_builtin_dictionary(FixVersion v);
    [[nodiscard]] const DataDictionary* dictionary(FixVersion v) const noexcept;

    // Audit log
    [[nodiscard]] IAuditLog* audit_log() noexcept { return audit_log_.get(); }
    void set_audit_log(std::unique_ptr<IAuditLog> log) { audit_log_ = std::move(log); }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    EngineConfig            cfg_;
    SessionManager          sessions_;
    DictionaryRegistry      dicts_;
    std::unique_ptr<IAuditLog> audit_log_;

    std::atomic<bool>       running_{false};
    std::thread             timer_thread_;

    // Transport → Session wiring per connection
    struct Connection {
        std::unique_ptr<ITransport> transport;
        Session*                    session  = nullptr;
    };
    std::vector<Connection>  connections_;
    mutable std::mutex       conn_mutex_;

    void timer_loop();
};

} // namespace fix
