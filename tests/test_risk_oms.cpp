// Tests for pre-trade risk and the OMS.
//
// Two kinds of check here, deliberately:
//
//  * Directed tests for the cases that are known to break systems:
//    a fill racing a cancel, an over-fill, a latched kill switch, a
//    notional multiply that overflows.
//  * A randomised invariant test. An OMS bug is usually a *state* you
//    can reach rather than a sequence you can enumerate by hand, so
//    firing random messages and asserting invariants afterwards finds
//    things directed tests do not think to check.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "feed/generator.hpp"
#include "hft/oms/manager.hpp"
#include "hft/risk/limits.hpp"

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

template <class A, class B>
void check_eq(const A& actual, const B& expected, const char* what) {
    ++g_checks;
    if (!(actual == static_cast<A>(expected))) {
        ++g_failures;
        std::printf("  FAIL  %s: expected %lld, got %lld\n", what,
                    static_cast<long long>(expected), static_cast<long long>(actual));
    }
}

using hft::Price;
using hft::Quantity;
using hft::Side;
namespace risk = hft::risk;
namespace oms = hft::oms;

constexpr Price kRef = Price::from_raw(1'000'000);  // $100.00

/// Limits for tests that are about position, rate or lifecycle.
///
/// The notional cap is deliberately generous here. An earlier revision
/// left it tight, which meant every position test tripped the notional
/// check first and the position check was never actually reached: the
/// test passed for the wrong reason, then failed once the cap was
/// noticed. Each test should fail for exactly one reason.
risk::Limits small_limits() {
    risk::Limits l;
    l.max_position_per_side = 1'000;
    l.max_abs_position = 1'500;
    l.max_order_notional_raw = 1'000'000'000;  // $100,000
    l.max_price_deviation_raw = 10'000;        // $1.00
    l.max_order_rate = 100;
    return l;
}

/// Limits with the notional cap as the binding constraint.
/// 20 shares at $100.00 is 20 * 1,000,000 raw = $2,000, so this cap
/// is exactly one boundary case wide.
risk::Limits tight_notional_limits() {
    risk::Limits l = small_limits();
    l.max_order_notional_raw = 20'000'000;  // $2,000
    return l;
}

// ---- Risk -----------------------------------------------------------

void test_risk_basics() {
    std::printf("risk: limits\n");
    risk::PreTradeRisk r(small_limits());
    r.set_reference_price(kRef);

    check(r.check(Side::bid, kRef, Quantity::from_raw(100), 1'000'000'000ULL).allowed(),
          "a small order inside every limit passes");

    // Fat finger: 5 dollars away with a $1 band.
    check_eq(static_cast<int>(r.check(Side::bid, Price::from_raw(1'500'000),
                                      Quantity::from_raw(1), 1'000'000'001ULL)
                                   .status),
             static_cast<int>(risk::LimitStatus::price_band), "fat finger hits the price band");
    check_eq(static_cast<int>(r.check(Side::bid, Price::from_raw(500'000),
                                      Quantity::from_raw(1), 1'000'000'002ULL)
                                   .status),
             static_cast<int>(risk::LimitStatus::price_band),
             "the band is symmetric below the reference too");

    // Notional is price_raw * qty in raw 1/10000 units, so 20 shares
    // at $100.00 is 20 * 1,000,000 = 20,000,000 raw, which is $2,000.
    // The cap below is $2,000, and the check rejects only when the
    // notional is strictly greater, so the boundary must pass.
    {
        risk::PreTradeRisk rn(tight_notional_limits());
        rn.set_reference_price(kRef);
        check(rn.check(Side::bid, kRef, Quantity::from_raw(20), 1'000'000'003ULL).allowed(),
              "an order exactly at the notional cap passes");
        check_eq(static_cast<int>(rn.check(Side::bid, kRef, Quantity::from_raw(21),
                                           1'000'000'004ULL)
                                      .status),
                 static_cast<int>(risk::LimitStatus::order_notional),
                 "one share over the cap is refused");
    }

    // Zero size has no meaning and is refused rather than admitted.
    check(!r.check(Side::bid, kRef, Quantity{}, 1'000'000'004ULL).allowed(),
          "zero size is refused");
}

void test_risk_no_reference() {
    std::printf("risk: missing reference price\n");
    risk::PreTradeRisk r(small_limits());
    // No reference set. Refusing is the safe default: quoting without a
    // reference is quoting against nothing.
    check_eq(static_cast<int>(r.check(Side::bid, kRef, Quantity::from_raw(1), 0).status),
             static_cast<int>(risk::LimitStatus::no_reference_price),
             "orders are refused with no reference price");
    r.set_reference_price(kRef);
    check(r.check(Side::bid, kRef, Quantity::from_raw(1), 0).allowed(),
          "and permitted once one arrives");
}

void test_risk_positions() {
    std::printf("risk: position and gross\n");
    risk::PreTradeRisk r(small_limits());
    r.set_reference_price(kRef);

    // Fill into a position, then try to exceed it.
    r.on_fill(Side::bid, Quantity::from_raw(900));
    check_eq(r.position(Side::bid), 900u, "bid position tracks fills");
    check_eq(r.net_position(), 900, "net position is long");
    check_eq(r.gross_position(), 900u, "gross matches");

    check_eq(static_cast<int>(r.check(Side::bid, kRef, Quantity::from_raw(200),
                                      1'000'000'000ULL)
                                   .status),
             static_cast<int>(risk::LimitStatus::position_limit), "per-side cap is enforced");

    // Each side is under its own cap, but together they breach gross.
    r.on_fill(Side::ask, Quantity::from_raw(900));
    check_eq(r.net_position(), 0, "offsetting sides net to zero");
    check_eq(r.gross_position(), 1'800u, "gross is the sum, not the net");
    check_eq(static_cast<int>(r.check(Side::ask, kRef, Quantity::from_raw(1),
                                      1'000'000'001ULL)
                                   .status),
             static_cast<int>(risk::LimitStatus::gross_limit),
             "gross cap binds even when each side is under its own");

    r.on_unfill(Side::ask, Quantity::from_raw(900));
    check_eq(r.gross_position(), 900u, "unfill restores the position");
    // Saturating, never negative.
    r.on_unfill(Side::ask, Quantity::from_raw(9'999));
    check_eq(r.gross_position(), 900u, "over-unfill saturates at zero rather than wrapping");
}

void test_risk_kill_switch() {
    std::printf("risk: kill switch\n");
    risk::PreTradeRisk r(small_limits());
    r.set_reference_price(kRef);

    check(r.check(Side::bid, kRef, Quantity::from_raw(10), 1'000'000'000ULL).allowed(),
          "orders pass before the switch is engaged");

    r.trip("feed gap detected");
    check(r.kill_switch_engaged(), "switch engaged");
    check_eq(static_cast<int>(r.check(Side::bid, kRef, Quantity::from_raw(10),
                                      1'000'000'001ULL)
                                   .status),
             static_cast<int>(risk::LimitStatus::kill_switch), "orders refused after");

    // The switch must outrank every other reason, so an operator sees
    // "kill switch" rather than a limit that happens to bind first.
    check_eq(static_cast<int>(r.check(Side::bid, Price::from_raw(9'000'000),
                                      Quantity::from_raw(99'999), 1'000'000'002ULL)
                                   .status),
             static_cast<int>(risk::LimitStatus::kill_switch),
             "kill switch outranks price band, position and rate");

    // It latches. Only an explicit reset clears it.
    r.reset_kill_switch();
    check(!r.kill_switch_engaged(), "explicit reset clears the switch");
    check(r.check(Side::bid, kRef, Quantity::from_raw(10), 1'000'000'003ULL).allowed(),
          "orders pass again after reset");
    check(std::string(r.kill_switch_reason()) == "feed gap detected",
          "the reason survives for the operator");
}

void test_risk_rate_limit() {
    std::printf("risk: rate limit\n");
    risk::Limits l = small_limits();
    l.max_order_rate = 10;  // 10 per second
    risk::PreTradeRisk r(l);
    r.set_reference_price(kRef);
    r.on_fill(Side::bid, Quantity{});  // no position effect

    constexpr hft::Nanos kT0 = 1'000'000'000ULL;
    int allowed = 0;
    for (int i = 0; i < 50; ++i) {
        if (r.check(Side::bid, kRef, Quantity::from_raw(1), kT0).allowed()) {
            ++allowed;
        }
    }
    // One full burst is admitted at t0, then the limiter bites.
    check_eq(allowed, 10, "a full burst is admitted, then the bucket empties");

    // Time moves; tokens return. Injected, not read, so this is exact.
    const int after_wait = [&] {
        int n = 0;
        for (int i = 0; i < 5; ++i) {
            if (r.check(Side::bid, kRef, Quantity::from_raw(1),
                        kT0 + 500'000'000ULL /* +0.5s */)
                    .allowed()) {
                ++n;
            }
        }
        return n;
    }();
    if (after_wait < 4 || after_wait > 5) {
        std::printf("  (rate limiter granted %d of 5 after 0.5s at 10/s)\n", after_wait);
    }
    check(after_wait >= 4 && after_wait <= 5,
          "half a second buys about half the rate back");

    // Time going backwards must not mint tokens.
    const hft::Nanos kBack = kT0 + 100'000'000ULL;
    int allowed_before = 0;
    for (int i = 0; i < 5; ++i) {
        if (r.check(Side::bid, kRef, Quantity::from_raw(1), kBack).allowed()) {
            ++allowed_before;
        }
    }
    int allowed_after = 0;
    for (int i = 0; i < 5; ++i) {
        if (r.check(Side::bid, kRef, Quantity::from_raw(1), kBack - 50'000'000ULL).allowed()) {
            ++allowed_after;
        }
    }
    check(allowed_after <= allowed_before, "a clock stepping backwards mints no tokens");
}

void test_risk_notional_overflow() {
    std::printf("risk: notional overflow\n");
    risk::Limits l = small_limits();
    l.max_order_notional_raw = INT64_MAX;  // remove the cap entirely
    risk::PreTradeRisk r(l);
    r.set_reference_price(kRef);

    // A price and size whose true product does not fit in 64 bits must
    // be refused, not wrapped into a small number that sails past the
    // cap. This is the whole reason the multiply is checked.
    const Price absurd = Price::from_raw(INT64_MAX / 2);
    const risk::Decision d =
        r.check(Side::bid, absurd, Quantity::from_raw(UINT32_MAX), 1'000'000'000ULL);
    check(!d.allowed(), "an unrepresentable notional is refused");
    check(d.status == risk::LimitStatus::price_band || d.status == risk::LimitStatus::order_notional,
          "and is refused for a reason, not by accident");
}

// ---- Transition table ------------------------------------------------

void test_transition_table() {
    std::printf("oms: transition table\n");

    // Written out independently of the implementation so the two can
    // disagree. An exhaustive 8x8 check against a hardcoded matrix
    // catches a single wrong edge that directed tests would miss.
    struct Edge {
        oms::OrdState from;
        oms::OrdState to;
        bool allowed;
    };
    const Edge kExpected[] = {
        {oms::OrdState::pending_new, oms::OrdState::working, true},
        {oms::OrdState::pending_new, oms::OrdState::partially_filled, true},
        {oms::OrdState::pending_new, oms::OrdState::filled, true},
        {oms::OrdState::pending_new, oms::OrdState::cancelled, true},
        {oms::OrdState::pending_new, oms::OrdState::rejected, true},
        {oms::OrdState::pending_new, oms::OrdState::pending_cancel, false},
        {oms::OrdState::pending_new, oms::OrdState::pending_replace, false},

        {oms::OrdState::working, oms::OrdState::working, false},
        {oms::OrdState::working, oms::OrdState::partially_filled, true},
        {oms::OrdState::working, oms::OrdState::filled, true},
        {oms::OrdState::working, oms::OrdState::pending_cancel, true},
        {oms::OrdState::working, oms::OrdState::pending_replace, true},
        {oms::OrdState::working, oms::OrdState::cancelled, true},
        {oms::OrdState::working, oms::OrdState::rejected, false},

        {oms::OrdState::pending_cancel, oms::OrdState::partially_filled, true},
        {oms::OrdState::pending_cancel, oms::OrdState::filled, true},
        {oms::OrdState::pending_cancel, oms::OrdState::cancelled, true},
        {oms::OrdState::pending_cancel, oms::OrdState::rejected, true},
        {oms::OrdState::pending_cancel, oms::OrdState::pending_cancel, false},
        {oms::OrdState::pending_cancel, oms::OrdState::working, false},
        {oms::OrdState::pending_cancel, oms::OrdState::pending_replace, false},

        {oms::OrdState::pending_replace, oms::OrdState::working, true},
        {oms::OrdState::pending_replace, oms::OrdState::partially_filled, true},
        {oms::OrdState::pending_replace, oms::OrdState::filled, true},
        {oms::OrdState::pending_replace, oms::OrdState::cancelled, true},
        {oms::OrdState::pending_replace, oms::OrdState::rejected, true},
        {oms::OrdState::pending_replace, oms::OrdState::pending_replace, false},
        {oms::OrdState::pending_replace, oms::OrdState::pending_cancel, false},

        {oms::OrdState::partially_filled, oms::OrdState::partially_filled, true},
        {oms::OrdState::partially_filled, oms::OrdState::filled, true},
        {oms::OrdState::partially_filled, oms::OrdState::pending_cancel, true},
        {oms::OrdState::partially_filled, oms::OrdState::cancelled, true},
        {oms::OrdState::partially_filled, oms::OrdState::working, false},
        {oms::OrdState::partially_filled, oms::OrdState::pending_replace, false},
        {oms::OrdState::partially_filled, oms::OrdState::rejected, false},

        {oms::OrdState::filled, oms::OrdState::working, false},
        {oms::OrdState::filled, oms::OrdState::partially_filled, false},
        {oms::OrdState::filled, oms::OrdState::cancelled, false},
        {oms::OrdState::filled, oms::OrdState::rejected, false},
        {oms::OrdState::filled, oms::OrdState::filled, false},

        {oms::OrdState::cancelled, oms::OrdState::working, false},
        {oms::OrdState::cancelled, oms::OrdState::filled, false},
        {oms::OrdState::cancelled, oms::OrdState::cancelled, false},

        {oms::OrdState::rejected, oms::OrdState::working, false},
        {oms::OrdState::rejected, oms::OrdState::filled, false},
        {oms::OrdState::rejected, oms::OrdState::cancelled, false},
    };

    for (const Edge& e : kExpected) {
        const bool actual = oms::can_transition(e.from, e.to);
        ++g_checks;
        if (actual != e.allowed) {
            ++g_failures;
            std::printf("  FAIL  %s -> %s: expected %d, got %d\n", oms::to_string(e.from),
                        oms::to_string(e.to), e.allowed ? 1 : 0, actual ? 1 : 0);
        }
    }

    // Terminal is terminal, for every target.
    for (oms::OrdState s : {oms::OrdState::filled, oms::OrdState::cancelled,
                             oms::OrdState::rejected}) {
        for (int t = 0; t < 8; ++t) {
            check(!oms::can_transition(s, static_cast<oms::OrdState>(t)),
                  "terminal states accept no transition");
        }
    }
}

// ---- OMS lifecycle ---------------------------------------------------

/// A test helper that refuses to hand back a slot it did not get.
///
/// Every OMS test below needs a live slot. If `submit` refuses one, the
/// correct outcome is a reported failure, not a null dereference
/// several lines later: an earlier revision crashed the whole binary
/// this way, which hid the real cause behind a heap fault.
///
/// Returns -1 on failure. Callers check.
[[nodiscard]] int submit_or_fail(oms::Manager& m, Side side, Price price, Quantity size,
                                 hft::Nanos now, const char* what) {
    const int slot = m.submit(side, price, size, now);
    check(slot >= 0, what);
    return slot;
}

oms::Manager make_manager(std::size_t capacity = 1'024) {
    return oms::Manager(capacity, small_limits());
}

void test_oms_lifecycle() {
    std::printf("oms: lifecycle\n");
    oms::Manager m = make_manager();
    m.set_reference_price(kRef);

    const int slot = submit_or_fail(m, Side::bid, kRef, Quantity::from_raw(100), 1'000,
                                    "a valid order is accepted");
    if (slot < 0) {
        return;
    }
    check_eq(static_cast<int>(m.at(slot)->state),
             static_cast<int>(oms::OrdState::pending_new), "it starts pending_new");

    check_eq(static_cast<int>(m.on_ack(slot, 5'000'001, 1'001)),
             static_cast<int>(oms::OmsStatus::ok), "ack accepted");
    check_eq(static_cast<int>(m.at(slot)->state), static_cast<int>(oms::OrdState::working),
             "it becomes working");
    check_eq(m.at(slot)->venue_ref, 5'000'001u, "the venue reference is recorded");

    check_eq(static_cast<int>(m.on_fill(slot, Quantity::from_raw(40), kRef, 1'002)),
             static_cast<int>(oms::OmsStatus::ok), "partial fill accepted");
    check_eq(static_cast<int>(m.at(slot)->state),
             static_cast<int>(oms::OrdState::partially_filled), "it becomes partially_filled");
    check_eq(m.at(slot)->leaves_qty.raw(), 60u, "60 leaves");
    check_eq(m.risk().position(Side::bid), 40u,
             "the position comes from the fill, not the resting size");

    check_eq(static_cast<int>(m.on_fill(slot, Quantity::from_raw(60), kRef, 1'003)),
             static_cast<int>(oms::OmsStatus::ok), "completing fill accepted");
    check_eq(static_cast<int>(m.at(slot)->state), static_cast<int>(oms::OrdState::filled),
             "it becomes filled");
    check_eq(m.risk().position(Side::bid), 100u, "the full order is now the position");

    // A late message on a terminal order must not resurrect it.
    check(m.on_fill(slot, Quantity::from_raw(1), kRef, 1'004) == oms::OmsStatus::illegal_transition,
          "a fill on a filled order is refused");
    check_eq(m.risk().position(Side::bid), 100u, "and does not move the position");
}

void test_oms_fill_races_cancel() {
    std::printf("oms: fill races cancel\n");
    oms::Manager m = make_manager();
    m.set_reference_price(kRef);

    const int slot = submit_or_fail(m, Side::bid, kRef, Quantity::from_raw(100), 1'000,
                                    "submit for the cancel race");
    if (slot < 0) {
        return;
    }
    m.on_ack(slot, 1, 1'001);
    check_eq(static_cast<int>(m.request_cancel(slot, 1'002)), static_cast<int>(oms::OmsStatus::ok),
             "cancel requested");
    check_eq(static_cast<int>(m.at(slot)->state),
             static_cast<int>(oms::OrdState::pending_cancel), "it is pending_cancel");

    // The fill arrives before the cancel ack. This is the race that
    // leaks inventory in a naive OMS.
    check_eq(static_cast<int>(m.on_fill(slot, Quantity::from_raw(30), kRef, 1'003)),
             static_cast<int>(oms::OmsStatus::ok), "a fill lands while the cancel is pending");
    check_eq(static_cast<int>(m.at(slot)->state),
             static_cast<int>(oms::OrdState::partially_filled),
             "it becomes partially_filled, not cancelled");
    check_eq(m.at(slot)->leaves_qty.raw(), 70u, "70 still resting and still to be cancelled");

    // The cancel ack then lands and retires the remainder.
    check_eq(static_cast<int>(m.on_cancel_ack(slot, 1'004)),
             static_cast<int>(oms::OmsStatus::ok), "the cancel ack is accepted");
    check_eq(static_cast<int>(m.at(slot)->state), static_cast<int>(oms::OrdState::cancelled),
             "the remainder is cancelled");
    check_eq(m.risk().position(Side::bid), 30u, "only the filled part became position");
}

void test_oms_fill_beats_cancel() {
    std::printf("oms: fill beats cancel entirely\n");
    oms::Manager m = make_manager();
    m.set_reference_price(kRef);

    const int slot = submit_or_fail(m, Side::ask, kRef, Quantity::from_raw(50), 1'000,
                                    "submit for the fill-beats-cancel case");
    if (slot < 0) {
        return;
    }
    m.on_ack(slot, 1, 1'001);
    m.request_cancel(slot, 1'002);
    m.on_fill(slot, Quantity::from_raw(50), kRef, 1'003);
    check_eq(static_cast<int>(m.at(slot)->state), static_cast<int>(oms::OrdState::filled),
             "fully filled before the cancel ack");

    // The stale cancel ack must be reported, not treated as a fault and
    // not applied to a finished order.
    check(m.on_cancel_ack(slot, 1'004) == oms::OmsStatus::illegal_transition,
          "a cancel ack after a full fill is refused");
    check_eq(m.risk().net_position(), -50, "the short is real");
}

void test_oms_overfill() {
    std::printf("oms: over-fill\n");
    oms::Manager m = make_manager();
    m.set_reference_price(kRef);
    const int slot = submit_or_fail(m, Side::bid, kRef, Quantity::from_raw(10), 1'000,
                                    "submit for the over-fill case");
    if (slot < 0) {
        return;
    }
    m.on_ack(slot, 1, 1'001);

    check(m.on_fill(slot, Quantity::from_raw(11), kRef, 1'002) == oms::OmsStatus::illegal_transition,
          "a fill larger than leaves is refused");
    check_eq(m.at(slot)->leaves_qty.raw(), 10u, "leaves is unchanged");
    check_eq(m.risk().position(Side::bid), 0u, "and the position did not move");

    check(m.on_fill(slot, Quantity{}, kRef, 1'003) == oms::OmsStatus::illegal_transition,
          "a zero fill is refused");
}

void test_oms_risk_integration() {
    std::printf("oms: risk gates submission\n");
    oms::Manager m = make_manager();
    m.set_reference_price(kRef);

    check(m.submit(Side::bid, kRef, Quantity::from_raw(100), 1'000) >= 0, "good order accepted");
    // Over the per-order notional cap.
    check_eq(m.submit(Side::bid, kRef, Quantity::from_raw(1'000'000), 1'001), -1,
             "an order over the notional cap is refused at submit");

    m.kill_all("operator", 2'000);
    check(m.risk().kill_switch_engaged(), "kill_all engages the switch");
    check_eq(m.submit(Side::bid, kRef, Quantity::from_raw(1), 2'001), -1,
             "no further orders are accepted");

    // A working order at the moment of the kill must be cancelled, not
    // left resting. A switch that stops new orders but leaves working
    // ones is a suggestion, not a kill switch.
    m.risk().reset_kill_switch();
    m.risk().reset_positions();
    const int slot = submit_or_fail(m, Side::bid, kRef, Quantity::from_raw(50), 3'000,
                                    "submit before the second kill");
    if (slot < 0) {
        return;
    }
    m.on_ack(slot, 1, 3'001);
    check_eq(m.count(oms::OrdState::working), 1u, "one order working");
    const std::size_t cancelled_before = m.count(oms::OrdState::cancelled);
    m.kill_all("second incident", 3'100);
    check_eq(m.count(oms::OrdState::working), 0u, "kill_all cancelled the working order");
    check_eq(m.count(oms::OrdState::cancelled), cancelled_before + 1,
             "and it is recorded as cancelled");
}

void test_oms_slot_recycling() {
    std::printf("oms: slot recycling\n");
    oms::Manager m(4, small_limits());
    m.set_reference_price(kRef);

    std::vector<int> slots;
    for (int i = 0; i < 4; ++i) {
        const int s = m.submit(Side::bid, kRef, Quantity::from_raw(10), 1'000 + i);
        check(s >= 0, "filled a slot");
        m.on_ack(s, 100 + i, 1'000 + i);
        slots.push_back(s);
    }
    // Pool is full. Refusing beats evicting a live order, which would
    // strand inventory nobody is tracking.
    check_eq(m.submit(Side::bid, kRef, Quantity::from_raw(10), 2'000), -1,
             "a full pool refuses rather than evicting");

    m.on_fill(slots[0], Quantity::from_raw(10), kRef, 2'001);
    const int reused = m.submit(Side::bid, kRef, Quantity::from_raw(10), 2'002);
    check(reused >= 0, "a terminal order's slot is reusable");
    check_eq(m.at(reused)->leaves_qty.raw(), 10u, "the reused slot is fully reset, not stale");
    check_eq(m.at(reused)->original_size.raw(), 10u, "and its original size is reset too");
}

// ---- Randomised invariant test ---------------------------------------

void test_oms_random_invariants() {
    std::printf("oms: randomised invariants\n");

    constexpr std::size_t kOps = 60'000;
    oms::Manager m(256, small_limits());
    m.set_reference_price(kRef);
    hft::feed::SplitMix64 rng(0xBEEF'0000'1234'5678ULL);

    hft::Nanos clock = 1'000'000'000ULL;
    std::vector<int> live_slots;

    // Independent tally of what the OMS should believe, so the
    // assertion is against a second computation rather than the
    // object's own bookkeeping.
    std::uint64_t expected_bid_filled = 0;
    std::uint64_t expected_ask_filled = 0;

    for (std::size_t i = 0; i < kOps; ++i) {
        clock += 1'000'000u;  // 1ms per step
        const std::uint64_t roll = rng.below(100);

        if (roll < 35 || live_slots.empty()) {
            const Side side = (rng.next() & 1u) == 0u ? Side::bid : Side::ask;
            // Stay inside the price band most of the time, and step
            // outside it occasionally so the risk layer is exercised
            // rather than bypassed.
            const Price px = (roll < 5)
                                 ? Price::from_raw(kRef.raw() + 900'000)
                                 : Price::from_raw(kRef.raw() +
                                                   static_cast<std::int64_t>(rng.below(2'001)) -
                                                   1'000);
            const int s = m.submit(side, px, Quantity::from_raw(1 + rng.below(50)), clock);
            if (s >= 0) {
                live_slots.push_back(s);
            }
            continue;
        }

        const std::size_t pick = static_cast<std::size_t>(rng.below(live_slots.size()));
        const int slot = live_slots[pick];
        const oms::Order* o = m.at(slot);
        if (o == nullptr) {
            ++g_failures;
            std::printf("  FAIL  live slot %d vanished at op %zu\n", slot, i);
            return;
        }

        if (roll < 45) {
            m.on_ack(slot, 1'000'000 + i, clock);
        } else if (roll < 60) {
            m.request_cancel(slot, clock);
        } else if (roll < 90) {
            const std::uint64_t q = 1 + rng.below(o->leaves_qty.raw() == 0
                                                        ? 1
                                                        : o->leaves_qty.raw());
            const Side side = o->side;
            const oms::OmsStatus st = m.on_fill(slot, Quantity::from_raw(q), o->price, clock);
            if (st == oms::OmsStatus::ok) {
                if (side == Side::bid) {
                    expected_bid_filled += q;
                } else {
                    expected_ask_filled += q;
                }
            }
        } else {
            if (m.on_cancel_ack(slot, clock) == oms::OmsStatus::ok ||
                m.on_reject(slot, clock) == oms::OmsStatus::ok) {
                live_slots[pick] = live_slots.back();
                live_slots.pop_back();
            }
        }
    }

    // Drain, accepting whatever terminal state each order is in.
    for (int slot : live_slots) {
        const oms::Order* o = m.at(slot);
        while (o != nullptr && oms::is_live(o->state)) {
            const oms::OmsStatus before = m.on_fill(
                slot, Quantity::from_raw(o->leaves_qty.raw() == 0 ? 1 : o->leaves_qty.raw()),
                o->price, clock);
            if (before != oms::OmsStatus::ok) {
                m.on_cancel_ack(slot, clock);
                m.on_reject(slot, clock);
                break;
            }
            o = m.at(slot);
        }
    }

    // ---- Invariants ----
    check(m.counters_consistent(),
          "the state counters sum to the number of occupied slots");

    check_eq(m.risk().position(Side::bid), expected_bid_filled,
             "the OMS bid position equals an independent tally of fills");
    check_eq(m.risk().position(Side::ask), expected_ask_filled,
             "the OMS ask position equals an independent tally of fills");

    std::size_t still_live = 0;
    for (oms::OrdState s : {oms::OrdState::pending_new, oms::OrdState::working,
                            oms::OrdState::pending_cancel, oms::OrdState::pending_replace,
                            oms::OrdState::partially_filled}) {
        still_live += m.count(s);
    }
    check_eq(still_live, 0u, "no order is left in a live state after draining");

    check_eq(m.count(oms::OrdState::filled) + m.count(oms::OrdState::cancelled) +
                 m.count(oms::OrdState::rejected),
             m.submitted(),
             "every accepted order reached exactly one terminal state");
    check_eq(m.submitted() + m.risk_rejected() + m.pool_full(), m.submitted() + m.rejected(),
             "refusal accounting adds up");
    check(m.risk_rejected() > 0 || m.pool_full() > 0,
          "the random stream actually hit some refusals");

    check(m.risk().total_rejected() > 0, "the random stream actually hit some limits");}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered: a crash mid-suite would otherwise swallow every line
    // printed before it and leave nothing to diagnose from.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Optional substring filter, so a single failing group can be
    // rerun in isolation.
    const char* filter = argc > 1 ? argv[1] : nullptr;
    auto want = [filter](const char* name) {
        return filter == nullptr || std::string(name).find(filter) != std::string::npos;
    };

    std::printf("risk and OMS tests\n------------------\n");
    if (want("basics"))      test_risk_basics();
    if (want("no_reference")) test_risk_no_reference();
    if (want("positions"))   test_risk_positions();
    if (want("kill"))        test_risk_kill_switch();
    if (want("rate"))        test_risk_rate_limit();
    if (want("overflow"))    test_risk_notional_overflow();
    if (want("transition"))  test_transition_table();
    if (want("lifecycle"))   test_oms_lifecycle();
    if (want("races"))       test_oms_fill_races_cancel();
    if (want("beats"))       test_oms_fill_beats_cancel();
    if (want("overfill"))    test_oms_overfill();
    if (want("integration")) test_oms_risk_integration();
    if (want("recycling"))   test_oms_slot_recycling();
    if (want("random"))      test_oms_random_invariants();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
