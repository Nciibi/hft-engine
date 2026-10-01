// Bucketed latency histogram.
//
// Reports percentiles, not means. In a trading system the mean latency
// is close to irrelevant: an average of 40ns is identical whether 99% of
// messages take 20ns and 1% take 2us, or whether they are all 40ns.
// Only the tail determines whether you get filled, so the tail is what
// this records.
//
// Bucket layout is linear, one bucket per `bucket_width_ns`. That is
// honest and easy to reason about, and its limitation is stated rather
// than hidden: a wide bucket width smears the resolution of the p50,
// and a sample beyond `max_ns` lands in an overflow bucket. An
// HdrHistogram-style log-linear mapping is the right answer for
// production reporting and is a later change to this file alone, since
// the interface does not change.
//
// Not thread-safe. One histogram per thread.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hft::util {

class LatencyHistogram final {
public:
    LatencyHistogram(std::uint32_t bucket_width_ns = 1, std::uint32_t max_ns = 1'000'000)
        : bucket_width_ns_(bucket_width_ns == 0 ? 1u : bucket_width_ns) {
        bucket_count_ = (max_ns / bucket_width_ns_) + 1;
        counts_.assign(bucket_count_, 0);
    }

    /// Record a sample. Values beyond the configured range are counted
    /// separately rather than clamped into the last bucket, so a
    /// pathological outlier cannot masquerade as a p999.
    void record(std::uint64_t nanos) noexcept {
        ++count_;
        sum_ += nanos;
        if (min_ == 0 || nanos < min_) {
            min_ = nanos;
        }
        if (nanos > max_) {
            max_ = nanos;
        }
        if (nanos < static_cast<std::uint64_t>(bucket_count_) * bucket_width_ns_) {
            ++counts_[static_cast<std::size_t>(nanos / bucket_width_ns_)];
        } else {
            ++overflow_;
        }
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

    [[nodiscard]] double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    /// Nearest-rank percentile, returned in nanoseconds.
    ///
    /// Nearest-rank rather than interpolating: interpolation invents
    /// values that were never observed, which is the same class of lie
    /// as averaging a latency distribution. Returns 0 for an empty
    /// histogram, and the upper bound of the last bucket when the
    /// requested rank falls beyond the recorded range, so a p999 on a
    /// histogram with overflow is visibly at the ceiling instead of
    /// quietly reading low.
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        if (p <= 0.0) {
            return min_;
        }
        if (p >= 1.0) {
            return max_;
        }
        const std::uint64_t rank =
            static_cast<std::uint64_t>(static_cast<double>(count_) * p + 0.5);
        const std::uint64_t target = rank == 0 ? 1 : rank;

        std::uint64_t seen = 0;
        for (std::size_t b = 0; b < counts_.size(); ++b) {
            seen += counts_[b];
            if (seen >= target) {
                return static_cast<std::uint64_t>(b + 1) * bucket_width_ns_;
            }
        }
        // The requested rank lies beyond the recorded range. Report the
        // ceiling rather than a value we did not measure.
        return static_cast<std::uint64_t>(counts_.size()) * bucket_width_ns_;
    }

    void reset() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0u);
        count_ = 0;
        overflow_ = 0;
        sum_ = 0;
        min_ = 0;
        max_ = 0;
    }

private:
    std::vector<std::uint32_t> counts_;
    std::size_t bucket_count_ = 0;
    std::uint32_t bucket_width_ns_ = 1;
    std::uint64_t count_ = 0;
    std::uint64_t overflow_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = 0;
    std::uint64_t max_ = 0;
};

}  // namespace hft::util
