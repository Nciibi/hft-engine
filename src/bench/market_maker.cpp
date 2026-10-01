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

void print_signed(const char* label, double v, int scale_note) {
    std::printf("  %-26s %+12.2f  raw %s\n", label, v,
                scale_note == 0 ? "" : "");
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
    hft::feed::CaptureStats stats{};
    const std::vector<std::uint8_t> data = hft::feed::generate_capture(capture, &stats);

    strategy::MarketMakerConfig mm_config;
    mm_config.markout_horizon = horizon;
    // gamma and k are chosen so the quote is competitive but not
    // reckless against this feed's volatility. They are defaults, not
    // fitted values: fitting them to the capture would be fitting to
    // the noise and would flatter the result.
    mm_config.quote.gamma = 1.0e-3;   // 1/price units
    mm_config.quote.k = 1.5;
    mm_config.quote.horizon_ticks = 250.0;   // observations, not seconds
    mm_config.quote.base_size = 100;
    mm_config.max_inventory = 5'000;

    strategy::MarketMaker mm(mm_config);
    lob::OrderBook book(1u << 20, 1u << 16);

    std::uint64_t ticks = 0;
    hft::Nanos now = 1'000'000'000ULL;

    std::size_t offset = 0;
    while (offset < data.size()) {
        if (data.size() - offset < hft::feed::kCaptureSequenceSize + hft::itch::kLengthPrefixSize) {
            break;
        }
        const std::size_t frame_at = offset + hft::feed::kCaptureSequenceSize;
        const hft::itch::DecodeResult r = hft::itch::decode(data.data() + frame_at,
                                                            data.size() - frame_at);
        const std::size_t stride = hft::itch::frame_stride(r);
        if (stride == 0) {
            break;
        }
        if (r.ok()) {
            hft::lob::apply(r.message, book);
            // Each message advances the clock. A fixed tick per message
            // is synthetic, and the markout horizon is therefore a
            // number of messages rather than a real duration. Stated
            // here so the horizon column is not read as a time.
            now += 4'000;
            ++ticks;
            mm.on_book(book, now);
        }
        offset = frame_at + stride;
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
    std::printf("ticks observed       %llu\n", (unsigned long long)ticks);
    std::printf("two-sided book       %llu  (%.1f%%)\n",
                (unsigned long long)mm.two_sided_observations(),
                ticks == 0 ? 0.0
                           : 100.0 * static_cast<double>(mm.two_sided_observations()) /
                                 static_cast<double>(ticks));
    std::printf("one-sided book       %llu\n",
                (unsigned long long)mm.one_sided_observations());
    std::printf("quotes placed        %llu\n", (unsigned long long)mm.quotes());
    std::printf("fills                %llu\n", (unsigned long long)s.fills);
    std::printf("markout horizon      %llu ticks (~%llu us synthetic)\n",
                (unsigned long long)(mm.adverse().horizon() / 4'000),
                (unsigned long long)(mm.adverse().horizon() / 1'000));
    std::printf("volatility sigma     %.2f raw/tick (%.4f%%)\n", mm.volatility().sigma(),
                mm.volatility().sigma_fraction() * 100.0);
    std::printf("risk term            %.6f\n", mm.quoter().risk_term());
    std::printf("half spread          %.2f raw\n", mm.quoter().half_spread());
    std::printf("\n");
    std::printf("INVENTORY\n");
    std::printf("--------\n");
    std::printf("bid shares filled    %llu\n", (unsigned long long)s.bid_shares);
    std::printf("ask shares filled    %llu\n", (unsigned long long)s.ask_shares);
    std::printf("net position         %lld\n", (long long)s.net_shares());
    std::printf("closing position     %lld\n", (long long)p.position());
    std::printf("\n");
    std::printf("SPREADS (raw price units, 1/10000)\n");
    std::printf("------------------------------------\n");
    std::printf("%-24s %12.2f\n", "mean quoted", s.mean_quoted_spread());
    std::printf("%-24s %12.2f\n", "mean effective", s.mean_effective_spread());
    std::printf("%-24s %12.2f\n", "mean realised", s.mean_realised_spread());
    std::printf("%-24s %12.2f\n", "mean markout", s.mean_markout());
    std::printf("\n");
    std::printf("ADVERSE SELECTION\n");
    std::printf("----------------\n");
    std::printf("resolved fills       %llu\n", (unsigned long long)s.resolved);
    std::printf("toxic fills          %llu\n", (unsigned long long)s.toxic);
    std::printf("toxicity rate        %.2f%%\n", s.toxicity_rate() * 100.0);
    std::printf("realisation ratio    %.3f  (realised / quoted)\n", s.realisation_ratio());
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
    std::printf("total pnl (raw)      %lld\n", (long long)p.total_pnl());
    std::printf("pnl per fill (raw)   %.2f\n", p.pnl_per_fill());
    std::printf("best step (raw)      %lld\n", (long long)p.best_step());
    std::printf("worst step (raw)     %lld\n", (long long)p.worst_step());
    std::printf("drawdown (raw)       %lld\n", (long long)p.drawdown());
    std::printf("\n");
    std::printf("Read the toxicity rate and realisation ratio, not the PnL.\n");
    return 0;
}
