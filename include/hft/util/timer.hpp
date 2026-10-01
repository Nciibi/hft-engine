// Portable monotonic timer.
//
// The measurement environment is a claim in this project, so the clock
// has to be defensible. Three requirements, in priority order:
//
//  1. Monotonic. A wall clock that can step backwards produces negative
//     latencies, which is worse than no measurement at all.
//  2. High resolution. On Windows QueryPerformanceCounter; on POSIX,
//     clock_gettime(CLOCK_MONOTONIC). Neither is rdtsc, deliberately:
//     raw TSC reads are not serialising, migrate between cores, and on
//     a laptop will happily report a frequency the CPU never ran at.
//     rdtsc with fences and calibration belongs in a dedicated
//     benchmarking discussion, not smuggled into a helper.
//  3. Zero configuration, no admin rights, works on a WSL dev box and
//     a rented metal instance alike.
//
// Everything here is a wall-clock measurement, not a cycle count. The
// committed benchmark numbers are nanoseconds, and a cycle count on an
// unknown-frequency CPU would not be nanoseconds.

#pragma once

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ctime>
#endif

#include <cstdint>

namespace hft::util {

class Timer final {
public:
#if defined(_WIN32)
    Timer() : freq_(static_cast<double>(frequency())) {
        start();
    }

    void start() noexcept { origin_ = now(); }

    [[nodiscard]] std::uint64_t elapsed_ns() const noexcept {
        return to_ns(now() - origin_);
    }

    /// Frequency in ticks per second, needed to convert QPC ticks to
    /// nanoseconds.
    [[nodiscard]] static double frequency() noexcept {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }

    [[nodiscard]] static std::uint64_t now() noexcept {
        LARGE_INTEGER t;
        ::QueryPerformanceCounter(&t);
        return static_cast<std::uint64_t>(t.QuadPart);
    }

private:
    [[nodiscard]] std::uint64_t to_ns(std::uint64_t ticks) const noexcept {
        return static_cast<std::uint64_t>((static_cast<double>(ticks) * 1e9) / freq_);
    }

    double freq_ = 1.0;
    std::uint64_t origin_ = 0;
#else
    Timer() { start(); }

    void start() noexcept { origin_ = now(); }

    [[nodiscard]] std::uint64_t elapsed_ns() const noexcept { return now() - origin_; }

    [[nodiscard]] static std::uint64_t now() noexcept {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }
#endif
};

}  // namespace hft::util
