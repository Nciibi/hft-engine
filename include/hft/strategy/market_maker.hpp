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
            // Never quote better than the touch. See the long note
            // below: the model prices around the mid, the book is not
            // centred on it, and an unclamped quote sits inside the
            // spread, which is a crossing order rather than a resting
            // one.
            const Price bid = quote.bid > *best_bid ? quote.bid : *best_bid;
            const Price ask = quote.ask < *best_ask ? quote.ask : *best_ask;

            const std::int64_t position = pnl_.position();

            if (bid < ask) {
                // Both sides placeable. Respect the inventory limit on
                // each side independently, so a long position stops
                // being added to but can still be worked down.
                bid_live_ = position < config_.max_inventory;
                ask_live_ = position > -config_.max_inventory;
                quoted_spread_raw_ = ask.raw() - bid.raw();
            } else {
                // The model wants to trade THROUGH the book: with any
                // real inventory the reservation shift exceeds the
                // book's own spread, so the clamped bid ends up above
                // the clamped ask.
                //
                // Emitting that is not a strategy, it is a crossed
                // quote. The only sensible action is to keep the side
                // that REDUCES the position and drop the other. With no
                // inventory there is no reason to prefer either side,
                // so quote nothing rather than guess.
                if (position > 0) {
                    bid_live_ = false;
                    ask_live_ = true;
                } else if (position < 0) {
                    bid_live_ = true;
                    ask_live_ = false;
                } else {
                    bid_live_ = false;
                    ask_live_ = false;
                }
                // Normalise the reported spread to what it would be if
                // both sides were quoted, so the metric stays
                // comparable between one-sided and two-sided quoting.
                //
                // A MAGNITUDE, not a signed distance. With a position
                // on one side the model's reservation shifts past the
                // mid, and `mid - bid` comes out negative; reporting
                // that as a "spread" produces a -121,786 effective
                // spread and a realisation ratio of zero, which reads
                // as a finding when it is an arithmetic slip.
                const std::int64_t distance =
                    bid_live_ ? mid.raw() - bid.raw() : ask.raw() - mid.raw();
                quoted_spread_raw_ = 2 * (distance < 0 ? -distance : distance);
            }

            resting_bid_ = bid;
            resting_ask_ = ask;
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
            if (bid_live_ && best_bid->raw() <= resting_bid_.raw()) {
                fill(Side::bid, resting_bid_, mid, now);
            } else if (ask_live_ && best_ask->raw() >= resting_ask_.raw()) {
                fill(Side::ask, resting_ask_, mid, now);
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
    [[nodiscard]] std::uint64_t quotes() const noexcept { return quotes_; }
    [[nodiscard]] const MarketMakerConfig& config() const noexcept { return config_; }

private:
    void fill(Side side, Price price, Price mid, Nanos now) noexcept {
        const std::uint32_t size = config_.quote.base_size;
        pnl_.on_fill(side, price, Quantity::from_raw(size));
        adverse_.on_fill(side, price, Quantity::from_raw(size), mid, quoted_spread_raw_, now);
        ++fills_;
        // The quote is consumed: one resting order fills once, and
        // leaving it up would let a single quote fill repeatedly as the
        // mid oscillated across it.
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
};

}  // namespace hft::strategy
