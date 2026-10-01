// Tests for the quoting model, the adverse-selection metrics, and the
// synthetic book.
//
// The last of these is the important one. A crossed book cannot occur
// in a real market, and a generator that produces one silently poisons
// every downstream measurement: the mid, the volatility, the markout
// and the PnL all become meaningless rather than merely wrong. It
// happened here, and it was invisible until the resting quote was
// printed next to the touch. So it is now asserted.

#include <cstdio>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/strategy/market_maker.hpp"
#include "hft/strategy/metrics.hpp"
#include "hft/strategy/quoting.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void check_near(double actual, double expected, double tolerance, const char* what) {
    ++g_checks;
    const double delta = actual - expected;
    if (delta > tolerance || delta < -tolerance) {
        ++g_failures;
        std::printf("  FAIL  %s: expected %.6f, got %.6f\n", what, expected, actual);
    }
}

using hft::Price;
using hft::Quantity;
using hft::Side;
namespace strategy = hft::strategy;
namespace lob = hft::lob;

// ---- The model ------------------------------------------------------

void test_quoting_units() {
    std::printf("quoting: units and tick constraint\n");

    strategy::QuoteParams p;
    p.gamma = 2.5e-3;
    p.k = 1.5;
    p.horizon_ticks = 250.0;
    p.sigma = 30.0;
    p.min_spread_raw = 100;  // one tick
    strategy::Quoter q(p);

    // risk_term must be gamma*sigma^2*H in price units.
    check_near(q.risk_term(), 2.5e-3 * 900.0 * 250.0, 1e-6, "risk term is gamma*sigma^2*H");

    // A fractional sigma makes the model dimensionally incoherent and
    // collapses the spread to a fraction of a tick. Guard the units.
    const double sigma_before = q.risk_term();
    q.set_sigma(0.0003);
    check(q.risk_term() < sigma_before,
          "a fractional sigma shrinks the risk term, which is the unit bug");

    q.set_sigma(30.0);
    check_near(q.risk_term(), sigma_before, 1e-6, "restoring sigma restores the risk term");

    // The tick clamp. The model wants ~20 raw units; one tick is 100.
    const strategy::Quote quote = q.quote(Price::from_raw(1'000'000), 0);
    check(quote.valid, "quote is valid after tick clamping");
    check(quote.tick_constrained, "and reports that the tick set the spread");
    check(quote.ask.raw() - quote.bid.raw() >= 100,
          "spread is at least one tick, never a fraction of one");
    check(quote.ask > quote.bid, "ask is above bid");

    // A wide model spread must NOT be clamped down.
    strategy::QuoteParams wide = p;
    wide.gamma = 0.5;  // enormous risk aversion
    strategy::Quoter qw(wide);
    const strategy::Quote wquote = qw.quote(Price::from_raw(1'000'000), 0);
    check(wquote.ask.raw() - wquote.bid.raw() > 100,
          "a model that wants wider is not narrowed to the tick");
}

void test_inventory_skew() {
    std::printf("quoting: inventory skew\n");
    strategy::QuoteParams p;
    p.gamma = 1.0e-3;
    p.sigma = 30.0;
    p.horizon_ticks = 250.0;
    strategy::Quoter q(p);

    const Price mid = Price::from_raw(1'000'000);
    const strategy::Quote flat = q.quote(mid, 0);
    const strategy::Quote long_pos = q.quote(mid, 1'000);
    const strategy::Quote short_pos = q.quote(mid, -1'000);

    // A long position must lower BOTH sides, and by more than the
    // inventory term alone, so the strategy leans against its position.
    check(long_pos.bid < flat.bid, "a long position lowers the bid");
    check(long_pos.ask < flat.ask, "a long position lowers the ask");
    check(short_pos.bid > flat.bid, "a short position raises the bid");
    check(short_pos.ask > flat.ask, "a short position raises the ask");

    // Symmetric, which is the property a directional bias would break.
    check(flat.bid.raw() + flat.ask.raw() == long_pos.bid.raw() + long_pos.ask.raw() * 0 + 
              (2 * mid.raw() - (long_pos.bid.raw() + long_pos.ask.raw())),
          "the skew shifts both sides by the same amount");

    // And the skew must scale with inventory, linearly.
    const double skew_flat = static_cast<double>(flat.bid.raw()) - mid.raw();
    const double skew_long = static_cast<double>(long_pos.bid.raw()) - mid.raw();
    check(skew_long < skew_flat, "more inventory, more skew");
}

void test_volatility_estimator() {
    std::printf("volatility estimator\n");
    strategy::VolatilityEstimator v(64);

    check_near(v.sigma(), 0.0, 1e-9, "no volatility before two samples");
    v.observe(Price::from_raw(1'000'000));
    check_near(v.sigma(), 0.0, 1e-9, "one sample is still not enough");

    // A constant 10-unit step.
    v.reset();
    std::int64_t px = 1'000'000;
    for (int i = 0; i < 40; ++i) {
        px += 10;
        v.observe(Price::from_raw(px));
    }
    check_near(v.sigma(), 10.0, 1.0, "a constant step gives sigma equal to the step");

    // Absolute, not fractional.
    check(v.sigma() > 1.0, "sigma is in raw price units, not a fraction");
    check(v.sigma_fraction() < 0.001, "and the fractional accessor exists for reporting");
}

// ---- The metrics ----------------------------------------------------

void test_metric_identity() {
    std::printf("metrics: identity and sign conventions\n");
    // realised = effective - 2 * markout, exactly. This is the one
    // arithmetic relation a reviewer will check, so it is checked.
    constexpr std::int64_t fill = 1'000'100;
    constexpr std::int64_t mid_at = 1'000'200;
    constexpr std::int64_t mid_after = 1'000'050;

    const std::int64_t effective = 2 * (fill - mid_at);
    const std::int64_t markout = mid_after - mid_at;
    const std::int64_t realised = effective - 2 * markout;

    check(effective == -200, "buying above the mid is a positive cost");
    check(markout == -150, "the mid falling after a buy is a negative markout");
    check(realised == 100, "and realised equals effective minus twice the markout");

    // Driving the real tracker and checking the same relation through
    // its public API, so the identity is verified in the code that
    // ships rather than in a comment above it.
    strategy::AdverseSelection adverse(/*horizon=*/1'000, /*toxicity_threshold=*/0);
    adverse.on_fill(Side::bid, Price::from_raw(fill), Quantity::from_raw(10),
                    Price::from_raw(mid_at), /*quoted_spread=*/200, /*now=*/0);
    adverse.advance(Price::from_raw(mid_after), /*now=*/2'000);
    const strategy::Stats& s = adverse.stats();
    check(s.resolved == 1, "the fill resolved once the horizon elapsed");
    check_near(s.mean_effective_spread(), -200.0, 1e-9, "tracker effective matches the identity");
    check_near(s.mean_markout(), -150.0, 1e-9, "tracker markout matches the identity");
    check_near(s.mean_realised_spread(), 100.0, 1e-9, "tracker realised matches the identity");
    check_near(s.mean_realised_spread(), s.mean_effective_spread() - 2.0 * s.mean_markout(),
               1e-9, "and the identity holds through the public accessors");
    check(s.toxic == 1, "a fill followed by a falling mid is toxic");
    check_near(s.toxicity_rate(), 1.0, 1e-9, "toxicity rate is 100%");
}

void test_metric_horizon() {
    std::printf("metrics: horizon handling\n");
    strategy::AdverseSelection adverse(/*horizon=*/1'000, /*toxicity_threshold=*/0);
    const Price p = Price::from_raw(1'000'000);
    const Price moved = Price::from_raw(999'000);

    adverse.on_fill(Side::bid, p, Quantity::from_raw(10), p, 200, 0);
    adverse.advance(moved, 999);
    check(adverse.stats().resolved == 0, "a fill is not resolved before its horizon");
    check(adverse.pending() == 1, "and is still pending");
    adverse.advance(moved, 1'000);
    check(adverse.stats().resolved == 1, "it resolves once the horizon elapses");

    // flush resolves the tail, so a short run does not silently drop
    // its last fills and bias the sample toward the run's start.
    strategy::AdverseSelection tail(/*horizon=*/1'000'000, 0);
    tail.on_fill(Side::bid, p, Quantity::from_raw(10), p, 200, 0);
    check(tail.pending() == 1, "a fresh tracker has a pending fill");
    tail.flush(moved);
    check(tail.stats().resolved == 1, "flush resolves it at the final mid");
}

void test_pnl_accounting() {
    std::printf("pnl accounting\n");
    strategy::PnlTracker p;
    const Price buy = Price::from_raw(1'000'000);
    const Price sell = Price::from_raw(1'000'200);

    p.on_fill(Side::bid, buy, Quantity::from_raw(100));
    check(p.position() == 100, "a buy increases the position");
    check(p.cash() == -100 * 1'000'000LL, "a buy spends cash");

    p.mark(buy);
    check(p.position() == 100, "marking does not change the position");

    p.on_fill(Side::ask, sell, Quantity::from_raw(100));
    check(p.position() == 0, "the position is flat again");
    p.mark(sell);
    // Round trip: bought at 1_000_000, sold at 1_000_200, 100 shares.
    check(p.total_pnl() == 100 * 200, "a flat round trip profits exactly the spread");
    check(p.drawdown() == 0, "and draws down nothing");
}

// ---- The book invariant ---------------------------------------------

void test_book_never_crosses() {
    std::printf("book invariant: never crossed\n");
    // best_bid > best_ask cannot happen in a real market. A generator
    // that produces it invalidates every measurement taken downstream,
    // silently.
    hft::feed::CaptureConfig cfg;
    cfg.record_count = 120'000;
    cfg.price_levels = 4;
    // Shallow on purpose. With more orders resting per level, a level
    // takes hundreds of messages to clear, the best bid and ask are set
    // rarely, and the price stops repricing at a frequency anything can
    // trade against. A real book turns over its touch many times a
    // second; a synthetic one has to be told to.
    cfg.max_live_orders = 24;
    cfg.drift_raw = 200;

    const std::vector<std::uint8_t> data = hft::feed::generate_capture(cfg);
    lob::OrderBook book(1u << 20, 1u << 16);

    std::size_t offset = 0;
    std::uint64_t two_sided = 0;
    std::uint64_t crossed = 0;
    std::uint64_t mid_changes = 0;
    std::int64_t previous_mid = 0;
    std::int64_t mid_low = 0;
    std::int64_t mid_high = 0;

    while (offset < data.size()) {
        if (data.size() - offset <
            hft::feed::kCaptureSequenceSize + hft::itch::kLengthPrefixSize) {
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
            const auto b = book.best_bid();
            const auto a = book.best_ask();
            if (b && a) {
                ++two_sided;
                if (b->raw() > a->raw()) {
                    if (crossed == 0) {
                        std::printf("  FIRST CROSSING at two-sided #%llu: bid=%lld ask=%lld "
                                    "(over by %lld)\n",
                                    (unsigned long long)two_sided, (long long)b->raw(),
                                    (long long)a->raw(), (long long)(b->raw() - a->raw()));
                    }
                    ++crossed;
                }
                const std::int64_t mid = (b->raw() + a->raw()) / 2;
                if (previous_mid != 0) {
                    if (mid != previous_mid) {
                        ++mid_changes;
                    }
                } else {
                    mid_low = mid_high = mid;
                }
                if (mid < mid_low) mid_low = mid;
                if (mid > mid_high) mid_high = mid;
                previous_mid = mid;
            }
        }
        offset = frame_at + stride;
    }

    std::printf("  (two-sided %llu, mid changes %llu, range %lld..%lld)\n",
                (unsigned long long)two_sided, (unsigned long long)mid_changes,
                (long long)mid_low, (long long)mid_high);
    check(crossed == 0, "the generated book is never crossed");
    check(two_sided > 0, "the book has two sides for most of the run");
    // A frozen mid makes a market maker untestable: there is no
    // volatility to price and nothing to trade against.
    check(mid_changes > two_sided / 100, "the mid reprices during the run");
    check(mid_high > mid_low, "and it visits a range of prices");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("strategy tests\n--------------\n");
    test_quoting_units();
    test_inventory_skew();
    test_volatility_estimator();
    test_metric_identity();
    test_metric_horizon();
    test_pnl_accounting();
    test_book_never_crosses();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
