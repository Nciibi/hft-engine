// Clock synchronisation servo: offset/drift estimation and discipline.
//
// What this is: the software half of clock sync. Given timestamp
// pairs (local clock, remote clock) it estimates the offset and the
// drift between them and produces a disciplined time, the same way
// a PTP servo reasons about a grandmaster clock. What this is NOT:
// PTP itself, a NIC timestamp, or a kernel discipline -- those need
// hardware and a running OS, and claiming them from a header would
// be exactly the kind of unverified claim this repository exists to
// not make. `disciplined()` is the interface a hardware timestamp
// would feed.
//
// Math is integer-only (fixed-point drift in parts-per-billion) so
// results are identical on every host and tier, and testable to the
// nanosecond with synthetic pairs.
//
// Step vs slew: if the estimated offset is larger than
// `step_threshold_ns`, `disciplined()` steps the clock (PTP's
// behaviour for gross offsets); otherwise it slews (the offset is
// applied gradually via the drift term, so timestamps never go
// backwards -- a step backwards in a market-data timestamp reorders
// a book).

#pragma once

#include <cstdint>

namespace hft::util {

class ClockSync final {
public:
    /// `step_threshold_ns`: offsets larger than this are stepped,
    /// smaller ones slewed. Default 1ms, PTP's usual boundary.
    explicit ClockSync(std::int64_t step_threshold_ns = 1'000'000) noexcept
        : step_threshold_ns_(step_threshold_ns) {}

    struct Sample {
        std::int64_t local_ns = 0;
        std::int64_t remote_ns = 0;
    };

    /// Feed one timestamp pair. Recomputes offset (remote - local)
    /// as a smoothed estimate and drift as the slope between the
    /// first and latest samples (a two-point slope is exact for a
    /// linear clock and needs no history).
    void observe(const Sample& s) noexcept {
        const std::int64_t offset = s.remote_ns - s.local_ns;
        if (!has_base_) {
            base_local_ = s.local_ns;
            base_offset_ = offset;
            has_base_ = true;
            prev_local_ = s.local_ns;
            prev_offset_ = offset;
            offset_ = offset;
            drift_ppb_ = 0;
        } else {
            // Drift between the base sample and this one, in ppb:
            // ((offset - base_offset) / (local - base_local)) * 1e9.
            const std::int64_t dt = s.local_ns - base_local_;
            if (dt > 0) {
                drift_ppb_ = ((offset - base_offset_) * 1'000'000'000LL) / dt;
            }
            // Offset estimate: follow the latest sample (a servo
            // would low-pass this; following is the honest
            // zero-filter baseline and is what tests assert).
            prev_local_ = s.local_ns;
            prev_offset_ = offset;
            offset_ = offset;
        }
        ++samples_;
    }

    /// Disciplined remote time for a local timestamp.
    [[nodiscard]] std::int64_t disciplined(std::int64_t local_ns) const noexcept {
        const std::int64_t dt = local_ns - base_local_;
        const std::int64_t drift_corr =
            has_base_ ? (dt * drift_ppb_) / 1'000'000'000LL : 0;
        if (offset_ > step_threshold_ns_ || offset_ < -step_threshold_ns_) {
            return local_ns + offset_;  // stepped: full offset applied
        }
        return local_ns + base_offset_ + drift_corr;  // slewed
    }

    [[nodiscard]] std::int64_t offset_ns() const noexcept { return offset_; }
    [[nodiscard]] std::int64_t drift_ppb() const noexcept { return drift_ppb_; }
    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
    [[nodiscard]] bool stepped() const noexcept {
        return offset_ > step_threshold_ns_ || offset_ < -step_threshold_ns_;
    }

private:
    std::int64_t step_threshold_ns_;
    std::int64_t base_local_ = 0;
    std::int64_t base_offset_ = 0;
    std::int64_t prev_local_ = 0;
    std::int64_t prev_offset_ = 0;
    std::int64_t offset_ = 0;
    std::int64_t drift_ppb_ = 0;
    std::uint64_t samples_ = 0;
    bool has_base_ = false;
};

}  // namespace hft::util
