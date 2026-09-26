// =============================================================================
// FIX Protocol Engine - Session implementation
// =============================================================================
#include "fix/session/session.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

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
    case SessionState::LogoutReceived:
        return "LogoutReceived";
    case SessionState::Reconnecting:
        return "Reconnecting";
    case SessionState::Disconnected:
        return "Disconnected";
    default:
        return "Unknown";
    }
}

Session::Session(SessionConfig cfg, std::unique_ptr<IMessageStore> store,
                 const DataDictionary *dict, SessionCallbacks cbs)
    : cfg_(std::move(cfg)),
      store_(std::move(store)),
      dict_(dict),
      cbs_(std::move(cbs)) {
    heartbeat_sec_.store(cfg_.heartbeat_interval, std::memory_order_relaxed);
    last_send_time_ms_.store(now_ms(), std::memory_order_relaxed);
    last_recv_time_ms_.store(now_ms(), std::memory_order_relaxed);
    logon_sent_time_ms_.store(now_ms(), std::memory_order_relaxed);
}

Session::~Session() = default;

Session::Millis Session::now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
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
        if (state_.load() == SessionState::Disconnected)
            break;
        try {
            process_message(msg);
        } catch (const std::exception &e) {
            // Log and continue – don't let a single bad message kill the session
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
    last_send_time_ms_.store(now_ms(), std::memory_order_relaxed);
    last_recv_time_ms_.store(now_ms(), std::memory_order_relaxed);

    if (cfg_.initiator) {
        // A dropped connection can leave the FSM in LogonSent/Active/LogoutSent
        // while logon() only accepts NotConnected/Reconnecting/Disconnected –
        // normalise first so the initiator always re-logons on reconnect.
        auto st = state_.load();
        if (st != SessionState::NotConnected && st != SessionState::Reconnecting &&
            st != SessionState::Disconnected) {
            state_.store(SessionState::Disconnected);
        }
        (void)logon();
    } else {
        // Acceptor: only WaitingLogon accepts an inbound Logon.
        state_.store(SessionState::WaitingLogon);
    }
}

Result<void> Session::send(Message msg) {
    return send_message(msg);
}

Result<void> Session::send_raw(const std::string &raw) {
    std::lock_guard lock(send_mutex_);
    if (cbs_.do_send)
        cbs_.do_send(raw);
    last_send_time_ms_.store(now_ms(), std::memory_order_relaxed);
    ++msgs_sent_;
    return {};
}

Result<void> Session::logon() {
    if (state_.load() != SessionState::NotConnected &&
        state_.load() != SessionState::Reconnecting &&
        state_.load() != SessionState::Disconnected) {
        return make_unexpected(ErrorCode::SessionError);
    }
    send_logon();
    return {};
}

Result<void> Session::logout(std::string_view reason) {
    send_logout(reason);
    return {};
}

void Session::on_timer() {
    auto st = state_.load();
    if (st != SessionState::Active && st != SessionState::LogonSent &&
        st != SessionState::WaitingLogon)
        return;

    const auto n = now_ms();
    // Timer-side reads go through the atomic mirror — cfg_.heartbeat_interval
    // is written by the IO thread in handle_logon (F2).
    const auto hb_ms = static_cast<Millis>(heartbeat_sec_.load(std::memory_order_relaxed)) * 1000;

    // Heartbeat check: if we haven't sent anything in heartbeat_interval, send HB
    if ((n - last_send_time_ms_.load(std::memory_order_relaxed)) >= hb_ms &&
        st == SessionState::Active) {
        send_heartbeat();
    }

    // TestRequest: if we haven't received anything in heartbeat_interval + tolerance
    const auto tolerance_ms = hb_ms + 2000;
    if ((n - last_recv_time_ms_.load(std::memory_order_relaxed)) >= tolerance_ms &&
        st == SessionState::Active) {
        if (!test_req_pending_.load(std::memory_order_acquire)) {
            send_test_request();
        } else {
            // No response to TestRequest – timeout
            if (cbs_.on_heartbeat_timeout)
                cbs_.on_heartbeat_timeout(cfg_.id);
            disconnect();
        }
    }

    // Logon timeout
    if (st == SessionState::LogonSent) {
        auto timeout_ms = static_cast<Millis>(cfg_.logon_timeout) * 1000;
        if ((n - logon_sent_time_ms_.load(std::memory_order_relaxed)) >= timeout_ms) {
            disconnect();
        }
    }
}

void Session::disconnect() {
    state_.store(SessionState::Disconnected);

    // The TestRequest round trip can never complete on a dead connection.
    test_req_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(test_req_mtx_);
        pending_test_req_id_.clear();
    }
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
    state_.store(SessionState::NotConnected);
    {
        std::lock_guard lock(parser_mtx_);
        parser_.reset();
    }
    test_req_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(test_req_mtx_);
        pending_test_req_id_.clear();
    }
    if (store_)
        return store_->reset();
    return {};
}

// ---------------------------------------------------------------------------
// Message processing
// ---------------------------------------------------------------------------
void Session::process_message(const Message &msg) {
    last_recv_time_ms_.store(now_ms(), std::memory_order_relaxed);

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

    // Basic validation
    if (!msg.has(tags::MsgType)) {
        send_reject(0, SessionRejectReason::RequiredTagMissing, "", tags::MsgType,
                    "Missing MsgType");
        return;
    }

    // Sequence number validation (unless we're in logon state)
    auto st = state_.load();
    const bool seq_validated = st != SessionState::WaitingLogon && st != SessionState::LogonSent &&
                               st != SessionState::NotConnected;
    if (seq_validated) {
        if (!validate_seq_num(msg))
            return;
    }

    // Duplicate standard-header tags: Reject (reason 13) for ANY parsed
    // message, including one received in a logon state (F7) — a duplicate-
    // header Logon must be rejected too, not silently accepted. RefSeqNum is
    // msg.seq_num(), which the parser keeps as the first (valid) occurrence.
    if (msg.has_duplicate_header()) {
        send_reject(msg.seq_num(), SessionRejectReason::TagAppearsMoreThanOnce,
                    std::string(msg.msg_type()), 0, "Tag appears more than once");
        // Consume the sequence number only when sequence validation actually
        // ran (the state-gated path above) and the message is not a re-send;
        // in logon states we just Reject and return without consuming.
        if (seq_validated && !msg.poss_dup()) {
            store_->incr_target_seq_num();
            store_->store_inbound(msg.seq_num(), msg.raw());
        }
        ++msgs_received_;
        return;
    }

    auto mt = msg.msg_type();
    if (is_admin_msg(mt)) {
        process_admin(msg);
    } else {
        process_app(msg);
    }

    // Advance target sequence number (unless PossDup)
    if (!msg.poss_dup()) {
        store_->incr_target_seq_num();
        store_->store_inbound(msg.seq_num(), msg.raw());
    }
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
        handle_sequence_reset(msg);
    else if (mt == msg_types::Reject)
        handle_reject(msg);
}

void Session::process_app(const Message &msg) {
    if (state_.load() != SessionState::Active) {
        // Reject app messages when not active
        send_reject(msg.seq_num(), SessionRejectReason::Other, std::string(msg.msg_type()), 0,
                    "Session not active");
        return;
    }
    if (cbs_.on_message)
        cbs_.on_message(cfg_.id, msg);
}

// ---------------------------------------------------------------------------
// Admin message handlers
// ---------------------------------------------------------------------------
void Session::handle_logon(const Message &msg) {
    auto st = state_.load();

    // Extract fields
    auto hb_opt = msg.get_int(tags::HeartBtInt);
    if (hb_opt) {
        // SessionConfig stays copyable (no atomics), so the timer-visible
        // value is mirrored into heartbeat_sec_ (F2).
        cfg_.heartbeat_interval = static_cast<int>(*hb_opt);
        heartbeat_sec_.store(static_cast<int>(*hb_opt), std::memory_order_release);
    }

    bool reset_flag = msg.get(tags::ResetSeqNumFlag).value_or("N") == "Y";
    if (reset_flag || cfg_.reset_on_logon) {
        store_->set_next_target_seq_num(1);
        store_->set_next_sender_seq_num(1);
    }

    if (st == SessionState::WaitingLogon) {
        // Acceptor: respond with logon
        send_logon();
        state_.store(SessionState::Active);
        if (cbs_.on_logon)
            cbs_.on_logon(cfg_.id);
    } else if (st == SessionState::LogonSent) {
        // Initiator: received response
        state_.store(SessionState::Active);
        if (cbs_.on_logon)
            cbs_.on_logon(cfg_.id);
    } else if (st == SessionState::Active) {
        // Re-logon (reset)
        if (reset_flag) {
            state_.store(SessionState::Active);
        }
    }
}

void Session::handle_logout(const Message &msg) {
    auto reason = msg.get(tags::Text).value_or("");
    auto st = state_.load();

    if (st == SessionState::LogoutSent) {
        // Mutual logout complete
        state_.store(SessionState::Disconnected);
    } else {
        // Peer initiated logout
        state_.store(SessionState::LogoutReceived);
        send_logout(""); // echo back
        state_.store(SessionState::Disconnected);
    }
    if (cbs_.on_logout)
        cbs_.on_logout(cfg_.id, reason);
}

void Session::handle_heartbeat(const Message &msg) {
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
        return;

    SeqNum begin = static_cast<SeqNum>(*begin_opt);
    SeqNum end = end_opt ? static_cast<SeqNum>(*end_opt) : 0;

    if (cbs_.on_resend_request)
        cbs_.on_resend_request(cfg_.id, begin, end);

    // Replay stored messages
    store_->get_messages(begin, end, [this](SeqNum seq, const std::string &raw) {
        // Re-send with PossDupFlag=Y and OrigSendingTime
        // For simplicity we just resend the raw bytes (a real impl would re-tag)
        if (cbs_.do_send)
            cbs_.do_send(raw);
    });
}

void Session::handle_sequence_reset(const Message &msg) {
    bool gap_fill = msg.get(tags::GapFillFlag).value_or("N") == "Y";
    auto new_seq_opt = msg.get_int(tags::NewSeqNo);
    if (!new_seq_opt)
        return;
    SeqNum new_seq = static_cast<SeqNum>(*new_seq_opt);

    if (gap_fill) {
        // GapFill: advance expected sequence number
        store_->set_next_target_seq_num(new_seq);
    } else {
        // Hard reset
        store_->set_next_target_seq_num(new_seq);
    }
}

void Session::handle_reject(const Message &msg) {
    // Log reject – application can be notified if needed
}

// ---------------------------------------------------------------------------
// Sequence validation
// ---------------------------------------------------------------------------
bool Session::validate_seq_num(const Message &msg) {
    SeqNum expected = store_->next_target_seq_num();
    SeqNum received = msg.seq_num();

    if (received == 0) {
        send_reject(0, SessionRejectReason::RequiredTagMissing, std::string(msg.msg_type()),
                    tags::MsgSeqNum);
        return false;
    }

    if (received > expected) {
        // Sequence gap detected
        if (cbs_.on_sequence_gap)
            cbs_.on_sequence_gap(cfg_.id, expected, received);
        send_resend_request(expected, 0); // request all missing
        return false;                     // don't process until gap is filled
    }

    if (received < expected) {
        if (!msg.poss_dup()) {
            // Too low and not a duplicate – this is fatal
            send_logout("MsgSeqNum too low");
            disconnect();
            return false;
        }
        return false; // skip duplicate
    }

    return true;
}

// ---------------------------------------------------------------------------
// Internal send helpers
// ---------------------------------------------------------------------------
void Session::send_logon() {
    Message m(msg_types::Logon);
    m.set(tags::EncryptMethod, std::int64_t(0));
    // send_logon can run on user/timer threads while handle_logon (IO thread)
    // adopts the peer's HeartBtInt — read the atomic mirror, not cfg_ (F2).
    m.set(tags::HeartBtInt, std::int64_t(heartbeat_sec_.load(std::memory_order_acquire)));
    if (cfg_.reset_on_logon)
        m.set(tags::ResetSeqNumFlag, "Y");
    if (!cfg_.username.empty())
        m.set(tags::Username, cfg_.username);
    if (!cfg_.password.empty())
        m.set(tags::Password, cfg_.password);
    if (!cfg_.default_appl_ver_id.empty())
        m.set(tags::DefaultApplVerID, cfg_.default_appl_ver_id);
    send_message(m);
    logon_sent_time_ms_.store(now_ms(), std::memory_order_relaxed);
    if (cfg_.initiator)
        state_.store(SessionState::LogonSent);
    else
        state_.store(SessionState::WaitingLogon);
}

void Session::send_logout(std::string_view reason) {
    Message m(msg_types::Logout);
    if (!reason.empty())
        m.set(tags::Text, reason);
    send_message(m);
    state_.store(SessionState::LogoutSent);
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

void Session::send_sequence_reset(SeqNum new_seq, bool gap_fill) {
    Message m(msg_types::SequenceReset);
    m.set(tags::NewSeqNo, static_cast<std::int64_t>(new_seq));
    if (gap_fill)
        m.set(tags::GapFillFlag, "Y");
    send_message(m);
}

void Session::send_reject(SeqNum ref_seq, SessionRejectReason reason, std::string_view ref_msg_type,
                          TagNum ref_tag, std::string_view text) {
    Message m(msg_types::Reject);
    if (ref_seq > 0)
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

Result<void> Session::send_message(Message &msg) {
    std::lock_guard lock(send_mutex_);

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

    store_->store_outbound(seq, *wire);
    store_->incr_sender_seq_num();

    if (cbs_.do_send)
        cbs_.do_send(*wire);

    last_send_time_ms_.store(now_ms(), std::memory_order_relaxed);
    ++msgs_sent_;
    return {};
}

bool Session::is_admin_msg(std::string_view mt) const noexcept {
    return mt == msg_types::Logon || mt == msg_types::Logout || mt == msg_types::Heartbeat ||
           mt == msg_types::TestRequest || mt == msg_types::ResendRequest ||
           mt == msg_types::SequenceReset || mt == msg_types::Reject;
}

std::string Session::sending_time_str() const {
    return MessageBuilder::format_timestamp_now();
}

} // namespace fix
