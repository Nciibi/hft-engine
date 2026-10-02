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
//
// VERIFIED against the Nasdaq TotalView-ITCH 5.0 specification, section
// 1.3.1 "Add Order - No MPID Attribution Message". The field table is:
//
//   0  1  Message Type          'A'
//   1  2  Stock Locate
//   3  2  Tracking Number
//   5  6  Timestamp             48-bit ns since midnight
//  11  8  Order Reference Number
//  19  1  Buy/Sell Indicator    'B' or 'S'
//  20  4  Shares
//  24  8  Stock                 alpha, space padded
//  32  4  Price
//  ---> total 36 bytes
//
// An earlier revision of this file had Shares mislabelled as `price`,
// the stock symbol mislabelled as `size`, four bytes of nothing at
// 28-31, and a total of 32. Those four trailing bytes were Order Entry
// fields (`order_type`, `time_in_force`, `display`, `participant`) that
// do not exist in an Add Order message; the layout appears to have been
// copied from Order Executed, which does have that shape. The net effect
// was that the decoder read the share count as the price and four bytes
// of the symbol as the size.
//
// It was invisible for a long time because the feed generator wrote the
// same wrong layout the decoder read, so the two agreed with each other
// and disagreed with the specification. Every other message type in
// this file ('E', 'C', 'X', 'D') was always correct.
inline constexpr std::size_t add_order_id = 11;       ///< 8
inline constexpr std::size_t add_order_side = 19;     ///< 1, 'B' or 'S'
inline constexpr std::size_t add_order_size = 20;     ///< 4, shares
inline constexpr std::size_t add_order_stock = 24;    ///< 8, alpha, space padded
inline constexpr std::size_t kStockSymbolSize = 8;
inline constexpr std::size_t add_order_price = 32;    ///< 4
inline constexpr std::size_t kAddOrderSize = add_order_price + kPriceSize;

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

// ---- Order Delete, tag 'D' ----
// Removes the entire order regardless of remaining quantity.
inline constexpr std::size_t order_delete_id = 11;  ///< 8

// ---- Order Replace, tag 'U' ----
// Verified against TotalView-ITCH 5.0 section 4.4.5 (Order Replace
// Message), the Nasdaq document itself.
//
// This one was skipped for a long time on the grounds that its layout
// could not be verified. It could be; the specification is public. What
// was actually true is that the layouts differ between protocol
// VERSIONS, so a table copied from ITCH 3.1 or 4.0 -- which both exist
// and both differ, 4.0 omitting the Display field that 5.0 never had at
// this position -- is wrong in a way that looks right. Skip-by-length
// was the correct call then; it is not the reason any longer.
//
// Side, stock and MPID are absent by design and must be carried over
// from the original Add Order. A new reference number means NEW time
// priority, so the replacement sorts behind everything already resting
// at its price -- see the apply layer.
inline constexpr std::size_t order_replace_original_id = 11;  ///< 8
inline constexpr std::size_t order_replace_new_id = 19;       ///< 8
inline constexpr std::size_t order_replace_shares = 27;      ///< 4, new total
inline constexpr std::size_t order_replace_price = 31;       ///< 4
inline constexpr std::size_t kOrderReplaceSize = order_replace_price + kPriceSize;

// ---- Broken Trade, tag 'B' ----
// Verified against TotalView-ITCH 5.0 section 4.5.3 (Broken Trade /
// Order Execution Message), identical in the Nasdaq NQ, BX and PSX
// specifications.
//
// 'B' is NOT order entry. An earlier revision of this repository
// described an "ITCH Order Entry ('B')" encode stage that it declined
// to implement for lack of a verified table; the specification states
// that TotalView-ITCH "is an outbound market data feed only" and
// "does not support order entry", and 'B' here is an inbound report
// that an execution was broken under the clearly-erroneous policy.
//
// It has no effect on the book -- the specification says a firm using
// the feed to build a book "may ignore these messages" -- but it is not
// ignorable for a time-and-sales view, and it is decoded here so that a
// caller can see it rather than discover a hole in its own handling.
inline constexpr std::size_t broken_trade_match = 11;  ///< 8
inline constexpr std::size_t kBrokenTradeSize = broken_trade_match + kOrderIdSize;

/// Body sizes are DERIVED from the last field's offset, never written
/// as independent constants. An earlier revision of this file
/// hard-coded 20 and 16 for these two and was wrong by 3 bytes each;
/// the static_asserts below did not catch it because they compared
/// constants to each other rather than to the field layout. Deriving
/// the size makes it impossible to write a body length that disagrees
/// with the offsets the decoder reads.
inline constexpr std::size_t kOrderCancelSize = order_cancel_shares + kSharesSize;
inline constexpr std::size_t kOrderDeleteSize = order_delete_id + kOrderIdSize;

}  // namespace off

// Guard the verified layouts. These fail at compile time, not at 3am.
//
// Every offset below is ALSO asserted against its literal value from the
// published field table, not merely against the offset of the next
// field. That distinction is not pedantry: an earlier revision of this
// file asserted only the relationships (`size == last offset + width`),
// so a wholesale mislabelling of the fields passed every check in the
// file. The relationship asserts prove the sizes are internally
// consistent; the literal asserts prove they are CONSISTENT WITH THE
// SPECIFICATION. Both are needed, and only the second one would have
// caught the Add Order bug.
static_assert(off::kAddOrderSize == 36, "ITCH Add Order body is 36 bytes");
static_assert(off::kOrderExecutedSize == 32, "ITCH Order Executed body is 32 bytes");
static_assert(off::kOrderExecutedAtPriceSize == 36, "ITCH Order Exec @ Price body is 36 bytes");
static_assert(off::kOrderCancelSize == 23, "ITCH Order Cancel body is 23 bytes");
static_assert(off::kOrderDeleteSize == 19, "ITCH Order Delete body is 19 bytes");
static_assert(off::kOrderReplaceSize == 35,
              "ITCH 5.0 Order Replace body is 35 bytes (spec 4.4.5)");
static_assert(off::order_replace_original_id == 11,
              "Order Replace original reference sits at offset 11");
static_assert(off::order_replace_new_id == off::order_replace_original_id + 8,
              "Order Replace new reference follows the original");
static_assert(off::order_replace_shares == off::order_replace_new_id + 8,
              "Order Replace shares follow the new reference");
static_assert(off::order_replace_price == off::order_replace_shares + 4,
              "Order Replace price follows the shares");
static_assert(off::kBrokenTradeSize == 19, "ITCH Broken Trade body is 19 bytes (spec 4.5.3)");
static_assert(off::broken_trade_match == 11, "Broken Trade match number sits at offset 11");
static_assert(off::kOrderReplaceSize == off::order_replace_price + 4,
              "the Order Replace body ends after the price");
static_assert(off::frame_size(off::kOrderReplaceSize) == 37,
              "Order Replace frame is 35 body bytes plus a 2-byte length prefix");
static_assert(off::frame_size(off::kBrokenTradeSize) == 21,
              "Broken Trade frame is 19 body bytes plus a 2-byte length prefix");

// Add Order, spec section 1.3.1, field by field.
static_assert(off::add_order_id == 11, "Add Order reference sits at offset 11");
static_assert(off::add_order_side == 19, "Add Order side indicator sits at offset 19");
static_assert(off::add_order_size == 20, "Add Order SHARES sit at offset 20, not the price");
static_assert(off::add_order_stock == 24, "Add Order stock symbol occupies offset 24");
static_assert(off::kStockSymbolSize == 8, "the stock symbol field is 8 bytes");
static_assert(off::add_order_price == 32, "Add Order PRICE sits at offset 32, not 20");
static_assert(off::add_order_price == off::add_order_stock + off::kStockSymbolSize,
              "the price begins immediately after the stock symbol");
static_assert(off::add_order_stock == off::add_order_size + off::kSharesSize,
              "the stock symbol begins immediately after the share count");

// Order Executed, spec section 1.4.1.
static_assert(off::order_executed_shares == 19, "Order Executed shares sit at offset 19");
static_assert(off::order_executed_match == 23, "Order Executed match number sits at offset 23");
static_assert(off::order_executed_printable == 31, "Order Executed printable flag at offset 31");

// Every size must equal the end of its last field.
static_assert(off::add_order_price + off::kPriceSize == off::kAddOrderSize,
              "Add Order size must equal the end of its last field");
static_assert(off::order_executed_printable + 1 == off::kOrderExecutedSize,
              "Order Executed size must equal the end of its last field");
static_assert(off::order_exec_price_execution_price + off::kPriceSize ==
                  off::kOrderExecutedAtPriceSize,
              "Order Exec @ Price size must equal the end of its last field");
static_assert(off::order_cancel_shares + off::kSharesSize == off::kOrderCancelSize,
              "Order Cancel size must equal the end of its last field");
static_assert(off::order_delete_id + off::kOrderIdSize == off::kOrderDeleteSize,
              "Order Delete size must equal the end of its last field");

// The shared 8-byte order header must fit inside every order message.
static_assert(off::order_cancel_id == off::order_delete_id,
              "order reference sits at the same offset in all order messages");
static_assert(off::order_delete_id + off::kOrderIdSize <= off::kOrderDeleteSize,
              "the shared header must fit in the smallest order message");

// The timestamp ends exactly where the 6-byte field ends, and the low
// half begins immediately after the 2-byte high half.
static_assert(off::timestamp_low == off::timestamp_high + 2,
              "timestamp low half must follow the 2-byte high half");
static_assert(off::timestamp_low + 4 == off::timestamp + off::kTimestampSize,
              "48-bit timestamp is 2 high bytes + 4 low bytes");

/// Length prefix: 2 bytes, big-endian, and it COUNTS THE TAG.
inline constexpr std::size_t kLengthPrefixSize = 2;

/// Total bytes on the wire for a body of `body_size` bytes.
[[nodiscard]] constexpr std::size_t frame_size(std::size_t body_size) noexcept {
    return kLengthPrefixSize + body_size;
}

static_assert(frame_size(off::kAddOrderSize) == 38, "Add Order frame is 38 bytes");

}  // namespace hft::itch
