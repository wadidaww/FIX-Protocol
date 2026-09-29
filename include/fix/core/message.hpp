#pragma once
// =============================================================================
// FIX Protocol Engine - Message
// =============================================================================
#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "constants.hpp"
#include "field.hpp"
#include "types.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// Wire-character validation (framing safety, C8)
//
// A FIX field value must never contain NUL (0x00) or SOH (0x01): SOH is the
// field delimiter, so embedding it in a value would inject additional
// Tag=Value fields on the wire; NUL corrupts C-string consumers downstream.
// Every API that accepts a field value (Message::set/add, MessageBuilder)
// validates through this helper and rejects the value.
//
// Other C0 control bytes are deliberately NOT rejected here: they are
// spec-invalid but do not break SOH framing, and being conservative avoids
// rejecting data a counterparty might legitimately (if unusually) send.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool has_invalid_wire_chars(std::string_view value) noexcept {
    for (unsigned char c : value) {
        if (c == 0x00 || c == 0x01)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// A FIX message: ordered list of fields, plus metadata extracted at parse time
//
// Field access rules:
//  * set(tag, ...)  — upsert semantics for application use: replaces the value
//                     of the FIRST existing occurrence, or appends.
//  * add(tag, ...)  — always appends, preserving wire order. Used by the
//                     parser so repeating groups (duplicate body tags) are
//                     retained instead of deduplicated (C7).
//  * get(tag)       — FIRST-occurrence-wins: returns the earliest stored
//                     field with that tag. Header fields are parsed before
//                     body fields, so header accessors (seq_num(),
//                     sender_comp_id(), ...) return the first-seen value even
//                     if a duplicate slipped through.
//  * get_all(tag)   — all occurrences in stored (wire) order.
//
// set()/add() return Result<void>: values are validated with
// has_invalid_wire_chars(); an invalid value is rejected and the message left
// unchanged. The result is intentionally NOT [[nodiscard]] so existing
// void-style call sites (session, apps, README examples) keep compiling —
// callers that must not silently drop a field should check it.
// ---------------------------------------------------------------------------
class Message {
public:
    Message() = default;
    explicit Message(std::string_view msg_type) { (void)set(tags::MsgType, msg_type); }

    // --- Field access -------------------------------------------------------
    // Upsert. Rejects values containing NUL/SOH (see has_invalid_wire_chars).
    Result<void> set(TagNum tag, std::string_view value) {
        if (has_invalid_wire_chars(value))
            return make_unexpected(ErrorCode::InvalidField);
        for (auto &f : fields_) {
            if (f.tag == tag) {
                f.value = value;
                return {};
            }
        }
        fields_.emplace_back(tag, value);
        return {};
    }

    // Explicit const char* overload to prevent char*→bool conversion
    Result<void> set(TagNum tag, const char *value) { return set(tag, std::string_view(value)); }

    Result<void> set(TagNum tag, std::string v) {
        if (has_invalid_wire_chars(v))
            return make_unexpected(ErrorCode::InvalidField);
        for (auto &f : fields_) {
            if (f.tag == tag) {
                f.value = std::move(v);
                return {};
            }
        }
        fields_.emplace_back(tag, std::move(v));
        return {};
    }

    Result<void> set(TagNum tag, std::int64_t value) { return set(tag, std::to_string(value)); }

    Result<void> set(TagNum tag, double value) {
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, 6);
        return set(tag, std::string_view(buf, res.ptr));
    }

    Result<void> set(TagNum tag, bool value) {
        return set(tag, value ? std::string_view("Y") : std::string_view("N"));
    }

    // Append a NEW occurrence of `tag` (repeating groups / duplicate tags),
    // preserving wire order. Rejects NUL/SOH values like set().
    Result<void> add(TagNum tag, std::string_view value) {
        if (has_invalid_wire_chars(value))
            return make_unexpected(ErrorCode::InvalidField);
        fields_.emplace_back(tag, value);
        return {};
    }

    Result<void> add(TagNum tag, const char *value) { return add(tag, std::string_view(value)); }

    Result<void> add(TagNum tag, std::string v) {
        if (has_invalid_wire_chars(v))
            return make_unexpected(ErrorCode::InvalidField);
        fields_.emplace_back(tag, std::move(v));
        return {};
    }

    Result<void> add(TagNum tag, std::int64_t value) { return add(tag, std::to_string(value)); }

    Result<void> add(TagNum tag, double value) {
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, 6);
        return add(tag, std::string_view(buf, res.ptr));
    }

    Result<void> add(TagNum tag, bool value) {
        return add(tag, value ? std::string_view("Y") : std::string_view("N"));
    }

    // First-occurrence-wins (see class comment).
    [[nodiscard]] std::optional<std::string_view> get(TagNum tag) const noexcept {
        for (const auto &f : fields_) {
            if (f.tag == tag)
                return f.value;
        }
        return std::nullopt;
    }

    // All occurrences of `tag`, in stored (wire) order — repeating groups.
    [[nodiscard]] std::vector<std::string_view> get_all(TagNum tag) const {
        std::vector<std::string_view> out;
        for (const auto &f : fields_) {
            if (f.tag == tag)
                out.push_back(f.value);
        }
        return out;
    }

    [[nodiscard]] bool has(TagNum tag) const noexcept { return get(tag).has_value(); }

    [[nodiscard]] std::optional<std::int64_t> get_int(TagNum tag) const noexcept {
        auto sv = get(tag);
        if (!sv)
            return std::nullopt;
        std::int64_t v{};
        auto [ptr, ec] = std::from_chars(sv->data(), sv->data() + sv->size(), v);
        if (ec == std::errc{})
            return v;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> get_double(TagNum tag) const noexcept {
        auto sv = get(tag);
        if (!sv)
            return std::nullopt;
        std::string tmp(*sv);
        char *end{};
        double d = std::strtod(tmp.c_str(), &end);
        if (end != tmp.c_str() + tmp.size())
            return std::nullopt;
        return d;
    }

    void remove(TagNum tag) {
        auto it = std::remove_if(fields_.begin(), fields_.end(),
                                 [tag](const Field &f) { return f.tag == tag; });
        fields_.erase(it, fields_.end());
    }

    // --- Standard header helpers -------------------------------------------
    [[nodiscard]] std::string_view msg_type() const noexcept {
        return get(tags::MsgType).value_or("");
    }

    [[nodiscard]] FixVersion begin_string_version() const noexcept {
        return parse_version(get(tags::BeginString).value_or(""));
    }

    [[nodiscard]] SeqNum seq_num() const noexcept {
        return static_cast<SeqNum>(get_int(tags::MsgSeqNum).value_or(0));
    }

    [[nodiscard]] std::string_view sender_comp_id() const noexcept {
        return get(tags::SenderCompID).value_or("");
    }

    [[nodiscard]] std::string_view target_comp_id() const noexcept {
        return get(tags::TargetCompID).value_or("");
    }

    [[nodiscard]] bool poss_dup() const noexcept {
        return get(tags::PossDupFlag).value_or("N") == "Y";
    }

    // --- Duplicate standard-header detection (C7) ---------------------------
    // Set by StreamParser when a standard-header tag (8/9/35/49/50/56/57/115/
    // 128/90/91/34/43/122/52/10) appeared more than once in one message
    // (FIX session Reject reason 13, TagAppearsMoreThanOnce). The message is
    // still delivered — first occurrence of each header field wins — so the
    // session layer can send a proper Reject and consume the sequence number.
    // The parser also surfaces ErrorCode::DuplicateField via last_error().
    [[nodiscard]] bool has_duplicate_header() const noexcept { return duplicate_header_; }
    void mark_duplicate_header() noexcept { duplicate_header_ = true; }

    // --- Direct field list access -------------------------------------------
    [[nodiscard]] const std::vector<Field> &fields() const noexcept { return fields_; }
    [[nodiscard]] std::vector<Field> &fields() noexcept { return fields_; }

    void clear() {
        fields_.clear();
        raw_.clear();
        duplicate_header_ = false;
    }

    // Store the original raw bytes (for audit log / retransmission)
    void set_raw(std::string raw) { raw_ = std::move(raw); }
    [[nodiscard]] const std::string &raw() const noexcept { return raw_; }

private:
    std::vector<Field> fields_;
    std::string raw_; // original wire bytes
    bool duplicate_header_ = false;
};

// Convenience alias
using MessagePtr = std::unique_ptr<Message>;
using MessageRef = const Message &;

} // namespace fix
