// =============================================================================
// FIX Protocol Engine - StreamParser implementation
// =============================================================================
#include "fix/parser/parser.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace fix {

StreamParser::StreamParser(Options opts)
    : opts_(opts) {
    buf_.reserve(65536);
}

void StreamParser::feed(std::span<const std::byte> data) {
    feed(reinterpret_cast<const char *>(data.data()), data.size());
}

void StreamParser::feed(const char *data, std::size_t len) {
    if (len == 0)
        return;
    if (write_pos_ + len > buf_.size()) {
        compact();
        if (write_pos_ + len > buf_.size()) {
            buf_.resize(write_pos_ + len + 65536);
        }
    }
    std::memcpy(buf_.data() + write_pos_, data, len);
    write_pos_ += len;

    // Try to parse as many complete messages as possible
    while (try_parse_one()) {}
}

bool StreamParser::next(Message &out) {
    if (pending_pos_ >= pending_.size()) {
        pending_.clear();
        pending_pos_ = 0;
        return false;
    }
    out = std::move(pending_[pending_pos_++]);
    return true;
}

void StreamParser::reset() {
    buf_.clear();
    read_pos_ = 0;
    write_pos_ = 0;
    consumed_ = 0;
    pending_.clear();
    pending_pos_ = 0;
    last_error_ = {};
}

void StreamParser::compact() {
    if (read_pos_ == 0)
        return;
    std::size_t remaining = write_pos_ - read_pos_;
    if (remaining > 0) {
        std::memmove(buf_.data(), buf_.data() + read_pos_, remaining);
    }
    write_pos_ = remaining;
    read_pos_ = 0;
}

// parse_field: parse one Tag=Value\x01 field from [data, data+len)
// Returns bytes consumed (including SOH), or 0 if incomplete
std::size_t StreamParser::parse_field(const char *data, std::size_t len, TagNum &tag,
                                      std::string_view &value) {
    // Find '=' separator
    std::size_t eq = 0;
    while (eq < len && data[eq] != '=' && data[eq] != SOH)
        ++eq;
    if (eq >= len || data[eq] != '=')
        return 0; // need more data

    // Parse tag number
    TagNum t{};
    auto [ptr, ec] = std::from_chars(data, data + eq, t);
    if (ec != std::errc{} || ptr != data + eq)
        return 0; // bad tag

    // Find value end (SOH)
    std::size_t soh = eq + 1;
    while (soh < len && data[soh] != SOH)
        ++soh;
    if (soh >= len)
        return 0; // need more data

    tag = t;
    value = std::string_view(data + eq + 1, soh - eq - 1);
    return soh + 1; // consumed up to and including SOH
}

std::uint8_t StreamParser::compute_checksum(const char *data, std::size_t len) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < len; ++i)
        sum += static_cast<unsigned char>(data[i]);
    return static_cast<std::uint8_t>(sum & 0xFF);
}

std::uint32_t StreamParser::checksum_to_int(std::string_view sv) noexcept {
    std::uint32_t v{};
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

bool StreamParser::try_parse_one() {
    const char *base = buf_.data() + read_pos_;
    std::size_t avail = write_pos_ - read_pos_;
    if (avail < 20)
        return false; // minimum viable message

    // We expect the message to start with tag 8 (BeginString)
    // Quick check
    if (avail < 5 || base[0] != '8' || base[1] != '=')
        return false;

    // Parse fields one by one until we accumulate a full message
    std::size_t pos = 0;
    TagNum tag = 0;
    std::string_view value;

    // -- Tag 8 (BeginString) -------------------------------------------------
    std::size_t consumed = parse_field(base + pos, avail - pos, tag, value);
    if (consumed == 0 || tag != tags::BeginString)
        return false;
    std::string begin_string(value);
    pos += consumed;

    // -- Tag 9 (BodyLength) --------------------------------------------------
    consumed = parse_field(base + pos, avail - pos, tag, value);
    if (consumed == 0 || tag != tags::BodyLength)
        return false;
    std::size_t body_length = 0;
    std::from_chars(value.data(), value.data() + value.size(), body_length);
    pos += consumed;

    if (opts_.max_msg_len > 0 && body_length > opts_.max_msg_len) {
        last_error_ = make_error_code(ErrorCode::BadLength);
        read_pos_ += pos; // skip to avoid infinite loop
        return false;
    }

    // body starts here; we need body_length bytes + "10=xxx\x01"
    std::size_t body_start = pos;
    std::size_t needed = body_start + body_length + 7; // 10=xxx\x01 at minimum
    if (avail < needed)
        return false; // wait for more data

    // Parse body fields until we hit tag 10
    Message msg;
    msg.set(tags::BeginString, begin_string);
    // We will set BodyLength too for completeness
    msg.set(tags::BodyLength, static_cast<std::int64_t>(body_length));

    while (pos < avail) {
        consumed = parse_field(base + pos, avail - pos, tag, value);
        if (consumed == 0)
            return false; // need more data
        if (tag == tags::CheckSum) {
            pos += consumed;
            // Validate checksum if requested
            if (opts_.validate_checksum) {
                std::uint8_t expected = compute_checksum(base, pos - consumed);
                std::uint32_t provided = checksum_to_int(value);
                if (expected != static_cast<std::uint8_t>(provided)) {
                    last_error_ = make_error_code(ErrorCode::BadChecksum);
                    // Skip this broken message
                    read_pos_ += pos;
                    consumed_ = read_pos_;
                    return false;
                }
            }
            // Validate body length if requested
            if (opts_.validate_body_len) {
                std::size_t actual_body = pos - consumed - body_start;
                if (actual_body != body_length) {
                    last_error_ = make_error_code(ErrorCode::BadLength);
                    read_pos_ += pos;
                    consumed_ = read_pos_;
                    return false;
                }
            }
            // Store raw bytes
            msg.set_raw(std::string(base, pos));
            // Finalise
            pending_.push_back(std::move(msg));
            ++msg_count_;
            read_pos_ += pos;
            consumed_ = read_pos_;
            return true;
        }
        msg.set(tag, std::string(value));
        pos += consumed;
    }
    return false; // incomplete
}

} // namespace fix
