// The C++ side of the decode trace: a decoded ITCH message, rendered as
// a trace record.
//
// This is the engine's implementation of the format specified in
// `rtl/TRACE_FORMAT.md`. The RTL side implements the same format a
// second time, from the prose, in VHDL. Nothing is shared between the
// two except the specification -- which is the point, because a shared
// header would let a mistake in one hide inside the other.
//
// The fields are rendered at their WIRE WIDTHS, so the trace is an image
// of the protocol rather than of the decoder's internal types. A price
// is four bytes on the wire and is rendered as eight hex characters, not
// as the sixteen a 64-bit fixed-point type would suggest.
//
// Every value is range-checked against the width it is rendered at.
// That check cannot fire with the current decoder, which reads a 4-byte
// field into a 64-bit type and therefore always fits -- and it is here
// because the failure it guards against is the worst one in this
// repository's history: a value rendered at the wrong width, or
// truncated to fit, would make two implementations agree while both
// described a different protocol.

#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "hft/itch/decode.hpp"
#include "hft/types.hpp"
#include "rtl/trace.hpp"

namespace hft::rtl {

/// The side byte as it appears on the wire: 'B' or 'S'.
[[nodiscard]] inline std::uint8_t side_byte(hft::Side side) noexcept {
    return side == hft::Side::bid ? static_cast<std::uint8_t>('B')
                                   : static_cast<std::uint8_t>('S');
}

/// True when a value fits in the given number of wire bytes.
[[nodiscard]] inline bool fits_wire(std::uint64_t value, int bytes) noexcept {
    if (bytes >= 8) {
        return true;
    }
    return value < (1ULL << (static_cast<unsigned>(bytes) * 8U));
}

/// Appends field values to a record, checking each against its width.
class FieldBuilder final {
public:
    FieldBuilder(std::vector<std::string>& sink, std::string& error) noexcept
        : sink_(sink), error_(error) {}

    /// Append an unsigned value rendered at `bytes` wire width.
    void add_u64(std::uint64_t value, int bytes, const char* name) {
        if (!fits_wire(value, bytes)) {
            fail(name, "value does not fit the field's wire width");
            return;
        }
        sink_.push_back(hex_u64(value, bytes));
    }

    /// Append a signed value, which must be non-negative to be a valid
    /// wire value. A negative price is not a price.
    void add_i64(std::int64_t value, int bytes, const char* name) {
        if (value < 0) {
            fail(name, "value is negative");
            return;
        }
        add_u64(static_cast<std::uint64_t>(value), bytes, name);
    }

    /// Append raw bytes.
    void add_bytes(const char* data, std::size_t count, const char* name) {
        sink_.push_back(hex_bytes(data, count));
        (void)name;
    }

    [[nodiscard]] bool ok() const noexcept { return error_.empty(); }

private:
    void fail(const char* name, const char* why) {
        if (error_.empty()) {
            error_ = std::string(name) + ": " + why;
        }
    }

    std::vector<std::string>& sink_;
    std::string& error_;
};

/// Render a decoded message's fields, in wire order.
[[nodiscard]] inline bool fields_of(const hft::itch::Message& message,
                                     std::vector<std::string>& fields,
                                     std::string& error) {
    fields.clear();
    FieldBuilder builder(fields, error);

    std::visit(
        [&builder](const auto& payload) noexcept {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, hft::itch::AddOrder>) {
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.id, 8, "id");
                builder.add_u64(side_byte(payload.side), 1, "side");
                builder.add_u64(payload.size.raw(), 4, "shares");
                builder.add_bytes(payload.stock, hft::itch::off::kStockSymbolSize, "stock");
                builder.add_i64(payload.price.raw(), 4, "price");
            } else if constexpr (std::is_same_v<T, hft::itch::OrderExecuted>) {
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.id, 8, "id");
                builder.add_u64(payload.shares.raw(), 4, "shares");
                builder.add_u64(payload.match_number, 8, "match");
            } else if constexpr (std::is_same_v<T, hft::itch::OrderExecutedAtPrice>) {
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.id, 8, "id");
                builder.add_u64(payload.shares.raw(), 4, "shares");
                builder.add_u64(payload.match_number, 8, "match");
                builder.add_u64(payload.printable, 1, "printable");
                builder.add_i64(payload.execution_price.raw(), 4, "execution_price");
            } else if constexpr (std::is_same_v<T, hft::itch::OrderCancel>) {
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.id, 8, "id");
                builder.add_u64(payload.shares.raw(), 4, "shares");
            } else if constexpr (std::is_same_v<T, hft::itch::OrderDelete>) {
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.id, 8, "id");
            } else if constexpr (std::is_same_v<T, hft::itch::OrderReplace>) {
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.original_id, 8, "original_id");
                builder.add_u64(payload.new_id, 8, "new_id");
                builder.add_u64(payload.shares.raw(), 4, "shares");
                builder.add_i64(payload.price.raw(), 4, "price");
            } else if constexpr (std::is_same_v<T, hft::itch::BrokenTrade>) {
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.match, 8, "match");
            } else if constexpr (std::is_same_v<T, hft::itch::Trade>) {
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.id, 8, "id");
                builder.add_u64(payload.side, 1, "side");
                builder.add_u64(payload.shares.raw(), 4, "shares");
                builder.add_i64(payload.price.raw(), 4, "price");
                builder.add_u64(payload.match, 8, "match");
            } else {
                // Cross Trade. The share count here is EIGHT bytes,
                // unlike every other message in the protocol.
                builder.add_u64(payload.stock_locate, 2, "locate");
                builder.add_u64(payload.tracking, 2, "tracking");
                builder.add_u64(payload.timestamp, 6, "timestamp");
                builder.add_u64(payload.shares, 8, "shares");
                builder.add_i64(payload.price.raw(), 4, "price");
                builder.add_u64(payload.match, 8, "match");
                builder.add_u64(payload.cross_type, 1, "cross_type");
            }
        },
        message.body);

    return builder.ok();
}

/// Build the trace record for one decoded frame.
///
/// The status mapping is the decoder's own, and the two truncation cases
/// are deliberately indistinguishable in the trace: a frame whose length
/// prefix was unreadable and a frame whose body was short are both
/// "truncated", because a consumer of the trace has the same question
/// about both -- "is there more to come?" -- and the same answer.
///
/// Whether the message TYPE is recorded depends on how far the decoder
/// got, and that is load-bearing rather than cosmetic. The decoder reads
/// the tag only after it knows the whole body is present, so a truncated
/// frame has no type even when its first byte is on the wire. The RTL
/// side must make the same decision at the same point, and a difference
/// here is a difference in where the decoder draws the line between
/// "not enough data yet" and "this frame is wrong".
[[nodiscard]] inline bool record_for(std::size_t index,
                                     const hft::itch::DecodeResult& result,
                                     TraceRecord& out,
                                     std::string& error) {
    error.clear();
    out = TraceRecord{};
    out.index = index;
    out.length = result.length;

    switch (result.status) {
        case hft::itch::DecodeStatus::ok:
            out.status = TraceStatus::decoded;
            out.type = static_cast<std::uint8_t>(result.message.type());
            out.has_type = true;
            break;
        case hft::itch::DecodeStatus::unknown_type:
            out.status = TraceStatus::skipped;
            out.type = result.tag;
            out.has_type = result.has_tag;
            break;
        case hft::itch::DecodeStatus::bad_length:
            out.status = TraceStatus::bad_length;
            out.type = result.tag;
            out.has_type = result.has_tag;
            break;
        case hft::itch::DecodeStatus::malformed:
            out.status = TraceStatus::malformed;
            out.type = result.tag;
            out.has_type = result.has_tag;
            break;
        case hft::itch::DecodeStatus::truncated:
        case hft::itch::DecodeStatus::truncated_body:
            out.status = TraceStatus::truncated;
            out.has_type = false;
            break;
    }

    if (out.status != TraceStatus::decoded) {
        return true;
    }

    if (!fields_of(result.message, out.fields, error)) {
        return false;
    }
    if (!validate_record(out, error)) {
        return false;
    }
    return true;
}

}  // namespace hft::rtl
