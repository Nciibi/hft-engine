#include "hft/types.hpp"

namespace hft {
namespace {

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

/// Largest whole-unit value that can be scaled by kScale without
/// overflowing int64. Prices beyond this are a configuration error, not
/// a value to silently wrap.
constexpr std::int64_t kMaxWhole = INT64_MAX / Price::kScale;

constexpr std::uint64_t kPow10[5] = {1, 10, 100, 1000, 10000};

}  // namespace

std::optional<Price> Price::parse(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t i = 0;
    std::int64_t whole = 0;
    std::size_t whole_digits = 0;

    while (i < text.size() && is_digit(text[i])) {
        whole = whole * 10 + (text[i] - '0');
        ++i;
        ++whole_digits;
        // Reject as soon as the value cannot possibly scale, rather
        // than after the multiplication has already overflowed.
        if (whole > kMaxWhole) {
            return std::nullopt;
        }
    }

    if (whole_digits == 0) {
        return std::nullopt;
    }

    std::int64_t frac = 0;
    int frac_digits = 0;

    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && is_digit(text[i])) {
            if (frac_digits == kDecimals) {
                // A fifth fractional digit cannot be represented
                // exactly. Rejecting is correct: rounding here would
                // silently introduce the exact error this type exists
                // to prevent.
                return std::nullopt;
            }
            frac = frac * 10 + (text[i] - '0');
            ++i;
            ++frac_digits;
        }
    }

    if (i != text.size()) {
        // Trailing characters: 'x', a second '.', an exponent, a sign.
        return std::nullopt;
    }

    // Right-pad the fraction to four digits using integer arithmetic.
    while (frac_digits < kDecimals) {
        frac *= 10;
        ++frac_digits;
    }

    return from_raw(whole * kScale + frac);
}

std::string Price::to_string() const {
    // Negate via unsigned to survive INT64_MIN, where -(raw) is UB.
    const bool negative = raw_ < 0;
    const std::uint64_t magnitude =
        negative ? (~static_cast<std::uint64_t>(raw_) + 1u) : static_cast<std::uint64_t>(raw_);

    const std::uint64_t whole = magnitude / static_cast<std::uint64_t>(kScale);
    const std::uint64_t frac = magnitude % static_cast<std::uint64_t>(kScale);

    std::string out;
    out.reserve(24);
    if (negative) {
        out.push_back('-');
    }

    // Manual integer to string: no allocation, no locale, no float.
    char digits[24];
    int n = 0;
    std::uint64_t v = whole;
    do {
        digits[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        out.push_back(digits[--n]);
    }

    out.push_back('.');
    for (int k = kDecimals - 1; k >= 0; --k) {
        out.push_back(static_cast<char>('0' + ((frac / kPow10[k]) % 10)));
    }
    return out;
}

}  // namespace hft
