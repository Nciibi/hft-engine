// Calibrated timestamp counter, and the reason it is calibrated.
//
// `timer.hpp` deliberately refuses to use rdtsc, and this file is the
// other half of that decision: rdtsc is the right tool for measuring
// nanosecond-scale work, but only once you have done four things to it
// that the raw instruction does not do for you.
//
//  1. Serialise it. RDTSC is not serialising. Without a fence the
//     compiler may hoist the read above the code being measured, and the
//     out-of-order engine may retire it early. The measured region then
//     reports a time it never occupied. LFENCE on both sides of the
//     region is the minimum that closes this on x86-64.
//  2. Calibrate it. The TSC counts at a fixed reference rate, not at the
//     core's actual clock. Under turbo, under a thermal cap, or simply
//     between P-states, one TSC tick is NOT one retired cycle and is not
//     one nanosecond. Every conversion in this file goes through a
//     measured ticks-per-second.
//  3. Pin it. On a CPU without an invariant TSC the counter is only
//     weakly synchronised across cores, so a thread that migrates
//     mid-measurement reads a delta that includes the inter-core offset.
//     `invariant()` tests for this rather than assuming it.
//  4. Decide whether it is invariant. `invariant()` reports the answer
//     for the host it runs on. On the machine this was developed on it is
//     true; that is a property of that CPU, not of this code.
//
// The consequence worth stating plainly: **on a modern x86-64 CPU with an
// invariant TSC, ticks are not cycles.** A core running at 4.2 GHz against
// a 3.2 GHz reference counter accumulates ticks at the 3.2 GHz rate.
// Converting a latency into "cycles" from a tick delta needs APERF/MPERF
// (MSR reads, so privileged or kernel-mediated) or `perf_event_open` with
// a cycles counter. This project reports nanoseconds and does not claim
// cycle counts, because the honest nanosecond is available everywhere and
// the honest cycle count is not.
//
// What this buys, and it is a real gain: the TSC keeps counting while the
// thread is descheduled. So the calibration below is immune to preemption
// -- both counters advance through it -- whereas a calibration built from
// `clock_gettime` deltas around a blocking sleep would be measuring the
// scheduler as much as the clock. That property is why this file exists
// next to a perfectly good portable timer.
//
// Not thread-safe and not intended to be: construction calibrates, and
// calibration is a multi-millisecond operation that belongs nowhere near
// a hot path. One instance per thread, constructed before measuring.

#pragma once

#if defined(_WIN32) || defined(__x86_64__) || defined(_M_X64)
#define HFT_TSC_AVAILABLE 1
#else
#define HFT_TSC_AVAILABLE 0
#endif

#if HFT_TSC_AVAILABLE
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>
#else
#include <x86intrin.h>
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hft/util/affinity.hpp"
#include "hft/util/timer.hpp"

namespace hft::util {

/// A calibrated timestamp counter.
///
/// Ticks become nanoseconds through a measured rate, never through an
/// assumed nominal frequency. On any CPU whose actual clock differs from
/// its reference rate -- most of them, most of the time -- that
/// distinction is the whole ballgame.
class TsClock final {
public:
    /// True when this build can read a timestamp counter at all.
    [[nodiscard]] static constexpr bool available() noexcept {
#if HFT_TSC_AVAILABLE
        return true;
#else
        return false;
#endif
    }

    /// Measure the tick rate against the monotonic clock.
    ///
    /// Median of several samples rather than one. The TSC advances through
    /// a preemption so the ratio itself is preserved, but a window that
    /// spans a clock adjustment is not, and a median keeps one
    /// pathological window from moving the answer. Median rather than mean
    /// for the usual reason: the tail must not move the central estimate.
    ///
    /// Multi-millisecond by construction. Call once per thread, off any
    /// measured path.
    TsClock() {
#if HFT_TSC_AVAILABLE
        ticks_per_second_ = calibrate();
#endif
    }

#if HFT_TSC_AVAILABLE

    /// Current tick count, serialised against everything around it.
    ///
    /// The trailing LFENCE is what makes the *end* of a measured region
    /// mean "this region has actually finished" rather than "the counter
    /// was read somewhere near here".
    [[nodiscard]] static std::uint64_t read() noexcept {
        _mm_lfence();
        const std::uint64_t t = __rdtsc();
        _mm_lfence();
        return t;
    }

    /// Current tick count, also waiting for prior instructions to retire.
    ///
    /// RDTSCP does not return until every instruction issued before it has
    /// completed, so the *start* of a measured region is trustworthy
    /// without a preceding fence -- which is why the bracketing pair is
    /// RDTSCP/LFENCE rather than two fences around RDTSC when the aux
    /// value is wanted. It also returns TSC_AUX, which encodes the
    /// processor it ran on: the portable way to detect that a measurement
    /// straddled a migration.
    ///
    /// On a Linux host TSC_AUX is architecturally defined as the CPU
    /// number. On Windows it is not, and is documented only as an opaque
    /// value -- it is usable as an equality test between two samples and
    /// must not be decoded. `same_processor()` therefore compares it for
    /// equality rather than interpreting it, which is the only portable
    /// use.
    [[nodiscard]] static std::uint64_t read_pinned(std::uint32_t* aux = nullptr) noexcept {
        std::uint32_t a = 0;
        const std::uint64_t t = __rdtscp(&a);
        _mm_lfence();
        if (aux != nullptr) {
            *aux = a;
        }
        return t;
    }

    /// True when two TSC_AUX values came from the same logical processor.
    ///
    /// Equality, not decoding: the field's meaning is architecture-defined
    /// on Linux and undefined on Windows, so the only claim that survives
    /// both is "it did not change".
    [[nodiscard]] static bool same_processor(std::uint32_t a,
                                             std::uint32_t b) noexcept {
        return a == b;
    }

    /// Measured tick rate, in ticks per second. Zero if calibration failed.
    [[nodiscard]] double ticks_per_second() const noexcept { return ticks_per_second_; }

    [[nodiscard]] bool calibrated() const noexcept { return ticks_per_second_ > 0.0; }

    [[nodiscard]] std::uint64_t to_ticks(std::uint64_t nanos) const noexcept {
        if (ticks_per_second_ <= 0.0) {
            return 0;
        }
        return static_cast<std::uint64_t>(
            static_cast<double>(nanos) * ticks_per_second_ / 1e9);
    }

    [[nodiscard]] double to_nanos(std::uint64_t ticks) const noexcept {
        if (ticks_per_second_ <= 0.0) {
            return 0.0;
        }
        return static_cast<double>(ticks) * 1e9 / ticks_per_second_;
    }

    /// Does the counter run at the same rate on both logical processors?
    ///
    /// The property whose absence makes a migrating thread's reading
    /// meaningless, and the reason pinning is not optional for a TSC
    /// measurement even when the OS promises to migrate rarely.
    ///
    /// The caller must pass processors on different *physical* cores: a
    /// sample from an SMT sibling shares its counter with the one already
    /// sampled and would agree no matter what the answer should be. On a
    /// single-physical-core host this returns true, because there is only
    /// one counter and the question does not arise.
    [[nodiscard]] static bool invariant(std::size_t this_logical,
                                        std::size_t other_logical,
                                        double tolerance = 0.005) noexcept {
        const TsClock expect;
        if (!expect.calibrated()) {
            return false;
        }
        const double target = expect.ticks_per_second();

        const double a = rate_on_processor(this_logical);
        if (a <= 0.0) {
            return false;
        }
        if (other_logical == this_logical ||
            shares_physical_core(this_logical, other_logical)) {
            // Only one physical core is available to compare. Report true
            // rather than a false negative from comparing a core with
            // itself, which would read as "not invariant".
            return true;
        }
        const double b = rate_on_processor(other_logical);
        if (b <= 0.0) {
            return false;
        }
        return std::fabs(a - b) / target <= tolerance;
    }

    /// Tick rate observed from `logical`, in ticks per second.
    ///
    /// Public because a benchmark that reports its clock's calibration
    /// should be able to show the per-processor samples too, not only the
    /// verdict.
    [[nodiscard]] static double rate_on_processor(std::size_t logical) noexcept {
        if (!pin_current_thread(logical)) {
            return 0.0;
        }
        const std::uint64_t ns0 = Timer::now_ns();
        const std::uint64_t t0 = read();
        std::uint64_t t1 = 0;
        wait_until_ns(ns0 + kCalibrationWindowNs, t1);
        const std::uint64_t dt = t1 - t0;
        const std::uint64_t dr_ns = Timer::now_ns() - ns0;
        if (dt == 0 || dr_ns == 0) {
            return 0.0;
        }
        return static_cast<double>(dt) / static_cast<double>(dr_ns) * 1e9;
    }

private:
    /// Long enough that the monotonic clock's resolution is not a material
    /// fraction of the window, short enough that calibration does not
    /// visibly delay startup.
    static constexpr std::uint64_t kCalibrationWindowNs = 20'000'000ULL;

    [[nodiscard]] static double calibrate() noexcept {
        constexpr int kSamples = 5;
        std::vector<double> rates;
        rates.reserve(kSamples);

        for (int i = 0; i < kSamples; ++i) {
            // Every reference-clock value in this function is captured in
            // NANOSECONDS via `now_ns()`, never in raw ticks. That is not
            // a style choice.
            //
            // An earlier version of this code took `r0 = Timer::now()` --
            // platform ticks -- and compared it against
            // `deadline = r0 + kCalibrationWindowNs`, adding a nanosecond
            // constant to a tick count. On a host whose QPC runs at 10MHz
            // that made every "20ms" calibration window two seconds long,
            // and the tool took 29 seconds instead of one. It reported a
            // calibration rather than an error, because the ratio it
            // computed was still self-consistent; it was just measured over
            // a hundred times the intended interval.
            //
            // This is the third instance of the same defect in this
            // repository, after the benchmarks that recorded raw QPC ticks
            // into nanosecond histograms. The rule that prevents it is
            // therefore mechanical rather than careful: a value read from
            // the reference clock is converted at the point of capture, and
            // no tick value is ever compared against a nanosecond constant.
            const std::uint64_t ns0 = Timer::now_ns();
            const std::uint64_t t0 = read();
            std::uint64_t t1 = 0;
            wait_until_ns(ns0 + kCalibrationWindowNs, t1);
            const std::uint64_t ns1 = Timer::now_ns();

            const std::uint64_t dt = t1 - t0;
            const std::uint64_t dr_ns = ns1 - ns0;
            if (dt == 0 || dr_ns == 0) {
                continue;
            }
            rates.push_back(static_cast<double>(dt) / static_cast<double>(dr_ns) * 1e9);
        }

        if (rates.empty()) {
            // Calibration failed outright. Returning 0 rather than a
            // nominal guess: a wrong ticks-per-second yields plausible
            // nanoseconds, and plausible wrong nanoseconds are strictly
            // worse than an obviously broken measurement. `calibrated()`
            // exists so the caller can tell the two apart.
            return 0.0;
        }
        std::sort(rates.begin(), rates.end());
        return rates[rates.size() / 2];
    }

    /// Spin until the reference clock passes `deadline_ns`, returning the
    /// TSC at that moment.
    ///
    /// A spin, not a sleep. A blocking sleep would hand the interval to
    /// the scheduler; busy-waiting keeps the core's clock high so a short
    /// window is not measured at a low P-state, and it removes the
    /// scheduler from the measurement entirely.
    ///
    /// The deadline is in nanoseconds and the poll is in nanoseconds,
    /// because `now_ns()` is the only reference-clock accessor used here.
    static void wait_until_ns(std::uint64_t deadline_ns,
                              std::uint64_t& out_tsc) noexcept {
        for (;;) {
            out_tsc = read();
            if (Timer::now_ns() >= deadline_ns) {
                return;
            }
            for (int spin = 0; spin < 64; ++spin) {
                _mm_pause();
            }
        }
    }

    double ticks_per_second_ = 0.0;
#else
    [[nodiscard]] double ticks_per_second() const noexcept { return 0.0; }
    [[nodiscard]] bool calibrated() const noexcept { return false; }
    [[nodiscard]] std::uint64_t to_ticks(std::uint64_t) const noexcept { return 0; }
    [[nodiscard]] double to_nanos(std::uint64_t) const noexcept { return 0.0; }
#endif
};

}  // namespace hft::util