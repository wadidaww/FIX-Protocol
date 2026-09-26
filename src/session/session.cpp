// =============================================================================
// FIX Protocol Engine - Session implementation
//
// Phase 2 (tasks 2.1-2.8): FIX session-layer conformance.
//   2.1  SequenceReset special-cased before seq validation (both modes),
//        no increment after SequenceReset, gap-fill PossDup/NewSeqNo rules.
//   2.2  ResendRequest replay under send_mutex_: store snapshot first, then
//        gap-fill admin runs, re-tag app messages (43=Y + 122), reject
//        BeginSeqNo > NextSender, gap_open_ throttling.
//   2.3  TestRequest deadline measured from test_req_sent_time_ (1.2x hb),
//        clear pending on ANY inbound, logout_timeout with heartbeats while
//        LogoutSent, HeartBtInt validation/adoption/echo.
//   2.4  CAS transition(), state gates on send()/logout(), duplicate Logon
//        -> Logout + disconnect, dead states removed.
//   2.5  handle_reject (Logon handshake termination + on_reject), missing
//        MsgType Reject consumes the seq with a valid RefSeqNum.
//   2.6  store_outbound checked before seq increment, exception boundaries,
//        on_error callback + counter, advance guaranteed around dispatch.
//   2.7  DataDictionary::validate on inbound app messages -> 35=j.
//   2.8  handle_logon: HeartBtInt required, initiator echo check, reset
//        gating in logon states only.
// =============================================================================
#include "fix/session/session.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fix {

std::string_view to_string(SessionState s) noexcept {
    switch (s) {
    case SessionState::NotConnected:
        return "NotConnected";
    case SessionState::WaitingLogon:
        return "WaitingLogon";
    case SessionState::LogonSent:
        return "LogonSent";
    case SessionState::Active:
        return "Active";
    case SessionState::LogoutSent:
        return "LogoutSent";
    case SessionState::Disconnected:
        return "Disconnected";
    }
    return "Unknown";
}

Session::Session(SessionConfig cfg, std::unique_ptr<IMessageStore> store,
                 const DataDictionary *dict, SessionCallbacks cbs)
    : cfg_(std::move(cfg)),
      store_(std::move(store)),
      dict_(dict),
      cbs_(std::move(cbs)) {
    // HeartBtInt = 0 would arm a Heartbeat on every timer tick (bandwidth
    // storm); floor the configured interval at 1 s. (2.3/2.8)
    cfg_.heartbeat_interval = std::max(1, cfg_.heartbeat_interval);
    heartbeat_sec_.store(cfg_.heartbeat_interval, std::memory_order_relaxed);
    const Millis t = now_ms();
    last_send_time_ms_.store(t, std::memory_order_relaxed);
    last_recv_time_ms_.store(t, std::memory_order_relaxed);
    logon_sent_time_ms_.store(t, std::memory_order_relaxed);
}

Session::~Session() = default;

Session::Millis Session::now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}

Session::Millis Session::clock_now() const {
    auto fn = clock_.load(std::memory_order_acquire);
    if (fn && *fn)
        return (*fn)();
    return now_ms();
}

void Session::set_time_source_for_test(std::function<Millis()> clock) {
    clock_.store(std::make_shared<const ClockFn>(std::move(clock)), std::memory_order_release);
    // Re-baseline every deadline against the new source: the previous values
    // are wall-clock instants that would look wildly far off relative to a
    // test clock, firing (or never firing) timers immediately. Message
    // SendingTime keeps using the real clock — the wire stays spec-conformant.
    const Millis now = clock_now();
    last_send_time_ms_.store(now, std::memory_order_relaxed);
    last_recv_time_ms_.store(now, std::memory_order_relaxed);
    logon_sent_time_ms_.store(now, std::memory_order_relaxed);
    test_req_sent_time_ms_.store(now, std::memory_order_relaxed);
    logout_sent_time_ms_.store(now, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Session::on_data(const char *data, std::size_t len) {
    bool healthy = false;
    Message msg;
    {
        std::lock_guard lock(parser_mtx_);
        parser_.feed(data, len);
        healthy = parser_.healthy();
    }
    if (!healthy) {
        // F10: the parser latched a fatal condition (an un-reclaimable
        // backlog). Surface it by dropping the connection — disconnect()
        // closes the transport AND resets the parser, so a reconnect starts
        // from a clean slate instead of a wedged, dropping parser.
        disconnect();
        return;
    }
    while (true) {
        {
            std::lock_guard lock(parser_mtx_);
            if (!parser_.next(msg))
                break;
        }
        // F14: stop dispatching once the session is torn down — e.g. an
        // identity mismatch on message 1 must not process message 2 from the
        // same batch.
        if (state_.load(std::memory_order_acquire) == SessionState::Disconnected)
            break;
        try {
            process_message(msg);
        } catch (const std::exception &e) {
            // 2.6: the old catch was empty — store/callback failures vanished.
            // Surface on on_error and keep the connection: one bad message
            // must not kill the session. (Callback throws are already caught
            // inside process_message's guarded dispatch so the sequence
            // advance still runs; this catches everything else.)
            report(e.what());
        } catch (...) {
            report("unknown exception while processing a message");
        }
    }
}

void Session::on_transport_connected() {
    // Fresh byte stream: any partial parser state or stale timer flags left
    // over from the previous connection are meaningless.
    {
        std::lock_guard lock(parser_mtx_);
        parser_.reset();
    }
    test_req_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(test_req_mtx_);
        pending_test_req_id_.clear();
    }
    clear_gap(); // throttle + F9 retry budget of the previous connection
    const Millis now = clock_now();
    last_send_time_ms_.store(now, std::memory_order_relaxed);
    last_recv_time_ms_.store(now, std::memory_order_relaxed);
    logon_sent_time_ms_.store(now, std::memory_order_relaxed);
    logout_sent_time_ms_.store(now, std::memory_order_relaxed);

    if (cfg_.initiator) {
        // A dropped connection can leave the FSM in LogonSent/Active/LogoutSent
        // while logon() only accepts NotConnected/Disconnected — normalise
        // first so the initiator always re-logons on reconnect. Runs on the
        // transport IO thread (the only thread that transitions the session
        // onto a fresh connection).
        auto st = state_.load(std::memory_order_acquire);
        if (st != SessionState::NotConnected && st != SessionState::Disconnected) {
            state_.store(SessionState::Disconnected, std::memory_order_release);
        }
        (void)logon();
    } else {
        // Acceptor: only WaitingLogon accepts an inbound Logon.
        state_.store(SessionState::WaitingLogon, std::memory_order_release);
    }
}

Result<void> Session::send(Message msg) {
    // 2.4: gate on Active — a disconnected session must not serialise and
    // store messages it can never transmit (phantom sequence numbers).
    if (state_.load(std::memory_order_acquire) != SessionState::Active)
        return make_unexpected(ErrorCode::SessionError);
    return send_message(msg);
}

Result<void> Session::send_raw(const std::string &raw) {
    // Raw escape hatch: bytes are already framed, no seq management. Kept
    // ungated (engine tests use it to inject an exact TestRequest frame).
    std::lock_guard lock(send_mutex_);
    if (cbs_.do_send)
        cbs_.do_send(raw);
    last_send_time_ms_.store(clock_now(), std::memory_order_relaxed);
    ++msgs_sent_;
    return {};
}

Result<void> Session::logon() {
    auto st = state_.load(std::memory_order_acquire);
    if (st != SessionState::NotConnected && st != SessionState::Disconnected)
        return make_unexpected(ErrorCode::SessionError);
    // Propagates the real failure (store reset / serialisation / persist) so
    // the caller never believes a Logon went out when none did (F1/F6).
    return send_logon();
}

Result<void> Session::logout(std::string_view reason) {
    auto st = state_.load(std::memory_order_acquire);
    if (st != SessionState::Active && st != SessionState::LogonSent &&
        st != SessionState::WaitingLogon)
        return make_unexpected(ErrorCode::SessionError);
    send_logout(reason);
    return {};
}

void Session::on_timer() {
    // 2.6 exception boundary: deliberately NONE inside Session — the engine's
    // timer loop wraps each on_timer() call (its backstop reports
    // ErrorCode::SessionError on EngineConfig::on_error). Catching here would
    // hide failures from that boundary. Unit tests drive on_timer() directly
    // with non-throwing callbacks.
    const auto st = state_.load(std::memory_order_acquire);
    const auto n = clock_now();
    // Timer-side reads go through the atomic mirror — cfg_.heartbeat_interval
    // is written by the IO thread in handle_logon (F2).
    const auto hb_ms = static_cast<Millis>(heartbeat_sec_.load(std::memory_order_relaxed)) * 1000;

    if (st == SessionState::Active) {
        // Heartbeat: nothing sent for a full interval → keep the pipe alive.
        if (n - last_send_time_ms_.load(std::memory_order_relaxed) >= hb_ms)
            send_heartbeat();

        if (!test_req_pending_.load(std::memory_order_acquire)) {
            // 2.3: probe after one silent interval.
            if (n - last_recv_time_ms_.load(std::memory_order_relaxed) >= hb_ms)
                send_test_request();
        } else {
            // 2.3: the deadline is 1.2×HeartBtInt measured from when WE sent
            // the TestRequest — the old code timed out on the next 200 ms
            // tick after sending it (last_recv-based), i.e. instantly.
            const auto deadline_ms = hb_ms + hb_ms / 5;
            if (n - test_req_sent_time_ms_.load(std::memory_order_relaxed) >= deadline_ms) {
                // Deliberately NOT guarded: on_timer's exception boundary IS
                // the engine's timer loop (2.6), which reports the failure on
                // EngineConfig::on_error. Swallowing here would hide it — and
                // a throw skips disconnect() only until the next tick, when
                // the still-pending deadline fires the callback again.
                if (cbs_.on_heartbeat_timeout)
                    cbs_.on_heartbeat_timeout(cfg_.id);
                disconnect();
            }
        }

        // F9: an open gap must NOT rely on the single ResendRequest that
        // request_gap() fired — a peer that lost those bytes (or restarted)
        // left a zombie session: Active forever, exactly one RR ever sent.
        // Re-arm every 1.2×HeartBtInt (same spacing as the TestRequest
        // deadline) while the gap stays open; the spacing itself is the
        // anti-flood guarantee, so nothing goes out between retries. After
        // kMaxGapRetries total attempts (initial + retries) give up loudly:
        // report on on_error and disconnect rather than hang. Cleared
        // together with gap_open_ wherever the gap closes (clear_gap()).
        if (gap_open_.load(std::memory_order_acquire)) {
            const auto retry_ms = hb_ms + hb_ms / 5; // 1.2 × HeartBtInt
            if (n - gap_rr_last_ms_.load(std::memory_order_relaxed) >= retry_ms) {
                const int attempt = gap_rr_attempts_.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (attempt > kMaxGapRetries) {
                    report("sequence gap: ResendRequest retry budget exhausted "
                           "(kMaxGapRetries); disconnecting");
                    disconnect();
                    return;
                }
                gap_rr_last_ms_.store(n, std::memory_order_relaxed);
                send_resend_request(store_->next_target_seq_num(), 0);
            }
        }
        return;
    }

    if (st == SessionState::LogonSent) {
        const auto timeout_ms = static_cast<Millis>(cfg_.logon_timeout) * 1000;
        if (n - logon_sent_time_ms_.load(std::memory_order_relaxed) >= timeout_ms)
            disconnect();
        return;
    }

    if (st == SessionState::LogoutSent) {
        // 2.3: logout_timeout was dead config — a peer that never answered a
        // Logout hung the session forever. Enforce it, and keep heartbeats
        // flowing while we wait so the peer still sees a live session.
        const auto timeout_ms = static_cast<Millis>(cfg_.logout_timeout) * 1000;
        if (n - logout_sent_time_ms_.load(std::memory_order_relaxed) >= timeout_ms) {
            disconnect();
        } else if (n - last_send_time_ms_.load(std::memory_order_relaxed) >= hb_ms) {
            send_heartbeat();
        }
        return;
    }

    // WaitingLogon: the acceptor waits indefinitely for the peer's Logon — it
    // initiated nothing, so there is no deadline to enforce. NotConnected /
    // Disconnected: nothing to drive.
}

void Session::disconnect() {
    state_.store(SessionState::Disconnected, std::memory_order_release);

    // The TestRequest round trip can never complete on a dead connection, and
    // an open gap throttle would suppress the first ResendRequest of the next
    // connection (its retry budget goes with it — F9).
    test_req_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(test_req_mtx_);
        pending_test_req_id_.clear();
    }
    clear_gap();
    // Drop partial frames / re-arm a parser that latched unhealthy so a
    // reconnect starts clean (also required by F10).
    {
        std::lock_guard lock(parser_mtx_);
        parser_.reset();
    }
    // Close the byte stream (F1). This half was missing: the engine wires
    // do_disconnect to ITransport::disconnect(), so without this call a
    // CompID-mismatched peer kept the socket open forever. Safe to invoke
    // from the transport's on_disconnected callback: by then
    // TcpTransport::close_connection() has already run, so disconnect()
    // sees conn_fd_ < 0 and no-ops — the callback chain terminates after one
    // hop instead of looping.
    if (cbs_.do_disconnect)
        cbs_.do_disconnect();
}

Result<void> Session::reset() {
    state_.store(SessionState::NotConnected, std::memory_order_release);
    {
        std::lock_guard lock(parser_mtx_);
        parser_.reset();
    }
    test_req_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(test_req_mtx_);
        pending_test_req_id_.clear();
    }
    clear_gap();
    if (store_)
        return store_->reset();
    return {};
}

// ---------------------------------------------------------------------------
// Message processing
// ---------------------------------------------------------------------------
void Session::process_message(const Message &msg) {
    // 2.3: ANY inbound traffic proves the peer is alive — clear the pending
    // TestRequest unconditionally. The old TestReqID-only match left the flag
    // stuck after any other inbound message, arming a spurious disconnect in
    // the next silent period.
    last_recv_time_ms_.store(clock_now(), std::memory_order_relaxed);
    test_req_pending_.store(false, std::memory_order_release);

    // --- Session identity validation (security) -----------------------------
    // BeginString must be present, parseable, and match this session's version.
    if (msg.begin_string_version() != cfg_.id.version) {
        send_logout("BeginString does not match session");
        disconnect();
        return;
    }
    // The counterparty must identify itself with our target CompID and address
    // us with our sender CompID. Anything else is a protocol violation and a
    // potential session-hijack attempt: terminate without processing the body.
    auto sender = msg.get(tags::SenderCompID);
    auto target = msg.get(tags::TargetCompID);
    if (!sender || !target || *sender != cfg_.id.targetCompID || *target != cfg_.id.senderCompID) {
        send_logout("CompID mismatch");
        disconnect();
        return;
    }

    // --- 2.5: missing MsgType ----------------------------------------------
    if (!msg.has(tags::MsgType)) {
        // Consume BEFORE rejecting, and only when this really is the next
        // message: rejecting while leaving the sequence hole in place makes
        // the peer resend, we reject again — a permanent reject loop. This
        // path runs BEFORE sequence validation, so unlike the dispatch
        // advance it additionally demands the exact expected MsgSeqNum: a
        // garbage frame claiming a future seq must not fast-forward our
        // numbering (the `> current` guard would do exactly that). The
        // RefSeqNum on the Reject is always present (F12).
        auto st = state_.load(std::memory_order_acquire);
        const bool seq_validated = st != SessionState::WaitingLogon &&
                                   st != SessionState::LogonSent &&
                                   st != SessionState::NotConnected;
        if (seq_validated && !msg.poss_dup() && msg.seq_num() == store_->next_target_seq_num()) {
            store_->incr_target_seq_num();
            store_->store_inbound(msg.seq_num(), msg.raw());
        }
        send_reject(msg.seq_num(), SessionRejectReason::RequiredTagMissing, "", tags::MsgType,
                    "Missing MsgType");
        ++msgs_received_;
        return;
    }

    const auto mt = msg.msg_type();

    // --- 2.1: MsgType=4 special-cased BEFORE sequence validation ------------
    // validate_seq_num would judge a reset-mode SequenceReset "too low, not
    // PossDup" and Logout before handle_sequence_reset ever ran, and any
    // advance after dispatch would leave NextTarget = NewSeqNo+1
    // (off-by-one). handle_sequence_reset owns the rules for BOTH modes.
    if (mt == msg_types::SequenceReset) {
        if (handle_sequence_reset(msg))
            ++msgs_received_;
        return; // never advance/store: NewSeqNo IS the sequence state (2.1)
    }

    // --- Sequence validation ------------------------------------------------
    auto st = state_.load(std::memory_order_acquire);
    const bool seq_validated = st != SessionState::WaitingLogon && st != SessionState::LogonSent &&
                               st != SessionState::NotConnected;
    if (seq_validated) {
        if (!validate_seq_num(msg))
            return;
        // The gap is closed the moment a message lands on the expected seq
        // (throttle + F9 retry budget go with it).
        clear_gap();
    }

    // --- Duplicate standard-header tags (F7) --------------------------------
    if (msg.has_duplicate_header()) {
        send_reject(msg.seq_num(), SessionRejectReason::TagAppearsMoreThanOnce,
                    std::string(msg.msg_type()), 0, "Tag appears more than once");
        // Gate == the dispatch advance's (F3): a session-level Reject
        // consumes exactly the slot an accepted copy of the same message
        // would. The old `seq_validated && !poss_dup` variant rejected an
        // in-sequence PossDup in Active state without ever consuming it —
        // the peer's next (correct) message then looked like a gap, we
        // re-requested, it re-sent, we re-rejected: a permanent loop.
        consume_inbound(msg, seq_validated);
        ++msgs_received_;
        return;
    }

    // --- Dispatch -----------------------------------------------------------
    // Guarded so a throwing user callback cannot skip the advance below (2.6:
    // otherwise the peer's resend of that very message loops forever).
    guarded([&] {
        if (is_admin_msg(mt))
            process_admin(msg);
        else
            process_app(msg);
    });

    // --- Advance (2.1 NextTarget = received+1; 2.6 guaranteed) --------------
    // Runs AFTER dispatch so a reset-Logon's own baseline (target := 1) is
    // consumed exactly once → 2 — the proven ordering the old code had.
    consume_inbound(msg, seq_validated);
    ++msgs_received_;
}

void Session::process_admin(const Message &msg) {
    auto mt = msg.msg_type();
    if (mt == msg_types::Logon)
        handle_logon(msg);
    else if (mt == msg_types::Logout)
        handle_logout(msg);
    else if (mt == msg_types::Heartbeat)
        handle_heartbeat(msg);
    else if (mt == msg_types::TestRequest)
        handle_test_request(msg);
    else if (mt == msg_types::ResendRequest)
        handle_resend_request(msg);
    else if (mt == msg_types::SequenceReset)
        // Defensive: normally intercepted by process_message before sequence
        // validation (2.1). Kept so a future reorder cannot silently drop a
        // SequenceReset, which would wedge the sequence state.
        (void)handle_sequence_reset(msg);
    else if (mt == msg_types::Reject)
        handle_reject(msg);
}

void Session::process_app(const Message &msg) {
    if (state_.load(std::memory_order_acquire) != SessionState::Active) {
        // Reject app messages when not active
        send_reject(msg.seq_num(), SessionRejectReason::Other, std::string(msg.msg_type()), 0,
                    "Session not active");
        return;
    }
    // 2.7: dictionary validation of inbound application messages. Only when a
    // dictionary exists (Session ctor arg or the engine-loaded registry) —
    // with no dictionary there is nothing to validate against. MsgTypes the
    // dictionary does not define are skipped: validation would only produce
    // UnknownMessage noise for legitimately-unknown traffic (the builtin
    // dictionary covers ~15 of the ~80 message types in FIX 4.2).
    if (cfg_.validate_fields) {
        if (const DataDictionary *dict = resolve_dictionary()) {
            if (dict->find_message(msg.msg_type())) {
                auto ok = dict->validate(msg);
                if (!ok) {
                    // 2.7: an application-message validation failure is a
                    // business rejection (35=j); session Reject (35=3) stays
                    // reserved for protocol violations. The message is never
                    // dispatched.
                    // F11: BusinessRejectReason (380) = 0 ("Other") — the old
                    // code sent 5, which the 4.2/4.4 tables define as
                    // "Duplicate", not as a generic "business reject", so the
                    // counterparty was told the wrong reason. There is no
                    // 380 code for "dictionary validation failed", so the
                    // specifics travel in Text (58) below (which the old call
                    // also passed but which 5 had masked).
                    send_business_reject(msg.seq_num(), 0, msg.msg_type(), ok.error().message());
                    return;
                }
            }
        }
    }
    // process_message's guarded dispatch wraps this call.
    if (cbs_.on_message)
        cbs_.on_message(cfg_.id, msg);
}

// ---------------------------------------------------------------------------
// Admin message handlers
// ---------------------------------------------------------------------------
void Session::handle_logon(const Message &msg) {
    auto st = state_.load(std::memory_order_acquire);

    // --- HeartBtInt (2.8) ---------------------------------------------------
    // FIX 4.2 mandates HeartBtInt on Logon; a value of 0 (or a missing one)
    // previously produced Heartbeat-every-tick or silently kept a stale
    // interval. Reject the handshake instead.
    auto hb_opt = msg.get_int(tags::HeartBtInt);
    if (!hb_opt || *hb_opt <= 0) {
        send_logout("HeartBtInt required");
        disconnect();
        return;
    }

    // --- Initiator echo check (2.8) -----------------------------------------
    // The acceptor must confirm the interval WE proposed — two different
    // liveness schedules would each wait for the other's traffic.
    if (cfg_.initiator && st == SessionState::LogonSent &&
        *hb_opt != heartbeat_sec_.load(std::memory_order_acquire)) {
        send_logout("HeartBtInt mismatch");
        disconnect();
        return;
    }

    // Adopt the peer's interval (2.8). cfg_.heartbeat_interval is written on
    // the IO thread; the timer thread reads the atomic mirror (F2). Must run
    // BEFORE send_logon so an acceptor's response carries the adopted value.
    cfg_.heartbeat_interval = static_cast<int>(*hb_opt);
    heartbeat_sec_.store(static_cast<int>(*hb_opt), std::memory_order_release);

    // --- ResetSeqNumFlag gating (2.8: logon states only) --------------------
    // An inbound 141=Y re-baselines what we EXPECT FROM THE PEER, and only
    // during the handshake; process_message's advance then consumes this
    // Logon, so the next expected becomes received+1 (= 2 for a fresh peer).
    // Our own numbering is NEVER rewound on receipt: the old code reset the
    // sender here too, reusing the sequence our own Logon had already burned,
    // and the peer then rejected our first app message as "MsgSeqNum too
    // low". cfg_.reset_on_logon is likewise NOT an inbound effect — it marks
    // OUR outbound Logon, and send_logon() applies it by calling
    // store_->reset() BEFORE the frame is numbered (F1), never here.
    const bool reset_flag = msg.get(tags::ResetSeqNumFlag).value_or("N") == "Y";
    if (reset_flag && (st == SessionState::WaitingLogon || st == SessionState::LogonSent)) {
        store_->set_next_target_seq_num(1);
    }

    // Whatever gap we were chasing is superseded by a completed handshake
    // (throttle + F9 retry budget).
    clear_gap();

    if (st == SessionState::WaitingLogon) {
        // Acceptor: respond with our Logon (adopted HeartBtInt), go Active.
        // F6: on_logon may only fire for a handshake that actually completed
        // — a failed Logon send or a concurrent teardown (CAS miss) would
        // otherwise surface as a logon for a dead session.
        const bool sent = send_logon().has_value();
        if (sent && transition(SessionState::WaitingLogon, SessionState::Active)) {
            if (cbs_.on_logon)
                guarded([&] { cbs_.on_logon(cfg_.id); });
        } else {
            report(sent ? "logon race: session left WaitingLogon before Active was published"
                        : "logon race: Logon response failed to send; on_logon suppressed");
        }
    } else if (st == SessionState::LogonSent) {
        // Initiator: the acceptor answered our Logon.
        if (transition(SessionState::LogonSent, SessionState::Active)) {
            if (cbs_.on_logon)
                guarded([&] { cbs_.on_logon(cfg_.id); });
        } else {
            // The timer thread hit logon_timeout, or the transport tore the
            // session down between our state read and this CAS (F6).
            report("logon race: session left LogonSent before Active was published; "
                   "on_logon suppressed");
        }
    } else if (st == SessionState::Active) {
        // Duplicate Logon (spec: Logout + disconnect). Reset gating is
        // logon-states only (2.8), so 141=Y does not excuse a mid-session
        // duplicate: the peer must not re-handshake over a live session.
        send_logout("Duplicate Logon");
        disconnect();
    }
    // NotConnected / LogoutSent / Disconnected: a stale Logon — ignore.
}

void Session::handle_logout(const Message &msg) {
    auto reason = msg.get(tags::Text).value_or("");
    auto st = state_.load(std::memory_order_acquire);

    if (st == SessionState::LogoutSent) {
        // Mutual logout complete. CAS: if the timer thread already timed the
        // Logout out, the state is Disconnected and stays there.
        (void)transition(SessionState::LogoutSent, SessionState::Disconnected);
    } else {
        // Peer initiated: echo the Logout WITHOUT entering LogoutSent (the
        // echo helper only writes the message), then DROP THE CONNECTION —
        // disconnect() stores Disconnected AND invokes do_disconnect (F2).
        // The old code stopped at the state store: on_timer has no work for
        // Disconnected, so the peer's FD stayed pinned forever (probe:
        // disconnect_called == false).
        // Re-entrancy is safe: the transport's own on_disconnected callback
        // re-enters disconnect() only after close_connection() has run
        // (conn_fd_ < 0 → no-op), so the chain terminates after one hop.
        // The intermediate LogoutReceived state was deleted in 2.4 — it only
        // created a window for the timer thread to run on a half-torn-down
        // session.
        send_logout_echo();
        disconnect();
    }
    // Exactly once: neither branch above fires on_logout (disconnect() only
    // raises do_disconnect), so this is the single invocation.
    if (cbs_.on_logout)
        guarded([&] { cbs_.on_logout(cfg_.id, reason); });
}

void Session::handle_heartbeat(const Message &msg) {
    // Liveness itself is already satisfied by process_message clearing
    // test_req_pending_ on any inbound (2.3); the strict TestReqID match is
    // kept as the precise path for the case where the flag was re-armed
    // between the inbound arriving and this dispatch.
    auto test_id = msg.get(tags::TestReqID).value_or("");
    if (!test_req_pending_.load(std::memory_order_acquire))
        return;
    // test_req_mtx_ only ever covers the ID string — never a send or callback.
    std::lock_guard lock(test_req_mtx_);
    if (test_id == pending_test_req_id_) {
        pending_test_req_id_.clear();
        test_req_pending_.store(false, std::memory_order_release);
    }
}

void Session::handle_test_request(const Message &msg) {
    auto test_id = msg.get(tags::TestReqID).value_or("TEST");
    send_heartbeat(test_id);
}

void Session::handle_resend_request(const Message &msg) {
    auto begin_opt = msg.get_int(tags::BeginSeqNo);
    auto end_opt = msg.get_int(tags::EndSeqNo);
    if (!begin_opt)
        return; // malformed: nothing to replay

    const SeqNum begin = static_cast<SeqNum>(*begin_opt);
    const SeqNum end = end_opt ? static_cast<SeqNum>(*end_opt) : 0;

    // F4: BeginSeqNo is a 1-based sequence (0/negative/undecodable -> reject).
    // Without this guard a stray BeginSeqNo=0 slipped past `begin > range_end`
    // and drove cursor=0, so gap-fill frames were emitted with 34=0 — an
    // impossible MsgSeqNum that breaks the peer's receiver. A negative value
    // would also have wrapped to an absurd SeqNum through the cast above.
    if (*begin_opt < 1) {
        send_reject(msg.seq_num(), SessionRejectReason::ValueIncorrect, std::string(msg.msg_type()),
                    tags::BeginSeqNo, "BeginSeqNo must be >= 1");
        return;
    }

    if (cbs_.on_resend_request)
        guarded([&] { cbs_.on_resend_request(cfg_.id, begin, end); });

    // 2.2: a BeginSeqNo beyond everything we could ever send is answered
    // with a session Reject instead of an empty (confusing) replay.
    const SeqNum next_sender = store_->next_sender_seq_num();
    if (begin > next_sender) {
        send_reject(msg.seq_num(), SessionRejectReason::ValueIncorrect, std::string(msg.msg_type()),
                    tags::BeginSeqNo, "BeginSeqNo > NextSenderMsgSeqNum");
        return;
    }

    // 2.2: snapshot the store FIRST — get_messages runs its callback under
    // the store mutex, and no store lock may be held across I/O or a user
    // callback. nullopt = store read failed: emit NOTHING rather than
    // gap-claim messages we could not read.
    auto snap = snapshot_store(begin, end);
    if (!snap) {
        report("resend: store read failed; replay suppressed");
        return;
    }
    const auto &frames = *snap;

    // Resolve the replay window: EndSeqNo 0 (or one past our numbering)
    // means "everything I have".
    SeqNum range_end = (end == 0 || end >= next_sender) ? next_sender - 1 : end;
    if (begin > range_end)
        return; // nothing in range exists (e.g. BeginSeqNo == NextSender)

    // 2.2: the whole replay runs under send_mutex_ so a concurrent send()
    // cannot interleave frames into the middle of the gap-fill sequence.
    std::lock_guard lock(send_mutex_);

    const std::string version(fix::to_string(cfg_.id.version));
    SeqNum cursor = begin; // first sequence still unaccounted for

    // Coalesce everything from `cursor` up to (excluding) `up_to` into ONE
    // SequenceReset GapFill (35=4, 123=Y, 43=Y, 34=cursor, 36=up_to).
    const auto flush_gap = [&](SeqNum up_to) {
        if (cursor >= up_to)
            return;
        Message gap(msg_types::SequenceReset);
        gap.set(tags::GapFillFlag, "Y");
        gap.set(tags::PossDupFlag, "Y");
        gap.set(tags::NewSeqNo, static_cast<std::int64_t>(up_to));
        auto wire =
            builder_.try_serialize(gap, version, cursor, cfg_.id.senderCompID, cfg_.id.targetCompID,
                                   MessageBuilder::format_timestamp_now());
        if (!wire) {
            // Cannot happen for these three fields (no SOH smuggling
            // possible), but never emit a half-frame: leave the range for
            // the peer to request again.
            report("resend: gap-fill serialisation failed");
            return;
        }
        emit_wire_locked(*wire);
        cursor = up_to;
    };

    for (const auto &frame : frames) {
        if (frame.seq < cursor)
            continue; // already covered by an earlier gap-fill
        if (frame.seq > range_end)
            break;

        // Classify the stored frame. Local parser instance: StreamParser is
        // single-thread-affine and the session parser must not be touched
        // from under send_mutex_.
        StreamParser local;
        Message stored;
        local.feed(frame.wire.data(), frame.wire.size());
        const bool parsed = local.next(stored);
        const bool corrupt = !parsed || stored.msg_type().empty();

        if (!corrupt && !is_admin_msg(stored.msg_type())) {
            // --- Application message: re-emit re-tagged (2.2) ---------------
            flush_gap(frame.seq); // close the hole before the message itself
            stored.set(tags::PossDupFlag, "Y");
            if (auto orig = stored.get(tags::SendingTime))
                stored.set(tags::OrigSendingTime, *orig);
            auto wire = builder_.try_serialize(stored, version, frame.seq, cfg_.id.senderCompID,
                                               cfg_.id.targetCompID,
                                               MessageBuilder::format_timestamp_now());
            if (wire) {
                emit_wire_locked(*wire);
            } else {
                // Re-tagging failed (should be impossible for parsed fields):
                // fall back to covering this sequence with the gap-fill.
                ++resend_corrupt_;
                flush_gap(frame.seq + 1);
                continue;
            }
            cursor = frame.seq + 1;
        } else {
            // --- Admin (incl. Logon/SequenceReset) or corrupt: never resent
            // verbatim. Logon must not be re-issued on an established
            // session, and a stored SequenceReset's NewSeqNo is stale — both
            // are covered by the enclosing gap-fill (leaving them out of the
            // gap-fill would leave a hole the peer can never close). A frame
            // we cannot parse is likewise only counted (2.2).
            if (corrupt)
                ++resend_corrupt_;
            // cursor unchanged: the run keeps growing until the next app
            // message or the trailing flush.
        }
    }
    flush_gap(range_end + 1); // trailing run: hole to the end of the range
}

bool Session::handle_sequence_reset(const Message &msg) {
    const bool gap_fill = msg.get(tags::GapFillFlag).value_or("N") == "Y";
    const SeqNum expected = store_->next_target_seq_num();
    const SeqNum received = msg.seq_num();
    const bool dup = msg.poss_dup();

    if (gap_fill) {
        // 2.1 gap-fill rules: PossDupFlag is mandatory...
        if (!dup) {
            send_logout("SequenceReset GapFill requires PossDupFlag");
            disconnect();
            return true;
        }
        // ...MsgSeqNum must be exactly the sequence the fill starts at...
        if (received > expected) {
            // Cannot trust NewSeqNo across a gap we have not closed: ask for
            // the range instead and drop this message (not consumed, not
            // counted — same contract as validate_seq_num's gap branch).
            request_gap(expected);
            return false;
        }
        if (received < expected)
            return true; // stale re-send of a fill we are already past
        // ...and NewSeqNo must be strictly greater than expected (2.1).
        auto new_seq_opt = msg.get_int(tags::NewSeqNo);
        if (!new_seq_opt || static_cast<SeqNum>(*new_seq_opt) <= expected) {
            send_logout("invalid NewSeqNo");
            disconnect();
            return true;
        }
        store_->set_next_target_seq_num(static_cast<SeqNum>(*new_seq_opt));
        clear_gap(); // the fill landed: throttle + F9 retry budget go too
        return true;
    }

    // Reset mode: MsgSeqNum is IGNORED (this is why MsgType=4 is special-
    // cased before sequence validation — a reset exists precisely to jump
    // over an out-of-sync numbering). Only NewSeqNo matters, and it must be
    // a usable sequence (> 0).
    auto new_seq_opt = msg.get_int(tags::NewSeqNo);
    if (!new_seq_opt || *new_seq_opt <= 0) {
        send_logout("invalid NewSeqNo");
        disconnect();
        return true;
    }
    store_->set_next_target_seq_num(static_cast<SeqNum>(*new_seq_opt));
    clear_gap(); // reset landed: throttle + F9 retry budget go too
    return true;
}

void Session::handle_reject(const Message &msg) {
    auto st = state_.load(std::memory_order_acquire);
    const SeqNum ref_seq = static_cast<SeqNum>(msg.get_int(tags::RefSeqNum).value_or(0));
    std::string info(msg.get(tags::Text).value_or(""));
    if (info.empty()) {
        if (auto reason = msg.get_int(tags::SessionRejectReason))
            info = "session reject reason " + std::to_string(*reason);
    }

    if (st == SessionState::LogonSent) {
        // 2.5: a Reject answering our Logon terminates the handshake — the
        // peer will not answer a retry differently, so waiting for the
        // logon_timeout only wastes the window. Surface it through on_logout
        // (the session ended) and drop the connection.
        const std::string why = "Logon rejected: " + (info.empty() ? "unknown" : info);
        if (cbs_.on_logout)
            guarded([&] { cbs_.on_logout(cfg_.id, why); });
        disconnect();
        return;
    }
    // 2.5: otherwise the application gets a chance to see the peer's reject.
    if (cbs_.on_reject)
        guarded([&] { cbs_.on_reject(cfg_.id, ref_seq, info); });
}

// ---------------------------------------------------------------------------
// Sequence validation
// ---------------------------------------------------------------------------
bool Session::validate_seq_num(const Message &msg) {
    SeqNum expected = store_->next_target_seq_num();
    SeqNum received = msg.seq_num();

    if (received == 0) {
        // RefSeqNum unknown here (there is none) — 45=0, per F12.
        send_reject(0, SessionRejectReason::RequiredTagMissing, std::string(msg.msg_type()),
                    tags::MsgSeqNum);
        return false;
    }

    if (received > expected) {
        // Sequence gap: notify, then ask once (gap_open_ throttles a burst
        // of out-of-sequence messages to a single ResendRequest — 2.2).
        if (cbs_.on_sequence_gap)
            guarded([&] { cbs_.on_sequence_gap(cfg_.id, expected, received); });
        request_gap(expected);
        return false; // don't process until the gap is filled
    }

    if (received < expected) {
        if (!msg.poss_dup()) {
            // Too low and not a duplicate – this is fatal
            send_logout("MsgSeqNum too low");
            disconnect();
            return false;
        }
        return false; // stale re-send: skip
    }

    return true;
}

void Session::consume_inbound(const Message &msg, bool seq_validated) {
    // The ONE gate for "this inbound message takes its sequence slot" (F3),
    // shared by the dispatch advance and the duplicate-header Reject path:
    //  * seq_validated (Active / logon-state messages whose seq was judged):
    //    consume — including an in-sequence PossDup, otherwise a completed
    //    resend would leave us requesting the same range forever;
    //  * !poss_dup: a fresh message in a logon state (seq never validated)
    //    still consumes its slot, but a PossDup re-send in a logon state has
    //    nothing validated to consume;
    //  * the `> current` guard never rewinds the expected sequence (a stale
    //    seq-0/old message in a logon state cannot yank it backwards).
    if (!seq_validated && msg.poss_dup())
        return;
    const SeqNum received = msg.seq_num();
    const SeqNum next = received + 1;
    if (next > store_->next_target_seq_num())
        store_->set_next_target_seq_num(next);
    store_->store_inbound(received, msg.raw()); // audit trail; best-effort
}

void Session::request_gap(SeqNum begin) {
    bool open = false;
    // 2.2: at most one ResendRequest per open gap. The flag flips back to
    // false when a message finally lands on the expected sequence (or on
    // disconnect/reset/handshake — clear_gap() there).
    if (gap_open_.compare_exchange_strong(open, true, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        // F9: this is attempt #1 of the gap — start the retry clock and
        // budget that on_timer() works through (re-send every 1.2xHeartBtInt,
        // give up after kMaxGapRetries).
        gap_rr_attempts_.store(1, std::memory_order_relaxed);
        gap_rr_last_ms_.store(clock_now(), std::memory_order_relaxed);
        send_resend_request(begin, 0); // EndSeqNo 0 = "everything"
    }
}

// ---------------------------------------------------------------------------
// Internal send helpers
// ---------------------------------------------------------------------------
Result<void> Session::send_logon() {
    Message m(msg_types::Logon);
    m.set(tags::EncryptMethod, std::int64_t(0));
    // send_logon can run on user/timer threads while handle_logon (IO thread)
    // adopts the peer's HeartBtInt — read the atomic mirror, not cfg_ (F2).
    m.set(tags::HeartBtInt, std::int64_t(heartbeat_sec_.load(std::memory_order_acquire)));
    const bool do_reset = cfg_.reset_on_logon;
    if (do_reset)
        m.set(tags::ResetSeqNumFlag, "Y");
    if (!cfg_.username.empty())
        m.set(tags::Username, cfg_.username);
    if (!cfg_.password.empty())
        m.set(tags::Password, cfg_.password);
    if (!cfg_.default_appl_ver_id.empty())
        m.set(tags::DefaultApplVerID, cfg_.default_appl_ver_id);

    // F1: 141=Y MUST rewind the store BEFORE the frame is numbered — the old
    // code only tagged the Logon and left the store counting from N+1, so a
    // pre-advanced session emitted "35=A|141=Y|34=501" and the peer's very
    // next seq-2 message was answered "MsgSeqNum too low", wedging the
    // handshake. Both store implementations define reset() as a full
    // re-baseline (seqs -> 1, outbound/inbound history cleared), which is
    // exactly what ResetSeqNumFlag promises. A failed reset must surface, not
    // silently send a frame that lies about the numbering.
    if (do_reset && store_) {
        if (auto r = store_->reset(); !r) {
            report("logon: store reset failed; Logon not sent");
            return std::unexpected(r.error());
        }
        clear_gap(); // the old numbering's gap/retry state is meaningless now
    }

    // F6: publish the handshake state BEFORE send_message(). do_send can come
    // back around a loopback faster than this function returns; a reply
    // processed while the FSM still read NotConnected/Disconnected was
    // dropped by handle_logon's "stale Logon — ignore" branch and the
    // handshake then deadlocked until logon_timeout.
    const SessionState desired =
        cfg_.initiator ? SessionState::LogonSent : SessionState::WaitingLogon;
    const SessionState prev = state_.load(std::memory_order_acquire);
    bool published = false;
    if (prev != desired) {
        // Only a pre-logon state may move onto the wire: never resurrect a
        // session a concurrent teardown already ended (and never downgrade a
        // live one).
        if (prev != SessionState::NotConnected && prev != SessionState::Disconnected)
            return make_unexpected(ErrorCode::SessionError);
        state_.store(desired, std::memory_order_release);
        published = true;
    }
    // 2.3 deadline base — written before the state so the timer thread
    // always sees a fresh baseline once LogonSent is visible.
    logon_sent_time_ms_.store(clock_now(), std::memory_order_relaxed);

    if (auto sent = send_message(m); !sent) {
        // No Logon went out: roll the FSM back so logon() stays retryable
        // from the pre-logon state and no timer deadline dangles.
        if (published)
            state_.store(prev, std::memory_order_release);
        return std::unexpected(sent.error());
    }
    return {};
}

void Session::send_logout(std::string_view reason) {
    Message m(msg_types::Logout);
    if (!reason.empty())
        m.set(tags::Text, reason);
    send_message(m);
    logout_sent_time_ms_.store(clock_now(), std::memory_order_relaxed); // 2.3 deadline base
    // CAS into LogoutSent, but never resurrect a session the transport (or
    // the heartbeat timeout) already tore down concurrently.
    auto st = state_.load(std::memory_order_acquire);
    while (st != SessionState::Disconnected) {
        if (state_.compare_exchange_weak(st, SessionState::LogoutSent, std::memory_order_acq_rel,
                                         std::memory_order_acquire))
            break;
    }
}

void Session::send_logout_echo() {
    // Peer-initiated logout: the wire echo only — the caller transitions
    // straight to Disconnected without lingering in LogoutSent.
    Message m(msg_types::Logout);
    send_message(m);
}

void Session::send_heartbeat(std::string_view test_req_id) {
    Message m(msg_types::Heartbeat);
    if (!test_req_id.empty())
        m.set(tags::TestReqID, test_req_id);
    send_message(m);
}

void Session::send_test_request() {
    static std::atomic<std::uint64_t> counter{0};
    const std::string req_id = "TEST-" + std::to_string(++counter);
    {
        // Publish the ID before the flag and before the send, and never hold
        // the mutex across send_message() (F2).
        std::lock_guard lock(test_req_mtx_);
        pending_test_req_id_ = req_id;
    }
    // 2.3: record the send time BEFORE arming the flag so the 1.2×HeartBtInt
    // deadline is always measured from (roughly) the actual send — the other
    // order would let the timer read a stale baseline while pending=true.
    test_req_sent_time_ms_.store(clock_now(), std::memory_order_relaxed);
    test_req_pending_.store(true, std::memory_order_release);

    Message m(msg_types::TestRequest);
    m.set(tags::TestReqID, req_id);
    send_message(m);
}

void Session::send_resend_request(SeqNum begin, SeqNum end) {
    Message m(msg_types::ResendRequest);
    m.set(tags::BeginSeqNo, static_cast<std::int64_t>(begin));
    m.set(tags::EndSeqNo, static_cast<std::int64_t>(end));
    send_message(m);
}

void Session::send_reject(SeqNum ref_seq, SessionRejectReason reason, std::string_view ref_msg_type,
                          TagNum ref_tag, std::string_view text) {
    // F12: tag 45 (RefSeqNum) is MANDATORY on a session Reject — the old
    // `if (ref_seq > 0)` omitted it entirely for unknown reference sequences
    // (missing MsgType path), and our own tests called that omission
    // "mandatory". Emit 45=0 (unknown) instead; the counterparty still gets
    // a structurally complete Reject.
    Message m(msg_types::Reject);
    m.set(tags::RefSeqNum, static_cast<std::int64_t>(ref_seq));
    m.set(tags::SessionRejectReason, static_cast<std::int64_t>(reason));
    if (!ref_msg_type.empty())
        m.set(tags::RefMsgType, ref_msg_type);
    if (ref_tag != 0)
        m.set(tags::RefTagID, static_cast<std::int64_t>(ref_tag));
    if (!text.empty())
        m.set(tags::Text, text);
    send_message(m);
}

void Session::send_business_reject(SeqNum ref_seq, int business_reject_reason,
                                   std::string_view ref_msg_type, std::string_view text) {
    // 2.7: BusinessMessageReject (35=j) for dictionary-validation failures on
    // application messages.
    // F12: RefSeqNum (45) is mandatory here too — 0 when the reference is
    // unknown.
    Message m(msg_types::BusinessMessageReject);
    m.set(tags::RefSeqNum, static_cast<std::int64_t>(ref_seq));
    m.set(tags::BusinessRejectReason, static_cast<std::int64_t>(business_reject_reason));
    if (!ref_msg_type.empty())
        m.set(tags::RefMsgType, ref_msg_type);
    if (!text.empty())
        m.set(tags::Text, text);
    send_message(m);
}

Result<void> Session::send_message(Message &msg) {
    std::lock_guard lock(send_mutex_);
    return send_message_locked(msg);
}

Result<void> Session::send_message_locked(Message &msg) {
    SeqNum seq = store_->next_sender_seq_num();
    auto bs = fix::to_string(cfg_.id.version);
    auto ts = MessageBuilder::format_timestamp_now();

    // Strict serialization (F5): try_serialize() surfaces the first rejected
    // field (e.g. an SOH smuggled in through Message::fields()) instead of
    // returning "". The error must be returned BEFORE the sequence number is
    // stored/incremented — the old code stored an empty wire frame, bumped
    // the sender seq and sent nothing, leaving a permanent gap.
    auto wire =
        builder_.try_serialize(msg, bs, seq, cfg_.id.senderCompID, cfg_.id.targetCompID, ts);
    if (!wire)
        return std::unexpected(wire.error());

    // 2.6: check store_outbound BEFORE incrementing. A failed persist means
    // the frame cannot be resent later, so burning the sequence here would
    // leave a permanent hole — abort the send with the real error instead
    // (the old code discarded the Result and always returned {}).
    auto stored = store_->store_outbound(seq, *wire);
    if (!stored)
        return std::unexpected(stored.error());
    store_->incr_sender_seq_num();

    if (cbs_.do_send)
        cbs_.do_send(*wire);

    last_send_time_ms_.store(clock_now(), std::memory_order_relaxed);
    ++msgs_sent_;
    return {};
}

void Session::emit_wire_locked(std::string_view wire) {
    // Resend replay emission: the caller holds send_mutex_. No sequence is
    // consumed and nothing is stored — the frame is a re-issue of an
    // already-numbered message (a gap-fill or a PossDup re-tagged app msg).
    if (cbs_.do_send)
        cbs_.do_send(std::string(wire));
    last_send_time_ms_.store(clock_now(), std::memory_order_relaxed);
    ++msgs_sent_;
}

std::optional<std::vector<Session::StoredFrame>> Session::snapshot_store(SeqNum begin, SeqNum end) {
    std::optional<std::vector<StoredFrame>> out = std::vector<StoredFrame>{};
    if (!store_)
        return out;
    auto res = store_->get_messages(begin, end, [&out](SeqNum seq, const std::string &raw) {
        out->push_back(StoredFrame{seq, raw});
    });
    if (!res)
        return std::nullopt;
    return out;
}

// ---------------------------------------------------------------------------
// State machine / error surface
// ---------------------------------------------------------------------------
bool Session::transition(SessionState expected, SessionState desired) {
    auto st = expected;
    return state_.compare_exchange_strong(st, desired, std::memory_order_acq_rel,
                                          std::memory_order_acquire);
}

void Session::report(std::string_view what) noexcept {
    ++errors_;
    if (!cbs_.on_error)
        return; // no logging infra in this process — documented floor
    try {
        cbs_.on_error(cfg_.id, what);
    } catch (...) {
        // The error sink itself must never take an IO/timer thread down.
    }
}

const DataDictionary *Session::resolve_dictionary() const {
    if (dict_)
        return dict_;
    // Fallback: the process-wide registry the engine populates via
    // Engine::load_dictionary() / load_builtin_dictionary().
    return DictionaryRegistry::instance().get(cfg_.id.version);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
bool Session::is_admin_msg(std::string_view mt) const noexcept {
    return mt == msg_types::Logon || mt == msg_types::Logout || mt == msg_types::Heartbeat ||
           mt == msg_types::TestRequest || mt == msg_types::ResendRequest ||
           mt == msg_types::SequenceReset || mt == msg_types::Reject;
}

} // namespace fix
