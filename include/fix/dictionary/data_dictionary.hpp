#pragma once
// =============================================================================
// FIX Protocol Engine - Data Dictionary
// =============================================================================
#include "../core/types.hpp"
#include "../core/message.hpp"
#include "../core/constants.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>
#include <set>
#include <memory>
#include <shared_mutex>
#include <filesystem>
#include <functional>

namespace fix {

// ---------------------------------------------------------------------------
// Field metadata
// ---------------------------------------------------------------------------
enum class FieldType : std::uint8_t {
    Int,
    Float,
    Char,
    String,
    Data,
    MultipleValueString,
    Boolean,
    UTCTimestamp,
    UTCTimeOnly,
    UTCDateOnly,
    LocalMktDate,
    MonthYear,
    DayOfMonth,
    NumInGroup,
    Qty,
    Price,
    PriceOffset,
    Amt,
    Percentage,
    Exchange,
    Country,
    Currency,
    SeqNum,
    TagNum,
    Length,
    Unknown,
};

FieldType parse_field_type(std::string_view sv) noexcept;
std::string_view to_string(FieldType t) noexcept;

struct FieldDef {
    TagNum      tag  = 0;
    std::string name;
    FieldType   type = FieldType::Unknown;
    std::set<std::string> valid_values;  // non-empty = enumerated
    bool        required = false;

    [[nodiscard]] bool is_enum() const noexcept { return !valid_values.empty(); }
    [[nodiscard]] bool validate_value(std::string_view v) const noexcept {
        if (!is_enum()) return true;
        return valid_values.count(std::string(v)) > 0;
    }
};

// ---------------------------------------------------------------------------
// Message definition
// ---------------------------------------------------------------------------
struct MessageDef {
    std::string          msg_type;
    std::string          name;
    bool                 is_admin   = false;
    std::vector<TagNum>  required_tags;
    std::vector<TagNum>  optional_tags;
    // repeating group delimiters: key = delimiter tag, value = member tags
    std::unordered_map<TagNum, std::vector<TagNum>> groups;
};

// ---------------------------------------------------------------------------
// DataDictionary
//
// Thread-safe read after initial load.  Supports hot-reload via reload().
// ---------------------------------------------------------------------------
class DataDictionary {
public:
    DataDictionary()  = default;
    ~DataDictionary() = default;

    // Load from an FIX XML specification file (e.g., FIX44.xml)
    Result<void> load(const std::filesystem::path& path);

    // Load the built-in minimal dictionary for a given version
    void load_builtin(FixVersion version);

    // Hot-reload: atomically replaces internal tables
    Result<void> reload(const std::filesystem::path& path);

    // --- Field lookup -------------------------------------------------------
    [[nodiscard]] const FieldDef* find_field(TagNum tag) const noexcept;
    [[nodiscard]] const FieldDef* find_field(std::string_view name) const noexcept;

    // --- Message lookup -----------------------------------------------------
    [[nodiscard]] const MessageDef* find_message(std::string_view msg_type) const noexcept;
    [[nodiscard]] const MessageDef* find_message_by_name(std::string_view name) const noexcept;

    // --- Validation ---------------------------------------------------------
    [[nodiscard]] Result<void> validate(const Message& msg) const;

    // --- Custom extension ---------------------------------------------------
    void register_field(FieldDef def);
    void register_message(MessageDef def);

    [[nodiscard]] FixVersion version() const noexcept { return version_; }
    [[nodiscard]] std::string_view version_string() const noexcept { return version_string_; }

private:
    mutable std::shared_mutex                        mutex_;
    FixVersion                                       version_ = FixVersion::Unknown;
    std::string                                      version_string_;
    std::unordered_map<TagNum, FieldDef>             fields_by_tag_;
    std::unordered_map<std::string, FieldDef*>       fields_by_name_;
    std::unordered_map<std::string, MessageDef>      messages_by_type_;
    std::unordered_map<std::string, MessageDef*>     messages_by_name_;

    void rebuild_name_index();
    void load_builtin_fields();
    void load_builtin_messages_42();
    void load_builtin_messages_44();
    void load_builtin_messages_50sp2();
};

// Global per-version dictionaries (lazily initialised)
class DictionaryRegistry {
public:
    static DictionaryRegistry& instance();

    [[nodiscard]] const DataDictionary* get(FixVersion v) const noexcept;
    void set(FixVersion v, std::shared_ptr<DataDictionary> dict);

    // Resolve ApplVerID string to FixVersion
    [[nodiscard]] static FixVersion resolve_appl_ver_id(std::string_view id) noexcept;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<int, std::shared_ptr<DataDictionary>> dicts_;
};

} // namespace fix
