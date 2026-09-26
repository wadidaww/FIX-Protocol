// =============================================================================
// FIX Protocol Engine - StreamParser implementation
// =============================================================================
#include "fix/parser/parser.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace fix {

namespace {

// Standard-header tags that must appear at most once per message
// (FIX session Reject reason 13, TagAppearsMoreThanOnce). Tag 10 (CheckSum)
// is enforced structurally by framing (exactly one trailer at
// body_start+BodyLength); a "10=" inside the body is a misplaced trailer and
// rejects the frame instead.
//
// Coverage: the FIX 4.2 spec §4.1 "Standard Header" plus the header fields
// named in include/fix/core/constants.hpp:
//   * 97 PossResend (NOT 7 — 7 is BeginSeqNo, a ResendRequest *body* field)
//   * 142/143 Sender/TargetLocationID, 144/145 OnBehalfOf/DeliverToLocationID
//   * 212/618 XmlDataLen/XmlData, 347 MessageEncoding, 369 LastMsgSeqNumProcessed
//   * 627 NoHops (group count — only a *duplicated* count tag is flagged)
//   * 1128/1129 ApplVerID/CstmApplVerID (FIXT.1.1 header)
// 144/145 are not named in constants.hpp yet, so they use literal tag numbers
// (parser-local; the shared header is owned elsewhere).
constexpr TagNum kStandardHeaderTags[] = {
    tags::BeginString,            // 8
    tags::BodyLength,             // 9
    tags::MsgType,                // 35
    tags::SenderCompID,           // 49
    tags::SenderSubID,            // 50
    tags::SenderLocationID,       // 142
    tags::TargetCompID,           // 56
    tags::TargetSubID,            // 57
    tags::TargetLocationID,       // 143
    tags::OnBehalfOfCompID,       // 115
    144,                          // OnBehalfOfLocationID (FIX 4.4 header)
    tags::DeliverToCompID,        // 128
    145,                          // DeliverToLocationID (FIX 4.4 header)
    tags::SecureDataLen,          // 90
    tags::SecureData,             // 91
    tags::MsgSeqNum,              // 34
    tags::PossDupFlag,            // 43
    tags::PossResend,             // 97
    tags::OrigSendingTime,        // 122
    tags::SendingTime,            // 52
    tags::XmlDataLen,             // 212
    tags::XmlData,                // 618
    tags::MessageEncoding,        // 347
    tags::LastMsgSeqNumProcessed, // 369
    tags::NoHops,                 // 627
    tags::ApplVerID,              // 1128
    tags::CstmApplVerID,          // 1129
};
constexpr std::size_t kHeaderTagCount = sizeof(kStandardHeaderTags) / sizeof(TagNum);

constexpr bool is_standard_header_tag(TagNum tag) {
    for (const TagNum t : kStandardHeaderTags) {
        if (t == tag)
            return true;
    }
    return false;
}

// Duplicate-header tracking sized EXACTLY by the whitelist: only whitelist
// tags are ever recorded, each distinct tag at most once, so the array can
// never overflow and duplicate detection can never be silently dropped.
struct HeaderSeen {
    TagNum tags[kHeaderTagCount]{};
    std::size_t count = 0;

    // Returns true if `tag` was already present; records it otherwise.
    bool check_and_mark(TagNum tag) {
        for (std::size_t i = 0; i < count; ++i) {
            if (tags[i] == tag)
                return true;
        }
        if (count >= kHeaderTagCount) {
            // Unreachable by construction (see comment above). Fail loudly
            // rather than dropping the duplicate detection on the floor.
            std::abort();
        }
        tags[count++] = tag;
        return false;
    }
};

} // namespace

StreamParser::StreamParser(Options opts)
    : opts_(opts) {
    if (opts_.max_msg_len > 0) {
        // Backlog cap: one full-size frame plus slack for an in-flight frame
        // header/trailer and alignment.
        buf_cap_ = opts_.max_msg_len + kBacklogSlack;
    }
    buf_.reserve(65536);
}

void StreamParser::feed(std::span<const std::byte> data) {
    feed(reinterpret_cast<const char *>(data.data()), data.size());
}

void StreamParser::feed(const char *data, std::size_t len) {
    if (len == 0)
        return;
    if (!healthy_) {
        // Fatal condition already latched: drop input so memory stays
        // bounded. The session layer should observe healthy()==false,
        // disconnect and call reset().
        return;
    }

    while (len > 0) {
        // Parse everything already buffered. Error paths advance read_pos_
        // and CONTINUE (ET-safe: edge-triggered epoll may never re-fire), so
        // valid messages buffered behind a bad frame are still delivered.
        drain();
        compact();

        // How many bytes may be appended without exceeding the backlog cap?
        std::size_t space;
        if (buf_cap_ == 0) {
            space = len; // cap disabled (max_msg_len == 0)
        } else if (write_pos_ < buf_cap_) {
            space = buf_cap_ - write_pos_;
        } else {
            space = 0;
        }

        if (space == 0) {
            // Backlog at the cap: reclaim by re-aligning on a frame start;
            // if that cannot free room the condition is fatal.
            const std::size_t before = buffered();
            enforce_cap();
            if (!healthy_)
                return; // input (and any residue) dropped
            if (buffered() >= before && write_pos_ >= buf_cap_) {
                // Defensive: resync made no room — treat as fatal rather
                // than spin.
                healthy_ = false;
                if (!last_error_)
                    last_error_ = make_error_code(ErrorCode::ParseError);
                return;
            }
            continue;
        }

        const std::size_t n = std::min(len, space);
        append_bytes(data, n);
        data += n;
        len -= n;
    }

    drain();
}

bool StreamParser::next(Message &out) {
    if (pending_pos_ >= pending_.size()) {
        pending_.clear();
        pending_pos_ = 0;
        out = Message{}; // documented: `out` is cleared when returning false
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
    msg_count_ = 0;
    resync_count_ = 0;
    pending_.clear();
    pending_pos_ = 0;
    last_error_ = {};
    healthy_ = true;
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

void StreamParser::append_bytes(const char *data, std::size_t len) {
    if (len == 0)
        return;
    const std::size_t need = write_pos_ + len;
    if (need < write_pos_) {
        // size_t overflow — cannot happen with sane inputs, but never risk
        // a wrapped resize + out-of-bounds memcpy.
        healthy_ = false;
        last_error_ = make_error_code(ErrorCode::BadLength);
        return;
    }
    if (need > buf_.size()) {
        // reserve() first (no value-initialisation), then resize() only up
        // to the bytes we are about to overwrite with memcpy — the previous
        // code zero-filled 64 KB chunks it never wrote (quadratic waste).
        // Invariant: buf_.size() >= write_pos_, so the resize range
        // [old_size, need) is a subset of [write_pos_, need) = write target.
        if (need <= std::numeric_limits<std::size_t>::max() - 65536)
            buf_.reserve(need + 65536);
        else
            buf_.reserve(need);
        buf_.resize(need);
    }
    std::memcpy(buf_.data() + write_pos_, data, len);
    write_pos_ = need;
}

void StreamParser::drain() {
    while (try_parse_one() != ParseStatus::NeedMore) {
        // Message / Error both made progress; keep going without new input.
    }
}

std::size_t StreamParser::resync_scan(std::error_code err) {
    const std::size_t avail = write_pos_ - read_pos_;
    if (avail < 2)
        return 0;
    const char *base = buf_.data() + read_pos_;

    // Search window for the next frame start ("8="), bounded by max_msg_len.
    const std::size_t window = (opts_.max_msg_len > 0) ? std::min(avail, opts_.max_msg_len) : avail;

    std::size_t discard = 0;
    bool found = false;
    for (std::size_t i = 0; i + 1 < window; ++i) {
        if (base[i] == '8' && base[i + 1] == '=') {
            discard = i;
            found = true;
            break;
        }
    }
    if (!found) {
        // No frame start within the window: drop everything but a short tail
        // (it may end with a partial "8=" spanning the next feed).
        const std::size_t keep = std::min(avail, kKeepTail);
        if (avail <= keep)
            return 0; // nothing can be dropped — caller waits for more
        discard = avail - keep;
    } else if (discard == 0) {
        return 0; // already aligned on a frame start — no-op
    }

    read_pos_ += discard;
    consumed_ += discard;
    ++resync_count_;
    last_error_ = err;
    return discard;
}

StreamParser::ParseStatus StreamParser::skip_frame(std::size_t n, std::error_code err) {
    const std::size_t avail = write_pos_ - read_pos_;
    // Always advance ≥1 byte: guarantees forward progress on every error.
    n = std::min(std::max<std::size_t>(n, 1), avail);
    read_pos_ += n;
    consumed_ += n;
    last_error_ = err;
    // Re-align on the next frame start; keeps `err` as last_error_ (it
    // already carries the specific reason the frame was rejected).
    resync_scan(err);
    return ParseStatus::Error;
}

void StreamParser::enforce_cap() {
    if (buf_cap_ == 0 || write_pos_ < buf_cap_)
        return;
    const std::size_t before = buffered();
    // Discard garbage up to the next candidate frame start…
    resync_scan(make_error_code(ErrorCode::ParseError));
    compact();
    // …anything newly aligned parses out immediately…
    drain();
    compact();
    if (buffered() >= before) {
        // Could not reclaim space: an unparseable backlog above the cap.
        // Fatal — latch unhealthy_, free the backlog, drop further input.
        healthy_ = false;
        if (!last_error_)
            last_error_ = make_error_code(ErrorCode::ParseError);
        consumed_ += (write_pos_ - read_pos_);
        buf_.clear();
        read_pos_ = 0;
        write_pos_ = 0;
    }
}

// parse_field: parse one Tag=Value<SOH> field from [data, data+len).
StreamParser::FieldStatus StreamParser::parse_field(const char *data, std::size_t len, TagNum &tag,
                                                    std::string_view &value,
                                                    std::size_t &consumed) {
    // Find '=' separator (SOH before '=' = malformed field).
    std::size_t eq = 0;
    while (eq < len && data[eq] != '=' && data[eq] != SOH)
        ++eq;
    if (eq == len)
        return FieldStatus::Incomplete; // need more data
    if (data[eq] == SOH)
        return FieldStatus::Invalid; // SOH with no '=' (e.g. "abc\x01…")

    // Parse tag: strictly numeric, fully consumed, within TagNum range.
    if (eq == 0)
        return FieldStatus::Invalid; // empty tag ("=value")
    TagNum t{};
    const auto [tptr, tec] = std::from_chars(data, data + eq, t);
    if (tec != std::errc{} || tptr != data + eq)
        return FieldStatus::Invalid; // non-numeric / overflow ("abc=", "99999999999=")

    // Find value end (SOH).
    std::size_t soh = eq + 1;
    while (soh < len && data[soh] != SOH)
        ++soh;
    if (soh == len)
        return FieldStatus::Incomplete; // need more data

    tag = t;
    value = std::string_view(data + eq + 1, soh - eq - 1);
    consumed = soh + 1; // consumed up to and including SOH
    return FieldStatus::Ok;
}

std::uint8_t StreamParser::compute_checksum(const char *data, std::size_t len) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < len; ++i)
        sum += static_cast<unsigned char>(data[i]);
    return static_cast<std::uint8_t>(sum & 0xFF);
}

bool StreamParser::parse_checksum(std::string_view sv, std::uint32_t &out) noexcept {
    if (sv.size() != 3)
        return false; // strictly 3 digits ("10=0185" / "10=12" rejected)
    for (char c : sv) {
        if (c < '0' || c > '9')
            return false;
    }
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    if (ec != std::errc{} || ptr != sv.data() + sv.size())
        return false;
    if (out > 255)
        return false; // "10=441" rejected (no uint8 wraparound)
    return true;
}

StreamParser::ParseStatus StreamParser::try_parse_one() {
    const std::size_t avail = write_pos_ - read_pos_;
    if (avail == 0)
        return ParseStatus::NeedMore;
    const char *base = buf_.data() + read_pos_;

    // -- 1. Frame synchronisation --------------------------------------------
    if (avail < 2)
        return ParseStatus::NeedMore;
    if (base[0] != '8' || base[1] != '=') {
        // Garbage prefix: scan forward for the next "8=" and discard the
        // junk (C6: never stall on input that doesn't start a frame).
        const std::size_t dropped = resync_scan(make_error_code(ErrorCode::ParseError));
        return dropped > 0 ? ParseStatus::Error : ParseStatus::NeedMore;
    }
    if (avail < kMinMsgLen)
        return ParseStatus::NeedMore; // plausible partial frame

    std::size_t pos = 0; // offset of the current field within the frame
    TagNum tag = 0;
    std::string_view value;
    std::string_view begin_value; // tag-8 value; stable until buf_ compacts
    std::size_t field_len = 0;

    // -- 2. Header field 8 (BeginString) -------------------------------------
    {
        const std::size_t scan = std::min(avail, kMaxHeaderScan);
        const FieldStatus fs = parse_field(base, scan, tag, value, field_len);
        if (fs == FieldStatus::Incomplete) {
            if (avail < kMaxHeaderScan)
                return ParseStatus::NeedMore;
            // Header field never terminates within the scan bound → garbage.
            return skip_frame(1, make_error_code(ErrorCode::ParseError));
        }
        if (fs == FieldStatus::Invalid || tag != tags::BeginString)
            return skip_frame(1, make_error_code(ErrorCode::InvalidField));
        begin_value = value; // stable: buf_ does not reallocate during parse
        pos = field_len;
    }

    // -- 3. Header field 9 (BodyLength) --------------------------------------
    {
        const std::size_t scan = std::min(avail - pos, kMaxHeaderScan);
        const FieldStatus fs = parse_field(base + pos, scan, tag, value, field_len);
        if (fs == FieldStatus::Incomplete) {
            if (avail - pos < kMaxHeaderScan)
                return ParseStatus::NeedMore;
            return skip_frame(pos, make_error_code(ErrorCode::ParseError));
        }
        if (fs == FieldStatus::Invalid || tag != tags::BodyLength)
            return skip_frame(pos, make_error_code(ErrorCode::BadLength));
        pos += field_len;
    }

    // -- 4. BodyLength value: strict parse + overflow-safe frame bounds -----
    std::size_t body_length = 0;
    {
        if (value.empty())
            return skip_frame(pos, make_error_code(ErrorCode::BadLength));
        const auto [blptr, blec] =
            std::from_chars(value.data(), value.data() + value.size(), body_length);
        if (blec != std::errc{} || blptr != value.data() + value.size())
            return skip_frame(pos, make_error_code(ErrorCode::BadLength)); // "9=zzz"
    }

    const std::size_t body_start = pos;
    // frame_end = body_start + body_length + 7 ("8=…9=…\x01" + body + trailer)
    const bool frame_end_ok =
        body_length <= std::numeric_limits<std::size_t>::max() - (body_start + 7);
    const std::size_t frame_end =
        frame_end_ok ? body_start + body_length + 7 : std::numeric_limits<std::size_t>::max();

    // Oversized / overflowing BodyLength: reject the whole frame, not just
    // the header (the old code stranded the parser mid-body forever).
    bool oversized = !frame_end_ok;
    if (!oversized && opts_.max_msg_len > 0) {
        const std::size_t overhead = body_start + 7;
        oversized = overhead > opts_.max_msg_len || body_length > opts_.max_msg_len - overhead;
    }
    if (oversized) {
        last_error_ = make_error_code(ErrorCode::BadLength);
        if (frame_end <= buf_cap_ && avail < frame_end) {
            // Could still fit within the cap — wait for the rest of the
            // frame, then skip it wholesale. The backlog cap guarantees this
            // cannot wait forever: if the peer never delivers, the buffer
            // hits the cap and the parser turns unhealthy (session
            // disconnects).
            return ParseStatus::NeedMore;
        }
        if (avail >= frame_end && frame_end >= pos)
            return skip_frame(frame_end, make_error_code(ErrorCode::BadLength));
        // Frame can never fit within the cap: skip the header and resync
        // onto the next candidate frame inside the claimed body.
        return skip_frame(pos, make_error_code(ErrorCode::BadLength));
    }

    // -- 5. Full frame must be buffered before body parsing -------------------
    if (avail < frame_end)
        return ParseStatus::NeedMore; // genuine "need more data" (bounded by cap)

    const std::size_t body_end = body_start + body_length;

    // -- 6. Parse body fields, bounded by body_start + BodyLength ------------
    Message msg;
    HeaderSeen seen;
    bool duplicate_header = false;

    // Seed the message with the header fields (copied while their
    // string_views into buf_ are still valid — buf_ does not reallocate
    // during a parse step).
    if (!msg.add(tags::BeginString, begin_value).has_value())
        return skip_frame(pos, make_error_code(ErrorCode::InvalidField));
    if (!msg.add(tags::BodyLength, static_cast<std::int64_t>(body_length)).has_value())
        return skip_frame(pos, make_error_code(ErrorCode::InvalidField));
    (void)seen.check_and_mark(tags::BeginString);
    (void)seen.check_and_mark(tags::BodyLength);

    const std::error_code dup_err = make_error_code(ErrorCode::DuplicateField);
    while (pos < body_end) {
        const FieldStatus fs = parse_field(base + pos, body_end - pos, tag, value, field_len);
        if (fs == FieldStatus::Incomplete) {
            // All bytes up to body_end are available: an unterminated field
            // means the claimed body cuts mid-field (or lacks a SOH).
            return skip_frame(body_end, make_error_code(ErrorCode::BadLength));
        }
        if (fs == FieldStatus::Invalid) {
            // Non-numeric / overflowing tag ("abc=zz", tag > TagNum range).
            return skip_frame(pos, make_error_code(ErrorCode::InvalidField));
        }
        if (tag == tags::CheckSum) {
            // "10=" inside the body = misplaced trailer → invalid frame
            // (requirement: missing/misplaced 10= rejects the frame and the
            // following message is still parsed via resync).
            return skip_frame(pos, make_error_code(ErrorCode::ParseError));
        }
        if (is_standard_header_tag(tag) && seen.check_and_mark(tag)) {
            // Duplicate standard-header tag (Reject reason 13): keep the
            // FIRST occurrence (first-wins), flag the message, still deliver.
            duplicate_header = true;
            last_error_ = dup_err;
            pos += field_len;
            continue;
        }
        if (!msg.add(tag, value).has_value()) {
            // NUL byte (or other rejected wire char) inside the value.
            return skip_frame(pos, make_error_code(ErrorCode::InvalidField));
        }
        pos += field_len;
    }

    // -- 7. CheckSum trailer must sit EXACTLY at body_start + BodyLength -----
    {
        const FieldStatus fs = parse_field(base + pos, kTrailerBytes, tag, value, field_len);
        if (fs != FieldStatus::Ok || tag != tags::CheckSum) {
            // Missing / misplaced trailer → invalid frame; skip the claimed
            // body only (never eats into the next message) and resync so a
            // following valid message still parses.
            return skip_frame(body_end, make_error_code(ErrorCode::ParseError));
        }
        // Strict value check first: "10=441" (>255, would wrap in uint8_t)
        // and non-3-digit values are BadChecksum; a structurally short /
        // long trailer ("10=185\x01" with bytes missing) is ParseError.
        std::uint32_t provided = 0;
        if (!parse_checksum(value, provided))
            return skip_frame(body_end, make_error_code(ErrorCode::BadChecksum));
        if (field_len != kTrailerBytes)
            return skip_frame(body_end, make_error_code(ErrorCode::ParseError));
        pos += field_len; // pos == frame_end

        if (opts_.validate_checksum) {
            const std::uint8_t expected = compute_checksum(base, pos - kTrailerBytes);
            if (expected != static_cast<std::uint8_t>(provided))
                return skip_frame(pos, make_error_code(ErrorCode::BadChecksum));
        }
        if (opts_.validate_body_len) {
            // Redundant under structural framing (the trailer position above
            // IS the claim check) — kept as an explicit invariant.
            if (body_end - body_start != body_length)
                return skip_frame(pos, make_error_code(ErrorCode::BadLength));
        }
    }

    // -- 8. Success ----------------------------------------------------------
    msg.set_raw(std::string(base, pos));
    if (duplicate_header)
        msg.mark_duplicate_header();
    pending_.push_back(std::move(msg));
    ++msg_count_;
    last_error_ = {}; // successful parse clears the error (unless flagged)
    if (duplicate_header)
        last_error_ = dup_err;
    read_pos_ += pos;
    consumed_ += pos;
    return ParseStatus::Message;
}

} // namespace fix
