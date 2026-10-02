// Market maker over a replayed capture.
//
// Reports the adverse-selection numbers, not just the PnL. The spread
// a strategy quotes, the spread it receives, and what happened to the
// price afterwards are three different things and a strategy that only
// reports the first is hiding its own risk.
//
// Usage:
//   hft_market_maker [records] [horizon_us]
//
// PnL from this tool is an UPPER BOUND and is labelled as such in its
// own output. The fill model has no queue position, no latency and no
// size at level, all of which flatter a passive strategy. The
// toxicity rate is the finding; the PnL is a sanity check that the
// accounting is not broken.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/strategy/market_maker.hpp"
#include "hft/types.hpp"

namespace {

using hft::Price;
using hft::Side;
namespace strategy = hft::strategy;
namespace lob = hft::lob;

/// Narrowing helper for printf's %llu. A lambda rather than a cast at
/// every call site, so the cast appears once.
[[nodiscard]] unsigned long long u64(std::uint64_t v) noexcept {
    return static_cast<unsigned long long>(v);
}

[[nodiscard]] long long i64(std::int64_t v) noexcept {
    return static_cast<long long>(v);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t records = 300'000;
    hft::Nanos horizon = 1'000'000;  // 1ms

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const bool numeric = argv[i][0] != '\0';
        if (numeric && positional == 0) {
            records = std::strtoull(argv[i], nullptr, 10);
            ++positional;
        } else if (positional == 0) {
            ++positional;
        } else {
            horizon = static_cast<hft::Nanos>(std::strtoull(argv[i], nullptr, 10) * 1'000u);
        }
    }

hft::feed::CaptureConfig capture;
    capture.record_count = records;
    // A shallow, fast-walking book. These are not arbitrary: an
    // unbounded or deep book only accumulates, never clears a price
    // level, and its mid is frozen for the whole run, which leaves a
    // market maker with zero volatility to price. A book that cannot
    // reprice cannot be used to study a strategy that trades it.
    //
    // Four levels a side is roughly a four-tick book, which is what a
    // liquid US equity actually looks like. Deeper is not more
    // realistic here, it is just a wider book to quote into.
    capture.price_levels = 4;
    // Live-order cap and reversion were both wrong until the Add Order
    // decoder was corrected, and the reason is worth recording.
    //
    // The decoder used to read the SHARE COUNT as the price, so the
    // "price" the book held was a random number in [1, 500] that
    // changed on every add. The touch appeared to move constantly, the
    // volatility estimate was comfortably non-zero, and every strategy
    // number looked healthy. It was all downstream of a misread field.
    //
    // With real prices, 24 live orders across 4 levels a side keeps the
    // touch permanently populated: it never clears, so the mid never
    // moves, so volatility is zero, so the A-S risk term is zero, so
    // the quoted spread collapses below a tick and the strategy quotes
    // nothing at all. Measured: the mid moved on 0.7% of observations
    // and sigma was exactly 0.
    //
    // Six live orders over 4 levels a side means levels do empty and the
    // touch does reprice -- about 4.3% of observations -- which is the
    // precondition for the strategy having anything to price at all.
    capture.max_live_orders = 6;
    // Reversion of 64 pulls the walk back toward the anchor by 1/64 of
    // the distance every single record, which pins it in place no
    // matter how large the drift. Four lets it travel.
    capture.reversion = 4;
    capture.drift_raw = 400;
    hft::feed::CaptureStats stats{};
    const std::vector<std::uint8_t> data = hft::feed::generate_capture(capture, &stats);

    strategy::MarketMakerConfig mm_config;
    mm_config.markout_horizon = horizon;
    // gamma is in 1/price units, and the risk term is
    // gamma*sigma^2*horizon_ticks. On this capture sigma lands near 40
    // raw units per tick with a 250-tick horizon, so the term is
    // gamma*1.6e5*250 = 4.0e7*gamma; at 2.5e-3 that is about 1.0e5 raw
    // units, i.e. the quoted half-spread lands around two to three ticks
    // and is set by the risk term rather than by the tick constraint.
    // That matters: a half-spread pinned below one tick is the A-S
    // model being overridden by the exchange's tick grid, and the
    // interesting behaviour is invisible.
    //
    // These are illustrative, not fitted. Fitting gamma to this
    // capture would be fitting it to the noise and would flatter every
    // number below.
    mm_config.quote.gamma = 2.5e-3;
    mm_config.quote.k = 1.5;
    mm_config.quote.horizon_ticks = 250.0;
    mm_config.quote.base_size = 10;
    // A tight limit on purpose. See the note at the end: with a
    // symmetric fill model the position is a random walk, so the limit
    // gets reached regardless. A small limit keeps the excursion
    // visible and the numbers honest rather than letting the position
    // wander thousands of shares into a regime the strategy was never
    // parameterised for.
    mm_config.max_inventory = 500;

    strategy::MarketMaker mm(mm_config);
    lob::OrderBook book(1u << 20, 1u << 16);

    std::uint64_t ticks = 0;
    hft::Nanos now = 1'000'000'000ULL;

    hft::feed::CaptureReader reader(data.data(), data.size());
    const std::uint8_t* frame = nullptr;
    std::size_t frame_size = 0;
    while (reader.next(frame, frame_size)) {
        const hft::itch::DecodeResult r = hft::itch::decode(frame, frame_size);
        if (r.ok()) {
            // The apply result is deliberately discarded here. A
            // mutation for an order the book has never seen is
            // reported, not fatal, and the replay tool is where that
            // count is examined.
            (void)hft::lob::apply(r.message, book);
            // Each message advances the clock. A fixed tick per message
            // is synthetic, and the markout horizon is therefore a
            // number of messages rather than a real duration. Stated
            // here so the horizon column is not read as a time.
            now += 4'000;
            ++ticks;
            mm.on_book(book, now);
        }
    }

    // Resolve the tail so a short run does not discard its last fills.
    Price final_mid{};
    if (const auto b = book.best_bid(); b.has_value()) {
        if (const auto a = book.best_ask(); a.has_value()) {
            final_mid = Price::from_raw((b->raw() + a->raw()) / 2);
        }
    }
    mm.finish(final_mid);

    const strategy::Stats& s = mm.adverse().stats();
    const strategy::PnlTracker& p = mm.pnl();

    std::printf("HFT Engine market maker\n");
    std::printf("-----------------------\n");
    std::printf("records              %zu\n", stats.records);
    std::printf("ticks observed       %llu\n", u64(ticks));
    const double two_sided_pct =
        ticks == 0 ? 0.0
                   : 100.0 * static_cast<double>(mm.two_sided_observations()) /
                         static_cast<double>(ticks);
    std::printf("two-sided book       %llu  (%.1f%%)\n", u64(mm.two_sided_observations()),
                two_sided_pct);
    std::printf("one-sided book       %llu\n", u64(mm.one_sided_observations()));
    // Read this before anything below it. A mid that does not move
    // means zero volatility, which means the A-S risk term is zero,
    // which means the spread collapses below a tick. Every spread,
    // markout and PnL figure below is conditioned on the book actually
    // being a market.
    std::printf("mid moves            %llu of %llu two-sided (%.3f%%)\n", u64(mm.mid_moves()),
                u64(mm.two_sided_observations()),
                mm.two_sided_observations() == 0
                    ? 0.0
                    : 100.0 * static_cast<double>(mm.mid_moves()) /
                          static_cast<double>(mm.two_sided_observations()));
    if (mm.mid_moves() == 0 && mm.two_sided_observations() > 0) {
        std::printf(
            "                    WARNING: the mid never moved. Volatility is zero by\n"
            "                    construction and everything below is degenerate.\n");
    } else if (mm.two_sided_observations() != 0 &&
               mm.mid_moves() * 50 < mm.two_sided_observations()) {
        std::printf(
            "                    WARNING: the mid moved on under 2%% of observations.\n"
            "                    More than half the per-tick returns are exactly zero,\n"
            "                    so the median-absolute-deviation volatility estimate is\n"
            "                    zero and the quoted spread is degenerate. Read the\n"
            "                    numbers below as a wiring check, not as a result.\n");
    }
    std::printf("quotes placed        %llu\n", u64(mm.quotes()));
    std::printf("fill checks          %llu  (bid %llu / ask %llu)\n", u64(mm.fill_checks()),
                u64(mm.bid_side_checks()), u64(mm.ask_side_checks()));
    std::printf("fills                %llu  (bid %llu / ask %llu)\n", u64(mm.fills()),
                u64(mm.bid_hits()), u64(mm.ask_hits()));
    std::printf("markout horizon      %llu ticks (~%llu us synthetic)\n",
                u64(mm.adverse().horizon() / 4'000), u64(mm.adverse().horizon() / 1'000));
    std::printf("volatility sigma     %.2f raw/tick (%.4f%%)\n", mm.volatility().sigma(),
                mm.volatility().sigma_fraction() * 100.0);
    std::printf("risk term            %.6f\n", mm.quoter().risk_term());
    std::printf("half spread          %.2f raw\n", mm.quoter().half_spread());
    std::printf("\n");
    std::printf("INVENTORY\n");
    std::printf("--------\n");
    std::printf("bid shares filled    %llu\n", u64(s.bid_shares));
    std::printf("ask shares filled    %llu\n", u64(s.ask_shares));
    std::printf("net position         %lld\n", i64(s.net_shares()));
    std::printf("max |position|       %lld  (limit %lld)\n", i64(mm.max_position_observed()),
                i64(mm_config.max_inventory));
    std::printf("closing position     %lld\n", i64(p.position()));
    std::printf("\n");
    std::printf("SPREADS (raw price units, 1/10000)\n");
    std::printf("------------------------------------\n");
    std::printf("%-24s %12.2f\n", "mean quoted", s.mean_quoted_spread());
    std::printf("%-24s %12.2f\n", "mean effective", s.mean_effective_spread());
    std::printf("%-24s %12.2f\n", "mean realised", s.mean_realised_spread());
    std::printf("%-24s %12.2f\n", "mean markout", s.mean_markout());
    std::printf("\n");
    std::printf("A negative EFFECTIVE spread is correct here, not a bug:\n");
    std::printf("a passive fill buys at the bid, which is below the mid, so\n");
    std::printf("2*sign*(fill-mid) is negative by construction. The profit is\n");
    std::printf("in the round trip. What costs money is adverse selection.\n");
    std::printf("\n");
    std::printf("ADVERSE SELECTION\n");
    std::printf("----------------\n");
    std::printf("resolved fills       %llu\n", u64(s.resolved));
    std::printf("toxic fills          %llu\n", u64(s.toxic));
    std::printf("toxicity rate        %.2f%%\n", s.toxicity_rate() * 100.0);
    std::printf("adverse cost / fill  %+.2f raw  (2 * markout; positive is a cost)\n",
                s.adverse_cost_per_fill());
    std::printf("\n");
    std::printf("IDENTITY CHECK\n");
    std::printf("--------------\n");
    // realised = effective - 2 * markout, exactly. If the
    // implementation drifts from the definition, this is where it
    // shows, and it is the one arithmetic relation a reviewer will
    // check.
    std::printf("effective - 2*markout = %.2f\n", s.mean_effective_spread() - 2.0 * s.mean_markout());
    std::printf("mean realised        = %.2f\n", s.mean_realised_spread());
    std::printf("\n");
    std::printf("PNL (UPPER BOUND - no queue position, no latency, no depth)\n");
    std::printf("----------------------------------------------------\n");
    std::printf("total pnl (raw)      %lld\n", i64(p.total_pnl()));
    std::printf("pnl per fill (raw)   %.2f\n", p.pnl_per_fill());
    std::printf("best step (raw)      %lld\n", i64(p.best_step()));
    std::printf("worst step (raw)     %lld\n", i64(p.worst_step()));
    std::printf("drawdown (raw)       %lld\n", i64(p.drawdown()));
    std::printf("\n");
    std::printf("Read the markout, the toxicity rate and the inventory excursion.\n");
    std::printf("The PnL is an upper bound and the most flattering number here.\n");
    std::printf(
        "\n"
        "KNOWN LIMITATION - read before quoting any of this:\n"
        "  The fill model is SYMMETRIC. Our bid and our ask are equally\n"
        "  likely to fill, because a fill is triggered by the touch moving\n"
        "  to our price regardless of why it moved. The position is\n"
        "  therefore a random walk, and the Avellaneda-Stoikov inventory\n"
        "  term cannot damp it: the skew widens the quote, but the fill\n"
        "  probability here does not fall off with distance the way it\n"
        "  does in a real book. The position consequently reaches its\n"
        "  limit and stays there.\n"
        "\n"
        "  Real inventory control depends on fills being ASYMMETRIC: your\n"
        "  bid fills more often precisely when the market is selling\n"
        "  down through you, which is the same condition that leaves you\n"
        "  long. Modelling that requires order-flow toxicity as an input,\n"
        "  which this simulator does not have. So the inventory numbers\n"
        "  here demonstrate that the mechanism is wired up, NOT that the\n"
        "  strategy controls inventory. The adverse-selection metrics do\n"
        "  not depend on this and stand on their own.\n");
    return 0;
}
