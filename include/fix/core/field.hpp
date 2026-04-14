#pragma once
// =============================================================================
// FIX Protocol Engine - Field (Tag=Value pair)
// =============================================================================
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>

#include "types.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// A raw field: tag number + raw bytes view (zero-copy from parse buffer)
// ---------------------------------------------------------------------------
struct RawField {
    TagNum tag = 0;
    std::string_view value; // points into the parse buffer – do NOT outlive buffer
};

// ---------------------------------------------------------------------------
// A field value type-tagged union
// ---------------------------------------------------------------------------
using FieldValue = std::variant<std::monostate, // absent
                                std::int64_t,   // integer numeric
                                double,         // float/price
                                std::string,    // char/string
                                bool            // boolean Y/N
                                >;

// ---------------------------------------------------------------------------
// Owned field with heap-allocated value string
// ---------------------------------------------------------------------------
struct Field {
    TagNum tag = 0;
    std::string value;

    Field() = default;
    Field(TagNum t, std::string_view v)
        : tag(t),
          value(v) {}
    Field(TagNum t, std::string v)
        : tag(t),
          value(std::move(v)) {}

    [[nodiscard]] std::string_view view() const noexcept { return value; }

    [[nodiscard]] std::optional<std::int64_t> as_int() const noexcept {
        std::int64_t result{};
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (ec == std::errc{})
            return result;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> as_double() const noexcept {
        // std::from_chars for float not universally available; fall back to strtod
        char *end = nullptr;
        double d = std::strtod(value.c_str(), &end);
        if (end != value.c_str() + value.size())
            return std::nullopt;
        return d;
    }

    [[nodiscard]] std::optional<bool> as_bool() const noexcept {
        if (value == "Y" || value == "y")
            return true;
        if (value == "N" || value == "n")
            return false;
        return std::nullopt;
    }

    [[nodiscard]] bool operator==(const Field &o) const noexcept {
        return tag == o.tag && value == o.value;
    }
};

} // namespace fix
