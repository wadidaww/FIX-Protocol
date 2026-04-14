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
