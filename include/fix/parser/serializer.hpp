#pragma once
// =============================================================================
// FIX Protocol Engine - Message Serializer
// =============================================================================
#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../core/constants.hpp"
#include "../core/field.hpp"
#include "../core/message.hpp"
#include "../core/types.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// MessageBuilder – incrementally constructs a FIX wire message.
//
// Usage:
//   MessageBuilder b;
//   b.begin("FIX.4.2", "D");          // Result<void> — check if you must not
//   b.add(tags::SenderCompID, "SENDER");
//   b.add(tags::TargetCompID, "TARGET");
//   b.add(tags::MsgSeqNum, 1);
//   ...
//   std::string wire = b.finish();    // appends BodyLength(9) and CheckSum(10)
//
// Wire-character validation (C8): every value routed through add()/append_field
// (including begin_string and msg_type) is checked with
// has_invalid_wire_chars(); a value containing NUL (0x00) or SOH (0x01) is
// REJECTED — SOH is the field delimiter, so embedding it would inject extra
// Tag=Value fields into the wire frame.
//
// Sticky error model:
//   * begin() clears the error and starts a fresh body.
//   * The first rejected value latches last_error(); every later add() fails
//     immediately without mutating the body.
//   * finish() returns "" while an error is latched (a half-built or
//     injected body must never reach the wire).
//   * last_error() surfaces the reason.
//
// Signatures note: begin()/add() return Result<void> (deliberately NOT
// [[nodiscard]] so existing void-style call sites keep compiling), while
// finish()/serialize() keep their historical std::string returns —
// "" is the error convention. try_serialize() is the strict variant that
// surfaces the error instead.
// ---------------------------------------------------------------------------
class MessageBuilder {
public:
    MessageBuilder() { body_.reserve(512); }

    // Start a new message: clears any latched error from a previous build.
    // MsgType is emitted first in the body (after the fixed header tags 8,9).
    // Rejects begin_string/msg_type containing NUL/SOH (C8).
    Result<void> begin(std::string_view begin_string, std::string_view msg_type) {
        last_error_ = {};
        begin_string_.clear();
        body_.clear();
        if (has_invalid_wire_chars(begin_string) || has_invalid_wire_chars(msg_type)) {
            last_error_ = make_error_code(ErrorCode::InvalidField);
            return std::unexpected(last_error_);
        }
        begin_string_ = begin_string;
        return append_field(tags::MsgType, msg_type);
    }

    // Append a field (validates the value — see class comment).
    Result<void> add(TagNum tag, std::string_view value) { return append_field(tag, value); }

    // Explicit const char* overload to prevent char*→bool conversion
    Result<void> add(TagNum tag, const char *value) { return add(tag, std::string_view(value)); }

    Result<void> add(TagNum tag, std::int64_t value) {
        char buf[24];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
        if (ec != std::errc{}) // unreachable for int64_t with a 24-byte buffer
            return fail(ErrorCode::InvalidField);
        return add(tag, std::string_view(buf, ptr - buf));
    }

    Result<void> add(TagNum tag, double value, int precision = 6) {
        char buf[64];
        auto [ptr, ec] =
            std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, precision);
        if (ec != std::errc{}) // NaN/inf or out-of-range conversion
            return fail(ErrorCode::InvalidField);
        return add(tag, std::string_view(buf, ptr - buf));
    }

    Result<void> add(TagNum tag, bool value) {
        return add(tag, value ? std::string_view("Y") : std::string_view("N"));
    }

    // Serialize a complete Message object into wire format.
    // Returns "" if any field value was rejected (see last_error()). Keeps
    // the historical std::string signature: Session::send_message consumes it
    // directly as `std::string wire = builder_.serialize(...)`.
    [[nodiscard]] std::string serialize(const Message &msg, std::string_view begin_string,
                                        SeqNum seq_num, std::string_view sender,
                                        std::string_view target, std::string_view sending_time) {
        return try_serialize(msg, begin_string, seq_num, sender, target, sending_time)
            .value_or(std::string{});
    }

    // Strict variant of serialize(): surfaces the first rejected field
    // instead of returning "". (Future session-side hardening can switch
    // Session::send_message to this without touching wire semantics.)
    [[nodiscard]] Result<std::string>
    try_serialize(const Message &msg, std::string_view begin_string, SeqNum seq_num,
                  std::string_view sender, std::string_view target, std::string_view sending_time);

    // Finalise: prefix BeginString + BodyLength, suffix CheckSum.
    // Returns "" while a latched error is present.
    [[nodiscard]] std::string finish() {
        if (last_error_)
            return {};
        return build(begin_string_, body_);
    }

    // First rejected field of the current build (empty = no error).
    [[nodiscard]] std::error_code last_error() const noexcept { return last_error_; }

    // Static helpers
    [[nodiscard]] static std::string build(std::string_view begin_string, std::string_view body);

    [[nodiscard]] static std::uint8_t compute_checksum(const char *data, std::size_t len) noexcept;

    // Format a UTC timestamp as FIX UTCTimestamp: YYYYMMDD-HH:MM:SS.sss
    [[nodiscard]] static std::string format_timestamp(TimePoint tp);
    [[nodiscard]] static std::string format_timestamp_now() { return format_timestamp(now()); }

private:
    std::string begin_string_;
    std::string body_;
    std::error_code last_error_; // sticky; cleared by begin()

    Result<void> fail(ErrorCode ec) {
        last_error_ = make_error_code(ec);
        return std::unexpected(last_error_);
    }

    Result<void> append_field(TagNum tag, std::string_view value) {
        if (last_error_) // sticky: never append after a rejected field
            return std::unexpected(last_error_);
        if (has_invalid_wire_chars(value)) // C8: NUL/SOH injection guard
            return fail(ErrorCode::InvalidField);
        char buf[12];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), tag);
        if (ec != std::errc{}) // unreachable: TagNum always formats
            return fail(ErrorCode::InvalidField);
        body_.append(buf, ptr);
        body_ += '=';
        body_.append(value.data(), value.size());
        body_ += SOH;
        return {};
    }
};

} // namespace fix
