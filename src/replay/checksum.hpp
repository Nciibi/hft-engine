// FNV-1a, used to fingerprint book state.
//
// FNV-1a is not a cryptographic hash and is not trying to be. It is
// used here because it is three lines, has no dependencies, produces
// identical results on every platform by construction, and is fast
// enough to run over book state repeatedly. The property being relied
// on is reproducibility, not collision resistance: the checksum's job
// is to make "the same input produced the same book" a checkable
// statement, and a second independent hash would not check that any
// better.
//
// The one thing FNV-1a is genuinely weak at is that it is
// order-sensitive but not structure-sensitive, so two states that
// differ only in how entries are grouped can collide. The fold in
// replay.cpp therefore emits a fully ordered, self-delimiting stream
// and hashes fixed-width integers, never formatted text.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hft::replay {

inline constexpr std::uint64_t fnv1a_offset_basis = 0xCBF2'9CE4'8422'2325ULL;
inline constexpr std::uint64_t fnv1a_prime = 0x0000'0100'0000'01B3ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a_u64(std::uint64_t hash, std::uint64_t value) noexcept {
    // Fold low byte first so the integer's bytes are consumed in a
    // fixed order. Without an explicit order, the result would depend
    // on host endianness and a checksum computed on an x86 host would
    // not match one computed on the same host for a different word
    // width.
    for (int i = 0; i < 8; ++i) {
        hash ^= static_cast<std::uint64_t>((value >> (i * 8)) & 0xFFu);
        hash *= fnv1a_prime;
    }
    return hash;
}

[[nodiscard]] constexpr std::uint64_t fnv1a_bytes(std::uint64_t hash,
                                                  const std::uint8_t* data,
                                                  std::size_t length) noexcept {
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= fnv1a_prime;
    }
    return hash;
}

}  // namespace hft::replay
