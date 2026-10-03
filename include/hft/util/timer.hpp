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
//     rdtsc with fences and calibration is `tsc.hpp`, and it is a
//     separate, opt-in tool.
//  3. Zero configuration, no admin rights, works on a WSL dev box and
//     a rented metal instance alike.
//
// ---- UNITS, AND A BUG THIS FILE USED TO CONTAIN ------------------------
//
// `now()` returns the platform's native ticks, and on Windows those ticks
// are **not** nanoseconds. Every benchmark in this repository recorded
// `now()` deltas straight into a `LatencyHistogram` and printed the result
// in columns headed as nanoseconds, so every latency figure the tools
// printed was wrong by the ratio between the host's QPC frequency and
// 1 GHz.
//
// On the development host that ratio is 100: `QueryPerformanceFrequency`
// returns 10 MHz there, one tick is 100 ns, and a reported "decode p50 of
// 2" was 200 ns. Throughput was unaffected, because it went through
// `elapsed_ns()` and was converted -- which is exactly why the bug survived:
// the one number that was checked against an independent quantity was the
// one number that was right, and the unconverted ones had nothing to be
// checked against. A host whose QPC runs at the nominal TSC rate would
// have been off by a different factor, in the other direction.
//
// The fix is not to make `now()` return nanoseconds. That would add a
// floating-point multiply and divide to every clock read, which is a
// measurable fraction of the cost of reading the clock at all -- a tax on
// the instrument paid by every measurement. Instead `now()` stays raw and
// fast, and `ticks_to_ns()` converts. **Every delta must pass through
// `ticks_to_ns()` before it reaches a histogram.** A latency figure in
// ticks is not a latency figure; it is a number whose meaning depends on
// a machine property nobody wrote down.
//
// The conversion is exact rather than approximate for any plausible
// uptime: a double holds integers exactly to 2^53, and the absolute
// nanosecond count only exceeds that after roughly a hundred days of
// continuous uptime. Past that the conversion loses single-nanosecond
// resolution on the *absolute* value while remaining exact on deltas,
// which is why `elapsed_ns()` differences first and then converts.

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
    /// QPC frequency in ticks per second, queried once.
    ///
    /// An inline variable rather than a function-local static so the
    /// initialisation happens before `main` and every later call is a
    /// plain load with no thread-safe-static guard on the path. A guard
    /// branch inside a clock read would be a measurable cost paid on
    /// every sample.
    inline static const double kHz =
        [] {
            LARGE_INTEGER f;
            ::QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();

    Timer() : freq_(kHz) { start(); }

    void start() noexcept { origin_ = now(); }

    [[nodiscard]] std::uint64_t elapsed_ns() const noexcept {
        return to_ns(now() - origin_);
    }

    [[nodiscard]] static double frequency() noexcept { return kHz; }

    /// Convert a tick delta to nanoseconds.
    ///
    /// Public, and load-bearing: this is the conversion whose absence
    /// silently mislabelled every latency figure in this repository. See
    /// the units note at the top of this file.
    [[nodiscard]] static std::uint64_t ticks_to_ns(std::uint64_t ticks) noexcept {
        return to_ns(ticks);
    }

    /// Raw platform ticks. Convert with `ticks_to_ns` before recording.
    [[nodiscard]] static std::uint64_t now() noexcept {
        LARGE_INTEGER t;
        ::QueryPerformanceCounter(&t);
        return static_cast<std::uint64_t>(t.QuadPart);
    }

    /// Nanoseconds since an arbitrary origin, as a monotonic value.
    ///
    /// Equivalent to `ticks_to_ns(now())` and provided because a benchmark
    /// that captures several stage boundaries wants them all in one unit
    /// before it differences any of them. Differencing after conversion is
    /// exact; converting after differencing is also exact; converting once
    /// per boundary and never again is the version that cannot be
    /// forgotten.
    [[nodiscard]] static std::uint64_t now_ns() noexcept {
        return to_ns(now());
    }

private:
    [[nodiscard]] static std::uint64_t to_ns(std::uint64_t ticks) noexcept {
        return static_cast<std::uint64_t>((static_cast<double>(ticks) * 1e9) / kHz);
    }

    double freq_ = kHz;
    std::uint64_t origin_ = 0;
#else
    Timer() { start(); }

    void start() noexcept { origin_ = now(); }

    [[nodiscard]] std::uint64_t elapsed_ns() const noexcept { return now() - origin_; }

    /// Nanoseconds per tick is exactly 1 here, so the conversion is
    /// identity. It exists as a named function so calling code reads the
    /// same on both platforms and cannot drift into assuming the Windows
    /// behaviour.
    [[nodiscard]] static constexpr std::uint64_t ticks_to_ns(std::uint64_t ticks) noexcept {
        return ticks;
    }

    [[nodiscard]] static std::uint64_t now_ns() noexcept { return now(); }

    [[nodiscard]] static double frequency() noexcept { return 1e9; }

    /// Raw platform ticks, which on POSIX are already nanoseconds.
    [[nodiscard]] static std::uint64_t now() noexcept {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }
#endif
};

}  // namespace hft::util