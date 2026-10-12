// =============================================================================
// FIX Protocol Engine - Microbenchmarks (Google Benchmark)
//
// Build:  cmake -B build -DCMAKE_BUILD_TYPE=Release -DFIX_BUILD_BENCHMARKS=ON
//         cmake --build build --target fix_bench
// Run:    ./build/fix_bench --benchmark_min_time=1s
//
// Covers the hot paths of the engine: wire parsing, wire serialisation,
// Message field access, dictionary validation and a full in-memory session
// round trip (initiator send -> acceptor parse/validate -> application
// callback). No sockets are involved, so results reflect CPU cost only.
// =============================================================================
#include "fix/core/constants.hpp"
#include "fix/core/message.hpp"
#include "fix/dictionary/data_dictionary.hpp"
#include "fix/parser/parser.hpp"
#include "fix/parser/serializer.hpp"
#include "fix/session/session.hpp"
#include "fix/store/memory_store.hpp"

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

using namespace fix;

namespace {

constexpr std::string_view kSendingTime = "20240101-12:00:00.000";

// A typical NewOrderSingle (35=D), ~200 bytes on the wire.
std::string make_new_order(SeqNum seq) {
    MessageBuilder b;
    b.begin("FIX.4.2", msg_types::NewOrderSingle);
    b.add(tags::SenderCompID, "CLIENT");
    b.add(tags::TargetCompID, "SERVER");
    b.add(tags::MsgSeqNum, static_cast<std::int64_t>(seq));
    b.add(tags::SendingTime, kSendingTime);
    b.add(tags::ClOrdID, "ORD-0000012345");
    b.add(tags::HandlInst, "1");
    b.add(tags::Symbol, "AAPL");
    b.add(tags::Side, "1");
    b.add(tags::TransactTime, kSendingTime);
    b.add(tags::OrderQty, static_cast<std::int64_t>(100));
    b.add(tags::OrdType, "2");
    b.add(tags::Price, 189.25, 2);
    b.add(tags::TimeInForce, "0");
    return b.finish();
}

Message make_new_order_message() {
    Message m(msg_types::NewOrderSingle);
    m.set(tags::ClOrdID, "ORD-0000012345");
    m.set(tags::HandlInst, "1");
    m.set(tags::Symbol, "AAPL");
    m.set(tags::Side, "1");
    m.set(tags::TransactTime, kSendingTime);
    m.set(tags::OrderQty, static_cast<std::int64_t>(100));
    m.set(tags::OrdType, "2");
    m.set(tags::Price, 189.25);
    m.set(tags::TimeInForce, "0");
    return m;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

// One complete frame fed and extracted per iteration (the common case for a
// low-rate session: each read() delivers exactly one message).
void BM_Parse_SingleMessage(benchmark::State &state) {
    const std::string wire = make_new_order(1);
    StreamParser parser;
    Message out;
    for (auto _ : state) {
        parser.feed(wire.data(), wire.size());
        bool ok = parser.next(out);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(wire.size()));
}
BENCHMARK(BM_Parse_SingleMessage);

// N frames delivered in one read() (bursty market-data style traffic).
void BM_Parse_Batch(benchmark::State &state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::string batch;
    for (std::size_t i = 0; i < n; ++i)
        batch += make_new_order(static_cast<SeqNum>(i + 1));
    StreamParser parser;
    Message out;
    for (auto _ : state) {
        parser.feed(batch.data(), batch.size());
        std::size_t got = 0;
        while (parser.next(out))
            ++got;
        if (got != n)
            state.SkipWithError("parser dropped messages");
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(batch.size()));
}
BENCHMARK(BM_Parse_Batch)->Arg(16)->Arg(128);

// One frame split across small TCP segments (worst-case reassembly).
void BM_Parse_Fragmented(benchmark::State &state) {
    const auto chunk = static_cast<std::size_t>(state.range(0));
    const std::string wire = make_new_order(1);
    StreamParser parser;
    Message out;
    for (auto _ : state) {
        for (std::size_t off = 0; off < wire.size(); off += chunk)
            parser.feed(wire.data() + off, std::min(chunk, wire.size() - off));
        bool ok = parser.next(out);
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(wire.size()));
}
BENCHMARK(BM_Parse_Fragmented)->Arg(16)->Arg(64);

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

// Incremental builder: begin/add.../finish (BodyLength + CheckSum).
void BM_Serialize_Builder(benchmark::State &state) {
    std::int64_t bytes = 0;
    SeqNum seq = 1;
    for (auto _ : state) {
        std::string wire = make_new_order(seq++);
        bytes += static_cast<std::int64_t>(wire.size());
        benchmark::DoNotOptimize(wire);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(bytes);
}
BENCHMARK(BM_Serialize_Builder);

// Message object -> wire, the path Session::send uses.
void BM_Serialize_Message(benchmark::State &state) {
    const Message msg = make_new_order_message();
    MessageBuilder b;
    std::int64_t bytes = 0;
    SeqNum seq = 1;
    for (auto _ : state) {
        std::string wire = b.serialize(msg, "FIX.4.2", seq++, "CLIENT", "SERVER", kSendingTime);
        bytes += static_cast<std::int64_t>(wire.size());
        benchmark::DoNotOptimize(wire);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(bytes);
}
BENCHMARK(BM_Serialize_Message);

void BM_Checksum(benchmark::State &state) {
    const std::string wire = make_new_order(1);
    for (auto _ : state) {
        auto cs = MessageBuilder::compute_checksum(wire.data(), wire.size());
        benchmark::DoNotOptimize(cs);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(wire.size()));
}
BENCHMARK(BM_Checksum);

void BM_FormatTimestamp(benchmark::State &state) {
    const auto tp = now();
    for (auto _ : state) {
        std::string ts = MessageBuilder::format_timestamp(tp);
        benchmark::DoNotOptimize(ts);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FormatTimestamp);

// ---------------------------------------------------------------------------
// Message access and validation
// ---------------------------------------------------------------------------

// Typical application read of a parsed order: a handful of lookups + numeric
// conversions over the linear field vector.
void BM_Message_FieldAccess(benchmark::State &state) {
    const std::string wire = make_new_order(1);
    StreamParser parser;
    parser.feed(wire.data(), wire.size());
    Message msg;
    if (!parser.next(msg)) {
        state.SkipWithError("setup parse failed");
        return;
    }
    for (auto _ : state) {
        auto sym = msg.get(tags::Symbol);
        auto qty = msg.get_int(tags::OrderQty);
        auto px = msg.get_double(tags::Price);
        auto id = msg.get(tags::ClOrdID);
        benchmark::DoNotOptimize(sym);
        benchmark::DoNotOptimize(qty);
        benchmark::DoNotOptimize(px);
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Message_FieldAccess);

void BM_Dictionary_Validate(benchmark::State &state) {
    DataDictionary dict;
    dict.load_builtin(FixVersion::FIX_4_2);
    const std::string wire = make_new_order(1);
    StreamParser parser;
    parser.feed(wire.data(), wire.size());
    Message msg;
    if (!parser.next(msg)) {
        state.SkipWithError("setup parse failed");
        return;
    }
    for (auto _ : state) {
        auto r = dict.validate(msg);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dictionary_Validate);

// ---------------------------------------------------------------------------
// Session layer
// ---------------------------------------------------------------------------

SessionConfig session_cfg(bool initiator) {
    SessionConfig cfg;
    cfg.id.version = FixVersion::FIX_4_2;
    cfg.id.senderCompID = initiator ? "CLIENT" : "SERVER";
    cfg.id.targetCompID = initiator ? "SERVER" : "CLIENT";
    cfg.initiator = initiator;
    cfg.heartbeat_interval = 30;
    return cfg;
}

// Two sessions wired back to back through in-memory queues (no transport).
// Frames are queued on send and pumped afterwards so neither session is
// re-entered from inside its own send path.
struct SessionPair {
    std::deque<std::string> to_acceptor;
    std::deque<std::string> to_initiator;
    std::uint64_t app_msgs = 0;
    bool capture_outbound = true; // false: initiator frames are discarded
    DataDictionary dict;
    std::unique_ptr<Session> initiator;
    std::unique_ptr<Session> acceptor;

    explicit SessionPair(bool validate) {
        SessionCallbacks icb;
        icb.do_send = [this](const std::string &s) {
            to_acceptor.push_back(s);
        };
        SessionCallbacks acb;
        acb.do_send = [this](const std::string &s) {
            to_initiator.push_back(s);
        };
        acb.on_message = [this](const SessionID &, const Message &) {
            ++app_msgs;
        };

        auto icfg = session_cfg(true);
        auto acfg = session_cfg(false);
        acfg.validate_fields = validate;
        dict.load_builtin(FixVersion::FIX_4_2);
        initiator = std::make_unique<Session>(icfg, std::make_unique<MemoryStore>(), nullptr, icb);
        acceptor = std::make_unique<Session>(acfg, std::make_unique<MemoryStore>(),
                                             validate ? &dict : nullptr, acb);
    }

    void pump() {
        while (!to_acceptor.empty() || !to_initiator.empty()) {
            while (!to_acceptor.empty()) {
                std::string f = std::move(to_acceptor.front());
                to_acceptor.pop_front();
                acceptor->on_data(f.data(), f.size());
            }
            while (!to_initiator.empty()) {
                std::string f = std::move(to_initiator.front());
                to_initiator.pop_front();
                initiator->on_data(f.data(), f.size());
            }
        }
    }

    bool establish() {
        initiator->on_transport_connected();
        acceptor->on_transport_connected();
        (void)initiator->logon();
        pump();
        return initiator->is_active() && acceptor->is_active();
    }
};

// Outbound only: Session::send (seq assignment, serialise, store, do_send).
void BM_Session_Send(benchmark::State &state) {
    SessionPair pair(false);
    if (!pair.establish()) {
        state.SkipWithError("logon failed");
        return;
    }
    pair.capture_outbound = false;
    const Message order = make_new_order_message();
    for (auto _ : state) {
        auto r = pair.initiator->send(order);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Session_Send);

// Inbound only: acceptor on_data for a pre-built app frame (parse, seq check,
// optional dictionary validation, on_message dispatch).
void BM_Session_Receive(benchmark::State &state) {
    SessionPair pair(state.range(0) != 0);
    if (!pair.establish()) {
        state.SkipWithError("logon failed");
        return;
    }
    // Pre-build frames so the measured path is receive only. Seq 1 was the
    // Logon, so application traffic starts at 2.
    constexpr std::size_t kFrames = 1 << 16;
    std::vector<std::string> frames;
    frames.reserve(kFrames);
    SeqNum seq = 2;
    for (std::size_t i = 0; i < kFrames; ++i)
        frames.push_back(make_new_order(seq++));
    std::size_t idx = 0;
    for (auto _ : state) {
        if (idx == kFrames) {
            state.PauseTiming();
            for (auto &f : frames)
                f = make_new_order(seq++);
            idx = 0;
            pair.to_initiator.clear();
            state.ResumeTiming();
        }
        const auto &f = frames[idx++];
        pair.acceptor->on_data(f.data(), f.size());
    }
    if (pair.app_msgs != static_cast<std::uint64_t>(state.iterations()))
        state.SkipWithError("acceptor did not deliver every message");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Session_Receive)->ArgName("validate")->Arg(0)->Arg(1);

// Full one-way trip: initiator send -> wire -> acceptor parse -> on_message.
void BM_Session_RoundTrip(benchmark::State &state) {
    SessionPair pair(false);
    if (!pair.establish()) {
        state.SkipWithError("logon failed");
        return;
    }
    const Message order = make_new_order_message();
    for (auto _ : state) {
        (void)pair.initiator->send(order);
        pair.pump();
    }
    if (pair.app_msgs != static_cast<std::uint64_t>(state.iterations()))
        state.SkipWithError("acceptor did not deliver every message");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Session_RoundTrip);

} // namespace

BENCHMARK_MAIN();
