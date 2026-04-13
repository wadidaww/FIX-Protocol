#pragma once
// =============================================================================
// FIX Protocol Engine - Tag Constants
// =============================================================================
#include <cstdint>
#include <string_view>

namespace fix {

// ---------------------------------------------------------------------------
// Standard FIX tag numbers
// ---------------------------------------------------------------------------
namespace tags {

// --- Standard Header Fields ---
inline constexpr std::uint32_t BeginString   =   8;
inline constexpr std::uint32_t BodyLength    =   9;
inline constexpr std::uint32_t MsgType       =  35;
inline constexpr std::uint32_t SenderCompID  =  49;
inline constexpr std::uint32_t TargetCompID  =  56;
inline constexpr std::uint32_t MsgSeqNum     =  34;
inline constexpr std::uint32_t SendingTime   =  52;
inline constexpr std::uint32_t OrigSendingTime = 122;
inline constexpr std::uint32_t PossDupFlag   =  43;
inline constexpr std::uint32_t PossResend    =  97;
inline constexpr std::uint32_t OnBehalfOfCompID = 115;
inline constexpr std::uint32_t DeliverToCompID  = 128;
inline constexpr std::uint32_t SecureDataLen    = 90;
inline constexpr std::uint32_t SecureData       = 91;
inline constexpr std::uint32_t SenderSubID      = 50;
inline constexpr std::uint32_t SenderLocationID = 142;
inline constexpr std::uint32_t TargetSubID      = 57;
inline constexpr std::uint32_t TargetLocationID = 143;
inline constexpr std::uint32_t XmlData          = 618;
inline constexpr std::uint32_t XmlDataLen       = 212;
inline constexpr std::uint32_t MessageEncoding  = 347;
inline constexpr std::uint32_t LastMsgSeqNumProcessed = 369;
inline constexpr std::uint32_t NoHops           = 627;
inline constexpr std::uint32_t ApplVerID        = 1128;
inline constexpr std::uint32_t CstmApplVerID    = 1129;

// --- Standard Trailer Fields ---
inline constexpr std::uint32_t SignatureLength  = 93;
inline constexpr std::uint32_t Signature        = 89;
inline constexpr std::uint32_t CheckSum         = 10;

// --- Session-level ---
inline constexpr std::uint32_t EncryptMethod    = 98;
inline constexpr std::uint32_t HeartBtInt       = 108;
inline constexpr std::uint32_t RawData          = 96;
inline constexpr std::uint32_t RawDataLength    = 95;
inline constexpr std::uint32_t ResetSeqNumFlag  = 141;
inline constexpr std::uint32_t NextExpectedMsgSeqNum = 789;
inline constexpr std::uint32_t TestReqID        = 112;
inline constexpr std::uint32_t GapFillFlag      = 123;
inline constexpr std::uint32_t NewSeqNo         = 36;
inline constexpr std::uint32_t BeginSeqNo       = 7;
inline constexpr std::uint32_t EndSeqNo         = 16;
inline constexpr std::uint32_t RefSeqNum        = 45;
inline constexpr std::uint32_t RefMsgType       = 372;
inline constexpr std::uint32_t SessionRejectReason = 373;
inline constexpr std::uint32_t Text             = 58;
inline constexpr std::uint32_t RefTagID         = 371;
inline constexpr std::uint32_t Username         = 553;
inline constexpr std::uint32_t Password         = 554;
inline constexpr std::uint32_t DefaultApplVerID = 1137;
inline constexpr std::uint32_t MaxMessageSize   = 383;

// --- Order fields ---
inline constexpr std::uint32_t ClOrdID          = 11;
inline constexpr std::uint32_t OrigClOrdID      = 41;
inline constexpr std::uint32_t OrderID          = 37;
inline constexpr std::uint32_t ExecID           = 17;
inline constexpr std::uint32_t ExecType         = 150;
inline constexpr std::uint32_t OrdStatus        = 39;
inline constexpr std::uint32_t Symbol           = 55;
inline constexpr std::uint32_t Side             = 54;
inline constexpr std::uint32_t OrderQty         = 38;
inline constexpr std::uint32_t OrdType          = 40;
inline constexpr std::uint32_t Price            = 44;
inline constexpr std::uint32_t StopPx           = 99;
inline constexpr std::uint32_t TimeInForce      = 59;
inline constexpr std::uint32_t TransactTime     = 60;
inline constexpr std::uint32_t LeavesQty        = 151;
inline constexpr std::uint32_t CumQty           = 14;
inline constexpr std::uint32_t AvgPx            = 6;
inline constexpr std::uint32_t LastQty          = 32;
inline constexpr std::uint32_t LastPx           = 31;
inline constexpr std::uint32_t OrdRejReason     = 103;
inline constexpr std::uint32_t CxlRejReason     = 102;
inline constexpr std::uint32_t CxlRejResponseTo = 434;
inline constexpr std::uint32_t ExecInst         = 18;
inline constexpr std::uint32_t HandlInst        = 21;
inline constexpr std::uint32_t MinQty           = 110;
inline constexpr std::uint32_t MaxFloor         = 111;
inline constexpr std::uint32_t SecondaryClOrdID = 526;
inline constexpr std::uint32_t Account          = 1;
inline constexpr std::uint32_t SecurityID       = 48;
inline constexpr std::uint32_t SecurityIDSource = 22;
inline constexpr std::uint32_t SecurityType     = 167;
inline constexpr std::uint32_t MaturityDate     = 541;
inline constexpr std::uint32_t PutOrCall        = 201;
inline constexpr std::uint32_t StrikePrice      = 202;
inline constexpr std::uint32_t Currency         = 15;
inline constexpr std::uint32_t IDSource         = 22;
inline constexpr std::uint32_t BusinessRejectReason = 380;

// --- Market data ---
inline constexpr std::uint32_t MDReqID          = 262;
inline constexpr std::uint32_t SubscriptionRequestType = 263;
inline constexpr std::uint32_t MarketDepth      = 264;
inline constexpr std::uint32_t MDUpdateType     = 265;
inline constexpr std::uint32_t AggregatedBook   = 266;
inline constexpr std::uint32_t NoMDEntryTypes   = 267;
inline constexpr std::uint32_t NoRelatedSym     = 146;
inline constexpr std::uint32_t MDEntryType      = 269;
inline constexpr std::uint32_t MDEntryPx        = 270;
inline constexpr std::uint32_t MDEntrySize      = 271;
inline constexpr std::uint32_t MDEntryDate      = 272;
inline constexpr std::uint32_t MDEntryTime      = 273;
inline constexpr std::uint32_t MDEntryID        = 278;
inline constexpr std::uint32_t MDUpdateAction   = 279;
inline constexpr std::uint32_t NoMDEntries      = 268;
inline constexpr std::uint32_t MDReqRejReason   = 281;

} // namespace tags

// ---------------------------------------------------------------------------
// Message type strings (tag 35 values)
// ---------------------------------------------------------------------------
namespace msg_types {
    inline constexpr std::string_view Heartbeat              = "0";
    inline constexpr std::string_view TestRequest            = "1";
    inline constexpr std::string_view ResendRequest          = "2";
    inline constexpr std::string_view Reject                 = "3";
    inline constexpr std::string_view SequenceReset          = "4";
    inline constexpr std::string_view Logout                 = "5";
    inline constexpr std::string_view IOI                    = "6";
    inline constexpr std::string_view Advertisement         = "7";
    inline constexpr std::string_view ExecutionReport        = "8";
    inline constexpr std::string_view OrderCancelReject      = "9";
    inline constexpr std::string_view Logon                  = "A";
    inline constexpr std::string_view News                   = "B";
    inline constexpr std::string_view Email                  = "C";
    inline constexpr std::string_view NewOrderSingle         = "D";
    inline constexpr std::string_view NewOrderList           = "E";
    inline constexpr std::string_view OrderCancelRequest     = "F";
    inline constexpr std::string_view OrderCancelReplaceRequest = "G";
    inline constexpr std::string_view OrderStatusRequest     = "H";
    inline constexpr std::string_view BusinessMessageReject  = "j";
    inline constexpr std::string_view MarketDataRequest      = "V";
    inline constexpr std::string_view MarketDataSnapshotFullRefresh   = "W";
    inline constexpr std::string_view MarketDataIncrementalRefresh    = "X";
    inline constexpr std::string_view MarketDataRequestReject         = "Y";
    inline constexpr std::string_view SecurityDefinitionRequest       = "c";
    inline constexpr std::string_view SecurityDefinition              = "d";
    inline constexpr std::string_view TradingSessionStatusRequest     = "g";
    inline constexpr std::string_view TradingSessionStatus            = "h";
    inline constexpr std::string_view MassQuote                       = "i";
    inline constexpr std::string_view Quote                           = "S";
    inline constexpr std::string_view QuoteRequest                    = "R";
} // namespace msg_types

// ---------------------------------------------------------------------------
// ApplVerID values (tag 1128)
// ---------------------------------------------------------------------------
namespace appl_ver_id {
    inline constexpr std::string_view FIX27    = "0";
    inline constexpr std::string_view FIX30    = "1";
    inline constexpr std::string_view FIX40    = "2";
    inline constexpr std::string_view FIX41    = "3";
    inline constexpr std::string_view FIX42    = "4";
    inline constexpr std::string_view FIX43    = "5";
    inline constexpr std::string_view FIX44    = "6";
    inline constexpr std::string_view FIX50    = "7";
    inline constexpr std::string_view FIX50SP1 = "8";
    inline constexpr std::string_view FIX50SP2 = "9";
} // namespace appl_ver_id

// ---------------------------------------------------------------------------
// Session reject reason codes (tag 373)
// ---------------------------------------------------------------------------
enum class SessionRejectReason : int {
    InvalidTagNumber            = 0,
    RequiredTagMissing          = 1,
    TagNotDefinedForMsgType     = 2,
    UndefinedTag                = 3,
    TagSpecWithoutValue         = 4,
    ValueIncorrect              = 5,
    IncorrectDataFormat         = 6,
    DecryptionProblem           = 7,
    SignatureProblem            = 8,
    CompIDProblem               = 9,
    SendingTimeAccuracyProblem  = 10,
    InvalidMsgType              = 11,
    XMLValidationError          = 12,
    TagAppearsMoreThanOnce      = 13,
    TagOutOfOrder               = 14,
    RepeatingGroupCountMismatch = 15,
    NonDataValueIncludesFieldDelimiter = 16,
    InvalidUnsupportedApplicationVersion = 17,
    Other                       = 99,
};

} // namespace fix
