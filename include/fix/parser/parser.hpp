#pragma once
// =============================================================================
// FIX Protocol Engine - Streaming SOH-delimited Parser
// =============================================================================
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "../core/constants.hpp"
#include "../core/field.hpp"
#include "../core/message.hpp"
#include "../core/types.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// ParseEvent: callback types delivered from the parser
// ---------------------------------------------------------------------------
enum class ParseEvent : std::uint8_t {
    FieldParsed,
    MessageComplete,
    ParseError,
};

// ---------------------------------------------------------------------------
// StreamParser options – must be defined *outside* the StreamParser class to
// avoid the GCC "default member initializer required before end of class"
// error when used as a default parameter.
// ---------------------------------------------------------------------------
struct StreamParserOptions {
    bool validate_checksum = true;
    // Body-length validation is enforced structurally by framing: the
    // CheckSum trailer ("10=NNN\x01") must sit exactly at
    // body_start + BodyLength, and the body loop never scans past that
    // offset (C6 fix). This flag additionally runs an explicit arithmetic
    // assertion of claimed-vs-actual length (redundant under structural
    // framing; retained for API compatibility).
    bool validate_body_len = true;
    // Maximum accepted message size AND unparsed-backlog cap (a small slack
    // for one in-flight frame is added on top). Frames whose BodyLength
    // would exceed this are skipped wholesale; a backlog that still exceeds
    // the cap marks the parser unhealthy. 0 = unlimited — ONLY safe on
    // trusted input (disables the DoS backstop entirely).
    std::size_t max_msg_len = 1u << 20; // 1 MB
};

// ---------------------------------------------------------------------------
// StreamParser
//
// Usage:
//   parser.feed(buf, len);        // push bytes from network
//   while (parser.next(msg)) {}   // consume completed messages
//
// Robustness (C6): the parser never stalls on garbage and never grows its
// buffer without bound:
//   * input not starting with "8=" is resync-scanned forward (discarding
//     garbage, counted by resync_count(), last_error() set);
//   * the unparsed backlog is hard-capped at max_msg_len + slack; a backlog
//     that cannot be reclaimed is fatal — healthy() turns false, further
//     feed() input is dropped, and the session layer should disconnect then
//     call reset();
//   * parse errors inside feed() ADVANCE the read position and the parse
//     loop CONTINUES (edge-triggered epoll may never re-fire), so valid
//     messages already buffered behind a bad frame are still delivered.
//     Only a genuine "need more bytes" stops the loop.
//   * last_error() is cleared when a message parses successfully (except a
//     successfully-delivered message carrying duplicate header tags, which
//     reports ErrorCode::DuplicateField — see Message::has_duplicate_header).
//
// THREAD SAFETY: a StreamParser instance is single-thread-affine and NOT
// internally synchronized. Call feed()/next()/reset() from a single thread at
// a time (in this engine: the session's IO thread). Do not share an instance
// across threads without external locking.
// ---------------------------------------------------------------------------
class StreamParser {
public:
    using Options = StreamParserOptions;

    explicit StreamParser(Options opts = Options{});

    // Feed raw bytes into the parser. While healthy(), the retained backlog
    // never exceeds the cap (max_msg_len + slack); input that cannot be
    // accepted marks the parser unhealthy and is dropped.
    void feed(std::span<const std::byte> data);
    void feed(const char *data, std::size_t len);

    // Returns true and fills `out` with the oldest complete, valid message.
    // When false is returned `out` is CLEARED (default-constructed) — it is
    // never left holding data from a previous call.
    bool next(Message &out);

    // Reset parser state (e.g., after a session reset); also re-arms a
    // parser that went unhealthy.
    void reset();

    // Total bytes consumed since construction/reset: successfully parsed
    // message bytes PLUS discarded garbage (resyncs/skips). Monotonic.
    [[nodiscard]] std::size_t bytes_consumed() const noexcept { return consumed_; }
    [[nodiscard]] std::uint64_t msg_count() const noexcept { return msg_count_; }
    [[nodiscard]] std::error_code last_error() const noexcept { return last_error_; }

    // --- Robustness / health (C6) -----------------------------------------
    // false after a fatal condition (unparsed backlog exceeded the buffer cap
    // even after a resync scan). The session layer should disconnect and
    // call reset(); further feed() input is dropped until then.
    [[nodiscard]] bool healthy() const noexcept { return healthy_; }
    // Number of resync events (garbage discarded to re-align on an "8="
    // frame start). Non-zero indicates a misbehaving/garbage peer.
    [[nodiscard]] std::uint64_t resync_count() const noexcept { return resync_count_; }
    // Bytes buffered but not yet parsed (0..cap while healthy()).
    [[nodiscard]] std::size_t buffered() const noexcept { return write_pos_ - read_pos_; }

private:
    // Tri-state result of parsing one "tag=value\x01" field.
    enum class FieldStatus : std::uint8_t {
        Ok,         // field parsed; `consumed` bytes advanced (incl. SOH)
        Incomplete, // data ends mid-field: wait for more bytes
        Invalid,    // malformed tag/value — the frame is unrecoverable
    };

    // Result of one try_parse_one() step:
    //   Message  – a complete message was appended to pending_
    //   Error    – invalid/garbage data skipped; read_pos_ advanced
    //   NeedMore – genuinely needs more bytes; stop the feed loop
    enum class ParseStatus : std::uint8_t {
        Message,
        Error,
        NeedMore
    };

    static constexpr std::size_t kMinMsgLen = 20;      // shortest viable FIX frame
    static constexpr std::size_t kTrailerBytes = 7;    // "10=NNN\x01"
    static constexpr std::size_t kKeepTail = 7;        // kept when resync finds no "8="
    static constexpr std::size_t kMaxHeaderScan = 64;  // bound on tag-8/9 field scans
    static constexpr std::size_t kBacklogSlack = 4096; // cap slack (one in-flight frame)

    std::vector<char> buf_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
    std::size_t consumed_ = 0;
    std::uint64_t msg_count_ = 0;
    std::uint64_t resync_count_ = 0;
    std::error_code last_error_;
    bool healthy_ = true;
    std::size_t buf_cap_ = 0; // max retained backlog (0 = unlimited)
    Options opts_;

    std::vector<Message> pending_;
    std::size_t pending_pos_ = 0;

    void compact();
    void append_bytes(const char *data, std::size_t len);
    void drain();
    ParseStatus try_parse_one();
    // Discard bytes up to the next "8=" frame start. Returns bytes dropped;
    // sets last_error_(err) and bumps resync_count_ when at least one byte
    // is discarded. A no-op when already aligned (or nothing can be dropped).
    std::size_t resync_scan(std::error_code err);
    // Skip `n` bytes of an invalid frame (always ≥1 → guaranteed progress),
    // then resync to the next frame start. Returns ParseStatus::Error.
    ParseStatus skip_frame(std::size_t n, std::error_code err);
    // Called when the backlog hits buf_cap_: resync + drain to reclaim; if
    // still full, latch the fatal unhealthy_ state.
    void enforce_cap();

    static FieldStatus parse_field(const char *data, std::size_t len, TagNum &tag,
                                   std::string_view &value, std::size_t &consumed);
    static std::uint8_t compute_checksum(const char *data, std::size_t len) noexcept;
    // Strict CheckSum value: exactly 3 ASCII digits, value ≤ 255.
    static bool parse_checksum(std::string_view sv, std::uint32_t &out) noexcept;
};

} // namespace fix
