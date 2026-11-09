// Adverse selection measurement.
//
// This is the part of a market maker that is hard and that most
// implementations get wrong, usually in one of two ways: by measuring
// only the spread they quoted rather than the spread they got, and by
// forgetting that a strategy which consistently fills is losing money
// to informed flow.
//
// The three quantities, and the relationship between them:
//
//   effective spread = 2 * sign * (fill_price - mid_at_fill)
//   markout(h)       =      sign * (mid_at(t+h) - mid_at_fill)
//   realised spread  = 2 * sign * (fill_price - mid_at(t+h))
//
// and therefore, exactly:
//
//   realised = effective - 2 * markout
//
// `sign` is +1 for a buy and -1 for a sell. The effective spread is a
// cost: positive means you paid more than the mid. The markout is a
// gain: positive means the price moved your way after you traded. The
// realised spread is what you actually kept, and it is the only one of
// the three that tells you whether the strategy makes money.
//
// A high effective spread with a deeply negative markout is the
// classic failure: the strategy looks profitable on every trade it
// reports and is losing money, because the trades it reports are the
// ones where it was filled by someone who knew better.
//
// **These metrics require the future.** Markout cannot be computed
// until the horizon has elapsed, and it must be computed against what
// actually happened, not what was forecast. That makes this an
// offline measurement, valid in a replay or a backtest, and invalid as
// a live signal. A live strategy cannot know its own markout. Anything
// claiming to do so in real time is measuring something else.

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "hft/strategy/quoting.hpp"
#include "hft/types.hpp"
#include "hft/util/histogram.hpp"

namespace hft::strategy {

/// A fill that has happened but whose horizon has not yet elapsed.
struct PendingFill {
    Side side = Side::bid;
    Price price{};
    Quantity size{};
    Price mid_at_fill{};
    std::int64_t quoted_spread_raw = 0;
    Nanos timestamp = 0;
};

struct Stats {
    std::uint64_t fills = 0;
    std::uint64_t resolved = 0;
    /// Fills that moved immediately against us by more than
    /// `toxicity_threshold_raw`.
    std::uint64_t toxic = 0;

    /// Sum of shares filled, and the signed net, for inventory
    /// reporting.
    std::uint64_t bid_shares = 0;
    std::uint64_t ask_shares = 0;

    /// Sum of raw price units, for means. Percentile detail lives in
    /// the histograms.
    std::int64_t effective_spread_sum = 0;
    std::int64_t realised_spread_sum = 0;
    std::int64_t markout_sum = 0;
    std::int64_t quoted_spread_sum = 0;

    [[nodiscard]] std::int64_t net_shares() const noexcept {
        return static_cast<std::int64_t>(bid_shares) - static_cast<std::int64_t>(ask_shares);
    }

    [[nodiscard]] double mean_effective_spread() const noexcept {
        return resolved == 0 ? 0.0
                             : static_cast<double>(effective_spread_sum) /
                                   static_cast<double>(resolved);
    }
    [[nodiscard]] double mean_realised_spread() const noexcept {
        return resolved == 0 ? 0.0
                             : static_cast<double>(realised_spread_sum) /
                                   static_cast<double>(resolved);
    }
    [[nodiscard]] double mean_markout() const noexcept {
        return resolved == 0 ? 0.0
                             : static_cast<double>(markout_sum) /
                                   static_cast<double>(resolved);
    }
    [[nodiscard]] double mean_quoted_spread() const noexcept {
        return resolved == 0 ? 0.0
                             : static_cast<double>(quoted_spread_sum) /
                                   static_cast<double>(resolved);
    }
    /// Fraction of fills that were immediately adverse. The single most
    /// informative number here: a strategy with a 90% toxic fill rate
    /// is providing liquidity to informed flow, not earning a spread.
    [[nodiscard]] double toxicity_rate() const noexcept {
        return resolved == 0 ? 0.0
                             : static_cast<double>(toxic) / static_cast<double>(resolved);
    }

    /// Adverse selection cost per fill, in raw price units, positive
    /// when it is a cost.
    ///
    /// This is 2 * mean markout, and it is the number that matters for
    /// a market maker. A passive fill at the bid makes the EFFECTIVE
    /// spread negative by construction, because a bid is below the mid
    /// and the formula 2*sign*(fill - mid) is measuring exactly that.
    /// Reading a negative effective spread as "we beat the mid" is a
    /// misreading: the profit is in the round trip, not in the
    /// per-leg comparison against the mid. What actually costs money
    /// is the price moving against us after we are filled, and that is
    /// the markout.
    ///
    /// An earlier revision of this file reported a "realisation ratio"
    /// of realised to quoted spread. For a market maker that ratio is
    /// negative whenever fills are passive, which is always, so it
    /// carried no information and read as a finding.
    [[nodiscard]] double adverse_cost_per_fill() const noexcept {
        return 2.0 * mean_markout();
    }
};

/// Resolves fills against the mid as the replay advances.
class AdverseSelection final {
public:
    /// `horizon` is how far ahead a fill's markout is measured.
    /// `toxicity_threshold_raw` is how far the mid must move against a
    /// fill before it counts as toxic. Zero means any adverse tick.
    ///
    /// `bucket_raw` is a histogram bucket width in nanoseconds, so
    /// uint32_t rather than a general size_t: the histogram stores its
    /// widths in uint32_t, and letting a size_t reach the constructor
    /// would quietly truncate a wide bucket to a different one than the
    /// caller asked for.
    explicit AdverseSelection(Nanos horizon = 1'000'000,  // 1ms
                              std::int64_t toxicity_threshold_raw = 0,
                              std::uint32_t bucket_raw = 1)
        : horizon_(horizon), toxicity_threshold_raw_(toxicity_threshold_raw),
          effective_(bucket_raw, 2'000'000), markout_(bucket_raw, 2'000'000),
          realised_(bucket_raw, 2'000'000) {}

    /// Record a fill. The mid and quoted spread are captured now,
    /// because both are unavailable later: the quoted spread is gone
    /// once the quote moves.
    void on_fill(Side side, Price price, Quantity size, Price mid_at_fill,
                 std::int64_t quoted_spread_raw, Nanos now) noexcept {
        pending_.push_back(PendingFill{side, price, size, mid_at_fill, quoted_spread_raw, now});
        ++stats_.fills;
        if (side == Side::bid) {
            stats_.bid_shares += size.raw();
        } else {
            stats_.ask_shares += size.raw();
        }
    }

    /// Advance the replay, resolving every fill whose horizon has
    /// elapsed. Called once per observation with the current mid.
    ///
    /// Ordering matters: this must be called before recording new
    /// fills, or a fill is resolved against a mid that includes its own
    /// impact. A strategy that marks itself out at the instant it
    /// trades is measuring its own footprint, not adverse selection.
    void advance(Price mid, Nanos now) noexcept {
        while (!pending_.empty() && now >= pending_.front().timestamp + horizon_) {
            resolve(pending_.front(), mid);
            pending_.pop_front();
        }
    }

    /// Resolve everything still pending at the current mid. Used at end
    /// of replay so a short run does not silently discard its last
    /// fills. These resolutions have a shorter horizon than the rest
    /// and are counted, because excluding them biases the sample
    /// toward the beginning of the run.
    void flush(Price mid) noexcept {
        while (!pending_.empty()) {
            resolve(pending_.front(), mid);
            pending_.pop_front();
        }
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }
    [[nodiscard]] Nanos horizon() const noexcept { return horizon_; }

    // Distribution detail, in raw price units.
    [[nodiscard]] const util::LatencyHistogram& effective_histogram() const noexcept {
        return effective_;
    }
    [[nodiscard]] const util::LatencyHistogram& markout_histogram() const noexcept {
        return markout_;
    }
    [[nodiscard]] const util::LatencyHistogram& realised_histogram() const noexcept {
        return realised_;
    }

private:
    void resolve(const PendingFill& f, Price mid) noexcept {
        const std::int64_t sign = f.side == Side::bid ? 1 : -1;

        const std::int64_t effective = 2 * sign * (f.price.raw() - f.mid_at_fill.raw());
        const std::int64_t markout = sign * (mid.raw() - f.mid_at_fill.raw());
        const std::int64_t realised = effective - 2 * markout;

        stats_.resolved += 1;
        stats_.effective_spread_sum += effective;
        stats_.markout_sum += markout;
        stats_.realised_spread_sum += realised;
        stats_.quoted_spread_sum += f.quoted_spread_raw;

        if (markout < -toxicity_threshold_raw_) {
            ++stats_.toxic;
        }

        record(effective_, effective);
        record(markout_, markout);
        record(realised_, realised);
    }

    /// Histograms take a magnitude, so the sign is kept in the sums and
    /// the distributions show how large the moves were rather than
    /// which way they went. Signed detail lives in the means.
    static void record(util::LatencyHistogram& histogram, std::int64_t value) noexcept {
        const std::uint64_t magnitude =
            static_cast<std::uint64_t>(value < 0 ? -value : value);
        histogram.record(magnitude);
    }

    Nanos horizon_ = 1'000'000;
    std::int64_t toxicity_threshold_raw_ = 0;
    std::deque<PendingFill> pending_;
    Stats stats_{};
    util::LatencyHistogram effective_;
    util::LatencyHistogram markout_;
    util::LatencyHistogram realised_;
};

/// Queue-position-aware fill model.
///
/// The symmetric fill model in market_maker.cpp fills any quote
/// the mid touches, as if it were first in queue and instant to
/// reach the venue. Real fills require two things that model
/// omits: (1) the shares ahead of you in the queue at your price
/// must be consumed/cancelled first, and (2) your quote takes
/// `latency_ns` to reach the book. This class tracks both. It is
/// deliberately deterministic: volume consumed at your price
/// reduces the queue ahead share-for-share (price-time priority
/// means nothing jumps the queue), and eligibility is a pure
/// function of time since placement. No randomness, so a backtest
/// that disagrees between runs has a bug, not noise.
class FillModel final {
public:
    explicit FillModel(Nanos latency_ns = 0) noexcept : latency_ns_(latency_ns) {}

    struct Placement {
        Side side = Side::bid;
        Price price{};
        Quantity size{};
        std::uint64_t ahead = 0;  // shares ahead at placement
        Nanos placed_at = 0;
        std::uint64_t filled = 0;
    };

    /// Place a quote with `ahead_shares` visibly ahead of it at
    /// the same price. Returns the queue position (shares ahead).
    [[nodiscard]] std::uint64_t place(Placement& p, std::uint64_t ahead_shares,
                                      Nanos now) noexcept {
        p.ahead = ahead_shares;
        p.placed_at = now;
        p.filled = 0;
        resting_ = &p;
        return p.ahead;
    }

    /// Volume traded at the quote's price: consumes the queue
    /// ahead first, then fills the resting quote. Returns shares
    /// filled on this observation.
    std::uint64_t on_trade_through(Price price, std::uint64_t shares, Nanos now) noexcept {
        if (resting_ == nullptr || price != resting_->price || !eligible(now)) {
            return 0;
        }
        const std::uint64_t consumed = shares < resting_->ahead ? shares : resting_->ahead;
        resting_->ahead -= consumed;
        shares -= consumed;
        const std::uint64_t fillable =
            shares < remaining(resting_) ? shares : remaining(resting_);
        resting_->filled += fillable;
        return fillable;
    }

    /// Cancellations ahead of the quote shrink the queue.
    void on_cancel_ahead(std::uint64_t shares) noexcept {
        if (resting_ == nullptr) {
            return;
        }
        resting_->ahead = shares < resting_->ahead ? resting_->ahead - shares : 0;
    }

    [[nodiscard]] bool eligible(const Placement& p, Nanos now) const noexcept {
        return now >= p.placed_at + latency_ns_ && p.ahead == 0;
    }
    [[nodiscard]] bool eligible(Nanos now) const noexcept {
        return resting_ != nullptr && eligible(*resting_, now);
    }
    [[nodiscard]] static std::uint64_t remaining(const Placement* p) noexcept {
        return p == nullptr ? 0 : p->size.raw() - p->filled;
    }
    [[nodiscard]] Nanos latency() const noexcept { return latency_ns_; }

private:
    Nanos latency_ns_;
    Placement* resting_ = nullptr;
};

/// PnL accounting for a market maker.
///
/// Cash-and-position bookkeeping, marked to market on every observation
/// so the equity curve reflects inventory risk and not just realised
/// fills. A strategy that reports only realised PnL on a book it
/// constantly re-quotes can look profitable while accumulating an
/// inventory it cannot exit.
class PnlTracker final {
public:
    void on_fill(Side side, Price price, Quantity size) noexcept {
        const std::int64_t sign = side == Side::bid ? 1 : -1;
        const std::int64_t value = sign * price.raw() * static_cast<std::int64_t>(size.raw());
        cash_ -= value;
        position_ += sign * static_cast<std::int64_t>(size.raw());
        ++fills_;
    }

    /// Mark to market. Returns the equity change since the last call.
    std::int64_t mark(Price mid) noexcept {
        const std::int64_t equity = cash_ + position_ * mid.raw();
        const std::int64_t delta = equity - last_equity_;
        last_equity_ = equity;
        // The peak must be updated here or drawdown() is always zero.
        // It was, for a while, which made the metric report a
        // risk-free strategy.
        if (equity > peak_equity_) {
            peak_equity_ = equity;
        }
        pnl_sum_ += delta;
        ++marks_;
        if (delta > best_) {
            best_ = delta;
        }
        if (delta < worst_) {
            worst_ = delta;
        }
        return delta;
    }

    [[nodiscard]] std::int64_t cash() const noexcept { return cash_; }
    [[nodiscard]] std::int64_t position() const noexcept { return position_; }
    [[nodiscard]] std::int64_t equity() const noexcept { return last_equity_; }
    [[nodiscard]] std::int64_t total_pnl() const noexcept { return pnl_sum_; }
    [[nodiscard]] std::int64_t best_step() const noexcept { return best_; }
    [[nodiscard]] std::int64_t worst_step() const noexcept { return worst_; }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }

    /// Drawdown from the peak equity seen so far, in raw units.
    [[nodiscard]] std::int64_t drawdown() const noexcept {
        return peak_equity_ - last_equity_;
    }

    [[nodiscard]] double pnl_per_fill() const noexcept {
        return fills_ == 0 ? 0.0
                           : static_cast<double>(pnl_sum_) / static_cast<double>(fills_);
    }

private:
    std::int64_t cash_ = 0;
    std::int64_t position_ = 0;
    std::int64_t last_equity_ = 0;
    std::int64_t peak_equity_ = 0;
    std::int64_t pnl_sum_ = 0;
    std::int64_t best_ = 0;
    std::int64_t worst_ = 0;
    std::uint64_t fills_ = 0;
    std::uint64_t marks_ = 0;
};

}  // namespace hft::strategy
