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
// A FIX message: ordered list of fields, plus metadata extracted at parse time
// ---------------------------------------------------------------------------
class Message {
public:
    Message() = default;
    explicit Message(std::string_view msg_type) { set(tags::MsgType, msg_type); }

    // --- Field access -------------------------------------------------------
    void set(TagNum tag, std::string_view value) {
        for (auto &f : fields_) {
            if (f.tag == tag) {
                f.value = value;
                return;
            }
        }
        fields_.emplace_back(tag, value);
    }

    // Explicit const char* overload to prevent char*→bool conversion
    void set(TagNum tag, const char *value) { set(tag, std::string_view(value)); }

    void set(TagNum tag, std::string v) {
        for (auto &f : fields_) {
            if (f.tag == tag) {
                f.value = std::move(v);
                return;
            }
        }
        fields_.emplace_back(tag, std::move(v));
    }

    void set(TagNum tag, std::int64_t value) { set(tag, std::to_string(value)); }

    void set(TagNum tag, double value) {
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, 6);
        set(tag, std::string_view(buf, res.ptr));
    }

    void set(TagNum tag, bool value) {
        set(tag, value ? std::string_view("Y") : std::string_view("N"));
    }

    [[nodiscard]] std::optional<std::string_view> get(TagNum tag) const noexcept {
        for (const auto &f : fields_) {
            if (f.tag == tag)
                return f.value;
        }
        return std::nullopt;
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

    // --- Direct field list access -------------------------------------------
    [[nodiscard]] const std::vector<Field> &fields() const noexcept { return fields_; }
    [[nodiscard]] std::vector<Field> &fields() noexcept { return fields_; }

    void clear() {
        fields_.clear();
        raw_.clear();
    }

    // Store the original raw bytes (for audit log / retransmission)
    void set_raw(std::string raw) { raw_ = std::move(raw); }
    [[nodiscard]] const std::string &raw() const noexcept { return raw_; }

private:
    std::vector<Field> fields_;
    std::string raw_; // original wire bytes
};

// Convenience alias
using MessagePtr = std::unique_ptr<Message>;
using MessageRef = const Message &;

} // namespace fix
