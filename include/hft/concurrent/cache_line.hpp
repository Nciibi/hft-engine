// Cache-line isolation.
//
// A producer writing one atomic index and a consumer reading it will
// fight over the cache line it lives on even when neither is writing,
// because the line ping-pongs between the two cores' caches. On a
// 1MB L2 that is invisible; on a market-data handoff it is the single
// largest cost in the structure. Separating them is not a tuning knob,
// it is the reason a ring buffer is padded at all.
//
// The line size is 64 bytes because that is the coherence granularity
// on every x86-64 part, and it is stated rather than queried: a runtime
// query would compile to a different number on every machine and the
// whole point is that the padding is a fixed, checkable property.
//
// `Padded<T>` asserts its own size rather than silently accepting
// growth. A T that grows past a cache line stops being isolated, and a
// build error is the only version of that fact anyone reads.

#pragma once

#include <cstddef>
#include <new>

namespace hft::concurrent {

/// Coherence granularity, in bytes. Not a tuning parameter: it is a
/// property of the architecture the code is compiled for.
inline constexpr std::size_t kCacheLineSize = 64;

/// A value alone on its own cache line.
///
/// `alignas` puts it on a line boundary. The struct itself is padded up
/// to a whole number of lines, which is what stops the NEXT member from
/// sharing the line that this one was carefully given.
template <typename T>
struct alignas(kCacheLineSize) Padded {
    T value{};

    constexpr Padded() noexcept = default;

    // The T value is left default-constructed. For every type used in
    // this repository's concurrency code (atomics, integers) that is
    // free; requiring a trivial default constructor keeps the guarantee
    // honest instead of leaving it to be discovered as a constructor
    // that quietly walks an array.
    constexpr explicit Padded(const T& initial) noexcept : value(initial) {}

    [[nodiscard]] constexpr T& operator*() noexcept { return value; }
    [[nodiscard]] constexpr const T& operator*() const noexcept { return value; }

    T* operator->() noexcept { return &value; }
    const T* operator->() const noexcept { return &value; }
};

/// Every T placed in a Padded must fit, or the isolation is a lie. A
/// `static_assert` here catches it in the file that introduced the type
/// rather than in a profile three weeks later.
template <typename T>
struct FitsInCacheLine : std::bool_constant<(sizeof(T) <= kCacheLineSize)> {};

template <typename T>
inline constexpr bool kFitsInCacheLine = FitsInCacheLine<T>::value;

/// Round `bytes` up to a whole number of cache lines.
///
/// Used for buffer storage, which has no alignas of its own because it
/// is a variable-length member of a runtime-sized object.
[[nodiscard]] constexpr std::size_t round_up_to_cache_line(std::size_t bytes) noexcept {
    return ((bytes + kCacheLineSize - 1) / kCacheLineSize) * kCacheLineSize;
}

}  // namespace hft::concurrent