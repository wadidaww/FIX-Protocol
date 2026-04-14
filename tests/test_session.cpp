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
