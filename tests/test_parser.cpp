// =============================================================================
// FIX Protocol Engine - Parser unit tests
// =============================================================================
#include "fix/core/constants.hpp"
#include "fix/parser/parser.hpp"
#include "fix/parser/serializer.hpp"

#include <gtest/gtest.h>

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

// Build a complete raw frame (correct BodyLength AND CheckSum) around `body`.
static std::string raw_frame(const std::string &body) {
    std::string f = "8=FIX.4.2";
    f += SOH;
    f += "9=";
    f += std::to_string(body.size());
    f += SOH;
    f += body;
    unsigned sum = 0;
    for (unsigned char c : f)
        sum += c;
    sum %= 256;
    f += "10=";
    f += static_cast<char>('0' + (sum / 100) % 10);
    f += static_cast<char>('0' + (sum / 10) % 10);
    f += static_cast<char>('0' + sum % 10);
    f += SOH;
    return f;
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
        if (p.next(msg)) {
            got = true;
            break;
        }
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
    auto cs_pos = wire.rfind("10=");
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
    order.set(tags::Symbol, "AAPL");
    order.set(tags::Side, "1");
    order.set(tags::OrderQty, 100.0);
    order.set(tags::OrdType, "2");
    order.set(tags::TransactTime, "20240101-12:00:00.000");

    MessageBuilder b;
    std::string wire =
        b.serialize(order, "FIX.4.2", 42, "SENDER", "TARGET", "20240101-12:00:00.000");
    EXPECT_FALSE(wire.empty());
    EXPECT_NE(wire.find("8=FIX.4.2"), std::string::npos);
    EXPECT_NE(wire.find("35=D"), std::string::npos);
    EXPECT_NE(wire.find("11=ORD001"), std::string::npos);
    EXPECT_NE(wire.find("55=AAPL"), std::string::npos);
    EXPECT_NE(wire.find("34=42"), std::string::npos);
}

// ===========================================================================
// Robustness tests — production-hardening Phase 1B
//   C6: parser never stalls on garbage, backlog stays bounded
//   C7: duplicate standard-header tags / repeating-group body tags
//   C8: SOH/NUL field-injection never reaches the wire
// ===========================================================================

// --- C6: garbage prefix is resync-scanned, message behind it delivered ----
TEST(ParserTest, GarbagePrefixResyncsAndDeliversMessage) {
    std::string garbage = "<<<GARBAGE>>> not-a-fix-frame ......................";
    EXPECT_EQ(garbage.find("8="), std::string::npos); // no accidental frame start
    std::string input = garbage + make_logon_42();

    StreamParser p;
    p.feed(input.data(), input.size());

    Message msg;
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
    EXPECT_GE(p.resync_count(), 1u);              // garbage was discarded
    EXPECT_EQ(p.last_error(), std::error_code()); // cleared on success

    Message extra;
    EXPECT_FALSE(p.next(extra));
}

// --- C6: >max_msg_len of garbage stays within the buffer cap --------------
TEST(ParserTest, GarbageBacklogStaysBounded) {
    StreamParser::Options opts;
    opts.max_msg_len = 1024;
    StreamParser p(opts);
    const std::size_t cap = opts.max_msg_len + 4096; // max + in-flight slack

    std::string garbage(8192, 'X'); // far above the cap, no frame start
    p.feed(garbage.data(), garbage.size());

    EXPECT_LE(p.buffered(), cap); // unbounded growth impossible
    EXPECT_GT(p.resync_count(), 0u);
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::ParseError));
    EXPECT_TRUE(p.healthy()); // eager resync reclaims — not fatal
    Message msg;
    EXPECT_FALSE(p.next(msg));

    // Still fully functional afterwards.
    std::string wire = make_logon_42();
    p.feed(wire.data(), wire.size());
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
}

// --- C6: huge BodyLength claim skips the frame, next message parses -------
TEST(ParserTest, OversizedBodyLengthSkipsFrame) {
    std::string bad = "8=FIX.4.2";
    bad += SOH;
    bad += "9=99999999"; // claims ~100 MB
    bad += SOH;
    bad += "35=0";
    bad += SOH;
    bad += "10=000";
    bad += SOH;

    StreamParser p;
    p.feed(bad.data(), bad.size());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::BadLength));
    EXPECT_GE(p.resync_count(), 1u);
    Message msg;
    EXPECT_FALSE(p.next(msg));

    std::string good = make_logon_42();
    p.feed(good.data(), good.size());
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
    EXPECT_FALSE(p.next(msg));
}

// --- C6: complete frame above max_msg_len skipped wholesale ---------------
TEST(ParserTest, CompleteOversizedFrameSkippedWholesale) {
    StreamParser::Options opts;
    opts.max_msg_len = 128;
    StreamParser p(opts);

    // Valid frame (right BodyLength/checksum) but its body is 500 bytes.
    std::string frame = "8=FIX.4.2";
    frame += SOH;
    frame += "9=500";
    frame += SOH;
    frame += "35=0";
    frame += SOH;
    frame.append(495, 'A');
    frame += "10=000";
    frame += SOH;
    ASSERT_EQ(frame.size(), 523u);

    p.feed(frame.data(), frame.size());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::BadLength));
    Message msg;
    EXPECT_FALSE(p.next(msg));

    std::string good = make_logon_42();
    p.feed(good.data(), good.size());
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
}

// --- C6: malformed field (non-numeric tag / tag overflow) recovery --------
TEST(ParserTest, MalformedFieldsRecoverToNextMessage) {
    // Frame 1: body field with a non-numeric tag ("abc=zz").
    std::string body1 = "35=0";
    body1 += SOH;
    body1 += "abc=zz";
    body1 += SOH;
    // Frame 2: tag overflowing TagNum (uint32).
    std::string body2 = "35=0";
    body2 += SOH;
    body2 += "99999999999=zz";
    body2 += SOH;

    std::string bad = raw_frame(body1) + raw_frame(body2);
    std::string good = make_logon_42();

    StreamParser p;
    p.feed(bad.data(), bad.size());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::InvalidField));
    Message msg;
    EXPECT_FALSE(p.next(msg));

    p.feed(good.data(), good.size());
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
    EXPECT_FALSE(p.next(msg));
}

// --- C6: missing CheckSum trailer; following message still parses ---------
// (Frame is fed together with a valid message because the trailer check
// needs body_end + 7 bytes to be buffered before it can fire.)
TEST(ParserTest, MissingChecksumTrailerRecovers) {
    std::string bad = "8=FIX.4.2";
    bad += SOH;
    bad += "9=5";
    bad += SOH;
    bad += "35=0";
    bad += SOH; // body complete, trailer absent
    std::string input = bad + make_logon_42();

    StreamParser p;
    p.feed(input.data(), input.size());

    // Exactly the valid Logon is delivered — the trailer-less frame is not.
    Message msg;
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
    EXPECT_EQ(msg.get(tags::SenderCompID).value_or(""), "SENDER");
    EXPECT_FALSE(p.next(msg));
}

// --- C6: malformed BodyLength value rejected, recovery --------------------
TEST(ParserTest, MalformedBodyLengthRejected) {
    std::string bad = "8=FIX.4.2";
    bad += SOH;
    bad += "9=zzz"; // non-numeric BodyLength
    bad += SOH;
    bad += "35=0";
    bad += SOH; // pad to >= minimum frame length

    StreamParser p;
    p.feed(bad.data(), bad.size());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::BadLength));
    Message msg;
    EXPECT_FALSE(p.next(msg));

    std::string good = make_logon_42();
    p.feed(good.data(), good.size());
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
}

// --- C6: CheckSum value > 255 rejected (would wrap uint8) -----------------
TEST(ParserTest, ChecksumValueOver255Rejected) {
    std::string bad = make_logon_42();
    auto pos = bad.rfind("10=");
    ASSERT_NE(pos, std::string::npos);
    bad = bad.substr(0, pos) + "10=441" + SOH; // 441 > 255

    StreamParser p;
    p.feed(bad.data(), bad.size());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::BadChecksum));
    Message msg;
    EXPECT_FALSE(p.next(msg));

    std::string good = make_logon_42();
    p.feed(good.data(), good.size());
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
}

// --- C6: 4-digit CheckSum value rejected by positional trailer parse ------
TEST(ParserTest, OversizedChecksumTrailerRejected) {
    std::string bad = make_logon_42();
    auto pos = bad.rfind("10=");
    ASSERT_NE(pos, std::string::npos);
    bad = bad.substr(0, pos) + "10=0185" + SOH; // 4 digits, not "10=NNN\x01"

    StreamParser p;
    p.feed(bad.data(), bad.size());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::ParseError));
    Message msg;
    EXPECT_FALSE(p.next(msg));

    std::string good = make_logon_42();
    p.feed(good.data(), good.size());
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
    EXPECT_FALSE(p.next(msg));
}

// --- C6: corrupted (but well-formed) checksum rejects, recovery -----------
TEST(ParserTest, CorruptChecksumThenValidMessageParses) {
    std::string bad = make_logon_42();
    auto pos = bad.rfind("10=");
    ASSERT_NE(pos, std::string::npos);
    bad[pos + 3] = (bad[pos + 3] == '0') ? '1' : '0'; // flip a digit

    StreamParser p;
    p.feed(bad.data(), bad.size());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::BadChecksum));
    Message msg;
    EXPECT_FALSE(p.next(msg));

    std::string good = make_logon_42();
    p.feed(good.data(), good.size());
    ASSERT_TRUE(p.next(msg));
    EXPECT_EQ(msg.msg_type(), msg_types::Logon);
    EXPECT_FALSE(p.next(msg));
}

// --- C7: duplicate standard-header tag flagged, first occurrence wins -----
TEST(ParserTest, DuplicateHeaderFlaggedFirstWins) {
    MessageBuilder b;
    ASSERT_TRUE(b.begin("FIX.4.2", "A").has_value());
    ASSERT_TRUE(b.add(tags::SenderCompID, "SENDER").has_value());
    ASSERT_TRUE(b.add(tags::TargetCompID, "TARGET").has_value());
    ASSERT_TRUE(b.add(tags::MsgSeqNum, std::int64_t(5)).has_value());
    ASSERT_TRUE(b.add(tags::MsgSeqNum, std::int64_t(9)).has_value()); // duplicate 34
    ASSERT_TRUE(b.add(tags::SendingTime, "20240101-12:00:00.000").has_value());
    std::string wire = b.finish();
    ASSERT_FALSE(wire.empty());

    StreamParser p;
    p.feed(wire.data(), wire.size());

    Message msg;
    ASSERT_TRUE(p.next(msg)); // still delivered — session can Reject it
    EXPECT_TRUE(msg.has_duplicate_header());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::DuplicateField));
    // First occurrence wins for accessors and for stored fields.
    EXPECT_EQ(msg.seq_num(), SeqNum(5));
    EXPECT_EQ(msg.get(tags::MsgSeqNum).value_or(""), "5");
    int count = 0;
    for (const auto &f : msg.fields()) {
        if (f.tag == tags::MsgSeqNum)
            ++count;
    }
    EXPECT_EQ(count, 1);
}

// --- C7: repeating-group duplicate body tags both retained ---------------
TEST(ParserTest, RepeatingGroupTagsBothRetained) {
    MessageBuilder b;
    ASSERT_TRUE(b.begin("FIX.4.2", "D").has_value());
    ASSERT_TRUE(b.add(tags::ClOrdID, "ORD001").has_value());
    ASSERT_TRUE(b.add(tags::Symbol, "AAPL").has_value());
    ASSERT_TRUE(b.add(tags::Symbol, "MSFT").has_value()); // duplicate body tag
    ASSERT_TRUE(b.add(tags::Side, "1").has_value());
    std::string wire = b.finish();
    ASSERT_FALSE(wire.empty());

    StreamParser p;
    p.feed(wire.data(), wire.size());
    Message msg;
    ASSERT_TRUE(p.next(msg));

    EXPECT_EQ(msg.get(tags::Symbol).value_or(""), "AAPL"); // first wins
    auto all = msg.get_all(tags::Symbol);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0], "AAPL");
    EXPECT_EQ(all[1], "MSFT");
    int count = 0;
    for (const auto &f : msg.fields()) {
        if (f.tag == tags::Symbol)
            ++count;
    }
    EXPECT_EQ(count, 2);
    EXPECT_FALSE(msg.has_duplicate_header()); // body tag, not a header
    EXPECT_EQ(p.last_error(), std::error_code());
}

// --- C8: SOH injection rejected by the builder, nothing reaches the wire --
TEST(SerializerTest, SoHInjectionRejectedByBuilder) {
    MessageBuilder b;
    ASSERT_TRUE(b.begin("FIX.4.2", "A").has_value());

    std::string injected = "AAA";
    injected += SOH;
    injected += "55=HACKED"; // would inject a 55= field on the wire
    auto r = b.add(tags::Symbol, injected);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::InvalidField));
    EXPECT_EQ(b.last_error(), make_error_code(ErrorCode::InvalidField));

    // Sticky: later adds fail too and finish() refuses to emit a frame.
    EXPECT_FALSE(b.add(tags::Side, "1").has_value());
    EXPECT_TRUE(b.finish().empty());

    // NUL byte rejected the same way.
    MessageBuilder b2;
    ASSERT_TRUE(b2.begin("FIX.4.2", "A").has_value());
    std::string nul = "AA";
    nul += '\0';
    nul += "BB";
    EXPECT_FALSE(b2.add(tags::Symbol, nul).has_value());
    EXPECT_TRUE(b2.finish().empty());

    // Injection through msg_type at begin() rejected as well.
    MessageBuilder b3;
    std::string bad_mt = "A";
    bad_mt += SOH;
    bad_mt += "35=X";
    EXPECT_FALSE(b3.begin("FIX.4.2", bad_mt).has_value());
    EXPECT_TRUE(b3.finish().empty());

    // begin() re-arms after an error.
    ASSERT_TRUE(b3.begin("FIX.4.2", "A").has_value());
    EXPECT_FALSE(static_cast<bool>(b3.last_error()));
}

// --- C8: SOH smuggled into a Message can never be serialized --------------
TEST(SerializerTest, SoHInjectionNeverReachesWire) {
    Message m(msg_types::Heartbeat);
    ASSERT_TRUE(m.set(tags::ClOrdID, "ORD1").has_value());
    // Bypass set(): inject SOH directly into the stored field.
    std::string injected = "AAA";
    injected += SOH;
    injected += "55=HACKED";
    m.fields().emplace_back(tags::Symbol, injected);

    MessageBuilder b;
    std::string wire = b.serialize(m, "FIX.4.2", 7, "S", "T", "20240101-12:00:00.000");
    EXPECT_TRUE(wire.empty());
    EXPECT_EQ(b.last_error(), make_error_code(ErrorCode::InvalidField));

    // The strict variant surfaces the same error.
    MessageBuilder b2;
    auto r = b2.try_serialize(m, "FIX.4.2", 7, "S", "T", "20240101-12:00:00.000");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::InvalidField));
}

// ===========================================================================
// F13 — standard-header duplicate whitelist completeness
//   97 = PossResend (7 is BeginSeqNo: a ResendRequest BODY field),
//   142/143 Sender/TargetLocationID, 144/145 OnBehalfOf/DeliverToLocationID.
// ===========================================================================

namespace {

// A well-formed frame with `extra` fields appended (values may repeat tags).
std::string header_dup_frame(TagNum tag, std::string_view first, std::string_view second) {
    MessageBuilder b;
    (void)b.begin("FIX.4.2", msg_types::Heartbeat);
    (void)b.add(tags::SenderCompID, "SENDER");
    (void)b.add(tags::TargetCompID, "TARGET");
    (void)b.add(tags::MsgSeqNum, std::int64_t(5));
    (void)b.add(tags::SendingTime, "20240101-12:00:00.000");
    (void)b.add(tag, first);
    (void)b.add(tag, second);
    return b.finish();
}

} // namespace

TEST(ParserTest, DuplicatePossResend97FlaggedAsHeader) {
    // Tag 97 = PossResend — a genuine standard-header tag that the old
    // whitelist (8/9/35/49/50/56/57/115/128/90/91/34/43/122/52) omitted.
    std::string wire = header_dup_frame(tags::PossResend, "Y", "N");
    ASSERT_FALSE(wire.empty());

    StreamParser p;
    p.feed(wire.data(), wire.size());
    Message msg;
    ASSERT_TRUE(p.next(msg));
    EXPECT_TRUE(msg.has_duplicate_header());
    EXPECT_EQ(p.last_error(), make_error_code(ErrorCode::DuplicateField));
    EXPECT_EQ(msg.get(tags::PossResend).value_or(""), "Y"); // first wins
}

TEST(ParserTest, DuplicateLocationIdsFlaggedAsHeader) {
    for (const TagNum tag :
         {TagNum{tags::SenderLocationID}, TagNum{tags::TargetLocationID}, TagNum{144},
          TagNum{145}}) { // 144/145 = OnBehalfOf/DeliverToLocationID
        std::string wire = header_dup_frame(tag, "LOC1", "LOC2");
        ASSERT_FALSE(wire.empty());
        StreamParser p;
        p.feed(wire.data(), wire.size());
        Message msg;
        ASSERT_TRUE(p.next(msg)) << "tag " << tag;
        EXPECT_TRUE(msg.has_duplicate_header()) << "duplicate header tag " << tag << " missed";
        EXPECT_EQ(msg.get(tag).value_or(""), "LOC1") << "tag " << tag;
    }
}

TEST(ParserTest, BeginSeqNo7IsBodyNotHeader) {
    // Tag 7 = BeginSeqNo: a ResendRequest *body* field (the review's claim
    // that "7 = PossResendFlag" was wrong — that is tag 97). Duplicating it
    // must NOT be reported as duplicate standard-header (reason 13).
    std::string wire = header_dup_frame(tags::BeginSeqNo, "1", "2");
    ASSERT_FALSE(wire.empty());

    StreamParser p;
    p.feed(wire.data(), wire.size());
    Message msg;
    ASSERT_TRUE(p.next(msg));
    EXPECT_FALSE(msg.has_duplicate_header());
    EXPECT_EQ(p.last_error(), std::error_code());
    auto all = msg.get_all(tags::BeginSeqNo);
    ASSERT_EQ(all.size(), 2u); // both body occurrences retained (C7)
    EXPECT_EQ(all[0], "1");
    EXPECT_EQ(all[1], "2");
}
