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
    bool validate_body_len = true;
    std::size_t max_msg_len = 1u << 20; // 1 MB
};

// ---------------------------------------------------------------------------
// StreamParser
//
// Usage:
//   parser.feed(buf, len);        // push bytes from network
//   while (parser.next(msg)) {}   // consume completed messages
// ---------------------------------------------------------------------------
class StreamParser {
public:
    using Options = StreamParserOptions;

    explicit StreamParser(Options opts = Options{});

    // Feed raw bytes into the parser.
    void feed(std::span<const std::byte> data);
    void feed(const char *data, std::size_t len);

    // Returns true and fills `out` if a complete, valid message is available.
    bool next(Message &out);

    // Reset parser state (e.g., after a session reset)
    void reset();

    [[nodiscard]] std::size_t bytes_consumed() const noexcept { return consumed_; }
    [[nodiscard]] std::uint64_t msg_count() const noexcept { return msg_count_; }
    [[nodiscard]] std::error_code last_error() const noexcept { return last_error_; }

private:
    std::vector<char> buf_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
    std::size_t consumed_ = 0;
    std::uint64_t msg_count_ = 0;
    std::error_code last_error_;
    Options opts_;

    std::vector<Message> pending_;
    std::size_t pending_pos_ = 0;

    void compact();
    bool try_parse_one();

    static std::size_t parse_field(const char *data, std::size_t len, TagNum &tag,
                                   std::string_view &value);
    static std::uint8_t compute_checksum(const char *data, std::size_t len) noexcept;
    static std::uint32_t checksum_to_int(std::string_view sv) noexcept;
};

} // namespace fix
