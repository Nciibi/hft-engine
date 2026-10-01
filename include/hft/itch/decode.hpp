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

#pragma once

#include <cstddef>
#include <cstdint>

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

struct AddOrder {
    StockLocate stock_locate = 0;
    TrackingNumber tracking = 0;
    Nanos timestamp = 0;
    OrderId id = kInvalidOrderId;
    Side side = Side::bid;
    Price price{};
    Quantity size{};
    std::uint8_t order_type = 0;
    std::uint8_t time_in_force = 0;
    std::uint8_t display = 0;
    std::uint8_t participant = 0;
};

struct Message {
    MessageType type = MessageType::add_order;
    AddOrder add_order{};
};

enum class DecodeStatus : std::uint8_t {
    ok = 0,
    /// Fewer than 2 bytes available: not even a length prefix.
    truncated,
    /// Fewer bytes available than the declared length requires.
    truncated_body,
    /// Well-formed frame of a type this build does not decode yet.
    /// `length` is populated so the caller can skip it.
    unknown_type,
    /// Declared length is too small to contain a tag.
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
[[nodiscard]] std::size_t frame_stride(const DecodeResult& result) noexcept {
    return result.status == DecodeStatus::truncated
               ? 0
               : static_cast<std::size_t>(kLengthPrefixSize) + result.length;
}

}  // namespace hft::itch
