// =============================================================================
// FIX Protocol Engine - Transport + lock-order hardening tests (Phase 3)
//
// Covers tasks 3.1 / 3.3 / 3.7 of docs/proposed-plan.md §5:
//   3.1  SessionManager::for_each must NOT hold the registry lock across its
//        callback (snapshot-then-iterate) — a callback that mutates the
//        manager (create/remove) deadlocked pre-3.1.
//   3.3  Cross-thread send queue: queued frames drain without loss (backpressure
//        via small SO_SNDBUF + a non-reading peer); SO_KEEPALIVE + TCP keepalive
//        knobs applied on accepted AND initiated sockets; DNS resolved once at
//        start() (never on the IO thread); peer-reset errors surfaced through
//        set_on_error → EngineConfig::on_error.
//   3.7  ::send to a reset peer must not raise SIGPIPE (MSG_NOSIGNAL); on
//        Linux the mutation (send_flags() → 0) kills this very process, so
//        "the test binary survives" IS the assertion.
//
// The LockOrder test lives in this TU because test_transport.cpp is the
// registration slot for this task set (CMakeLists pre-registered it).
// =============================================================================
#include "fix/session/session_manager.hpp"
#include "fix/transport/tcp_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace fix;
using namespace std::chrono_literals;

namespace {

#ifndef _WIN32
// Ask the OS for a free TCP port (bind to 0, read it back, release it).
std::uint16_t find_free_port() {
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
}

// Poll until pred() is true or the deadline passes.
template <typename Pred>
bool wait_until(Pred &&pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

TcpTransportConfig make_transport_cfg(std::uint16_t port, bool initiator) {
    TcpTransportConfig tc;
    tc.host = "127.0.0.1";
    tc.port = port;
    tc.initiator = initiator;
    tc.reconnect_delay = 50ms; // fast retries keep the test snappy
    tc.reconnect_delay_max = 200ms;
    return tc;
}

// Connect to 127.0.0.1:port, retrying until the acceptor's listener is up
// (start() binds asynchronously) or `timeout` elapses. rcvbuf > 0 pins the
// peer's receive buffer *before* the handshake so the window is negotiated
// small (backpressure for the queue-drain test).
int raw_connect(std::uint16_t port, std::chrono::milliseconds timeout, int rcvbuf = 0) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            if (rcvbuf > 0)
                (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
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
        std::this_thread::sleep_for(10ms);
    }
}

void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Force an RST instead of a graceful FIN on close (SO_LINGER{1,0}).
void close_with_rst(int fd) {
    linger lg{1, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    ::close(fd);
}

// RAII release latch for parking a transport IO thread inside an on_data
// callback. Declared AFTER the transport it guards: destroyed FIRST, so
// release_ is raised before ~TcpTransport tries to join the IO thread (an
// ASSERT early-return can therefore never leave a parked thread behind).
struct ParkGuard {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    ~ParkGuard() { release.store(true, std::memory_order_release); }
};
#endif // !_WIN32

} // namespace

// ---------------------------------------------------------------------------
// 3.1 — Lock order: for_each snapshots under the lock and iterates lock-free.
//
// Pre-3.1, SessionManager::for_each held shared_lock across the callback; a
// callback that called remove()/create_session() then took unique_lock on the
// SAME non-recursive shared_mutex from the owning thread — deadlock/UB. This
// test *is* the regression: under the mutation (revert for_each to hold the
// lock) it hangs, which the mutation run observes as a timeout failure.
// ---------------------------------------------------------------------------
TEST(LockOrder, ForEachAllowsManagerMutationFromCallback) {
    SessionManager mgr;

    auto make_cfg = [](std::string sender, std::string target) {
        SessionConfig cfg;
        cfg.id.version = FixVersion::FIX_4_2;
        cfg.id.senderCompID = std::move(sender);
        cfg.id.targetCompID = std::move(target);
        return cfg;
    };

    auto s1 = mgr.create_session(make_cfg("SEND1", "TARG1"));
    auto s2 = mgr.create_session(make_cfg("SEND2", "TARG2"));
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    ASSERT_EQ(mgr.count(), 2u);

    // Remove from inside the pass — deadlocked while for_each held shared_lock.
    int visited = 0;
    bool removed = false;
    mgr.for_each([&](Session &s) {
        ++visited;
        if (s.id().senderCompID == "SEND2")
            removed = mgr.remove(s.id());
    });
    EXPECT_EQ(visited, 2) << "snapshot must visit every session registered at pass start";
    EXPECT_TRUE(removed) << "remove() from inside for_each must not deadlock";
    EXPECT_EQ(mgr.count(), 1u);

    // Create from inside the pass — same deadlock class; the new session must
    // NOT appear in the current (already snapshotted) pass.
    bool created = false;
    int visited2 = 0;
    mgr.for_each([&](Session &) {
        ++visited2;
        if (!created)
            created = static_cast<bool>(mgr.create_session(make_cfg("SEND3", "TARG3")));
    });
    EXPECT_TRUE(created) << "create_session() from inside for_each must not deadlock";
    EXPECT_EQ(visited2, 1) << "a session added mid-pass belongs to the NEXT pass";
    EXPECT_EQ(mgr.count(), 2u);
}

#ifndef _WIN32

// ---------------------------------------------------------------------------
// 3.3.1 — Cross-thread send queue: a non-IO thread floods an acceptor whose
// peer deliberately does NOT read. With SO_SNDBUF ≈ 128 KB (2×65536) and the
// peer's SO_RCVBUF pinned to 8 KB, only ~144 KB can sit in kernel space — the
// remaining ~7.8 MB of the 8 MB payload MUST travel through send_queue_ and
// be flushed by the IO thread (EPOLLOUT edges + the unconditional tick flush +
// the cross-thread wake in send()). Loss, duplication, reordering or a stall
// past the deadline all fail here.
// ---------------------------------------------------------------------------
TEST(TcpTransportQueue, CrossThreadQueuedSendsDrainWithoutLoss) {
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    auto tp = std::make_unique<TcpTransport>(make_transport_cfg(port, /*initiator=*/false));
    std::atomic<bool> connected{false};
    tp->set_on_connected([&] { connected.store(true, std::memory_order_release); });
    ASSERT_TRUE(tp->start().has_value());

    // Peer: tiny receive buffer, does not read during the fill phase.
    const int cfd = raw_connect(port, 5000ms, /*rcvbuf=*/8192);
    ASSERT_GE(cfd, 0) << "raw client never reached the acceptor";
    ASSERT_TRUE(wait_until([&] { return connected.load(std::memory_order_acquire); }, 5000ms))
        << "acceptor never adopted the connection";
    set_nonblocking(cfd);

    constexpr std::size_t kChunk = 64 * 1024;
    constexpr int kChunks = 128; // 8 MB total, ≫ the ~144 KB kernel capacity
    const std::size_t total = kChunk * kChunks;

    // Fill from a NON-IO thread: every send() must be accepted (queued), the
    // transport is connected and enqueueing is its contract.
    std::atomic<bool> send_ok{true};
    std::thread sender([&] {
        std::string buf(kChunk, '\0');
        for (int i = 0; i < kChunks && send_ok.load(std::memory_order_relaxed); ++i) {
            const std::uint64_t base = static_cast<std::uint64_t>(i) * kChunk;
            for (std::size_t j = 0; j < kChunk; ++j)
                buf[j] = static_cast<char>((base + j) % 251);
            if (!tp->send(buf.data(), buf.size()))
                send_ok.store(false, std::memory_order_relaxed);
        }
    });
    sender.join();
    EXPECT_TRUE(send_ok.load()) << "connected transport must accept queued cross-thread sends";

    // Drain: every byte must arrive, in order, byte-exact.
    std::size_t got = 0;
    bool pattern_ok = true;
    std::vector<char> buf(kChunk);
    const auto deadline = std::chrono::steady_clock::now() + 15000ms;
    while (got < total && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::recv(cfd, buf.data(), buf.size(), 0);
        if (n > 0) {
            for (ssize_t k = 0; k < n; ++k)
                if (buf[static_cast<std::size_t>(k)] !=
                    static_cast<char>((got + static_cast<std::size_t>(k)) % 251))
                    pattern_ok = false;
            got += static_cast<std::size_t>(n);
        } else if (n == 0) {
            break; // transport closed on us — that also fails the count check
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
    }
    EXPECT_EQ(got, total) << "lost or stalled frames: received " << got << " of " << total;
    EXPECT_TRUE(pattern_ok) << "byte stream corrupted (loss/dup/reorder)";

    ::close(cfd);
    tp->stop();
}

// ---------------------------------------------------------------------------
// 3.3.2 — Keepalive on the ACCEPTED socket: after connect, SO_KEEPALIVE and
// the TCP_KEEPIDLE/KEEPINTVL/KEEPCNT knobs must read back with the configured
// values (via the transport's diagnostic socket_options() snapshot).
// ---------------------------------------------------------------------------
TEST(TcpTransportKeepalive, AppliedToAcceptedSocket) {
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    auto tp = std::make_unique<TcpTransport>(make_transport_cfg(port, /*initiator=*/false));
    std::atomic<bool> connected{false};
    tp->set_on_connected([&] { connected.store(true, std::memory_order_release); });
    ASSERT_TRUE(tp->start().has_value());

    const int cfd = raw_connect(port, 5000ms);
    ASSERT_GE(cfd, 0);
    ASSERT_TRUE(wait_until([&] { return connected.load(std::memory_order_acquire); }, 5000ms));

    auto opts = tp->socket_options();
    ASSERT_TRUE(opts.has_value()) << "live connection must expose its socket options";
    EXPECT_TRUE(opts->keepalive) << "SO_KEEPALIVE not applied to the accepted socket";
    EXPECT_TRUE(opts->nodelay) << "TCP_NODELAY not applied to the accepted socket";
#if defined(TCP_KEEPIDLE) || defined(TCP_KEEPALIVE)
    EXPECT_EQ(opts->keepidle_sec, TcpTransport::kKeepIdleSec);
    EXPECT_EQ(opts->keepintvl_sec, TcpTransport::kKeepIntvlSec);
    EXPECT_EQ(opts->keepcnt, TcpTransport::kKeepCnt);
#endif
    // send_buffer_size (65536) applied as SO_SNDBUF — the kernel reports the
    // value it enforces (explicit sets are doubled by Linux).
    EXPECT_GE(opts->send_buffer_bytes, 65536)
        << "TcpTransportConfig::send_buffer_size not applied as SO_SNDBUF";

    ::close(cfd);
    tp->stop();
}

// ---------------------------------------------------------------------------
// 3.3.2 — Keepalive on the INITIATED socket (the initiator side of the same
// connection must be tuned identically).
// ---------------------------------------------------------------------------
TEST(TcpTransportKeepalive, AppliedToInitiatedSocket) {
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    // Raw listener: the kernel completes the handshake from the listen
    // backlog, so no accept() is needed for the transport to connect.
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(lfd, 0);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    ASSERT_EQ(::bind(lfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(lfd, 8), 0);

    auto tp = std::make_unique<TcpTransport>(make_transport_cfg(port, /*initiator=*/true));
    std::atomic<bool> connected{false};
    tp->set_on_connected([&] { connected.store(true, std::memory_order_release); });
    ASSERT_TRUE(tp->start().has_value());
    ASSERT_TRUE(wait_until([&] { return connected.load(std::memory_order_acquire); }, 5000ms))
        << "initiator never connected to the raw listener";

    auto opts = tp->socket_options();
    ASSERT_TRUE(opts.has_value());
    EXPECT_TRUE(opts->keepalive) << "SO_KEEPALIVE not applied to the initiated socket";
    EXPECT_TRUE(opts->nodelay) << "TCP_NODELAY not applied to the initiated socket";
#if defined(TCP_KEEPIDLE) || defined(TCP_KEEPALIVE)
    EXPECT_EQ(opts->keepidle_sec, TcpTransport::kKeepIdleSec);
    EXPECT_EQ(opts->keepintvl_sec, TcpTransport::kKeepIntvlSec);
    EXPECT_EQ(opts->keepcnt, TcpTransport::kKeepCnt);
#endif

    tp->stop();
    ::close(lfd);
}

// ---------------------------------------------------------------------------
// 3.7 — SIGPIPE: ::send() to a peer that has RST the connection must not kill
// the process (MSG_NOSIGNAL on every send site).
//
// The IO thread is deliberately parked inside an on_data callback so it cannot
// tear the connection down first — that guarantees the test thread's send()
// hits the reset socket with a live fd (deterministic, not a race with the
// event loop). Mutation: send_flags() → 0 makes the ::send below raise SIGPIPE
// (default action = terminate), so ctest reports a CRASHING test binary — the
// survival of this EXPECT is the assertion.
// ---------------------------------------------------------------------------
TEST(TcpTransportSigpipe, SendToResetPeerDoesNotRaiseSigpipe) {
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    auto tp = std::make_unique<TcpTransport>(make_transport_cfg(port, /*initiator=*/false));
    // After the transport: destroyed first → release raised before the join.
    ParkGuard park;
    std::atomic<bool> connected{false};
    tp->set_on_connected([&] { connected.store(true, std::memory_order_release); });
    tp->set_on_data([&](const char *, std::size_t) {
        park.entered.store(true, std::memory_order_release);
        while (!park.release.load(std::memory_order_acquire))
            std::this_thread::sleep_for(2ms);
    });
    ASSERT_TRUE(tp->start().has_value());

    const int cfd = raw_connect(port, 5000ms);
    ASSERT_GE(cfd, 0);
    ASSERT_TRUE(wait_until([&] { return connected.load(std::memory_order_acquire); }, 5000ms));

    // Park the IO thread inside on_data: with one inbound byte it blocks there
    // and cannot observe the reset below.
    const char poke = 'x';
    ASSERT_EQ(::send(cfd, &poke, 1, 0), 1);
    ASSERT_TRUE(wait_until([&] { return park.entered.load(std::memory_order_acquire); }, 5000ms))
        << "on_data never fired — cannot park the IO thread";

    close_with_rst(cfd);
    std::this_thread::sleep_for(150ms); // let the RST land in our socket state

    // The connection is now live-but-reset for us: every send() below performs
    // ::send() on it (queue is empty → direct write). It must fail with an
    // error — and must NOT raise SIGPIPE.
    const std::string payload(4096, 'S');
    for (int i = 0; i < 3; ++i) {
        auto r = tp->send(payload);
        EXPECT_FALSE(r.has_value()) << "send to a reset peer must surface an error (i=" << i << ")";
        std::this_thread::sleep_for(50ms);
    }
    // Still alive = SIGPIPE was suppressed.
    SUCCEED() << "process survived ::send to a reset peer";

    park.release.store(true, std::memory_order_release);
    tp->stop();
}

// ---------------------------------------------------------------------------
// 3.3.4 — Peer reset must surface on set_on_error (→ EngineConfig::on_error).
// The epoll HUP/ERR branch used to tear the connection down SILENTLY: a reset
// reports EPOLLERR|EPOLLHUP together with EPOLLIN and that check runs FIRST,
// so handle_recv's ECONNRESET reporting never executed. Mutation: removing the
// SO_ERROR reporting from the HUP/ERR branch fails this test.
// ---------------------------------------------------------------------------
TEST(TcpTransportErrors, PeerResetSurfacesOnError) {
    const std::uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    auto tp = std::make_unique<TcpTransport>(make_transport_cfg(port, /*initiator=*/false));
    std::atomic<bool> connected{false};
    std::atomic<int> error_count{0};
    std::atomic<int> last_error{0};
    std::atomic<bool> disconnected{false};
    tp->set_on_connected([&] { connected.store(true, std::memory_order_release); });
    tp->set_on_error([&](std::error_code ec) {
        last_error.store(ec.value(), std::memory_order_release);
        error_count.fetch_add(1, std::memory_order_acq_rel);
    });
    tp->set_on_disconnected(
        [&](std::string_view) { disconnected.store(true, std::memory_order_release); });
    ASSERT_TRUE(tp->start().has_value());

    const int cfd = raw_connect(port, 5000ms);
    ASSERT_GE(cfd, 0);
    ASSERT_TRUE(wait_until([&] { return connected.load(std::memory_order_acquire); }, 5000ms));

    close_with_rst(cfd); // RST, not FIN

    ASSERT_TRUE(wait_until([&] { return disconnected.load(std::memory_order_acquire); }, 5000ms))
        << "connection was never torn down after the reset";
    EXPECT_GE(error_count.load(std::memory_order_acquire), 1)
        << "peer reset tore the connection down WITHOUT firing set_on_error — "
           "EngineConfig::on_error can never see transport failures";
#ifdef __linux__
    EXPECT_EQ(last_error.load(std::memory_order_acquire), ECONNRESET)
        << "the surfaced error must carry the real socket error (SO_ERROR)";
#endif

    tp->stop();
}

// ---------------------------------------------------------------------------
// 3.3.3 — DNS: resolution happens once, on the caller's thread, inside
// start(). A host name that cannot resolve fails start() loudly and fast
// instead of spawning an IO thread that blocks in getaddrinfo() on every
// reconnect attempt. Mutation: moving getaddrinfo() back into do_connect()
// makes start() succeed → this test fails.
// ---------------------------------------------------------------------------
TEST(TcpTransportDns, UnresolvableHostFailsStartOnCallerThread) {
    TcpTransportConfig tc;
    tc.host = "invalid fix host name"; // syntactically invalid: EAI_NONAME with
                                       // no network round-trip → deterministic
    tc.port = 12345;
    tc.initiator = true;

    auto tp = std::make_unique<TcpTransport>(tc);
    std::atomic<int> error_count{0};
    tp->set_on_error([&](std::error_code) { error_count.fetch_add(1, std::memory_order_acq_rel); });

    const auto t0 = std::chrono::steady_clock::now();
    auto r = tp->start();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_FALSE(r.has_value()) << "unresolvable host must fail start(), not the IO loop";
    EXPECT_GE(error_count.load(std::memory_order_acquire), 1)
        << "resolution failure must surface on set_on_error";
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5000)
        << "resolution of a syntactically invalid name must not block on the network";

    tp->stop(); // no thread was spawned; must be a safe no-op
}

#endif // !_WIN32
