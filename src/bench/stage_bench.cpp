// Per-stage latency, at two book depths.
//
// What this measures
// ------------------
// PLAN.md Phase 6 asks for decode, book update, decision, risk and
// encode, reported separately, at a shallow and a deep book state. This
// tool does the first four. It does not do encode, and the reason is in
// `ITCH ENCODE` in the output below.
//
// Two book states, because one number cannot cover both:
//
//   SHALLOW  10 price levels a side, the shape a liquid US equity has.
//   DEEP     1000 price levels a side.
//
// The price ladder here is a sorted linked list, so inserting a price
// that is not adjacent to the best walks from the head of the ladder.
// Book-update cost is therefore proportional to ladder depth, and a
// single figure across both would be a figure about nothing. Reporting
// them separately is the whole reason this tool exists.
//
// What each stage is, precisely
// -----------------------------
//   decode        itch::decode, one frame to one payload struct
//   book update   lob::apply, one payload to one book mutation
//   decision      strategy::Quoter::quote, mid + inventory to two prices
//   risk check    risk::PreTradeRisk::check, one order to one verdict
//   end to end    all four, in that order, on the same message
//
// `end to end` is measured as one pass over the loop, not as a sum of
// the four stage figures. A sum would be a number about four
// independent measurements; the pass is a number about the pipeline.
// They will not agree exactly, because each stage figure includes its
// own clock pair and the four pairs are attributed differently.
//
// Measurement discipline, inherited from `bench.cpp`:
//   * the cost of a clock read is measured and printed first;
//   * the feed is generated and faulted in outside the timed region;
//   * ladder depth and resting size are printed next to the numbers,
//     because a latency figure without them is not interpretable.
//
// Usage:
//   hft_stage_bench [records]

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "bench/report.hpp"
#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/risk/limits.hpp"
#include "hft/strategy/quoting.hpp"
#include "hft/types.hpp"

namespace {

using hft::Price;
using hft::Quantity;
using hft::Side;
namespace itch = hft::itch;
namespace lob = hft::lob;
namespace risk = hft::risk;
namespace strategy = hft::strategy;

/// The two depths, named for what they are rather than for their size.
struct BookShape {
    const char* name;
    std::size_t levels_per_side;
};

constexpr BookShape kShallow{"shallow", 10};
constexpr BookShape kDeep{"deep", 1'000};

/// The synthetic clock. One tick per record, so the rate limiter sees a
/// plausible order rate and the markout horizon is a number of messages
/// rather than a duration. Stated here so the horizon is not read as a
/// time.
constexpr hft::Nanos kTickNanos = 4'000;

struct StageResult {
    bench::LatencyHistogram decode;
    bench::LatencyHistogram book;
    bench::LatencyHistogram decision;
    bench::LatencyHistogram risk;
    bench::LatencyHistogram end_to_end;

    std::uint64_t records = 0;
    std::uint64_t applied = 0;
    std::uint64_t quotes = 0;
    std::uint64_t risk_allowed = 0;
    std::uint64_t risk_refused = 0;
    std::uint64_t tick_constrained = 0;
    std::uint64_t mid_moves = 0;
    double elapsed_ns = 0.0;

    std::uint32_t bid_levels = 0;
    std::uint32_t ask_levels = 0;
};

/// One full pass over the feed, timing each stage separately.
[[nodiscard]] StageResult run(const std::vector<std::uint8_t>& data, std::size_t pool, bool dense) {
    StageResult r;

    // Dense price ladder, selected by argv[2]. The sparse book links
    // levels into a sorted list and walks it to position a new price; the
    // dense book addresses it by (price - floor) / tick and keeps an
    // occupancy bitmap for best-bid/ask. Same differential-tested
    // behaviour, different mechanism -- which is what makes this an A/B
    // rather than a rewrite.
    hft::lob::LadderConfig ladder;
    ladder.dense = dense;
    // The generator walks around $100 with drift; a band from $10 to $400
    // covers every shape this tool produces with enormous margin.
    ladder.floor_price = Price::from_int(10);
    ladder.tick = 100;  // one cent, the generator's tick_raw
    ladder.ticks_per_side = 1u << 16;
    lob::OrderBook book(pool, pool, 0, ladder);
    if (dense) {
        std::printf("ladder           DENSE (direct-indexed price grid)\n");
    }

    strategy::QuoteParams qp;
    qp.gamma = 2.5e-3;
    qp.k = 1.5;
    qp.horizon_ticks = 250.0;
    qp.base_size = 100;
    qp.sigma = 40.0;
    qp.min_spread_raw = 100;
    strategy::Quoter quoter(qp);

    risk::Limits limits;
    limits.max_order_rate = 1'000'000;  // effectively off; see below
    risk::PreTradeRisk risk_engine(limits);
    risk_engine.set_reference_price(Price::from_int(100));

    hft::Nanos now = 1'000'000'000ULL;
    std::int64_t inventory = 0;
    std::optional<Price> previous_mid;

    bench::Timer timer;
    hft::feed::CaptureReader reader(data.data(), data.size());
    const std::uint8_t* frame = nullptr;
    std::size_t frame_size = 0;

    while (reader.next(frame, frame_size)) {
        const std::uint64_t t0 = bench::Stopwatch::now_ns();
        const itch::DecodeResult decoded = itch::decode(frame, frame_size);
        const std::uint64_t t1 = bench::Stopwatch::now_ns();

        ++r.records;
        if (!decoded.ok()) {
            continue;
        }

        r.decode.record(t1 - t0);

        const lob::ApplyResult result = lob::apply(decoded.message, book);
        const std::uint64_t t2 = bench::Stopwatch::now_ns();
        r.book.record(t2 - t1);
        if (result.applied) {
            ++r.applied;
        }

        // Decision: quote around the current mid. This is the per-message
        // cost of the strategy thinking, with no fill simulation and no
        // PnL -- those are measured by hft_market_maker and including
        // them here would make this row a different number.
        const std::optional<Price> bid = book.best_bid();
        const std::optional<Price> ask = book.best_ask();
        Price mid{};
        if (bid.has_value() && ask.has_value()) {
            mid = Price::from_raw((bid->raw() + ask->raw()) / 2);
        }
        const strategy::Quote q = quoter.quote(mid, inventory);
        const std::uint64_t t3 = bench::Stopwatch::now_ns();
        r.decision.record(t3 - t2);
        if (q.valid) {
            ++r.quotes;
            if (q.tick_constrained) {
                ++r.tick_constrained;
            }
        }

        // Risk: check the quote we would actually send. Time is injected,
        // so the rate limiter sees a plausible rate without reading a
        // clock, and the whole check stays deterministic.
        now += kTickNanos;
        const risk::Decision verdict = risk_engine.check(
            Side::bid, q.valid ? q.bid : mid, Quantity::from_raw(q.bid_size), now);
        const std::uint64_t t4 = bench::Stopwatch::now_ns();
        r.risk.record(t4 - t3);
        if (verdict.allowed()) {
            ++r.risk_allowed;
        } else {
            ++r.risk_refused;
        }

        r.end_to_end.record(t4 - t0);

        if (previous_mid.has_value() && *previous_mid != mid) {
            ++r.mid_moves;
        }
        previous_mid = mid;
    }

    r.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    r.bid_levels = book.level_count(Side::bid);
    r.ask_levels = book.level_count(Side::ask);
    return r;
}

/// A capture at a given book shape.
[[nodiscard]] std::vector<std::uint8_t> make_feed(const BookShape& shape, std::size_t records,
                                                  hft::feed::CaptureStats* stats) {
    hft::feed::CaptureConfig capture;
    capture.record_count = records;
    capture.price_levels = shape.levels_per_side;
    // The live-order cap is what makes the book reprice. Left unbounded,
    // every level only ever grows, the touch is set once and never
    // moves, and the mid is frozen for the whole run -- which makes the
    // decision stage measure a degenerate input.
    //
    // Sized at ONE order per level, not three. Measured over 400,000
    // records at 10 levels per side: a cap of 10 produced 8,234 mid
    // moves, a cap of 30 produced 17. Three orders of slack at the
    // touch is three orders too many -- the touch simply does not
    // empty. The deep shape showed the same thing harder: the mid-move
    // count was identical at 1,000 and at 40,000 live orders, because a
    // thick touch is a sticky touch.
    capture.max_live_orders = shape.levels_per_side;
    // Reversion weak, and drift scaled to the ladder rather than fixed.
    //
    // Both were measured. A fixed drift of 400 raw is four ticks: fine
    // against a 10-level ladder, and nothing at all against a 1,000-level
    // one, where the best bid became a stable maximum over a band two
    // hundred times wider than the walk ever travelled. The mid moved 23
    // times in 2,000,000 records and no cap setting changed it.
    //
    // The stationary spread of the walk is `drift * sqrt(reversion / 6)`,
    // which is the part that is easy to get wrong: at reversion 1000 the
    // amplification is about 13x, so half the ladder as a drift setting
    // produces a spread of thirteen halves. Measured over 150,000 records
    // at 1,000 levels: reversion 64 gave 5 mid moves, 500 gave 50, 1000
    // gave 220, book uncrossed throughout. At 10 levels the same setting
    // gives 3,370, so one policy serves both depths and the comparison
    // stays a comparison of depth rather than of price process.
    capture.drift_raw =
        static_cast<std::int64_t>(shape.levels_per_side) * capture.tick_raw / 2;
    capture.reversion = 1000;
    // The anchor moves with the drift, and this is not cosmetic. A walk
    // of the width above wanders thousands of ticks from its mean, so a
    // walk centred on the default anchor eventually produces a negative
    // price and the generator drops the record: 8,477 of 2,000,000 went
    // missing at the deep shape, and the tool printed a truncation
    // warning that had nothing to do with the decoder. Centring the walk
    // a hundred drift-widths up keeps every price positive without
    // touching the shape of the distribution, which is the part the
    // benchmark actually measures.
    capture.anchor_raw = capture.drift_raw * 100;
    return hft::feed::generate_capture(capture, stats);
}

void report(const BookShape& shape, const StageResult& r, std::size_t records) {
    std::printf("\n%s book: %s levels per side\n", shape.name,
                bench::humanize(shape.levels_per_side).c_str());
    std::printf("  %-22s bid %6u levels, ask %6u levels\n", "final book", r.bid_levels,
                r.ask_levels);
    std::printf("  %-22s %s records, %s applied, %s quotes, %s tick-constrained\n", "",
                bench::humanize(r.records).c_str(), bench::humanize(r.applied).c_str(),
                bench::humanize(r.quotes).c_str(), bench::humanize(r.tick_constrained).c_str());
    std::printf("  %-22s %s allowed, %s refused by risk\n", "",
                bench::humanize(r.risk_allowed).c_str(), bench::humanize(r.risk_refused).c_str());
    std::printf("  %-22s %s mid moves of %s observations\n", "",
                bench::humanize(r.mid_moves).c_str(), bench::humanize(r.records).c_str());

    const double rate =
        r.elapsed_ns > 0.0 ? static_cast<double>(r.records) * 1e9 / r.elapsed_ns : 0.0;
    std::printf("  %-22s %12s msg/s  %9.3f ms\n", "throughput",
                bench::humanize(static_cast<std::uint64_t>(rate)).c_str(), r.elapsed_ns / 1e6);

    std::printf("\n");
    bench::histogram_row("decode", r.decode);
    bench::histogram_row("book update", r.book);
    bench::histogram_row("decision", r.decision);
    bench::histogram_row("risk check", r.risk);
    bench::histogram_row("end to end", r.end_to_end);
    std::fflush(stdout);

    if (r.records < records) {
        std::printf("  WARNING: %s of %s records decoded. The remaining frames were\n"
                    "           undecodable and the figures above cover a shorter run\n"
                    "           than intended.\n",
                    bench::humanize(r.records).c_str(), bench::humanize(records).c_str());
    }

    // The mid-move count is printed above and is NOT a validity warning
    // here, though it is one in hft_market_maker. The difference is what
    // depends on it.
    //
    // For the market maker the mid feeds the volatility estimate, so a
    // static mid means a zero spread and a strategy that quotes nothing.
    // For a latency benchmark the mid is only an argument to quote(); the
    // arithmetic is identical whether the mid moved or not, so the stage
    // figures stand.
    //
    // What a static touch DOES change is the mix of book mutations. With
    // the touch permanently occupied, most adds land at interior levels
    // and the expensive path -- walking the ladder to a price that is not
    // adjacent to the best -- is rarer than in a live book. So these
    // figures are, if anything, optimistic for the deep shape. The
    // shallow-versus-deep comparison is unaffected, because both shapes
    // have the same mix.
    //
    // The rate is printed as a percentage rather than bucketed into
    // "under 2%". The deep shape sits near 0.01% and the shallow shape
    // near 2%, and a note that calls both "under 2%" describes neither.
    if (r.records > 0) {
        const double pct = 100.0 * static_cast<double>(r.mid_moves) /
                           static_cast<double>(r.records);
        if (pct < 0.1) {
            std::printf(
                "  note: the mid moved on %.3f%% of observations. The touch is\n"
                "        effectively static: at this depth a few orders are spread\n"
                "        across a ladder wide enough that the nearest resting order\n"
                "        does not change hands often. The decision stage below is\n"
                "        therefore timing the quoting arithmetic on a frozen mid,\n"
                "        which is the arithmetic and not the behaviour. Stage figures\n"
                "        stand; this tool does not claim a deep-book strategy result.\n",
                pct);
        } else if (pct < 2.0) {
            std::printf(
                "  note: the mid moved on %.3f%% of observations. Repricing, but\n"
                "        rarely. Book-update cost is weighted towards interior-level\n"
                "        adds because the touch is often occupied.\n",
                pct);
        }
    }
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t records = 2'000'000;
    bool dense = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "dense") {
            dense = true;
        } else {
            records = std::strtoull(argv[i], nullptr, 10);
        }
    }
    if (records == 0) {
        records = 1;
    }

    bench::print_environment("per-stage latency");

    // Both pools are sized from the largest feed so the shallower run is
    // not silently constrained by a smaller pool.
    const std::size_t pool = records;

    bench::section("MEASUREMENT COST");
    const bench::LatencyHistogram clock = bench::measure_clock_overhead();
    bench::print_clock_overhead(clock);

    std::printf("\nrecords per shape    %s\n", bench::humanize(records).c_str());
    std::printf("pool orders          %s\n", bench::humanize(pool).c_str());
    std::printf("pool memory          %.0f MiB\n",
                static_cast<double>(pool * (sizeof(lob::OrderNode) + sizeof(lob::LevelNode))) /
                    (1024.0 * 1024.0));

    bench::section("PER-STAGE LATENCY, BY BOOK DEPTH");
    std::printf("\n  Each row is one clock pair around one call. `end to end` is a\n"
                "  separate pass over the same work, not a sum of the rows above\n"
                "  it: a sum would describe four measurements, not a pipeline.\n");

    for (const BookShape& shape : {kShallow, kDeep}) {
        hft::feed::CaptureStats stats{};
        const std::vector<std::uint8_t> feed = make_feed(shape, records, &stats);

        // One throwaway run first, so the measured one is not paying for
        // page faults in the feed or in the pools.
        (void)run(feed, pool, dense);

        const StageResult r = run(feed, pool, dense);
        report(shape, r, records);
    }

    bench::section("ITCH ENCODE");
    std::printf(
        "  NOT MEASURED -- AND THE REASON IS NOT THE ONE GIVEN HERE BEFORE.\n"
        "\n"
        "  An earlier revision of this text said the missing stage was \"ITCH\n"
        "  Order Entry ('B')\", skipped because its field table could not be\n"
        "  verified. That was wrong about the protocol, and it was verifiable:\n"
        "  the specification is public at nasdaqtrader.com, and section 1.1 says\n"
        "\n"
        "    \"TOTALVIEW-ITCH is an outbound market data feed only. The ITCH\n"
        "     protocol does not support order entry.\"\n"
        "\n"
        "  There is no Order Entry message in TotalView-ITCH 5.0 to skip. Tag\n"
        "  'B' is not order entry either: it is the Broken Trade message, a\n"
        "  19-byte INBOUND market-data report that an execution was cancelled\n"
        "  under the clearly-erroneous policy. Nasdaq's own feed specifications\n"
        "  for NQ, BX and PSX all define it identically -- Match Number at\n"
        "  offset 11, length 8. A stage that encoded 'B' as an order would have\n"
        "  put an eight-byte match number where a venue expects order entry.\n"
        "\n"
        "  Order entry for Nasdaq is a different product entirely -- Nasdaq\n"
        "  Basic, OU Clearsight, FIX -- and modelling one of those is a\n"
        "  different piece of work with its own specification. Inventing an\n"
        "  ITCH order-entry layout to fill a row in this table is precisely the\n"
        "  mistake that made the Add Order decoder read the share count as the\n"
        "  price for the entire life of this project, and it survived every\n"
        "  test because the generator was guessing the same way.\n"
        "\n"
        "  So the row stays missing, and the missing reason is now a correct\n"
        "  one. A number here would describe a protocol this engine does not\n"
        "  speak.\n");

    bench::print_publication_notice();
    return 0;
}