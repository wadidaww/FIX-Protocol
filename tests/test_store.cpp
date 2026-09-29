// =============================================================================
// FIX Protocol Engine - Store unit tests
// =============================================================================
#include "fix/store/file_store.hpp"
#include "fix/store/memory_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <csignal>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace fix;

namespace {
// PID suffix so parallel suites (ctest -j, several checkouts on one box)
// never share a store directory.
std::string test_pid() {
#ifdef _WIN32
    return std::to_string(static_cast<unsigned long>(_getpid()));
#else
    return std::to_string(::getpid());
#endif
}

// Dump every file in the store directory — attached to reopen failures so
// an intermittent persistence bug shows the on-disk bytes, not just "1 != 7".
void dump_dir(const std::filesystem::path &dir) {
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
        std::ifstream f(e.path(), std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        SCOPED_TRACE("file=" + e.path().filename().string() + " content=[" + content + "]");
    }
}
} // namespace

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
        // PID-unique AND wiped on start: suites are frequently run in
        // parallel (ctest -j, several checkouts on one box) and a shared
        // /tmp name lets one process delete or poison another's store
        // mid-test — stale seq files from a previous run must never be
        // loaded either.
        tmpdir = std::filesystem::temp_directory_path() /
                 (std::string("fix_filestore_") + test_info->name() + "_" + test_pid());
        std::error_code ec;
        std::filesystem::remove_all(tmpdir, ec);
        std::filesystem::create_directories(tmpdir);
        sid.senderCompID = "SENDER";
        sid.targetCompID = "TARGET";
        sid.version = FixVersion::FIX_4_4;
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmpdir, ec);
    }
};

TEST_F(FileStoreTest, InitialSeqNums) {
    FileStore s(tmpdir, sid);
    EXPECT_EQ(s.next_sender_seq_num(), 1u);
    EXPECT_EQ(s.next_target_seq_num(), 1u);
}

TEST_F(FileStoreTest, PersistsSeqNums) {
    {
        FileStore s(tmpdir, sid);
        ASSERT_TRUE(s.set_next_sender_seq_num(7).has_value());
        ASSERT_TRUE(s.set_next_target_seq_num(13).has_value());
    }
    // Reopen
    FileStore s2(tmpdir, sid);
    if (s2.next_sender_seq_num() != 7u || s2.next_target_seq_num() != 13u)
        dump_dir(tmpdir);
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
        ASSERT_TRUE(s.set_next_sender_seq_num(55).has_value());
    }
    FileStore s2(tmpdir, sid);
    s2.refresh(); // should reload from disk
    if (s2.next_sender_seq_num() != 55u)
        dump_dir(tmpdir);
    EXPECT_EQ(s2.next_sender_seq_num(), 55u);
}

// ---------------------------------------------------------------------------
// 3.2: compound sequence primitives (claim + advance)
// ---------------------------------------------------------------------------
TEST(MemoryStoreTest, CompoundClaimIsAtomicUnderConcurrency) {
    MemoryStore s;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    constexpr SeqNum kTotal = kThreads * kPerThread;

    std::vector<std::thread> ths;
    std::vector<std::vector<SeqNum>> got(static_cast<size_t>(kThreads));
    for (int t = 0; t < kThreads; ++t) {
        ths.emplace_back([&s, &got, t] {
            auto &mine = got[static_cast<size_t>(t)];
            mine.reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                auto r = s.next_sender_seq_num_incr();
                mine.push_back(r.has_value() ? *r : 0);
            }
        });
    }
    for (auto &th : ths)
        th.join();

    std::set<SeqNum> unique;
    for (const auto &mine : got)
        unique.insert(mine.begin(), mine.end());

    EXPECT_EQ(unique.size(), static_cast<size_t>(kTotal)); // no duplicates, no losses
    EXPECT_EQ(*unique.begin(), 1u);
    EXPECT_EQ(*unique.rbegin(), kTotal);
    EXPECT_EQ(s.next_sender_seq_num(), kTotal + 1);
}

TEST(MemoryStoreTest, AdvanceTargetIsMonotonicUnderConcurrency) {
    MemoryStore s;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;

    std::vector<std::thread> ths;
    for (int t = 0; t < kThreads; ++t) {
        ths.emplace_back([&s, t] {
            // Advance in scrambled order: final value must be the max.
            for (int i = 0; i < kPerThread; ++i) {
                const SeqNum v =
                    static_cast<SeqNum>((t + 1) * kPerThread - ((i * 7919) % kPerThread));
                (void)s.advance_next_target_seq_num(v);
            }
        });
    }
    for (auto &th : ths)
        th.join();

    EXPECT_EQ(s.next_target_seq_num(), static_cast<SeqNum>(kThreads * kPerThread));
    // Monotonicity: never rewinds even for low values
    EXPECT_TRUE(s.advance_next_target_seq_num(1).has_value());
    EXPECT_EQ(s.next_target_seq_num(), static_cast<SeqNum>(kThreads * kPerThread));
}

TEST_F(FileStoreTest, CompoundClaimIsDurableAcrossReopen) {
    {
        FileStore s(tmpdir, sid);
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; ++t) {
            ths.emplace_back([&s] {
                for (int i = 0; i < 5; ++i) {
                    auto r = s.next_sender_seq_num_incr();
                    if (r)
                        (void)s.store_outbound(*r, "CLAIM-" + std::to_string(*r));
                }
            });
        }
        for (auto &th : ths)
            th.join();
        EXPECT_EQ(s.next_sender_seq_num(), 21u);
    }
    // Reopen: all 20 claimed numbers must have been durably persisted exactly once.
    FileStore s2(tmpdir, sid);
    EXPECT_EQ(s2.next_sender_seq_num(), 21u);
    std::vector<std::string> msgs;
    s2.get_messages(1, 20, [&](SeqNum, const std::string &r) { msgs.push_back(r); });
    EXPECT_EQ(msgs.size(), 20u);
}

// ---------------------------------------------------------------------------
// 3.4: crash safety — kill -9 recovery, corrupt sequence files
// ---------------------------------------------------------------------------
#ifndef _WIN32
TEST_F(FileStoreTest, Kill9DoesNotResetSequences) {
    int pfd[2];
    ASSERT_EQ(::pipe(pfd), 0);

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // ---- child: persist state (all fsynced), signal readiness, park.
        ::close(pfd[0]);
        int code = 0;
        try {
            FileStore::Options opts;
            opts.fsync_messages = true;
            FileStore s(tmpdir, sid, opts);
            for (SeqNum i = 1; i <= 3; ++i) {
                auto c = s.next_sender_seq_num_incr();
                if (!c) {
                    code = 1;
                    break;
                }
                if (auto r = s.store_outbound(*c, "KILLMSG" + std::to_string(*c)); !r) {
                    code = 2;
                    break;
                }
            }
            if (code == 0 && !s.advance_next_target_seq_num(7))
                code = 3;
            if (code == 0 && !s.store_inbound(1, "KILLIN1"))
                code = 4;
            // NO close(): we simulate a process that dies without unwinding.
        } catch (...) {
            code = 5;
        }
        if (code != 0)
            ::_exit(code);
        const char ok = '!';
        (void)!::write(pfd[1], &ok, 1);
        for (;;)
            ::pause(); // wait for SIGKILL from the parent
    }

    // ---- parent: wait for readiness, then kill hard.
    ::close(pfd[1]);
    char ready = 0;
    const ssize_t n = ::read(pfd[0], &ready, 1);
    ::close(pfd[0]);
    if (n != 1) {
        int status = 0;
        (void)::waitpid(pid, &status, 0);
        FAIL() << "child exited early with code " << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    ASSERT_EQ(::kill(pid, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);

    // Re-open exactly as a restarted process would: nothing may reset to 1.
    auto s = FileStore::open(tmpdir, sid);
    ASSERT_TRUE(s.has_value()) << (s.has_value() ? "" : s.error().message());
    EXPECT_EQ((*s)->next_sender_seq_num(), 4u); // three claims survived
    EXPECT_EQ((*s)->next_target_seq_num(), 7u);
    std::vector<std::string> msgs;
    (*s)->get_messages(1, 3, [&](SeqNum, const std::string &r) { msgs.push_back(r); });
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0], "KILLMSG1");
    EXPECT_EQ(msgs[2], "KILLMSG3");
}

// SHOULD#3: persist_seqs() claims "write-tmp + fsync + atomic rename", i.e.
// a reader can only ever see a COMPLETE old or new sequence file. That claim
// was untested under contention: this test samples the file ~1000 times
// (100 µs apart) while a child process claims sequence numbers as fast as it
// can, then kills it with SIGKILL and re-opens. Every snapshot must parse
// cleanly (or the file must not exist yet) — never torn.
TEST_F(FileStoreTest, SeqFileSnapshotsAreNeverTornWhilePersisting) {
    int pfd[2];
    ASSERT_EQ(::pipe(pfd), 0);

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // ---- child: signal readiness, then claim in a tight loop, forever.
        ::close(pfd[0]);
        try {
            FileStore s(tmpdir, sid);
            const char ok = '!';
            (void)!::write(pfd[1], &ok, 1);
            for (SeqNum i = 1;; ++i) {
                if (!s.next_sender_seq_num_incr())
                    ::_exit(1); // persist failed — the parent will see it
                if ((i % 64) == 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        } catch (...) {
            ::_exit(5);
        }
    }

    // ---- parent: wait for readiness, sample, then kill hard.
    ::close(pfd[1]);
    char ready = 0;
    const ssize_t n = ::read(pfd[0], &ready, 1);
    ::close(pfd[0]);
    if (n != 1) {
        int status = 0;
        (void)::waitpid(pid, &status, 0);
        FAIL() << "child exited early with code " << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }

    const auto seqfile = tmpdir / "SENDER~TARGET.seqnums";
    // Strict mirror of load_seqs(): exactly two whitespace-separated values
    // >= 1 and nothing after them.
    auto parses_cleanly = [](const std::string &content) {
        std::istringstream is(content);
        long long a = -1, b = -1;
        if (!(is >> a >> b))
            return false;
        std::string extra;
        if (is >> extra)
            return false;
        return a >= 1 && b >= 1;
    };

    std::size_t observed = 0;
    std::size_t torn = 0;
    for (int i = 0; i < 1000; ++i) {
        std::error_code ec;
        if (std::filesystem::exists(seqfile, ec) && !ec) {
            std::ifstream f(seqfile, std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
            if (f.good() || f.eof()) {
                ++observed;
                if (!parses_cleanly(content))
                    ++torn;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    EXPECT_EQ(torn, 0u) << "a sequence file snapshot must never be half-written";
    EXPECT_GE(observed, 10u) << "the sampler never actually watched a persist";

    ASSERT_EQ(::kill(pid, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);

    // A restarted process must see a COMPLETE file — never a torn one that
    // refuses to start, never a silent rebase to 1.
    auto s = FileStore::open(tmpdir, sid);
    ASSERT_TRUE(s.has_value()) << (s.has_value() ? "" : s.error().message());
    EXPECT_GE((*s)->next_sender_seq_num(), 1u);
    const auto final_content = [&] {
        std::ifstream f(seqfile, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }();
    EXPECT_TRUE(parses_cleanly(final_content)) << "final content=[" << final_content << "]";
}
#endif // !_WIN32

TEST_F(FileStoreTest, CorruptSeqFileRefusesToStart) {
    {
        std::ofstream f(tmpdir / "SENDER~TARGET.seqnums");
        f << "not-a-number\n5\n";
    }
    auto r = FileStore::open(tmpdir, sid);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::StoreError));
    // The legacy constructor fails loudly instead of silently resetting.
    EXPECT_THROW((void)FileStore(tmpdir, sid), std::system_error);
    // The corrupt file is left untouched (no destructive "reset to 1").
    std::ifstream in(tmpdir / "SENDER~TARGET.seqnums");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "not-a-number\n5\n");
}

TEST_F(FileStoreTest, TruncatedSeqFileRefusesToStart) {
    {
        std::ofstream f(tmpdir / "SENDER~TARGET.seqnums");
        f << "42\n";
    }
    auto r = FileStore::open(tmpdir, sid);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::StoreError));
    EXPECT_THROW((void)FileStore(tmpdir, sid), std::system_error);
}

TEST_F(FileStoreTest, EmptySeqFileRefusesToStart) {
    { std::ofstream f(tmpdir / "SENDER~TARGET.seqnums"); }
    auto r = FileStore::open(tmpdir, sid);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::StoreError));
}

TEST_F(FileStoreTest, ZeroSeqNumsRefuseToStart) {
    {
        std::ofstream f(tmpdir / "SENDER~TARGET.seqnums");
        f << "0\n0\n";
    }
    auto r = FileStore::open(tmpdir, sid);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::StoreError));
    EXPECT_THROW((void)FileStore(tmpdir, sid), std::system_error);
}

TEST_F(FileStoreTest, TrailingGarbageSeqFileRefusesToStart) {
    {
        std::ofstream f(tmpdir / "SENDER~TARGET.seqnums");
        f << "1 2 xyz\n";
    }
    auto r = FileStore::open(tmpdir, sid);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), make_error_code(ErrorCode::StoreError));
}

TEST_F(FileStoreTest, MissingSeqFileStartsFreshAtOne) {
    auto r = FileStore::open(tmpdir, sid);
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
    EXPECT_EQ((*r)->next_sender_seq_num(), 1u);
    EXPECT_EQ((*r)->next_target_seq_num(), 1u);
    // Fresh stores are lazy: nothing hits the disk until the first mutation
    // (a missing file already means "start at 1" for the next opener).
    EXPECT_FALSE(std::filesystem::exists(tmpdir / "SENDER~TARGET.seqnums"));

    // The first claim materialises the file durably.
    ASSERT_TRUE((*r)->next_sender_seq_num_incr().has_value());
    ASSERT_TRUE(std::filesystem::exists(tmpdir / "SENDER~TARGET.seqnums"));

    auto r2 = FileStore::open(tmpdir, sid);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ((*r2)->next_sender_seq_num(), 2u);
    EXPECT_EQ((*r2)->next_target_seq_num(), 1u);
}

// ---------------------------------------------------------------------------
// 3.4: file naming / sanitisation / close semantics
// ---------------------------------------------------------------------------
TEST_F(FileStoreTest, FilenamesAreSanitizedAndQualifierScoped) {
    SessionID weird = sid;
    weird.senderCompID = "BAD/SENDER:1";
    weird.targetCompID = "TAR..GET\\2";
    weird.qualifier = "q/x";

    const auto dir = tmpdir / "weird";
    {
        FileStore s(dir, weird);
        // SHOULD#2: %-hex escapes (uppercase) — path separators, dots and
        // colons can never escape the store directory, and unlike the old
        // '_' substitution the encoding is injective.
        EXPECT_EQ(s.seq_file_path().parent_path(), dir);
        EXPECT_EQ(s.seq_file_path().filename().string(),
                  "BAD%2FSENDER%3A1~TAR%2E%2EGET%5C2~q%2Fx.seqnums");
        // The seq file is created on first persist (lazy), never eagerly.
        ASSERT_TRUE(s.set_next_sender_seq_num(1).has_value());
        EXPECT_TRUE(std::filesystem::is_regular_file(s.seq_file_path()));
        EXPECT_TRUE(std::filesystem::exists(dir / s.seq_file_path().filename()));
    }
    // Nothing was written outside the directory itself.
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        EXPECT_EQ(entry.path().parent_path(), dir);
        EXPECT_TRUE(entry.is_regular_file());
    }

    // Same CompIDs, different qualifier → isolated files.
    {
        const auto dir2 = tmpdir / "qual";
        SessionID a = sid;
        a.qualifier = "A";
        SessionID b = sid;
        b.qualifier = "B";
        FileStore sa(dir2, a);
        FileStore sb(dir2, b);
        ASSERT_TRUE(sa.set_next_sender_seq_num(99).has_value());
        EXPECT_EQ(sa.seq_file_path(), dir2 / "SENDER~TARGET~A.seqnums");
        EXPECT_EQ(sb.seq_file_path(), dir2 / "SENDER~TARGET~B.seqnums");
        EXPECT_NE(sa.seq_file_path(), sb.seq_file_path());
        EXPECT_EQ(sb.next_sender_seq_num(), 1u); // untouched by A's store
    }

    // Default (no qualifier) keeps the historic name.
    {
        const auto dir3 = tmpdir / "noq";
        FileStore s(dir3, sid);
        EXPECT_EQ(s.seq_file_path().filename().string(), "SENDER~TARGET.seqnums");
    }

    // SHOULD#2 injectivity: "A/B" and "A_B" must NEVER share one store (the
    // old '_' mapping made them the same file — two sessions, one seq state).
    {
        const auto dir4 = tmpdir / "collide";
        SessionID x = sid;
        x.senderCompID = "A/B";
        x.targetCompID = "T";
        SessionID y = sid;
        y.senderCompID = "A_B";
        y.targetCompID = "T";
        FileStore sx(dir4, x);
        FileStore sy(dir4, y);
        EXPECT_EQ(sx.seq_file_path().filename().string(), "A%2FB~T.seqnums");
        EXPECT_EQ(sy.seq_file_path().filename().string(), "A_B~T.seqnums");
        EXPECT_NE(sx.seq_file_path(), sy.seq_file_path());
        ASSERT_TRUE(sx.set_next_sender_seq_num(7).has_value());
        EXPECT_EQ(sy.next_sender_seq_num(), 1u); // A's state never bleeds into B
    }

    // Empty components get the encoder-unreachable "%0" token — never ""
    // (which would collide with a missing component) and never a bare "_".
    {
        const auto dir5 = tmpdir / "empty";
        SessionID e = sid;
        e.senderCompID = "";
        e.targetCompID = "";
        FileStore s(dir5, e);
        EXPECT_EQ(s.seq_file_path().filename().string(), "%0~%0.seqnums");
    }

    // Over-long components: the first 32 RAW bytes are encoded, then
    // "~<8-hex FNV-1a-64 low 32 bits of the full value>".
    {
        const auto dir6 = tmpdir / "long";
        SessionID lid = sid;
        lid.senderCompID = std::string(100, 'L');
        FileStore s(dir6, lid);
        const std::string name = s.seq_file_path().filename().string();
        EXPECT_EQ(name, std::string(32, 'L') + "~13C1B9E5~TARGET.seqnums");
        ASSERT_TRUE(s.set_next_sender_seq_num(1).has_value());
        EXPECT_TRUE(std::filesystem::is_regular_file(s.seq_file_path()));
    }

    // Two 40-char components sharing the ENTIRE 32-byte prefix still land on
    // different files — the fingerprint carries the remaining 8 bytes.
    {
        const auto dir7 = tmpdir / "long2";
        SessionID a = sid;
        a.senderCompID = std::string(32, 'L') + "AAAAAAAA";
        SessionID b = sid;
        b.senderCompID = std::string(32, 'L') + "BBBBBBBB";
        FileStore sa(dir7, a);
        FileStore sb(dir7, b);
        EXPECT_EQ(sa.seq_file_path().filename().string(),
                  std::string(32, 'L') + "~623B39CD~TARGET.seqnums");
        EXPECT_EQ(sb.seq_file_path().filename().string(),
                  std::string(32, 'L') + "~AE154325~TARGET.seqnums");
        EXPECT_NE(sa.seq_file_path(), sb.seq_file_path());
    }
}

// SHOULD#2: the component encoder itself, asserted directly (Public API so
// tests can pin the exact contract).
TEST(FileStoreSanitizeTest, ComponentEncodingIsInjectiveAndSeparatorFree) {
    // Safe bytes pass through untouched.
    EXPECT_EQ(FileStore::sanitize_component("plain-ID_9"), "plain-ID_9");
    // Everything else becomes uppercase %XX.
    EXPECT_EQ(FileStore::sanitize_component("A/B"), "A%2FB");
    EXPECT_EQ(FileStore::sanitize_component("A_B"), "A_B");
    EXPECT_EQ(FileStore::sanitize_component("~"), "%7E");
    EXPECT_EQ(FileStore::sanitize_component("."), "%2E");
    EXPECT_EQ(FileStore::sanitize_component(":"), "%3A");
    // The two inputs that used to COLLIDE under '_' substitution.
    EXPECT_NE(FileStore::sanitize_component("A/B"), FileStore::sanitize_component("A_B"));
    // Empty → encoder-unreachable token.
    EXPECT_EQ(FileStore::sanitize_component(""), "%0");
    // The escape itself is not ambiguous: a literal "%7E" re-escapes the '%'.
    EXPECT_EQ(FileStore::sanitize_component("%7E"), "%257E");
    // At the cap: verbatim, no fingerprint. One raw byte over: fingerprint.
    EXPECT_EQ(FileStore::sanitize_component(std::string(32, 'L')), std::string(32, 'L'));
    EXPECT_EQ(FileStore::sanitize_component(std::string(33, 'L')),
              std::string(32, 'L') + "~69A2116B");
    // Control bytes / 0xFF / spaces / separators never survive.
    const std::string raw = "/\\.:% \r\n\t\x01\xFF";
    const std::string enc = FileStore::sanitize_component(raw);
    EXPECT_EQ(enc.find_first_of("/\\."), std::string::npos);
    EXPECT_EQ(enc.find('~'), std::string::npos); // no fingerprint below the cap
    for (char c : enc) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    c == '%' || c == '_' || c == '-');
    }
}

// SHOULD#4: files written by the pre-sanitisation layout ("SENDER-TARGET.*",
// raw CompIDs, no qualifier) follow the session onto the new name — an
// upgraded process must not silently restart at seq 1 (sequence hijack).
TEST_F(FileStoreTest, LegacyNamedFilesMigrateOnOpen) {
    const auto legacy_seq = tmpdir / "SENDER-TARGET.seqnums";
    const auto legacy_out = tmpdir / "SENDER-TARGET.out.log";
    const auto legacy_in = tmpdir / "SENDER-TARGET.in.log";
    {
        std::ofstream f(legacy_seq);
        f << "50\n70\n";
    }
    {
        std::ofstream f(legacy_out, std::ios::app);
        f << "49|8=FIX.4.4|35=x|9=100|10=000|\n";
    }
    {
        std::ofstream f(legacy_in, std::ios::app);
        f << "1|8=FIX.4.4|35=0|10=111|\n";
    }

    auto r = FileStore::open(tmpdir, sid);
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
    auto &s = *(*r);

    // The sequence history came across...
    EXPECT_EQ(s.next_sender_seq_num(), 50u);
    EXPECT_EQ(s.next_target_seq_num(), 70u);
    EXPECT_TRUE(std::filesystem::exists(tmpdir / "SENDER~TARGET.seqnums"));
    EXPECT_FALSE(std::filesystem::exists(legacy_seq));

    // ...and so did both logs (crash-safe order: logs moved first, seq last).
    EXPECT_TRUE(std::filesystem::exists(tmpdir / "SENDER~TARGET.out.log"));
    EXPECT_FALSE(std::filesystem::exists(legacy_out));
    EXPECT_TRUE(std::filesystem::exists(tmpdir / "SENDER~TARGET.in.log"));
    EXPECT_FALSE(std::filesystem::exists(legacy_in));

    // The migrated outbound history is still indexable for a gap-fill.
    std::vector<std::string> msgs;
    ASSERT_TRUE(s.get_messages(49, 49, [&](SeqNum seq, const std::string &raw) {
                     EXPECT_EQ(seq, 49u);
                     msgs.push_back(raw);
                 }).has_value());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs.front(), "8=FIX.4.4|35=x|9=100|10=000|");
}

// NIT#10 / torn write: a frame cut off mid-append by a crash must never be
// indexed (no partial FIX frame on a gap-fill) and must be TRUNCATED away,
// otherwise the next append would concatenate onto it and corrupt both.
TEST_F(FileStoreTest, TornWriteIsDroppedAndTruncatedOnReopen) {
    const auto outfile = tmpdir / "SENDER~TARGET.out.log";
    {
        std::ofstream f(outfile, std::ios::binary | std::ios::app);
        f << "1|8=FIX.4.4|35=A|10=AAA|\n";
        f << "2|8=FIX.4.4|35=0|10=BBB|\n";
        f << "3|8=FIX.4.4|35=1"; // torn: the '\n' never hit the disk
    }

    auto r = FileStore::open(tmpdir, sid);
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
    auto &s = *(*r);

    // The torn record is invisible to replay...
    std::vector<SeqNum> seqs;
    ASSERT_TRUE(s.get_messages(1, 0, [&](SeqNum seq, const std::string &) {
                     seqs.push_back(seq);
                 }).has_value());
    EXPECT_EQ(seqs, (std::vector<SeqNum>{1, 2}));

    // ...and it is gone from disk, so the next append starts on a record
    // boundary instead of merging with the partial frame.
    ASSERT_TRUE(s.store_outbound(3, "8=FIX.4.4|35=0|10=CCC|").has_value());
    std::string content;
    {
        std::ifstream f(outfile, std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(content, "1|8=FIX.4.4|35=A|10=AAA|\n"
                       "2|8=FIX.4.4|35=0|10=BBB|\n"
                       "3|8=FIX.4.4|35=0|10=CCC|\n");

    // Reopen: all three replay, still no phantom record.
    auto r2 = FileStore::open(tmpdir, sid);
    ASSERT_TRUE(r2.has_value()) << (r2.has_value() ? "" : r2.error().message());
    std::vector<SeqNum> seqs2;
    ASSERT_TRUE(
        (*r2)
            ->get_messages(1, 0, [&](SeqNum seq, const std::string &) { seqs2.push_back(seq); })
            .has_value());
    EXPECT_EQ(seqs2, (std::vector<SeqNum>{1, 2, 3}));
}

TEST_F(FileStoreTest, CloseIsIdempotentAndBlocksWrites) {
    FileStore s(tmpdir, sid);
    ASSERT_TRUE(s.set_next_sender_seq_num(9).has_value());
    ASSERT_TRUE(s.close().has_value());
    EXPECT_TRUE(s.is_closed());
    EXPECT_TRUE(s.close().has_value()); // idempotent

    EXPECT_FALSE(s.store_outbound(1, "x").has_value());
    EXPECT_FALSE(s.store_inbound(1, "x").has_value());
    EXPECT_FALSE(s.next_sender_seq_num_incr().has_value());
    EXPECT_FALSE(s.advance_next_target_seq_num(100).has_value());

    // State persisted before close survives.
    FileStore s2(tmpdir, sid);
    EXPECT_EQ(s2.next_sender_seq_num(), 9u);
}
