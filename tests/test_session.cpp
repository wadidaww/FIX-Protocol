// =============================================================================
// FIX Protocol Engine - Session unit tests
// =============================================================================
#include "fix/core/constants.hpp"
#include "fix/parser/serializer.hpp"
#include "fix/session/session.hpp"
#include "fix/store/memory_store.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace fix;

// ---------------------------------------------------------------------------
// Helper: build a wire FIX message
// ---------------------------------------------------------------------------
static std::string
make_wire(std::string_view begin_str, std::string_view msg_type, SeqNum seq,
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

static SessionConfig make_cfg(bool initiator = false) {
    SessionConfig cfg;
    cfg.id.version = FixVersion::FIX_4_2;
    cfg.id.senderCompID = "SERVER";
    cfg.id.targetCompID = "CLIENT";
    cfg.initiator = initiator;
    cfg.heartbeat_interval = 30;
    return cfg;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
TEST(SessionTest, InitialStateNotConnected) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session s(make_cfg(false), std::move(store), nullptr, cbs);
    EXPECT_EQ(s.state(), SessionState::NotConnected);
}

TEST(SessionTest, AcceptorReceivesLogon_TransitionsToActive) {
    std::vector<std::string> sent;
    bool logon_called = false;

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
        logon_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);

    // Simulate that logon was "started" (acceptor begins in WaitingLogon)
    // We call logon() which sets the state
    sess.logon(); // acceptor sends logon and enters WaitingLogon

    // Now feed a Logon message from the counterparty
    std::string wire = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                 {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(wire.data(), wire.size());

    EXPECT_EQ(sess.state(), SessionState::Active);
    EXPECT_TRUE(logon_called);
    EXPECT_FALSE(sent.empty()); // acceptor sent back a Logon
}

TEST(SessionTest, InitiatorLogon_SetsLogonSentState) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(true), std::move(store), nullptr, cbs);

    sess.logon();

    EXPECT_EQ(sess.state(), SessionState::LogonSent);
    ASSERT_FALSE(sent.empty());
    // The sent message should be a Logon
    StreamParser p;
    p.feed(sent[0].data(), sent[0].size());
    Message m;
    ASSERT_TRUE(p.next(m));
    EXPECT_EQ(m.msg_type(), msg_types::Logon);
}

TEST(SessionTest, HeartbeatSent) {
    std::vector<std::string> sent;
    bool logon_called = false;

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
        logon_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    // Feed a logon
    std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                  {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());
    ASSERT_EQ(sess.state(), SessionState::Active);

    std::size_t sent_before = sent.size();

    // Send a heartbeat manually
    Message hb(msg_types::Heartbeat);
    sess.send(hb);

    EXPECT_GT(sent.size(), sent_before);

    // Verify it's a heartbeat
    StreamParser p;
    p.feed(sent.back().data(), sent.back().size());
    Message m;
    ASSERT_TRUE(p.next(m));
    EXPECT_EQ(m.msg_type(), msg_types::Heartbeat);
}

TEST(SessionTest, TestRequestAndResponse) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                  {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());

    // Feed a TestRequest from counterparty
    std::string tr = make_wire("FIX.4.2", msg_types::TestRequest, 2, "CLIENT", "SERVER",
                               {{tags::TestReqID, "PING1"}});
    sess.on_data(tr.data(), tr.size());

    // Session should have sent a Heartbeat with TestReqID=PING1
    bool found = false;
    for (const auto &s : sent) {
        StreamParser p;
        p.feed(s.data(), s.size());
        Message m;
        if (p.next(m) && m.msg_type() == msg_types::Heartbeat) {
            if (m.get(tags::TestReqID).value_or("") == "PING1") {
                found = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found);
}

TEST(SessionTest, LogoutFlow) {
    std::vector<std::string> sent;
    bool logout_called = false;

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
    };
    cbs.on_logout = [&](const SessionID &, std::string_view) {
        logout_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                  {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());

    // Feed a Logout from counterparty
    std::string lo =
        make_wire("FIX.4.2", msg_types::Logout, 2, "CLIENT", "SERVER", {{tags::Text, "Goodbye"}});
    sess.on_data(lo.data(), lo.size());

    EXPECT_TRUE(logout_called);
    EXPECT_EQ(sess.state(), SessionState::Disconnected);
}

TEST(SessionTest, SequenceNumberIncremented) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                  {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());

    // After logon exchange: acceptor sent logon (from sess.logon()) +
    // logon response (from handle_logon) = 2 messages
    std::size_t after_logon = sess.msgs_sent();
    EXPECT_GE(after_logon, 1u); // at least one logon message

    Message m(msg_types::Heartbeat);
    sess.send(m);

    EXPECT_EQ(sess.msgs_sent(), after_logon + 1u);
}

// ---------------------------------------------------------------------------
// Session identity validation (security) tests
// ---------------------------------------------------------------------------
TEST(SessionTest, WrongTargetCompID_TerminatesSession) {
    std::vector<std::string> sent;
    bool logon_called = false;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
        logon_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    // Impersonator: right sender (CLIENT) but wrong target (not SERVER)
    std::string wire = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "EVIL",
                                 {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(wire.data(), wire.size());

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_FALSE(logon_called);

    // A Logout must have been sent in response
    bool sent_logout = false;
    for (const auto &s : sent) {
        StreamParser p;
        p.feed(s.data(), s.size());
        Message m;
        if (p.next(m) && m.msg_type() == msg_types::Logout) {
            sent_logout = true;
            break;
        }
    }
    EXPECT_TRUE(sent_logout);
}

TEST(SessionTest, WrongSenderCompID_TerminatesSession) {
    std::vector<std::string> sent;
    bool logon_called = false;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
        logon_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    std::string wire = make_wire("FIX.4.2", msg_types::Logon, 1, "ATTACKER", "SERVER",
                                 {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(wire.data(), wire.size());

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_FALSE(logon_called);
}

TEST(SessionTest, WrongBeginString_TerminatesSession) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs); // FIX 4.2 session
    sess.logon();

    // Counterparty claims to be FIX 4.4 on a FIX 4.2 session
    std::string wire = make_wire("FIX.4.4", msg_types::Logon, 1, "CLIENT", "SERVER",
                                 {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(wire.data(), wire.size());

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
}

TEST(SessionTest, DuplicateHeaderTag_SendsReject) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                  {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());
    ASSERT_EQ(sess.state(), SessionState::Active);

    // Heartbeat with duplicated MsgSeqNum tag (first value wins for RefSeqNum)
    MessageBuilder b;
    b.begin("FIX.4.2", msg_types::Heartbeat);
    b.add(tags::SenderCompID, "CLIENT");
    b.add(tags::TargetCompID, "SERVER");
    b.add(tags::MsgSeqNum, std::int64_t(2));
    b.add(tags::MsgSeqNum, std::int64_t(99));
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    std::string wire = b.finish();
    sess.on_data(wire.data(), wire.size());

    // Session must send Reject with reason 13 (TagAppearsMoreThanOnce)
    bool found_reject = false;
    for (const auto &s : sent) {
        StreamParser p;
        p.feed(s.data(), s.size());
        Message m;
        if (p.next(m) && m.msg_type() == msg_types::Reject) {
            auto reason = m.get(tags::SessionRejectReason);
            if (reason && *reason == "13") {
                found_reject = true;
                // RefSeqNum must be the first (valid) sequence number
                auto ref = m.get(tags::RefSeqNum);
                ASSERT_TRUE(ref.has_value());
                EXPECT_EQ(*ref, "2");
                break;
            }
        }
    }
    EXPECT_TRUE(found_reject);
}

// ---------------------------------------------------------------------------
// F1 regression: Session::disconnect() must invoke cbs_.do_disconnect so the
// engine can actually close the transport socket on a protocol violation.
// ---------------------------------------------------------------------------
TEST(SessionTest, WrongCompID_InvokesDisconnectCallback) {
    std::vector<std::string> sent;
    bool logon_called = false;
    bool disconnect_called = false;

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
        logon_called = true;
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon(); // acceptor → WaitingLogon

    std::string wire = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "EVIL",
                                 {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(wire.data(), wire.size());

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called) << "disconnect() must close the transport (F1)";
    EXPECT_FALSE(logon_called);

    // The peer still gets a Logout before the socket goes away.
    bool sent_logout = false;
    for (const auto &s : sent) {
        StreamParser p;
        p.feed(s.data(), s.size());
        Message m;
        if (p.next(m) && m.msg_type() == msg_types::Logout) {
            sent_logout = true;
            break;
        }
    }
    EXPECT_TRUE(sent_logout);
}

// ---------------------------------------------------------------------------
// F14 regression: once a message in a batch tears the session down, the
// remaining messages of the SAME batch must not be dispatched.
// ---------------------------------------------------------------------------
TEST(SessionTest, WrongCompID_StopsProcessingRemainingBatch) {
    std::vector<std::string> sent;
    bool disconnect_called = false;
    bool app_message_seen = false;

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_message = [&](const SessionID &, const Message &) {
        app_message_seen = true;
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    // msg1: identity mismatch → Logout + disconnect.
    std::string first = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "EVIL",
                                  {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    // msg2: otherwise-valid message delivered in the same feed() batch.
    std::string second = make_wire("FIX.4.2", msg_types::Heartbeat, 2, "CLIENT", "SERVER");
    const std::string batch = first + second;
    sess.on_data(batch.data(), batch.size());

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
    // msg2 was never dispatched: no message counted, nothing on the wire.
    EXPECT_EQ(sess.msgs_received(), 0u);
    EXPECT_FALSE(app_message_seen);
}

// ---------------------------------------------------------------------------
// F5 regression: a serialization failure (SOH smuggled in via fields())
// must NOT burn a sender sequence number — send() returns the error and the
// sequence stays put.
// ---------------------------------------------------------------------------
TEST(SessionTest, SerializationFailureDoesNotBurnSenderSequence) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon();

    std::string logon = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                  {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    sess.on_data(logon.data(), logon.size());
    ASSERT_EQ(sess.state(), SessionState::Active);

    // Session exposes its store: the cleanest seq-number observation point.
    const IMessageStore *store_ptr = sess.store();
    ASSERT_NE(store_ptr, nullptr);
    const SeqNum seq_before = store_ptr->next_sender_seq_num();
    const std::uint64_t sent_before = sess.msgs_sent();
    const std::size_t wire_before = sent.size();

    // Craft a message whose stored field value contains SOH + an injected
    // field, bypassing Message::set()'s validation via the mutable accessor.
    Message m(msg_types::Heartbeat);
    m.set(tags::Text, "harmless");
    std::string injected = "AAA";
    injected += SOH;
    injected += "55=HACKED";
    m.fields().emplace_back(tags::Symbol, injected);

    auto r = sess.send(m);

    ASSERT_FALSE(r.has_value()) << "send() must propagate the serialization error";
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::InvalidField));
    // Nothing was stored, transmitted or counted — and above all the sender
    // sequence number is untouched (no permanent gap).
    EXPECT_EQ(store_ptr->next_sender_seq_num(), seq_before) << "failed send burned a seq number";
    EXPECT_EQ(sess.msgs_sent(), sent_before);
    EXPECT_EQ(sent.size(), wire_before);

    // A subsequent healthy send still uses the very same sequence number.
    Message ok(msg_types::Heartbeat);
    ASSERT_TRUE(sess.send(ok).has_value());
    EXPECT_EQ(store_ptr->next_sender_seq_num(), seq_before + 1);
    EXPECT_EQ(sess.msgs_sent(), sent_before + 1);
}

// ---------------------------------------------------------------------------
// F7 regression: the duplicate-header Reject must fire in ANY state — a
// dup-header Logon received in WaitingLogon is rejected (reason 13), the
// session does NOT go Active, and no sequence number is consumed.
// ---------------------------------------------------------------------------
TEST(SessionTest, DuplicateHeaderLogon_RejectedWhileWaitingLogon) {
    std::vector<std::string> sent;
    bool logon_called = false;

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
        logon_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    sess.logon(); // acceptor → WaitingLogon
    ASSERT_EQ(sess.state(), SessionState::WaitingLogon);
    ASSERT_NE(sess.store(), nullptr);
    const SeqNum target_before = sess.store()->next_target_seq_num();

    // Logon carrying a duplicated MsgSeqNum (standard-header tag).
    MessageBuilder b;
    b.begin("FIX.4.2", msg_types::Logon);
    b.add(tags::SenderCompID, "CLIENT");
    b.add(tags::TargetCompID, "SERVER");
    b.add(tags::MsgSeqNum, std::int64_t(1));
    b.add(tags::MsgSeqNum, std::int64_t(99)); // duplicate header tag
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    b.add(tags::EncryptMethod, std::int64_t(0));
    b.add(tags::HeartBtInt, std::int64_t(30));
    std::string wire = b.finish();
    ASSERT_FALSE(wire.empty());
    sess.on_data(wire.data(), wire.size());

    bool found_reject = false;
    for (const auto &s : sent) {
        StreamParser p;
        p.feed(s.data(), s.size());
        Message m;
        if (p.next(m) && m.msg_type() == msg_types::Reject) {
            auto reason = m.get(tags::SessionRejectReason);
            if (reason && *reason == "13") {
                found_reject = true;
                auto ref = m.get(tags::RefSeqNum);
                ASSERT_TRUE(ref.has_value());
                EXPECT_EQ(*ref, "1") << "RefSeqNum must be the first (valid) seq";
                break;
            }
        }
    }
    EXPECT_TRUE(found_reject) << "dup-header Reject must fire in logon states too (F7)";
    EXPECT_EQ(sess.state(), SessionState::WaitingLogon) << "dup-header Logon must not activate";
    EXPECT_FALSE(logon_called);
    // In logon states sequence validation never ran → no consumption (F7).
    EXPECT_EQ(sess.store()->next_target_seq_num(), target_before);
}

// ===========================================================================
// Phase 2 (tasks 2.1-2.8): FIX session conformance tests
//
// Every test targets the specific conformance gap it names; the comments
// note what the pre-Phase-2 implementation did wrong so a future regression
// shows up as a named failure rather than a silent behaviour drift.
// ===========================================================================
namespace {

// Parse one complete frame from a collected wire string.
bool parse_frame(const std::string &wire, Message &out) {
    StreamParser p;
    p.feed(wire.data(), wire.size());
    return p.next(out);
}

std::string field_of(const Message &m, TagNum tag) {
    return std::string(m.get(tag).value_or(""));
}

// Feed a peer Logon addressed to this session (identity-valid).
void feed_logon(Session &sess, SeqNum seq,
                std::initializer_list<std::pair<TagNum, std::string_view>> extra = {
                    {tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}}) {
    std::string wire = make_wire("FIX.4.2", msg_types::Logon, seq, "CLIENT", "SERVER", extra);
    sess.on_data(wire.data(), wire.size());
}

void feed_wire(Session &sess, const std::string &wire) {
    sess.on_data(wire.data(), wire.size());
}

// Acceptor handshake: logon() -> WaitingLogon, peer Logon(seq1) -> Active.
// Afterwards: next_target == 2, next_sender == 3.
void handshake_acceptor(Session &sess) {
    sess.logon();
    feed_logon(sess, 1);
}

std::size_t count_msg_type(const std::vector<std::string> &frames, std::string_view mt) {
    std::size_t n = 0;
    for (const auto &w : frames) {
        Message m;
        if (parse_frame(w, m) && m.msg_type() == mt)
            ++n;
    }
    return n;
}

// Store whose store_outbound() can be told to fail — 2.6's exit criterion
// "store-failure aborts send without seq burn" cannot be exercised with
// MemoryStore, which never fails.
class FlakyStore : public IMessageStore {
public:
    SeqNum next_sender_seq_num() const noexcept override { return sender_; }
    SeqNum next_target_seq_num() const noexcept override { return target_; }
    void set_next_sender_seq_num(SeqNum n) override { sender_ = n; }
    void set_next_target_seq_num(SeqNum n) override { target_ = n; }
    void incr_sender_seq_num() override { ++sender_; }
    void incr_target_seq_num() override { ++target_; }
    Result<void> store_outbound(SeqNum, const std::string &) override {
        return fail_outbound ? make_unexpected(ErrorCode::StoreError) : Result<void>{};
    }
    Result<void> store_inbound(SeqNum, const std::string &) override { return {}; }
    Result<void> get_messages(SeqNum, SeqNum, MessageCallback) const override { return {}; }
    Result<void> reset() override {
        sender_ = 1;
        target_ = 1;
        return {};
    }
    Result<void> refresh() override { return {}; }

    bool fail_outbound = false;

private:
    SeqNum sender_ = 1;
    SeqNum target_ = 1;
};

} // namespace

// ---------------------------------------------------------------------------
// 2.1: SequenceReset is special-cased BEFORE sequence validation, in both
// modes, and never followed by a sequence increment.
// ---------------------------------------------------------------------------
TEST(SessionTest, SequenceResetGapFillAndResetModes) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);
    ASSERT_NE(sess.store(), nullptr);
    ASSERT_EQ(sess.store()->next_target_seq_num(), 2u);

    const std::size_t sends_after_handshake = sent.size();

    // Gap-fill mode: MsgSeqNum == expected, NewSeqNo > expected. The old
    // code validated first, applied NewSeqNo and then advanced again ->
    // NextTarget = NewSeqNo+1 (off-by-one that desynced the whole session).
    feed_wire(
        sess,
        make_wire("FIX.4.2", msg_types::SequenceReset, 2, "CLIENT", "SERVER",
                  {{tags::PossDupFlag, "Y"}, {tags::GapFillFlag, "Y"}, {tags::NewSeqNo, "5"}}));
    EXPECT_EQ(sess.state(), SessionState::Active);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 5u)
        << "gap-fill must set NextTarget exactly to NewSeqNo (no +1 advance)";

    // Reset mode: MsgSeqNum is ignored by design — the old code judged 999 a
    // sequence gap and issued a ResendRequest instead of resetting.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::SequenceReset, 999, "CLIENT", "SERVER",
                              {{tags::NewSeqNo, "7"}}));
    EXPECT_EQ(sess.state(), SessionState::Active);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 7u)
        << "reset mode must apply NewSeqNo regardless of MsgSeqNum";

    // Neither variant may have produced a Logout or a ResendRequest.
    EXPECT_EQ(sent.size(), sends_after_handshake) << "SequenceReset must not emit anything";
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 0u);
    EXPECT_EQ(count_msg_type(sent, msg_types::Logout), 0u);
}

TEST(SessionTest, GapFillRequiresPossDupFlag) {
    std::vector<std::string> sent;
    bool disconnect_called = false;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);

    // A gap-fill without PossDupFlag=Y is protocol-invalid (2.1).
    feed_wire(sess, make_wire("FIX.4.2", msg_types::SequenceReset, 2, "CLIENT", "SERVER",
                              {{tags::GapFillFlag, "Y"}, {tags::NewSeqNo, "5"}}));

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
    bool found = false;
    for (const auto &w : sent) {
        Message m;
        if (parse_frame(w, m) && m.msg_type() == msg_types::Logout &&
            field_of(m, tags::Text).find("PossDupFlag") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found) << "non-PossDup gap-fill must be answered with a Logout";
    // Nothing was applied: the expected sequence stayed put.
    EXPECT_EQ(sess.store()->next_target_seq_num(), 2u);
}

TEST(SessionTest, GapFillInvalidNewSeqNoDisconnects) {
    std::vector<std::string> sent;
    bool disconnect_called = false;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);

    // NewSeqNo must be strictly greater than the expected sequence (2.1):
    // NewSeqNo == expected would "fill" nothing and wedge the sequence.
    feed_wire(
        sess,
        make_wire("FIX.4.2", msg_types::SequenceReset, 2, "CLIENT", "SERVER",
                  {{tags::PossDupFlag, "Y"}, {tags::GapFillFlag, "Y"}, {tags::NewSeqNo, "2"}}));

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
    bool found = false;
    for (const auto &w : sent) {
        Message m;
        if (parse_frame(w, m) && m.msg_type() == msg_types::Logout &&
            field_of(m, tags::Text).find("invalid NewSeqNo") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 2u) << "invalid NewSeqNo must not be applied";
}

// ---------------------------------------------------------------------------
// 2.2: ResendRequest replay — gap-fill admin runs, re-tagged app messages,
// no sender-sequence consumption, BeginSeqNo bounds.
// ---------------------------------------------------------------------------
TEST(SessionTest, ResendRequestGapFillsAdminAndRetagsAppMessages) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess); // stores: 1=Logon, 2=Logon; next_sender=3

    // 3=Heartbeat (admin), 4=NewOrderSingle (app), 5=Heartbeat (admin).
    Message hb(msg_types::Heartbeat);
    ASSERT_TRUE(sess.send(hb).has_value());
    Message order(msg_types::NewOrderSingle);
    order.set(tags::ClOrdID, "ORD-R1");
    ASSERT_TRUE(sess.send(order).has_value());
    const std::string order_frame = sent.back();
    Message stored_order;
    ASSERT_TRUE(parse_frame(order_frame, stored_order));
    const std::string orig_time = field_of(stored_order, tags::SendingTime);
    ASSERT_FALSE(orig_time.empty());
    Message hb2(msg_types::Heartbeat);
    ASSERT_TRUE(sess.send(hb2).has_value());
    ASSERT_EQ(sess.store()->next_sender_seq_num(), 6u);

    const std::size_t before = sent.size();
    feed_wire(sess, make_wire("FIX.4.2", msg_types::ResendRequest, 2, "CLIENT", "SERVER",
                              {{tags::BeginSeqNo, "1"}, {tags::EndSeqNo, "0"}}));
    ASSERT_EQ(sent.size(), before + 3u)
        << "expected leading gap-fill + app message + trailing gap-fill";

    Message m;
    // 1) leading gap-fill: the two Logons and the Heartbeat are admin and are
    //    coalesced into ONE SequenceReset (34=1, 36=4).
    ASSERT_TRUE(parse_frame(sent[before], m));
    EXPECT_EQ(m.msg_type(), msg_types::SequenceReset);
    EXPECT_EQ(field_of(m, tags::GapFillFlag), "Y");
    EXPECT_EQ(field_of(m, tags::PossDupFlag), "Y");
    EXPECT_EQ(field_of(m, tags::MsgSeqNum), "1");
    EXPECT_EQ(field_of(m, tags::NewSeqNo), "4");

    // 2) the app message re-emitted at its own seq with PossDupFlag=Y and
    //    OrigSendingTime = the original SendingTime (2.2).
    ASSERT_TRUE(parse_frame(sent[before + 1], m));
    EXPECT_EQ(m.msg_type(), msg_types::NewOrderSingle);
    EXPECT_EQ(field_of(m, tags::PossDupFlag), "Y");
    EXPECT_EQ(field_of(m, tags::MsgSeqNum), "4");
    EXPECT_EQ(field_of(m, tags::OrigSendingTime), orig_time);
    EXPECT_EQ(field_of(m, tags::ClOrdID), "ORD-R1");

    // 3) trailing gap-fill covering the final Heartbeat.
    ASSERT_TRUE(parse_frame(sent[before + 2], m));
    EXPECT_EQ(m.msg_type(), msg_types::SequenceReset);
    EXPECT_EQ(field_of(m, tags::MsgSeqNum), "5");
    EXPECT_EQ(field_of(m, tags::NewSeqNo), "6");

    // The replay must not burn a single sender sequence number (otherwise the
    // store and the peer diverge after every resend).
    EXPECT_EQ(sess.store()->next_sender_seq_num(), 6u);
}

TEST(SessionTest, ResendRequestBeginSeqNoBeyondNextSenderRejected) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess); // next_sender == 3
    ASSERT_EQ(sess.store()->next_sender_seq_num(), 3u);

    const std::size_t before = sent.size();
    feed_wire(sess, make_wire("FIX.4.2", msg_types::ResendRequest, 2, "CLIENT", "SERVER",
                              {{tags::BeginSeqNo, "10"}, {tags::EndSeqNo, "0"}}));

    // Exactly one frame goes out: a session Reject, no gap-fill replay.
    ASSERT_EQ(sent.size(), before + 1u);
    Message m;
    ASSERT_TRUE(parse_frame(sent[before], m));
    EXPECT_EQ(m.msg_type(), msg_types::Reject);
    EXPECT_EQ(field_of(m, tags::RefSeqNum), "2");
    EXPECT_EQ(field_of(m, tags::SessionRejectReason), "5"); // ValueIncorrect
    EXPECT_EQ(field_of(m, tags::RefMsgType), "2");
    EXPECT_EQ(field_of(m, tags::RefTagID), "7"); // BeginSeqNo
    EXPECT_EQ(count_msg_type(sent, msg_types::SequenceReset), 0u);
    EXPECT_EQ(sess.state(), SessionState::Active)
        << "a bad ResendRequest must not kill the session";
}

TEST(SessionTest, SequenceGapThrottledToOneResendRequest) {
    std::vector<std::string> sent;
    int gap_events = 0;
    SeqNum last_expected = 0;
    SeqNum last_received = 0;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_sequence_gap = [&](const SessionID &, SeqNum expected, SeqNum received) {
        ++gap_events;
        last_expected = expected;
        last_received = received;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess); // next_target == 2

    // Out-of-sequence burst: the old code emitted one ResendRequest PER
    // message; gap_open_ must collapse the burst into a single request.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 3, "CLIENT", "SERVER"));
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 1u);
    EXPECT_EQ(gap_events, 1);
    EXPECT_EQ(last_expected, 2u);
    EXPECT_EQ(last_received, 3u);

    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 4, "CLIENT", "SERVER"));
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 1u) << "second gap must be throttled";
    EXPECT_EQ(gap_events, 2) << "the callback still reports every gap";

    // A message landing on the expected sequence closes the gap...
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 2, "CLIENT", "SERVER"));
    EXPECT_EQ(sess.store()->next_target_seq_num(), 3u);
    EXPECT_EQ(sess.state(), SessionState::Active);

    // ...so the NEXT gap opens a fresh ResendRequest.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 5, "CLIENT", "SERVER"));
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 2u);
    EXPECT_EQ(gap_events, 3);
}

// ---------------------------------------------------------------------------
// 2.3: virtual-clock timers — TestRequest probe, 1.2x deadline, logout
// timeout with heartbeats while LogoutSent.
// ---------------------------------------------------------------------------
TEST(SessionTest, TestRequestDeadlineIsOnePointTwoHeartbeats) {
    std::vector<std::string> sent;
    bool timeout_called = false;
    bool disconnect_called = false;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_heartbeat_timeout = [&](const SessionID &) {
        timeout_called = true;
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs); // hb = 30 s

    // Virtual clock MUST be installed before the session is driven so the
    // handshake baselines land on t=0.
    Session::Millis fake = 0;
    sess.set_time_source_for_test([&fake] { return fake; });
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);
    ASSERT_EQ(sent.size(), 2u);

    // One silent interval: Heartbeat (send-idle) + TestRequest (recv-idle).
    fake = 30'000;
    sess.on_timer();
    EXPECT_EQ(count_msg_type(sent, msg_types::TestRequest), 1u);
    bool has_req_id = false;
    for (const auto &w : sent) {
        Message m;
        if (parse_frame(w, m) && m.msg_type() == msg_types::TestRequest &&
            !field_of(m, tags::TestReqID).empty())
            has_req_id = true;
    }
    EXPECT_TRUE(has_req_id) << "TestRequest must carry a TestReqID";
    EXPECT_EQ(count_msg_type(sent, msg_types::Heartbeat), 1u);
    EXPECT_EQ(sess.state(), SessionState::Active);

    // Well before 1.2xHeartBtInt after the probe the session must survive —
    // the old code disconnected on the very next timer tick after sending
    // the TestRequest (deadline measured from last_recv, not test_req_sent).
    fake = 40'000;
    sess.on_timer();
    EXPECT_EQ(sess.state(), SessionState::Active);
    EXPECT_FALSE(disconnect_called);

    // Exactly 1.2 x 30 s = 36 s after the TestRequest: unanswered -> drop.
    fake = 66'000; // 30'000 (probe) + 36'000 (deadline)
    sess.on_timer();
    EXPECT_TRUE(timeout_called) << "on_heartbeat_timeout must fire at 1.2xHeartBtInt";
    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
}

TEST(SessionTest, LogoutTimeoutDisconnectsAndKeepsHeartbeats) {
    std::vector<std::string> sent;
    bool disconnect_called = false;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    SessionConfig cfg = make_cfg(false);
    cfg.logout_timeout = 60; // give the heartbeat observation a window
    auto store = std::make_unique<MemoryStore>();
    Session sess(cfg, std::move(store), nullptr, cbs);

    Session::Millis fake = 0;
    sess.set_time_source_for_test([&fake] { return fake; });
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);

    ASSERT_TRUE(sess.logout("bye").has_value());
    ASSERT_EQ(sess.state(), SessionState::LogoutSent);

    // Heartbeats keep flowing while we await the peer's Logout (2.3: the old
    // timer skipped LogoutSent entirely).
    fake = 30'000;
    sess.on_timer();
    EXPECT_EQ(count_msg_type(sent, msg_types::Heartbeat), 1u);
    EXPECT_EQ(sess.state(), SessionState::LogoutSent);
    EXPECT_FALSE(disconnect_called);

    // logout_timeout was dead config — a peer that never answered kept the
    // session pinned in LogoutSent forever.
    fake = 60'000;
    sess.on_timer();
    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
}

// ---------------------------------------------------------------------------
// 2.4: state gates + duplicate Logon.
// ---------------------------------------------------------------------------
TEST(SessionTest, SendAndLogoutAreGatedByState) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);

    // NotConnected: nothing may go out (the old code happily serialised and
    // stored messages for a session that could never transmit them).
    Message hb(msg_types::Heartbeat);
    auto r = sess.send(hb);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::SessionError));
    auto lr = sess.logout("x");
    ASSERT_FALSE(lr.has_value());
    EXPECT_EQ(lr.error(), make_error_code(ErrorCode::SessionError));
    EXPECT_EQ(sess.store()->next_sender_seq_num(), 1u) << "rejected send burned a seq";

    // WaitingLogon: still not Active.
    ASSERT_TRUE(sess.logon().has_value());
    ASSERT_EQ(sess.state(), SessionState::WaitingLogon);
    ASSERT_FALSE(sess.send(hb).has_value());
    ASSERT_FALSE(sess.logon().has_value()) << "logon() must not re-fire while connected";

    // Active: sends flow.
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);
    ASSERT_TRUE(sess.send(hb).has_value());
    EXPECT_EQ(sess.store()->next_sender_seq_num(), 4u);
}

TEST(SessionTest, DuplicateLogonLogoutsAndDisconnects) {
    std::vector<std::string> sent;
    bool disconnect_called = false;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);

    // Spec: a Logon on an already-active session -> Logout + disconnect.
    // (The old code silently ignored it, letting a peer re-handshake over a
    // live session.)
    feed_logon(sess, 2);
    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
    bool found = false;
    for (const auto &w : sent) {
        Message m;
        if (parse_frame(w, m) && m.msg_type() == msg_types::Logout &&
            field_of(m, tags::Text) == "Duplicate Logon")
            found = true;
    }
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// 2.5: handle_reject + missing-MsgType Reject.
// ---------------------------------------------------------------------------
TEST(SessionTest, RejectDuringLogonSentTerminatesHandshake) {
    std::vector<std::string> sent;
    bool disconnect_called = false;
    std::string logout_reason;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logout = [&](const SessionID &, std::string_view reason) {
        logout_reason = reason;
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(true), std::move(store), nullptr, cbs); // initiator
    ASSERT_TRUE(sess.logon().has_value());
    ASSERT_EQ(sess.state(), SessionState::LogonSent);

    // A peer Reject answering our Logon must end the handshake immediately —
    // the old handle_reject was empty, leaving the session stuck in
    // LogonSent until logon_timeout expired.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Reject, 1, "CLIENT", "SERVER",
                              {{tags::RefSeqNum, "1"},
                               {tags::SessionRejectReason, "3"},
                               {tags::Text, "Logon refused"}}));

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
    EXPECT_NE(logout_reason.find("Logon rejected"), std::string::npos) << logout_reason;
    EXPECT_NE(logout_reason.find("Logon refused"), std::string::npos) << logout_reason;
}

TEST(SessionTest, RejectWhileActiveSurfacesOnRejectCallback) {
    std::vector<std::string> sent;
    int reject_calls = 0;
    SeqNum reject_ref = 0;
    std::string reject_text;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_reject = [&](const SessionID &, SeqNum ref_seq, std::string_view text) {
        ++reject_calls;
        reject_ref = ref_seq;
        reject_text = text;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);

    // Outside the handshake a peer Reject is surfaced, not fatal (2.5).
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Reject, 2, "CLIENT", "SERVER",
                              {{tags::RefSeqNum, "2"},
                               {tags::SessionRejectReason, "99"},
                               {tags::Text, "nope"}}));
    EXPECT_EQ(reject_calls, 1);
    EXPECT_EQ(reject_ref, 2u);
    EXPECT_EQ(reject_text, "nope");
    EXPECT_EQ(sess.state(), SessionState::Active);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 3u) << "the Reject itself must be consumed";
}

TEST(SessionTest, MissingMsgTypeRejectedWithRefSeqNumAndConsumed) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);
    ASSERT_EQ(sess.store()->next_target_seq_num(), 2u);

    // A well-framed message with NO MsgType (35) — BodyLength/CheckSum are
    // computed by MessageBuilder, only the 35= field itself is omitted.
    std::string body;
    body += std::string("49=CLIENT") + SOH;
    body += std::string("56=SERVER") + SOH;
    body += std::string("34=2") + SOH;
    body += std::string("52=20240101-12:00:00.000") + SOH;
    feed_wire(sess, MessageBuilder::build("FIX.4.2", body));

    // 2.5: RefSeqNum is mandatory (old code sent 0 = omitted) AND the
    // sequence must be consumed (old code rejected without consuming ->
    // the peer resent the same frame forever).
    bool found = false;
    for (const auto &w : sent) {
        Message m;
        if (parse_frame(w, m) && m.msg_type() == msg_types::Reject) {
            found = true;
            EXPECT_EQ(field_of(m, tags::RefSeqNum), "2") << "Reject must carry a valid RefSeqNum";
            EXPECT_EQ(field_of(m, tags::SessionRejectReason), "1"); // RequiredTagMissing
            EXPECT_EQ(field_of(m, tags::RefTagID), "35");
            EXPECT_EQ(field_of(m, tags::Text), "Missing MsgType");
        }
    }
    EXPECT_TRUE(found) << "missing MsgType must be rejected";
    EXPECT_EQ(sess.store()->next_target_seq_num(), 3u) << "the bad message must be consumed";
    EXPECT_EQ(sess.state(), SessionState::Active);
}

// ---------------------------------------------------------------------------
// 2.6: exception boundary around dispatch — the sequence advance is
// guaranteed even when the user callback throws.
// ---------------------------------------------------------------------------
TEST(SessionTest, ThrowingOnMessageStillAdvancesSequenceAndReportsError) {
    int on_message_calls = 0;
    int error_calls = 0;
    std::string last_error;
    SessionCallbacks cbs;
    cbs.on_message = [&](const SessionID &, const Message &) {
        ++on_message_calls;
        if (on_message_calls == 1)
            throw std::runtime_error("boom");
    };
    cbs.on_error = [&](const SessionID &, std::string_view what) {
        ++error_calls;
        last_error = what;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);
    ASSERT_EQ(sess.store()->next_target_seq_num(), 2u);

    // The throw must be contained (IO thread survives) AND the sequence must
    // still advance — the old code skipped incr on unwind, so the peer's
    // next message looked like a gap -> permanent ResendRequest storm.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::NewOrderSingle, 2, "CLIENT", "SERVER",
                              {{tags::ClOrdID, "ORD-T1"}}));
    EXPECT_EQ(on_message_calls, 1);
    EXPECT_EQ(error_calls, 1);
    EXPECT_EQ(last_error, "boom");
    EXPECT_EQ(sess.error_count(), 1u);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 3u) << "throwing callback must not stall seq";
    EXPECT_EQ(sess.state(), SessionState::Active) << "the session must survive the throw";

    // A follow-up message dispatches normally.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::NewOrderSingle, 3, "CLIENT", "SERVER",
                              {{tags::ClOrdID, "ORD-T2"}}));
    EXPECT_EQ(on_message_calls, 2);
    EXPECT_EQ(error_calls, 1);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 4u);
}

// ---------------------------------------------------------------------------
// 2.7: dictionary validation on inbound app messages.
// ---------------------------------------------------------------------------
TEST(SessionTest, DictionaryValidationRejectsBadAppMessage) {
    auto dict = std::make_shared<DataDictionary>();
    dict->load_builtin(FixVersion::FIX_4_2);

    std::vector<std::string> sent;
    int dispatched = 0;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_message = [&](const SessionID &, const Message &) {
        ++dispatched;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), dict.get(), cbs);
    handshake_acceptor(sess); // admin messages bypass validation
    ASSERT_EQ(sess.state(), SessionState::Active);
    const std::size_t sends_after_handshake = sent.size();

    // Missing required fields (HandlInst/Symbol/Side/TransactTime/OrderQty/
    // OrdType) -> BusinessMessageReject (35=j), never dispatched.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::NewOrderSingle, 2, "CLIENT", "SERVER",
                              {{tags::ClOrdID, "ORD-BAD"}}));
    ASSERT_EQ(sent.size(), sends_after_handshake + 1u) << "expected exactly one 35=j";
    Message m;
    ASSERT_TRUE(parse_frame(sent.back(), m));
    EXPECT_EQ(m.msg_type(), msg_types::BusinessMessageReject);
    EXPECT_EQ(field_of(m, tags::RefSeqNum), "2");
    EXPECT_EQ(field_of(m, tags::BusinessRejectReason), "5"); // Business Reject
    EXPECT_EQ(field_of(m, tags::RefMsgType), msg_types::NewOrderSingle);
    EXPECT_FALSE(field_of(m, tags::Text).empty()) << "35=j must say what failed";
    EXPECT_EQ(dispatched, 0) << "an invalid message must not reach the application";
    EXPECT_EQ(sess.state(), SessionState::Active) << "35=j must not kill the session";
    EXPECT_EQ(sess.store()->next_target_seq_num(), 3u);

    // A conforming NewOrderSingle passes validation and is dispatched.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::NewOrderSingle, 3, "CLIENT", "SERVER",
                              {{tags::ClOrdID, "ORD-GOOD"},
                               {tags::HandlInst, "1"},
                               {tags::Symbol, "AAPL"},
                               {tags::Side, "1"},
                               {tags::TransactTime, "20240101-12:00:00.000"},
                               {tags::OrderQty, "100"},
                               {tags::OrdType, "2"}}));
    EXPECT_EQ(dispatched, 1);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 4u);

    // MsgTypes the dictionary does not define are NOT validation failures —
    // they are dispatched as-is (News is outside the builtin 15 types).
    feed_wire(sess, make_wire("FIX.4.2", msg_types::News, 4, "CLIENT", "SERVER",
                              {{tags::Text, "headline"}}));
    EXPECT_EQ(dispatched, 2) << "undefined MsgTypes must skip validation, not bounce";
    EXPECT_EQ(sess.store()->next_target_seq_num(), 5u);
}

// ---------------------------------------------------------------------------
// 2.6 exit criterion: a store failure aborts the send WITHOUT burning the
// sequence number (MemoryStore never fails, hence the injected store).
// ---------------------------------------------------------------------------
TEST(SessionTest, StoreFailureAbortsSendWithoutBurningSequence) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<FlakyStore>();
    FlakyStore *flaky = store.get();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);
    ASSERT_EQ(sess.store()->next_sender_seq_num(), 3u);

    flaky->fail_outbound = true;
    const std::uint64_t sent_before = sess.msgs_sent();
    const std::size_t wire_before = sent.size();
    Message hb(msg_types::Heartbeat);
    auto r = sess.send(hb);
    ASSERT_FALSE(r.has_value()) << "send() must surface the store failure";
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::StoreError));
    EXPECT_EQ(sess.store()->next_sender_seq_num(), 3u)
        << "failed persist must not burn the sequence (permanent gap otherwise)";
    EXPECT_EQ(sess.msgs_sent(), sent_before);
    EXPECT_EQ(sent.size(), wire_before);

    // The very same sequence number is used once the store recovers.
    flaky->fail_outbound = false;
    Message hb2(msg_types::Heartbeat);
    ASSERT_TRUE(sess.send(hb2).has_value());
    EXPECT_EQ(sess.store()->next_sender_seq_num(), 4u);
    EXPECT_EQ(sent.size(), wire_before + 1u);
}

// ---------------------------------------------------------------------------
// 2.8: Logon / HeartBtInt conformance.
// ---------------------------------------------------------------------------
TEST(SessionTest, LogonRequiresPositiveHeartBtInt) {
    auto run_case = [](std::optional<std::string_view> hb_value, const char *expected_reason) {
        std::vector<std::string> sent;
        bool disconnect_called = false;
        SessionCallbacks cbs;
        cbs.do_send = [&](const std::string &s) {
            sent.push_back(s);
        };
        cbs.do_disconnect = [&]() {
            disconnect_called = true;
        };

        auto store = std::make_unique<MemoryStore>();
        Session sess(make_cfg(false), std::move(store), nullptr, cbs);
        sess.logon();
        ASSERT_EQ(sess.state(), SessionState::WaitingLogon);

        // FIX 4.2 mandates HeartBtInt on Logon; 0 (or absent) previously kept
        // a stale interval or heartbeated every tick. Built directly (not via
        // a named initializer_list, whose backing array would dangle).
        std::string wire;
        if (hb_value)
            wire = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                             {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, *hb_value}});
        else
            wire = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                             {{tags::EncryptMethod, "0"}});
        sess.on_data(wire.data(), wire.size());

        EXPECT_EQ(sess.state(), SessionState::Disconnected);
        EXPECT_TRUE(disconnect_called);
        bool found = false;
        for (const auto &w : sent) {
            Message m;
            if (parse_frame(w, m) && m.msg_type() == msg_types::Logout &&
                field_of(m, tags::Text) == expected_reason)
                found = true;
        }
        EXPECT_TRUE(found) << "expected Logout with reason '" << expected_reason << "'";
    };

    run_case(std::nullopt, "HeartBtInt required");
    run_case("0", "HeartBtInt required");
}

TEST(SessionTest, InitiatorChecksHeartBtIntEcho) {
    std::vector<std::string> sent;
    bool disconnect_called = false;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    // The acceptor must confirm the interval WE proposed (cfg: 30 s) — two
    // different liveness schedules would each wait for the other's traffic.
    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(true), std::move(store), nullptr, cbs); // initiator, hb=30
    ASSERT_TRUE(sess.logon().has_value());
    ASSERT_EQ(sess.state(), SessionState::LogonSent);

    feed_logon(sess, 1, {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "15"}});
    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
    bool found = false;
    for (const auto &w : sent) {
        Message m;
        if (parse_frame(w, m) && m.msg_type() == msg_types::Logout &&
            field_of(m, tags::Text) == "HeartBtInt mismatch")
            found = true;
    }
    EXPECT_TRUE(found);

    // The agreeing case completes the handshake.
    sent.clear();
    disconnect_called = false;
    auto store2 = std::make_unique<MemoryStore>();
    Session sess2(make_cfg(true), std::move(store2), nullptr, cbs);
    ASSERT_TRUE(sess2.logon().has_value());
    feed_logon(sess2, 1, {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
    EXPECT_EQ(sess2.state(), SessionState::Active);
    EXPECT_FALSE(disconnect_called);
}

TEST(SessionTest, PeerHeartBtIntAdoptionDrivesTimer) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs); // cfg hb = 30 s

    Session::Millis fake = 0;
    sess.set_time_source_for_test([&fake] { return fake; });
    sess.logon();
    // Peer proposes 45 s: adopt it (and echo it in our Logon response) instead
    // of keeping our 30 s schedule.
    feed_logon(sess, 1, {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "45"}});
    ASSERT_EQ(sess.state(), SessionState::Active);

    Message m;
    ASSERT_TRUE(parse_frame(sent.back(), m));
    EXPECT_EQ(m.msg_type(), msg_types::Logon);
    EXPECT_EQ(field_of(m, tags::HeartBtInt), "45") << "our Logon must echo the adopted interval";

    // 40 s < adopted 45 s: no traffic yet (a non-adopting session would have
    // probed at 30 s).
    const std::size_t before = sent.size();
    fake = 40'000;
    sess.on_timer();
    EXPECT_EQ(sent.size(), before) << "timer must follow the ADOPTED HeartBtInt";

    // At 45 s the adopted interval elapses: Heartbeat + TestRequest probe.
    fake = 45'000;
    sess.on_timer();
    EXPECT_EQ(sent.size(), before + 2u);
    EXPECT_EQ(count_msg_type(sent, msg_types::Heartbeat), 1u);
    EXPECT_EQ(count_msg_type(sent, msg_types::TestRequest), 1u);
}
