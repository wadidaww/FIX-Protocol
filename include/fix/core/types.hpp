#pragma once
// =============================================================================
// FIX Protocol Engine - Core Types
// =============================================================================
#include <cstdint>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <array>
#include <optional>
#include <variant>
#include <expected>
#include <system_error>
#include <functional>
#include <chrono>
#include <atomic>
#include <memory>
#include <cstring>

namespace fix {

// ---------------------------------------------------------------------------
// Fundamental integer types
// ---------------------------------------------------------------------------
using TagNum  = std::uint32_t;
using SeqNum  = std::uint64_t;
using Price   = double;
using Qty     = double;

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------
enum class ErrorCode : int {
    Ok                   = 0,
    ParseError           = 1,
    ValidationError      = 2,
    SequenceError        = 3,
    SessionError         = 4,
    TransportError       = 5,
    StoreError           = 6,
    DictionaryError      = 7,
    TimeoutError         = 8,
    AuthError            = 9,
    UnsupportedVersion   = 10,
    MissingField         = 11,
    InvalidField         = 12,
    DuplicateField       = 13,
    BadChecksum          = 14,
    BadLength            = 15,
    UnknownMessage       = 16,
    RejectCode           = 17,
};

struct FixErrorCategory : std::error_category {
    const char* name() const noexcept override { return "fix"; }
    std::string message(int ev) const override {
        switch (static_cast<ErrorCode>(ev)) {
            case ErrorCode::Ok:                 return "ok";
            case ErrorCode::ParseError:         return "parse error";
            case ErrorCode::ValidationError:    return "validation error";
            case ErrorCode::SequenceError:      return "sequence error";
            case ErrorCode::SessionError:       return "session error";
            case ErrorCode::TransportError:     return "transport error";
            case ErrorCode::StoreError:         return "store error";
            case ErrorCode::DictionaryError:    return "dictionary error";
            case ErrorCode::TimeoutError:       return "timeout";
            case ErrorCode::AuthError:          return "authentication error";
            case ErrorCode::UnsupportedVersion: return "unsupported FIX version";
            case ErrorCode::MissingField:       return "missing required field";
            case ErrorCode::InvalidField:       return "invalid field value";
            case ErrorCode::DuplicateField:     return "duplicate field";
            case ErrorCode::BadChecksum:        return "bad checksum";
            case ErrorCode::BadLength:          return "bad body length";
            case ErrorCode::UnknownMessage:     return "unknown message type";
            case ErrorCode::RejectCode:         return "message rejected";
            default:                            return "unknown fix error";
        }
    }
};

inline const FixErrorCategory& fix_error_category() noexcept {
    static FixErrorCategory cat;
    return cat;
}

inline std::error_code make_error_code(ErrorCode e) noexcept {
    return {static_cast<int>(e), fix_error_category()};
}

template <typename T>
using Result = std::expected<T, std::error_code>;

inline auto make_unexpected(ErrorCode ec) {
    return std::unexpected(make_error_code(ec));
}

// ---------------------------------------------------------------------------
// FIX protocol version
// ---------------------------------------------------------------------------
enum class FixVersion : std::uint8_t {
    FIX_4_0    = 0,
    FIX_4_1    = 1,
    FIX_4_2    = 2,
    FIX_4_3    = 3,
    FIX_4_4    = 4,
    FIX_5_0    = 5,
    FIX_5_0SP1 = 6,
    FIX_5_0SP2 = 7,
    FIXT_1_1   = 8,
    Unknown    = 255,
};

constexpr std::string_view to_string(FixVersion v) noexcept {
    switch (v) {
        case FixVersion::FIX_4_0:    return "FIX.4.0";
        case FixVersion::FIX_4_1:    return "FIX.4.1";
        case FixVersion::FIX_4_2:    return "FIX.4.2";
        case FixVersion::FIX_4_3:    return "FIX.4.3";
        case FixVersion::FIX_4_4:    return "FIX.4.4";
        case FixVersion::FIX_5_0:    return "FIX.5.0";
        case FixVersion::FIX_5_0SP1: return "FIX.5.0SP1";
        case FixVersion::FIX_5_0SP2: return "FIX.5.0SP2";
        case FixVersion::FIXT_1_1:   return "FIXT.1.1";
        default:                     return "Unknown";
    }
}

inline FixVersion parse_version(std::string_view sv) noexcept {
    if (sv == "FIX.4.0")    return FixVersion::FIX_4_0;
    if (sv == "FIX.4.1")    return FixVersion::FIX_4_1;
    if (sv == "FIX.4.2")    return FixVersion::FIX_4_2;
    if (sv == "FIX.4.3")    return FixVersion::FIX_4_3;
    if (sv == "FIX.4.4")    return FixVersion::FIX_4_4;
    if (sv == "FIX.5.0")    return FixVersion::FIX_5_0;
    if (sv == "FIX.5.0SP1") return FixVersion::FIX_5_0SP1;
    if (sv == "FIX.5.0SP2") return FixVersion::FIX_5_0SP2;
    if (sv == "FIXT.1.1")   return FixVersion::FIXT_1_1;
    return FixVersion::Unknown;
}

// ---------------------------------------------------------------------------
// Timestamp helpers
// ---------------------------------------------------------------------------
using Clock     = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<Clock>;
using Duration  = std::chrono::nanoseconds;

inline TimePoint now() noexcept { return Clock::now(); }

// ---------------------------------------------------------------------------
// Session identifier
// ---------------------------------------------------------------------------
struct SessionID {
    FixVersion version;
    std::string senderCompID;
    std::string targetCompID;
    std::string qualifier; // optional session qualifier

    bool operator==(const SessionID& o) const noexcept {
        return version == o.version &&
               senderCompID == o.senderCompID &&
               targetCompID == o.targetCompID &&
               qualifier == o.qualifier;
    }

    std::string to_string() const {
        std::string s(fix::to_string(version));
        s += ':';
        s += senderCompID;
        s += "->";
        s += targetCompID;
        if (!qualifier.empty()) { s += '/'; s += qualifier; }
        return s;
    }
};

struct SessionIDHash {
    std::size_t operator()(const SessionID& sid) const noexcept {
        std::size_t h = std::hash<int>{}(static_cast<int>(sid.version));
        h ^= std::hash<std::string>{}(sid.senderCompID) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(sid.targetCompID) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(sid.qualifier)    + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ---------------------------------------------------------------------------
// SOH delimiter (tag=value\x01)
// ---------------------------------------------------------------------------
inline constexpr char SOH = '\x01';

} // namespace fix

// Allow ErrorCode to be used with std::error_code
template <>
struct std::is_error_code_enum<fix::ErrorCode> : std::true_type {};
