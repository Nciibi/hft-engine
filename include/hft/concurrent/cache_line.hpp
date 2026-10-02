// Cache-line isolation.
//
// A producer writing one atomic index and a consumer reading it will
// fight over the cache line it lives on even when neither is writing,
// because the line ping-pongs between the two cores' caches. On a 1MB
// L2 that is invisible; on a market-data handoff it is the single
// largest cost in the structure. Separating them is not a tuning knob,
// it is the reason a ring buffer is padded at all.
//
// The line size is 64 bytes because that is the coherence granularity
// on every x86-64 part, and it is stated rather than queried: a runtime
// query would compile to a different number on every machine, and the
// whole point is that the padding is a fixed, checkable property.
//
// `Padded<T>` asserts its own size rather than silently accepting
// growth. A T that grows past a cache line stops being isolated, and a
// build error is the only version of that fact anyone ever reads.

#pragma once

#include <cstddef>

namespace hft::concurrent {

/// Coherence granularity, in bytes. Not a tuning parameter: it is a
/// property of the architecture the code is compiled for.
inline constexpr std::size_t kCacheLineSize = 64;

/// True when `T` fits in the space `Padded<T>` can isolate.
///
/// Declared as a variable rather than a trait so the failure message
/// names the size that broke the invariant.
template <typename T>
inline constexpr bool kFitsInCacheLine = sizeof(T) <= kCacheLineSize;

/// A value alone on its own cache line.
///
/// `alignas` puts it on a line boundary, and because the struct's own
/// alignment is 64 bytes its size is rounded up to a whole number of
/// lines. That rounding is what stops the NEXT member from sharing the
/// line this one was carefully given.
template <typename T>
struct alignas(kCacheLineSize) Padded {
    static_assert(kFitsInCacheLine<T>,
                  "T is larger than a cache line: Padded<T> no longer isolates it, and the "
                  "false sharing it exists to prevent will return silently");

    /// Default-constructed, not value-initialised-then-overwritten. For
    /// every type this repository puts in a Padded (an atomic index, a
    /// counter) that is free, and the constructor stays constexpr so
    /// the containing object can still be a namespace-scope constant.
    T value{};

    [[nodiscard]] constexpr T& operator*() noexcept { return value; }
    [[nodiscard]] constexpr const T& operator*() const noexcept { return value; }

    T* operator->() noexcept { return &value; }
    const T* operator->() const noexcept { return &value; }
};

/// Every `Padded` is a whole number of cache lines by construction.
static_assert(sizeof(Padded<std::size_t>) % kCacheLineSize == 0,
              "Padded must occupy whole cache lines or it does not isolate anything");

/// Round `bytes` up to a whole number of cache lines.
///
/// For storage that has no `alignas` of its own, because it is a
/// variable-length member sized at construction.
[[nodiscard]] constexpr std::size_t round_up_to_cache_line(std::size_t bytes) noexcept {
    return ((bytes + kCacheLineSize - 1) / kCacheLineSize) * kCacheLineSize;
}

}  // namespace hft::concurrent