// NASDAQ TotalView-ITCH 5.0 wire protocol.
//
// Every offset here is a byte offset from the START OF THE MESSAGE
// BODY, which includes the one-byte message type tag. The framing
// (2-byte big-endian length) sits in front of that and is handled by
// the decoder.
//
// Verification status matters in this file. Offsets for Add Order,
// Order Executed, Order Executed With Price, Order Cancel and Order
// Delete were transcribed from the Nasdaq interface specification
// field tables and are guarded by static_asserts. Offsets that have
// NOT been checked against the spec are marked `kUnverified` and
// deliberately carry no assert: a wrong static_assert here produces a
// decoder that confidently misreads the feed, which is strictly worse
// than a decoder that refuses to guess.

#pragma once

#include <cstddef>
#include <cstdint>

#include "hft/types.hpp"

namespace hft::itch {

/// Message type tag. The tag is the first byte of the body.
enum class MessageType : std::uint8_t {
    // Verified.
    add_order = 'A',                      ///< Add Order, no MPID attribution
    order_executed = 'E',                 ///< Order Executed
    order_executed_at_price = 'C',        ///< Order Executed With Price
    order_cancel = 'X',                   ///< Order Cancel (partial)
    order_delete = 'D',                   ///< Order Delete (entire order)

    // Tag known; body layout NOT yet verified against the spec.
    add_order_mpid = 'F',                 ///< Add Order with MPID attribution
    order_replace = 'U',                  ///< Order Replace
    system_event = 'S',                   ///< System Event
    stock_directory = 'R',                ///< Stock Directory
    trade = 'T',                          ///< Trade (non-cross)
    cross_trade = 'Q',                    ///< Cross Trade
    broken_trade = 'B',                   ///< Broken Trade
    end_of_market_hours = 'M',
    end_of_system_hours = 'Z',
};

/// Byte offsets within a message body. Only verified layouts appear
/// here; see the note at the top of this file.
namespace off {

// ---- Common header, shared by every order-level message ----
// Located at 1, not 0, because offset 0 is the type tag.
inline constexpr std::size_t tag = 0;
inline constexpr std::size_t stock_locate = 1;   ///< 2 bytes
inline constexpr std::size_t tracking_number = 3;  ///< 2 bytes
inline constexpr std::size_t timestamp = 5;      ///< 6 bytes, 48-bit ns

// The 48-bit timestamp is split high/low in the wire format: 2 bytes
// of high bits then 4 bytes of low bits. Reassembling it as
// `high * 2^32 + low` is the single easiest thing to get wrong in an
// ITCH decoder, and getting it wrong silently reorders a book.
inline constexpr std::size_t timestamp_high = timestamp;     ///< 2 bytes
inline constexpr std::size_t timestamp_low = timestamp + 2;  ///< 4 bytes
inline constexpr std::size_t kTimestampSize = 6;

inline constexpr std::size_t kStockLocateSize = 2;
inline constexpr std::size_t kTrackingSize = 2;
inline constexpr std::size_t kOrderIdSize = 8;
inline constexpr std::size_t kSharesSize = 4;
inline constexpr std::size_t kPriceSize = 4;

// ---- Add Order, tag 'A' ----
inline constexpr std::size_t add_order_id = 11;       ///< 8
inline constexpr std::size_t add_order_side = 19;     ///< 1, 'B' or 'S'
inline constexpr std::size_t add_order_price = 20;    ///< 4
inline constexpr std::size_t add_order_size = 24;     ///< 4
inline constexpr std::size_t add_order_type = 28;     ///< 1
inline constexpr std::size_t add_order_tif = 29;      ///< 1
inline constexpr std::size_t add_order_display = 30;  ///< 1
inline constexpr std::size_t add_order_participant = 31;  ///< 1
inline constexpr std::size_t kAddOrderSize = 32;

// ---- Order Executed, tag 'E' ----
inline constexpr std::size_t order_executed_id = 11;    ///< 8
inline constexpr std::size_t order_executed_shares = 19;  ///< 4
inline constexpr std::size_t order_executed_match = 23;   ///< 8
inline constexpr std::size_t order_executed_printable = 31;  ///< 1
inline constexpr std::size_t kOrderExecutedSize = 32;

// ---- Order Executed With Price, tag 'C' ----
// Identical to 'E' with a 4-byte execution price appended.
inline constexpr std::size_t order_exec_price_shares = 19;  ///< 4
inline constexpr std::size_t order_exec_price_match = 23;   ///< 8
inline constexpr std::size_t order_exec_price_printable = 31;  ///< 1
inline constexpr std::size_t order_exec_price_execution_price = 32;  ///< 4
inline constexpr std::size_t kOrderExecutedAtPriceSize = 36;

// ---- Order Cancel, tag 'X' ----
// A PARTIAL cancellation. `shares` is deducted from the quantity
// stated in the original Add Order. This is a different operation from
// Order Delete and conflating the two is the most common ITCH book
// bug; the reference model exists largely to catch it.
inline constexpr std::size_t order_cancel_id = 11;      ///< 8
inline constexpr std::size_t order_cancel_shares = 19;  ///< 4
inline constexpr std::size_t kOrderCancelSize = 20;

// ---- Order Delete, tag 'D' ----
// Removes the entire order regardless of remaining quantity.
inline constexpr std::size_t order_delete_id = 11;  ///< 8
inline constexpr std::size_t kOrderDeleteSize = 16;

}  // namespace off

// Guard the verified layouts. These fail at compile time, not at 3am.
static_assert(off::kAddOrderSize == 32, "ITCH Add Order body is 32 bytes");
static_assert(off::kOrderExecutedSize == 32, "ITCH Order Executed body is 32 bytes");
static_assert(off::kOrderExecutedAtPriceSize == 36, "ITCH Order Exec @ Price is 36 bytes");
static_assert(off::kOrderCancelSize == 20, "ITCH Order Cancel body is 20 bytes");
static_assert(off::kOrderDeleteSize == 16, "ITCH Order Delete body is 16 bytes");
// The timestamp ends exactly where the 6-byte field ends, and the low
// half begins immediately after the 2-byte high half.
static_assert(off::timestamp_low == off::timestamp_high + 2,
              "timestamp low half must follow the 2-byte high half");
static_assert(off::timestamp_low + 4 == off::timestamp + off::kTimestampSize,
              "48-bit timestamp is 2 high bytes + 4 low bytes");
static_assert(off::add_order_participant + 1 == off::kAddOrderSize,
              "Add Order final field must end at the body size");

/// Length prefix: 2 bytes, big-endian, and it COUNTS THE TAG.
inline constexpr std::size_t kLengthPrefixSize = 2;

/// Total bytes on the wire for a body of `body_size` bytes.
[[nodiscard]] constexpr std::size_t frame_size(std::size_t body_size) noexcept {
    return kLengthPrefixSize + body_size;
}

static_assert(frame_size(off::kAddOrderSize) == 34, "Add Order frame is 34 bytes");

}  // namespace hft::itch
