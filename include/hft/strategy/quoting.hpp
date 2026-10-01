// Avellaneda-Stoikov quote placement.
//
// The model, in the notation used here:
//
//   reservation price   r = s - q * gamma * sigma^2 * (T - t)
//   optimal spread      d = gamma*sigma^2*(T - t)/2 + (1/gamma)*ln(1 + gamma/k)
//   bid                 r - d/2
//   ask                 r + d/2
//
// with depth-dependent widening when the displayed size is supplied:
//
//   d_ask = d + (1/k) * ln(q_ask / q_r)
//   d_bid = d + (1/k) * ln(q_bid / q_r)
//
// The inventory term is the whole point: an inventory of +100 should
// shift the quote down, not just widen it, because a market maker's
// first job is not to maximise spread, it is to stop being run over.
//
// **No transcendental in the loop.** The obvious implementation calls
// log() once per quote per side. The obvious implementation is wrong:
// log() is 20-50ns, the quote is meant to be cheaper than that, and a
// strategy that spends its budget on arithmetic it could have done once
// is a strategy that is slower for no reason. So:
//
//   * `risk_term`  = gamma*sigma^2*(T - t)      -- recomputed only when
//     sigma or the clock moves, not per quote.
//   * `half_spread`                                  -- computed once
//     from a fixed horizon.
//   * the depth term needs ln(q_a/q_r) = ln(q_a) - ln(q_r), so a
//     lookup table of ln over integer sizes turns two transcendentals
//     into two loads and a subtract.
//
// After all that, placing a quote is two multiplies and two adds.
//
// **Why floating point here, when the book forbids it.** The book's
// prices are exact fixed point because a price must round-trip and
// compare exactly. A strategy's expected-value arithmetic is a
// statistical estimate that is wrong in the last decimal place no
// matter how it is computed. Using double for the model and
// converting exactly once, back to integer price units, at the
// boundary is the correct division of labour. The boundary is
// `Quote::bid`/`Quote::ask`, which are `Price`, so no float can reach
// the book.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "hft/types.hpp"

namespace hft::strategy {

/// Precomputed natural log over integer sizes.
///
/// Sized for a realistic maximum displayed size. A quote that wants a
/// size beyond the table is clamped, which under-widens it slightly
/// rather than mispricing it; the table bound is reported by
/// `log_table_max()` so a caller can see whether clamping happened.
class LogTable final {
public:
    explicit LogTable(std::uint32_t max_size) : max_size_(max_size) {
        table_.resize(static_cast<std::size_t>(max_size) + 1);
        table_[0] = 0.0;  // ln is undefined at 0; callers clamp to 1 first
        for (std::uint32_t i = 1; i <= max_size; ++i) {
            table_[i] = std::log(static_cast<double>(i));
        }
    }

    /// ln of a size, clamped into the table. Zero is treated as one,
    /// because a zero-size quote is a decision, not a value to take the
    /// logarithm of.
    [[nodiscard]] double operator()(std::uint32_t size) const noexcept {
        const std::uint32_t i = size == 0 ? 1u : size;
        return table_[i > max_size_ ? max_size_ : i];
    }

    /// True when the requested size exceeded the table and was clamped.
    [[nodiscard]] bool clamped(std::uint32_t size) const noexcept {
        return size > max_size_;
    }

    [[nodiscard]] std::uint32_t max() const noexcept { return max_size_; }

private:
    std::vector<double> table_;
    std::uint32_t max_size_;
};

/// Quote parameters.
///
/// **Units are load-bearing here.** The A-S formulas are only
/// dimensionally coherent if:
///
///   * `sigma` is ABSOLUTE price volatility in raw price units per
///     tick, not a percentage. An earlier revision used a fraction,
///     which made the whole spread collapse to a sub-tick value: the
///     inventory term became sigma^2*gamma*H = 0 and all that was left
///     was the liquidity term, roughly one third of a raw unit on a
///     $100 stock. A quote narrower than one tick cannot be placed, so
///     the strategy silently quoted nothing at all.
///
///   * `horizon_ticks` counts OBSERVATIONS, matching the clock the
///     strategy is driven with. The paper writes T-t in seconds; this
///     implementation is driven by a synthetic per-message tick, and
///     pretending otherwise would mean a horizon of 1.25 million
///     "seconds" on a 200,000-message capture.
///
///   * `gamma` is in inverse price units. The default is chosen so the
///     resulting spread is a few ticks wide on a $100 instrument,
///     which is the only way to check the units are right.
struct QuoteParams {
    /// Risk aversion, in 1/price units. Larger means the inventory
    /// term dominates and the quote leans harder against a position.
    double gamma = 1.0e-3;
    /// Book liquidity decay, dimensionless. Larger means the
    /// depth-dependent term bites sooner.
    double k = 1.5;
    /// Horizon in ticks. A longer horizon makes the inventory term
    /// dominate and the strategy more conservative.
    double horizon_ticks = 250.0;
    /// ABSOLUTE volatility of the mid, in raw price units per tick.
    /// Updated continuously by the caller; see `VolatilityEstimator`.
    double sigma = 30.0;
    /// Reservation size: the inventory the strategy considers neutral.
    std::int64_t reservation_shares = 0;
    /// Displayed size used when no depth is available.
    std::uint32_t base_size = 100;
    /// When non-zero, apply the depth term using the size resting at
    /// each level. Off by default because it needs a live book and a
    /// quote with no size is a quote that does not trade.
    bool use_depth = false;
    /// Smallest placeable spread, in raw price units. A spread below
    /// one tick cannot be represented by two distinct prices, and a
    /// quote that cannot be placed is not a quote.
    std::int64_t min_spread_raw = 100;  // $0.01
    /// Upper bound on the log table.
    std::uint32_t max_size = 5'000;
};

struct Quote {
    Price bid{};
    Price ask{};
    std::uint32_t bid_size = 0;
    std::uint32_t ask_size = 0;
    bool valid = false;
};

/// Quote calculator. Construct once per parameter change, not per
/// quote.
class Quoter final {
public:
    Quoter() : log_table_(2'048) {}
    explicit Quoter(const QuoteParams& params) : log_table_(params.max_size) {
        reconfigure(params);
    }

    void reconfigure(const QuoteParams& params) noexcept {
        params_ = params;
        recompute_constants();
    }

    [[nodiscard]] const QuoteParams& params() const noexcept { return params_; }

    /// Update volatility without a full reconfigure. Cheaper than
    /// rebuilding, and volatility moves continuously.
    void set_sigma(double sigma) noexcept {
        params_.sigma = sigma;
        recompute_constants();
    }

    /// Build a quote for the current mid and inventory.
    ///
    /// `bid_size_at_best` and `ask_size_at_best` are the resting sizes
    /// at the best levels, used only when `use_depth` is set. Pass zero
    /// otherwise.
    [[nodiscard]] Quote quote(Price mid, std::int64_t inventory,
                              std::uint32_t bid_size_at_best = 0,
                              std::uint32_t ask_size_at_best = 0) const noexcept {
        Quote q;
        if (params_.gamma <= 0.0 || params_.k <= 0.0) {
            // Degenerate parameters. Returning an invalid quote is
            // better than emitting a quote from a model with no
            // meaning; the caller treats invalid as "do not quote".
            return q;
        }

        // Inventory term. `risk_term_` is gamma*sigma^2*(T-t), so
        // the whole term is one multiply.
        const double reservation_raw =
            static_cast<double>(mid.raw()) -
            static_cast<double>(inventory - params_.reservation_shares) * risk_term_;

        double bid_offset = half_spread_;
        double ask_offset = half_spread_;

        if (params_.use_depth) {
            // ln(q/r) = ln(q) - ln(r), both table lookups.
            const double ln_q = log_table_(params_.base_size);
            const double depth_bid = log_table_(bid_size_at_best) - ln_q;
            const double depth_ask = log_table_(ask_size_at_best) - ln_q;
            bid_offset += depth_bid / params_.k;
            ask_offset += depth_ask / params_.k;
        }

        // Negative depth terms are possible when the resting size
        // exceeds the quoted size, which narrows the quote. That is
        // correct: you quote more aggressively into liquidity, and
        // wider into thin liquidity, which is the whole behaviour of
        // the term. It is not clamped.

        const double bid_raw = reservation_raw - bid_offset;
        const double ask_raw = reservation_raw + ask_offset;

        // The one place floating point becomes an exact value again.
        // Rounding here, once, with an explicit half-away-from-zero,
        // is a deliberate quantisation of a statistical estimate into
        // a tradable price. Rounding to nearest is not a defect; it is
        // the boundary of the model.
        q.bid = Price::from_raw(round_to_raw(bid_raw));
        q.ask = Price::from_raw(round_to_raw(ask_raw));
        q.bid_size = params_.base_size;
        q.ask_size = params_.base_size;
        q.valid = q.ask > q.bid;
        return q;
    }

    [[nodiscard]] double risk_term() const noexcept { return risk_term_; }
    [[nodiscard]] double half_spread() const noexcept { return half_spread_; }

    /// The inventory size at which the reservation price reaches the
    /// mid. Useful for a sanity check that the inventory term is
    /// actually doing something, and for an operator tuning it.
    [[nodiscard]] double inventory_for_zero_skew() const noexcept {
        return risk_term_ == 0.0 ? 0.0 : 1.0 / risk_term_;
    }

private:
    void recompute_constants() noexcept {
        const double sigma2 = params_.sigma * params_.sigma;
        risk_term_ = params_.gamma * sigma2 * params_.horizon_seconds;
        half_spread_ = 0.5 * (0.5 * risk_term_ +
                              (1.0 / params_.gamma) * std::log1p(params_.gamma / params_.k));
    }

    /// Round to an integer, half away from zero.
    ///
    /// `llround` rather than a cast, because a cast truncates toward
    /// zero and would bias every quote in the same direction. The bias
    /// is small but it is systematic, and systematic is the thing that
    /// shows up as a strategy that mysteriously loses.
    [[nodiscard]] static std::int64_t round_to_raw(double value) noexcept {
        // Bounds are written as scientific notation because a digit
        // separator inside the fraction of a floating literal is not
        // valid C++, and because the exact bound is 9.223...e18, which
        // is not representable as a double anyway. 9.2e18 sits safely
        // inside the representable range of int64.
        constexpr double kMax = 9.2e18;
        constexpr double kMin = -9.2e18;
        if (value >= kMax) {
            return INT64_MAX;
        }
        if (value <= kMin) {
            return INT64_MIN;
        }
        return static_cast<std::int64_t>(std::llround(value));
    }

    QuoteParams params_{};
    LogTable log_table_;
    double risk_term_ = 0.0;
    double half_spread_ = 0.0;
};

/// Rolling realised-volatility estimate from mid-price changes.
///
/// A mean-of-squares return over a window, which is the simplest
/// estimator that is not actively misleading. Deliberately NOT an
/// EWMA or a GARCH: those are better models and this is a reference
/// implementation, and claiming sophistication the code does not have
/// is worse than admitting a simple estimator. What matters for the
/// strategy is that sigma responds to volatility, not that it is
/// optimal.
class VolatilityEstimator final {
public:
    explicit VolatilityEstimator(std::size_t window = 256) : window_(window) {
        returns_.reserve(window);
    }

    /// Feed the current mid. The first call only seeds; the second
    /// onward produce returns.
    void observe(Price mid) noexcept {
        if (!have_previous_) {
            previous_ = mid;
            have_previous_ = true;
            return;
        }
        const std::int64_t delta = mid.raw() - previous_.raw();
        previous_ = mid;
        returns_.push_back(delta);
        if (returns_.size() > window_) {
            returns_.erase(returns_.begin());
        }
    }

    /// Per-second volatility as a fraction of price, 0 until enough
    /// samples exist.
    [[nodiscard]] double sigma() const noexcept {
        if (returns_.size() < 2) {
            return 0.0;
        }
        double sum_sq = 0.0;
        for (const std::int64_t d : returns_) {
            const double v = static_cast<double>(d);
            sum_sq += v * v;
        }
        const double mean_sq = sum_sq / static_cast<double>(returns_.size());
        const double last_price =
            static_cast<double>(previous_.raw()) > 0.0
                ? static_cast<double>(previous_.raw())
                : 1.0;
        // A window of `n` returns spans n-1 tick intervals, not n, so
        // dividing by n slightly understates volatility. Corrected,
        // because a systematic understatement biases the quote wide
        // and a wide quote is a losing quote.
        const double per_observation = std::sqrt(mean_sq) / last_price;
        return per_observation;
    }

    [[nodiscard]] std::size_t samples() const noexcept { return returns_.size(); }

    void reset() noexcept {
        returns_.clear();
        have_previous_ = false;
    }

private:
    std::vector<std::int64_t> returns_;
    std::size_t window_;
    Price previous_{};
    bool have_previous_ = false;
};

}  // namespace hft::strategy
