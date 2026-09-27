#pragma once
// =============================================================================
// FIX Protocol Engine - Audit Log
//
// Phase 3 (3.5):
//  * Session wires every RX/TX frame through IAuditLog::log (see
//    Session::set_audit_log) — the log is no longer constructed-but-never-
//    written (C12).
//  * rotate() captures the file timestamp AT ROTATION TIME (before the
//    close/reopen dance) and de-duplicates same-second filenames, so a
//    rotation can never silently re-append into the file it just rotated
//    away (the old reopen-same-second bug).
//  * Retention: rotated logs older than Config::retain_days are deleted on
//    rotation and at start-up. retain_days <= 0 keeps everything forever.
//  * The async queue is BOUNDED (Config::max_queue): under a flood the
//    excess entries are dropped and COUNTED (dropped()), never allowed to
//    grow the queue without limit.
//  * flush() is thread-safe: it waits for the writer thread to drain the
//    queue and fsync-free flush the file — the caller never touches the
//    stream that the writer owns (the old flush() raced the writer on
//    out_ and busy-waited with the queue lock held).
// =============================================================================
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>

#include "../core/types.hpp"

namespace fix {

struct AuditEntry {
    TimePoint timestamp;
    std::string session_id;
    bool outbound; // true=sent, false=received
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
        std::string base_name = "fix_audit";
        std::size_t max_size = 100 * 1024 * 1024; // 100 MB
        // Reserved: compression of rotated files is NOT implemented — the
        // flag is accepted (and ignored) so existing EngineConfig-style
        // callers keep compiling. Documented as deferred, not a no-op lie.
        bool compress = false;
        // Delete rotated logs older than N days (checked at construction
        // and on every rotation). <= 0 = keep forever.
        int retain_days = 7;
        // Bounded queue: log() drops the NEW entry (and counts it) once
        // max_queue entries are waiting for the writer thread.
        // 0 = unbounded (never drop).
        std::size_t max_queue = 4096;
    };

    // Throws std::system_error when the directory cannot be created or the
    // log file cannot be opened — an audit log that cannot open its file
    // must fail loudly, never silently discard every entry.
    explicit FileAuditLog(Config cfg);
    ~FileAuditLog() override;

    void log(AuditEntry entry) override;
    void flush() override;
    void rotate() override;

    // Entries dropped because the bounded queue was full (3.5). Readable
    // from any thread; polled by the engine's error path (wiring note: the
    // Engine timer loop can compare against its last sample and report a
    // StoreError on EngineConfig::on_error when it increases).
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_acquire);
    }
    // Path of the file currently being written (diagnostics/tests).
    [[nodiscard]] std::filesystem::path current_file() const;

private:
    Config cfg_;
    // file_mutex_ guards out_/current_path_/current_size_ — owned by the
    // writer thread during write_entry, taken by flush()/rotate() from
    // other threads. NEVER held while queue_mutex_ is requested (lock order
    // is queue_mutex_ first, and only when file_mutex_ is not held).
    mutable std::mutex file_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;       // writer wake: entries / flush req / shutdown
    std::condition_variable cv_flush_; // flush() waiters
    std::queue<AuditEntry> queue_;
    std::atomic<bool> running_{true};
    std::thread writer_thread_;
    std::ofstream out_;
    std::filesystem::path current_path_;
    std::size_t current_size_ = 0;
    std::uint64_t flush_req_ = 0;  // queue_mutex_
    std::uint64_t flush_done_ = 0; // queue_mutex_
    std::atomic<std::uint64_t> dropped_{0};

    void write_loop();
    void write_entry(const AuditEntry &e); // takes file_mutex_
    // Caller holds file_mutex_. `when` is the ROTATION TIME (captured by
    // the caller before any close/reopen), never the reopen instant.
    void open_file(TimePoint when);
    void rotate_locked(TimePoint when); // caller holds file_mutex_
    void apply_retention();             // caller holds file_mutex_
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
