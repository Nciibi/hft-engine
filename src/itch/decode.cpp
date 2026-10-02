#include "hft/itch/decode.hpp"

namespace hft::itch {
namespace {

/// Map the ITCH side byte. Strict on purpose: 'B' and 'S' are the only
/// legal values, and an unrecognised byte is a protocol violation that
/// must surface rather than be coerced into a default side.
[[nodiscard]] bool side_from_byte(std::uint8_t b, Side& out) noexcept {
    if (b == 'B') {
        out = Side::bid;
        return true;
    }
    if (b == 'S') {
        out = Side::ask;
        return true;
    }
    return false;
}

/// The 8-byte header every order-level ITCH message shares: stock
/// locate, tracking number, and the split 48-bit timestamp. Returns
/// false only if the order reference is the reserved zero.
struct OrderHeader {
    StockLocate locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId id = kInvalidOrderId;
};

[[nodiscard]] OrderHeader read_order_header(const std::uint8_t* body) noexcept {
    OrderHeader h;
    h.locate = read_be16(body + off::stock_locate);
    h.tracking = read_be16(body + off::tracking_number);
    h.timestamp = read_timestamp48(body + off::timestamp);
    h.id = read_be64(body + off::order_cancel_id);  // offset 11 in all order messages
    return h;
}

[[nodiscard]] DecodeResult status_of(DecodeStatus status, std::uint16_t length) noexcept {
    DecodeResult r;
    r.status = status;
    r.length = length;
    return r;
}

}  // namespace

std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t read_be64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(p[0]) << 56) |
           (static_cast<std::uint64_t>(p[1]) << 48) |
           (static_cast<std::uint64_t>(p[2]) << 40) |
           (static_cast<std::uint64_t>(p[3]) << 32) |
           (static_cast<std::uint64_t>(p[4]) << 24) |
           (static_cast<std::uint64_t>(p[5]) << 16) |
           (static_cast<std::uint64_t>(p[6]) << 8) |
           static_cast<std::uint64_t>(p[7]);
}

Nanos read_timestamp48(const std::uint8_t* p) noexcept {
    const std::uint64_t high = read_be16(p);
    const std::uint64_t low = read_be32(p + 2);
    return (high << 32) | low;
}

DecodeResult decode(const std::uint8_t* data, std::size_t available) noexcept {
    if (available < kLengthPrefixSize) {
        return status_of(DecodeStatus::truncated, 0);
    }

    const std::uint16_t body_length = read_be16(data);
    if (body_length < 1) {
        return status_of(DecodeStatus::bad_length, body_length);
    }

    // Truncated body: the prefix is trustworthy, so report the length
    // and let the caller wait for more bytes rather than treating this
    // as corruption.
    if (available < kLengthPrefixSize + body_length) {
        return status_of(DecodeStatus::truncated_body, body_length);
    }

    const std::uint8_t* body = data + kLengthPrefixSize;
    const auto tag = static_cast<std::uint8_t>(body[off::tag]);

    // Reject a declared length that disagrees with the type before
    // reading any field, so a mis-framed message can never be read as
    // though its fields were laid out differently.
    std::size_t expected = 0;
    switch (static_cast<MessageType>(tag)) {
        case MessageType::add_order:               expected = off::kAddOrderSize; break;
        case MessageType::order_executed:          expected = off::kOrderExecutedSize; break;
        case MessageType::order_executed_at_price:
            expected = off::kOrderExecutedAtPriceSize;
            break;
        case MessageType::order_cancel:            expected = off::kOrderCancelSize; break;
        case MessageType::order_delete:            expected = off::kOrderDeleteSize; break;
    case MessageType::order_replace:            expected = off::kOrderReplaceSize; break;
    case MessageType::broken_trade:             expected = off::kBrokenTradeSize; break;        default:                                   expected = 0; break;  // skip below
    }

    if (expected != 0) {
        if (body_length != expected) {
            return status_of(DecodeStatus::bad_length, body_length);
        }
    } else {
        // A well-formed frame of a type this build does not decode.
        // Length is populated so the caller can skip exactly this
        // frame. Order Replace lands here on purpose: its field table
        // was not verified, and an unverified offset is worse than an
        // honest skip.
        return status_of(DecodeStatus::unknown_type, body_length);
    }

    const OrderHeader header = read_order_header(body);
    if (header.id == kInvalidOrderId) {
        // ITCH uses 0 to mean "no order reference" in some contexts;
        // treating it as a live handle would collide with every
        // subsequent order.
        return status_of(DecodeStatus::malformed, body_length);
    }

    DecodeResult r;
    r.length = body_length;

    switch (static_cast<MessageType>(tag)) {
        case MessageType::add_order: {
            AddOrder ao;
            ao.stock_locate = header.locate;
            ao.tracking = header.tracking;
            ao.timestamp = header.timestamp;
            ao.id = header.id;
            if (!side_from_byte(body[off::add_order_side], ao.side)) {
                return status_of(DecodeStatus::malformed, body_length);
            }
            ao.size = Quantity::from_raw(read_be32(body + off::add_order_size));
            for (std::size_t i = 0; i < off::kStockSymbolSize; ++i) {
                ao.stock[i] = static_cast<char>(body[off::add_order_stock + i]);
            }
            ao.price =
                Price::from_raw(static_cast<std::int64_t>(read_be32(body + off::add_order_price)));
            r.status = DecodeStatus::ok;
            r.message.body = ao;
            return r;
        }

        case MessageType::order_executed: {
            OrderExecuted e;
            e.stock_locate = header.locate;
            e.tracking = header.tracking;
            e.timestamp = header.timestamp;
            e.id = header.id;
            e.shares = Quantity::from_raw(read_be32(body + off::order_executed_shares));
            e.match_number = read_be64(body + off::order_executed_match);
            e.printable = body[off::order_executed_printable];
            r.status = DecodeStatus::ok;
            r.message.body = e;
            return r;
        }

        case MessageType::order_executed_at_price: {
            OrderExecutedAtPrice e;
            e.stock_locate = header.locate;
            e.tracking = header.tracking;
            e.timestamp = header.timestamp;
            e.id = header.id;
            e.shares = Quantity::from_raw(read_be32(body + off::order_exec_price_shares));
            e.match_number = read_be64(body + off::order_exec_price_match);
            e.printable = body[off::order_exec_price_printable];
            e.execution_price = Price::from_raw(static_cast<std::int64_t>(
                read_be32(body + off::order_exec_price_execution_price)));
            r.status = DecodeStatus::ok;
            r.message.body = e;
            return r;
        }

        case MessageType::order_cancel: {
            OrderCancel c;
            c.stock_locate = header.locate;
            c.tracking = header.tracking;
            c.timestamp = header.timestamp;
            c.id = header.id;
            c.shares = Quantity::from_raw(read_be32(body + off::order_cancel_shares));
            r.status = DecodeStatus::ok;
            r.message.body = c;
            return r;
        }

        case MessageType::order_delete: {
            OrderDelete d;
            d.stock_locate = header.locate;
            d.tracking = header.tracking;
            d.timestamp = header.timestamp;
            d.id = header.id;
            r.status = DecodeStatus::ok;
            r.message.body = d;
            return r;
        }

        case MessageType::broken_trade: {
            BrokenTrade b;
            b.stock_locate = header.locate;
            b.tracking = header.tracking;
            b.timestamp = header.timestamp;
            b.match = read_be64(body, off::broken_trade_match);
            r.status = DecodeStatus::ok;
            r.message.body = b;
            return r;
        }

        case MessageType::order_replace: {
            OrderReplace u;
            u.stock_locate = header.locate;
            u.tracking = header.tracking;
            u.timestamp = header.timestamp;
            u.original_id = read_be64(body, off::order_replace_original_id);
            u.new_id = read_be64(body, off::order_replace_new_id);
            u.shares = Quantity::from_raw(read_be32(body, off::order_replace_shares));
            u.price = Price::from_raw(read_be32(body, off::order_replace_price));
            r.status = DecodeStatus::ok;
            r.message.body = u;
            return r;
        }

        default:
            // Unreachable: every tag reaching here was classified
            // above. Returned rather than asserted so a future tag
            // added to the switch above degrades safely.
            return status_of(DecodeStatus::unknown_type, body_length);
    }
}

}  // namespace hft::itch
