// =============================================================================
// FIX Protocol Engine - End-to-end loopback tests
//
// These tests exercise the full stack over a real socket: Engine → Session →
// TcpTransport → loopback TCP → parser → Session. They exist specifically to
// catch integration bugs that unit tests cannot see (e.g. the engine never
// wiring Session::send to the transport, or the event loop wedging after a
// peer disconnect).
// =============================================================================
#include "fix/core/constants.hpp"
#include "fix/engine.hpp"
#include "fix/parser/parser.hpp"
#include "fix/parser/serializer.hpp"
#include "fix/transport/transport.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
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

SessionConfig make_session_cfg(std::string sender, std::string target, bool initiator) {
    SessionConfig cfg;
    cfg.id.version = FixVersion::FIX_4_2;
    cfg.id.senderCompID = std::move(sender);
    cfg.id.targetCompID = std::move(target);
    cfg.initiator = initiator;
    cfg.heartbeat_interval = 5;
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

struct Endpoints {
    std::atomic<bool> acceptor_logged_on{false};
    std::atomic<bool> initiator_logged_on{false};
    std::mutex mtx;
    std::vector<Message> received; // on acceptor side
    std::vector<Message> initiator_received;

    void on_acceptor_message(const Message &m) {
        std::lock_guard lk(mtx);
        received.push_back(m);
    }
    void on_initiator_message(const Message &m) {
        std::lock_guard lk(mtx);
        initiator_received.push_back(m);
    }
    bool acceptor_got(const std::string &cl_ord_id) {
        std::lock_guard lk(mtx);
        for (const auto &m : received)
            if (m.get(tags::ClOrdID).value_or("") == cl_ord_id)
                return true;
        return false;
    }
};

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

// A well-formed FIX 4.2 Logon frame with caller-chosen CompIDs / seq.
std::string make_raw_logon(std::string_view sender, std::string_view target, SeqNum seq) {
    MessageBuilder b;
    b.begin("FIX.4.2", msg_types::Logon);
    b.add(tags::SenderCompID, sender);
    b.add(tags::TargetCompID, target);
    b.add(tags::MsgSeqNum, static_cast<std::int64_t>(seq));
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    b.add(tags::EncryptMethod, std::int64_t(0));
    b.add(tags::HeartBtInt, std::int64_t(5));
    return b.finish();
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// 1. Handshake + application message flow through the public API.
//    Would have caught: do_send never wired (C1), on_connected empty (C2).
// ---------------------------------------------------------------------------
TEST(EndToEnd, HandshakeAndOrderFlow) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#endif
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    Engine engine(make_engine_config());
    Endpoints ep;

    // Acceptor first so its listener is registered before the initiator starts.
    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_logon = [&](const SessionID &) {
        ep.acceptor_logged_on.store(true);
    };
    acceptor_cbs.on_message = [&](const SessionID &, const Message &m) {
        ep.on_acceptor_message(m);
    };
    Session *acceptor = engine.add_session(
        make_session_cfg("SERVER", "CLIENT", /*initiator=*/false),
        make_tcp_transport(make_transport_cfg(port, /*initiator=*/false)), acceptor_cbs);
    ASSERT_NE(acceptor, nullptr);

    SessionCallbacks initiator_cbs;
    initiator_cbs.on_logon = [&](const SessionID &) {
        ep.initiator_logged_on.store(true);
    };
    initiator_cbs.on_message = [&](const SessionID &, const Message &m) {
        ep.on_initiator_message(m);
    };
    Session *initiator = engine.add_session(
        make_session_cfg("CLIENT", "SERVER", /*initiator=*/true),
        make_tcp_transport(make_transport_cfg(port, /*initiator=*/true)), initiator_cbs);
    ASSERT_NE(initiator, nullptr);

    ASSERT_TRUE(engine.start().has_value());

    // Both sides must reach Active (auto-Logon on the initiator, reply on the
    // acceptor) within the deadline.
    ASSERT_TRUE(wait_until(
        [&] { return ep.acceptor_logged_on.load() && ep.initiator_logged_on.load(); }, 15000ms))
        << "handshake did not complete: acceptor=" << ep.acceptor_logged_on.load()
        << " initiator=" << ep.initiator_logged_on.load();
    EXPECT_EQ(initiator->state(), SessionState::Active);
    EXPECT_EQ(acceptor->state(), SessionState::Active);

    // Send an application message through the public API: this is the actual
    // C1 regression check — bytes must reach the wire and the far side.
    Message order(msg_types::NewOrderSingle);
    order.set(tags::ClOrdID, "ORD-E2E-1");
    order.set(tags::Symbol, "AAPL");
    order.set(tags::Side, "1");
    order.set(tags::OrdType, "2");
    auto sent = initiator->send(order);
    ASSERT_TRUE(sent.has_value()) << "send() failed: " << sent.error().message();

    ASSERT_TRUE(wait_until([&] { return ep.acceptor_got("ORD-E2E-1"); }, 10000ms))
        << "application message never reached the acceptor";

    // And the initiator must receive the ExecutionReport the acceptor sends back.
    Message ack(msg_types::ExecutionReport);
    ack.set(tags::ClOrdID, "ORD-E2E-1");
    ack.set(tags::OrderID, "EX-1");
    ASSERT_TRUE(acceptor->send(ack).has_value());

    bool got_report = wait_until(
        [&] {
            std::lock_guard lk(ep.mtx);
            for (const auto &m : ep.initiator_received)
                if (m.msg_type() == msg_types::ExecutionReport)
                    return true;
            return false;
        },
        10000ms);
    EXPECT_TRUE(got_report) << "ExecutionReport never reached the initiator";

    engine.stop();
}

// ---------------------------------------------------------------------------
// 2. Duplicate SessionID must be rejected, never silently replace (C4).
// ---------------------------------------------------------------------------
TEST(EndToEnd, DuplicateSessionIdRejected) {
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    Engine engine(make_engine_config());
    SessionCallbacks cbs;

    Session *first = engine.add_session(make_session_cfg("SERVER", "CLIENT", false),
                                        make_tcp_transport(make_transport_cfg(port, false)), cbs);
    ASSERT_NE(first, nullptr);

    Session *dup = engine.add_session(make_session_cfg("SERVER", "CLIENT", false),
                                      make_tcp_transport(make_transport_cfg(port + 1, false)), cbs);
    EXPECT_EQ(dup, nullptr) << "duplicate SessionID must be rejected";

    // Different qualifier = different session = allowed
    SessionConfig qcfg = make_session_cfg("SERVER", "CLIENT", false);
    qcfg.id.qualifier = "ALT";
    Session *qualified =
        engine.add_session(qcfg, make_tcp_transport(make_transport_cfg(port + 2, false)), cbs);
    EXPECT_NE(qualified, nullptr);

    engine.stop();
}

// ---------------------------------------------------------------------------
// 3. Acceptor must serve a *second* connection after the first peer goes away
//    (regression for the event-loop wedge: recv()==0 used to leave the epoll
//    loop spinning forever so the acceptor never accepted again).
//    Also covers remove_session under a live engine and add_session-after-start.
// ---------------------------------------------------------------------------
TEST(EndToEnd, AcceptorRecoversAfterDisconnect) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#endif
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    Engine engine(make_engine_config());

    std::atomic<int> acceptor_logons{0};
    std::atomic<bool> first_logon{false};
    std::atomic<bool> re_logon{false};

    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_logon = [&](const SessionID &) {
        acceptor_logons.fetch_add(1);
        first_logon.store(true);
    };
    {
        SessionConfig cfg = make_session_cfg("SERVER", "CLIENT", false);
        Session *acceptor = engine.add_session(
            cfg, make_tcp_transport(make_transport_cfg(port, false)), acceptor_cbs);
        ASSERT_NE(acceptor, nullptr);
    }

    // First initiator connects and handshakes.
    {
        SessionCallbacks icbs;
        SessionConfig icfg = make_session_cfg("CLIENT", "SERVER", true);
        SessionID first_initiator_id = icfg.id;
        Session *init1 =
            engine.add_session(icfg, make_tcp_transport(make_transport_cfg(port, true)), icbs);
        ASSERT_NE(init1, nullptr);
        ASSERT_TRUE(engine.start().has_value());
        ASSERT_TRUE(wait_until([&] { return first_logon.load(); }, 15000ms))
            << "first handshake failed";

        // Tear the first initiator down (removes session + stops transport).
        EXPECT_TRUE(engine.remove_session(first_initiator_id));
    }

    // Acceptor must return to accept() and handle a brand-new initiator.
    {
        SessionCallbacks icbs;
        icbs.on_logon = [&](const SessionID &) {
            re_logon.store(true);
        };
        SessionConfig icfg = make_session_cfg("CLIENT", "SERVER", true);
        Session *init2 =
            engine.add_session(icfg, // after start(): must auto-start
                               make_tcp_transport(make_transport_cfg(port, true)), icbs);
        ASSERT_NE(init2, nullptr);
        ASSERT_TRUE(wait_until([&] { return re_logon.load(); }, 15000ms))
            << "acceptor did not serve a second connection (event-loop wedge regression)";
        EXPECT_EQ(init2->state(), SessionState::Active);
        EXPECT_EQ(acceptor_logons.load(), 2) << "acceptor should have logged on twice";
    }

    engine.stop();
}

// ---------------------------------------------------------------------------
// 4. F1 regression: a raw peer sending a Logon with the WRONG CompID must get
//    a Logout followed by a CLOSED connection (recv()==0) — Session::
//    disconnect() has to invoke do_disconnect so the transport actually drops
//    the socket — and the acceptor must then serve a second, valid client.
// ---------------------------------------------------------------------------
TEST(EndToEnd, WrongCompIdRawClientLogoutThenConnectionClosed) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#else
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    Engine engine(make_engine_config());
    std::atomic<int> acceptor_logons{0};

    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_logon = [&](const SessionID &) {
        ++acceptor_logons;
    };
    Session *acceptor = engine.add_session(
        make_session_cfg("SERVER", "CLIENT", /*initiator=*/false),
        make_tcp_transport(make_transport_cfg(port, /*initiator=*/false)), acceptor_cbs);
    ASSERT_NE(acceptor, nullptr);
    ASSERT_TRUE(engine.start().has_value());

    // --- Client 1: hostile peer, Logon addressed to the wrong CompID ------
    const int fd = raw_connect(port, 5000ms);
    ASSERT_GE(fd, 0) << "raw client never reached the acceptor";
    ASSERT_TRUE(raw_send_all(fd, make_raw_logon("CLIENT", "EVIL", 1)));

    bool eof = false;
    const std::string resp =
        raw_read_until(fd, 5000ms, [](const std::string &) { return false; }, &eof);
    ::close(fd);

    EXPECT_TRUE(eof) << "acceptor must close the socket after a CompID violation (F1); got: "
                     << resp;
    EXPECT_NE(resp.find("35=5"), std::string::npos)
        << "expected a Logout before the connection closed; got: " << resp;
    EXPECT_EQ(acceptor_logons.load(), 0) << "a wrong-CompID Logon must never activate";

    // --- Client 2: the acceptor must recover and serve a valid peer -------
    const int fd2 = raw_connect(port, 5000ms);
    ASSERT_GE(fd2, 0) << "acceptor did not accept a second client (F1 recovery)";
    ASSERT_TRUE(raw_send_all(fd2, make_raw_logon("CLIENT", "SERVER", 1)));
    const std::string resp2 = raw_read_until(
        fd2, 5000ms, [](const std::string &s) { return s.find("35=A") != std::string::npos; });
    ::close(fd2);

    EXPECT_NE(resp2.find("35=A"), std::string::npos)
        << "second raw client must receive a Logon reply; got: " << resp2;
    ASSERT_TRUE(wait_until([&] { return acceptor->state() == SessionState::Active; }, 5000ms));
    EXPECT_EQ(acceptor_logons.load(), 1);

    engine.stop();
#endif
}

// ---------------------------------------------------------------------------
// 5. F3 regression: engine.remove_session() called from inside an on_message
//    callback (i.e. from a session's own transport IO thread) must not crash,
//    deadlock or terminate — the engine still has to stop cleanly afterwards.
// ---------------------------------------------------------------------------
TEST(EndToEnd, RemoveSessionFromMessageCallbackIsSafe) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#endif
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    Engine engine(make_engine_config());
    std::atomic<bool> acceptor_logged_on{false};
    std::atomic<bool> initiator_logged_on{false};
    std::atomic<bool> first_message{false};
    std::atomic<bool> removed{false};

    const SessionConfig acc_cfg = make_session_cfg("SERVER", "CLIENT", false);
    const SessionID acceptor_id = acc_cfg.id;

    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_logon = [&](const SessionID &) {
        acceptor_logged_on.store(true);
    };
    acceptor_cbs.on_message = [&](const SessionID &sid, const Message &) {
        // F3: remove the session from its own IO thread exactly once.
        if (!first_message.exchange(true))
            removed.store(engine.remove_session(sid));
    };
    Session *acceptor = engine.add_session(
        acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acceptor_cbs);
    ASSERT_NE(acceptor, nullptr);

    SessionCallbacks initiator_cbs;
    initiator_cbs.on_logon = [&](const SessionID &) {
        initiator_logged_on.store(true);
    };
    Session *initiator =
        engine.add_session(make_session_cfg("CLIENT", "SERVER", true),
                           make_tcp_transport(make_transport_cfg(port, true)), initiator_cbs);
    ASSERT_NE(initiator, nullptr);

    ASSERT_TRUE(engine.start().has_value());
    ASSERT_TRUE(wait_until([&] { return acceptor_logged_on.load() && initiator_logged_on.load(); },
                           15000ms))
        << "handshake did not complete";

    Message order(msg_types::NewOrderSingle);
    order.set(tags::ClOrdID, "ORD-F3-1");
    order.set(tags::Symbol, "AAPL");
    order.set(tags::Side, "1");
    order.set(tags::OrdType, "2");
    ASSERT_TRUE(initiator->send(order).has_value());

    ASSERT_TRUE(wait_until([&] { return first_message.load() && removed.load(); }, 10000ms))
        << "on_message never ran, or remove_session() failed from the callback";
    ASSERT_TRUE(wait_until([&] { return engine.get_session(acceptor_id) == nullptr; }, 5000ms))
        << "removed session still visible in the manager";

    // The engine keeps running: a further send towards the now-dead peer is
    // allowed to fail at the transport, but must not crash anything.
    Message order2(msg_types::NewOrderSingle);
    order2.set(tags::ClOrdID, "ORD-F3-2");
    order2.set(tags::Symbol, "MSFT");
    order2.set(tags::Side, "1");
    order2.set(tags::OrdType, "2");
    (void)initiator->send(order2);

    // Must return without hanging or aborting.
    engine.stop();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// 6. Reviewer gap: initiator auto-reconnect after the peer connection drops —
//    handshake → remove the acceptor (listener + connection close → FIN at
//    the initiator) → re-add the acceptor under the SAME ID (re-listens) →
//    the initiator must reconnect and re-logon, both sides Active again.
// ---------------------------------------------------------------------------
TEST(EndToEnd, InitiatorReconnectsAfterAcceptorRestart) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#endif
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    Engine engine(make_engine_config());
    std::atomic<int> acceptor_logons{0};
    std::atomic<int> initiator_logons{0};

    const SessionConfig acc_cfg = make_session_cfg("SERVER", "CLIENT", false);
    const SessionID acceptor_id = acc_cfg.id;
    SessionCallbacks acceptor_cbs;
    acceptor_cbs.on_logon = [&](const SessionID &) {
        ++acceptor_logons;
    };
    Session *acceptor = engine.add_session(
        acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acceptor_cbs);
    ASSERT_NE(acceptor, nullptr);

    SessionCallbacks initiator_cbs;
    initiator_cbs.on_logon = [&](const SessionID &) {
        ++initiator_logons;
    };
    Session *initiator =
        engine.add_session(make_session_cfg("CLIENT", "SERVER", true),
                           make_tcp_transport(make_transport_cfg(port, true)), initiator_cbs);
    ASSERT_NE(initiator, nullptr);

    ASSERT_TRUE(engine.start().has_value());
    ASSERT_TRUE(wait_until(
        [&] { return acceptor_logons.load() >= 1 && initiator_logons.load() >= 1; }, 15000ms))
        << "initial handshake failed";
    ASSERT_EQ(initiator->state(), SessionState::Active);

    // Drop the acceptor entirely: the listener and the live connection close,
    // so the initiator transport observes a FIN and starts reconnecting.
    ASSERT_TRUE(engine.remove_session(acceptor_id));

    // Re-add with the SAME SessionID: the fresh transport re-listens on the
    // same port (add_session after start() auto-starts the transport).
    Session *acceptor2 = engine.add_session(
        acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acceptor_cbs);
    ASSERT_NE(acceptor2, nullptr);

    ASSERT_TRUE(wait_until(
        [&] { return acceptor_logons.load() >= 2 && initiator_logons.load() >= 2; }, 20000ms))
        << "initiator did not reconnect + re-logon (acceptor=" << acceptor_logons.load()
        << " initiator=" << initiator_logons.load() << ")";
    EXPECT_EQ(acceptor2->state(), SessionState::Active);
    EXPECT_EQ(initiator->state(), SessionState::Active);

    engine.stop();
}
