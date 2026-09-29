// =============================================================================
// FIX Protocol Engine - Phase 2 e2e timer + exception-backstop tests
//
// These cover the engine/transport half of task 2.6:
//   * exception boundaries at the engine timer thread, the transport callback
//     lambdas and Engine::stop()'s logout loop (a throwing user callback must
//     surface on EngineConfig::on_error and never kill a background thread),
//   * do_send transport-error surfacing (2.6),
//   * the heartbeat TestRequest → disconnect-when-silent timer path over a
//     real loopback socket.
//
// Helper patterns (free-port discovery, raw FIX framing, deadline polling)
// are mirrored from tests/test_e2e.cpp — that file is read-only here.
// =============================================================================
#include "fix/core/constants.hpp"
#include "fix/engine.hpp"
#include "fix/parser/serializer.hpp"
#include "fix/transport/transport.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

using namespace fix;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// Shared helpers (same patterns as tests/test_e2e.cpp)
// ---------------------------------------------------------------------------

// Ask the OS for a free TCP port (bind to 0, read it back, release it).
// On Windows (documented no-op transport) fall back to a fixed port.
std::uint16_t find_free_port() {
#ifdef _WIN32
    return 34567;
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }
    std::uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
#endif
}

EngineConfig make_engine_config() {
    EngineConfig cfg;
    cfg.enable_audit = false;
    cfg.use_file_store = false;
    cfg.timer_interval_ms = 50;
    return cfg;
}

SessionConfig make_session_cfg(std::string sender, std::string target, bool initiator,
                               int heartbeat_sec = 2) {
    SessionConfig cfg;
    cfg.id.version = FixVersion::FIX_4_4;
    cfg.id.senderCompID = std::move(sender);
    cfg.id.targetCompID = std::move(target);
    cfg.initiator = initiator;
    cfg.heartbeat_interval = heartbeat_sec;
    return cfg;
}

TcpTransportConfig make_transport_cfg(std::uint16_t port, bool initiator) {
    TcpTransportConfig tc;
    tc.host = "127.0.0.1";
    tc.port = port;
    tc.initiator = initiator;
    tc.reconnect_delay = std::chrono::milliseconds{50}; // fast retries keep the test snappy
    tc.reconnect_delay_max = std::chrono::milliseconds{200};
    return tc;
}

// Poll until pred() is true or the deadline passes.
template <typename Pred>
bool wait_until(Pred &&pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Raw FIX framing (MessageBuilder computes BodyLength + CheckSum, so the
// frames are valid on the wire)
// ---------------------------------------------------------------------------
std::string make_raw_logon(std::string_view sender, std::string_view target, SeqNum seq,
                           int heartbeat_sec) {
    MessageBuilder b;
    (void)b.begin("FIX.4.4", msg_types::Logon);
    (void)b.add(tags::SenderCompID, sender);
    (void)b.add(tags::TargetCompID, target);
    (void)b.add(tags::MsgSeqNum, static_cast<std::int64_t>(seq));
    (void)b.add(tags::SendingTime, MessageBuilder::format_timestamp_now());
    (void)b.add(tags::EncryptMethod, std::int64_t(0));
    (void)b.add(tags::HeartBtInt, static_cast<std::int64_t>(heartbeat_sec));
    return b.finish();
}

std::string make_raw_test_request(std::string_view sender, std::string_view target, SeqNum seq,
                                  std::string_view test_req_id) {
    MessageBuilder b;
    (void)b.begin("FIX.4.4", msg_types::TestRequest);
    (void)b.add(tags::SenderCompID, sender);
    (void)b.add(tags::TargetCompID, target);
    (void)b.add(tags::MsgSeqNum, static_cast<std::int64_t>(seq));
    (void)b.add(tags::SendingTime, MessageBuilder::format_timestamp_now());
    (void)b.add(tags::TestReqID, test_req_id);
    return b.finish();
}

// "35=X" followed by the real SOH delimiter — a substring that can only match
// a message-type field, never a value that happens to contain "35=X".
std::string msg_frame(std::string_view msg_type) {
    std::string s = "35=";
    s += msg_type;
    s += SOH;
    return s;
}

#ifndef _WIN32
// --- Raw-socket client helpers ---------------------------------------------
// Connect to 127.0.0.1:port, retrying until the acceptor's listener is up
// (engine.start() binds asynchronously) or `timeout` elapses. Returns fd/-1.
int raw_connect(std::uint16_t port, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0)
                return fd;
            ::close(fd);
        }
        if (std::chrono::steady_clock::now() > deadline)
            return -1;
        std::this_thread::sleep_for(20ms);
    }
}

bool raw_send_all(int fd, const std::string &data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// Read from fd until `stop(buf)` holds, the peer closes the connection
// (recv() == 0 → *eof = true), a hard error hits, or `timeout` elapses.
// Uses short SO_RCVTIMEO slices so the deadline is honoured.
std::string raw_read_until(int fd, std::chrono::milliseconds timeout,
                           const std::function<bool(const std::string &)> &stop,
                           bool *eof = nullptr) {
    if (eof)
        *eof = false;
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 50000; // 50 ms slice
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buf;
    char tmp[4096];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop && stop(buf))
            break;
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) { // peer closed (FIN)
            if (eof)
                *eof = true;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue; // slice expired – re-check the deadline
        if (errno == EINTR)
            continue;
        break; // hard error
    }
    return buf;
}

// Closes the fd on every exit path (including gtest ASSERT early-returns).
struct ScopedFd {
    int fd;
    explicit ScopedFd(int f = -1)
        : fd(f) {}
    ~ScopedFd() {
        if (fd >= 0)
            ::close(fd);
    }
    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;
    int get() const { return fd; }
};
#endif

// ---------------------------------------------------------------------------
// FakeTransport: an in-process ITransport used to prove that a transport-level
// send failure reaches EngineConfig::on_error through Session's do_send
// wiring (2.6) without needing a real socket.
// ---------------------------------------------------------------------------
class FakeTransport : public ITransport {
public:
    void set_on_connected(ConnectedCallback cb) override { on_connected_ = std::move(cb); }
    void set_on_disconnected(DisconnectedCallback cb) override { on_disconnected_ = std::move(cb); }
    void set_on_data(DataCallback cb) override { on_data_ = std::move(cb); }
    void set_on_error(ErrorCallback cb) override { on_error_ = std::move(cb); }

    Result<void> start() override {
        // Simulate an immediately-established connection whose peer Logons
        // straight away: the acceptor must reach Active before we return.
        if (on_connected_)
            on_connected_();
        if (on_data_) {
            const std::string logon = make_raw_logon("CLIENT", "SERVER", 1, 1);
            on_data_(logon.data(), logon.size());
        }
        started_.store(true, std::memory_order_release);
        return {};
    }

    void stop() override { started_.store(false, std::memory_order_release); }
    void disconnect() override {} // no socket to tear down

    Result<void> send(const char *data, std::size_t len) override {
        if (fail_sends.load(std::memory_order_acquire))
            return make_unexpected(ErrorCode::TransportError);
        std::lock_guard lock(mtx_);
        sent_.append(data, len);
        return {};
    }

    bool is_connected() const noexcept override { return started_.load(std::memory_order_acquire); }

    // Test knob: deliver bytes to the session as if the peer had sent them
    // (the fake has no IO thread of its own).
    void inject(const std::string &data) {
        if (on_data_)
            on_data_(data.data(), data.size());
    }

    // Test knobs
    std::atomic<bool> fail_sends{false};

private:
    mutable std::mutex mtx_;
    std::string sent_;
    std::atomic<bool> started_{false};

    ConnectedCallback on_connected_;
    DisconnectedCallback on_disconnected_;
    DataCallback on_data_;
    ErrorCallback on_error_;
};

// Thread-safe collector for EngineConfig::on_error (fires on engine threads).
struct ErrorSink {
    std::mutex mtx;
    std::vector<std::error_code> errors;

    std::size_t size() {
        std::lock_guard lock(mtx);
        return errors.size();
    }
    bool contains(std::error_code ec) {
        std::lock_guard lock(mtx);
        for (const auto &e : errors)
            if (e == ec)
                return true;
        return false;
    }
    std::error_code last() {
        std::lock_guard lock(mtx);
        return errors.empty() ? std::error_code{} : errors.back();
    }
};

// Shared state behind RecordingAudit: it OUTLIVES the sink object itself, so
// a test can keep observing "what did the old sink receive" after the Engine
// has destroyed it in set_audit_log — which is exactly the moment a missing
// re-wire (NIT#5) would start writing into freed memory.
struct AuditCounter {
    std::mutex mtx;
    std::vector<AuditEntry> entries;

    std::size_t size() {
        std::lock_guard lock(mtx);
        return entries.size();
    }
    bool has_outbound(std::string_view needle) {
        std::lock_guard lock(mtx);
        for (const auto &e : entries)
            if (e.outbound && e.raw.find(needle) != std::string::npos)
                return true;
        return false;
    }
    bool has_inbound(std::string_view needle) {
        std::lock_guard lock(mtx);
        for (const auto &e : entries)
            if (!e.outbound && e.raw.find(needle) != std::string::npos)
                return true;
        return false;
    }
};

struct RecordingAudit final : IAuditLog {
    explicit RecordingAudit(std::shared_ptr<AuditCounter> c)
        : counter(std::move(c)) {}
    void log(AuditEntry entry) override {
        std::lock_guard lock(counter->mtx);
        counter->entries.push_back(std::move(entry));
    }
    void flush() override {}
    void rotate() override {}
    std::shared_ptr<AuditCounter> counter;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. Heartbeat timer path over a real socket: a raw client that Logons and
//    then goes SILENT must receive a TestRequest (35=1 with TestReqID) and
//    then have the connection dropped (recv()==0) — the 1.2×HeartBtInt
//    timeout. Heartbeat interval is 1 s so the whole scenario fits in ~8 s.
// ---------------------------------------------------------------------------
TEST(TimerE2E, AcceptorSendsTestRequestThenDisconnectsSilentPeer) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#else
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    // Callback state declared BEFORE the engine so that, on an ASSERT
    // early-return, the engine (and its IO threads) is destroyed first and
    // can no longer touch these.
    std::atomic<bool> timeout_reported{false};
    Engine engine(make_engine_config());

    SessionConfig acc_cfg = make_session_cfg("SERVER", "CLIENT", /*initiator=*/false,
                                             /*heartbeat_sec=*/1);
    acc_cfg.reset_on_logon = true; // fresh handshake, no inherited sequence state
    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_heartbeat_timeout = [&](const SessionID &) {
        timeout_reported.store(true);
    };
    Session *acceptor = engine.add_session(
        acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acceptor_cbs);
    ASSERT_NE(acceptor, nullptr);
    ASSERT_TRUE(engine.start().has_value());

    ScopedFd fd(raw_connect(port, 5000ms));
    ASSERT_GE(fd.get(), 0) << "raw client never reached the acceptor";

    // Valid FIX 4.4 Logon (BodyLength/CheckSum computed by MessageBuilder).
    ASSERT_TRUE(raw_send_all(fd.get(), make_raw_logon("CLIENT", "SERVER", 1, 1)));

    // Phase 1: the acceptor must answer with its own Logon and go Active.
    const std::string logon_reply = raw_read_until(fd.get(), 5000ms, [](const std::string &s) {
        return s.find(msg_frame(msg_types::Logon)) != std::string::npos;
    });
    ASSERT_NE(logon_reply.find(msg_frame(msg_types::Logon)), std::string::npos)
        << "no Logon reply from the acceptor; got: " << logon_reply;
    ASSERT_TRUE(wait_until([&] { return acceptor->state() == SessionState::Active; }, 3000ms))
        << "acceptor never reached Active after the handshake";

    // Phase 2: go SILENT. The timer must raise a TestRequest carrying a
    // TestReqID within the deadline.
    const std::string test_req_frame = msg_frame(msg_types::TestRequest);
    bool eof = false;
    const auto t_silent = std::chrono::steady_clock::now();
    const std::string after_logon = raw_read_until(
        fd.get(), 2500ms,
        [&](const std::string &s) {
            return s.find(test_req_frame) != std::string::npos &&
                   s.find("112=") != std::string::npos;
        },
        &eof);
    const auto test_request_at = std::chrono::steady_clock::now() - t_silent;
    EXPECT_NE(after_logon.find(test_req_frame), std::string::npos)
        << "no TestRequest within 2.5 s of client silence (elapsed "
        << std::chrono::duration_cast<std::chrono::milliseconds>(test_request_at).count()
        << " ms); buffer: " << after_logon;
    EXPECT_NE(after_logon.find("112="), std::string::npos)
        << "TestRequest must carry a TestReqID; buffer: " << after_logon;
    EXPECT_FALSE(eof) << "connection must not be dropped before the TestRequest went out";

    // Phase 3: still silent — the peer must now be disconnected (1.2×HeartBtInt
    // after the unanswered TestRequest) within ~3 s more.
    bool closed = false;
    (void)raw_read_until(fd.get(), 3000ms, [](const std::string &) { return false; }, &closed);
    EXPECT_TRUE(closed) << "acceptor did not drop the silent peer within 3 s of the TestRequest";

    EXPECT_TRUE(timeout_reported.load())
        << "on_heartbeat_timeout must fire when the TestRequest is unanswered";
    engine.stop();
#endif
}

// ---------------------------------------------------------------------------
// 2. A user on_message callback that throws std::runtime_error on the first
//    application message must not take the engine down: the session's
//    per-message catch handles it (Phase 1), and even if something deeper
//    throws the engine-side backstops keep the IO/timer threads alive. Proof:
//    a follow-up TestRequest still gets an answer, and the timer keeps
//    driving heartbeats out of the acceptor afterwards.
// ---------------------------------------------------------------------------
TEST(TimerE2E, EngineSurvivesThrowingUserCallback) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#else
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    // Callback state before the engine (destroyed after it — see test 1).
    std::atomic<bool> acceptor_logged_on{false};
    std::atomic<bool> initiator_logged_on{false};
    std::atomic<int> app_msgs{0};
    std::atomic<bool> threw{false};

    Engine engine(make_engine_config());

    SessionConfig acc_cfg = make_session_cfg("SERVER", "CLIENT", /*initiator=*/false,
                                             /*heartbeat_sec=*/2);
    // Deliberately NOT reset_on_logon (the raw-client tests 1/3 do use it):
    // an acceptor reset echoes ResetSeqNumFlag=Y, and the current session-side
    // handle_logon resets the INITIATOR's outbound seq on that echo while the
    // acceptor still expects seq 2 — the next send() would then be dropped as
    // "MsgSeqNum too low" (session reset semantics, plan 2.8 — not ours).
    const SessionID acceptor_id = acc_cfg.id;
    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_logon = [&](const SessionID &) {
        acceptor_logged_on.store(true);
    };
    acceptor_cbs.on_message = [&](const SessionID &, const Message &) {
        // Throw std::runtime_error on the FIRST application message only.
        if (app_msgs.fetch_add(1) == 0) {
            threw.store(true);
            throw std::runtime_error("user on_message callback exploded");
        }
    };
    Session *acceptor = engine.add_session(
        acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acceptor_cbs);
    ASSERT_NE(acceptor, nullptr);

    SessionCallbacks initiator_cbs;
    initiator_cbs.on_logon = [&](const SessionID &) {
        initiator_logged_on.store(true);
    };
    Session *initiator = engine.add_session(
        make_session_cfg("CLIENT", "SERVER", /*initiator=*/true, /*heartbeat_sec=*/2),
        make_tcp_transport(make_transport_cfg(port, true)), initiator_cbs);
    ASSERT_NE(initiator, nullptr);
    ASSERT_NE(initiator->store(), nullptr);

    ASSERT_TRUE(engine.start().has_value());
    ASSERT_TRUE(wait_until([&] { return acceptor_logged_on.load() && initiator_logged_on.load(); },
                           15000ms))
        << "handshake did not complete";

    // Application message #1 → the acceptor's user callback throws.
    Message order(msg_types::NewOrderSingle);
    order.set(tags::ClOrdID, "ORD-THROW-1");
    order.set(tags::Symbol, "AAPL");
    order.set(tags::Side, "1");
    order.set(tags::OrdType, "2");
    ASSERT_TRUE(initiator->send(order).has_value());
    ASSERT_TRUE(wait_until([&] { return threw.load(); }, 10000ms))
        << "on_message never ran — the throw never happened";

    const auto recv_before = initiator->msgs_received();
    const auto sent_after_throw = acceptor->msgs_sent();

    // The user side sends a TestRequest: the acceptor must still answer —
    // either with the Heartbeat for it, or with a ResendRequest/Logout for
    // the sequence number the throwing dispatch may have skipped. Any reply
    // proves the acceptor's IO thread, parser and session survived the throw.
    const SeqNum seq = initiator->store()->next_sender_seq_num();
    const std::string tr = make_raw_test_request("CLIENT", "SERVER", seq, "PING-1");
    ASSERT_FALSE(tr.empty());
    ASSERT_TRUE(initiator->send_raw(tr).has_value());
    ASSERT_TRUE(wait_until([&] { return initiator->msgs_received() > recv_before; }, 10000ms))
        << "no response from the acceptor after the throwing user callback";

    // And the timer thread must still be driving the acceptor session:
    // with HeartBtInt=2 heartbeats keep going out (msgs_sent is monotonic).
    ASSERT_TRUE(wait_until([&] { return acceptor->msgs_sent() > sent_after_throw; }, 8000ms))
        << "acceptor stopped sending after the throw — timer/IO thread died?";

    EXPECT_TRUE(engine.is_running());
    EXPECT_EQ(engine.get_session(acceptor_id), acceptor);
    EXPECT_GE(app_msgs.load(), 1);

    engine.stop();
#endif
}

// ---------------------------------------------------------------------------
// 3. Engine timer backstop: a user on_heartbeat_timeout callback that throws
//    a NON-std exception (Session's per-message catch only covers
//    std::exception, and on_timer has no catch of its own) must be reported
//    on EngineConfig::on_error instead of std::terminate-ing the timer
//    thread. Surviving past the throw (assertions below, engine.stop(),
//    process still alive) IS the proof — an escaped exception would have
//    aborted the whole test binary.
// ---------------------------------------------------------------------------
TEST(TimerE2E, EngineBackstopSurvivesNonStdTimerCallbackThrow) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#else
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    // Callback state before the engine (destroyed after it — see test 1).
    ErrorSink sink;
    std::atomic<int> timeouts{0};

    EngineConfig ecfg = make_engine_config();
    ecfg.on_error = [&](std::error_code ec) {
        std::lock_guard lock(sink.mtx);
        sink.errors.push_back(ec);
    };
    Engine engine(ecfg);

    SessionConfig acc_cfg = make_session_cfg("SERVER", "CLIENT", /*initiator=*/false,
                                             /*heartbeat_sec=*/1);
    acc_cfg.reset_on_logon = true;
    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_heartbeat_timeout = [&](const SessionID &) {
        // Non-std throw: escapes Session's catch(const std::exception &) and
        // must be caught by the engine's timer-thread backstop.
        if (timeouts.fetch_add(1) == 0)
            throw 42;
    };
    Session *acceptor = engine.add_session(
        acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acceptor_cbs);
    ASSERT_NE(acceptor, nullptr);
    ASSERT_TRUE(engine.start().has_value());

    ScopedFd fd(raw_connect(port, 5000ms));
    ASSERT_GE(fd.get(), 0) << "raw client never reached the acceptor";
    ASSERT_TRUE(raw_send_all(fd.get(), make_raw_logon("CLIENT", "SERVER", 1, 1)));

    // Handshake first: read the Logon reply.
    const std::string logon_reply = raw_read_until(fd.get(), 5000ms, [](const std::string &s) {
        return s.find(msg_frame(msg_types::Logon)) != std::string::npos;
    });
    ASSERT_NE(logon_reply.find(msg_frame(msg_types::Logon)), std::string::npos)
        << "no Logon reply from the acceptor; got: " << logon_reply;

    // Go silent: wait for the TestRequest (the timer path that arms the
    // heartbeat-timeout callback). Generous 6 s deadline: the exact firing
    // point depends on the session-side timer implementation.
    const std::string test_req_frame = msg_frame(msg_types::TestRequest);
    const std::string saw = raw_read_until(fd.get(), 6000ms, [&](const std::string &s) {
        return s.find(test_req_frame) != std::string::npos;
    });
    ASSERT_NE(saw.find(test_req_frame), std::string::npos)
        << "no TestRequest ever arrived — timer path not running; buffer: " << saw;

    // The timeout callback now throws a non-std exception on the timer thread.
    // The engine backstop must surface it on the sink with a meaningful code.
    const bool reported = wait_until([&] { return sink.size() >= 1; }, 3000ms);
    if (!reported) {
        ADD_FAILURE() << "non-std callback exception never reached EngineConfig::on_error "
                         "(the timer thread would have died otherwise)"
                      << "; timeouts=" << timeouts.load()
                      << " state=" << to_string(acceptor->state()) << " sink=" << sink.size()
                      << " msgs_sent=" << acceptor->msgs_sent();
    }
    EXPECT_TRUE(sink.contains(make_error_code(ErrorCode::SessionError)))
        << "backstop must report fix::ErrorCode::SessionError; last code: "
        << sink.last().category().name() << "/" << sink.last().value();
    EXPECT_GE(timeouts.load(), 1);

    // Keep running past several timer ticks: a timer thread killed by the
    // exception would already have std::terminate'd the process, so reaching
    // these assertions and a clean stop() is the survival proof.
    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(engine.is_running()) << "engine died after the backstop fired";
    EXPECT_GE(sink.size(), 1u);

    engine.stop();
    EXPECT_FALSE(engine.is_running());
#endif
}

// ---------------------------------------------------------------------------
// 4. do_send error surfacing (2.6): the transport Result returned by
//    ITransport::send used to be discarded ((void) cast) inside Engine's
//    do_send wiring. With a transport that fails the write, the failure must
//    reach EngineConfig::on_error instead of vanishing.
// ---------------------------------------------------------------------------
TEST(TimerE2E, TransportSendFailureSurfacesOnEngineErrorSink) {
    ErrorSink sink;
    EngineConfig ecfg = make_engine_config();
    ecfg.on_error = [&](std::error_code ec) {
        std::lock_guard lock(sink.mtx);
        sink.errors.push_back(ec);
    };
    Engine engine(ecfg);

    auto fake = std::make_unique<FakeTransport>();
    FakeTransport *fake_ptr = fake.get();

    std::atomic<bool> logged_on{false};
    SessionConfig acc_cfg = make_session_cfg("SERVER", "CLIENT", /*initiator=*/false,
                                             /*heartbeat_sec=*/1);
    acc_cfg.reset_on_logon = true;
    SessionCallbacks cbs;
    cbs.on_logon = [&](const SessionID &) {
        logged_on.store(true);
    };
    Session *acceptor = engine.add_session(acc_cfg, std::move(fake), cbs);
    ASSERT_NE(acceptor, nullptr);

    ASSERT_TRUE(engine.start().has_value());
    ASSERT_TRUE(wait_until(
        [&] { return logged_on.load() && acceptor->state() == SessionState::Active; }, 3000ms))
        << "fake transport handshake never completed";
    ASSERT_EQ(sink.size(), 0u) << "healthy sends must not surface transport errors";

    // Break the transport, then send: do_send must report the failure.
    fake_ptr->fail_sends.store(true);
    Message order(msg_types::NewOrderSingle);
    order.set(tags::ClOrdID, "ORD-SENDFAIL-1");
    order.set(tags::Symbol, "AAPL");
    order.set(tags::Side, "1");
    order.set(tags::OrdType, "2");
    (void)acceptor->send(order); // session-level Result is session territory

    EXPECT_GE(sink.size(), 1u)
        << "transport send failure never reached EngineConfig::on_error (do_send "
           "wiring still drops the Result)";
    EXPECT_EQ(sink.last(), make_error_code(ErrorCode::TransportError))
        << "the surfaced code must be the transport's own error code";

    // The engine keeps running despite the failed write.
    EXPECT_TRUE(engine.is_running());
    engine.stop();
}

// ---------------------------------------------------------------------------
// 5. Engine::set_audit_log must RE-WIRE live sessions (NIT#5). Swapping only
//    the member leaves every session pointing at the OLD sink, which the swap
//    then destroys — the next audited frame is a use-after-free. Covered for
//    both call orders: before add_session (wiring new sessions) and after the
//    handshake (rewiring live ones).
// ---------------------------------------------------------------------------
TEST(TimerE2E, SetAuditLogRewiresLiveSessions) {
    Engine engine(make_engine_config());

    // Sink #1 installed BEFORE add_session: new sessions must pick it up.
    auto counter1 = std::make_shared<AuditCounter>();
    engine.set_audit_log(std::make_unique<RecordingAudit>(counter1));

    auto fake = std::make_unique<FakeTransport>();
    FakeTransport *fake_ptr = fake.get();

    std::atomic<bool> logged_on{false};
    SessionConfig acc_cfg = make_session_cfg("SERVER", "CLIENT", /*initiator=*/false,
                                             /*heartbeat_sec=*/1);
    acc_cfg.reset_on_logon = true;
    SessionCallbacks cbs;
    cbs.on_logon = [&](const SessionID &) {
        logged_on.store(true);
    };
    Session *acceptor = engine.add_session(acc_cfg, std::move(fake), cbs);
    ASSERT_NE(acceptor, nullptr);
    ASSERT_TRUE(engine.start().has_value());
    ASSERT_TRUE(wait_until(
        [&] { return logged_on.load() && acceptor->state() == SessionState::Active; }, 3000ms))
        << "fake transport handshake never completed";

    // The handshake was audited into sink #1...
    EXPECT_GT(counter1->size(), 0u) << "no pre-swap frame reached the first sink";
    EXPECT_TRUE(counter1->has_outbound(msg_frame(msg_types::Logon)));
    const std::size_t first_at_swap = counter1->size();

    // ...then the sink is SWAPPED. This destroys RecordingAudit #1: with a
    // missing re-wire, `acceptor` would keep pointing at the freed object and
    // the next frame below would be a use-after-free (ASan gate) / would land
    // in counter1 (assertion below).
    auto counter2 = std::make_shared<AuditCounter>();
    engine.set_audit_log(std::make_unique<RecordingAudit>(counter2));
    RecordingAudit *second_raw = static_cast<RecordingAudit *>(engine.audit_log());
    ASSERT_NE(second_raw, nullptr);
    EXPECT_EQ(second_raw->counter, counter2);

    // Post-swap traffic, both directions: TX via send(), RX via inject().
    Message hb(msg_types::Heartbeat);
    ASSERT_TRUE(acceptor->send(hb).has_value());
    fake_ptr->inject(make_raw_test_request("CLIENT", "SERVER", 2, "PING-AFTER-SWAP"));

    ASSERT_TRUE(wait_until([&] { return counter2->size() >= 2u; }, 3000ms))
        << "post-swap frames never reached the new sink";
    EXPECT_TRUE(counter2->has_outbound(msg_frame(msg_types::Heartbeat)));
    EXPECT_TRUE(counter2->has_inbound("PING-AFTER-SWAP"));

    // Sink #1's state outlives the destroyed sink object: it must NOT have
    // grown — every post-swap frame went to sink #2 only.
    EXPECT_EQ(counter1->size(), first_at_swap)
        << "a session kept auditing into the destroyed old sink (set_audit_log "
           "swapped the member without re-wiring live sessions)";

    engine.stop();
}

// ---------------------------------------------------------------------------
// 6. Engine::stop() must return cleanly even when a user on_logout callback
//    throws — a throw during the shutdown logout pass must not abort the
//    joins and wedge the process.
// ---------------------------------------------------------------------------
TEST(TimerE2E, StopDoesNotCrashWhenUserLogoutCallbackThrows) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#else
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    // Callback state before the engine (destroyed after it — see test 1).
    std::atomic<bool> acceptor_logged_on{false};
    std::atomic<bool> initiator_logged_on{false};

    Engine engine(make_engine_config());

    auto throwing_logout = [&](const SessionID &, std::string_view) {
        throw std::runtime_error("user on_logout callback exploded");
    };

    SessionConfig acc_cfg = make_session_cfg("SERVER", "CLIENT", /*initiator=*/false,
                                             /*heartbeat_sec=*/2);
    // No reset_on_logon — see test 2 for the ResetSeqNumFlag echo skew.
    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_logon = [&](const SessionID &) {
        acceptor_logged_on.store(true);
    };
    acceptor_cbs.on_logout = throwing_logout;
    Session *acceptor = engine.add_session(
        acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acceptor_cbs);
    ASSERT_NE(acceptor, nullptr);

    SessionCallbacks initiator_cbs;
    initiator_cbs.on_logon = [&](const SessionID &) {
        initiator_logged_on.store(true);
    };
    initiator_cbs.on_logout = throwing_logout;
    Session *initiator = engine.add_session(
        make_session_cfg("CLIENT", "SERVER", /*initiator=*/true, /*heartbeat_sec=*/2),
        make_tcp_transport(make_transport_cfg(port, true)), initiator_cbs);
    ASSERT_NE(initiator, nullptr);

    ASSERT_TRUE(engine.start().has_value());
    ASSERT_TRUE(wait_until([&] { return acceptor_logged_on.load() && initiator_logged_on.load(); },
                           15000ms))
        << "handshake did not complete";

    // A throwing on_logout must neither abort shutdown nor hang the joins.
    engine.stop();
    EXPECT_FALSE(engine.is_running());
    SUCCEED();
#endif
}
