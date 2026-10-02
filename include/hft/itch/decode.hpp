// Zero-copy ITCH decoder.
//
// Design constraints:
//
//  * No alignment guarantee. The input is typically a pointer into an
//    mmap'd file, so every multi-byte read is assembled byte by byte
//    rather than reinterpret_cast through a pointer. This is portable
//    and well-defined; a bswap/SIMD fast path is a later optimisation,
//    not a correctness decision.
//  * No packed structs and no bitfields. Struct layout with those is
//    implementation-defined in ways that differ between MSVC and GCC,
//    which is precisely how a feed decoder ends up working on the
//    machine you benchmarked and not the one you deployed on.
//  * Never reads past `available`. A streaming decoder sees truncated
//    frames routinely and must report that, not fault.
//  * Unknown message types report their length so the caller can skip
//    them. A live feed always contains types this build does not
//    implement; crashing is not an option and guessing is worse.
//
// The decoded payloads are held in a std::variant rather than as five
// separate members of one fat struct. A struct with all five members
// would be roughly three times larger than the largest payload and get
// copied for every message, including the common ones; the variant is
// sized to the largest payload plus a discriminant, and `type` is
// derived from the variant rather than being a second source of truth
// that can disagree with it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

#include "hft/itch/protocol.hpp"
#include "hft/types.hpp"

namespace hft::itch {

[[nodiscard]] std::uint16_t read_be16(const std::uint8_t* p) noexcept;
[[nodiscard]] std::uint32_t read_be32(const std::uint8_t* p) noexcept;
[[nodiscard]] std::uint64_t read_be64(const std::uint8_t* p) noexcept;

/// Reassemble ITCH's split 48-bit nanosecond timestamp.
///
/// The wire format is 2 bytes of high bits followed by 4 bytes of low
/// bits. Reading this as a contiguous 6-byte little-endian integer, or
/// as high*65536+low, both produce plausible-looking timestamps that
/// reorder the book. This function is the only place it happens.
[[nodiscard]] Nanos read_timestamp48(const std::uint8_t* p) noexcept;

// ---- Decoded payloads ----------------------------------------------
//
// Each mirrors one ITCH message body exactly. All are trivially
// copyable, so the variant never allocates.

/// ITCH 'A'. Consumes no quantity: it creates the order.
struct AddOrder {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId id = kInvalidOrderId;
    Side side = Side::bid;
    /// Share count. NOT the price: in the wire layout the two fields
    /// are adjacent and in the opposite order to what the previous
    /// revision of this decoder assumed.
    Quantity size{};
    /// The display price, at the END of the message body, after the
    /// eight-byte stock symbol.
    Price price{};
    /// The 8-byte space-padded stock symbol.
    ///
    /// Carried rather than discarded because a real handler needs it to
    /// route the message to the right book, and a decoder that throws
    /// the symbol away cannot be fed a multi-symbol feed at all. This
    /// build runs one instrument, so nothing reads it -- which is
    /// exactly why it was worth keeping: the field exists so the
    /// multi-shard work does not have to re-derive the layout.
    char stock[off::kStockSymbolSize] = {};
};

/// ITCH 'E'. Consumes `shares` from the order's remaining size.
struct OrderExecuted {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId id = kInvalidOrderId;
    Quantity shares{};
    std::uint64_t match_number = 0;
    std::uint8_t printable = 0;
};

/// ITCH 'C'. As 'E', but the fill happened at a price other than the
/// order's displayed price. The execution price is carried here, not
/// inferred from the book: the resting order's own price is
/// unchanged by a fill at a different price.
struct OrderExecutedAtPrice {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId id = kInvalidOrderId;
    Quantity shares{};
    std::uint64_t match_number = 0;
    std::uint8_t printable = 0;
    Price execution_price{};
};

/// ITCH 'X'. A PARTIAL cancellation: `shares` is deducted from the
/// quantity stated in the original Add Order. Distinct from Order
/// Delete, and conflating the two is the most common ITCH book bug.
struct OrderCancel {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId id = kInvalidOrderId;
    Quantity shares{};
};

/// ITCH 'D'. Removes the entire order, discarding any remainder.
struct OrderDelete {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId id = kInvalidOrderId;
};

/// ITCH 'U'. Verified against TotalView-ITCH 5.0 section 4.4.5.
///
/// The original order ceases to exist entirely -- its remaining shares
/// "are no longer accessible" per the specification, which is not the
/// same as a partial cancel and not the same as a delete followed by an
/// add, because the replacement carries a NEW reference number and
/// therefore NEW time priority: it sorts behind everything already
/// resting at its price.
///
/// Side, stock and MPID are deliberately absent from the wire format and
/// are carried over from the original Add Order. That is why this struct
/// cannot be applied on its own and why the apply layer has to look the
/// original order up.
struct OrderReplace {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId original_id = kInvalidOrderId;
    OrderId new_id = kInvalidOrderId;
    Quantity shares{};
    Price price{};
};

/// ITCH 'B'. Verified against TotalView-ITCH 5.0 section 4.5.3.
///
/// 'B' is Broken Trade, not order entry: an inbound report that an
/// execution was cancelled under the clearly-erroneous policy. It has
/// no effect on the book, so the apply layer is a no-op for it, but it
/// matters to any time-and-sales view and a handler that never sees it
/// has a hole it does not know about.
struct BrokenTrade {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId match = kInvalidOrderId;
};

/// ITCH 'P', Trade (Non-Cross). Verified against section 1.5.1.
///
/// The tag is 'P'. An earlier revision of this repository listed it as
/// 'T', which no version of TotalView-ITCH 5.0 uses -- see the note on
/// `off::kTradeSize`.
///
/// Reported for non-displayed order types: no Add Order is generated
/// for such an order, so a Trade message is the only evidence that one
/// matched. It follows that this message arrives with `id` legitimately
/// ZERO on the binary feeds ("Nasdaq will populate the Order Reference
/// Number field ... as zero"), which is why an order reference of zero
/// is a value to carry rather than reject here. The specification states
/// these messages do not affect the book, so the apply layer does not
/// touch it.
struct Trade {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId id = kInvalidOrderId;
    std::uint8_t side = 0;   ///< 'B' or 'S'
    Quantity shares{};
    Price price{};
    OrderId match = kInvalidOrderId;
};

/// ITCH 'Q', Cross Trade. Verified against section 1.5.2.
///
/// Note the share count is EIGHT bytes here, against four in every
/// other message in this file. A cross prints far more volume than a
/// single execution, and getting that width wrong shifts every
/// subsequent field.
struct CrossTrade {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    std::uint64_t shares = 0;
    Price price{};
    OrderId match = kInvalidOrderId;
    std::uint8_t cross_type = 0;  ///< 'O' opening, 'C' closing, 'H' halt/IPO
};

using MessageBody = std::variant<AddOrder, OrderExecuted, OrderExecutedAtPrice, OrderCancel,
                                 OrderDelete, OrderReplace, BrokenTrade, Trade, CrossTrade>;

/// A decoded message: the payload plus the tag derived from it.
struct Message {
    MessageBody body{};

    [[nodiscard]] MessageType type() const noexcept {
        return static_cast<MessageType>(std::visit(
            [](const auto& payload) noexcept -> std::uint8_t {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, AddOrder>) {
                    return static_cast<std::uint8_t>(MessageType::add_order);
                } else if constexpr (std::is_same_v<T, OrderExecuted>) {
                    return static_cast<std::uint8_t>(MessageType::order_executed);
                } else if constexpr (std::is_same_v<T, OrderExecutedAtPrice>) {
                    return static_cast<std::uint8_t>(MessageType::order_executed_at_price);
                } else if constexpr (std::is_same_v<T, OrderCancel>) {
                    return static_cast<std::uint8_t>(MessageType::order_cancel);
                } else if constexpr (std::is_same_v<T, OrderDelete>) {
                    return static_cast<std::uint8_t>(MessageType::order_delete);
                } else if constexpr (std::is_same_v<T, OrderReplace>) {
                    return static_cast<std::uint8_t>(MessageType::order_replace);
                } else if constexpr (std::is_same_v<T, BrokenTrade>) {
                    return static_cast<std::uint8_t>(MessageType::broken_trade);
                } else if constexpr (std::is_same_v<T, Trade>) {
                    return static_cast<std::uint8_t>(MessageType::trade);
                } else {
                    return static_cast<std::uint8_t>(MessageType::cross_trade);
                }
            },
            body));
    }
};

static_assert(std::variant_size_v<MessageBody> == 9,
              "MessageBody must cover every decoded ITCH type");

enum class DecodeStatus : std::uint8_t {
    ok = 0,
    /// Fewer than 2 bytes available: not even a length prefix.
    truncated,
    /// Fewer bytes available than the declared length requires.
    truncated_body,
    /// Well-formed frame of a type this build does not decode yet.
    /// `length` is populated so the caller can skip it.
    ///
    /// This is also the result for Order Replace ('U'). Its body
    /// layout was not verified against the published field table, so
    /// rather than guess an offset and silently misparse a live feed,
    /// it is skipped by length exactly like any other unknown type.
    /// Guessing would be the worse failure: skip-by-length is
    /// recoverable, a wrong field offset is not.
    unknown_type,
    /// Declared length is too small to contain a tag, or disagrees
    /// with the length required by the decoded type.
    bad_length,
    /// A field held a value outside its defined range, e.g. a side
    /// byte that is neither 'B' nor 'S'. Never defaulted: silently
    /// reading an unknown side as a bid corrupts the book in a way no
    /// downstream test would catch.
    malformed,
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::truncated;
    /// Declared body length in bytes, excluding the 2-byte prefix.
    /// Populated whenever the prefix itself was readable.
    std::uint16_t length = 0;
    Message message{};

    [[nodiscard]] bool ok() const noexcept { return status == DecodeStatus::ok; }
    /// True when the frame was structurally sound but this build does
    /// not decode its type. The caller should skip `length + 2` bytes
    /// and continue rather than treating it as an error.
    [[nodiscard]] bool skippable() const noexcept {
        return status == DecodeStatus::unknown_type;
    }
};

/// Decode one frame from `data`.
///
/// `available` is the number of readable bytes at `data`. On success or
/// on a skippable/unknown result, the caller advances by
/// `2 + result.length` bytes.
[[nodiscard]] DecodeResult decode(const std::uint8_t* data, std::size_t available) noexcept;

/// Advance past the current frame, or return 0 if the buffer cannot
/// even supply a length prefix.
///
/// `inline` because this is defined in a header: without it every
/// translation unit emits a definition and the link fails with a
/// duplicate symbol.
[[nodiscard]] inline std::size_t frame_stride(const DecodeResult& result) noexcept {
    return result.status == DecodeStatus::truncated
               ? 0
               : static_cast<std::size_t>(kLengthPrefixSize) + result.length;
}

}  // namespace hft::itch
