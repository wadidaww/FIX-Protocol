// =============================================================================
// FIX Protocol Engine - TSAN stress tests (Phase 3, task 3.6)
//
// Every test in this file drives the SAME machinery from several threads at
// once so ThreadSanitizer can observe the ordering of the shared-state
// protocols: session state atomics, Session::send_mutex_, store sequence
// claims, the parser guard, the audit queue/rotation locks and the engine
// registry locks.
//
// Invariant style (deliberate):
//  * EXACT equalities where this test controls every producer (the count is
//    derived in a comment at each assertion).
//  * Bounded lower/upper bounds where a race partner may legitimately win or
//    lose (raw client vs. initiator connection races) - outcomes are counted
//    and reported, never asserted as a single fixed winner: that would be a
//    flaky test, not a stronger test.
//  * Every wait has a deadline and every loop has an exit flag: a wedged test
//    is a product bug (ctest timeout), never a retry candidate.
//
// Lifetime rules used throughout (docs/proposed-plan.md 5):
//  * Callback state (counters, Session* slots) is declared BEFORE the Engine
//    so it is destroyed AFTER it; slots are written once before
//    engine.start() and then read from callback threads (thread-creation
//    ordering makes plain reads safe).
//  * std::jthreads are declared AFTER the sessions they touch and are joined
//    explicitly before the test body ends.
// =============================================================================
#include "fix/core/constants.hpp"
#include "fix/core/message.hpp"
#include "fix/core/types.hpp"
#include "fix/engine.hpp"
#include "fix/log/message_log.hpp"
#include "fix/parser/parser.hpp"
#include "fix/parser/serializer.hpp"
#include "fix/session/session.hpp"
#include "fix/store/memory_store.hpp"
#include "fix/transport/transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <process.h>
#endif

#include <gtest/gtest.h>

using namespace fix;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// Ports / config (same patterns as tests/test_e2e.cpp)
// ---------------------------------------------------------------------------

// Reserve `n` DISTINCT ephemeral ports by binding them to probe sockets and
// HOLDING those sockets open until release() is called immediately before
// Engine::start(). The usual bind(0)/close-then-rebind pattern opens a
// milliseconds-wide TOCTOU window: `ctest -j16` runs this binary
// concurrently with its per-test entries, so another process's bind(0) can
// claim a released candidate before the engine binds it and start() fails
// (observed once under parallel ASan). Holding the reservations shrinks the
// open window to the microseconds between release() and the engine's bind.
// RAII: a failed ASSERT between reserve() and release() still closes the
// probe sockets in the destructor.
class ReservedPorts {
public:
    std::vector<std::uint16_t> reserve(std::size_t n) {
#ifdef _WIN32
        (void)n;
        return {}; // TCP tests skip on Windows before ever reserving
#else
        std::set<std::uint16_t> seen;
        std::vector<std::uint16_t> out;
        for (int guard = 0; guard < 64 && out.size() < n; ++guard) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                continue;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                ::close(fd);
                continue;
            }
            socklen_t len = sizeof(addr);
            if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
                ::close(fd);
                continue;
            }
            const std::uint16_t p = ntohs(addr.sin_port);
            if (seen.insert(p).second) {
                out.push_back(p);
                fds_.push_back(fd); // keep the port claimed
            } else {
                ::close(fd);
            }
        }
        return out;
#endif
    }

    // Give the ports to the engine. Call immediately before Engine::start().
    void release() {
#ifndef _WIN32
        for (const int fd : fds_)
            if (fd >= 0)
                ::close(fd);
#endif
        fds_.clear();
    }

    ~ReservedPorts() { release(); }
    ReservedPorts() = default;
    ReservedPorts(const ReservedPorts &) = delete;
    ReservedPorts &operator=(const ReservedPorts &) = delete;

private:
    std::vector<int> fds_;
};

EngineConfig make_engine_config() {
    EngineConfig cfg;
    cfg.enable_audit = false;
    cfg.use_file_store = false;
    cfg.timer_interval_ms = 50;
    return cfg;
}

SessionConfig make_session_cfg(std::string sender, std::string target, bool initiator,
                               int heartbeat_sec = 5) {
    SessionConfig cfg;
    cfg.id.version = FixVersion::FIX_4_2;
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

// Poll until pred() is true or the deadline passes. Never blocks forever:
// a timeout becomes a failed EXPECT in the caller, not a hang.
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
// Wire helpers (mirrors tests/test_session.cpp / test_audit.cpp)
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

// Minimal application message the dispatcher accepts with validate_fields off
// (the default): 35=D with a unique ClOrdID.
Message make_nos(std::string_view clord) {
    Message m(msg_types::NewOrderSingle);
    (void)m.set(tags::ClOrdID, std::string(clord));
    (void)m.set(tags::Symbol, "AAPL");
    (void)m.set(tags::Side, "1");
    (void)m.set(tags::OrdType, "2");
    return m;
}

void feed_wire(Session &sess, const std::string &wire) {
    sess.on_data(wire.data(), wire.size());
}

// Peer Logon addressed to a session whose identity is (self <- peer).
void feed_logon(Session &sess, SeqNum seq, std::string_view peer, std::string_view self) {
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Logon, seq, peer, self,
                              {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}}));
}

// Acceptor handshake: logon() -> WaitingLogon, peer Logon(seq1) -> Active.
// Afterwards: next_target == 2, next_sender == 3.
void handshake_acceptor(Session &sess, std::string_view peer, std::string_view self) {
    sess.logon();
    feed_logon(sess, 1, peer, self);
}

bool parse_frame(const std::string &wire, Message &out) {
    StreamParser p;
    p.feed(wire.data(), wire.size());
    return p.next(out);
}

// "35=<mt><SOH>" - a substring that can only match a MsgType field.
std::string msg_frame(std::string_view msg_type) {
    std::string s = "35=";
    s += msg_type;
    s += SOH;
    return s;
}

// ---------------------------------------------------------------------------
// Audit helpers (mirrors tests/test_audit.cpp)
// ---------------------------------------------------------------------------
std::string test_pid() {
#ifdef _WIN32
    return std::to_string(static_cast<unsigned long>(_getpid()));
#else
    return std::to_string(::getpid());
#endif
}

std::filesystem::path make_temp_dir(const char *tag) {
    auto *ti = testing::UnitTest::GetInstance()->current_test_info();
    auto dir = std::filesystem::temp_directory_path() /
               (std::string("fix_stress_") + tag + "_" + ti->name() + "_" + test_pid());
    std::filesystem::remove_all(dir);
    return dir;
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

std::string read_file(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// FileAuditLog plus an atomic counter of entries ACCEPTED (queued or counted
// as dropped - log() returns normally either way). The counter increments
// AFTER the inner call returns, so at quiescence
//     lines_on_disk + inner.dropped() == total()
// must hold exactly: every accepted entry is either written (one line) or
// was dropped at enqueue (counted, never written).
class CountingAuditLog : public IAuditLog {
public:
    explicit CountingAuditLog(FileAuditLog::Config cfg)
        : inner_(std::move(cfg)) {}

    void log(AuditEntry entry) override {
        inner_.log(std::move(entry));
        total_.fetch_add(1, std::memory_order_acq_rel);
    }
    void flush() override { inner_.flush(); }
    void rotate() override { inner_.rotate(); }

    [[nodiscard]] std::uint64_t total() const noexcept {
        return total_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return inner_.dropped(); }

private:
    FileAuditLog inner_; // destroyed LAST: its writer thread may still run
    std::atomic<std::uint64_t> total_{0};
};

#ifndef _WIN32
// --- Raw-socket client helpers (same patterns as tests/test_e2e.cpp) -------
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

// Read from fd until stop(buf) holds, the peer closes, a hard error hits, or
// `timeout` elapses. Short SO_RCVTIMEO slices keep the deadline honest.
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
            continue;
        if (errno == EINTR)
            continue;
        break; // hard error
    }
    return buf;
}

// Well-formed FIX 4.2 Logon with caller-chosen identity and sequence.
// HeartBtInt=30 matches make_session_cfg(..., 30): the acceptor's adopted
// interval can then never trip the initiator's HeartBtInt echo check (2.8).
std::string make_raw_logon(std::string_view sender, std::string_view target, SeqNum seq) {
    MessageBuilder b;
    b.begin("FIX.4.2", msg_types::Logon);
    b.add(tags::SenderCompID, sender);
    b.add(tags::TargetCompID, target);
    b.add(tags::MsgSeqNum, static_cast<std::int64_t>(seq));
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    b.add(tags::EncryptMethod, std::int64_t(0));
    b.add(tags::HeartBtInt, std::int64_t(30));
    return b.finish();
}
#endif

} // namespace

// ===========================================================================
// 1. Concurrent application traffic on a stable pair while a second pair and
//    the session registry churn underneath it.
//
// What races: 4 loader threads sending on pair 1, pair-1's acceptor echoing
// from its transport IO thread, a churn-sender hammering pair 2 through
// outages, and a control thread remove/re-adding sessions on the engine
// registry (pair-2 acceptor + a throwaway acceptor) - all while the engine
// timer thread ticks every session every 50 ms.
//
// Invariants (derived at each assertion):
//  * bookkeeping: attempts == ok + err per sender;
//  * no phantoms: pair-1 acceptor recv <= loaders' ok (it can only receive
//    what a loader successfully handed to the transport); after quiescence
//    recv == loaders' ok (loopback TCP never drops and pair 1 is never
//    churned) and echoes == accepted echoes (each accepted echo is
//    delivered);
//  * control: every cycle removed the acceptor (get_session -> nullptr), saw
//    the initiator leave Active, and a send against the non-Active initiator
//    FAILED with SessionError - a deterministic outage gate, not a timing
//    guess;
//  * the churn sender must observe >= 1 outage error (remove windows span
//    hundreds of ms at a 1 ms send period).
// ===========================================================================
TEST(StressEngine, ConcurrentSendTrafficAndSessionChurn) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#else
    ReservedPorts reserved_ports;
    const auto ports = reserved_ports.reserve(3);
    ASSERT_EQ(ports.size(), 3u) << "could not find 3 distinct free ports";
    for (auto p : ports)
        ASSERT_NE(p, 0);
    const std::uint16_t p_load = ports[0];
    const std::uint16_t p_pair2 = ports[1];
    const std::uint16_t p_thrw = ports[2];

    // -- Callback state declared BEFORE the engine (destroyed after it) ------
    struct SenderStats {
        std::uint64_t attempts = 0;
        std::uint64_t ok = 0;
        std::uint64_t err = 0;
    };
    std::array<SenderStats, 4> loaders{};      // slot i written by loader i ONLY
    std::atomic<std::uint64_t> recv_at_acc{0}; // pair-1 acceptor app messages
    std::atomic<std::uint64_t> echoes{0};      // pair-1 initiator echo messages
    std::atomic<std::uint64_t> echo_try{0};
    std::atomic<std::uint64_t> echo_ok{0};
    std::atomic<std::uint64_t> echo_err{0};
    std::atomic<std::uint64_t> echo_seq{0}; // unique echo ClOrdIDs
    std::atomic<std::uint64_t> init1_logons{0};
    std::atomic<std::uint64_t> acc1_logons{0};
    std::atomic<std::uint64_t> init2_logons{0};
    std::atomic<std::uint64_t> acc2_logons{0};
    std::atomic<std::uint64_t> control_cycles{0};
    std::atomic<std::uint64_t> gate_errors{0};

    // The acceptor needs its own Session* inside on_message: written once
    // before engine.start(), read only from callbacks afterwards (creation
    // ordering makes that race-free without an atomic).
    Session *acc1_slot = nullptr;

    Engine engine(make_engine_config());

    // -- Pair 1: the stable traffic pair -------------------------------------
    const SessionConfig acc1_cfg = make_session_cfg("SERVER1", "CLIENT1", false, 30);
    SessionCallbacks acc1_cbs;
    acc1_cbs.on_logon = [&](const SessionID &) {
        ++acc1_logons;
    };
    acc1_cbs.on_message = [&](const SessionID &, const Message &) {
        ++recv_at_acc;
        // Echo best-effort: try/ok/err lets the drain equality distinguish
        // "echo lost at the transport" from "echo never attempted".
        ++echo_try;
        auto r = acc1_slot->send(make_nos("R" + std::to_string(echo_seq.fetch_add(1))));
        if (r)
            ++echo_ok;
        else
            ++echo_err;
    };
    Session *acc1 = engine.add_session(
        acc1_cfg, make_tcp_transport(make_transport_cfg(p_load, false)), acc1_cbs);
    ASSERT_NE(acc1, nullptr);
    acc1_slot = acc1;

    SessionCallbacks init1_cbs;
    init1_cbs.on_logon = [&](const SessionID &) {
        ++init1_logons;
    };
    init1_cbs.on_message = [&](const SessionID &, const Message &) {
        ++echoes;
    };
    Session *init1 =
        engine.add_session(make_session_cfg("CLIENT1", "SERVER1", true, 30),
                           make_tcp_transport(make_transport_cfg(p_load, true)), init1_cbs);
    ASSERT_NE(init1, nullptr);

    // -- Pair 2: the churned pair --------------------------------------------
    const SessionConfig acc2_cfg = make_session_cfg("SERVER2", "CLIENT2", false, 30);
    const SessionID acc2_id = acc2_cfg.id;
    SessionCallbacks acc2_cbs;
    acc2_cbs.on_logon = [&](const SessionID &) {
        ++acc2_logons;
    };
    Session *acc2 = engine.add_session(
        acc2_cfg, make_tcp_transport(make_transport_cfg(p_pair2, false)), acc2_cbs);
    ASSERT_NE(acc2, nullptr);

    SessionCallbacks init2_cbs;
    init2_cbs.on_logon = [&](const SessionID &) {
        ++init2_logons;
    };
    Session *init2 =
        engine.add_session(make_session_cfg("CLIENT2", "SERVER2", true, 30),
                           make_tcp_transport(make_transport_cfg(p_pair2, true)), init2_cbs);
    ASSERT_NE(init2, nullptr);

    // -- Throwaway acceptor: pure registry churn (never receives traffic) -----
    const SessionConfig thr_cfg = make_session_cfg("THRW-SRV", "THRW-CLI", false, 30);
    Session *thr =
        engine.add_session(thr_cfg, make_tcp_transport(make_transport_cfg(p_thrw, false)));
    ASSERT_NE(thr, nullptr);

    reserved_ports.release(); // hand the ports to the engine's binds
    ASSERT_TRUE(engine.start().has_value());
    ASSERT_TRUE(wait_until(
        [&] {
            return init1_logons.load() >= 1 && acc1_logons.load() >= 1 &&
                   init2_logons.load() >= 1 && acc2_logons.load() >= 1 &&
                   init1->state() == SessionState::Active &&
                   acc1->state() == SessionState::Active &&
                   init2->state() == SessionState::Active && acc2->state() == SessionState::Active;
        },
        20000ms))
        << "initial handshakes did not converge (i1=" << init1_logons << " a1=" << acc1_logons
        << " i2=" << init2_logons << " a2=" << acc2_logons << ")";

    // -- Loaders: steady app traffic on pair 1 --------------------------------
    std::atomic<bool> stop_load{false};
    const auto load_start = std::chrono::steady_clock::now();
    std::vector<std::jthread> loader_threads;
    loader_threads.reserve(loaders.size());
    for (std::size_t t = 0; t < loaders.size(); ++t) {
        loader_threads.emplace_back([&, t] {
            auto &st = loaders[t];
            int i = 0;
            while (!stop_load.load(std::memory_order_acquire)) {
                auto r = init1->send(make_nos("L" + std::to_string(t) + "-" + std::to_string(i++)));
                ++st.attempts;
                if (r)
                    ++st.ok;
                else
                    ++st.err;
                std::this_thread::sleep_for(500us); // bound MemoryStore growth
            }
        });
    }

    // -- Churn sender: hammers pair-2's initiator through outages -------------
    SenderStats churn_stats; // written by this thread only, read after join
    std::jthread churn_sender([&] {
        int i = 0;
        while (!stop_load.load(std::memory_order_acquire)) {
            auto r = init2->send(make_nos("C-" + std::to_string(i++)));
            ++churn_stats.attempts;
            if (r)
                ++churn_stats.ok;
            else
                ++churn_stats.err;
            std::this_thread::sleep_for(1ms);
        }
    });

    // -- Control: remove/re-add pair-2's acceptor + registry churn ------------
    constexpr int kControlCycles = 4;
    std::jthread control([&] {
        for (int k = 0; k < kControlCycles; ++k) {
            EXPECT_TRUE(engine.remove_session(acc2_id)) << "cycle " << k << ": remove failed";
            EXPECT_TRUE(wait_until([&] { return engine.get_session(acc2_id) == nullptr; }, 2000ms))
                << "cycle " << k << ": removed acceptor still in the manager";
            // Deterministic outage gate: the initiator cannot be Active
            // without its acceptor, so this send MUST fail with SessionError.
            const bool left_active =
                wait_until([&] { return init2->state() != SessionState::Active; }, 5000ms);
            EXPECT_TRUE(left_active) << "cycle " << k << ": initiator never left Active";
            auto gate = init2->send(make_nos("GATE-" + std::to_string(k)));
            EXPECT_FALSE(gate.has_value()) << "cycle " << k << ": send on a dead peer must fail";
            if (!gate)
                ++gate_errors;
            // Registry churn on a session that never receives traffic.
            EXPECT_TRUE(engine.remove_session(thr_cfg.id)) << "cycle " << k;
            EXPECT_NE(
                engine.add_session(thr_cfg, make_tcp_transport(make_transport_cfg(p_thrw, false))),
                nullptr)
                << "cycle " << k;
            // Re-add pair-2's acceptor: the initiator auto-reconnects and
            // both sides must converge again before the next cycle.
            Session *re = engine.add_session(
                acc2_cfg, make_tcp_transport(make_transport_cfg(p_pair2, false)), acc2_cbs);
            EXPECT_NE(re, nullptr) << "cycle " << k;
            const bool converged = wait_until(
                [&] {
                    Session *a = engine.get_session(acc2_id);
                    return init2->state() == SessionState::Active && a != nullptr &&
                           a->state() == SessionState::Active;
                },
                20000ms);
            EXPECT_TRUE(converged) << "cycle " << k << ": pair-2 did not reconverge";
            ++control_cycles;
        }
    });

    control.join();
    // Keep the loaders running for at least 3 s so the churn window and the
    // traffic window genuinely overlap.
    while (std::chrono::steady_clock::now() - load_start < 3000ms)
        std::this_thread::sleep_for(20ms);
    stop_load.store(true, std::memory_order_release);
    for (auto &t : loader_threads)
        t.join();
    churn_sender.join();

    // -- Quiescence: every accepted pair-1 message and echo must have landed --
    std::uint64_t sum_attempts = 0, sum_ok = 0, sum_err = 0;
    for (const auto &s : loaders) {
        sum_attempts += s.attempts;
        sum_ok += s.ok;
        sum_err += s.err;
    }
    const bool drained = wait_until(
        [&] { return recv_at_acc.load() == sum_ok && echoes.load() == echo_ok.load(); }, 10000ms);

    EXPECT_TRUE(drained) << "pair-1 did not drain: recv=" << recv_at_acc << " ok=" << sum_ok
                         << " echoes=" << echoes << " echo_ok=" << echo_ok;
    EXPECT_EQ(recv_at_acc.load(), sum_ok) << "every successfully sent pair-1 message arrives";
    EXPECT_EQ(echoes.load(), echo_ok.load()) << "every accepted echo arrives";
    // NIT#12: the old EXPECT_LE(recv_at_acc, sum_ok) was a duplicate of the
    // EXPECT_EQ above (it can never fail once that passes); assert a bound
    // the equality does NOT imply instead: nothing may arrive that was never
    // even attempted.
    EXPECT_LE(recv_at_acc.load(), sum_attempts) << "no phantom messages";
    // Non-vacuity: the accounting equalities below are only meaningful when
    // the load actually issued traffic (0 == 0 + 0 would pass for free).
    EXPECT_GT(sum_attempts, 0u) << "pair-1 load never ran";
    EXPECT_EQ(sum_attempts, sum_ok + sum_err) << "every send attempt is counted exactly once";
    EXPECT_GT(echo_try.load(), 0u) << "echo load never ran";
    EXPECT_EQ(echo_try.load(), echo_ok.load() + echo_err.load());

    EXPECT_EQ(control_cycles.load(), static_cast<std::uint64_t>(kControlCycles));
    EXPECT_EQ(gate_errors.load(), static_cast<std::uint64_t>(kControlCycles))
        << "every post-remove send must fail with SessionError";
    EXPECT_GE(churn_stats.err, 1u) << "churn sender never observed an outage";
    EXPECT_EQ(churn_stats.attempts, churn_stats.ok + churn_stats.err);
    // Initial handshake + one per re-add convergence (>=; extra logons would
    // mean a connection cycle we did not control - reported, not fatal).
    EXPECT_GE(acc2_logons.load(), 1u + kControlCycles);
    EXPECT_GE(init2_logons.load(), 1u + kControlCycles);
    EXPECT_NE(engine.get_session(acc2_id), nullptr);
    EXPECT_NE(engine.get_session(thr_cfg.id), nullptr);
    EXPECT_GE(acc1_logons.load(), 1u);
    EXPECT_GE(init1_logons.load(), 1u);

    // Diagnostics for reports / failure triage (visible in XML output).
    RecordProperty("loader_ok", std::to_string(sum_ok));
    RecordProperty("loader_err", std::to_string(sum_err));
    RecordProperty("churn_err", std::to_string(churn_stats.err));
    RecordProperty("control_cycles", std::to_string(control_cycles.load()));

    engine.stop();
    EXPECT_FALSE(engine.is_running());
#endif
}

// ===========================================================================
// 2. Handshake churn: a raw FIX client repeatedly handshakes against the
//    acceptor WHILE a control thread removes/re-adds that same acceptor and
//    its initiator auto-reconnects.
//
// What races: raw connect/Logon processing on the acceptor's transport IO
// thread vs. Engine::remove_session stopping and joining that thread; raw
// client vs. initiator racing to be the adopted connection after each
// re-add; engine registry add/remove concurrent with the handshake loop.
//
// Outcome honesty: whether the raw client wins a post-re-add window is a
// genuine race (single-connection acceptor serves one peer at a time). So:
//  * the WARM-UP handshake (before the initiator exists) is deterministic
//    and hard-asserted - it proves the raw path works;
//  * concurrent cycle outcomes are COUNTED and bounded (attempts, >= 1
//    success overall), never asserted per-cycle: the acceptor is only ever
//    adopted in WaitingLogon state (a raw Logon is either fully processed
//    or the connection is never adopted / refused) - no wedges, no flakes;
//  * control-side assertions ARE deterministic: remove succeeds, initiator
//    leaves Active, send fails with SessionError, reconvergence happens.
// ===========================================================================
TEST(StressEngine, RawHandshakeRacesAcceptorChurn) {
#ifdef _WIN32
    GTEST_SKIP() << "TCP transport is a documented no-op on Windows";
#else
    ReservedPorts reserved_port;
    const auto ports = reserved_port.reserve(1);
    ASSERT_EQ(ports.size(), 1u);
    ASSERT_NE(ports[0], 0);
    const std::uint16_t port = ports[0];

    // -- State declared BEFORE the engine ------------------------------------
    std::atomic<std::uint64_t> acc_logons{0};
    std::atomic<std::uint64_t> init_logons{0};
    std::atomic<std::uint64_t> raw_handshakes{0}; // warm-up + concurrent wins
    std::atomic<std::uint64_t> raw_attempts{0};   // concurrent cycles only
    std::atomic<bool> a_stop{false};

    Engine engine(make_engine_config());

    const SessionConfig acc_cfg = make_session_cfg("SRVS", "CLIS", false, 30);
    const SessionID acc_id = acc_cfg.id;
    SessionCallbacks acc_cbs;
    acc_cbs.on_logon = [&](const SessionID &) {
        ++acc_logons;
    };
    Session *acc =
        engine.add_session(acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acc_cbs);
    ASSERT_NE(acc, nullptr);

    reserved_port.release(); // hand the port to the engine's bind
    ASSERT_TRUE(engine.start().has_value());

    // -- Deterministic warm-up: raw handshake before any initiator exists -----
    const int warm_fd = raw_connect(port, 5000ms);
    ASSERT_GE(warm_fd, 0) << "raw client never reached the acceptor";
    ASSERT_TRUE(raw_send_all(warm_fd, make_raw_logon("CLIS", "SRVS", 1)));
    const std::string warm = raw_read_until(warm_fd, 5000ms, [](const std::string &s) {
        return s.find(msg_frame(msg_types::Logon)) != std::string::npos;
    });
    ASSERT_NE(warm.find(msg_frame(msg_types::Logon)), std::string::npos)
        << "warm-up handshake got no Logon reply: " << warm;
    // handle_logon sends its reply BEFORE the Active CAS (session.cpp:
    // send-then-transition), so the raw client can receive 35=A while the
    // acceptor is still WaitingLogon - the transition is the very next
    // statement on the IO thread. Wait for the publish while the connection
    // is still open: closing first would move the state to Disconnected and
    // hide exactly the transition being observed (seen under ASan+j16).
    ASSERT_TRUE(wait_until([&] { return acc->state() == SessionState::Active; }, 5000ms))
        << "acceptor never published Active after the warm-up reply (state="
        << static_cast<int>(acc->state()) << ")";
    ::close(warm_fd);
    ++raw_handshakes;

    // -- Add the initiator; both sides must converge --------------------------
    SessionCallbacks init_cbs;
    init_cbs.on_logon = [&](const SessionID &) {
        ++init_logons;
    };
    Session *init =
        engine.add_session(make_session_cfg("CLIS", "SRVS", true, 30),
                           make_tcp_transport(make_transport_cfg(port, true)), init_cbs);
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(wait_until(
        [&] {
            return acc_logons.load() >= 2 && init_logons.load() >= 1 &&
                   init->state() == SessionState::Active && acc->state() == SessionState::Active;
        },
        20000ms))
        << "initiator handshake did not converge (acc=" << acc_logons << " init=" << init_logons
        << ")";

    // -- Loop A: raw client cycles (concurrent with loop B's churn) -----------
    constexpr int kRawCycles = 4;
    std::jthread loop_a([&] {
        for (int i = 2; i <= 1 + kRawCycles && !a_stop.load(std::memory_order_acquire); ++i) {
            ++raw_attempts;
            const int fd = raw_connect(port, 1500ms);
            if (fd < 0)
                continue; // listener down (churn window) - bounded attempt
            bool sent = raw_send_all(fd, make_raw_logon("CLIS", "SRVS", static_cast<SeqNum>(i)));
            std::string resp;
            if (sent)
                resp = raw_read_until(fd, 500ms, [](const std::string &s) {
                    return s.find(msg_frame(msg_types::Logon)) != std::string::npos;
                });
            if (resp.find(msg_frame(msg_types::Logon)) != std::string::npos)
                ++raw_handshakes;
            ::close(fd);
            std::this_thread::sleep_for(50ms);
        }
    });

    // -- Loop B: remove/re-add the acceptor; initiator auto-reconnects --------
    constexpr int kChurnCycles = 3;
    std::atomic<std::uint64_t> b_cycles{0};
    std::atomic<std::uint64_t> b_gates{0};
    std::jthread loop_b([&] {
        for (int k = 0; k < kChurnCycles; ++k) {
            EXPECT_TRUE(engine.remove_session(acc_id)) << "cycle " << k;
            EXPECT_TRUE(wait_until([&] { return engine.get_session(acc_id) == nullptr; }, 2000ms))
                << "cycle " << k;
            const bool left =
                wait_until([&] { return init->state() != SessionState::Active; }, 5000ms);
            EXPECT_TRUE(left) << "cycle " << k << ": initiator never left Active";
            auto gate = init->send(make_nos("G2-" + std::to_string(k)));
            EXPECT_FALSE(gate.has_value()) << "cycle " << k;
            if (!gate)
                ++b_gates;
            Session *re = engine.add_session(
                acc_cfg, make_tcp_transport(make_transport_cfg(port, false)), acc_cbs);
            EXPECT_NE(re, nullptr) << "cycle " << k;
            const bool conv = wait_until(
                [&] {
                    Session *a = engine.get_session(acc_id);
                    return init->state() == SessionState::Active && a != nullptr &&
                           a->state() == SessionState::Active;
                },
                20000ms);
            EXPECT_TRUE(conv) << "cycle " << k << ": did not reconverge";
            ++b_cycles;
        }
    });

    loop_b.join();
    a_stop.store(true, std::memory_order_release);
    loop_a.join();

    EXPECT_EQ(b_cycles.load(), static_cast<std::uint64_t>(kChurnCycles));
    EXPECT_EQ(b_gates.load(), static_cast<std::uint64_t>(kChurnCycles));
    EXPECT_GE(raw_attempts.load(), 1u) << "loop A never ran a cycle";
    EXPECT_GE(raw_handshakes.load(), 1u) << "warm-up handshake must be counted";
    Session *fin = engine.get_session(acc_id);
    ASSERT_NE(fin, nullptr);
    // Loop A only stops AFTER loop_b joined, so its final raw attempt can
    // still disturb the connection that loop B's last convergence observed
    // (raw takes the acceptor slot, both peers reconnect). Wait for the
    // final steady state instead of sampling it immediately.
    EXPECT_TRUE(wait_until(
        [&] {
            return init->state() == SessionState::Active && fin->state() == SessionState::Active;
        },
        20000ms))
        << "no final reconvergence after churn ended (init=" << static_cast<int>(init->state())
        << " fin=" << static_cast<int>(fin->state()) << ")";

    // Diagnostics: concurrent raw outcomes are genuinely racy - record how
    // many cycles ran and how many won a handshake so a report can show the
    // distribution instead of a bare ">= 1".
    RecordProperty("raw_attempts", std::to_string(raw_attempts.load()));
    RecordProperty("raw_handshakes", std::to_string(raw_handshakes.load()));

    engine.stop();
    EXPECT_FALSE(engine.is_running());
#endif
}

// ===========================================================================
// 3. Concurrent senders must claim gap-free, unique outbound sequences.
//
// 4 threads x 500 sends against one Active standalone session. Session
// serialises frame production under send_mutex_ and claims the sequence via
// the store's atomic compound primitive (3.2). The wire frames captured in
// do_send must therefore be exactly the contiguous range
// [next_sender_after_handshake, +2000): no duplicates (two senders claiming
// the same number) and no gaps (a burned-but-unsent sequence).
// ===========================================================================
TEST(StressSession, ConcurrentSendClaimsContiguousUniqueSequences) {
    // Declared before the session: destroyed after it.
    std::mutex frames_mu;
    std::vector<std::string> frames; // every frame handed to do_send

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        std::lock_guard lk(frames_mu);
        frames.push_back(s);
    };

    constexpr std::string_view kPeer = "CLI";
    constexpr std::string_view kSelf = "SRV";
    Session sess(make_session_cfg(std::string(kSelf), std::string(kPeer), false, 30),
                 std::make_unique<MemoryStore>(), nullptr, cbs);

    handshake_acceptor(sess, kPeer, kSelf);
    ASSERT_EQ(sess.state(), SessionState::Active);
    const SeqNum start = sess.store()->next_sender_seq_num();
    ASSERT_EQ(start, 3u) << "handshake must leave next_sender at 3 (Logon x2)";

    struct Stats {
        std::uint64_t ok = 0;
        std::uint64_t err = 0;
    };
    std::array<Stats, 4> st{};
    constexpr int kPerThread = 500;
    {
        std::vector<std::jthread> threads;
        threads.reserve(st.size());
        for (std::size_t t = 0; t < st.size(); ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    auto r = sess.send(make_nos("T" + std::to_string(t) + "-" + std::to_string(i)));
                    if (r)
                        ++st[t].ok;
                    else
                        ++st[t].err;
                }
            });
        }
        for (auto &th : threads)
            th.join();
    }

    std::uint64_t ok = 0, err = 0;
    for (const auto &s : st) {
        ok += s.ok;
        err += s.err;
    }
    EXPECT_EQ(err, 0u) << "an Active standalone session must accept every send";
    EXPECT_EQ(ok, 4u * kPerThread);

    // Parse the captured frames: keep the application messages' MsgSeqNum.
    std::vector<SeqNum> seqs;
    {
        std::lock_guard lk(frames_mu);
        for (const auto &w : frames) {
            Message m;
            if (parse_frame(w, m) && m.msg_type() == msg_types::NewOrderSingle)
                seqs.push_back(m.seq_num());
        }
    }
    ASSERT_EQ(seqs.size(), 4u * kPerThread);
    std::sort(seqs.begin(), seqs.end());
    std::size_t bad = seqs.size();
    for (std::size_t i = 0; i < seqs.size(); ++i) {
        if (seqs[i] != start + i) {
            bad = i;
            break;
        }
    }
    EXPECT_EQ(bad, seqs.size()) << "sequences must be exactly [start, start+" << seqs.size()
                                << ") contiguous and unique; first mismatch at index " << bad;
    EXPECT_EQ(sess.store()->next_sender_seq_num(), start + 4u * kPerThread)
        << "store counter must land exactly past the last claim";
}

// ===========================================================================
// 4. Audit flush/rotate racing concurrent session traffic.
//
// Two sessions (one driving thread each) share ONE tiny FileAuditLog
// (max_size=4 KiB -> rotation every ~15 entries; max_queue=64 -> drops under
// floods are expected and COUNTED). Threads interleave sends, feeds, flush()
// and rotate() while the log's writer thread drains the queue and rotates on
// size - the exact cross-thread interleavings TSAN must clear.
//
// Invariants:
//  * exact accepted-entry count: 2 sessions x (3 handshake frames + 2 x K
//    traffic frames) = 6 + 4K - every send succeeds (Active, MemoryStore,
//    no timer) and every complete inbound frame is audited before dispatch;
//  * exact disk accounting after a final flush:
//        lines + dropped == total
//    (written line per accepted entry, or counted drop at enqueue);
//  * rotation happened (files >= 2) and both sessions' IN/OUT markers are
//    present across the rotated files.
// ===========================================================================
TEST(StressAudit, FlushRotateRaceUnderConcurrentSessionTraffic) {
    constexpr int kPer = 300;
    const auto dir = make_temp_dir("audit");

    FileAuditLog::Config fcfg;
    fcfg.dir = dir;
    fcfg.max_size = 4096; // force rotation during the flood
    fcfg.retain_days = 1; // retention enabled but keeps fresh files
    fcfg.max_queue = 64;  // tiny bounded queue: drops are legal, losses are not

    CountingAuditLog audit(fcfg); // declared BEFORE the sessions it is wired to

    std::atomic<std::uint64_t> app_rx1{0};
    std::atomic<std::uint64_t> app_rx2{0};

    // Session 1 (declared first -> destroyed last of the two; both before audit)
    SessionCallbacks cb1;
    cb1.do_send = [](const std::string &) {
    }; // frames not needed here
    cb1.on_message = [&](const SessionID &, const Message &) {
        ++app_rx1;
    };
    Session s1(make_session_cfg("AUD1", "PEER1", false, 30), std::make_unique<MemoryStore>(),
               nullptr, cb1);
    s1.set_audit_log(&audit);

    SessionCallbacks cb2;
    cb2.do_send = [](const std::string &) {
    };
    cb2.on_message = [&](const SessionID &, const Message &) {
        ++app_rx2;
    };
    Session s2(make_session_cfg("AUD2", "PEER2", false, 30), std::make_unique<MemoryStore>(),
               nullptr, cb2);
    s2.set_audit_log(&audit);

    struct DriveStats {
        std::uint64_t sends_ok = 0;
        std::uint64_t sends_err = 0;
    };
    DriveStats d1, d2; // each written by its own thread only

    auto drive = [&](Session &sess, std::string_view peer, std::string_view self, DriveStats *st) {
        EXPECT_TRUE(sess.logon().has_value());
        feed_logon(sess, 1, peer, self);
        EXPECT_EQ(sess.state(), SessionState::Active);
        SeqNum next_in = 2; // peer Logon consumed seq 1
        for (int i = 0; i < kPer; ++i) {
            auto r = sess.send(make_nos("A-" + std::to_string(i)));
            if (r)
                ++st->sends_ok;
            else
                ++st->sends_err;
            const std::string w =
                make_wire("FIX.4.2", msg_types::NewOrderSingle, next_in++, peer, self);
            sess.on_data(w.data(), w.size());
            if ((i % 64) == 63)
                audit.flush(); // writer-drain vs. concurrent log()
            if ((i % 40) == 39)
                audit.rotate(); // file reopen vs. writer rotation
        }
    };

    {
        std::jthread t1([&] { drive(s1, "PEER1", "AUD1", &d1); });
        std::jthread t2([&] { drive(s2, "PEER2", "AUD2", &d2); });
        t1.join();
        t2.join();
    }

    // Drain the writer, then account for every accepted entry.
    audit.flush();

    EXPECT_EQ(d1.sends_ok, static_cast<std::uint64_t>(kPer));
    EXPECT_EQ(d2.sends_ok, static_cast<std::uint64_t>(kPer));
    EXPECT_EQ(d1.sends_err + d2.sends_err, 0u);
    EXPECT_EQ(app_rx1.load(), static_cast<std::uint64_t>(kPer));
    EXPECT_EQ(app_rx2.load(), static_cast<std::uint64_t>(kPer));

    // Exact accepted count: per session 1 (TX logon) + 1 (RX logon) +
    // 1 (TX logon reply) + K x (1 TX send + 1 RX feed), two sessions.
    const std::uint64_t expected = 6u + 4u * static_cast<std::uint64_t>(kPer);
    EXPECT_EQ(audit.total(), expected);

    const std::uint64_t dropped = audit.dropped();
    const auto files = audit_files(dir);
    ASSERT_FALSE(files.empty());
    std::string all;
    std::size_t lines = 0;
    for (const auto &f : files) {
        const std::string content = read_file(f);
        lines += static_cast<std::size_t>(std::count(content.begin(), content.end(), '\n'));
        all += content;
    }
    EXPECT_EQ(lines + dropped, audit.total())
        << "every accepted entry is either one line on disk or a counted drop "
        << "(lines=" << lines << " dropped=" << dropped << " total=" << audit.total() << ")";
    EXPECT_GE(files.size(), 2u) << "size-triggered rotation must have produced >1 file";
    EXPECT_NE(all.find("IN  "), std::string::npos) << "received frames must be audited";
    EXPECT_NE(all.find("OUT "), std::string::npos) << "sent frames must be audited";
    EXPECT_NE(all.find("AUD1"), std::string::npos) << "session 1 identity missing from the trail";
    EXPECT_NE(all.find("AUD2"), std::string::npos) << "session 2 identity missing from the trail";
    RecordProperty("audit_total", std::to_string(audit.total()));
    RecordProperty("audit_dropped", std::to_string(dropped));
    RecordProperty("audit_lines", std::to_string(lines));
    RecordProperty("audit_files", std::to_string(files.size()));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec); // best-effort cleanup
}

// ===========================================================================
// 5. Timer vs. transport-IO vs. user-send vs. disconnect() on ONE session
//    with a fake (atomic) clock.
//
// Threads: a timer thread advancing the virtual clock and calling on_timer()
// (send-idle heartbeats, TestRequest probes, gap-fill retries race with
// everything else), an IO thread feeding in-sequence app messages plus
// periodic gap injections (future seq -> ResendRequest -> fills) and
// TestRequests (heartbeat responses), and a user thread calling send().
// Main calls disconnect() WHILE all three are still running, then stops and
// joins them.
//
// Invariants (all hold regardless of which side wins each race - including
// a legitimate heartbeat-timeout give-up before main's disconnect):
//  * end state is Disconnected and do_disconnect ran at least once;
//  * user bookkeeping: attempts == ok + err;
//  * the handshake completed (msgs_received >= 1) and traffic actually
//    flowed (feeds > 0);
//  * no crash, no hang, every thread exits on its stop token.
// ===========================================================================
TEST(StressSession, TimerIoAndDisconnectRaceEndCoherent) {
    // Declared before the session (destroyed after it): the session's clock
    // lambda and callbacks keep referencing these until ~Session runs.
    std::atomic<std::int64_t> fake{0};
    std::mutex wire_mu;
    std::vector<std::string> wire; // every frame handed to do_send
    std::atomic<std::uint64_t> disconnect_calls{0};
    std::atomic<std::uint64_t> feeds{0};

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        std::lock_guard lk(wire_mu);
        wire.push_back(s);
    };
    cbs.do_disconnect = [&] {
        disconnect_calls.fetch_add(1, std::memory_order_acq_rel);
    };

    constexpr std::string_view kPeer = "T5CLI";
    constexpr std::string_view kSelf = "T5SRV";
    Session sess(make_session_cfg(std::string(kSelf), std::string(kPeer), false, 5),
                 std::make_unique<MemoryStore>(), nullptr, cbs);
    sess.set_time_source_for_test([&fake] { return fake.load(); });

    // Handshake at fake t=0, then one deterministic successful send before
    // any thread exists (so ok >= 1 is a fact, not a scheduling bet).
    handshake_acceptor(sess, kPeer, kSelf);
    ASSERT_EQ(sess.state(), SessionState::Active);
    ASSERT_GE(sess.msgs_received(), 1u);
    ASSERT_TRUE(sess.send(make_nos("WARM")).has_value());

    // -- Timer thread: advance the virtual clock, drive on_timer -------------
    std::jthread timer_th([&](std::stop_token st) {
        std::uint64_t n = 0;
        while (!st.stop_requested()) {
            fake.fetch_add(1, std::memory_order_relaxed);
            sess.on_timer();
            if ((++n & 0x3FF) == 0)
                std::this_thread::yield();
        }
    });

    // -- IO thread: in-sequence traffic, gap injections, TestRequests --------
    std::jthread io_th([&](std::stop_token st) {
        SeqNum next_in = 2; // handshake consumed peer seq 1
        std::uint64_t i = 0;
        while (!st.stop_requested()) {
            if ((i % 337) == 336) {
                // Gap injection: a future seq is rejected (-> ResendRequest
                // from the session), then the missing frames are filled.
                std::string w =
                    make_wire("FIX.4.2", msg_types::NewOrderSingle, next_in + 2, kPeer, kSelf);
                sess.on_data(w.data(), w.size());
                w = make_wire("FIX.4.2", msg_types::NewOrderSingle, next_in++, kPeer, kSelf);
                sess.on_data(w.data(), w.size());
                w = make_wire("FIX.4.2", msg_types::NewOrderSingle, next_in++, kPeer, kSelf);
                sess.on_data(w.data(), w.size());
                w = make_wire("FIX.4.2", msg_types::NewOrderSingle, next_in++, kPeer, kSelf);
                sess.on_data(w.data(), w.size());
            } else if ((i % 777) == 776) {
                // Inbound probe: the session answers with a Heartbeat from
                // its IO thread, racing the user's send() and the timer.
                const std::string w = make_wire("FIX.4.2", msg_types::TestRequest, next_in++, kPeer,
                                                kSelf, {{tags::TestReqID, "P5"}});
                sess.on_data(w.data(), w.size());
            } else {
                const std::string w =
                    make_wire("FIX.4.2", msg_types::NewOrderSingle, next_in++, kPeer, kSelf);
                sess.on_data(w.data(), w.size());
            }
            feeds.fetch_add(1, std::memory_order_relaxed);
            ++i;
        }
    });

    // -- User thread: hammer send() with a periodic pause --------------------
    // The pause creates send-idle windows the timer's heartbeat path fills,
    // and keeps the captured wire vector bounded.
    struct UserStats {
        std::uint64_t attempts = 0;
        std::uint64_t ok = 0;
        std::uint64_t err = 0;
    } user;
    std::jthread user_th([&](std::stop_token st) {
        std::uint64_t i = 0;
        while (!st.stop_requested()) {
            auto r = sess.send(make_nos("U-" + std::to_string(i++)));
            ++user.attempts;
            if (r)
                ++user.ok;
            else
                ++user.err;
            if ((i % 500) == 0)
                std::this_thread::sleep_for(1ms);
            else
                std::this_thread::sleep_for(100us);
        }
    });

    std::this_thread::sleep_for(1200ms);

    // THE race: disconnect while timer, IO and user threads are mid-flight.
    sess.disconnect();

    timer_th.request_stop();
    io_th.request_stop();
    user_th.request_stop();
    timer_th.join();
    io_th.join();
    user_th.join();

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_GE(disconnect_calls.load(), 1u) << "disconnect() must run do_disconnect";
    // NIT#12: the old attempts == ok + err is a loop invariant (trivially
    // true, and vacuous if the thread never looped); assert bounds that say
    // something about the system instead.
    EXPECT_GT(user.ok, 0u) << "the user sender never succeeded before stop";
    EXPECT_GE(sess.msgs_sent(), user.ok) << "every successful send produced a frame";
    EXPECT_LE(user.attempts, 100000u) << "attempts must stay within the designed rate cap";
    EXPECT_GE(sess.msgs_received(), 1u) << "handshake Logon must have been processed";
    EXPECT_GT(feeds.load(), 0u);
    {
        std::lock_guard lk(wire_mu);
        EXPECT_GT(wire.size(), 0u) << "handshake produced at least one frame";
    }
    RecordProperty("user_ok", std::to_string(user.ok));
    RecordProperty("user_err", std::to_string(user.err));
    RecordProperty("feeds", std::to_string(feeds.load()));
    RecordProperty("disconnect_calls", std::to_string(disconnect_calls.load()));
    // A heartbeat-timeout give-up before main's disconnect() is a valid
    // coherent outcome (counted, not asserted): report it for diagnosis.
    if (disconnect_calls.load() > 1)
        SUCCEED() << "early heartbeat give-up observed (disconnect_calls="
                  << disconnect_calls.load() << ")";
}
