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
// dup-header Logon received in WaitingLogon is rejected (reason 13) and the
// session does NOT go Active.
//
// F3: the Reject DOES consume the sequence (same gate as the dispatch
// advance — `seq_validated || !poss_dup`, shared via consume_inbound). The
// pre-F3 variant refused to consume anything in a logon state, so a session
// that rejected a message kept awaiting the exact same MsgSeqNum forever.
// Session-level Rejects are the response to that message: it is answered and
// taken, exactly like an accepted copy of it would be.
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
    // F3: the Reject consumes the rejected message's slot (shared gate with
    // the dispatch advance), so the session is NOT left awaiting seq 1 it has
    // already answered.
    EXPECT_EQ(sess.store()->next_target_seq_num(), target_before + 1)
        << "a session-level Reject must consume the sequence (F3)";
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
    int logout_calls = 0;
    std::string logout_reason;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };
    cbs.on_logout = [&](const SessionID &, std::string_view r) {
        ++logout_calls;
        logout_reason = r;
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
    // #8: the app must learn the session ended at the timeout (it used to
    // only see on_logout for a COMPLETED logout and hung releasing resources).
    EXPECT_EQ(logout_calls, 1) << "logout timeout notifies exactly once";
    EXPECT_EQ(logout_reason, std::string("logout timeout"));
    // A late peer Logout after the timeout must not double-notify.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Logout, 2, "CLIENT", "SERVER"));
    EXPECT_EQ(logout_calls, 1) << "notify_logout gate suppresses the late duplicate";
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
    // F8: validate_fields now defaults to OFF (HandlInst is marked required
    // by our FIX 4.2 dictionary yet is officially optional in FIX 4.4+, so a
    // default-on flag bounced legitimate cross-version traffic). Tests that
    // exercise 2.7 must opt in explicitly — this is that test.
    SessionConfig cfg = make_cfg(false);
    cfg.validate_fields = true;
    Session sess(cfg, std::move(store), dict.get(), cbs);
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
    // F11: BusinessRejectReason 0 = "Other" (the specifics live in 58).
    // The old assertion pinned 5, which the FIX tables define as "Duplicate"
    // — a wrong reason, not a generic "Business Reject".
    EXPECT_EQ(field_of(m, tags::BusinessRejectReason), "0"); // Other
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

// ===========================================================================
// Phase-3 review round (F1-F12): one named regression per finding. The
// comments state what the pre-review implementation did wrong so a future
// regression fails as a named test instead of drifting silently.
// ===========================================================================

// ---------------------------------------------------------------------------
// F1: ResetSeqNumFlag (141=Y) on OUR outbound Logon must rewind the store
// BEFORE the frame is numbered. The old code only tagged the Logon, so a
// pre-advanced store emitted "34=500" and the peer's very next seq-1 message
// was answered "MsgSeqNum too low" — the handshake wedged forever.
// ---------------------------------------------------------------------------
TEST(SessionTest, ResetOnLogonRewindsStoreBeforeNumberingTheLogon) {
    std::vector<std::string> sent;
    int logon_calls = 0;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
        ++logon_calls;
    };

    auto store = std::make_unique<MemoryStore>();
    MemoryStore *raw = store.get();
    for (SeqNum i = 1; i < 500; ++i) // next_sender -> 500
        raw->incr_sender_seq_num();
    raw->set_next_target_seq_num(500);

    SessionConfig cfg = make_cfg(/*initiator=*/true);
    cfg.reset_on_logon = true;
    Session sess(cfg, std::move(store), nullptr, cbs);

    ASSERT_TRUE(sess.logon().has_value());
    ASSERT_EQ(sent.size(), 1u);
    Message m;
    ASSERT_TRUE(parse_frame(sent[0], m));
    EXPECT_EQ(m.msg_type(), msg_types::Logon);
    EXPECT_EQ(field_of(m, tags::ResetSeqNumFlag), "Y");
    EXPECT_EQ(field_of(m, tags::MsgSeqNum), "1") << "141=Y must renumber the Logon itself (F1)";
    EXPECT_EQ(raw->next_sender_seq_num(), 2u) << "the reset baseline must survive the send";
    EXPECT_EQ(raw->next_target_seq_num(), 1u);
    EXPECT_EQ(sess.state(), SessionState::LogonSent);

    // The acceptor answers from ITS fresh baseline. Pre-fix our expectation
    // was still 500, so seq 1 read as 499 messages too low -> Logout.
    feed_logon(sess, 1);
    EXPECT_EQ(sess.state(), SessionState::Active) << "peer's seq-1 Logon must be accepted (F1)";
    EXPECT_EQ(logon_calls, 1);
    EXPECT_EQ(raw->next_target_seq_num(), 2u);
}

// ---------------------------------------------------------------------------
// F3: the duplicate-header Reject and the dispatch advance share ONE gate
// (consume_inbound). In Active state an in-sequence dup-header message is
// rejected AND consumed; the old code rejected without consuming, so the
// peer's next correct message looked like a permanent gap — re-request,
// re-send, re-reject loop.
// ---------------------------------------------------------------------------
TEST(SessionTest, ActiveInSequenceDuplicateHeaderRejectConsumesSequence) {
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

    // Heartbeat at the EXPECTED seq carrying a duplicated SenderCompID.
    MessageBuilder b;
    b.begin("FIX.4.2", msg_types::Heartbeat);
    b.add(tags::SenderCompID, "CLIENT");
    b.add(tags::SenderCompID, "CLIENT"); // duplicate standard-header tag
    b.add(tags::TargetCompID, "SERVER");
    b.add(tags::MsgSeqNum, std::int64_t(2));
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    const std::string wire = b.finish();
    ASSERT_FALSE(wire.empty());
    feed_wire(sess, wire);

    EXPECT_EQ(count_msg_type(sent, msg_types::Reject), 1u);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 3u)
        << "a session-level Reject must consume the slot it answered (F3)";
    EXPECT_EQ(sess.state(), SessionState::Active);

    // The decisive assertion: the peer's NEXT message is in sequence. Pre-fix
    // the target was still 2, so seq 3 opened a gap and a ResendRequest.
    const std::size_t frames_before = sent.size();
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 3, "CLIENT", "SERVER"));
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 0u)
        << "no phantom gap after a rejected-but-consumed message (F3)";
    EXPECT_EQ(sent.size(), frames_before) << "no reply is due for an in-sequence Heartbeat";
    EXPECT_EQ(sess.store()->next_target_seq_num(), 4u);
}

// ---------------------------------------------------------------------------
// F4: BeginSeqNo is 1-based. A stray BeginSeqNo=0 used to slip past the
// `begin > range_end` guard and drive the replay cursor to 0, emitting
// gap-fill frames with 34=0 — an impossible MsgSeqNum.
// ---------------------------------------------------------------------------
TEST(SessionTest, ResendRequestWithZeroBeginSeqNoRejected) {
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
                              {{tags::BeginSeqNo, "0"}, {tags::EndSeqNo, "0"}}));

    ASSERT_EQ(sent.size(), before + 1u) << "exactly one Reject, no replay";
    Message m;
    ASSERT_TRUE(parse_frame(sent[before], m));
    EXPECT_EQ(m.msg_type(), msg_types::Reject);
    EXPECT_EQ(field_of(m, tags::RefSeqNum), "2");
    EXPECT_EQ(field_of(m, tags::SessionRejectReason), "5"); // ValueIncorrect
    EXPECT_EQ(field_of(m, tags::RefMsgType), msg_types::ResendRequest);
    EXPECT_EQ(field_of(m, tags::RefTagID), "7"); // BeginSeqNo
    EXPECT_EQ(count_msg_type(sent, msg_types::SequenceReset), 0u)
        << "a cursor of 0 must never reach gap-fill emission (F4)";
    EXPECT_EQ(sess.state(), SessionState::Active);
}

// ---------------------------------------------------------------------------
// F6: the handshake state must be published BEFORE send_message(). do_send
// can loop a peer's answer back into this session faster than logon()
// returns; the old order left handle_logon reading NotConnected/Disconnected
// and dropping the Logon ("stale Logon — ignore"), after which the timer
// waited out logon_timeout on a session that had already exchanged Logons.
// ---------------------------------------------------------------------------
TEST(SessionTest, LogonResponseDuringSendLoopbackActivatesSession) {
    std::vector<std::string> sent;
    int logon_calls = 0;
    Session *sess_ptr = nullptr; // bound after construction, captured by ref
    bool feed_answer = false;

    SessionCallbacks cbs;
    cbs.on_logon = [&](const SessionID &) {
        ++logon_calls;
    };
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
        if (feed_answer) {
            // Loopback: the acceptor's reply arrives while our own Logon is
            // still inside send_logon().
            feed_answer = false;
            std::string answer = make_wire("FIX.4.2", msg_types::Logon, 1, "CLIENT", "SERVER",
                                           {{tags::EncryptMethod, "0"}, {tags::HeartBtInt, "30"}});
            sess_ptr->on_data(answer.data(), answer.size());
        }
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(/*initiator=*/true), std::move(store), nullptr, cbs);
    sess_ptr = &sess;

    feed_answer = true;
    ASSERT_TRUE(sess.logon().has_value());
    EXPECT_EQ(sess.state(), SessionState::Active)
        << "the loopback answer must activate the session, not be dropped (F6)";
    EXPECT_EQ(logon_calls, 1) << "on_logon must fire exactly once (F6)";
    ASSERT_EQ(sent.size(), 1u) << "our Logon only; the answer arrives inbound";
    Message m;
    ASSERT_TRUE(parse_frame(sent[0], m));
    EXPECT_EQ(m.msg_type(), msg_types::Logon);
    EXPECT_EQ(field_of(m, tags::MsgSeqNum), "1");
}

// ---------------------------------------------------------------------------
// F6: a Logon that never reached the wire must leave the FSM retryable.
// The old code published LogonSent/WaitingLogon unconditionally, so a store
// failure parked the session in a handshake state with no Logon on the wire.
// ---------------------------------------------------------------------------
TEST(SessionTest, LogonSendFailureLeavesSessionRetryable) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<FlakyStore>();
    FlakyStore *flaky = store.get();
    flaky->fail_outbound = true;

    Session sess(make_cfg(/*initiator=*/true), std::move(store), nullptr, cbs);
    auto r = sess.logon();
    ASSERT_FALSE(r.has_value()) << "logon() must surface a failed Logon send (F6)";
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::StoreError));
    EXPECT_TRUE(sent.empty()) << "no Logon may reach the wire";
    EXPECT_EQ(sess.state(), SessionState::NotConnected)
        << "a failed Logon must not leave the FSM stuck in LogonSent (F6)";

    // Retryable from the pre-logon state once the store recovers.
    flaky->fail_outbound = false;
    ASSERT_TRUE(sess.logon().has_value());
    EXPECT_EQ(sess.state(), SessionState::LogonSent);
    EXPECT_EQ(sent.size(), 1u);
}

// ---------------------------------------------------------------------------
// F6: on_logon may only fire for a handshake that actually completed. An
// acceptor whose Logon response failed to send used to report success anyway.
// ---------------------------------------------------------------------------
TEST(SessionTest, AcceptorLogonSendFailureSuppressesOnLogon) {
    std::vector<std::string> sent;
    int logon_calls = 0;
    std::string last_error;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
        ++logon_calls;
    };
    cbs.on_error = [&](const SessionID &, std::string_view what) {
        last_error.assign(what.data(), what.size());
    };

    auto store = std::make_unique<FlakyStore>();
    FlakyStore *flaky = store.get();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    ASSERT_TRUE(sess.logon().has_value());
    ASSERT_EQ(sess.state(), SessionState::WaitingLogon);

    flaky->fail_outbound = true; // the response Logon cannot be persisted
    feed_logon(sess, 1);

    EXPECT_EQ(logon_calls, 0) << "on_logon must be suppressed for a failed handshake (F6)";
    EXPECT_EQ(sess.state(), SessionState::WaitingLogon);
    EXPECT_NE(last_error.find("logon race"), std::string::npos)
        << "the failure must surface on on_error, got: " << last_error;
    EXPECT_GE(sess.error_count(), 1u);
}

// ---------------------------------------------------------------------------
// F8: dictionary validation of inbound app messages is opt-in. HandlInst is
// marked required by our FIX 4.2 dictionary yet is optional in FIX 4.4+, so
// the old default-on flag bounced legitimate cross-version traffic.
// ---------------------------------------------------------------------------
TEST(SessionTest, ValidateFieldsDefaultsToOff) {
    SessionConfig cfg;
    EXPECT_FALSE(cfg.validate_fields)
        << "2.7 validation must be opt-in (F8); DictionaryValidationRejectsBadAppMessage opts in";
}

// ---------------------------------------------------------------------------
// F9: an open gap must not rely on the single ResendRequest request_gap()
// fired. Re-arm every 1.2xHeartBtInt while the gap stays open, then give up
// loudly after kMaxGapRetries total attempts instead of hanging as a zombie
// Active session with exactly one request ever sent.
// ---------------------------------------------------------------------------
TEST(SessionTest, GapRetriesUpToBudgetThenDisconnects) {
    std::vector<std::string> sent;
    int gap_events = 0;
    bool disconnect_called = false;
    std::string last_error;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_sequence_gap = [&](const SessionID &, SeqNum, SeqNum) {
        ++gap_events;
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };
    cbs.on_error = [&](const SessionID &, std::string_view what) {
        last_error.assign(what.data(), what.size());
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs); // hb = 30 s

    Session::Millis fake = 0;
    sess.set_time_source_for_test([&fake] { return fake; });
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);
    ASSERT_EQ(sess.store()->next_target_seq_num(), 2u);

    // Open a gap: attempt #1 goes out immediately (attempt 1 of kMaxGapRetries).
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 3, "CLIENT", "SERVER"));
    ASSERT_EQ(count_msg_type(sent, msg_types::ResendRequest), 1u);

    // Before the 1.2x spacing elapses there is no re-arm: the spacing IS the
    // anti-flood guarantee (1 heartbeat = 30 s < 36 s retry interval).
    fake = 30'000;
    sess.on_timer();
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 1u)
        << "no retry inside the 1.2xHeartBtInt spacing (F9)";
    EXPECT_EQ(sess.state(), SessionState::Active);

    // Each full spacing re-arms; the peer is kept alive with an out-of-seq
    // Heartbeat (still > expected, so it neither closes nor restarts the gap).
    const int kMaxGapRetries = 5;
    for (int cycle = 1; cycle < kMaxGapRetries; ++cycle) { // attempts 2..5
        feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 3, "CLIENT", "SERVER"));
        fake += 36'000; // 1.2 x 30 s
        sess.on_timer();
        ASSERT_EQ(count_msg_type(sent, msg_types::ResendRequest),
                  static_cast<std::size_t>(cycle + 1))
            << "cycle " << cycle << " must re-arm exactly one ResendRequest (F9)";
        ASSERT_EQ(sess.state(), SessionState::Active) << "budget not exhausted yet";
    }
    ASSERT_EQ(count_msg_type(sent, msg_types::ResendRequest), 5u) << "initial + 4 retries";
    EXPECT_EQ(gap_events, 5) << "every out-of-seq feed reports a gap, requests stay throttled";

    // Attempt 6 exceeds the budget: report + disconnect instead of hanging.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 3, "CLIENT", "SERVER"));
    fake += 36'000;
    sess.on_timer();
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 5u) << "no 6th request is sent";
    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called);
    EXPECT_GE(sess.error_count(), 1u);
    EXPECT_NE(last_error.find("kMaxGapRetries"), std::string::npos)
        << "budget exhaustion must be loud, got: " << last_error;

    // Every retry addressed the still-open range (BeginSeqNo = expected seq).
    int checked = 0;
    for (const auto &w : sent) {
        Message m;
        if (parse_frame(w, m) && m.msg_type() == msg_types::ResendRequest) {
            EXPECT_EQ(field_of(m, tags::BeginSeqNo), "2");
            EXPECT_EQ(field_of(m, tags::EndSeqNo), "0");
            ++checked;
        }
    }
    EXPECT_EQ(checked, 5);
}

// ---------------------------------------------------------------------------
// F12: RefSeqNum (45) is MANDATORY on a session Reject — 0 when the reference
// sequence is unknown. The old `if (ref_seq > 0)` omitted the tag entirely for
// a message with no usable MsgSeqNum.
// ---------------------------------------------------------------------------
TEST(SessionTest, MissingMsgSeqNumRejectCarriesRefSeqNumZero) {
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

    // Well-framed message with NO MsgSeqNum (34) at all.
    std::string body;
    body += std::string("35=0") + SOH;
    body += std::string("49=CLIENT") + SOH;
    body += std::string("56=SERVER") + SOH;
    body += std::string("52=20240101-12:00:00.000") + SOH;
    feed_wire(sess, MessageBuilder::build("FIX.4.2", body));

    ASSERT_EQ(count_msg_type(sent, msg_types::Reject), 1u);
    Message m;
    for (const auto &w : sent) {
        if (parse_frame(w, m) && m.msg_type() == msg_types::Reject)
            break;
    }
    EXPECT_EQ(field_of(m, tags::RefSeqNum), "0")
        << "45 must be present (0 = unknown), never omitted (F12)";
    EXPECT_EQ(field_of(m, tags::SessionRejectReason), "1"); // RequiredTagMissing
    EXPECT_EQ(field_of(m, tags::RefTagID), "34");           // MsgSeqNum
    // Nothing validated → nothing consumed: the sequence state is untouched.
    EXPECT_EQ(sess.store()->next_target_seq_num(), 2u);
    EXPECT_EQ(sess.state(), SessionState::Active);
}

// ---------------------------------------------------------------------------
// F5: 43 (PossDupFlag) and 122 (OrigSendingTime) are standard HEADER tags but
// used to be emitted from the body loop, i.e. after 52 SendingTime, on every
// resend replay. Header emission order is 34, 43, 122, 52 — asserted on the
// raw bytes, not through the parser (which would hide the order).
// ---------------------------------------------------------------------------
TEST(SessionTest, ResendReplayEmitsHeaderTagsInStandardOrder) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess); // stores 1=Logon, 2=Logon

    Message order(msg_types::NewOrderSingle);
    order.set(tags::ClOrdID, "ORD-HDR");
    ASSERT_TRUE(sess.send(order).has_value());
    Message stored;
    ASSERT_TRUE(parse_frame(sent.back(), stored));
    ASSERT_FALSE(field_of(stored, tags::SendingTime).empty());

    const std::size_t before = sent.size();
    feed_wire(sess, make_wire("FIX.4.2", msg_types::ResendRequest, 2, "CLIENT", "SERVER",
                              {{tags::BeginSeqNo, "1"}, {tags::EndSeqNo, "0"}}));
    ASSERT_GT(sent.size(), before + 1u);

    const auto pos = [](const std::string &frame, std::string_view tag) {
        return frame.find(std::string(1, SOH) + std::string(tag) + "=");
    };

    // 1) leading gap-fill: 43 present, no 122, order 34 < 43 < 52.
    {
        const std::string &frame = sent[before];
        const auto i34 = pos(frame, "34");
        const auto i43 = pos(frame, "43");
        const auto i52 = pos(frame, "52");
        ASSERT_NE(i34, std::string::npos);
        ASSERT_NE(i43, std::string::npos);
        ASSERT_NE(i52, std::string::npos);
        EXPECT_LT(i34, i43);
        EXPECT_LT(i43, i52);
        EXPECT_EQ(pos(frame, "122"), std::string::npos) << "no OrigSendingTime on a gap-fill";
    }

    // 2) re-tagged app message: 43 AND 122 present, both before 52.
    {
        const std::string &frame = sent[before + 1];
        const auto i34 = pos(frame, "34");
        const auto i43 = pos(frame, "43");
        const auto i122 = pos(frame, "122");
        const auto i52 = pos(frame, "52");
        ASSERT_NE(i34, std::string::npos);
        ASSERT_NE(i43, std::string::npos);
        ASSERT_NE(i122, std::string::npos);
        ASSERT_NE(i52, std::string::npos);
        EXPECT_LT(i34, i43) << "34 precedes 43 (F5)";
        EXPECT_LT(i43, i122) << "43 precedes 122 (F5)";
        EXPECT_LT(i122, i52) << "122 must NOT trail 52 (F5)";
    }
}

// ---------------------------------------------------------------------------
// F2: a peer-initiated Logout must actually TEAR DOWN the connection. The old
// peer branch only stored Disconnected — on_timer has no work for that state
// and do_disconnect never ran, so the transport FD (and the accept loop for a
// raw peer) stayed pinned forever: state said "disconnected", the socket did
// not.
// ---------------------------------------------------------------------------
TEST(SessionTest, PeerInitiatedLogoutClosesConnection) {
    std::vector<std::string> sent;
    int logout_calls = 0;
    bool disconnect_called = false;
    std::string logout_reason;

    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.on_logon = [&](const SessionID &) {
    };
    cbs.on_logout = [&](const SessionID &, std::string_view reason) {
        ++logout_calls;
        logout_reason.assign(reason.data(), reason.size());
    };
    cbs.do_disconnect = [&]() {
        disconnect_called = true;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);

    feed_wire(sess, make_wire("FIX.4.2", msg_types::Logout, 2, "CLIENT", "SERVER",
                              {{tags::Text, "Goodbye"}}));

    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_TRUE(disconnect_called)
        << "the transport must be dropped on a peer-initiated Logout (F2)";
    EXPECT_EQ(count_msg_type(sent, msg_types::Logout), 1u) << "the echo still goes out (F2)";
    EXPECT_EQ(logout_calls, 1) << "exactly one on_logout, not one per branch (F2)";
    EXPECT_EQ(logout_reason, std::string("Goodbye"));
}

// ===========================================================================
// Follow-up round: Phase 2 re-verification blockers.
// ===========================================================================

// Blocker 2: F3's regression must pin the REPORTED scenario — an IN-SEQUENCE
// *PossDup* with a duplicated header in Active. The pre-fix gate
// (seq_validated && !poss_dup) rejected-but-never-consumed exactly this
// message; the earlier non-PossDup test passed even with the fix reverted.
TEST(SessionTest, ActivePossDupDuplicateHeaderRejectConsumesSequence) {
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

    MessageBuilder b;
    b.begin("FIX.4.2", msg_types::Heartbeat);
    b.add(tags::PossDupFlag, "Y"); // the case the old gate let through
    b.add(tags::SenderCompID, "CLIENT");
    b.add(tags::SenderCompID, "CLIENT"); // duplicate standard-header tag
    b.add(tags::TargetCompID, "SERVER");
    b.add(tags::MsgSeqNum, std::int64_t(2));
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    const std::string wire = b.finish();
    ASSERT_FALSE(wire.empty());
    feed_wire(sess, wire);

    EXPECT_EQ(count_msg_type(sent, msg_types::Reject), 1u);
    EXPECT_EQ(sess.store()->next_target_seq_num(), 3u)
        << "in-sequence PossDup + dup header must be consumed (F3 loop)";
    EXPECT_EQ(sess.state(), SessionState::Active);

    // Decisive: the peer's next message is in sequence — no phantom RR.
    const std::size_t before = sent.size();
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 3, "CLIENT", "SERVER"));
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 0u);
    EXPECT_EQ(sent.size(), before);
}

// Blocker 1: the COMPLETED (mutual) logout must tear the transport down —
// the F2 contract covers both handle_logout branches, and a late peer Logout
// after teardown must not double-notify (notify_logout gate).
TEST(SessionTest, MutualLogoutClosesTransportAndNotifiesOnce) {
    std::vector<std::string> sent;
    int disconnect_calls = 0;
    int logout_calls = 0;
    std::string logout_reason;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };
    cbs.do_disconnect = [&] {
        ++disconnect_calls;
    };
    cbs.on_logout = [&](const SessionID &, std::string_view r) {
        ++logout_calls;
        logout_reason = r;
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);

    ASSERT_TRUE(sess.logout("Bye").has_value());
    ASSERT_EQ(sess.state(), SessionState::LogoutSent);
    ASSERT_EQ(disconnect_calls, 0) << "still waiting for the peer's ack";

    // Peer acks our Logout (in sequence: handshake consumed 1, ours is 2).
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Logout, 2, "CLIENT", "SERVER",
                              {{tags::Text, "Ack"}}));
    EXPECT_EQ(sess.state(), SessionState::Disconnected);
    EXPECT_EQ(disconnect_calls, 1) << "mutual logout must drop the FD (re-verify blocker 1)";
    EXPECT_EQ(logout_calls, 1);
    EXPECT_EQ(logout_reason, std::string("Ack"));

    // Late duplicate after teardown: echo may still be attempted, but the
    // app must NOT see a second on_logout.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Logout, 3, "CLIENT", "SERVER"));
    EXPECT_EQ(logout_calls, 1) << "notify_logout gate suppresses duplicates";
}

// Adjacent to F4: a ResendRequest without its required BeginSeqNo gets a
// session Reject instead of a silent drop (the peer's gap stayed unanswered).
TEST(SessionTest, ResendRequestMissingBeginSeqNoRejected) {
    std::vector<std::string> sent;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(false), std::move(store), nullptr, cbs);
    handshake_acceptor(sess);
    ASSERT_EQ(sess.state(), SessionState::Active);

    MessageBuilder b;
    b.begin("FIX.4.2", msg_types::ResendRequest);
    b.add(tags::SenderCompID, "CLIENT");
    b.add(tags::TargetCompID, "SERVER");
    b.add(tags::MsgSeqNum, std::int64_t(2));
    b.add(tags::EndSeqNo, std::int64_t(0)); // required tag 7 absent
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    const std::string wire = b.finish();
    ASSERT_FALSE(wire.empty());
    feed_wire(sess, wire);

    EXPECT_EQ(count_msg_type(sent, msg_types::Reject), 1u);
    EXPECT_EQ(count_msg_type(sent, msg_types::SequenceReset), 0u)
        << "no replay without a BeginSeqNo";

    // The RR was still a normal in-sequence message: consumed, so seq 3 is
    // in sequence and no phantom gap opens.
    feed_wire(sess, make_wire("FIX.4.2", msg_types::Heartbeat, 3, "CLIENT", "SERVER"));
    EXPECT_EQ(count_msg_type(sent, msg_types::ResendRequest), 0u);
}

// #4: concurrent logon() races must emit exactly ONE Logon frame — the loser
// hits send_logon's duplicate-initiator guard instead of double-sending (and,
// with reset_on_logon, double-resetting the store).
TEST(SessionTest, ConcurrentLogonEmitsSingleLogonFrame) {
    std::vector<std::string> sent;
    std::mutex mtx;
    SessionCallbacks cbs;
    cbs.do_send = [&](const std::string &s) {
        std::lock_guard lk(mtx);
        sent.push_back(s);
    };

    auto store = std::make_unique<MemoryStore>();
    Session sess(make_cfg(/*initiator=*/true), std::move(store), nullptr, cbs);

    std::atomic<int> oks{0};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            if (sess.logon().has_value())
                ++oks;
        });
    for (auto &t : threads)
        t.join();

    std::lock_guard lk(mtx);
    EXPECT_EQ(count_msg_type(sent, msg_types::Logon), 1u)
        << "exactly one Logon may leave a racing initiator (#4)";
    EXPECT_EQ(oks.load(), 1) << "exactly one caller wins the handshake";
    EXPECT_EQ(sess.state(), SessionState::LogonSent);
    EXPECT_EQ(sess.store()->next_sender_seq_num(), 2u)
        << "no second numbering round (double reset would keep this at 1)";
}
