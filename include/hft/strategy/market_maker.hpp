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
            return;
        }

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
            quoted_spread_raw_ = quote.ask.raw() - quote.bid.raw();
            resting_bid_ = quote.bid;
            resting_ask_ = quote.ask;
            // Suppress the side that would push the position further
            // out, while leaving the other side live so the position
            // can still be worked down. A market maker that stops
            // quoting entirely at its limit cannot reduce its
            // inventory, which is the opposite of what a limit is for.
            bid_live_ = pnl_.position() < config_.max_inventory;
            ask_live_ = pnl_.position() > -config_.max_inventory;
            const bool edge_ok = quoted_spread_raw_ >= config_.min_edge_raw;
            quote_valid_ = quote.ask > quote.bid && edge_ok && (bid_live_ || ask_live_);
            if (quote_valid_) {
                ++quotes_;
            }
        } else {
            quote_valid_ = false;
            bid_live_ = false;
            ask_live_ = false;
        }

        // (4) Test the PREVIOUS quote against the new mid. A quote
        // placed on this observation cannot fill on it: it did not
        // exist when this mid was formed.
        if (quote_active_) {
            if (bid_live_ && crossed_down(resting_bid_, mid)) {
                fill(Side::bid, resting_bid_, mid, now);
            } else if (ask_live_ && crossed_up(resting_ask_, mid)) {
                fill(Side::ask, resting_ask_, mid, now);
            }
        }

        // (5) The current quote becomes the resting one for the next
        // observation.
        quote_active_ = quote_valid_;
    }

    /// Whether the mid traded at or through a resting bid. A passive
    /// bid is hit when the market sells down to it, so the condition is
    /// that the mid fell to or below our price.
    [[nodiscard]] static bool crossed_down(Price our_bid, Price mid) noexcept {
        return mid <= our_bid;
    }

    [[nodiscard]] static bool crossed_up(Price our_ask, Price mid) noexcept {
        return mid >= our_ask;
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
    std::uint64_t quotes_ = 0;
    std::uint64_t fills_ = 0;
};

}  // namespace hft::strategy
