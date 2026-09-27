#pragma once
// =============================================================================
// FIX Protocol Engine - File-Based Message Store (append-only log)
//
// Phase 3 (3.4) crash safety contract:
//  * EVERY sequence-number mutation is persisted via
//    write-tmp -> fsync -> atomic rename -> parent-dir fsync, so the seq
//    file is never observed half-written: kill -9 mid-persist leaves either
//    the old or the new value, never a torn one.
//  * load_seqs() strictly validates the file. Corrupt / truncated /
//    unparsable content REFUSES TO START: open() returns ErrorCode::
//    StoreError and the legacy constructor throws std::system_error.
//    There is NO silent reset to 1 — a silent reset is a sequence-hijack
//    vector (a peer could force a restart, then replay from seq 1).
//  * Message appends check the stream state and return Result errors;
//    per-append fsync is opt-in (Options::fsync_messages, default OFF —
//    sequence state is always fsynced, so a crash can at worst lose the
//    last un-fsynced message frames, never the sequence numbers).
//  * Filenames embed sanitized CompIDs + qualifier so two sessions can
//    never collide on the same files (C10).
//  * close() is explicit, idempotent, and runs from the destructor.
// =============================================================================
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "message_store.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// FileStore
// ---------------------------------------------------------------------------
class FileStore : public IMessageStore {
public:
    struct Options {
        // fsync(2) after every message append (outbound + inbound log).
        // Crash-safe against power loss for the MESSAGE frames themselves,
        // at the cost of one fsync per stored frame. Default OFF for
        // throughput — sequence state is always fsynced regardless (see
        // persist_seqs), so sequences can never be silently lost; only the
        // most recent un-fsynced frames could vanish on a power cut (a gap
        // the peer recovers with a normal ResendRequest). Tests enable it.
        bool fsync_messages = false;
    };

    // Result-returning factory — the PREFERRED entry point: a corrupt,
    // truncated or unparsable sequence file yields ErrorCode::StoreError so
    // the caller can refuse to start the session (engine wiring must
    // propagate this; see Engine::add_session). Exceptions from construction
    // are folded into the error code. (Two overloads rather than a default
    // argument: GCC rejects `Options = {}` defaults for NSDMI-bearing nested
    // types inside the enclosing class definition.)
    static Result<std::unique_ptr<FileStore>> open(std::filesystem::path dir,
                                                   const SessionID &sid) {
        return open(std::move(dir), sid, Options{});
    }
    static Result<std::unique_ptr<FileStore>> open(std::filesystem::path dir, const SessionID &sid,
                                                   Options opts);

    // Legacy constructors. Identical semantics except that a corrupt
    // sequence file FAILS LOUDLY by throwing std::system_error instead of
    // returning a Result — never a silent reset. Prefer open() wherever the
    // caller can propagate a Result.
    explicit FileStore(std::filesystem::path dir, const SessionID &sid);
    explicit FileStore(std::filesystem::path dir, const SessionID &sid, Options opts);
    ~FileStore() override;

    SeqNum next_sender_seq_num() const noexcept override {
        return sender_seq_.load(std::memory_order_acquire);
    }
    SeqNum next_target_seq_num() const noexcept override {
        return target_seq_.load(std::memory_order_acquire);
    }
    Result<void> set_next_sender_seq_num(SeqNum n) override;
    Result<void> set_next_target_seq_num(SeqNum n) override;
    Result<void> incr_sender_seq_num() override;
    Result<void> incr_target_seq_num() override;

    // 3.2 compound primitives: the claim persists atomically under the
    // store mutex and rolls back on failure, so an error consumes nothing.
    Result<SeqNum> next_sender_seq_num_incr() override;
    Result<void> advance_next_target_seq_num(SeqNum n) override;

    Result<void> store_outbound(SeqNum seq, const std::string &raw) override;
    Result<void> store_inbound(SeqNum seq, const std::string &raw) override;
    Result<void> get_messages(SeqNum begin, SeqNum end, MessageCallback cb) const override;
    Result<void> reset() override;
    Result<void> refresh() override;
    // Flush + fsync + close both message logs and the final sequence
    // snapshot; idempotent (second call is a no-op returning success).
    Result<void> close() override;

    // -- Diagnostics / tests -------------------------------------------------
    [[nodiscard]] const std::filesystem::path &seq_file_path() const noexcept { return seqfile_; }
    [[nodiscard]] const std::filesystem::path &out_file_path() const noexcept { return outfile_; }
    [[nodiscard]] bool is_closed() const noexcept {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    // Sanitize one path component: keep [A-Za-z0-9_-], map everything else
    // (/, :, ., spaces, ...) to '_', hard-cap at `cap` characters. Public so
    // tests can assert the exact contract. Never returns an empty string.
    static std::string sanitize_component(std::string_view raw, std::size_t cap = 32);

private:
    std::filesystem::path dir_;
    std::filesystem::path seqfile_;
    std::filesystem::path outfile_;
    std::filesystem::path infile_;
    Options opts_;

    std::atomic<SeqNum> sender_seq_{1};
    std::atomic<SeqNum> target_seq_{1};
    mutable std::mutex mutex_;
    std::ofstream out_stream_;
    std::ofstream in_stream_;
    // In-memory index: seq -> byte offset in outfile
    std::map<SeqNum, std::streampos> out_index_;
    bool closed_ = false;

    // Parses + validates seqfile_ and only THEN commits the values: a
    // corrupt file leaves the in-memory counters untouched and returns an
    // error (never a silent rebase to 1). Missing file = fresh session.
    Result<void> load_seqs();
    // Caller holds mutex_. Crash-safe: tmp file + fsync + atomic rename +
    // parent-dir fsync.
    Result<void> persist_seqs();
    void rebuild_index();
    // Build "<sender>~<target>[~<qualifier>]" with sanitized components.
    // '~' is the separator precisely because sanitisation removes it from
    // components, so the encoding is unambiguous and collision-free.
    static std::string make_prefix(const SessionID &sid);
};

} // namespace fix
