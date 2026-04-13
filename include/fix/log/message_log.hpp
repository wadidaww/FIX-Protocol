#pragma once
// =============================================================================
// FIX Protocol Engine - Audit Log
// =============================================================================
#include "../core/types.hpp"
#include <string>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>
#include <functional>

namespace fix {

struct AuditEntry {
    TimePoint   timestamp;
    std::string session_id;
    bool        outbound;    // true=sent, false=received
    std::string raw;
};

// ---------------------------------------------------------------------------
// IAuditLog
// ---------------------------------------------------------------------------
class IAuditLog {
public:
    virtual ~IAuditLog() = default;
    virtual void log(AuditEntry entry) = 0;
    virtual void flush() = 0;
    virtual void rotate() = 0;
};

// ---------------------------------------------------------------------------
// FileAuditLog - async append-only file log
// ---------------------------------------------------------------------------
class FileAuditLog : public IAuditLog {
public:
    struct Config {
        std::filesystem::path dir;
        std::string           base_name  = "fix_audit";
        std::size_t           max_size   = 100 * 1024 * 1024; // 100 MB
        bool                  compress   = false;
        int                   retain_days = 7;
    };

    explicit FileAuditLog(Config cfg);
    ~FileAuditLog() override;

    void log(AuditEntry entry) override;
    void flush() override;
    void rotate() override;

private:
    Config          cfg_;
    std::mutex      queue_mutex_;
    std::condition_variable cv_;
    std::queue<AuditEntry>  queue_;
    std::atomic<bool>       running_{true};
    std::thread             writer_thread_;
    std::ofstream           out_;
    std::size_t             current_size_ = 0;

    void write_loop();
    void write_entry(const AuditEntry& e);
    void open_file();
};

// ---------------------------------------------------------------------------
// NullAuditLog (discard all – for tests / benchmarks)
// ---------------------------------------------------------------------------
class NullAuditLog : public IAuditLog {
public:
    void log(AuditEntry) override {}
    void flush() override {}
    void rotate() override {}
};

} // namespace fix
