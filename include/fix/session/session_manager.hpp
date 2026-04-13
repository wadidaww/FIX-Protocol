#pragma once
// =============================================================================
// FIX Protocol Engine - Session Manager
// =============================================================================
#include "session.hpp"
#include "../store/memory_store.hpp"
#include "../dictionary/data_dictionary.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <functional>

namespace fix {

class SessionManager {
public:
    SessionManager() = default;

    // Create and register a session
    Session* create_session(SessionConfig cfg,
                            std::unique_ptr<IMessageStore> store = nullptr,
                            const DataDictionary* dict = nullptr,
                            SessionCallbacks cbs = {});

    // Look up by session ID
    [[nodiscard]] Session* find(const SessionID& sid) noexcept;
    [[nodiscard]] const Session* find(const SessionID& sid) const noexcept;

    // Look up by sender/target
    [[nodiscard]] Session* find(std::string_view sender,
                                std::string_view target) noexcept;

    // Remove a session
    bool remove(const SessionID& sid);

    // Iterate
    void for_each(std::function<void(Session&)> fn);

    // Broadcast timer ticks to all sessions
    void tick_all();

    [[nodiscard]] std::size_t count() const noexcept;

private:
    mutable std::shared_mutex                                    mutex_;
    std::unordered_map<std::string, std::unique_ptr<Session>>    sessions_;

    static std::string make_key(const SessionID& sid);
};

} // namespace fix
