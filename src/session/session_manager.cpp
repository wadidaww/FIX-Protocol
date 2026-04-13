// =============================================================================
// FIX Protocol Engine - SessionManager implementation
// =============================================================================
#include "fix/session/session_manager.hpp"

namespace fix {

std::string SessionManager::make_key(const SessionID& sid) {
    return sid.senderCompID + ":" + sid.targetCompID + ":" +
           std::string(fix::to_string(sid.version));
}

Session* SessionManager::create_session(SessionConfig cfg,
                                         std::unique_ptr<IMessageStore> store,
                                         const DataDictionary* dict,
                                         SessionCallbacks cbs) {
    if (!store) store = std::make_unique<MemoryStore>();

    auto key  = make_key(cfg.id);
    auto sess = std::make_unique<Session>(std::move(cfg), std::move(store),
                                          dict, std::move(cbs));
    std::unique_lock lock(mutex_);
    auto* ptr = sess.get();
    sessions_[key] = std::move(sess);
    return ptr;
}

Session* SessionManager::find(const SessionID& sid) noexcept {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(make_key(sid));
    return it != sessions_.end() ? it->second.get() : nullptr;
}

const Session* SessionManager::find(const SessionID& sid) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(make_key(sid));
    return it != sessions_.end() ? it->second.get() : nullptr;
}

Session* SessionManager::find(std::string_view sender,
                               std::string_view target) noexcept {
    std::shared_lock lock(mutex_);
    for (auto& [key, sess] : sessions_) {
        const auto& sid = sess->id();
        if (sid.senderCompID == sender && sid.targetCompID == target)
            return sess.get();
    }
    return nullptr;
}

bool SessionManager::remove(const SessionID& sid) {
    std::unique_lock lock(mutex_);
    return sessions_.erase(make_key(sid)) > 0;
}

void SessionManager::for_each(std::function<void(Session&)> fn) {
    std::shared_lock lock(mutex_);
    for (auto& [key, sess] : sessions_) fn(*sess);
}

void SessionManager::tick_all() {
    for_each([](Session& s){ s.on_timer(); });
}

std::size_t SessionManager::count() const noexcept {
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

} // namespace fix
