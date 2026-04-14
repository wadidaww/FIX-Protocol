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
//   b.begin("FIX.4.2", "D");
//   b.add(tags::SenderCompID, "SENDER");
//   b.add(tags::TargetCompID, "TARGET");
//   b.add(tags::MsgSeqNum, 1);
//   ...
//   std::string wire = b.finish(); // appends BodyLength(9) and CheckSum(10)
// ---------------------------------------------------------------------------
class MessageBuilder {
public:
    MessageBuilder() { body_.reserve(512); }

    void begin(std::string_view begin_string, std::string_view msg_type) {
        begin_string_ = begin_string;
        body_.clear();
        // MsgType must come first in the body (after the fixed header tags 8,9)
        append_field(tags::MsgType, msg_type);
    }

    void add(TagNum tag, std::string_view value) { append_field(tag, value); }

    // Explicit const char* overload to prevent char*→bool conversion
    void add(TagNum tag, const char *value) { add(tag, std::string_view(value)); }

    void add(TagNum tag, std::int64_t value) {
        char buf[24];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
        add(tag, std::string_view(buf, ptr - buf));
    }

    void add(TagNum tag, double value, int precision = 6) {
        char buf[64];
        auto [ptr, ec] =
            std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, precision);
        add(tag, std::string_view(buf, ptr - buf));
    }

    void add(TagNum tag, bool value) {
        add(tag, value ? std::string_view("Y") : std::string_view("N"));
    }

    // Serialize a complete Message object into wire format
    [[nodiscard]] std::string serialize(const Message &msg, std::string_view begin_string,
                                        SeqNum seq_num, std::string_view sender,
                                        std::string_view target, std::string_view sending_time);

    // Finalise: prefix BeginString + BodyLength, suffix CheckSum
    [[nodiscard]] std::string finish() { return build(begin_string_, body_); }

    // Static helpers
    [[nodiscard]] static std::string build(std::string_view begin_string, std::string_view body);

    [[nodiscard]] static std::uint8_t compute_checksum(const char *data, std::size_t len) noexcept;

    // Format a UTC timestamp as FIX UTCTimestamp: YYYYMMDD-HH:MM:SS.sss
    [[nodiscard]] static std::string format_timestamp(TimePoint tp);
    [[nodiscard]] static std::string format_timestamp_now() { return format_timestamp(now()); }

private:
    std::string begin_string_;
    std::string body_;

    void append_field(TagNum tag, std::string_view value) {
        char buf[12];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), tag);
        body_.append(buf, ptr);
        body_ += '=';
        body_.append(value.data(), value.size());
        body_ += SOH;
    }
};

} // namespace fix
