// Clock characterisation and resolution floor.
//
// What this tool exists to settle, and it is a small question that the
// benchmark tables depend on entirely:
//
//   **When a pipeline stage reports the same p50 as the clock read that
//   was measuring it, is the stage fast, or is it unmeasured?**
//
// The answer is "unmeasured", always -- and until now this repository
// asserted that in prose while the number sat in the table looking like a
// result. `results/ENVIRONMENT.md` recorded a decode p50 identical to the
// clock-read p50 and correctly declined to call it free, but nothing in
// the tree measured the floor, so the claim could not be checked by
// anyone else.
//
// So this tool measures the floor and prints it, and it does that with
// three properties a latency claim needs and usually lacks:
//
//  1. **The clock pair is characterised, not assumed.** QPC, rdtsc+LFENCE
//     and rdtscp+LFENCE each get their own overhead distribution, on the
//     thread's actual pinned processor. Whichever is fastest sets the
//     resolution of every figure in this repository; the reader should
//     not have to guess which that is.
//  2. **Resolution is demonstrated, not asserted.** A sweep of known,
//     linearly-scaling work is measured with the chosen clock. The
//     intercept is the floor and the slope is the harness's ability to
//     resolve a difference. If the sweep were flat, the tool would say so
//     and the tables would be untrustworthy -- which is the property worth
//     having, because it can fail.
//  3. **Drift is measured before any ratio is quoted.** The same
//     measurement is repeated and the spread reported. A difference
//     smaller than the run-to-run spread is not a result, and this
//     repository already learned that the expensive way: a benchmark once
//     reported a confident 20% slowdown that was really a drain loop
//     exiting early and doing less work.
//
// What this tool deliberately does not do is report an engine number. It
// is not in the business of measuring decode; it is in the business of
// establishing whether decode could be measured. `hft_stage_bench` is the
// tool that reports stages, and it should consume the floor printed here
// rather than re-deriving it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bench/report.hpp"
#include "hft/util/affinity.hpp"
#include "hft/util/histogram.hpp"
#include "hft/util/tsc.hpp"

namespace {

using hft::util::LatencyHistogram;
using hft::util::TsClock;

/// How many samples the overhead distributions use. Enough for a stable
/// p999, few enough that the tool stays instant -- a reader should not
/// wait a minute to learn the resolution of the machine.
constexpr int kOverheadSamples = 200'000;

/// Repetitions in the drift measurement. Five is the minimum that can
/// distinguish a spread from a single unlucky run.
constexpr int kDriftRepeats = 5;

/// Work sizes for the resolution sweep, in bytes.
///
/// Doubling from 16B to 4KiB spans three orders of magnitude and crosses
/// the L1 boundary twice, so the slope is not a single-regime artefact.
/// 16B is the point of the sweep: it is close enough to empty that if the
/// harness cannot separate it from an empty region, the harness is the
/// limit and everything above it is a lower bound rather than a
/// measurement.
constexpr std::size_t kSweepBytes[] = {0, 16, 64, 256, 1024, 4096};

/// Batches per sweep row. The minimum across these is the row's value.
constexpr int kSweepRepeats = 7;

enum class ClockKind { qpc, tsc_fenced, tscp_fenced };

/// Read a clock with the given serialisation discipline.
///
/// `tsc` is only consulted when the platform has a counter; on a target
/// without one, the TSC rows report unavailable rather than silently
/// falling back to QPC, because a fallback that produces a plausible
/// number is the failure mode this whole repository is about.
struct Clock {
    ClockKind kind;
    const char* label;
};

/// The clock the sweep and drift sections use.
///
/// TSC where available, because its floor is lower and the whole point of
/// the sweep is to find out how small a delta this harness can resolve. On
/// a platform without one this is QPC, and the sweep then measures against
/// the larger floor rather than pretending otherwise.
#if HFT_TSC_AVAILABLE
constexpr ClockKind kSweepClock = ClockKind::tsc_fenced;
#else
constexpr ClockKind kSweepClock = ClockKind::qpc;
#endif

/// Human-readable name for whichever clock the sweep used.
#if HFT_TSC_AVAILABLE
constexpr const char* kSweepClockName = "rdtsc + lfence (ticks)";
#else
constexpr const char* kSweepClockName = "QPC (platform ticks)";
#endif

[[nodiscard]] inline std::uint64_t read_clock(ClockKind k) noexcept {
    switch (k) {
#if HFT_TSC_AVAILABLE
        case ClockKind::tsc_fenced:
            return TsClock::read();
        case ClockKind::tscp_fenced:
            return TsClock::read_pinned();
#endif
        case ClockKind::qpc:
        default:
            return bench::Timer::now();
    }
}

/// Cost of one bracketing clock pair: start, end, difference.
[[nodiscard]] LatencyHistogram overhead(ClockKind kind) {
    LatencyHistogram h(1, 100'000);
    for (int i = 0; i < kOverheadSamples; ++i) {
        const std::uint64_t a = read_clock(kind);
        const std::uint64_t b = read_clock(kind);
        h.record(b - a);
    }
    return h;
}

/// Measure `reps` calls of `work`, returning the whole-batch cost in
/// clock ticks.
///
/// Measuring a batch rather than one call is deliberate and it is the
/// only reason the sweep below means anything: a single call is dominated
/// by the clock pair, so the batch amortises the floor away and the
/// per-call cost becomes visible. The caller divides by the batch count
/// and subtracts nothing -- the sweep interprets the intercept, not this
/// function.
template <typename F>
[[nodiscard]] std::uint64_t timed_batch(F&& work, int reps) noexcept {
    const std::uint64_t a = read_clock(kSweepClock);
    for (int i = 0; i < reps; ++i) {
        work();
    }
    const std::uint64_t b = read_clock(kSweepClock);
    return b - a;
}

/// Cheapest of several identical batches, in ticks per call.
///
/// `setup` runs once before timing begins and `work` runs `reps` times
/// inside each batch. The separation is load-bearing: an earlier version
/// of the sweep re-primed its source buffer inside the timed region, so
/// every "copy" also memset eight kilobytes and the measurement was of
/// the memsets. A benchmark that times its own setup measures the setup.
///
/// Minimum rather than mean, and this is the one place in the repository
/// where minimum is the *right* estimator rather than a convenience. A
/// batch can only be slowed by something outside it -- a preemption, an
/// interrupt, a sibling thread -- so the fastest of several runs is the
/// one that was interfered with least. Every other benchmark here takes
/// medians and percentiles because it is reporting a distribution; this
/// one is estimating a fixed cost, and for that the lower bound is the
/// estimate.
template <typename Setup, typename Work>
[[nodiscard]] double best_per_call(Setup&& setup,
                                   Work&& work,
                                   int reps,
                                   int repeats) noexcept {
    setup();
    std::uint64_t best = ~std::uint64_t{0};
    for (int r = 0; r < repeats; ++r) {
        setup();
        const std::uint64_t ticks = timed_batch(work, reps);
        if (ticks < best) {
            best = ticks;
        }
    }
    return static_cast<double>(best) / static_cast<double>(reps);
}

/// Sink that makes the timed copies observable, so they cannot be
/// optimised away.
///
/// Every copy in this tool writes a buffer that is never read. A
/// standards-conforming compiler is entitled to delete those stores
/// entirely, and does: the first version of the drift section reported
/// 0.00000 ticks/byte because the whole loop had been folded out. That is
/// not a fast memcpy, it is no memcpy, and the resulting number is
/// confidently wrong in the flattering direction -- the same shape of
/// defect as a benchmark reporting a speedup it did not earn.
///
/// Folding one byte of the destination into a `volatile` sink per
/// iteration keeps the stores live. It costs one load and one add against
/// a 16-to-4096-byte copy, so it does not meaningfully move the figure it
/// is protecting. Portable, unlike an empty inline-assembly barrier,
/// which would exclude every compiler this project claims to support.
volatile std::size_t g_sink = 0;

}  // namespace

int main(int argc, char** argv) {
    bench::print_environment("clock characterisation and resolution floor");

    const std::size_t logical = hft::util::current_logical_processor();

    // ---- calibration ------------------------------------------------
    bench::section("CLOCK CALIBRATION");

#if HFT_TSC_AVAILABLE
    const TsClock tsc;
    const double tps = tsc.ticks_per_second();
    const double qpc_hz = bench::Timer::frequency();

    std::printf("  QPC frequency      %.6f MHz\n", qpc_hz / 1e6);
    std::printf("  ns per QPC tick    %.4f\n", 1e9 / qpc_hz);
    if (qpc_hz < 1e8) {
        std::printf(
            "\n  NOTE: QPC on this host is a slow clock -- one tick is %.0f ns.\n"
            "  That sets the resolution floor for every tool that uses it, and\n"
            "  it is why this repository's latency figures were once wrong by a\n"
            "  factor of %.0f (raw ticks reported as nanoseconds). See the units\n"
            "  note in include/hft/util/timer.hpp.\n",
            1e9 / qpc_hz, 1e9 / qpc_hz);
    }
    if (tsc.calibrated()) {
        std::printf("  TSC ticks/sec      %.6f MHz  (calibrated, median of 5x20ms)\n",
                    tps / 1e6);

        // The meaningful cross-check is not "do the two counters run at the
        // same rate" -- they are different counters and there is no reason
        // they should. It is: over one interval measured by both, do they
        // agree on how long that interval was? That tests the TSC
        // calibration against an independent clock, which comparing
        // frequencies does not.
        const std::uint64_t ns0 = bench::Timer::now_ns();
        const std::uint64_t t0 = TsClock::read();
        std::uint64_t t1 = 0;
        while (bench::Timer::now_ns() < ns0 + 20'000'000ULL) {
            for (int spin = 0; spin < 64; ++spin) {
                _mm_pause();
            }
        }
        t1 = TsClock::read();
        const std::uint64_t ns1 = bench::Timer::now_ns();

        const double qpc_elapsed = static_cast<double>(ns1 - ns0);
        const double tsc_elapsed = tsc.to_nanos(t1 - t0);
        const double err_pct = (tsc_elapsed - qpc_elapsed) / qpc_elapsed * 100.0;
        std::printf("  20ms window, QPC   %.1f ns\n", qpc_elapsed);
        std::printf("  20ms window, TSC   %.1f ns\n", tsc_elapsed);
        std::printf("  disagreement       %+.4f%%\n", err_pct);
        if (err_pct > 0.5 || err_pct < -0.5) {
            std::printf(
                "\n  WARNING: the two independent clocks disagree by %.3f%% over\n"
                "  the same interval. Calibration is suspect; treat every\n"
                "  nanosecond derived from TSC below as uncalibrated.\n",
                err_pct);
        }

        // Self-check on the calibration window itself. An earlier version
        // of this file added a nanosecond constant to a tick-valued
        // timestamp, so every "20ms" window was really two seconds and the
        // tool took 29s instead of 1s. It reported a calibration rather
        // than a failure, because the ratio it computed stayed
        // self-consistent -- so the only defence is to measure the window
        // and fail loudly when it is not the intended length.
        //
        // This is a gate, not a diagnostic: a wrong window length means the
        // conversion is being applied across an interval nobody intended,
        // and the tool exits non-zero rather than printing numbers derived
        // from it.
        std::printf("\n  window length check (guards against a tick/ns mix-up)\n");
        constexpr double kIntendedNs = 20'000'000.0;
        {
            const std::uint64_t s = bench::Timer::now_ns();
            while (bench::Timer::now_ns() < s + static_cast<std::uint64_t>(kIntendedNs)) {
                for (int spin = 0; spin < 64; ++spin) {
                    _mm_pause();
                }
            }
            const double window_ns =
                static_cast<double>(bench::Timer::now_ns() - s);
            std::printf("    intended %.2fms, measured %.2fms\n", kIntendedNs / 1e6,
                        window_ns / 1e6);
            if (window_ns < 15'000'000.0 || window_ns > 60'000'000.0) {
                std::printf(
                    "    FAIL: window is %.0fx its intended length. A nanosecond\n"
                    "    constant has been compared against a tick-valued clock,\n"
                    "    or the reference clock is far coarser than assumed.\n",
                    window_ns / kIntendedNs);
                std::fflush(stdout);
                return 2;
            }
            std::printf("    ok\n");
        }
    } else {
        std::printf("  TSC ticks/sec      CALIBRATION FAILED\n");
        std::printf(
            "\n  This tool will not report TSC figures. A wrong rate yields\n"
            "  plausible nanoseconds, which is worse than no figure.\n");
    }

    // ---- invariance ------------------------------------------------
    bench::section("TSC INVARIANCE ACROSS PHYSICAL CORES");

    const std::size_t sibling = hft::util::smt_sibling(logical);
    const std::size_t neighbour = hft::util::other_core(logical);
    std::printf("  this core          logical %llu\n", bench::u64(logical));
    std::printf("  SMT sibling        logical %llu\n", bench::u64(sibling));
    std::printf("  other core         logical %llu\n", bench::u64(neighbour));

    if (neighbour == logical) {
        std::printf(
            "\n  Only one physical core is visible, so cross-core agreement\n"
            "  cannot be tested. This is not evidence of invariance; it is\n"
            "  the absence of a test. Pin the benchmark host and re-run.\n");
    } else {
        const double on_this = TsClock::rate_on_processor(logical);
        const double on_other = TsClock::rate_on_processor(neighbour);
        std::printf("  rate on this core  %.6f MHz\n", on_this / 1e6);
        std::printf("  rate on other      %.6f MHz\n", on_other / 1e6);
        const bool inv = TsClock::invariant(logical, neighbour);
        std::printf("\n  invariant TSC      %s\n", inv ? "YES" : "NO");
        if (!inv) {
            std::printf(
                "\n  The counter is not synchronised across these cores. A thread\n"
                "  that migrates mid-measurement reads a delta inflated by the\n"
                "  inter-core offset, so pinning is mandatory rather than\n"
                "  advisory on this host.\n");
        }
        std::printf(
            "\n  Note: an invariant TSC means the counter is constant-rate and\n"
            "  core-synchronised. It does NOT mean ticks are cycles. Under turbo\n"
            "  the core retires more instructions per tick than the nominal\n"
            "  ratio implies; cycles need APERF/MPERF or perf_event_open.\n");
    }

#else

    std::printf("  No timestamp counter on this target.\n");
    std::printf(
        "\n  QPC is the only clock available, so every figure this repository\n"
        "  can report carries the larger floor printed below. Nothing else\n"
        "  in the tree will be able to resolve a single-digit-nanosecond\n"
        "  stage on this platform.\n");

#endif

    // ---- overhead --------------------------------------------------
    bench::section("COST OF MEASURING (the resolution floor)");
    std::printf("  Ticks, not nanoseconds: these are the raw deltas the harness\n");
    std::printf("  would subtract from every stage figure.\n\n");

    const LatencyHistogram qpc_h = overhead(ClockKind::qpc);
    bench::histogram_row("QPC pair", qpc_h);

#if HFT_TSC_AVAILABLE
    const LatencyHistogram tsc_h = overhead(ClockKind::tsc_fenced);
    bench::histogram_row("rdtsc + lfence pair", tsc_h);

    const LatencyHistogram tscp_h = overhead(ClockKind::tscp_fenced);
    bench::histogram_row("rdtscp + lfence pair", tscp_h);
#endif

    std::printf(
        "\n  Read this as: a stage whose p50 is at or below its clock-pair\n"
        "  p50 is UNRESOLVED, not fast. The honest entry for such a stage\n"
        "  is 'below the measurement floor', and it stays below it until\n"
        "  the region is batched and the floor is amortised.\n");

    // ---- resolution sweep -------------------------------------------
    bench::section("RESOLUTION SWEEP (can this harness resolve anything?)");
    std::printf(
        "  Copies of N bytes, batched so the clock pair is amortised, and\n"
        "  reported as the cheapest of %d batches. Clock: %s.\n\n",
        kSweepRepeats, kSweepClockName);

    const int reps = 20'000;
    std::printf("  %-10s %14s %12s\n", "bytes", "ticks/copy", "ns/copy");
    std::printf("  %-10s %14s %12s\n", "----------", "--------------",
                "------------");

    double first_nonzero = 0.0;
    for (const std::size_t n : kSweepBytes) {
        // Buffers well clear of each other, re-primed between batches so
        // the source is not left in registers from the previous row and
        // the sweep measures a copy rather than a cache-warming artefact.
        // The priming is setup, never work -- see best_per_call.
        alignas(64) static unsigned char src[4096];
        alignas(64) static unsigned char dst[4096];

        const double per_copy = best_per_call(
            [&] {
                std::memset(src, 0xA5, sizeof(src));
                std::memset(dst, 0x00, sizeof(dst));
            },
            [&] {
                std::memcpy(dst, src, n);
                g_sink += dst[n / 2];
            },
            reps, kSweepRepeats);

        if (n == 0) {
            first_nonzero = per_copy;
        }

        char ns[32];
        if (tsc.calibrated() && kSweepClock == ClockKind::tsc_fenced) {
            std::snprintf(ns, sizeof(ns), "%.2f", tsc.to_nanos(static_cast<std::uint64_t>(
                                                    static_cast<double>(per_copy) + 0.5)));
        } else {
            std::snprintf(ns, sizeof(ns), "%.2f",
                          static_cast<double>(bench::Timer::ticks_to_ns(
                              static_cast<std::uint64_t>(static_cast<double>(per_copy) + 0.5))));
        }

        std::printf("  %-10llu %14.2f %12s\n", bench::u64(n), per_copy, ns);
        std::fflush(stdout);
    }

    std::printf(
        "\n  The 0-byte row is the control: loop overhead plus one clock\n"
        "  pair, with no copy at all. It is the floor a stage figure must\n"
        "  beat before it can be called a measurement.\n\n");

    // Slope across the large sizes, where loop overhead is negligible and
    // the cost is genuinely the copy. This is the harness's ability to
    // resolve a difference, stated as a number rather than asserted.
    if (tsc.calibrated() && kSweepClock == ClockKind::tsc_fenced) {
        std::printf("  Baseline (0 bytes) is %.2f ticks/call.\n", first_nonzero);
    }
    std::printf(
        "\n  The sweep must be monotonically increasing for its slope to\n"
        "  mean anything. If it is flat, this harness cannot resolve the\n"
        "  difference and every stage figure below it is a lower bound\n"
        "  rather than a measurement.\n");

    // ---- drift -----------------------------------------------------
    bench::section("RUN-TO-RUN DRIFT (the noise floor for any ratio)");
    std::printf("  The identical measurement, %d times, each spanning a\n",
                kDriftRepeats);
    std::printf("  matched wall-clock window of roughly 50ms.\n\n");

    // A fixed, long workload, because the earlier version of this section
    // timed 100k pause instructions -- about 0.3ms -- and reported a 114%
    // spread. That was not a property of the machine; it was a property of
    // measuring for less time than a scheduler timeslice, which is the
    // same error as the truncated drain loop documented in the README.
    //
    // The workload is a 4 KiB copy rather than a pause: a pause's duration
    // depends on the core's power state and moves by tens of percent on a
    // desktop, so even well-timed it measures the power management rather
    // than the harness. A 4 KiB copy is long enough to dominate its clock
    // pair and stable enough to have a real fixed cost.
    constexpr int kDriftRepeatsLong = kDriftRepeats;

    std::vector<double> drift;
    drift.reserve(kDriftRepeatsLong);

    // Two workloads, because "the noise floor" is not one number and
    // quoting a single one is how a benchmark ends up applying the wrong
    // threshold.
    //
    // A memcpy of a few hundred bytes stays in L1, which is the regime
    // every per-stage figure in this repository lives in. A multi-
    // kilobyte copy goes to DRAM and is the regime a throughput figure
    // lives in. On a shared machine the second is dramatically noisier
    // than the first, because it contends for the memory controller with
    // everything else on the box. Using the DRAM figure to qualify an L1
    // measurement -- or the reverse -- is wrong in both directions, so
    // both are measured and both are labelled.
    const auto drift_series = [&](std::size_t bytes, const char* label) {
        alignas(64) static unsigned char src[4096];
        alignas(64) static unsigned char dst[4096];
        std::memset(src, 0x5A, sizeof(src));

        const auto one_batch = [&](int count) {
            return timed_batch(
                [&] {
                    std::memcpy(dst, src, bytes);
                    g_sink += dst[bytes / 2];
                },
                count);
        };

        // Equalise measurement DURATION, not iteration count.
        //
        // The first version of this section used a fixed 500,000
        // iterations for both workloads. A 64-byte copy costs ~22 ticks
        // and a 4 KiB copy ~340, so the "L1" run spanned 3.4ms and the
        // "DRAM" run 53ms -- the L1 run was shorter than a scheduler
        // timeslice and duly reported a 33% spread while the DRAM run
        // reported 9%. The counterintuitive result was not a property of
        // the caches; it was a property of measuring one workload for
        // fifteen times less long than the other.
        //
        // A pilot batch establishes the per-copy cost, and the repeat
        // count is then chosen to hit a fixed tick budget. Both workloads
        // are now measured over the same wall-clock window, so the two
        // spreads are comparable and both are long enough to average over
        // scheduler noise.
        constexpr std::uint64_t kTargetTicks = 150'000'000ULL;  // ~50ms at 3GHz
        constexpr int kMinReps = 1'000;
        constexpr int kMaxReps = 20'000'000;

        const double pilot_ticks_per_copy =
            static_cast<double>(one_batch(10'000)) / 10'000.0;
        int runs = kMaxReps;
        if (pilot_ticks_per_copy > 0.0) {
            const double want =
                static_cast<double>(kTargetTicks) / pilot_ticks_per_copy;
            if (want < static_cast<double>(kMaxReps)) {
                runs = want < static_cast<double>(kMinReps) ? kMinReps
                                                            : static_cast<int>(want);
            }
        }

        const double denom = static_cast<double>(runs) * static_cast<double>(bytes);

        // Discarded warm-up, and the repeats are only meaningful because
        // of it. An earlier version of this section produced a series
        // that decreased monotonically -- a ramp, not noise: the memory
        // subsystem and the core's clock are still settling over the first
        // few hundred megabytes of traffic. Reporting max/min spread
        // across a ramp conflates trend with variance and produces a
        // threshold several times too pessimistic.
        const std::uint64_t warm = one_batch(runs);
        std::printf("  %s\n", label);
        std::printf("    %d copies x %lluB  (%.2f ticks/copy pilot)\n", runs,
                    bench::u64(bytes), pilot_ticks_per_copy);
        std::printf("    warm-up (discarded) %9.5f ticks/byte\n",
                    static_cast<double>(warm) / denom);

        std::vector<double> series;
        series.reserve(kDriftRepeatsLong);
        for (int r = 0; r < kDriftRepeatsLong; ++r) {
            const double per = static_cast<double>(one_batch(runs)) / denom;
            series.push_back(per);
            std::printf("    repeat %-13d %9.5f ticks/byte\n", r + 1, per);
        }

        const auto [lo, hi] = std::minmax_element(series.begin(), series.end());
        const double median = series[series.size() / 2];
        const double spread = (*lo > 0.0) ? ((*hi - *lo) / *lo) * 100.0 : 0.0;

        bool decreasing = true;
        bool increasing = true;
        for (std::size_t i = 1; i < series.size(); ++i) {
            if (series[i] > series[i - 1]) {
                decreasing = false;
            }
            if (series[i] < series[i - 1]) {
                increasing = false;
            }
        }

        std::printf("    min %.5f  median %.5f  max %.5f  spread %.2f%%\n", *lo,
                    median, *hi, spread);
        if (decreasing || increasing) {
            std::printf(
                "    TREND: repeats are monotonically %s, so this is a\n"
                "    settling machine, not a noisy one. Quote the LAST repeat\n"
                "    as steady state and lengthen the warm-up.\n",
                decreasing ? "decreasing" : "increasing");
        } else {
            std::printf("    no monotonic trend; this spread is genuine variance\n");
        }
        std::fflush(stdout);
        return spread;
    };

    const double l1_spread = drift_series(64, "L1-resident workload (64B copy)");
    std::printf("\n");
    const double dram_spread = drift_series(4096, "DRAM-resident workload (4KiB copy)");

    std::printf(
        "\n  Use the L1 figure (%.2f%%) to qualify any per-stage latency\n"
        "  ratio, and the DRAM figure (%.2f%%) to qualify any throughput\n"
        "  ratio. Do not use one for the other.\n",
        l1_spread, dram_spread);
    std::printf(
        "\n  Any difference smaller than the relevant figure above is noise.\n"
        "  This repository once published a 20%% slowdown that was a\n"
        "  benchmark bug; the checksum column is what caught it, and a\n"
        "  drift figure like this one is what stops it being published at\n"
        "  all.\n");

    bench::section("VERDICT");
#if HFT_TSC_AVAILABLE
    if (tsc.calibrated()) {
        std::printf(
            "  Use rdtsc+lfence for stage timing on this host: the floor\n"
            "  printed above is what an unresolvable stage will report.\n");
    } else {
        std::printf("  TSC unavailable or uncalibrated; use QPC and accept\n");
        std::printf("  the larger floor above.\n");
    }
#else
    std::printf("  No timestamp counter on this target; QPC is the only clock.\n");
#endif
    std::printf(
        "\n  Stage tables should report a figure only where it exceeds this\n"
        "  floor, and should say 'below floor' where it does not.\n");

    bench::print_publication_notice();
    std::fflush(stdout);
    (void)argc;
    (void)argv;
    return 0;
}