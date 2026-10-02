// Shared output helpers for the benchmark tools.
//
// The two concurrency benchmarks and `bench.cpp` all report the same
// kinds of number, and a latency table whose rows are formatted
// differently in different tools is a table nobody can read across. The
// formatting lives here so it is defined once.
//
// Two rules every tool in this repository follows, and which these
// helpers exist to make hard to break:
//
//  1. The cost of measuring is reported before the measurements. A
//     per-stage p50 that equals the clock-read p50 is not a fast stage,
//     it is an unresolvable one, and the reader has to be able to see
//     that without taking it on trust.
//  2. The host is described before the numbers. A latency figure
//     without the machine and the placement it came from is an anecdote.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "hft/util/affinity.hpp"
#include "hft/util/histogram.hpp"
#include "hft/util/timer.hpp"

namespace bench {

using hft::util::LatencyHistogram;
using hft::util::Timer;

/// Narrowing helpers for printf's `%llu` and `%llx`, so the cast appears
/// once instead of at every call site.
[[nodiscard]] inline unsigned long long u64(std::uint64_t v) noexcept {
    return static_cast<unsigned long long>(v);
}

[[nodiscard]] inline long long i64(std::int64_t v) noexcept {
    return static_cast<long long>(v);
}

/// Thousands-separated decimal, for counts a reader has to read rather
/// than compare.
///
/// The `i >= lead` guard is load-bearing. Without it the test
/// `(i - lead) % 3 == 0` evaluates `i - lead` in unsigned arithmetic for
/// every digit in the leading group, and the underflow lands on
/// `SIZE_MAX` -- which is divisible by three, because 2^64 is not. So
/// every two-digit number came out with a separator between its digits:
/// 23 mid moves printed as "2 3 mid moves", 10 levels as "1 0 levels",
/// while 1,471 and 1,999,998 printed correctly. The bug was invisible in
/// any number wide enough to have real groups and wrong in exactly the
/// small counts that appear most often in these tables.
[[nodiscard]] inline std::string humanize(std::uint64_t v) {
    const std::string digits = std::to_string(v);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && i >= lead && (i - lead) % 3 == 0) {
            out.push_back(' ');
        }
        out.push_back(digits[i]);
    }
    return out;
}

inline void section(const char* title) {
    std::printf("\n%s\n", title);
    std::size_t len = 0;
    while (title[len] != '\0') {
        ++len;
    }
    for (std::size_t i = 0; i < len; ++i) {
        std::printf("-");
    }
    std::printf("\n");
    // Explicit flush at every section boundary. Windows C runtimes do
    // not honour line buffering on redirected output, so a run that is
    // piped to a file -- or that hangs -- would otherwise lose every line
    // printed before the problem. A benchmark you cannot see into is a
    // benchmark you cannot debug.
    std::fflush(stdout);
}

inline void note(const char* text) {
    std::printf("\n%s\n", text);
    std::fflush(stdout);
}

/// One p50/p99/p999 row, in the format used across this repository.
///
/// Overflow is surfaced rather than absorbed. A histogram whose samples
/// exceeded its range is reporting a censored tail, and printing a
/// p999 from it without saying so would be a number that reads as
/// measured and is not.
inline void histogram_row(const char* label, const LatencyHistogram& h) {
    std::printf("  %-26s p50 %7.0f  p99 %7.0f  p999 %7.0f  max %8.0f  n %llu\n", label,
                static_cast<double>(h.percentile(0.50)),
                static_cast<double>(h.percentile(0.99)),
                static_cast<double>(h.percentile(0.999)),
                static_cast<double>(h.max()), u64(h.count()));
    if (h.overflow_count() != 0) {
        std::printf("  %-26s WARNING: %llu samples beyond the histogram range; p999 is censored\n",
                    "", u64(h.overflow_count()));
    }
    std::fflush(stdout);
}

/// Cost of a single `Timer::now()` pair.
///
/// Reported first, before anything else, because every per-stage figure
/// in these tools includes one clock pair. When this and a stage's p50
/// are the same number, the stage is below the resolution of the
/// measurement and the honest reading is "unmeasured", not "free".
inline LatencyHistogram measure_clock_overhead(int samples = 200'000) {
    LatencyHistogram hist(1, 100'000);
    for (int i = 0; i < samples; ++i) {
        const std::uint64_t a = Timer::now();
        const std::uint64_t b = Timer::now();
        hist.record(b - a);
    }
    return hist;
}

inline void print_clock_overhead(const LatencyHistogram& hist) {
    histogram_row("clock read", hist);
    std::printf("\n  Every per-stage figure below includes one clock pair.\n");
}

/// Environment banner. Printed before any number, never after.
///
/// Includes a standing warning that these are development-machine
/// figures. That is not false modesty: PLAN.md section 6 forbids
/// publishing numbers from consumer silicon, and a benchmark that
/// prints its own caveat is far harder to quote out of context than one
/// that does not.
inline void print_environment(const char* tool) {
    std::printf("HFT Engine - %s\n", tool);
    std::printf("=========================\n\n");
    std::printf("topology             %s\n", hft::util::describe_topology().c_str());
    std::printf("this thread          %s\n", hft::util::describe_affinity("main").c_str());
    std::printf("\n");
    std::fflush(stdout);
}

/// Warning printed at the end of any run on a machine that is not the
/// designated benchmark host.
inline void print_publication_notice() {
    std::printf(
        "\n"
        "NOTE: unless this run was made on the rented bare-metal benchmark\n"
        "host, these figures are from consumer silicon and are NOT for\n"
        "publication. See results/ENVIRONMENT.md. A latency table without\n"
        "a host specification is an anecdote.\n");
}

}  // namespace bench