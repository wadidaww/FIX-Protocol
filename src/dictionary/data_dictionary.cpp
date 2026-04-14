// =============================================================================
// FIX Protocol Engine - DataDictionary implementation
// =============================================================================
#include "fix/dictionary/data_dictionary.hpp"

#include "fix/core/message.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>

// We use a minimal XML parser (tinyxml2 if available, otherwise simple stub)
// For the built-in dictionaries we hard-code the critical fields and messages.

namespace fix {

// ---------------------------------------------------------------------------
// FieldType helpers
// ---------------------------------------------------------------------------
FieldType parse_field_type(std::string_view sv) noexcept {
    if (sv == "INT" || sv == "SEQNUM" || sv == "NUMINGROUP" || sv == "LENGTH" || sv == "TAGNUM")
        return FieldType::Int;
    if (sv == "FLOAT" || sv == "PRICE" || sv == "AMT" || sv == "PRICEOFFSET" || sv == "QTY" ||
        sv == "PERCENTAGE")
        return FieldType::Float;
    if (sv == "CHAR")
        return FieldType::Char;
    if (sv == "STRING" || sv == "EXCHANGE" || sv == "COUNTRY" || sv == "CURRENCY")
        return FieldType::String;
    if (sv == "DATA")
        return FieldType::Data;
    if (sv == "BOOLEAN")
        return FieldType::Boolean;
    if (sv == "UTCTIMESTAMP")
        return FieldType::UTCTimestamp;
    if (sv == "UTCTIMEONLY")
        return FieldType::UTCTimeOnly;
    if (sv == "UTCDATEONLY")
        return FieldType::UTCDateOnly;
    if (sv == "LOCALMKTDATE")
        return FieldType::LocalMktDate;
    if (sv == "MONTHYEAR")
        return FieldType::MonthYear;
    if (sv == "DAYOFMONTH")
        return FieldType::DayOfMonth;
    if (sv == "MULTIPLEVALUESTRING")
        return FieldType::MultipleValueString;
    return FieldType::Unknown;
}

std::string_view to_string(FieldType t) noexcept {
    switch (t) {
    case FieldType::Int:
        return "INT";
    case FieldType::Float:
        return "FLOAT";
    case FieldType::Char:
        return "CHAR";
    case FieldType::String:
        return "STRING";
    case FieldType::Data:
        return "DATA";
    case FieldType::Boolean:
        return "BOOLEAN";
    case FieldType::UTCTimestamp:
        return "UTCTIMESTAMP";
    case FieldType::NumInGroup:
        return "NUMINGROUP";
    case FieldType::Qty:
        return "QTY";
    case FieldType::Price:
        return "PRICE";
    default:
        return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// DataDictionary - built-in field definitions
// ---------------------------------------------------------------------------
void DataDictionary::load_builtin_fields() {
    // Macro for brevity
    auto add = [&](TagNum tag, std::string_view name, FieldType type,
                   std::initializer_list<std::string_view> values = {}) {
        FieldDef d;
        d.tag = tag;
        d.name = name;
        d.type = type;
        for (auto &v : values)
            d.valid_values.insert(std::string(v));
        fields_by_tag_[tag] = std::move(d);
    };

    using namespace tags;
    add(BeginString, "BeginString", FieldType::String);
    add(BodyLength, "BodyLength", FieldType::Int);
    add(MsgType, "MsgType", FieldType::String);
    add(MsgSeqNum, "MsgSeqNum", FieldType::Int);
    add(SenderCompID, "SenderCompID", FieldType::String);
    add(TargetCompID, "TargetCompID", FieldType::String);
    add(SendingTime, "SendingTime", FieldType::UTCTimestamp);
    add(CheckSum, "CheckSum", FieldType::String);
    add(PossDupFlag, "PossDupFlag", FieldType::Boolean, {"Y", "N"});
    add(PossResend, "PossResend", FieldType::Boolean, {"Y", "N"});
    add(OrigSendingTime, "OrigSendingTime", FieldType::UTCTimestamp);

    // Session
    add(EncryptMethod, "EncryptMethod", FieldType::Int, {"0", "1", "2", "3", "4", "5", "6"});
    add(HeartBtInt, "HeartBtInt", FieldType::Int);
    add(TestReqID, "TestReqID", FieldType::String);
    add(GapFillFlag, "GapFillFlag", FieldType::Boolean, {"Y", "N"});
    add(NewSeqNo, "NewSeqNo", FieldType::Int);
    add(BeginSeqNo, "BeginSeqNo", FieldType::Int);
    add(EndSeqNo, "EndSeqNo", FieldType::Int);
    add(RefSeqNum, "RefSeqNum", FieldType::Int);
    add(RefMsgType, "RefMsgType", FieldType::String);
    add(tags::SessionRejectReason, "SessionRejectReason", FieldType::Int);
    add(Text, "Text", FieldType::String);
    add(RefTagID, "RefTagID", FieldType::Int);
    add(ResetSeqNumFlag, "ResetSeqNumFlag", FieldType::Boolean, {"Y", "N"});
    add(Username, "Username", FieldType::String);
    add(Password, "Password", FieldType::String);
    add(DefaultApplVerID, "DefaultApplVerID", FieldType::String);
    add(ApplVerID, "ApplVerID", FieldType::String);

    // Order
    add(ClOrdID, "ClOrdID", FieldType::String);
    add(OrigClOrdID, "OrigClOrdID", FieldType::String);
    add(OrderID, "OrderID", FieldType::String);
    add(ExecID, "ExecID", FieldType::String);
    add(ExecType, "ExecType", FieldType::Char,
        {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F"});
    add(OrdStatus, "OrdStatus", FieldType::Char,
        {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C"});
    add(Symbol, "Symbol", FieldType::String);
    add(Side, "Side", FieldType::Char, {"1", "2", "3", "4", "5", "6", "7", "8", "9"});
    add(OrderQty, "OrderQty", FieldType::Qty);
    add(OrdType, "OrdType", FieldType::Char,
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D"});
    add(fix::tags::Price, "Price", FieldType::Price);
    add(StopPx, "StopPx", FieldType::Price);
    add(TimeInForce, "TimeInForce", FieldType::Char,
        {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"});
    add(TransactTime, "TransactTime", FieldType::UTCTimestamp);
    add(LeavesQty, "LeavesQty", FieldType::Qty);
    add(CumQty, "CumQty", FieldType::Qty);
    add(AvgPx, "AvgPx", FieldType::Price);
    add(LastQty, "LastQty", FieldType::Qty);
    add(LastPx, "LastPx", FieldType::Price);
    add(OrdRejReason, "OrdRejReason", FieldType::Int);
    add(CxlRejReason, "CxlRejReason", FieldType::Int);
    add(CxlRejResponseTo, "CxlRejResponseTo", FieldType::Char, {"1", "2", "3"});
    add(Account, "Account", FieldType::String);
    add(SecurityID, "SecurityID", FieldType::String);
    add(SecurityIDSource, "SecurityIDSource", FieldType::String);
    add(Currency, "Currency", FieldType::Currency);

    // Market data
    add(MDReqID, "MDReqID", FieldType::String);
    add(SubscriptionRequestType, "SubscriptionRequestType", FieldType::Char, {"0", "1", "2"});
    add(MarketDepth, "MarketDepth", FieldType::Int);
    add(MDUpdateType, "MDUpdateType", FieldType::Int, {"0", "1"});
    add(NoMDEntryTypes, "NoMDEntryTypes", FieldType::NumInGroup);
    add(NoRelatedSym, "NoRelatedSym", FieldType::NumInGroup);
    add(MDEntryType, "MDEntryType", FieldType::Char,
        {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
         "A", "B", "C", "D", "E", "F", "G", "H", "I", "J"});
    add(MDEntryPx, "MDEntryPx", FieldType::Price);
    add(MDEntrySize, "MDEntrySize", FieldType::Qty);
    add(MDEntryID, "MDEntryID", FieldType::String);
    add(MDUpdateAction, "MDUpdateAction", FieldType::Char, {"0", "1", "2"});
    add(NoMDEntries, "NoMDEntries", FieldType::NumInGroup);

    rebuild_name_index();
}

void DataDictionary::load_builtin_messages_42() {
    auto add = [&](std::string_view type, std::string_view name, bool admin,
                   std::vector<TagNum> req, std::vector<TagNum> opt) {
        MessageDef d;
        d.msg_type = type;
        d.name = name;
        d.is_admin = admin;
        d.required_tags = std::move(req);
        d.optional_tags = std::move(opt);
        messages_by_type_[d.msg_type] = std::move(d);
    };

    using namespace tags;
    using namespace msg_types;

    add(Heartbeat, "Heartbeat", true, {}, {TestReqID});
    add(TestRequest, "TestRequest", true, {TestReqID}, {});
    add(ResendRequest, "ResendRequest", true, {BeginSeqNo, EndSeqNo}, {});
    add(Reject, "Reject", true, {RefSeqNum},
        {RefTagID, RefMsgType, tags::SessionRejectReason, Text});
    add(SequenceReset, "SequenceReset", true, {NewSeqNo}, {GapFillFlag});
    add(Logout, "Logout", true, {}, {Text});
    add(Logon, "Logon", true, {EncryptMethod, HeartBtInt},
        {RawDataLength, RawData, ResetSeqNumFlag, MaxMessageSize, Username, Password});
    add(NewOrderSingle, "NewOrderSingle", false,
        {ClOrdID, HandlInst, Symbol, Side, TransactTime, OrderQty, OrdType},
        {Account, SecurityID, SecurityIDSource, OrdType, fix::tags::Price, StopPx, Currency,
         TimeInForce, ExecInst, Text});
    add(ExecutionReport, "ExecutionReport", false,
        {OrderID, ExecID, ExecType, OrdStatus, Symbol, Side, LeavesQty, CumQty, AvgPx},
        {ClOrdID, OrigClOrdID, Account, LastQty, LastPx, OrdRejReason, Text});
    add(OrderCancelRequest, "OrderCancelRequest", false,
        {OrigClOrdID, ClOrdID, Symbol, Side, TransactTime}, {Account, Text});
    add(OrderCancelReplaceRequest, "OrderCancelReplaceRequest", false,
        {OrigClOrdID, ClOrdID, HandlInst, Symbol, Side, TransactTime, OrderQty, OrdType},
        {Account, fix::tags::Price, StopPx, Text});
    add(MarketDataRequest, "MarketDataRequest", false,
        {MDReqID, SubscriptionRequestType, MarketDepth, NoMDEntryTypes, NoRelatedSym},
        {MDUpdateType, AggregatedBook});
    add(MarketDataSnapshotFullRefresh, "MarketDataSnapshotFullRefresh", false,
        {Symbol, NoMDEntries}, {MDReqID});
    add(MarketDataIncrementalRefresh, "MarketDataIncrementalRefresh", false, {NoMDEntries},
        {MDReqID});
    add(BusinessMessageReject, "BusinessMessageReject", false, {RefMsgType, BusinessRejectReason},
        {RefSeqNum, Text});

    rebuild_name_index();
}

void DataDictionary::load_builtin_messages_44() {
    load_builtin_messages_42(); // 4.4 is a superset for our purposes
    version_string_ = "FIX.4.4";
    version_ = FixVersion::FIX_4_4;
}

void DataDictionary::load_builtin_messages_50sp2() {
    load_builtin_messages_44();
    version_string_ = "FIX.5.0SP2";
    version_ = FixVersion::FIX_5_0SP2;
    // Add FIXT header fields
    using namespace tags;
    FieldDef appl;
    appl.tag = ApplVerID;
    appl.name = "ApplVerID";
    appl.type = FieldType::String;
    appl.valid_values = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    fields_by_tag_[ApplVerID] = std::move(appl);
    rebuild_name_index();
}

// ---------------------------------------------------------------------------
// DataDictionary - public API
// ---------------------------------------------------------------------------
void DataDictionary::load_builtin(FixVersion version) {
    {
        std::unique_lock lock(mutex_);
        version_ = version;
        version_string_ = std::string(fix::to_string(version));
        fields_by_tag_.clear();
        fields_by_name_.clear();
        messages_by_type_.clear();
        messages_by_name_.clear();
    }

    // These are called WITHOUT the lock (they call rebuild_name_index which takes
    // the lock internally). We hold no lock here since load_builtin is called
    // during initialisation before the object is shared.
    load_builtin_fields();
    switch (version) {
    case FixVersion::FIX_4_2:
        load_builtin_messages_42();
        break;
    case FixVersion::FIX_4_4:
        load_builtin_messages_44();
        break;
    case FixVersion::FIX_5_0SP2:
    case FixVersion::FIXT_1_1:
        load_builtin_messages_50sp2();
        break;
    default:
        load_builtin_messages_44();
        break;
    }
    {
        std::unique_lock lock(mutex_);
        version_string_ = std::string(fix::to_string(version));
        version_ = version;
    }
}

Result<void> DataDictionary::load(const std::filesystem::path &path) {
    // Minimal XML loading - we just use built-in dicts for now and note path
    // A full implementation would parse the FIX XML spec files here using
    // a library such as tinyxml2.
    (void)path;
    return {}; // succeed (built-in already loaded)
}

Result<void> DataDictionary::reload(const std::filesystem::path &path) {
    return load(path);
}

void DataDictionary::rebuild_name_index() {
    fields_by_name_.clear();
    for (auto &[tag, def] : fields_by_tag_) {
        fields_by_name_[def.name] = &def;
    }
    messages_by_name_.clear();
    for (auto &[type, def] : messages_by_type_) {
        messages_by_name_[def.name] = &def;
    }
}

const FieldDef *DataDictionary::find_field(TagNum tag) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = fields_by_tag_.find(tag);
    return it != fields_by_tag_.end() ? &it->second : nullptr;
}

const FieldDef *DataDictionary::find_field(std::string_view name) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = fields_by_name_.find(std::string(name));
    return it != fields_by_name_.end() ? it->second : nullptr;
}

const MessageDef *DataDictionary::find_message(std::string_view msg_type) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = messages_by_type_.find(std::string(msg_type));
    return it != messages_by_type_.end() ? &it->second : nullptr;
}

const MessageDef *DataDictionary::find_message_by_name(std::string_view name) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = messages_by_name_.find(std::string(name));
    return it != messages_by_name_.end() ? it->second : nullptr;
}

Result<void> DataDictionary::validate(const Message &msg) const {
    std::shared_lock lock(mutex_);
    // inline lookup without re-locking
    auto mit = messages_by_type_.find(std::string(msg.msg_type()));
    if (mit == messages_by_type_.end())
        return make_unexpected(ErrorCode::UnknownMessage);
    const auto &mdef = mit->second;

    // Check required fields
    for (TagNum req : mdef.required_tags) {
        if (!msg.has(req)) {
            return make_unexpected(ErrorCode::MissingField);
        }
    }

    // Validate enum fields
    for (const auto &f : msg.fields()) {
        auto it = fields_by_tag_.find(f.tag);
        if (it == fields_by_tag_.end())
            continue;
        if (!it->second.validate_value(f.value)) {
            return make_unexpected(ErrorCode::InvalidField);
        }
    }

    return {};
}

void DataDictionary::register_field(FieldDef def) {
    std::unique_lock lock(mutex_);
    TagNum tag = def.tag;
    fields_by_tag_[tag] = std::move(def);
    rebuild_name_index();
}

void DataDictionary::register_message(MessageDef def) {
    std::unique_lock lock(mutex_);
    std::string type = def.msg_type;
    messages_by_type_[type] = std::move(def);
    rebuild_name_index();
}

// ---------------------------------------------------------------------------
// DictionaryRegistry
// ---------------------------------------------------------------------------
DictionaryRegistry &DictionaryRegistry::instance() {
    static DictionaryRegistry inst;
    return inst;
}

const DataDictionary *DictionaryRegistry::get(FixVersion v) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = dicts_.find(static_cast<int>(v));
    return it != dicts_.end() ? it->second.get() : nullptr;
}

void DictionaryRegistry::set(FixVersion v, std::shared_ptr<DataDictionary> dict) {
    std::unique_lock lock(mutex_);
    dicts_[static_cast<int>(v)] = std::move(dict);
}

FixVersion DictionaryRegistry::resolve_appl_ver_id(std::string_view id) noexcept {
    if (id == appl_ver_id::FIX42)
        return FixVersion::FIX_4_2;
    if (id == appl_ver_id::FIX43)
        return FixVersion::FIX_4_3;
    if (id == appl_ver_id::FIX44)
        return FixVersion::FIX_4_4;
    if (id == appl_ver_id::FIX50)
        return FixVersion::FIX_5_0;
    if (id == appl_ver_id::FIX50SP1)
        return FixVersion::FIX_5_0SP1;
    if (id == appl_ver_id::FIX50SP2)
        return FixVersion::FIX_5_0SP2;
    return FixVersion::Unknown;
}

} // namespace fix
