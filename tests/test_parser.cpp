// =============================================================================
// FIX Protocol Engine - Parser unit tests
// =============================================================================
#include <gtest/gtest.h>
#include "fix/parser/parser.hpp"
#include "fix/parser/serializer.hpp"
#include "fix/core/constants.hpp"

using namespace fix;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string make_logon_42() {
    // Build a minimal FIX 4.2 Logon message manually
    MessageBuilder b;
    b.begin("FIX.4.2", "A");
    b.add(tags::SenderCompID, "SENDER");
    b.add(tags::TargetCompID, "TARGET");
    b.add(tags::MsgSeqNum, std::int64_t(1));
    b.add(tags::SendingTime, "20240101-12:00:00.000");
    b.add(tags::EncryptMethod, std::int64_t(0));
    b.add(tags::HeartBtInt, std::int64_t(30));
    return b.finish();
}

// ---------------------------------------------------------------------------
// Parser tests
// ---------------------------------------------------------------------------
TEST(ParserTest, ParsesCompleteLogon) {
    std::string wire = make_logon_42();
    ASSERT_FALSE(wire.empty());

    StreamParser p;
    p.feed(wire.data(), wire.size());

    Message msg;
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
    EXPECT_EQ(msg.get(tags::SenderCompID).value_or(""), "SENDER");
    EXPECT_EQ(msg.get(tags::TargetCompID).value_or(""), "TARGET");
    EXPECT_EQ(msg.seq_num(), SeqNum(1));
}

TEST(ParserTest, ParsesBeginString) {
    std::string wire = make_logon_42();
    StreamParser p;
    p.feed(wire.data(), wire.size());

    Message msg;
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.begin_string_version(), FixVersion::FIX_4_2);
}

TEST(ParserTest, HandlesPartialFeed) {
    std::string wire = make_logon_42();
    StreamParser p;

    // Feed in small chunks of 3 bytes
    Message msg;
    bool got = false;
    for (std::size_t i = 0; i < wire.size(); i += 3) {
        std::size_t chunk = std::min(std::size_t(3), wire.size() - i);
        p.feed(wire.data() + i, chunk);
        if (p.next(msg)) { got = true; break; }
    }
    EXPECT_TRUE(got);
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
}

TEST(ParserTest, MultipleConcatenatedMessages) {
    std::string wire1 = make_logon_42();
    std::string wire2 = make_logon_42();
    std::string combined = wire1 + wire2;

    StreamParser p;
    p.feed(combined.data(), combined.size());

    Message m1, m2;
    ASSERT_TRUE(p.next(m1));
    ASSERT_TRUE(p.next(m2));
    EXPECT_EQ(m1.msg_type(), msg_types::Logon);
    EXPECT_EQ(m2.msg_type(), msg_types::Logon);

    Message m3;
    EXPECT_FALSE(p.next(m3));
}

TEST(ParserTest, BadChecksumRejected) {
    std::string wire = make_logon_42();

    // Corrupt checksum: last 4 bytes before SOH = "10=NNN\x01"
    // Find "10=" in wire
    auto pos = wire.rfind("10=");
    ASSERT_NE(pos, std::string::npos);
    wire[pos + 3] = (wire[pos + 3] == '0') ? '1' : '0'; // flip a digit

    StreamParser::Options opts;
    opts.validate_checksum = true;
    StreamParser p(opts);
    p.feed(wire.data(), wire.size());

    Message msg;
    EXPECT_FALSE(p.next(msg));
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::BadChecksum));
}

TEST(ParserTest, NoChecksumValidation) {
    std::string wire = make_logon_42();
    auto pos = wire.rfind("10=");
    ASSERT_NE(pos, std::string::npos);
    wire[pos + 3] = (wire[pos + 3] == '0') ? '1' : '0';

    StreamParser::Options opts;
    opts.validate_checksum = false;
    opts.validate_body_len = false;
    StreamParser p(opts);
    p.feed(wire.data(), wire.size());

    Message msg;
    EXPECT_TRUE(p.next(msg)); // should succeed with validation off
}

TEST(ParserTest, ResetClearsState) {
    std::string wire = make_logon_42();
    StreamParser p;
    p.feed(wire.data(), wire.size() / 2); // partial feed
    p.reset();
    p.feed(wire.data(), wire.size()); // full feed after reset

    Message msg;
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
}

// ---------------------------------------------------------------------------
// Serializer tests
// ---------------------------------------------------------------------------
TEST(SerializerTest, ChecksumIsCorrect) {
    std::string wire = make_logon_42();
    ASSERT_FALSE(wire.empty());

    // Find "10=NNN\x01"
    auto pos = wire.rfind("10=");
    ASSERT_NE(pos, std::string::npos);

    // Parse claimed checksum
    std::string cs_str = wire.substr(pos + 3, 3);
    int claimed = std::stoi(cs_str);

    // Compute checksum over everything before "10="
    int sum = 0;
    for (std::size_t i = 0; i < pos; ++i)
        sum += static_cast<unsigned char>(wire[i]);
    int expected = sum % 256;

    EXPECT_EQ(claimed, expected);
}

TEST(SerializerTest, BodyLengthIsCorrect) {
    std::string wire = make_logon_42();

    // Find "9=NNN\x01"
    auto tag9 = wire.find("9=");
    ASSERT_NE(tag9, std::string::npos);
    auto soh9 = wire.find('\x01', tag9);
    ASSERT_NE(soh9, std::string::npos);
    int body_len = std::stoi(wire.substr(tag9 + 2, soh9 - tag9 - 2));

    // Body starts after "9=NNN\x01" and ends before "10=NNN\x01"
    auto body_start = soh9 + 1;
    auto cs_pos     = wire.rfind("10=");
    ASSERT_NE(cs_pos, std::string::npos);

    int actual_body = static_cast<int>(cs_pos - body_start);
    EXPECT_EQ(body_len, actual_body);
}

TEST(SerializerTest, RoundTrip) {
    // Build a message, serialize it, parse it back
    std::string wire = make_logon_42();
    StreamParser p;
    p.feed(wire.data(), wire.size());
    Message msg;
    ASSERT_TRUE(p.next(msg));

    EXPECT_EQ(msg.get(tags::SenderCompID).value_or(""), "SENDER");
    EXPECT_EQ(msg.get(tags::TargetCompID).value_or(""), "TARGET");
    EXPECT_EQ(msg.get(tags::EncryptMethod).value_or(""), "0");
    EXPECT_EQ(msg.get(tags::HeartBtInt).value_or(""), "30");
}

TEST(SerializerTest, TimestampFormat) {
    auto ts = MessageBuilder::format_timestamp_now();
    // Format: YYYYMMDD-HH:MM:SS.sss
    EXPECT_EQ(ts.size(), 21u);
    EXPECT_EQ(ts[8], '-');
    EXPECT_EQ(ts[11], ':');
    EXPECT_EQ(ts[14], ':');
    EXPECT_EQ(ts[17], '.');
}

TEST(SerializerTest, SerializeMessage) {
    Message order(msg_types::NewOrderSingle);
    order.set(tags::ClOrdID, "ORD001");
    order.set(tags::Symbol,  "AAPL");
    order.set(tags::Side,    "1");
    order.set(tags::OrderQty, 100.0);
    order.set(tags::OrdType, "2");
    order.set(tags::TransactTime, "20240101-12:00:00.000");

    MessageBuilder b;
    std::string wire = b.serialize(order, "FIX.4.2", 42,
                                    "SENDER", "TARGET",
                                    "20240101-12:00:00.000");
    EXPECT_FALSE(wire.empty());
    EXPECT_NE(wire.find("8=FIX.4.2"), std::string::npos);
    EXPECT_NE(wire.find("35=D"),      std::string::npos);
    EXPECT_NE(wire.find("11=ORD001"), std::string::npos);
    EXPECT_NE(wire.find("55=AAPL"),   std::string::npos);
    EXPECT_NE(wire.find("34=42"),     std::string::npos);
}
