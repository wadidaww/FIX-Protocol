// =============================================================================
// FIX Protocol Engine - Audit log tests (Phase 3, task 3.5)
//
//  * Session wires every RX/TX frame through IAuditLog (RecordingAuditLog in
//    the first tests, FileAuditLog end-to-end afterwards).
//  * Rejected frames (identity violations, bad sequence numbers) are audited
//    BEFORE they are rejected — an attacker's probe must leave a trace.
//  * FileAuditLog rotation stamps, same-second uniqueness, retention and the
//    bounded-queue drop counter.
// =============================================================================
#include "fix/core/constants.hpp"
#include "fix/log/message_log.hpp"
#include "fix/parser/serializer.hpp"
#include "fix/session/session.hpp"
#include "fix/store/memory_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace fix;

namespace {

// PID suffix so parallel suites (ctest -j, several checkouts on one box)
// never share an audit directory.
std::string test_pid() {
#ifdef _WIN32
    return std::to_string(static_cast<unsigned long>(_getpid()));
#else
    return std::to_string(::getpid());
#endif
}

// ---------------------------------------------------------------------------
// Helpers (mirrors tests/test_session.cpp)
// ---------------------------------------------------------------------------
std::string make_wire(std::string_view begin_str, std::string_view msg_type, SeqNum seq,
                      std::string_view sender, std::string_view target,
                      std::initializer_list<std::pair<TagNum, std::string_view>> extra = {}) {
    MessageBuilder b;
    b.begin(begin_str, msg_type);
    b.add(tags::SenderCompID, sender);
    b.add(tags::TargetCompID, target);
    b.add(tags::MsgSeqNum, static_cast<std::int64_t>(seq));
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    for (auto &[tag, val] : extra)
        b.add(tag, val);
    return b.finish();
}

SessionConfig make_cfg() {
    SessionConfig cfg;
    cfg.id.version = FixVersion::FIX_4_2;
    cfg.id.senderCompID = "SERVER";
    cfg.id.targetCompID = "CLIENT";
    cfg.initiator = false; // acceptor
    cfg.heartbeat_interval = 30;
    return cfg;
}

// Thread-safe recording sink.
class RecordingAuditLog : public IAuditLog {
public:
    void log(AuditEntry entry) override {
        std::lock_guard lk(m_);
        entries_.push_back(std::move(entry));
    }
    void flush() override {}
    void rotate() override {}

    std::vector<AuditEntry> snapshot() const {
        std::lock_guard lk(m_);
        return entries_;
    }

private:
    mutable std::mutex m_;
    std::vector<AuditEntry> entries_;
};

std::string read_file(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<std::filesystem::path> audit_files(const std::filesystem::path &dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".log")
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Current local second, formatted like the rotation stamp.
std::string now_stamp() {
    const std::time_t tt = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
    return buf;
}

// "fix_audit_20260927_101530[_N].log" -> "20260927_101530"
std::string stamp_of(const std::filesystem::path &p) {
    std::string s = p.filename().string();
    const std::string prefix = "fix_audit_";
    if (s.rfind(prefix, 0) != 0)
        return "";
    s = s.substr(prefix.size());
    if (s.size() > 4 && s.rfind(".log") == s.size() - 4)
        s.resize(s.size() - 4);
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (true) {
        const std::size_t u = s.find('_', pos);
        parts.push_back(s.substr(pos, u == std::string::npos ? u : u - pos));
        if (u == std::string::npos)
            break;
        pos = u + 1;
    }
    if (parts.size() >= 3) // drop the uniqueness suffix
        parts.resize(2);
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i)
            joined += '_';
        joined += parts[i];
    }
    return joined;
}

std::filesystem::path make_temp_dir(const char *tag) {
    auto *ti = testing::UnitTest::GetInstance()->current_test_info();
    // PID-unique: parallel suites (ctest -j, several checkouts on one box)
    // must never share — one process's remove_all would otherwise delete
    // another's files mid-test.
    auto dir = std::filesystem::temp_directory_path() /
               (std::string("fix_audit_") + tag + "_" + ti->name() + "_" + test_pid());
    std::filesystem::remove_all(dir);
    return dir;
}

} // namespace

// ---------------------------------------------------------------------------
// Session <-> RecordingAuditLog
// ---------------------------------------------------------------------------
TEST(AuditSessionTest, SessionExchangeIsAuditedRXAndTX) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    RecordingAuditLog audit;
    Session sess(make_cfg(), std::make_unique<MemoryStore>(), nullptr, cbs);
    sess.set_audit_log(&audit);
    ASSERT_EQ(sess.audit_log(), &audit);

    // Acceptor start: outgoing Logon (TX).
    ASSERT_TRUE(sess.logon().has_value());

    // Incoming Logon (RX) — session becomes Active.
    const std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                        {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());
    ASSERT_EQ(sess.state(), SessionState::Active);

    // Outbound application/admin message (TX).
    std::size_t before = audit.snapshot().size();
    Message hb(msg_types::Heartbeat);
    ASSERT_TRUE(sess.send(hb).has_value());

    // Inbound TestRequest (RX) + the automatic heartbeat response (TX).
    before = audit.snapshot().size();
    const std::string treq = make_wire("FIX.4.2", msg_types::TestRequest, 2, "CLIENT", "SERVER",
                                       {{tags::TestReqID, "ABCD"}});
    sess.on_data(treq.data(), treq.size());

    const auto entries = audit.snapshot();
    ASSERT_GT(entries.size(), before); // test request produced new entries

    const std::string sid = "FIX.4.2:SERVER->CLIENT";
    int rx_logon = 0, tx_logon = 0, rx_treq = 0, tx_hb = 0, wrong_session = 0;
    for (const auto &e : entries) {
        if (e.session_id != sid)
            ++wrong_session;
        if (!e.outbound && e.raw.find("35=A") != std::string::npos)
            ++rx_logon;
        if (e.outbound && e.raw.find("35=A") != std::string::npos)
            ++tx_logon;
        if (!e.outbound && e.raw.find("35=1") != std::string::npos)
            ++rx_treq;
        if (e.outbound && e.raw.find("35=0") != std::string::npos)
            ++tx_hb;
    }
    EXPECT_EQ(wrong_session, 0);
    EXPECT_GE(rx_logon, 1) << "inbound Logon must be audited";
    EXPECT_GE(tx_logon, 1) << "outbound Logon must be audited";
    EXPECT_GE(rx_treq, 1) << "inbound TestRequest must be audited";
    EXPECT_GE(tx_hb, 1) << "outbound heartbeat response must be audited";

    // RX entries must carry the wire bytes verbatim.
    bool rx_raw_present = false;
    for (const auto &e : entries) {
        if (!e.outbound && e.raw == logon) {
            rx_raw_present = true;
            break;
        }
    }
    EXPECT_TRUE(rx_raw_present) << "audit must store the raw frame, not a summary";
}

TEST(AuditSessionTest, RejectedFrameIsAuditedBeforeRejection) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    RecordingAuditLog audit;
    Session sess(make_cfg(), std::make_unique<MemoryStore>(), nullptr, cbs);
    sess.set_audit_log(&audit);
    ASSERT_TRUE(sess.logon().has_value());
    const std::size_t before = audit.snapshot().size();

    // Impersonation attempt: correct sender, wrong target.
    const std::string evil = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "EVIL",
                                       {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(evil.data(), evil.size());
    ASSERT_EQ(sess.state(), SessionState::Disconnected); // rejected...

    // ...but never invisible: the probe must be in the audit trail.
    const auto entries = audit.snapshot();
    ASSERT_GT(entries.size(), before);
    bool audited = false;
    for (std::size_t i = before; i < entries.size(); ++i) {
        if (!entries[i].outbound && entries[i].raw == evil)
            audited = true;
    }
    EXPECT_TRUE(audited) << "frames rejected for identity violations must still be audited";
}

TEST(AuditSessionTest, NullAuditLogIsSafeAndNoOp) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    Session sess(make_cfg(), std::make_unique<MemoryStore>(), nullptr, cbs);
    // No audit log wired: every frame must flow through unchanged.
    ASSERT_TRUE(sess.logon().has_value());
    const std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                        {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());
    EXPECT_EQ(sess.state(), SessionState::Active);
    EXPECT_EQ(sess.audit_log(), nullptr);
}

// ---------------------------------------------------------------------------
// FileAuditLog end-to-end
// ---------------------------------------------------------------------------
TEST(AuditFileLogTest, WritesSessionExchangeToFile) {
    const auto dir = make_temp_dir("file");

    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    FileAuditLog::Config cfg;
    cfg.dir = dir;
    FileAuditLog audit(cfg);

    Session sess(make_cfg(), std::make_unique<MemoryStore>(), nullptr, cbs);
    sess.set_audit_log(&audit);
    ASSERT_TRUE(sess.logon().has_value());
    const std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                        {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());
    ASSERT_EQ(sess.state(), SessionState::Active);

    Message hb(msg_types::Heartbeat);
    ASSERT_TRUE(sess.send(hb).has_value());

    audit.flush();

    const auto files = audit_files(dir);
    ASSERT_EQ(files.size(), 1u);
    const std::string content = read_file(files[0]);

    EXPECT_NE(content.find("IN  FIX.4.2:SERVER->CLIENT"), std::string::npos);
    EXPECT_NE(content.find("OUT FIX.4.2:SERVER->CLIENT"), std::string::npos);
    // Raw frames with SOH replaced by '|' for readability.
    EXPECT_NE(content.find("35=A|"), std::string::npos);
    EXPECT_NE(content.find("35=0|"), std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST(AuditFileLogTest, RotateStampsNewFileAndIsolatesEntries) {
    const auto dir = make_temp_dir("rotate");

    FileAuditLog::Config cfg;
    cfg.dir = dir;
    FileAuditLog audit(cfg);

    audit.log(AuditEntry{Clock::now(), "S", true, "FIRST"});
    audit.flush();
    auto before = audit_files(dir);
    ASSERT_EQ(before.size(), 1u);
    const auto old_file = audit.current_file();
    ASSERT_EQ(old_file, before[0]);

    const std::string stamp0 = now_stamp();
    // Cross at least one second boundary so the rotation stamp has a chance
    // to differ from the pre-rotation stamp (a stale stamp would re-open the
    // file that was just rotated away).
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    audit.rotate();
    const std::string stamp1 = now_stamp();

    const auto new_file = audit.current_file();
    EXPECT_NE(new_file, old_file) << "rotate must switch to a different file";
    const auto after = audit_files(dir);
    ASSERT_EQ(after.size(), 2u);
    EXPECT_NE(std::find(after.begin(), after.end(), new_file), after.end());
    EXPECT_NE(std::find(after.begin(), after.end(), old_file), after.end())
        << "old_file=" << old_file.string() << " new_file=" << new_file.string()
        << " dir listing: " << [&] {
               std::string acc;
               for (const auto &p : after)
                   acc += p.filename().string() + " ";
               return acc;
           }();

    // The stamp must be the ROTATION time, not the pre-rotation (or worse,
    // original file-creation) time.
    const std::string new_stamp = stamp_of(new_file);
    EXPECT_GT(new_stamp, stamp0) << "rotation stamp must postdate the pre-rotation second";
    EXPECT_LE(new_stamp, stamp1);

    // Entries logged after rotation land ONLY in the new file.
    audit.log(AuditEntry{Clock::now(), "S", true, "SECOND"});
    audit.flush();
    EXPECT_EQ(read_file(old_file).find("SECOND"), std::string::npos);
    EXPECT_NE(read_file(new_file).find("SECOND"), std::string::npos);
    EXPECT_NE(read_file(old_file).find("FIRST"), std::string::npos);

    // Same-second rotation must still create a fresh, uniquely named file.
    audit.rotate();
    audit.log(AuditEntry{Clock::now(), "S", true, "THIRD"});
    audit.flush();
    const auto after2 = audit_files(dir);
    EXPECT_EQ(after2.size(), 3u) << "a same-second rotation must not re-append the old file";
    std::size_t third_hits = 0;
    for (const auto &p : after2)
        if (read_file(p).find("THIRD") != std::string::npos)
            ++third_hits;
    EXPECT_EQ(third_hits, 1u);

    std::filesystem::remove_all(dir);
}

TEST(AuditFileLogTest, RetentionDeletesStaleLogsAtStartAndRotation) {
    const auto dir = make_temp_dir("reten");

    // Stale (> 7 days) and recent (< 7 days) pre-existing rotated logs.
    const auto stale = dir / "fix_audit_20200101_000000.log";
    const auto recent = dir / "fix_audit_20200102_000000.log";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(stale);
        f << "old\n";
    }
    {
        std::ofstream f(recent);
        f << "newish\n";
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(stale, now - std::chrono::hours(24 * 10));
    std::filesystem::last_write_time(recent, now - std::chrono::hours(24 * 1));

    {
        FileAuditLog::Config cfg;
        cfg.dir = dir;
        cfg.retain_days = 7;
        FileAuditLog audit(cfg);

        EXPECT_FALSE(std::filesystem::exists(stale))
            << "start-up retention pass must delete logs older than retain_days";
        EXPECT_TRUE(std::filesystem::exists(recent)) << "logs within the window must survive";
        const auto opened = audit.current_file();
        EXPECT_TRUE(std::filesystem::exists(opened));

        // Rotation triggers another retention pass.
        const auto stale2 = dir / "fix_audit_20200103_000000.log";
        {
            std::ofstream f(stale2);
            f << "old2\n";
        }
        std::filesystem::last_write_time(stale2, now - std::chrono::hours(24 * 10));
        audit.rotate();
        audit.flush();
        EXPECT_FALSE(std::filesystem::exists(stale2))
            << "rotation retention pass must delete stale logs";
        EXPECT_TRUE(std::filesystem::exists(opened))
            << "retention must never delete the file currently being written";
    }

    // retain_days <= 0 keeps everything forever.
    const auto dir2 = make_temp_dir("reten0");
    std::filesystem::create_directories(dir2);
    const auto stale3 = dir2 / "fix_audit_20200101_000000.log";
    {
        std::ofstream f(stale3);
        f << "old3\n";
    }
    std::filesystem::last_write_time(stale3, now - std::chrono::hours(24 * 30));
    {
        FileAuditLog::Config cfg;
        cfg.dir = dir2;
        cfg.retain_days = 0;
        FileAuditLog audit(cfg);
        EXPECT_TRUE(std::filesystem::exists(stale3)) << "retain_days = 0 must keep everything";
    }

    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(dir2);
}

TEST(AuditFileLogTest, BoundedQueueDropsUnderFloodAreCounted) {
    const auto dir = make_temp_dir("flood");

    FileAuditLog::Config cfg;
    cfg.dir = dir;
    cfg.max_queue = 64; // deliberately tiny: producers must outrun the writer
    FileAuditLog audit(cfg);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;
    constexpr int kTotal = kThreads * kPerThread;
    const std::string payload(1024, 'F');

    std::vector<std::thread> ths;
    for (int t = 0; t < kThreads; ++t) {
        ths.emplace_back([&audit, &payload] {
            for (int i = 0; i < kPerThread; ++i)
                audit.log(AuditEntry{Clock::now(), "FLOOD", true, payload});
        });
    }
    for (auto &th : ths)
        th.join();

    const std::uint64_t dropped = audit.dropped();
    EXPECT_GT(dropped, 0u) << "a 64-entry queue under a 4-thread flood must shed load";

    audit.flush();

    // Every non-dropped entry is on disk exactly once — the counter is not
    // cosmetic: lost = dropped.
    std::uint64_t lines = 0;
    for (const auto &p : audit_files(dir)) {
        const std::string content = read_file(p);
        lines += static_cast<std::uint64_t>(std::count(content.begin(), content.end(), '\n'));
    }
    EXPECT_EQ(lines, static_cast<std::uint64_t>(kTotal) - dropped);
    EXPECT_LT(lines, static_cast<std::uint64_t>(kTotal));

    std::filesystem::remove_all(dir);
}
