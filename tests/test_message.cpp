// =============================================================================
// FIX Protocol Engine - Message unit tests
// =============================================================================
#include "fix/core/constants.hpp"
#include "fix/core/message.hpp"

#include <gtest/gtest.h>

using namespace fix;

TEST(MessageTest, SetAndGet) {
    Message m;
    m.set(tags::Symbol, "MSFT");
    EXPECT_EQ(m.get(tags::Symbol).value_or(""), "MSFT");
}

TEST(MessageTest, SetOverwrites) {
    Message m;
    m.set(tags::Symbol, "AAPL");
    m.set(tags::Symbol, "MSFT");
    EXPECT_EQ(m.get(tags::Symbol).value_or(""), "MSFT");
    // Only one instance
    int count = 0;
    for (const auto &f : m.fields()) {
        if (f.tag == tags::Symbol)
            ++count;
    }
    EXPECT_EQ(count, 1);
}

TEST(MessageTest, HasField) {
    Message m;
    EXPECT_FALSE(m.has(tags::Symbol));
    m.set(tags::Symbol, "GOOG");
    EXPECT_TRUE(m.has(tags::Symbol));
}

TEST(MessageTest, Remove) {
    Message m;
    m.set(tags::Symbol, "IBM");
    m.remove(tags::Symbol);
    EXPECT_FALSE(m.has(tags::Symbol));
}

TEST(MessageTest, GetInt) {
    Message m;
    m.set(tags::MsgSeqNum, std::int64_t(42));
    EXPECT_EQ(m.get_int(tags::MsgSeqNum).value_or(0), 42);
}

TEST(MessageTest, GetDouble) {
    Message m;
    m.set(tags::Price, 123.456);
    auto v = m.get_double(tags::Price);
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, 123.456, 0.001);
}

TEST(MessageTest, SeqNum) {
    Message m;
    m.set(tags::MsgSeqNum, std::int64_t(99));
    EXPECT_EQ(m.seq_num(), SeqNum(99));
}

TEST(MessageTest, MsgType) {
    Message m(msg_types::NewOrderSingle);
    EXPECT_EQ(m.msg_type(), msg_types::NewOrderSingle);
}

TEST(MessageTest, PossDupDefault) {
    Message m;
    EXPECT_FALSE(m.poss_dup());
}

TEST(MessageTest, PossDupTrue) {
    Message m;
    m.set(tags::PossDupFlag, "Y");
    EXPECT_TRUE(m.poss_dup());
}

TEST(MessageTest, SetBool) {
    Message m;
    m.set(tags::ResetSeqNumFlag, true);
    EXPECT_EQ(m.get(tags::ResetSeqNumFlag).value_or("N"), "Y");
    m.set(tags::ResetSeqNumFlag, false);
    EXPECT_EQ(m.get(tags::ResetSeqNumFlag).value_or("Y"), "N");
}

TEST(MessageTest, Clear) {
    Message m;
    m.set(tags::Symbol, "X");
    m.set(tags::Side, "1");
    m.clear();
    EXPECT_FALSE(m.has(tags::Symbol));
    EXPECT_TRUE(m.fields().empty());
}

TEST(MessageTest, RawStorage) {
    Message m;
    m.set_raw("raw_bytes_here");
    EXPECT_EQ(m.raw(), "raw_bytes_here");
}

// ===========================================================================
// Production-hardening Phase 1B — C7 (repeating groups / duplicate headers)
//                                 C8 (wire-character validation)
// ===========================================================================

TEST(MessageTest, AddAppendsDuplicatesAndGetAll) {
    Message m;
    ASSERT_TRUE(m.add(tags::Symbol, "AAPL").has_value());
    ASSERT_TRUE(m.add(tags::Symbol, "MSFT").has_value());
    EXPECT_EQ(m.get(tags::Symbol).value_or(""), "AAPL"); // first wins
    auto all = m.get_all(tags::Symbol);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0], "AAPL");
    EXPECT_EQ(all[1], "MSFT");
    // set() still upserts the first occurrence only.
    ASSERT_TRUE(m.set(tags::Symbol, "GOOG").has_value());
    EXPECT_EQ(m.get(tags::Symbol).value_or(""), "GOOG");
    EXPECT_EQ(m.get_all(tags::Symbol).size(), 2u);
}

TEST(MessageTest, SetRejectsSoHAndNul) {
    Message m;
    std::string soh = "AAA";
    soh += SOH;
    soh += "55=HACKED"; // would inject a Tag=Value pair on the wire
    auto r1 = m.set(tags::Symbol, soh);
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error(), make_error_code(ErrorCode::InvalidField));
    EXPECT_FALSE(m.has(tags::Symbol)); // message left unchanged

    std::string nul("AA\0", 3);
    auto r2 = m.add(tags::Symbol, nul);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error(), make_error_code(ErrorCode::InvalidField));
    EXPECT_TRUE(m.fields().empty());

    // A valid value is still accepted.
    ASSERT_TRUE(m.set(tags::Symbol, "AAPL").has_value());
    EXPECT_EQ(m.get(tags::Symbol).value_or(""), "AAPL");
}

TEST(MessageTest, HasInvalidWireChars) {
    EXPECT_TRUE(has_invalid_wire_chars(std::string_view("A\x01"
                                                        "B"))); // SOH
    EXPECT_TRUE(has_invalid_wire_chars(std::string_view("A\0"
                                                        "B",
                                                        3))); // NUL
    EXPECT_TRUE(has_invalid_wire_chars(std::string_view("\x01")));
    EXPECT_FALSE(has_invalid_wire_chars("plain value"));
    EXPECT_FALSE(has_invalid_wire_chars(""));
}

TEST(MessageTest, DuplicateHeaderFlagLifecycle) {
    Message m;
    EXPECT_FALSE(m.has_duplicate_header());
    m.mark_duplicate_header();
    EXPECT_TRUE(m.has_duplicate_header());
    m.clear();
    EXPECT_FALSE(m.has_duplicate_header()); // clear() resets the flag
}
