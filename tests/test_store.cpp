// =============================================================================
// FIX Protocol Engine - Store unit tests
// =============================================================================
#include "fix/store/file_store.hpp"
#include "fix/store/memory_store.hpp"

#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

using namespace fix;

// ---------------------------------------------------------------------------
// MemoryStore tests
// ---------------------------------------------------------------------------
TEST(MemoryStoreTest, InitialSeqNums) {
    MemoryStore s;
    EXPECT_EQ(s.next_sender_seq_num(), 1u);
    EXPECT_EQ(s.next_target_seq_num(), 1u);
}

TEST(MemoryStoreTest, SetSeqNums) {
    MemoryStore s;
    s.set_next_sender_seq_num(42);
    s.set_next_target_seq_num(100);
    EXPECT_EQ(s.next_sender_seq_num(), 42u);
    EXPECT_EQ(s.next_target_seq_num(), 100u);
}

TEST(MemoryStoreTest, IncrSeqNums) {
    MemoryStore s;
    s.incr_sender_seq_num();
    s.incr_sender_seq_num();
    EXPECT_EQ(s.next_sender_seq_num(), 3u);
    s.incr_target_seq_num();
    EXPECT_EQ(s.next_target_seq_num(), 2u);
}

TEST(MemoryStoreTest, StoreAndRetrieve) {
    MemoryStore s;
    s.store_outbound(1, "MSG1");
    s.store_outbound(2, "MSG2");
    s.store_outbound(3, "MSG3");

    std::vector<std::pair<SeqNum, std::string>> collected;
    s.get_messages(1, 3,
                   [&](SeqNum seq, const std::string &raw) { collected.emplace_back(seq, raw); });

    ASSERT_EQ(collected.size(), 3u);
    EXPECT_EQ(collected[0].first, 1u);
    EXPECT_EQ(collected[0].second, "MSG1");
    EXPECT_EQ(collected[2].second, "MSG3");
}

TEST(MemoryStoreTest, GetMessagesRange) {
    MemoryStore s;
    for (int i = 1; i <= 5; ++i) {
        s.store_outbound(i, "M" + std::to_string(i));
    }

    std::vector<SeqNum> seqs;
    s.get_messages(2, 4, [&](SeqNum seq, const std::string &) { seqs.push_back(seq); });

    ASSERT_EQ(seqs.size(), 3u);
    EXPECT_EQ(seqs[0], 2u);
    EXPECT_EQ(seqs[2], 4u);
}

TEST(MemoryStoreTest, Reset) {
    MemoryStore s;
    s.set_next_sender_seq_num(50);
    s.store_outbound(1, "MSG1");
    s.reset();

    EXPECT_EQ(s.next_sender_seq_num(), 1u);

    std::vector<std::string> msgs;
    s.get_messages(1, 10, [&](SeqNum, const std::string &r) { msgs.push_back(r); });
    EXPECT_TRUE(msgs.empty());
}

// ---------------------------------------------------------------------------
// FileStore tests
// ---------------------------------------------------------------------------
class FileStoreTest : public ::testing::Test {
protected:
    std::filesystem::path tmpdir;
    SessionID sid;

    void SetUp() override {
        auto *test_info = testing::UnitTest::GetInstance()->current_test_info();
        tmpdir = std::filesystem::temp_directory_path() /
                 (std::string("fix_filestore_") + test_info->name());
        std::filesystem::create_directories(tmpdir);
        sid.senderCompID = "SENDER";
        sid.targetCompID = "TARGET";
        sid.version = FixVersion::FIX_4_4;
    }
    void TearDown() override { std::filesystem::remove_all(tmpdir); }
};

TEST_F(FileStoreTest, InitialSeqNums) {
    FileStore s(tmpdir, sid);
    EXPECT_EQ(s.next_sender_seq_num(), 1u);
    EXPECT_EQ(s.next_target_seq_num(), 1u);
}

TEST_F(FileStoreTest, PersistsSeqNums) {
    {
        FileStore s(tmpdir, sid);
        s.set_next_sender_seq_num(7);
        s.set_next_target_seq_num(13);
    }
    // Reopen
    FileStore s2(tmpdir, sid);
    EXPECT_EQ(s2.next_sender_seq_num(), 7u);
    EXPECT_EQ(s2.next_target_seq_num(), 13u);
}

TEST_F(FileStoreTest, StoreAndRetrieve) {
    FileStore s(tmpdir, sid);
    s.store_outbound(1, "RAWMSG1");
    s.store_outbound(2, "RAWMSG2");

    std::vector<std::string> msgs;
    s.get_messages(1, 2, [&](SeqNum, const std::string &r) { msgs.push_back(r); });

    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0], "RAWMSG1");
    EXPECT_EQ(msgs[1], "RAWMSG2");
}

TEST_F(FileStoreTest, Reset) {
    FileStore s(tmpdir, sid);
    s.set_next_sender_seq_num(99);
    s.store_outbound(1, "MSG");
    s.reset();

    EXPECT_EQ(s.next_sender_seq_num(), 1u);

    std::vector<std::string> msgs;
    s.get_messages(1, 10, [&](SeqNum, const std::string &r) { msgs.push_back(r); });
    EXPECT_TRUE(msgs.empty());
}

TEST_F(FileStoreTest, Refresh) {
    {
        FileStore s(tmpdir, sid);
        s.set_next_sender_seq_num(55);
    }
    FileStore s2(tmpdir, sid);
    s2.refresh(); // should reload from disk
    EXPECT_EQ(s2.next_sender_seq_num(), 55u);
}
