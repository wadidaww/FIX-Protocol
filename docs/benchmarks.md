# Benchmarks

Microbenchmarks for the engine's hot paths, built on
[Google Benchmark](https://github.com/google/benchmark). They live in
`benchmarks/bench_main.cpp` and are off by default.

## Running

```bash
cmake -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DFIX_BUILD_TESTS=OFF -DFIX_BUILD_APPS=OFF -DFIX_BUILD_BENCHMARKS=ON
cmake --build build-bench --target fix_bench
./build-bench/fix_bench --benchmark_min_time=1s --benchmark_repetitions=5 \
      --benchmark_report_aggregates_only=true
```

Useful flags: `--benchmark_filter=Session` to run a subset,
`--benchmark_out=results.json --benchmark_out_format=json` to save results for
comparison with `compare.py` from the Google Benchmark repo. Google Benchmark
is used from the system if CMake finds it, otherwise fetched (v1.9.1).

For stable numbers, run on an idle machine with CPU frequency scaling set to
`performance`, and compare runs from the same machine only. The build does not
use `-march=native`, so results reflect portable `-O3` code.

## What is measured

All cases use a ~200-byte FIX 4.2 NewOrderSingle (35=D, 14 body fields). No
sockets are involved; the session cases wire two `Session` objects back to back
through in-memory queues with a `MemoryStore`, so these are CPU costs only.

| Benchmark | Path |
|---|---|
| `BM_Parse_SingleMessage` | `StreamParser::feed` + `next` for one complete frame |
| `BM_Parse_Batch/N` | N frames delivered in one `feed` (bursty reads) |
| `BM_Parse_Fragmented/N` | one frame fed in N-byte chunks (TCP reassembly) |
| `BM_Serialize_Builder` | `MessageBuilder` begin/add/finish |
| `BM_Serialize_Message` | `MessageBuilder::serialize(Message, ...)`, the `Session::send` path |
| `BM_Checksum` | `compute_checksum` over one frame |
| `BM_FormatTimestamp` | `format_timestamp` (SendingTime) |
| `BM_Message_FieldAccess` | 4 lookups incl. `get_int` / `get_double` on a parsed order |
| `BM_Dictionary_Validate` | `DataDictionary::validate` with the built-in FIX 4.2 dictionary |
| `BM_Session_Send` | `Session::send`: seq assignment, serialise, store, `do_send` |
| `BM_Session_Receive/validate:V` | acceptor `on_data`: parse, seq check, optional dictionary validation, `on_message` |
| `BM_Session_RoundTrip` | initiator `send` → wire → acceptor parse → `on_message` |

## Baseline results

Commit `2f2a341` (main), Release build, median of 5 repetitions at
`--benchmark_min_time=1s`.

Machine: cloud VM, 4 vCPU Intel Xeon @ 2.10 GHz (L2 2 MiB/core, L3 260 MiB),
Linux 6.18, GCC 13.3, `-O3`. This is a shared VM, so run-to-run variation
(coefficient of variation) was 1–20% depending on the case; treat
differences under ~15% as noise.

| Benchmark | Time / op | Throughput |
|---|---:|---:|
| Parse, single message | 835 ns | 1.20 M msg/s (190 MiB/s) |
| Parse, batch of 16 | 837 ns/msg | 1.20 M msg/s (190 MiB/s) |
| Parse, batch of 128 | 804 ns/msg | 1.24 M msg/s (198 MiB/s) |
| Parse, 16-byte fragments | 1342 ns | 745 k msg/s |
| Parse, 64-byte fragments | 960 ns | 1.04 M msg/s |
| Serialize via builder | 626 ns | 1.60 M msg/s |
| Serialize `Message` | 520 ns | 1.92 M msg/s |
| Checksum (200 B) | 14 ns | 11.0 GiB/s |
| Format timestamp | 238 ns | 4.20 M/s |
| Field access (4 lookups) | 94 ns | 10.7 M/s |
| Dictionary validate | 175 ns | 5.71 M/s |
| Session send | 1318 ns | 759 k msg/s |
| Session receive, no validation | 1222 ns | 818 k msg/s |
| Session receive, with validation | 1551 ns | 645 k msg/s |
| Session round trip (send + receive) | 2519 ns | 397 k msg/s |

### Observations

- Parsing (~0.8 µs per 200-byte message) costs more than serialising
  (~0.5 µs). The checksum itself is negligible (14 ns), so parse time is
  dominated by field splitting and per-field allocation into `Message`.
- Fragmented input adds up to ~60% when frames arrive in 16-byte pieces.
- `format_timestamp` is ~240 ns, a meaningful share of every outbound message.
- The session layer roughly doubles the bare parse/serialise cost
  (send 1.3 µs vs 0.5 µs serialise; receive 1.2 µs vs 0.8 µs parse), from
  locking, the message store copy and callback dispatch.
