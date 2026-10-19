// The decode trace: the contract between the C++ engine and the RTL model.
//
// This file is the FORMAT, and nothing else. It deliberately does not
// include the ITCH decoder, because the whole point of the exercise is
// that two implementations written from the same published specification
// must agree -- and a format that both sides share as code is a format
// that cannot catch either side being wrong about the protocol.
//
// The format is therefore specified in prose in `rtl/TRACE_FORMAT.md`,
// and this header is one implementation of it. The RTL side implements it
// a second time, in VHDL, from the prose.
//
// ---- What a trace record is -----------------------------------------
//
// One record per frame in the capture, recording what the decoder
// decided about that frame and, when it decoded one, the value of every
// field it extracted.
//
// The field values are the point. Two implementations that both read the
// same bytes can still disagree about what those bytes MEAN -- reading
// the share count as the price, which is the bug this repository shipped
// for most of its life. A trace that recorded the raw body bytes would
// agree no matter how wrong either side was, because both would be
// echoing the same buffer. A trace that records extracted values
// disagrees the moment one of them extracts the wrong thing.
//
// ---- What a trace record is not --------------------------------------
//
// It is not a checksum. A checksum says "these two states are equal"; a
// trace says "these two states are equal, and if they are not, here is
// the record and the field that differ". The difference matters when the
// answer is no.
//
// ---- Positional comparison ------------------------------------------
//
// Records are compared POSITIONALLY: field 0 against field 0, field 1
// against field 1, and so on. The field NAMES in this header are
// diagnostics for a human reading a failure, not part of the format and
// not part of the comparison. Two implementations that disagree about
// which field is which will be caught by the values disagreeing -- and
// the names are only there so the report says "price" rather than
// "field 7".

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iosfwd>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace hft::rtl {

/// What the decoder decided about one frame.
enum class TraceStatus : char {
    /// The frame decoded. Only this status carries field values.
    decoded = 'D',
    /// A well-formed frame of a type this build does not decode. The
    /// length is known, so the frame can be skipped by length.
    skipped = 'S',
    /// The declared length disagrees with the length the type requires.
    bad_length = 'L',
    /// A field held a value outside its defined range.
    malformed = 'M',
    /// Fewer bytes were available than the length prefix promised.
    truncated = 'T',
};

/// One field of a decoded message: its name, and its width on the wire.
///
/// The widths are the wire widths, so a 4-byte price is 8 hex
/// characters and an 8-byte order reference is 16. The trace is an image
/// of the wire format, not a rendering of the decoder's internal types.
struct FieldSpec {
    const char* name;
    int bytes;
};

inline constexpr FieldSpec kFieldsAddOrder[] = {
    {"locate", 2},  {"tracking", 2}, {"timestamp", 6}, {"id", 8},
    {"side", 1},    {"shares", 4},    {"stock", 8},     {"price", 4},
};

inline constexpr FieldSpec kFieldsExecuted[] = {
    {"locate", 2}, {"tracking", 2}, {"timestamp", 6}, {"id", 8},
    {"shares", 4}, {"match", 8},    {"printable", 1},
};

inline constexpr FieldSpec kFieldsExecutedAtPrice[] = {
    {"locate", 2},          {"tracking", 2}, {"timestamp", 6}, {"id", 8},
    {"shares", 4},          {"match", 8},    {"printable", 1}, {"execution_price", 4},
};

inline constexpr FieldSpec kFieldsCancel[] = {
    {"locate", 2}, {"tracking", 2}, {"timestamp", 6}, {"id", 8}, {"shares", 4},
};

inline constexpr FieldSpec kFieldsDelete[] = {
    {"locate", 2}, {"tracking", 2}, {"timestamp", 6}, {"id", 8},
};

inline constexpr FieldSpec kFieldsReplace[] = {
    {"locate", 2},   {"tracking", 2}, {"timestamp", 6}, {"original_id", 8},
    {"new_id", 8},   {"shares", 4},    {"price", 4},
};

inline constexpr FieldSpec kFieldsBrokenTrade[] = {
    {"locate", 2}, {"tracking", 2}, {"timestamp", 6}, {"match", 8},
};

inline constexpr FieldSpec kFieldsTrade[] = {
    {"locate", 2}, {"tracking", 2}, {"timestamp", 6}, {"id", 8},
    {"side", 1},   {"shares", 4},    {"price", 4},     {"match", 8},
};

// The Trade and Cross Trade field lists have NO stock symbol, and that
// is not an oversight. The C++ decoder reads the Trade share count at
// offset 20 and the price at offset 32, stepping over the eight-byte
// stock symbol at offset 24 on the way, and it does not carry that
// symbol anywhere. The Cross Trade price at offset 27 is reached the
// same way, over the symbol at offset 19. The trace records what the
// decoder produces, so neither symbol is in it.
//
// The step is still covered: the RTL side must reach the price at 32 (or
// 27), and an implementation that read the price at 24 (or 19) would put
// the symbol bytes in the trace where the price belongs, and the
// comparison would fail. What is not covered is an implementation that
// skipped the wrong number of bytes AND happened to produce the same
// price -- which is not a thing a decoder can do.
inline constexpr FieldSpec kFieldsCrossTrade[] = {
    {"locate", 2}, {"tracking", 2}, {"timestamp", 6}, {"shares", 8},
    {"price", 4},  {"match", 8},    {"cross_type", 1},
};

/// The field list for a message type, in wire order.
///
/// Returns null and sets `count` to zero for a type this build does not
/// decode. The type is the raw message tag byte, not a `MessageType`
/// enum, because this header does not depend on the decoder.
inline const FieldSpec* field_specs(std::uint8_t type, std::size_t& count) noexcept {
    switch (type) {
        case 'A': count = std::size(kFieldsAddOrder); return kFieldsAddOrder;
        case 'E': count = std::size(kFieldsExecuted); return kFieldsExecuted;
        case 'C': count = std::size(kFieldsExecutedAtPrice); return kFieldsExecutedAtPrice;
        case 'X': count = std::size(kFieldsCancel); return kFieldsCancel;
        case 'D': count = std::size(kFieldsDelete); return kFieldsDelete;
        case 'U': count = std::size(kFieldsReplace); return kFieldsReplace;
        case 'B': count = std::size(kFieldsBrokenTrade); return kFieldsBrokenTrade;
        case 'P': count = std::size(kFieldsTrade); return kFieldsTrade;
        case 'Q': count = std::size(kFieldsCrossTrade); return kFieldsCrossTrade;
        default: count = 0; return nullptr;
    }
}

/// One frame, as the decoder saw it.
struct TraceRecord {
    /// Position of the frame in the capture, from zero.
    std::size_t index = 0;
    TraceStatus status = TraceStatus::decoded;
    /// The message tag byte, when the decoder reached it.
    std::uint8_t type = 0;
    /// False when the decoder stopped before the tag was readable.
    has_type = false;
    /// Declared body length, excluding the two-byte length prefix.
    std::uint16_t length = 0;
    /// Field values, fixed-width lowercase hex, in wire order. Empty
    /// for every status other than `decoded`.
    std::vector<std::string> fields;
};

/// One difference between two traces.
struct TraceDifference {
    std::size_t index = 0;
    /// What differed: "status", "type", "length", "field <name>", or
    /// "record count".
    std::string what;
    std::string expected;
    std::string actual;
};

/// Lowercase hex, zero-padded to `bytes` bytes, most significant first.
///
/// `bytes` is the WIDTH ON THE WIRE, so the value is rendered exactly as
/// the bytes appear in the frame.
[[nodiscard]] inline std::string hex_u64(std::uint64_t value, int bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(static_cast<std::size_t>(bytes) * 2U);
    for (int i = bytes - 1; i >= 0; --i) {
        const auto byte = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
        const std::size_t at = static_cast<std::size_t>(i) * 2U;
        out[at] = kDigits[byte >> 4U];
        out[at + 1U] = kDigits[byte & 0x0FU];
    }
    return out;
}

/// Lowercase hex of raw bytes.
[[nodiscard]] inline std::string hex_bytes(const char* data, std::size_t count) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2U);
    for (std::size_t i = 0; i < count; ++i) {
        const auto byte = static_cast<std::uint8_t>(static_cast<unsigned char>(data[i]));
        out.push_back(kDigits[byte >> 4U]);
        out.push_back(kDigits[byte & 0x0FU]);
    }
    return out;
}

/// The two-hex-character rendering of a tag byte, or "--" when the
/// decoder never reached it.
[[nodiscard]] inline std::string type_token(const TraceRecord& record) {
    if (!record.has_type) {
        return "--";
    }
    return hex_u64(record.type, 1);
}

/// Render one record as a line, without the newline.
[[nodiscard]] inline std::string format_record(const TraceRecord& record) {
    std::string out;
    out.reserve(48U);
    out.append(std::to_string(record.index));
    out.push_back(' ');
    out.push_back(static_cast<char>(record.status));
    out.push_back(' ');
    out.append(type_token(record));
    out.push_back(' ');
    out.append(std::to_string(record.length));
    for (const std::string& field : record.fields) {
        out.push_back(' ');
        out.append(field);
    }
    return out;
}

/// The trace file header. A comment, so a reader can skip it and a
/// human can recognise the file.
inline constexpr const char* kTraceHeader = "# hft-rtl-trace v1";

/// Write a record to a stream. Returns false on a write error.
inline bool write_record(std::ostream& out, const TraceRecord& record) {
    out << format_record(record) << '\n';
    return static_cast<bool>(out);
}

/// Read one record from a stream.
///
/// Blank lines and lines beginning with '#' are skipped, so a trace can
/// carry comments. Returns false at end of input, or on a malformed
/// line, with `error` set.
inline bool read_record(std::istream& in, TraceRecord& out, std::string& error) {
    error.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        break;
    }
    if (line.empty()) {
        return false;
    }

    std::vector<std::string> tokens;
    std::string current;
    for (const char c : line) {
        if (c == ' ') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }

    if (tokens.size() < 4U) {
        error = "record has fewer than four tokens: " + line;
        return false;
    }

    TraceRecord record;
    try {
        record.index = static_cast<std::size_t>(std::stoull(tokens[0]));
    } catch (const std::exception&) {
        error = "record index is not a number: " + tokens[0];
        return false;
    }

    if (tokens[1].size() != 1U) {
        error = "record status is not a single character: " + tokens[1];
        return false;
    }
    const char status = tokens[1][0];
    switch (status) {
        case 'D': record.status = TraceStatus::decoded; break;
        case 'S': record.status = TraceStatus::skipped; break;
        case 'L': record.status = TraceStatus::bad_length; break;
        case 'M': record.status = TraceStatus::malformed; break;
        case 'T': record.status = TraceStatus::truncated; break;
        default:
            error = std::string("unknown record status: ") + tokens[1];
            return false;
    }

    if (tokens[2] == "--") {
        record.has_type = false;
    } else {
        if (tokens[2].size() != 2U) {
            error = "record type is not two hex characters: " + tokens[2];
            return false;
        }
        record.type = static_cast<std::uint8_t>(std::stoul(tokens[2], nullptr, 16));
        record.has_type = true;
    }

    try {
        record.length = static_cast<std::uint16_t>(std::stoul(tokens[3]));
    } catch (const std::exception&) {
        error = "record length is not a number: " + tokens[3];
        return false;
    }

    for (std::size_t i = 4U; i < tokens.size(); ++i) {
        record.fields.push_back(tokens[i]);
    }

    out = std::move(record);
    return true;
}

/// Check a record against the field specification for its type.
///
/// Catches the two ways a record can be wrong in a way a reader would
/// not notice: a field count that does not match the type, and a field
/// whose width is not the width the type declares. The second is the
/// one that matters -- a 4-byte price rendered as 16 hex characters
/// would still compare equal to another implementation that made the
/// same mistake, and the trace would be quietly describing a different
/// layout from the one on the wire.
[[nodiscard]] inline bool validate_record(const TraceRecord& record, std::string& error) {
    if (record.status != TraceStatus::decoded) {
        if (!record.fields.empty()) {
            error = "a non-decoded record carries field values";
            return false;
        }
        return true;
    }
    if (!record.has_type) {
        error = "a decoded record has no type";
        return false;
    }
    std::size_t expected_count = 0;
    const FieldSpec* specs = field_specs(record.type, expected_count);
    if (specs == nullptr) {
        error = "a decoded record has a type this build does not decode";
        return false;
    }
    if (record.fields.size() != expected_count) {
        error = "record has " + std::to_string(record.fields.size()) + " fields, the type has " +
                std::to_string(expected_count);
        return false;
    }
    for (std::size_t i = 0; i < expected_count; ++i) {
        const std::size_t want = static_cast<std::size_t>(specs[i].bytes) * 2U;
        if (record.fields[i].size() != want) {
            error = std::string("field ") + specs[i].name + " is " +
                    std::to_string(record.fields[i].size()) + " characters, the wire width is " +
                    std::to_string(want);
            return false;
        }
    }
    return true;
}

/// Compare two traces positionally.
///
/// Fills `differences` with up to `max_differences` entries and returns
/// true when the traces are identical. The record COUNT is compared
/// first: two traces of different lengths cannot be compared field by
/// field, and saying so is more useful than reporting every record past
/// the shorter one as missing.
inline bool compare_traces(const std::vector<TraceRecord>& expected,
                            const std::vector<TraceRecord>& actual,
                            std::vector<TraceDifference>& differences,
                            std::size_t max_differences) {
    differences.clear();
    if (expected.size() != actual.size()) {
        TraceDifference d;
        d.index = expected.size() < actual.size() ? expected.size() : actual.size();
        d.what = "record count";
        d.expected = std::to_string(expected.size());
        d.actual = std::to_string(actual.size());
        differences.push_back(d);
        return false;
    }

    bool identical = true;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const TraceRecord& e = expected[i];
        const TraceRecord& a = actual[i];

        if (e.index != a.index) {
            identical = false;
            if (differences.size() < max_differences) {
                differences.push_back({i, "index", std::to_string(e.index), std::to_string(a.index)});
            }
            continue;
        }
        if (e.status != a.status) {
            identical = false;
            if (differences.size() < max_differences) {
                differences.push_back({i, "status", std::string(1, static_cast<char>(e.status)),
                                       std::string(1, static_cast<char>(a.status))});
            }
            continue;
        }
        if (e.has_type != a.has_type || (e.has_type && e.type != a.type)) {
            identical = false;
            if (differences.size() < max_differences) {
                differences.push_back({i, "type", type_token(e), type_token(a)});
            }
            continue;
        }
        if (e.length != a.length) {
            identical = false;
            if (differences.size() < max_differences) {
                differences.push_back({i, "length", std::to_string(e.length),
                                       std::to_string(a.length)});
            }
            continue;
        }
        if (e.fields.size() != a.fields.size()) {
            identical = false;
            if (differences.size() < max_differences) {
                differences.push_back({i, "field count", std::to_string(e.fields.size()),
                                       std::to_string(a.fields.size())});
            }
            continue;
        }
        for (std::size_t f = 0; f < e.fields.size(); ++f) {
            if (e.fields[f] == a.fields[f]) {
                continue;
            }
            identical = false;
            if (differences.size() >= max_differences) {
                continue;
            }
            std::string what = "field";
            std::size_t expected_count = 0;
            const FieldSpec* specs = field_specs(e.type, expected_count);
            if (specs != nullptr && f < expected_count) {
                what = std::string("field ") + specs[f].name;
            } else {
                what += " " + std::to_string(f);
            }
            differences.push_back({i, what, e.fields[f], a.fields[f]});
        }
    }
    return identical;
}

}  // namespace hft::rtl
