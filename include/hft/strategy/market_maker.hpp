// Market maker simulation against a replayed book.
//
// The fill model, stated up front because it dominates every number
// that comes out.
//
// **This is a backtest, and a simple one.** A resting quote is filled
// when the replayed mid trades at or through it, after the quote has
// been resting for at least one observation. There is no queue
// position, no size at level, no latency between quoting and being
// hit, and no fill on touch without a subsequent adverse move.
//
// Every one of those omissions flatters the strategy. In a real book
// you are behind the exchange in a queue, you pay a round trip to
// reach the market, and a "fill" you did not get is a spread you did
// not earn. A backtest that reports a profit here is reporting an
// upper bound, and the honest presentation is to say so rather than to
// quote the number without its assumptions.
//
// The reason to build it anyway is that the ADVERSE SELECTION metrics
// are robust to these omissions in a way PnL is not. Whether fills are
// systematically toxic does not depend on queue position. So the PnL
// here is illustrative and the toxicity rate is the finding.

#pragma once

#include <algorithm>
#include <cstdint>

#include "hft/lob/order_book.hpp"
#include "hft/strategy/metrics.hpp"
#include "hft/strategy/quoting.hpp"
#include "hft/types.hpp"

namespace hft::strategy {

/// Configuration for the simulated market maker.
struct MarketMakerConfig {
    QuoteParams quote{};
    /// Markout horizon in nanoseconds.
    Nanos markout_horizon = 1'000'000;  // 1ms
    /// A fill counts as toxic when the mid moves against it by at
    /// least this many raw price units past the horizon. Zero means
    /// any adverse tick.
    std::int64_t toxicity_threshold_raw = 0;
    /// Cap on position, in shares. The strategy stops quoting beyond
    /// it. A market maker with no inventory limit is not a market
    /// maker.
    std::int64_t max_inventory = 5'000;
    /// Only quote if the mid is known, and only quote inside this many
    /// raw units of it.
    std::int64_t min_edge_raw = 1;
};

/// The simulated market maker.
class MarketMaker final {
public:
    explicit MarketMaker(const MarketMakerConfig& config)
        : config_(config), quoter_(config.quote),
          adverse_(config.markout_horizon, config.toxicity_threshold_raw) {}

    /// One observation of the book. Call once per market data event.
    ///
    /// Order of operations is load-bearing:
    ///   1. advance the adverse-selection tracker, so a fill is never
    ///      marked out against a mid that includes its own impact;
    ///   2. read the new mid;
    ///   3. feed volatility;
    ///   4. decide and place quotes;
    ///   5. test the previous quote against the new mid for a fill.
    void on_book(const lob::OrderBook& book, Nanos now) noexcept {
        ++observations_;

        const auto best_bid = book.best_bid();
        const auto best_ask = book.best_ask();
        if (!best_bid.has_value() || !best_ask.has_value()) {
            // One-sided or empty. There is no mid to quote around, and
            // quoting a mid you synthesised from one side is how a
            // strategy ends up leaning on a side of the market that
            // does not exist.
            ++one_sided_;
            return;
        }
        ++two_sided_;

        const Price mid = Price::from_raw((best_bid->raw() + best_ask->raw()) / 2);
        last_mid_ = mid;

        // (1) Resolve outstanding markouts against the CURRENT mid,
        // before this observation's own fill is added.
        adverse_.advance(mid, now);
        pnl_.mark(mid);

        // (2) and (3) Volatility, then a fresh quote.
        vol_.observe(mid);
        quoter_.set_sigma(vol_.sigma());

        const Quote quote = quoter_.quote(mid, pnl_.position());
        if (quote.valid) {
            // A passive quote is at or OUTSIDE its own touch, and
            // never crosses.
            //
            //   bid = min(model_bid, best_bid)
            //   ask = max(model_ask, best_ask)
            //
            // Both clamps refuse to make the strategy more aggressive
            // than the market, which is the whole discipline of being a
            // maker rather than a taker. The earlier clamps were the
            // other way round (max with the bid, min with the ask),
            // which looks symmetric and is not: they let the ask fall
            // BELOW the bid whenever the model's inventory shift
            // exceeded the book's spread. That produced a crossed
            // quote that filled instantly at a terrible price and
            // reported a -$32,000 PnL with a 0.998 realisation ratio,
            // which is the most convincing kind of wrong.
            //
            // The model still does the deciding. When it wants a WIDER
            // quote than the touch offers, the clamp is inactive and
            // the widening happens. When it wants to be more
            // aggressive, the clamp refuses and the strategy joins the
            // queue instead. A long inventory therefore stops bidding
            // far away while continuing to offer, which is inventory
            // control expressed purely through quote placement.
            const Price bid = quote.bid < *best_bid ? quote.bid : *best_bid;
            const Price ask = quote.ask > *best_ask ? quote.ask : *best_ask;

            const std::int64_t position = pnl_.position();
            bid_live_ = position < config_.max_inventory;
            ask_live_ = position > -config_.max_inventory;

            resting_bid_ = bid;
            resting_ask_ = ask;
            // The mid as of the moment this quote was placed. A fill on
            // this quote must be measured against THIS mid, not the
            // mid after the book moved to fill it.
            //
            // Using the post-move mid is the bug this comment exists to
            // prevent: it prices the fill against a mid that already
            // contains the adverse move, which makes every fill look
            // better than the mid and reports a NEGATIVE effective
            // spread. A market maker cannot systematically buy below
            // the mid. Measuring against the post-move mid says it can.
            quote_mid_ = mid;
            quoted_spread_raw_ = ask.raw() - bid.raw();
            quote_valid_ = quoted_spread_raw_ >= config_.min_edge_raw && (bid_live_ || ask_live_);
            if (quote_valid_) {
                ++quotes_;
            }
        } else {
            quote_valid_ = false;
            bid_live_ = false;
            ask_live_ = false;
        }

        // (4) Test the PREVIOUS quote against the new book. A quote
        // placed on this observation cannot fill on it: it did not
        // exist when this touch was formed.
        //
        // Fill detection watches the TOUCH, not the mid. The mid of a
        // book that is 13 ticks wide is a heavily smoothed number: it
        // almost never moves a full tick in one direction, so testing
        // against it produced a strategy that quoted every tick and
        // filled never. A resting bid is hit when the market trades
        // DOWN to its level, which shows up as the best bid moving
        // through it.
        if (quote_active_) {
            ++fill_checks_;
            if (bid_live_) ++bid_checks_;
            if (ask_live_) ++ask_checks_;
            if (bid_live_ && best_bid->raw() <= resting_bid_.raw()) {
                fill(Side::bid, resting_bid_, now);
            } else if (ask_live_ && best_ask->raw() >= resting_ask_.raw()) {
                fill(Side::ask, resting_ask_, now);
            }
        }

        // (5) The current quote becomes the resting one for the next
        // observation.
        quote_active_ = quote_valid_;
    }

    /// Resolve outstanding markouts at the final mid.
    void finish(Price final_mid) noexcept {
        adverse_.flush(final_mid);
        pnl_.mark(final_mid);
    }

    [[nodiscard]] const AdverseSelection& adverse() const noexcept { return adverse_; }
    [[nodiscard]] const PnlTracker& pnl() const noexcept { return pnl_; }
    [[nodiscard]] const VolatilityEstimator& volatility() const noexcept { return vol_; }
    [[nodiscard]] const Quoter& quoter() const noexcept { return quoter_; }
    [[nodiscard]] std::uint64_t observations() const noexcept { return observations_; }
    /// Observations where both sides had a price. A strategy that
    /// spends most of its time here being unable to quote is a
    /// strategy whose feed is wrong, and this is how you find out.
    [[nodiscard]] std::uint64_t two_sided_observations() const noexcept { return two_sided_; }
    [[nodiscard]] std::uint64_t one_sided_observations() const noexcept { return one_sided_; }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
    /// Fill-path diagnostics. If `checks` is large and `fills` is
    /// small, the quote is resting but the condition is not being met,
    /// which points at the fill model rather than at the strategy.
    [[nodiscard]] std::uint64_t fill_checks() const noexcept { return fill_checks_; }
    [[nodiscard]] std::uint64_t bid_side_checks() const noexcept { return bid_checks_; }
    [[nodiscard]] std::uint64_t ask_side_checks() const noexcept { return ask_checks_; }
    [[nodiscard]] std::uint64_t bid_hits() const noexcept { return bid_hits_; }
    [[nodiscard]] std::uint64_t ask_hits() const noexcept { return ask_hits_; }
    [[nodiscard]] std::uint64_t quotes() const noexcept { return quotes_; }

    /// The quote currently resting, for diagnostics. Exposed because
    /// "why did the strategy never fill" is a question that needs the
    /// answer to be observable, and guessing at it from the outside
    /// wasted several iterations.
    [[nodiscard]] Price resting_bid() const noexcept { return resting_bid_; }
    [[nodiscard]] Price resting_ask() const noexcept { return resting_ask_; }
    [[nodiscard]] std::int64_t position() const noexcept { return pnl_.position(); }
    [[nodiscard]] Price last_mid() const noexcept { return last_mid_; }
    [[nodiscard]] bool quote_active() const noexcept { return quote_active_; }
    [[nodiscard]] std::int64_t quoted_spread_raw() const noexcept { return quoted_spread_raw_; }
    [[nodiscard]] const MarketMakerConfig& config() const noexcept { return config_; }

private:
    void fill(Side side, Price price, Nanos now) noexcept {
        const std::uint32_t size = config_.quote.base_size;
        pnl_.on_fill(side, price, Quantity::from_raw(size));
        adverse_.on_fill(side, price, Quantity::from_raw(size), quote_mid_, quoted_spread_raw_,
                         now);
        ++fills_;
        if (side == Side::bid) {
            ++bid_hits_;
        } else {
            ++ask_hits_;
        }
        const std::int64_t position = pnl_.position();
        const std::int64_t magnitude = position < 0 ? -position : position;
        if (magnitude > max_position_observed_) {
            max_position_observed_ = magnitude;
        }
        // The quote is consumed: one resting order fills once, and
        // leaving it up would let a single quote fill repeatedly as the
        // market oscillated across it.
        quote_active_ = false;
    }

    MarketMakerConfig config_;
    Quoter quoter_;
    VolatilityEstimator vol_{};
    AdverseSelection adverse_;
    PnlTracker pnl_{};

    Price resting_bid_{};
    Price resting_ask_{};
    Price last_mid_{};
    std::int64_t quoted_spread_raw_ = 0;
    bool quote_valid_ = false;
    bool quote_active_ = false;
    bool bid_live_ = true;
    bool ask_live_ = true;
    std::uint64_t observations_ = 0;
    std::uint64_t two_sided_ = 0;
    std::uint64_t one_sided_ = 0;
    std::uint64_t quotes_ = 0;
    std::uint64_t fills_ = 0;
    std::uint64_t fill_checks_ = 0;
    std::uint64_t bid_checks_ = 0;
    std::uint64_t ask_checks_ = 0;
    std::uint64_t bid_hits_ = 0;
    std::uint64_t ask_hits_ = 0;`n    std::int64_t max_position_observed_ = 0;
};

}  // namespace hft::strategy
