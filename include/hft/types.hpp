// Core value types for the HFT Engine.
//
// The central discipline of this codebase is that a price is never a
// floating point number. `Price` has no constructor from `float` or
// `double`, not by convention but by construction: once such a
// constructor exists it will be used, usually in a config loader or a
// benchmark, and the rounding error will surface months later as an
// unreproducible PnL discrepancy. ITCH carries prices as 4-byte
// integers in units of 1/10000, so that is exactly what we store.

#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hft {

/// Fixed-point price in units of 1/10000, matching ITCH precision.
///
/// The default-constructed value is zero, not a sentinel. Callers that
/// need an "unset" price should use `std::optional<Price>`; a
/// distinguished in-band sentinel in a price type is a bug factory.
class Price final {
public:
    static constexpr std::int64_t kScale = 10'000;
    /// Number of decimal digits below the wire representation.
    static constexpr int kDecimals = 4;

    constexpr Price() noexcept = default;

    [[nodiscard]] static constexpr Price from_raw(std::int64_t raw) noexcept {
        Price p;
        p.raw_ = raw;
        return p;
    }

    [[nodiscard]] static constexpr Price from_int(std::int64_t whole) noexcept {
        return from_raw(whole * kScale);
    }

    /// Parse an exact decimal string such as "123.45", "123", or
    /// "123.4500". Rejects a sign, an exponent, more than four
    /// fractional digits, and anything trailing. Performs pure integer
    /// arithmetic; no float is constructed at any point.
    ///
    /// Intended for configuration and test fixtures, never for the hot
    /// path.
    [[nodiscard]] static std::optional<Price> parse(std::string_view text) noexcept;

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }

    /// Render as a decimal string with exactly four fractional digits.
    [[nodiscard]] std::string to_string() const;

    auto operator<=>(const Price&) const noexcept = default;

private:
    std::int64_t raw_ = 0;
};

static_assert(Price::kScale == 10'000, "ITCH price scale is 1/10000");
static_assert(sizeof(Price) == sizeof(std::int64_t),
              "Price must be a bare int64_t: no vtable, no padding");

/// Order quantity. ITCH carries shares as a 4-byte unsigned integer;
/// we widen to 64 bits so accumulated notional can never overflow.
/// Never negative: a size is subtracted, not negated.
class Quantity final {
public:
    constexpr Quantity() noexcept = default;

    [[nodiscard]] static constexpr Quantity from_raw(std::uint64_t raw) noexcept {
        Quantity q;
        q.raw_ = raw;
        return q;
    }

    [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }

    /// Saturating subtraction. An over-subtraction is a protocol
    /// violation, not a value to propagate: a book that can be driven
    /// to a negative size is worse than a book that clamps and
    /// reports. Callers detect the condition by comparing the
    /// pre-call value to the post-call value.
    [[nodiscard]] constexpr Quantity saturating_sub(Quantity other) noexcept {
        return from_raw(raw_ > other.raw_ ? raw_ - other.raw_ : 0);
    }

    auto operator<=>(const Quantity&) const noexcept = default;

private:
    std::uint64_t raw_ = 0;
};

static_assert(sizeof(Quantity) == sizeof(std::uint64_t), "Quantity must be a bare uint64_t");

/// Day-unique order reference number as carried in ITCH.
using OrderId = std::uint64_t;

inline constexpr OrderId kInvalidOrderId = 0;

/// ITCH's 48-bit nanoseconds-since-midnight, widened to 64 bits so the
/// 2-byte high half can be shifted rather than multiplied.
using Nanos = std::uint64_t;

/// ITCH tracking number, 2 bytes. Not a sequence number: the SOUP
/// sequence lives in the MoldUDP64 wrapper, not the message body.
using TrackingNumber = std::uint16_t;

/// ITCH stock locate, 2 bytes.
using StockLocate = std::uint16_t;

enum class Side : std::uint8_t {
    bid = 0,
    ask = 1,
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::bid ? Side::ask : Side::bid;
}

enum class OrderState : std::uint8_t {
    /// Fully working on the book.
    new_order = 0,
    /// Some quantity consumed, still working.
    partially_filled = 1,
    /// Fully consumed. Not on the book.
    filled = 2,
    /// Removed before completion. Not on the book.
    cancelled = 3,
    /// Superseded by a replace; superseded orders leave the book.
    replaced = 4,
};

[[nodiscard]] constexpr bool is_working(OrderState s) noexcept {
    return s == OrderState::new_order || s == OrderState::partially_filled;
}

}  // namespace hft
