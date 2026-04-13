// =============================================================================
// FIX Protocol Engine - Engine implementation
// =============================================================================
#include "fix/engine.hpp"
#include <stdexcept>
#include <algorithm>

namespace fix {

Engine::Engine(EngineConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.enable_audit) {
        FileAuditLog::Config ac;
        ac.dir       = cfg_.log_dir;
        ac.base_name = "fix_audit";
        audit_log_   = std::make_unique<FileAuditLog>(std::move(ac));
    } else {
        audit_log_ = std::make_unique<NullAuditLog>();
    }
}

Engine::~Engine() {
    stop();
}

Result<void> Engine::start() {
    if (running_.load()) return make_unexpected(ErrorCode::SessionError);
    running_.store(true, std::memory_order_release);

    timer_thread_ = std::thread([this]{
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg_.timer_interval_ms));
            sessions_.tick_all();
        }
    });

    // Start all registered transports
    {
        std::lock_guard lock(conn_mutex_);
        for (auto& conn : connections_) {
            conn.transport->start();
        }
    }

    return {};
}

void Engine::stop() {
    if (!running_.exchange(false)) return;
    if (timer_thread_.joinable()) timer_thread_.join();

    // Logout all sessions
    sessions_.for_each([](Session& s){
        if (s.is_active()) {
            s.logout("Engine shutdown");
        }
    });

    // Stop all transports
    std::lock_guard lock(conn_mutex_);
    for (auto& conn : connections_) {
        conn.transport->stop();
    }
    connections_.clear();
}

Session* Engine::add_session(SessionConfig cfg,
                               std::unique_ptr<ITransport> transport,
                               SessionCallbacks user_cbs,
                               const DataDictionary* dict) {
    // Wire up transport → session
    SessionCallbacks internal_cbs = user_cbs;

    // Create store
    std::unique_ptr<IMessageStore> store;
    if (cfg_.use_file_store) {
        store = std::make_unique<FileStore>(cfg_.store_dir, cfg.id);
    } else {
        store = std::make_unique<MemoryStore>();
    }

    Session* sess = sessions_.create_session(std::move(cfg), std::move(store),
                                               dict, internal_cbs);

    if (transport) {
        transport->set_on_connected([sess]{
            if (sess->id().version >= FixVersion::FIX_4_2) {
                // Initiator automatically sends Logon on connect
                // (acceptors wait)
                // The SessionConfig.initiator flag controls this
            }
        });
        transport->set_on_data([sess](const char* data, std::size_t len){
            sess->on_data(data, len);
        });
        transport->set_on_disconnected([sess](std::string_view reason){
            sess->disconnect();
        });

        // Wire session send → transport
        // We need to update the session's do_send callback
        // Since Session callbacks are set at construction, we do it via a shared
        // transport pointer approach. This is a design limitation we handle by
        // setting the do_send on the session after creation.
        // For now, we store the connection and let the session use it directly.

        std::lock_guard lock(conn_mutex_);
        connections_.push_back({std::move(transport), sess});
    }

    return sess;
}

bool Engine::remove_session(const SessionID& sid) {
    return sessions_.remove(sid);
}

Session* Engine::get_session(const SessionID& sid) noexcept {
    return sessions_.find(sid);
}

void Engine::load_dictionary(FixVersion v, const std::filesystem::path& xml_path) {
    auto dict = std::make_shared<DataDictionary>();
    dict->load_builtin(v);
    dict->load(xml_path); // overlay with XML if available
    DictionaryRegistry::instance().set(v, std::move(dict));
}

void Engine::load_builtin_dictionary(FixVersion v) {
    auto dict = std::make_shared<DataDictionary>();
    dict->load_builtin(v);
    DictionaryRegistry::instance().set(v, std::move(dict));
}

const DataDictionary* Engine::dictionary(FixVersion v) const noexcept {
    return DictionaryRegistry::instance().get(v);
}

} // namespace fix
