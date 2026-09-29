// =============================================================================
// FIX Protocol Engine - SessionManager implementation
// =============================================================================
#include "fix/session/session_manager.hpp"

#include <vector>

namespace fix {

std::string SessionManager::make_key(const SessionID &sid) {
    // qualifier participates: sessions that differ only by qualifier must not
    // collide (they are distinct FIX sessions).
    return sid.senderCompID + ":" + sid.targetCompID + ":" +
           std::string(fix::to_string(sid.version)) + ":" + sid.qualifier;
}

std::shared_ptr<Session> SessionManager::create_session(SessionConfig cfg,
                                                        std::unique_ptr<IMessageStore> store,
                                                        const DataDictionary *dict,
                                                        SessionCallbacks cbs) {
    if (!store)
        store = std::make_unique<MemoryStore>();

    auto key = make_key(cfg.id);
    auto sess = std::make_shared<Session>(std::move(cfg), std::move(store), dict, std::move(cbs));
    std::unique_lock lock(mutex_);
    auto [it, inserted] = sessions_.try_emplace(key, sess);
    if (!inserted)
        return {}; // duplicate SessionID – never replace a live session
    return sess;
}

std::shared_ptr<Session> SessionManager::find_shared(const SessionID &sid) {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(make_key(sid));
    return it != sessions_.end() ? it->second : nullptr;
}

Session *SessionManager::find(const SessionID &sid) noexcept {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(make_key(sid));
    return it != sessions_.end() ? it->second.get() : nullptr;
}

const Session *SessionManager::find(const SessionID &sid) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(make_key(sid));
    return it != sessions_.end() ? it->second.get() : nullptr;
}

Session *SessionManager::find(std::string_view sender, std::string_view target) noexcept {
    std::shared_lock lock(mutex_);
    for (auto &[key, sess] : sessions_) {
        const auto &sid = sess->id();
        if (sid.senderCompID == sender && sid.targetCompID == target)
            return sess.get();
    }
    return nullptr;
}

bool SessionManager::remove(const SessionID &sid) {
    std::unique_lock lock(mutex_);
    return sessions_.erase(make_key(sid)) > 0;
}

void SessionManager::for_each(std::function<void(Session &)> fn) {
    // 3.1: snapshot under the lock, release it, THEN iterate. The callback
    // reaches user code and network I/O (timer pass → Session::on_timer →
    // on_* callbacks / do_send; shutdown pass → logout), and holding
    // shared_lock across that deadlocked the moment a callback called
    // create_session()/remove() (unique_lock on the same non-recursive
    // shared_mutex from the owning thread = UB/deadlock). The shared_ptr
    // snapshot also keeps a session alive for the whole pass even if another
    // thread removes it mid-iteration; sessions added concurrently are picked
    // up by the next pass.
    std::vector<std::shared_ptr<Session>> snapshot;
    {
        std::shared_lock lock(mutex_);
        snapshot.reserve(sessions_.size());
        for (const auto &[key, sess] : sessions_)
            snapshot.push_back(sess);
    }
    for (const auto &sess : snapshot)
        fn(*sess);
}

std::size_t SessionManager::count() const noexcept {
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

} // namespace fix
