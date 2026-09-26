# FIX-Protocol Production-Readiness Plan

> Companion to `docs/codebase-knowledge.md` (analysis, findings, file:line map).
> Status: **PROPOSED — awaiting approval of scope decisions (§7).**
> Date: 2026-09-26

---

## 1. Goal

Take the engine from "compiles, unit tests pass, cannot run a session" to
**production-ready**: spec-conformant FIX session handling, safe under
malformed/adversarial input, thread-safe, crash-safe persistence, honest
documentation, CI-enforced quality gates, and benchmarked hot paths.

**Definition of done**
- [ ] End-to-end handshake + order flow works over a real loopback socket.
- [ ] All P0/Critical + P1/High findings from the knowledge base resolved.
- [ ] Full test suite (unit + e2e + fuzz + concurrency) green under ASan/UBSan/TSAN.
- [ ] README claims match reality; `find_package(FIXEngine)` works.
- [ ] Every phase reviewed by `code-reviewer` agent before commit.

---

## 2. Working agreement (all phases)

1. **Baseline before touching anything**: full build + ctest green (57/57 today).
2. **Test-first where feasible**: each fix lands with a regression test that fails
   before / passes after (especially for C1–C13).
3. **Agent workflow per phase**:
   - `backend-developer` / `fullstack-engineer` implement (parallel, disjoint files)
   - `test-engineer` adds/extends tests for the phase
   - `code-reviewer` reviews the diff → findings addressed → I verify build+tests+sanitizers
   - commit (small, per-concern commits)
4. **Verification commands** each phase:
   ```bash
   cmake --build build --parallel
   cd build && ctest --output-on-failure -j4
   # sanitizer build for phases 1–3, 6:
   cmake -B build-asan -DFIX_ENABLE_ASAN=ON -DFIX_ENABLE_UBSAN=ON && cmake --build build-asan && ctest --test-dir build-asan
   ```
5. **No scope creep**: README truth-up happens only in Phase 6 (except where a
   fix makes a claim false, note it in the commit).

---

## 3. Phase 1 — Make it work (P0 wiring & memory safety)

*Agents: `backend-developer` (engine/transport), `fullstack-engineer` (parser/serializer),
`security-engineer` (review of injection + session-identity validation).*

| ID | Task | Ref |
|---|---|---|
| 1.1 | Wire `do_send` → `ITransport::send` in `Engine::add_session`; pass transport as `shared_ptr`; return `Result` on null transport | C1 |
| 1.2 | Implement `on_connected`: initiator → auto-`logon()`; acceptor → WaitingLogon; verify `SO_ERROR` before firing | C2 |
| 1.3 | Fix event loop: return from `run_event_loop` on `recv()==0`/error; register `EPOLLRDHUP`; acceptor loops back to `accept()`; initiator reconnects with backoff honoring `reconnect_delay` | C3 |
| 1.4 | Session lifetime: sessions as `shared_ptr`, transport callbacks capture `weak_ptr` + `lock()`; duplicate SessionID → error (no replace); `remove_session` stops/erases Connection **first** under `conn_mutex_`; include `qualifier` in `make_key` | C4 |
| 1.5 | fd lifecycle: eventfd/self-pipe to wake epoll; IO thread closes own fds; `stop()` signals + joins only; clear `send_queue_` on disconnect | C5 |
| 1.6 | Parser robustness: tri-state `parse_field` (Ok/Incomplete/Invalid); resync scan to next `8=` bounded by `max_msg_len`; cap `buf_.size()` (disconnect on exceed); skip full body on oversized BodyLength (update `consumed_`); body loop bounded by `body_start+body_length`, require `10=` exactly there; continue `feed()` loop after errors (ET-safe); strict 3-digit checksum ≤255 with `from_chars` checks; check BodyLength `from_chars`; clear `last_error_` on success | C6 |
| 1.7 | Groups: parser appends (never dedups) via `fields().emplace_back`; duplicate detection for header tags → Reject reason 13; add `Message::get_all(tag)` | C7 |
| 1.8 | Serializer: reject values containing SOH/NUL/control bytes in `append_field` + `Message::set` (return error) | C8 |
| 1.9 | Session identity: validate BeginString vs session version, Sender/TargetCompID vs `cfg_.id` → Reject(reason)/disconnect | C9 |
| 1.10 | **Regression test: e2e loopback handshake** (initiator+acceptor on 127.0.0.1, assert wire Logon/LogonResponse bytes) — would catch C1/C2 | — |

**Exit criteria**: loopback handshake green; parser fuzz (garbage, oversized,
truncated, injection) green under ASan/UBSan; injection + hijack tests fail-to-pass.

---

## 4. Phase 2 — FIX session conformance (P1)

*Agents: `backend-developer` (implementation), `lead-engineer` (spec review),
`test-engineer` (protocol scenario tests).*

| ID | Task | Ref |
|---|---|---|
| 2.1 | Sequence handling: `NextTargetMsgSeqNum = received+1`; special-case `MsgType=4` **before** seq validation (both modes, gap-fill rules: PossDup required, `NewSeqNo > expected`); stop incrementing after SequenceReset; `send_sequence_reset` reachable | SeqReset |
| 2.2 | Resend: under `send_mutex_`; GapFill admin messages (`SequenceReset GapFillFlag=Y PossDupFlag=Y`); re-tag app messages `PossDupFlag=Y` + `OrigSendingTime`; read store → release lock → send; reject `BeginSeqNo > NextSender`; `gap_open_` throttle flag | Resend |
| 2.3 | Timers: `test_request_sent_time_`; disconnect at `≥ 1.2×HeartBtInt` after TestRequest; clear `test_req_pending_` on **any** inbound; implement Logout timeout (`logout_timeout`) with heartbeats continuing in LogoutSent; validate `HeartBtInt > 0` (reject/ignore otherwise) | Timer bugs |
| 2.4 | FSM: CAS `transition(from,to)` helper; state gates on `send()`/`logout()`; duplicate Logon → Logout + disconnect; delete dead `LogoutReceived`; wire `Reconnecting` or remove | FSM |
| 2.5 | Reject handling: implement `handle_reject` (terminate handshake on rejected Logon, surface callback); consume seq + valid `RefSeqNum` when rejecting missing MsgType | `handle_reject` |
| 2.6 | Error propagation: `do_send` → `Result<void>`; check `store_outbound` **before** incrementing seq; `Session::send` returns real error; exception boundaries at `on_timer`/transport/`stop()`; replace silent catch with `on_error` callback + counter; advance seq **before** dispatch (scope guard) so throwing user cb can't cause resend storms | Store/errors |
| 2.7 | Dictionary validation wired: call `dict_->validate()` on inbound app messages when `validate_fields` (default ON for app msgs), emit session Reject / BusinessMessageReject appropriately | Dict dead |
| 2.8 | `handle_logon`: validate `ResetSeqNumFlag` gating (logon states only), require HeartBtInt, echo-check initiator | Logon |

**Exit criteria**: protocol scenario tests green: gap→ResendRequest→GapFill
(admin + app), SequenceReset both modes, duplicate logon, TestRequest timeout
at 1.2×, logout timeout, store-failure aborts send without seq burn.

---

## 5. Phase 3 — Concurrency & storage hardening

*Agents: `low-latency-engineer` (lock/perf-aware changes), `security-engineer`
(race/FD review), `test-engineer` (TSAN stress tests).*

| ID | Task |
|---|---|
| 3.1 | Document + enforce lock order `manager → recv → send → store → transport`; **never** hold manager/store/send lock across user callbacks or I/O (snapshot session list under lock, release, iterate) |
| 3.2 | Make timing/test-request state `std::atomic` (or move all session mutations onto one strand — decide in §7); fix store seq compound ops; `state_` CAS transitions |
| 3.3 | Transport: level-triggered `EPOLLOUT` (or drain queue from `send()` when writable) for cross-thread queue; `SO_KEEPALIVE`; non-blocking DNS (resolve before loop or async); send errors surfaced via `on_error` |
| 3.4 | FileStore crash-safety: seq persistence via write-tmp + `fsync` + `rename` + dir fsync; validate `load_seqs` (fallback = refuse to start, not silent reset); `fsync` on message append (configurable); propagate write errors; include `qualifier` + sanitized CompIDs in filenames; explicit `close()` |
| 3.5 | Audit log: actually write on every RX/TX (wire into `process_message`/`send_message`); fix `rotate()` (timestamp at rotation, not reopen); implement retention (`retain_days`); bounded queue with drop counter |
| 3.6 | TSAN stress tests: multi-thread send + traffic + add/remove session; FileStore parallel; audit flush/rotate race |
| 3.7 | `SIGPIPE` handling (`MSG_NOSIGNAL` where absent, `SO_NOSIGPIPE`/signal ignore otherwise) |

**Exit criteria**: TSAN clean under stress; kill -9 FileStore recovery test
(seq not reset); no lock held across user callback (asserted in review).

---

## 6. Phase 4 — Dictionary & validation depth

*Agents: `backend-developer`, `researcher` (FIX XML schema), `test-engineer`.*

| ID | Task | Note |
|---|---|---|
| 4.1 | Decide XML loader vs expanded tables (§7 decision 2) | |
| 4.2 | If XML: parse QuickFIX-style `FIX42/44/50SP2.xml` (fields, components, groups, enums, messages) with `std::string_view`-based lightweight parser; build tables off-line, swap atomically via `shared_ptr<const Tables>` | |
| 4.3 | If tables: expand to full core-trading message set (NewOrderSingle, ExecutionReport, OrderCancel*, OrderCancelReject, MarketData*, BusinessMessageReject, etc.) + their repeating groups + per-version enum tables (TimeInForce/OrdStatus/ExecType correct per version) | |
| 4.4 | Either way: populate `MessageDef::groups`; validate group counts (`RepeatingGroupCountMismatch`); fix `load_builtin` unlocked mutation (lock or build-then-swap); `load()` returns error for missing/unparsed file; fix test that codifies the stub | |
| 4.5 | Dictionary registry: return `shared_ptr<const DataDictionary>`; Session holds `shared_ptr`; remove dead `Engine::dicts_` | |
| 4.6 | Wire `resolve_appl_ver_id` for FIXT 1.1 routing (or document as future work) | |

**Exit criteria**: validation runs in session (test: malformed NewOrderSingle →
BusinessMessageReject); no false rejects for valid 4.4 enums; hot-reload test
actually swaps tables atomically under concurrent readers (TSAN).

---

## 7. Phase 5 — Performance (hot path)

*Agents: `low-latency-engineer` (lead), `backend-developer`, `test-engineer` (benchmarks).*

| ID | Task |
|---|---|
| 5.1 | `Message`: tag→slot flat hash index (or sorted-once + binary search) → O(1) `get`/`set`; kill O(n²); append-only group storage |
| 5.2 | Parser: incremental parse state (remember pos/state between feeds — kill quadratic re-parse); `reserve` instead of zero-filling `resize`; copy-on-demand `raw_` (only when resend needs it); optional field value SSO strategy review |
| 5.3 | Serializer: single-pass build into one reserved buffer with streaming checksum (eliminate 3-4 full copies); proper standard-header partition (also fixes TX ordering finding) |
| 5.4 | Numeric parsing: `from_chars` for doubles (fix locale bug + allocation), checked `ec`/`ptr` everywhere; fix `set(tag,int-literal)` ambiguity; precision param for `set(double)` |
| 5.5 | Benchmarks: Google Benchmark targets (parse MB/s, serialize ns/msg, end-to-end loopback msgs/s + p50/p99 latency); record numbers + hardware in README |
| 5.6 | Remove dead zero-copy types (`RawField`,`FieldValue`) or implement view path with documented lifetime; fix misleading comments |

**Exit criteria**: no regression in correctness suites; benchmark numbers
recorded; allocations per message measured before/after (via
`-fsanitize=address` counting or `perf`/massif-style spot check).

---

## 8. Phase 6 — Tests, CI, build & docs (quality gates)

*Agents: `test-engineer` (coverage), `pipeline-engineer`/`devops-engineer` (CI),
`code-reviewer` (final audit), `lead-engineer` (sign-off).*

| ID | Task |
|---|---|
| 6.1 | **Tests**: fill gaps — e2e handshake (from 1.10, extend to order flow), resend/gap, SequenceReset both modes, duplicate logon, timers (injectable clock), disconnect/reconnect, `remove_session` under traffic, CompID rejection, malformed-input table, FileStore crash-recovery, concurrency stress, `Engine` lifecycle (start-after-add, double start/stop) |
| 6.2 | **Fuzzing**: libFuzzer target for `StreamParser::feed` + round-trip serializer (seed corpus from real messages); wire into CI as time-boxed job |
| 6.3 | **CMake**: remove duplicate `add_test`; proper export (`EXPORT`, `configure_package_config_file`, install version file) + install-and-consume CI smoke test; `target_compile_features(cxx_std_23)`; `FIX_WARNINGS_AS_ERRORS` option + expanded warnings (`-Wconversion -Wshadow -Wformat=2 -Wnull-dereference…`); guard ASAN+TSAN combo; LTO target-scoped; cache default build type; `CONFIGURE_DEPENDS` glob; prefer `clang-format-18`; `PROJECT_IS_TOP_LEVEL` guards; drop unused spdlog find/link; PRIVATE `atomic` w/ feature check |
| 6.4 | **CI**: add `refactor` (or all) branches to triggers; clang-tidy job (+`.clang-tidy`); coverage job (gcovr); fuzz job; benchmark smoke; `timeout-minutes` on all jobs; ccache/actions caching; pin FetchContent to SHA; `apt-get update` in format job; artifact-tracked-in-git guard |
| 6.5 | **Repo hygiene**: `.gitignore` add `build*/`, `CMakeUserPresets.json`, runtime dirs; `.clangd` (`CompilationDatabase: build`) + symlink note; root `compile_commands` docs; `.editorconfig`; remove dead declarations (`timer_loop`, `build_header_fields`, dead config fields — or implement them: `reconnect_delay` done in 1.3, `logout_timeout` in 2.3) |
| 6.6 | **README rewrite**: correct every false claim (§9 knowledge base) — implemented features only, unimplemented → Roadmap (TLS, Windows transport, MiFID II, XML hot-reload if deferred); real test count; real CI compilers/trigger scope; benchmark section; architecture diagram reflecting real flow |
| 6.7 | **Security review pass**: `security-engineer` re-verifies injection, hijack, fuzz results, DoS caps post-fix |

**Exit criteria**: CI green on this branch with new jobs; coverage of `src/`
materially up (target: all TUs >80% line); README audit clean; `find_package`
smoke test passes.

---

## 9. Agent assignment summary

| Phase | Lead agents | Reviewers |
|---|---|---|
| 1 P0 wiring/safety | `backend-developer`, `fullstack-engineer` | `security-engineer`, `code-reviewer` |
| 2 conformance | `backend-developer`, `test-engineer` | `lead-engineer` (spec), `code-reviewer` |
| 3 concurrency/storage | `low-latency-engineer`, `test-engineer` | `security-engineer`, `code-reviewer` |
| 4 dictionary | `backend-developer`, `researcher` | `test-engineer`, `code-reviewer` |
| 5 performance | `low-latency-engineer`, `test-engineer` | `code-reviewer` (bench sanity) |
| 6 tests/CI/docs | `test-engineer`, `pipeline-engineer`, `devops-engineer` | `code-reviewer`, `lead-engineer` |

I (coordinating agent) run verification after every phase and keep this plan updated.

---

## 10. Open decisions (blocking start)

1. **Scope** — run all 6 phases, or stop after Phases 1–3 (P0/P1) for review?
   *Recommendation: all 6, checkpoint after Phase 3.*
2. **Dictionary strategy (Phase 4)** —
   (a) real FIX XML loader (correct, larger effort) vs
   (b) expanded hand-written core tables + wired validation (faster, good enough for trading core).
   *Recommendation: (b) now, XML loader as Phase 7 later.* Note: either way the
   stub `load()` must stop reporting success.
3. **TLS & Windows transport** — implement, or de-scope (remove claims, document
   POSIX-only)? *Recommendation: de-scope now → Roadmap.*
4. **Threading model** — (a) incremental: atomics + lock-order fixes (lower risk)
   vs (b) full single-strand-per-session rework (cleaner, bigger change).
   *Recommendation: (a) in Phase 3, revisit if TSAN stress reveals deeper issues.*
5. **Branch** — commit on `refactor`, or new branch (e.g. `production-hardening`)?
   *Recommendation: new branch off `refactor` so phases are reviewable as PRs.*

## 11. Out of scope (this effort)

- TLS/OpenSSL implementation, Windows socket transport (→ Roadmap)
- Full FIX Repository dictionary coverage for all 4.2/4.4/5.0 messages (~100+)
- Kernel-bypass/io_uring networking, DPDK (not needed for this tier)
- History rewrite of sibling-branch build artifacts (documented only)
