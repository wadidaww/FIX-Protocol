// =============================================================================
// FIX Protocol Engine - MessageBuilder / Serializer implementation
// =============================================================================
#include "fix/parser/serializer.hpp"

#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fix {

std::uint8_t MessageBuilder::compute_checksum(const char *data, std::size_t len) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < len; ++i)
        sum += static_cast<unsigned char>(data[i]);
    return static_cast<std::uint8_t>(sum & 0xFF);
}

std::string MessageBuilder::build(std::string_view begin_string, std::string_view body) {
    // Header: "8=FIX.4.2\x01 9=<len>\x01"
    // Body:   already built (does NOT include tag 8/9)
    // Trailer: "10=<checksum>\x01"

    std::string header;
    header.reserve(32);
    header += "8=";
    header.append(begin_string.data(), begin_string.size());
    header += SOH;

    // body_length = length of everything after "9=NNN\x01" up to and including
    // everything before "10=\x01".  Per FIX spec: from start of MsgType field
    // through the delimiter of the last field before Checksum.
    std::string body_str(body);

    // Compute BodyLength field string to include in length calculation
    std::string bl_str = "9=";
    bl_str += std::to_string(body_str.size());
    bl_str += SOH;

    // Checksum covers header + bl_str + body
    std::string pre_cs = header + bl_str + body_str;

    std::uint8_t cs = compute_checksum(pre_cs.data(), pre_cs.size());

    char cs_buf[16];
    std::snprintf(cs_buf, sizeof(cs_buf), "%03u", static_cast<unsigned>(cs));

    std::string result;
    result.reserve(pre_cs.size() + 8);
    result += pre_cs;
    result += "10=";
    result += cs_buf;
    result += SOH;
    return result;
}

std::string MessageBuilder::serialize(const Message &msg, std::string_view begin_string,
                                      SeqNum seq_num, std::string_view sender,
                                      std::string_view target, std::string_view sending_time) {
    begin(begin_string, msg.msg_type());
    // Standard header fields (after 8, 9, 35)
    add(tags::SenderCompID, sender);
    add(tags::TargetCompID, target);
    add(tags::MsgSeqNum, static_cast<std::int64_t>(seq_num));
    add(tags::SendingTime, sending_time);

    // Body fields (skip header/trailer tags we already handle)
    for (const auto &f : msg.fields()) {
        if (f.tag == tags::BeginString || f.tag == tags::BodyLength || f.tag == tags::MsgType ||
            f.tag == tags::SenderCompID || f.tag == tags::TargetCompID ||
            f.tag == tags::MsgSeqNum || f.tag == tags::SendingTime || f.tag == tags::CheckSum) {
            continue;
        }
        add(f.tag, f.value);
    }
    return finish();
}

std::string MessageBuilder::format_timestamp(TimePoint tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03d", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

} // namespace fix
