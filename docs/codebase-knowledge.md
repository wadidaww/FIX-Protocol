# FIX-Protocol Codebase — Knowledge Base

> Structured record of the full codebase analysis performed 2026-09-26.
> Produced by 4 parallel deep-analysis agents (core/parser, session/engine,
> store/transport/log/dict, tests/CI/build) + direct verification.
> Companion document: `docs/proposed-plan.md`.

---

## 1. Repository Overview

| Item | Value |
|---|---|
| Language / standard | C++23 (`std::expected`, `from_chars`) |
| Build | CMake ≥ 3.25, Ninja, 9 build options (tests/apps/shared/ASAN/TSAN/UBSAN/LTO/no-exceptions) |
| Size | ~4,800 LOC (include + src + tests + apps); 9 source TUs, 16 headers |
| Tests | 56 GTest cases (57 CTest entries — duplicate registration, see §8) — **all passing** |
| License | Apache-2.0 |
| Branch | `refactor` (1 unpushed commit ahead of `origin/refactor`) |
| CI | GitHub Actions: Linux GCC-14/Clang-19, macOS, Windows MSVC, ASAN+UBSAN job, TSAN job, format-check |

### File inventory

```
include/fix/
  core/         types.hpp (238), field.hpp (81), message.hpp (148), constants.hpp (205)
  parser/       parser.hpp (86), serializer.hpp (95)
  dictionary/   data_dictionary.hpp (154)
  session/      session.hpp (177), session_manager.hpp (49)
  store/        message_store.hpp (52), memory_store.hpp (72), file_store.hpp (64)
  transport/    transport.hpp (68), tcp_transport.hpp (79)
  log/          message_log.hpp (83)
  engine.hpp    (93)
src/
  parser/parser.cpp (203), serializer.cpp (97)
  dictionary/data_dictionary.cpp (414)
  session/session.cpp (458), session_manager.cpp (68)
  store/file_store.cpp (168)
  transport/tcp_transport.cpp (323)
  log/message_log.cpp (130)
  engine.cpp (137)
tests/          test_parser (225), test_session (243), test_dictionary (143),
                test_store (155), test_message (102)
apps/oms/main.cpp (180)   — reference OMS demo
```

---

## 2. Architecture & Data Flow

### Layers
```
Application (apps/oms, user code)
   │  Session::send(Message) / SessionCallbacks
Engine (src/engine.cpp)
   │  owns sessions, transports, timer thread (200 ms tick), audit log (never used)
Session (src/session/session.cpp)  ← FSM + seq numbers + admin msg handling
   │  StreamParser (RX) / MessageBuilder (TX)
   │  IMessageStore (MemoryStore | FileStore)  +  DataDictionary (unused in session!)
ITransport (TcpTransport)  ← 1 thread per transport, epoll ET (Linux) / busy-poll
```

### Receive path (function-by-function)
1. `TcpTransport::handle_recv()` — `tcp_transport.cpp:248-272`: `::recv()` into 64 KB `recv_buf_`, loops to `EAGAIN`, calls `on_data_` per read.
2. `Session::on_data()` — `session.cpp:51-61`: `parser_.feed()` then `while (parser_.next(msg)) process_message(msg)`; **all exceptions silently swallowed** (`catch(const std::exception&){}` empty).
3. `StreamParser::feed()` — `parser.cpp:22-36`: `compact()` then `buf_.resize(write_pos_+len+65536)` (**zero-fills**), `memcpy` append, `while (try_parse_one())` eagerly parses everything into `pending_`.
4. `StreamParser::try_parse_one()` — `parser.cpp:111-201`: requires `8=` at offset 0 → parse 8 → parse 9 (BodyLength, **from_chars unchecked**) → wait `avail >= body_start+body_length+7` → loop fields via `parse_field()` (string_view into `buf_`) → `msg.set(tag, string)` (copy) → verify checksum (`parser.cpp:167-177`) + body length (`:179-187`) → `set_raw(full copy)` → push to `pending_`.
5. `Session::process_message()` — `session.cpp:145-178`: seq validation → admin/app dispatch → store inbound.

### Transmit path
1. `Session::send()` → `send_message()` — `session.cpp:428-446`: under `send_mutex_`: assign seq → `builder_.serialize(msg,…)` → `store_outbound` (**Result discarded**) → `cbs_.do_send(wire)`.
2. `try_serialize` — `serializer.cpp:83-96`: emits the fixed header `8,9,35,49,56,34,[43],[122],52` (43 `PossDupFlag` / 122 `OrigSendingTime` in header position when present — F5; `123`/`36` deliberately stay body fields), then all remaining fields in stored order minus the header skip-list; `build()` computes BodyLength + CheckSum (mod-256).
3. `TcpTransport::send()` — direct `::send()` if connected, else queue under `send_mutex_`.

### Threads
| Thread | Entry points |
|---|---|
| App/user | `Session::send/logon/logout/disconnect/reset`, `Engine::add/remove_session` |
| Engine timer (200 ms) | `Engine` timer loop → `Session::on_timer` (heartbeats, timeouts, gap retry, user cb). `SessionManager::tick_all` was deleted as dead code (F10) |
| Transport IO (1/transport) | `Session::on_data` → full admin/app dispatch, replies, user callbacks |
| `Engine::stop()` / dtor | `logout()` via `for_each` (snapshot-then-iterate since 3.1 — the registry lock is **not** held across the logout/user callbacks) |

A single `Session` is touched by ≥3 threads. Only outbound path has `send_mutex_`.
**Lock order (3.1, authoritative statement)**: `manager → recv → send → store → transport`,
documented at the top of `include/fix/session/session_manager.hpp` (cross-referenced in
`include/fix/engine.hpp`); rules: never hold manager/store/send across a user callback,
snapshot before you iterate; `Session::send_message` holding `send_mutex_` across the
`do_send` transport *enqueue* is the explicitly accepted boundary.

---

## 3. What Is Correct (verified, incl. ASan+UBSan fuzz of 3,000 streams / 312 KB — clean)

- **CheckSum math**: mod-256 over bytes `[0, start_of_"10=")` with `unsigned char` casts — TX and RX — **no signed-char bug**.
- **BodyLength definition**: TX counts `35=`…SOH of last body field, excludes 8/9/10. RX validates `actual = start("10=") − start("35=")`.
- **SOH framing / trailing SOH**, partial frames across TCP reads (3-byte chunking tested; fuzz-confirmed with random chunk sizes).
- **No memory-safety bugs**: parse_field strictly bounded; all views copied before reuse; no dangling views today.
- **No global mutable state** in core layer; pure helpers reentrant.
- Wire-format arithmetic is sound — the foundation is trustworthy.

---

## 4. Critical Findings (P0 — engine unusable / unsafe)

### C1. Engine never wires `do_send` → zero bytes transmitted via public API
- `engine.cpp:99-102` contains comment *"We need to update the session's do_send callback … not possible … after creation"* — `Session::send_message` (`session.cpp:440-441`) silently drops every message. README usage example and `apps/oms` are non-functional. No test touches `Engine`, so nothing catches it.

### C2. `on_connected` empty → no auto-Logon, acceptor never completes handshake
- `engine.cpp:87` sets an empty lambda. Initiator never sends Logon; `SessionConfig::initiator` has no effect.

### C3. Peer FIN wedges the event loop → no reconnect, acceptor serves 1 client ever
- On `recv()==0` → `close_connection()` closes `conn_fd_` (auto-removed from epoll) → `epoll_wait` returns 0 forever; loop only exits on `running_==false`/`EPOLLHUP/EPOLLERR`. Linux reports peer FIN as `EPOLLIN` (recv 0), not HUP. `EPOLLRDHUP` not registered. Consequences: acceptor never returns to `accept()`; initiator never re-iterates `do_connect()`. Also: `connected_` set optimistically right after `EINPROGRESS` (`tcp_transport.cpp:132-135`, no `SO_ERROR` check); fixed 5 s reconnect sleep ignoring `reconnect_delay`; `getaddrinfo` blocks IO thread.

### C4. Session use-after-free
- `SessionManager::create_session` overwrites `sessions_[key]` → old Session destroyed while old transport callbacks hold raw `Session*` (`engine.cpp:87-96`) → UAF.
- `Engine::remove_session` erases session but leaves `connections_` running → UAF + thread/fd leak.
- `SessionManager::find` returns raw `Session*` after releasing lock.
- `make_key` **ignores `SessionID::qualifier`** → sessions differing only by qualifier collide silently.

### C5. `TcpTransport::stop()` fd double-close race
- `stop()` (caller thread) closes `epoll_fd_/listen_fd_/conn_fd_` (plain `int`s) while IO thread is in `epoll_wait/recv/send`, then joins. IO thread may close already-closed/reused fd → double-close. Fix: eventfd self-wakeup, IO thread closes own fds.

### C6. Parser: no resync — permanent stall + unbounded memory (DoS)
- `try_parse_one` only inspects offset 0; if not `8=` returns false forever, never advances `read_pos_`, never errors. Every `feed()` grows `buf_` without bound. Reproduced: garbage(3)+valid → 0 messages; 100 KB 'Z' → `bytes_consumed()==0` retained forever.
- Oversized BodyLength path (`parser.cpp:142-146`) skips **only the header** → parser stranded mid-body forever; `consumed_` not updated.
- Malformed field (`abc=zz`, tag > uint32, empty tag) → `parse_field` returns 0 for both "incomplete" and "invalid" → silent permanent stall.
- Missing `10=` → body loop unbounded by `body_length` → swallows subsequent messages' fields, then discards both.
- Inflated-but-under-limit BodyLength → head-of-line blocks valid buffered messages.
- Error paths `return false` stop the `feed()` loop while ET epoll may never re-trigger → messages stuck.
- Quadratic CPU on adversarial input (re-parse all fields every feed + zero-fill growth).

### C7. Repeating groups destroyed on parse
- Parser inserts via `Message::set()` which **deduplicates** (`message.hpp:27-35`): `55=AAA … 55=BBB` → last wins. Reproduced: `146=2…` groups collapse to 1 entry. Duplicate *header* tags also silently accepted (spec: Reject reason 13 `TagAppearsMoreThanOnce`). `MessageDef::groups` never populated anywhere. Blocks MarketData/Parties/Allocs.

### C8. Field injection on serialize (security)
- `serializer.hpp:85-92` appends value verbatim — a value containing SOH + `98=1` injects a real field on the wire. Reproduced. Fix: reject SOH/control chars in `append_field` + `Message::set`.

### C9. No session identity validation (security)
- `BeginString` never validated (`session.cpp:148` = comment only); `SenderCompID`/`TargetCompID` **never checked** → any peer that reaches the socket can inject messages into the session (hijack). `8=BOGUS` accepted by parser.

### C10. FileStore crash-unsafe sequence persistence
- `persist_seqs()` = truncate-then-write (non-atomic), no fsync anywhere; `load_seqs()` unvalidated → torn write = silent seq reset to 1. Write failures return success but still index. Store filenames exclude `qualifier` → two sessions clobber each other. No cross-process locking.

### C11. DataDictionary stubs report success
- `load()` = `(void)path; return {};` (no XML parsing) — `reload()` just calls it. Tests **codify the stub** (assert loading nonexistent path succeeds). `load_builtin()` clears maps under lock, repopulates **without lock** (comment claiming otherwise is false). Hot-reload "atomic swap" claim false.

### C12. Audit log never written
- `Engine` constructs `FileAuditLog` (`engine.cpp:13-20`) but **zero call sites** of `IAuditLog::log`. MiFID II / SEC 605/606 claims unsupported. `rotate()` reopens same-second file (re-appends); `retain_days`/`compress` unused.

### C13. Windows transport is a no-op
- All socket code inside `#ifndef _WIN32` (`do_connect`, `handle_recv/send`, `send()` body, `run_acceptor`); `make_nonblocking/tcp_nodelay` return 0 stubs. CI passes only because there are no transport tests.

---

## 5. High-Severity Findings (P1 — protocol conformance & robustness)

### Session/FSM
- **Resend/GapFill non-conformant** (`session.cpp:275-310`): replays raw bytes without `PossDupFlag=Y`/`OrigSendingTime` (self-admitted `:290`); no gap-fill of admin messages; calls `do_send` bypassing `send_mutex_`; holds store mutex across I/O; no `gap_open_` throttling → one ResendRequest per out-of-seq message; `BeginSeqNo > NextSender` not rejected.
- **SequenceReset broken**: `validate_seq_num` runs *before* dispatch (`session.cpp:157-163`) → reset-mode SequenceReset judged "too low, not PossDup" → Logout+disconnect before `handle_sequence_reset` runs. Non-PossDup SequenceReset leaves `NextTarget = NewSeqNo+1` (off-by-one). No checks: gap-fill requires `PossDup=Y`, `NewSeqNo > expected`.
- **TestRequest timeout bug (critical)** (`session.cpp:106-117`): disconnect fires on the *next 200 ms tick* after sending TestRequest (uses `last_recv_time_` never `test_request_sent_time_`) vs required `1.2×HeartBtInt` (36 s at 30 s hb). `test_req_pending_` cleared only by matching TestReqID — any other inbound traffic leaves it stuck → next stale period disconnects without retry.
- **HeartBtInt adoption**: `handle_logon` blindly copies peer value; `HeartBtInt=0` → heartbeat every 200 ms (bandwidth storm); missing value not rejected; race with timer thread.
- **Logout timeout dead**: `cfg_.logout_timeout` unused; `LogoutSent` state skips timer gate → peer that never answers Logout hangs forever; no heartbeats while awaiting Logout.
- **Duplicate Logon** not handled (spec: Logout + disconnect).
- **FSM**: dead `LogoutReceived` state, `Reconnecting` never used; non-CAS check-then-act on `state_` (two loads in `logon()`); `send()` succeeds while Disconnected → phantom seq numbers.
- **`handle_reject` empty** (`session.cpp:312-314`) — session-level Reject invisible; rejected Logon never terminates handshake.
- Reject for missing MsgType sent **before** consuming seq → peer sees gap → resend loop; `RefSeqNum=0` (mandatory field).
- `is_admin_msg` hard-codes 7 types (incomplete for 4.4+/FIXT).

### Store error handling
- All `Result<void>` returns discarded: `store_outbound` (`session.cpp:437`) — seq incremented even on failure → permanent desync; `send_message` **always returns `{}`**; `do_send` is `void` → transport errors unreportable. FileStore dtor `catch(...){}`.

### Dictionary
- Validation **never invoked** *(at audit time)*: `Session::dict_` stored then never read; `SessionConfig::validate_fields` dead; `DataDictionary::validate()` has zero call sites → malformed orders passed straight to app. Wired in Phase 2 task 2.7 — since F8 it defaults to **OFF** and must be opted into.
- Coverage: **66 fields** (of ~1,000+), **15 message types** (of ~80+ in 4.2 alone), zero repeating-group definitions. FIX 4.4 = `load_builtin_messages_42()` relabeled; FIX 5.0SP2 = 4.4 + `ApplVerID` field. FIXT 1.1 gets 5.0 content.
- Enum tables wrong: `TimeInForce` missing 4.4 A/B/C/D; `OrdStatus`/`ExecType` mismatched per version; `Side` includes invalid `9`; `CheckSum` typed String.
- Fields referenced by message defs (HandlInst 21, ExecInst 18, RawData 96…) never defined in `load_builtin_fields`.
- `resolve_appl_ver_id` correct but zero production callers (no ApplVerID routing).
- `DictionaryRegistry::set()` replaces shared_ptr → dangling raw `Session::dict_` / `Engine::dictionary()`.

### Threading (TSAN-reportable races)
| Object | Writers | Readers |
|---|---|---|
| `last_recv_time_` | IO (`session.cpp:146`) | timer (`:108`) |
| `last_send_time_` | send path (under `send_mutex_`) | timer (**no lock**, `:102`) |
| `logon_sent_time_` | `logon()` caller | timer (`:122`) |
| `test_req_pending_`, `pending_test_req_id_` | IO + timer | both |
| `cfg_.heartbeat_interval` | IO (`handle_logon:218`) | timer (`:101,107`) |
| `parser_` | any thread calling `reset()` | IO |
| store seq compound ops | IO (`handle_logon:222-223`) | user (`send`) |
| `conn_fd_/epoll_fd_/listen_fd_` (plain int) | `stop()`/`send()` caller | IO thread |

- ~~**Deadlock hazard**: `SessionManager::for_each` holds `shared_lock` across `on_timer` → user callbacks + network I/O; callback calling `add_session/remove_session` (unique_lock same mutex) = recursive `shared_mutex` lock → UB/deadlock.~~ **FIXED (3.1)**: `for_each` snapshots the `shared_ptr` list under the lock, releases it, then iterates; `session_manager.cpp` + regression test `LockOrder.ForEachAllowsManagerMutationFromCallback`.
- Lock ordering (audit 3.1): resend path snapshots the store **before** taking `send_mutex_` (no `store → send` nesting); send path takes `send_mutex_ → store → transport` — all consistent with the documented order `manager → recv → send → store → transport`.
- `do_send` invoked **while `send_mutex_` held**: kept as the documented *accepted boundary* (3.1) — it is the transport enqueue (bounded, non-blocking, no user code) that establishes `send → transport` ordering; user callbacks (`SessionCallbacks::on_*`, `EngineConfig` hooks) were audited to run with no manager/store/send lock held.
- User `on_message` throwing inside `on_data` catch skips `incr_target_seq_num` → next message looks like gap → ResendRequest storm. Timer-thread callback exceptions → `std::terminate`.

### Transport
- Cross-thread `send_queue_` + **EPOLLOUT edge-triggered**: data queued after last writable edge sits until unrelated edge → stalled output.
- No `EPOLLRDHUP`, no `SO_KEEPALIVE`, no read deadline → half-open peer leaves session Active.
- Acceptor = `accept()` + 10 ms sleep busy-loop (not epoll); non-Linux fallback = 1 ms busy-sleep, never flushes queued sends.
- `send_queue_` never cleared on disconnect → stale bytes on next connection; send errors silently `break` leaving `connected_==true`.
- Bind/listen failure exits acceptor thread silently; `set_on_error` never wired by Engine.
- `run_acceptor` overwrites `conn_fd_` without closing previous (fd leak once C3 fixed).

### Engine lifecycle
- `add_session` **after `start()`**: transport never started (sweep only in `start()`) → silent no-op.
- `Engine::stop()`: no wait for Logout response, transports stopped before queued bytes flush; `start/stop` cycles broken (`stop()` clears `connections_`).
- Dead `Engine::dicts_` member (the `Engine::timer_loop()` claim removed — the timer is a lambda in `start()`, `engine.cpp`.)
- Transport callbacks capture raw `Session*`; `SessionCallbacks` copied 3× per `add_session` (missing `std::move`).

### Error handling
- Exceptions swallowed silently (`session.cpp:55-59` empty catch); no `on_error` wiring; no exception boundary at timer/transport/`stop()` entry points.
- `std::filesystem::create_directories` unwrapped in log/store ctors → throw at construction → `std::terminate`.
- Dead/ignored config *(at audit time)*: `reconnect_delay` (hardcoded 5 s), `reset_seq_num`, `validate_fields`, `logout_timeout`, `reset_on_disconnect`, `send_buffer_size`; `build_header_fields` declared never defined. Several are live since Phase 2: `validate_fields` (opt-in, F8), `logout_timeout`, `reset_on_logon` (applies `store->reset()` before numbering, F1).

---

## 6. Medium Findings (P2 — correctness, performance, portability)

### Parser/serializer correctness
- `from_chars` errors unchecked: BodyLength garbage→0 (`parser.cpp:139`); `get_int("12abc")==12` (ptr never checked, `message.hpp:77-79`, `field.hpp:53-55`); `seq_num()` casts int64→uint64: `34=-5` → huge → bogus gap storm.
- CheckSum value: `from_chars` error ignored, narrowed `static_cast<uint8_t>` → `10=441` accepted when low byte matches (reproduced); 4-digit `10=0NNN` accepted; no exact-3-digit rule.
- `to_chars` `ec` unchecked: `1e300` fixed-6dp → 64-char garbage value on wire (measured); `inf`/`nan` serialize literally.
- `std::strtod` **locale-dependent** (de_DE parses `1.5` as `1.0`), accepts `inf`/hex/whitespace; empty→0.0.
- `Message::set(tag, 1)` = hard compile error (ambiguous int64/double/bool overloads).
- `set(double)` hard-codes 6 dp (no precision param) → sub-µ precision loss.
- TX header order wrong: `43/122/50/115/128/…` not in skip-list → emitted *after* `52`/body → strict counterparties reject `TagOutOfOrder`.
- `last_error_` never cleared on success; `consumed_` missed on one error path; `next()` leaves stale `out`.
- Data/Length field pairs (95/96, 93/89…) unsupported — SOH inside data fields corrupts framing.
- `poss_dup()` accepts only `Y`, `as_bool` accepts y/n — inconsistent.
- Missing direct `<cstdlib>`/`<cstdio>` includes (transitive only → MSVC/libc++ risk).

### Performance (hot path)
- `Message::get/set` = **linear scan** → building n-field message is O(n²).
- Per inbound message: 1 `std::string` per field (SSO ≤15 bytes, heap beyond — timestamps at 21 chars heap-allocate), full-message `raw_` copy, `vector<Field>` growth without `reserve`, unbounded `pending_` (no backpressure).
- Per outbound `finish()`: ~3–4 full-message copies/allocs (body_str, pre_cs concat, result).
- `get_double` allocates a string per call; no `reserve`; quadratic re-parse per feed; zero-fill growth.
- README "zero-copy" claim false; `RawField`/`FieldValue` (view types) are **dead code**, never referenced.

### Portability
- Header guards POSIX under `#ifdef __linux__`, .cpp under `#ifndef _WIN32` (fragile).
- `MSG_NOSIGNAL` Linux-only; no `SIGPIPE`/`SO_NOSIGPIPE` fallback → possible process termination on macOS/others.
- Store/log filename built from CompIDs without sanitization (`/`, `:` in path).

---

## 7. Tests — coverage & gaps

**Covered (56 tests):** parser happy-path + 3-byte chunking + concatenation; outbound checksum/body-length; Message get/set; dictionary load/lookup/validate (unit) + `resolve_appl_ver_id`; MemoryStore/FileStore basics (tmpdir per test — parallel collision fixed in history); session basics (logon, heartbeat, seq).

**Zero coverage for:** end-to-end handshake over real socket (**would have caught C1**), resend/gap recovery, SequenceReset both modes, duplicate logon, timer/timeouts (TestRequest/Logout), disconnect+reconnect, `remove_session` under traffic, CompID/BeginString rejection, malformed/garbage/resync/oversized input, duplicate tags/groups, SOH injection, FileStore crash recovery, concurrency (TSAN job vacuous), Engine lifecycle, transport, message_log. TUs `engine.cpp`, `session_manager.cpp`, `tcp_transport.cpp`, `message_log.cpp` (658 lines) have **0% coverage**.

---

## 8. Build / CI / Hygiene

- **Duplicate ctest registration**: `gtest_discover_tests` (`CMakeLists.txt:184`) + `add_test(fix_unit_tests)` (`:186`) → 57 entries, every test runs twice, races under `ctest -j`.
- **`find_package(FIXEngine)` broken**: no `EXPORT`, no `FIXEngineConfig.cmake` generated, version file never installed (`:201-216`).
- **CI doesn't run on `refactor` branch** — triggers: pushes `main`+`copilot/**`, PRs to `main` only (`ci.yml:4-8`).
- `-Werror` absent; `-Wno-unused-parameter` blanket; missing `-Wconversion/-Wshadow/-Wformat=2/…`; `FIX_WARNINGS_AS_ERRORS` option doesn't exist.
- spdlog: `find_package(QUIET)` + PUBLIC link + `FIX_USE_SPDLOG=1` but **never used in any source** → dev/CI builds differ; `vcpkg.json` decorative (spdlog+gtest features unused, no toolchain wired).
- GTest FetchContent pinned to mutable tag `v1.14.0` (not SHA).
- `clang-format` glob without `CONFIGURE_DEPENDS`; local format target may pick clang-format ≠ CI's 18.
- Default build type set as normal var → cache shows empty `CMAKE_BUILD_TYPE`.
- No `.clang-tidy`, `.editorconfig`, `CMakePresets.json`, coverage, fuzzing, benchmarks, `timeout-minutes`, caching, `git ls-files` artifact guard.
- `build*/` pattern missing in `.gitignore` (only `build/`) — sibling branch committed 150 `build_test/` artifacts (still in remote history, ~3.5 MB).
- `compile_commands.json` exists only in `build/`, unreachable from root for clangd; no `.clangd`.
- Local `build/` (4 MB artifacts) and `.cache/` (clangd index) on disk, both gitignored ✓. `.opencode/` (63 MB incl. node_modules) untracked ✓ — commit `b3e2ae9 "add .opencode"` only changed `.gitignore` (misleading message).

---

## 9. README Claims Audit

| Claim | Verdict |
|---|---|
| BodyLength/CheckSum correctness | ✅ true |
| "56 tests" | ✅ true (but CTest says 57) |
| Build options table (9 options) | ✅ true |
| Reactor/epoll edge-triggered | ⚠️ initiator yes; acceptor = busy-poll; fallback = busy-sleep |
| "Lock-free atomic counters" | ⚠️ MemoryStore yes; FileStore = mutex + rewrite per increment |
| "Gap detection / GapFill processing" | ⚠️ implemented, untested, non-conformant |
| "ApplVerID routing" | ⚠️ function exists + tested, zero callers |
| CI "every push" / "GCC 13 + Clang 18" | ❌ stale/false (triggers narrow; uses GCC-14/Clang-19) |
| Hot-reloadable atomic dictionary swap | ❌ no-op stub |
| FileStore crash-safe | ❌ truncate-write, no fsync |
| Audit rotation **and retention** | ❌ rotation broken; retention absent |
| MiFID II / SEC 605/606 compliance | ❌ audit log never written |
| TLS 1.3 ready | ❌ flag only, no OpenSSL code |
| Zero-copy parser, no intermediate allocs | ❌ copies everywhere |
| "production-ready, ultra-reliable" | ❌ unsupported (no e2e test, C1–C13) |
| Usage example | ❌ compiles but transmits nothing |

---

## 10. FIX Spec Gaps (feature completeness)

1. Repeating groups (incl. nested) — absent + destroyed on parse; `MessageDef::groups` never populated; no count-vs-entries validation (`RepeatingGroupCountMismatch`=15 unused).
2. Data/Length pairs (95/96, 93/89, 212/618, 90/91) — absent.
3. Encoding validation (347 MessageEncoding, UTF-8, printable-ASCII) — absent.
4. Session value validation (required tags, enums, ordering) — implemented in dict, **never called**.
5. BeginString/version negotiation, FIXT ApplVerID defaulting — absent.
6. Standard-header TX ordering, PossDup+OrigSendingTime pairing — absent.
7. Logout timeout / heartbeats-while-awaiting-Logout — absent.
8. Sequence wrap handling (2,000,000,000) — absent (SeqNum is uint64, low risk).
9. TLS, Windows transport, multi-session over one listener load-balancing — absent (note: sibling branch has UDP/load-balancer experiments).

---

## 11. Key File:Line Reference Map

| Concern | Location |
|---|---|
| do_send not wired | `src/engine.cpp:99-102` |
| on_connected empty | `src/engine.cpp:87` |
| Session UAF paths | `src/engine.cpp:112-114`, `src/session/session_manager.cpp:22` |
| Event-loop wedge | `src/transport/tcp_transport.cpp:248-272` vs `:200-246` |
| stop() fd race | `src/transport/tcp_transport.cpp:46-63` vs `:213-235` |
| Parser no-resync | `src/parser/parser.cpp:119-120`, `:25-31`, `:142-146` |
| Checksum lax | `src/parser/parser.cpp:105-109,170` |
| set() dedup destroys groups | `include/fix/core/message.hpp:27-35`, `src/parser/parser.cpp:197` |
| SOH injection | `include/fix/parser/serializer.hpp:85-92` |
| CompID/BString not validated | `src/session/session.cpp:148` |
| FileStore truncate-write | `src/store/file_store.cpp:35-50,84-94` |
| Dict load stub | `src/dictionary/data_dictionary.cpp:290-300` |
| Audit never written | `src/engine.cpp:13-20` (construction only) |
| TestRequest timeout bug | `src/session/session.cpp:106-117` vs `:390-391` |
| SeqReset after validation | `src/session/session.cpp:157-163` vs `:296-310` |
| Resend no PossDup | `src/session/session.cpp:288-293` |
| Store Result discarded | `src/session/session.cpp:437`, `:175` |
| Dict validation dead | `src/session/session.cpp:40`; `include/fix/session/session.hpp:36` |
| ~~Deadlock: cb under manager lock~~ fixed by snapshot `for_each` (3.1) | `src/session/session_manager.cpp` (`for_each`), statement in `include/fix/session/session_manager.hpp` |
| Timing races | `src/session/session.cpp:146` vs `:108`, `:218` vs `:101` |
| add_session after start | `src/engine.cpp:40-45` vs `:71-110` |
| Duplicate ctest | `CMakeLists.txt:184,186` |
| Broken install/export | `CMakeLists.txt:201-216` |
| CI branch triggers | `.github/workflows/ci.yml:4-8` |
