// =============================================================================
// FIX Protocol Engine - FileAuditLog implementation
//
// Phase 3 (3.5): rotation timestamp captured at ROTATION time (with
// same-second uniqueness), retention by age, bounded queue with drop
// counter, writer-thread-owned stream with a race-free flush().
// =============================================================================
#include "fix/log/message_log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace fix {

namespace {

// "%Y%m%d_%H%M%S" in local time — the filename stamp for a rotation.
std::string format_stamp(TimePoint when) {
    const auto tt = std::chrono::system_clock::to_time_t(when);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "%04d%02d%02d_%02d%02d%02d", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return suffix;
}

} // namespace

FileAuditLog::FileAuditLog(Config cfg)
    : cfg_(std::move(cfg)) {
    std::error_code ec;
    std::filesystem::create_directories(cfg_.dir, ec);
    if (ec)
        throw std::system_error(ec, "FileAuditLog: cannot create log directory '" +
                                        cfg_.dir.string() + "'");
    {
        std::lock_guard fl(file_mutex_);
        apply_retention(); // start-up retention pass (3.5)
        open_file(Clock::now());
    }
    // An audit log that cannot open its file must fail loudly — silently
    // discarding every entry is exactly the C12 failure mode.
    if (!out_.is_open())
        throw std::system_error(make_error_code(ErrorCode::StoreError),
                                "FileAuditLog: cannot open audit file in '" + cfg_.dir.string() +
                                    "'");
    writer_thread_ = std::thread([this] { write_loop(); });
}

FileAuditLog::~FileAuditLog() {
    {
        // The state change AND the notify must happen under queue_mutex_:
        // notifying outside the lock opens a lost-wakeup window where the
        // writer has already evaluated its predicate (running_ still true)
        // but has not yet registered its wait — the notify lands before the
        // wait does and the join() below hangs forever (TSAN's slowdown
        // made this near-deterministic).
        std::lock_guard lock(queue_mutex_);
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
    }
    if (writer_thread_.joinable())
        writer_thread_.join(); // drains the remaining queue + final flush
    std::lock_guard fl(file_mutex_);
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

void FileAuditLog::log(AuditEntry entry) {
    {
        std::lock_guard lock(queue_mutex_);
        // Bounded queue (3.5): prefer dropping the NEW entry over unbounded
        // memory growth under a flood; every drop is counted.
        if (cfg_.max_queue > 0 && queue_.size() >= cfg_.max_queue) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.push(std::move(entry));
    }
    cv_.notify_one();
}

void FileAuditLog::flush() {
    // Protocol: bump a request token, let the writer thread (the ONLY
    // owner of out_) drain the queue and flush the file, then wait for the
    // token to be acknowledged. The old implementation flushed out_ from
    // the caller's thread while the writer was writing it (data race) and
    // busy-waited holding the queue lock.
    std::uint64_t token;
    {
        std::lock_guard lock(queue_mutex_);
        token = ++flush_req_;
    }
    cv_.notify_all();
    std::unique_lock lock(queue_mutex_);
    cv_flush_.wait(lock, [this, token] { return flush_done_ >= token; });
}

void FileAuditLog::rotate() {
    // ROTATION TIME is captured here, before any close/reopen work: the
    // new filename must carry the moment the rotation was requested, not
    // whenever the reopen happens to complete (3.5 / C12).
    const TimePoint when = Clock::now();
    std::lock_guard fl(file_mutex_);
    rotate_locked(when);
}

std::filesystem::path FileAuditLog::current_file() const {
    std::lock_guard fl(file_mutex_);
    return current_path_;
}

void FileAuditLog::write_loop() {
    std::unique_lock lock(queue_mutex_);
    while (true) {
        cv_.wait(lock, [this] {
            return !queue_.empty() || flush_req_ > flush_done_ ||
                   !running_.load(std::memory_order_acquire);
        });

        // Drain everything queued, releasing the queue lock while each entry
        // is written (never hold queue_mutex_ across file_mutex_).
        while (!queue_.empty()) {
            AuditEntry e = std::move(queue_.front());
            queue_.pop();
            lock.unlock();
            write_entry(e);
            lock.lock();
            cv_flush_.notify_all();
        }

        // Acknowledge flush requests only when the queue is empty both
        // before and after the file flush: every entry queued before a
        // flush() call is then guaranteed to be on disk when it returns.
        if (flush_req_ > flush_done_ && queue_.empty()) {
            lock.unlock();
            {
                std::lock_guard fl(file_mutex_);
                if (out_.is_open())
                    out_.flush();
            }
            lock.lock();
            if (queue_.empty()) {
                flush_done_ = flush_req_;
                // Wake flush() waiters — without this notify a flush() that
                // arrives while the queue is already empty (nothing to drain,
                // so the per-entry notify at the bottom of the drain loop
                // never fires) would wait forever for an ack nobody signals.
                cv_flush_.notify_all();
            } else
                cv_.notify_all(); // more work raced in — loop again
        }

        if (!running_.load(std::memory_order_acquire) && queue_.empty() &&
            flush_req_ == flush_done_)
            break;
    }
}

void FileAuditLog::write_entry(const AuditEntry &e) {
    std::lock_guard fl(file_mutex_);
    if (!out_.is_open()) {
        // Nothing to write to (open failed mid-run): count the loss instead
        // of dropping it silently.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ISO-8601 timestamp of the EVENT (entry capture time, not write time)
    auto tt = std::chrono::system_clock::to_time_t(e.timestamp);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(e.timestamp.time_since_epoch()) %
        1000;
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    char ts[64];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  (int)ms.count());

    std::string line;
    line.reserve(e.raw.size() + 80);
    line += ts;
    line += ' ';
    line += (e.outbound ? "OUT " : "IN  ");
    line += e.session_id;
    line += ' ';
    // Replace SOH with | for readability
    for (char c : e.raw)
        line += (c == '\x01') ? '|' : c;
    line += '\n';

    out_ << line;
    current_size_ += line.size();

    if (current_size_ >= cfg_.max_size)
        rotate_locked(Clock::now()); // size-triggered rotation, same call stack
}

void FileAuditLog::open_file(TimePoint when) {
    const std::string stamp = format_stamp(when);
    const std::string stem = cfg_.base_name + '_' + stamp;
    auto path = cfg_.dir / (stem + ".log");

    // Same-second rotations (and a restart within one second) must never
    // re-append into a file that already exists — that was the old
    // rotate()-reopens-the-same-file bug (C12). Disambiguate with _1, _2, ...
    std::error_code ec;
    int n = 0;
    while (std::filesystem::exists(path, ec) && !ec) {
        path = cfg_.dir / (stem + '_' + std::to_string(++n) + ".log");
        if (n > 1000000)
            break; // paranoia: cannot happen with sane rotation rates
    }

    out_.open(path, std::ios::app | std::ios::binary);
    current_path_ = out_.is_open() ? path : std::filesystem::path{};
    current_size_ = 0;
}

void FileAuditLog::rotate_locked(TimePoint when) {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    current_path_.clear(); // the rotated file is no longer "current" —
                           // retention may now delete it if it is stale
    apply_retention();
    open_file(when);
}

void FileAuditLog::apply_retention() {
    // Retention (3.5): delete rotated logs older than retain_days.
    // retain_days <= 0 = keep forever (explicit opt-out).
    if (cfg_.retain_days <= 0)
        return;

    std::error_code ec;
    const auto cutoff =
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24) * cfg_.retain_days;
    const std::string want_prefix = cfg_.base_name + '_';

    std::filesystem::directory_iterator it(cfg_.dir, ec);
    if (ec)
        return;
    const std::filesystem::directory_iterator end;
    while (it != end) {
        std::error_code ec2;
        const auto p = it->path();
        it.increment(ec2); // advance BEFORE any possible remove()
        if (ec2)
            break;
        if (!std::filesystem::is_regular_file(p, ec2) || ec2)
            continue;
        if (p == current_path_)
            continue; // never delete the file we are writing
        const auto name = p.filename().string();
        if (name.rfind(want_prefix, 0) != 0)
            continue; // only our own "<base>_<stamp>.log" files
        if (p.extension() != ".log")
            continue;
        const auto m = std::filesystem::last_write_time(p, ec2);
        if (ec2)
            continue;
        if (m < cutoff)
            std::filesystem::remove(p, ec2); // best effort: a locked/stale
                                             // file must not break rotation
    }
}

} // namespace fix
