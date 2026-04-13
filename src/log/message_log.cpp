// =============================================================================
// FIX Protocol Engine - FileAuditLog implementation
// =============================================================================
#include "fix/log/message_log.hpp"
#include <iomanip>
#include <ctime>
#include <sstream>
#include <chrono>

namespace fix {

FileAuditLog::FileAuditLog(Config cfg) : cfg_(std::move(cfg)) {
    std::filesystem::create_directories(cfg_.dir);
    open_file();
    writer_thread_ = std::thread([this]{ write_loop(); });
}

FileAuditLog::~FileAuditLog() {
    running_.store(false);
    cv_.notify_all();
    if (writer_thread_.joinable()) writer_thread_.join();
    if (out_.is_open()) out_.close();
}

void FileAuditLog::log(AuditEntry entry) {
    std::lock_guard lock(queue_mutex_);
    queue_.push(std::move(entry));
    cv_.notify_one();
}

void FileAuditLog::flush() {
    // Wait until queue is drained
    while (true) {
        std::lock_guard lock(queue_mutex_);
        if (queue_.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (out_.is_open()) out_.flush();
}

void FileAuditLog::rotate() {
    if (out_.is_open()) out_.close();
    open_file();
    current_size_ = 0;
}

void FileAuditLog::write_loop() {
    while (running_.load() || [this]{
        std::lock_guard lock(queue_mutex_);
        return !queue_.empty(); }()) {
        std::unique_lock lock(queue_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100),
                     [this]{ return !queue_.empty() || !running_.load(); });
        while (!queue_.empty()) {
            AuditEntry e = std::move(queue_.front());
            queue_.pop();
            lock.unlock();
            write_entry(e);
            lock.lock();
        }
    }
}

void FileAuditLog::write_entry(const AuditEntry& e) {
    if (!out_.is_open()) return;

    // ISO-8601 timestamp
    auto tt  = std::chrono::system_clock::to_time_t(e.timestamp);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   e.timestamp.time_since_epoch()) % 1000;
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  (int)ms.count());

    std::string line;
    line.reserve(e.raw.size() + 80);
    line += ts;
    line += ' ';
    line += (e.outbound ? "OUT " : "IN  ");
    line += e.session_id;
    line += ' ';
    // Replace SOH with | for readability
    for (char c : e.raw) line += (c == '\x01') ? '|' : c;
    line += '\n';

    out_ << line;
    current_size_ += line.size();

    if (current_size_ >= cfg_.max_size) {
        out_.flush();
        rotate();
    }
}

void FileAuditLog::open_file() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "%04d%02d%02d_%02d%02d%02d",
                  tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    auto path = cfg_.dir / (cfg_.base_name + "_" + suffix + ".log");
    out_.open(path, std::ios::app | std::ios::binary);
}

} // namespace fix
