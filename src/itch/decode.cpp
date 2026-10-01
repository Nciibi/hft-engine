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

[[nodiscard]] DecodeResult truncated_result() noexcept {
    DecodeResult r;
    r.status = DecodeStatus::truncated;
    return r;
}

[[nodiscard]] DecodeResult unknown_type_result(std::uint16_t length) noexcept {
    DecodeResult r;
    r.status = DecodeStatus::unknown_type;
    r.length = length;
    r.message.type = static_cast<MessageType>(0);
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
        return truncated_result();
    }

    const std::uint16_t body_length = read_be16(data);
    if (body_length < 1) {
        DecodeResult r;
        r.status = DecodeStatus::bad_length;
        r.length = body_length;
        return r;
    }

    // Truncated body: the prefix is trustworthy, so report the length
    // and let the caller wait for more bytes rather than treating this
    // as corruption.
    if (available < kLengthPrefixSize + body_length) {
        DecodeResult r;
        r.status = DecodeStatus::truncated_body;
        r.length = body_length;
        return r;
    }

    const std::uint8_t* body = data + kLengthPrefixSize;
    const std::uint8_t tag = body[off::tag];

    if (tag != static_cast<std::uint8_t>(MessageType::add_order)) {
        // Structurally valid, not ours. Length is populated so the
        // caller can skip exactly this frame.
        return unknown_type_result(body_length);
    }

    // A frame tagged 'A' must declare exactly the Add Order length.
    // Accepting a longer frame would let a mis-framed message read
    // adjacent bytes as order fields.
    if (body_length != off::kAddOrderSize) {
        DecodeResult r;
        r.status = DecodeStatus::bad_length;
        r.length = body_length;
        return r;
    }

    AddOrder ao;
    ao.stock_locate = read_be16(body + off::stock_locate);
    ao.tracking = read_be16(body + off::tracking_number);
    ao.timestamp = read_timestamp48(body + off::timestamp);
    ao.id = read_be64(body + off::add_order_id);

    if (!side_from_byte(body[off::add_order_side], ao.side)) {
        DecodeResult r;
        r.status = DecodeStatus::malformed;
        r.length = body_length;
        return r;
    }

    ao.price = Price::from_raw(static_cast<std::int64_t>(read_be32(body + off::add_order_price)));
    ao.size = Quantity::from_raw(read_be32(body + off::add_order_size));
    ao.order_type = body[off::add_order_type];
    ao.time_in_force = body[off::add_order_tif];
    ao.display = body[off::add_order_display];
    ao.participant = body[off::add_order_participant];

    if (ao.id == kInvalidOrderId) {
        // ITCH uses 0 to mean "no order reference" in some contexts;
        // treating it as a real handle would collide with every
        // subsequent order.
        DecodeResult r;
        r.status = DecodeStatus::malformed;
        r.length = body_length;
        return r;
    }

    DecodeResult r;
    r.status = DecodeStatus::ok;
    r.length = body_length;
    r.message.type = MessageType::add_order;
    r.message.add_order = ao;
    return r;
}

}  // namespace hft::itch
