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
