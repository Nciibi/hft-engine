// Tests for symbol sharding.
//
// The thing being tested is ROUTING, and routing is the part of a
// multi-symbol book that can be wrong without anything crashing. A
// mutation applied to the wrong book produces no error, no exception
// and no gap in the sequence. It just quietly corrupts one instrument
// while another looks perfectly healthy, and the corruption surfaces
// much later as an inventory reconciliation failure.
//
// So the central test here is differential. Two independent routing
// strategies are driven over the same capture into two separate sets of
// books, and the books are compared state for state:
//
//   ORACLE    a std::map from order reference number to symbol. Slow,
//             linear in the number of orders, and correct by
//             inspection: it cannot lose an entry or misprobe.
//
//   REF INDEX include/hft/lob/shards.hpp's open-addressed table.
//
// They share no data structure. If the fast table returns the wrong
// symbol for any reference over this capture, the two books diverge and
// the test says which symbol and which step.
//
// Note that the oracle and the fast path are NOT allowed to share the
// routing logic either: the oracle derives a record's symbol from the
// reference map alone, because only the Add that created the reference
// carried a symbol. That is exactly the constraint the real system is
// under, so the test is under it too.

#include <cstdio>
#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/lob/shards.hpp"
#include "hft/types.hpp"

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

void check_eq_size(std::size_t actual, std::size_t expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL  %s: expected %zu, got %zu\n", what, expected, actual);
    }
}

using hft::OrderId;
using hft::Price;
using hft::Side;
namespace itch = hft::itch;
namespace lob = hft::lob;

/// The order reference a mutation names.
///
/// Every decoded order-level message carries one in a field called `id`,
/// which is why this is a single visit rather than a switch: a new
/// decoded type would fail to compile here instead of silently routing
/// by an unset reference.
[[nodiscard]] OrderId reference_of(const itch::Message& message) noexcept {
    return std::visit(
        [](const auto& payload) noexcept -> OrderId {
            if constexpr (requires { payload.id; }) {
                return payload.id;
            } else {
                return hft::kInvalidOrderId;
            }
        },
        message.body);
}

// ---- Symbol ---------------------------------------------------------

void test_symbol() {
    std::printf("Symbol\n");

    const lob::Symbol aapl = lob::Symbol::from_wire("AAPL    ");
    check(aapl == lob::Symbol::from_wire("AAPL    "), "padding normalises: 'AAPL    ' equals itself");

    // Case folding. The venue upper-cases symbols and feeds are not
    // consistent about it; a case-sensitive compare would split one
    // instrument into two half-empty books.
    const lob::Symbol lower = lob::Symbol::from_wire("aapl    ");
    check(aapl == lower, "comparison is case-insensitive");

    const lob::Symbol amzn = lob::Symbol::from_wire("AMZN    ");
    check(!(aapl == amzn), "different symbols are different");

    check_eq_size(aapl.length(), 4, "trailing spaces are stripped from the length");
    check_eq_size(lob::Symbol{}.length(), 0, "a default Symbol is empty");
    check(lob::Symbol{}.empty(), "a default Symbol reports empty");

    // A shorter name must not match a longer one that shares its
    // prefix. Comparing only up to the shorter length would make
    // "AAPL" and "AAPLX" the same instrument.
    const lob::Symbol aap = lob::Symbol::from_wire("AAP     ");
    check(!(aap == aapl), "a prefix is not the same symbol");

    // An all-space field is what a null-filled symbol decodes to.
    const lob::Symbol blank = lob::Symbol::from_wire("        ");
    check(blank.empty(), "an all-space field is empty, not a symbol called '        '");
}

// ---- Shard assignment -----------------------------------------------

void test_shard_assignment() {
    std::printf("shard assignment\n");

    const std::size_t shards = 8;
    const lob::Symbol a = lob::Symbol::from_wire("SYM00000");

    const std::size_t first = lob::shard_of(a, shards);
    const std::size_t again = lob::shard_of(a, shards);
    check(first == again, "a symbol always maps to the same shard");
    check(first < shards, "the shard is in range");

    // Not a correctness requirement -- collisions are expected and
    // handled -- but a total absence of distribution would mean the
    // hash is broken, which would concentrate all the load on one shard.
    int distinct = 0;
    for (std::size_t s = 0; s < 64; ++s) {
        const lob::Symbol sym = lob::Symbol::from_wire(hft::feed::symbol_name(s).c_str());
        const std::size_t shard = lob::shard_of(sym, shards);
        bool seen = false;
        for (std::size_t t = 0; t < s; ++t) {
            const lob::Symbol other = lob::Symbol::from_wire(hft::feed::symbol_name(t).c_str());
            if (lob::shard_of(other, shards) == shard) {
                seen = true;
            }
        }
        if (!seen) {
            ++distinct;
        }
    }
    check(distinct >= 4, "the hash spreads 64 symbols over at least 4 of 8 shards");
}

// ---- ShardSet -------------------------------------------------------

void test_shard_set() {
    std::printf("ShardSet\n");

    lob::ShardSet shards(1'024, 256);
    check_eq_size(shards.symbol_count(), 0, "a fresh set holds no symbols");
    check_eq_size(shards.find(lob::Symbol::from_wire("AAPL    ")), lob::ShardSet::kNotFound,
                  "an unclaimed symbol is not found");

    const lob::Symbol aapl = lob::Symbol::from_wire("AAPL    ");
    const std::size_t index = shards.claim(aapl);
    check_eq_size(index, 0, "the first claimed symbol gets index 0");
    check_eq_size(shards.symbol_count(), 1, "claiming one symbol builds one book");

    // Idempotent. This is the property that stops a second Add for the
    // same instrument from creating a second book, which would leave
    // both books permanently incomplete.
    check_eq_size(shards.claim(aapl), index, "claiming the same symbol again returns the same index");
    check_eq_size(shards.symbol_count(), 1, "and does not build a second book");

    const std::size_t amzn = shards.claim(lob::Symbol::from_wire("AMZN    "));
    check(amzn != index, "a different symbol gets a different index");
    check_eq_size(shards.symbol_count(), 2, "and a second book");

    // Case folding must apply to claiming too, not just to equality.
    check_eq_size(shards.claim(lob::Symbol::from_wire("aapl    ")), index,
                  "claiming is case-insensitive");

    check(shards.owns(aapl), "owns reports a claimed symbol");
    check(!shards.owns(lob::Symbol::from_wire("TSLA    ")), "owns reports an unclaimed symbol");

    // The books must be usable and independent.
    hft::lob::BookStatus st{};
    shards.book(index).add(Side::bid, Price::from_int(10), hft::Quantity::from_raw(100), 1, st);
    check(st == hft::lob::BookStatus::ok, "the claimed symbol's book accepts an order");
    check_eq_size(shards.book(amzn).order_count(Side::bid), 0,
                  "the other symbol's book is unaffected");
}

// ---- RefIndex -------------------------------------------------------

void test_ref_index() {
    std::printf("RefIndex\n");

    // Sized for the entries the test will insert, not for the entries
    // it happens to want. The index rounds up to a power of two AND
    // holds at most half of its slots, so `RefIndex(n)` comfortably
    // holds `n` and `RefIndex(2n)` does not. Asking for the wrong one
    // fails with a wall of identical messages, which is its own kind of
    // unhelpful.
    constexpr OrderId kEntries = 2'000;
    lob::RefIndex index(kEntries * 2);
    check(!index.full(), "a fresh index is not full");
    check_eq_size(index.size(), 0, "and is empty");
    check(index.slot_count() >= kEntries * 2,
          "the table is at least twice the entries it must hold");

    const OrderId kBase = 1'000'000;
    bool all_inserted = true;
    for (OrderId r = 0; r < kEntries; ++r) {
        if (!index.insert(kBase + r, static_cast<std::size_t>(r % 7))) {
            all_inserted = false;
        }
    }
    check(all_inserted, "every insert below the load factor succeeds");
    check_eq_size(index.size(), kEntries, "every insert is counted");

    // Every reference must come back with the owner it was given. A
    // single wrong answer here is an order mutated on the wrong book.
    // Aggregated into one check so a failure does not bury the rest of
    // the suite in two thousand identical lines.
    bool all_right = true;
    std::size_t first_wrong = 0;
    for (OrderId r = 0; r < kEntries; ++r) {
        std::size_t owner = 999;
        if (!index.lookup(kBase + r, owner) || owner != static_cast<std::size_t>(r % 7)) {
            all_right = false;
            if (first_wrong == 0) {
                first_wrong = r;
            }
        }
    }
    if (!all_right) {
        std::printf("    first wrong lookup at reference offset %llu\n",
                    static_cast<unsigned long long>(first_wrong));
    }
    check(all_right, "every reference returns the owner it was inserted with");

    std::size_t not_found = 0;
    for (OrderId r = kEntries; r < kEntries + 100; ++r) {
        std::size_t owner = 999;
        if (!index.lookup(kBase + r, owner)) {
            ++not_found;
        }
    }
    check_eq_size(not_found, 100, "references never inserted are reported as absent");

    // Idempotent for the same owner, and REFUSED for a different one.
    // Two symbols claiming one reference is a bug that must surface
    // rather than be silently overwritten.
    check(index.insert(kBase, 0), "re-inserting with the same owner is accepted");
    check(!index.insert(kBase, 3), "re-inserting with a different owner is refused");
    std::size_t owner = 999;
    check(index.lookup(kBase, owner) && owner == 0,
          "and the refused insert did not change the owner");

    // Reference 0 is offset by one internally, so it must still work.
    // It is a legal value and a sentinel collision here would lose a
    // real order.
    check(index.insert(0, 5), "reference 0 inserts");
    check(index.lookup(0, owner) && owner == 5, "reference 0 round trips");

    // A full table must say so rather than failing silently. A handler
    // whose index quietly stopped recording references is a handler
    // quietly losing mutations.
    bool reported_full = false;
    for (OrderId r = 5'000'000; r < 6'000'000; ++r) {
        if (!index.insert(r, 1)) {
            reported_full = true;
            break;
        }
    }
    check(reported_full, "a full index refuses inserts instead of overwriting");
    check(index.full(), "and reports itself full");
}

// ---- The differential test ------------------------------------------

/// Full state comparison between two books. Anything that differs is a
/// routing bug, so this compares levels, aggregates and individual
/// orders rather than just a summary.
[[nodiscard]] bool books_equal(const lob::OrderBook& a, const lob::OrderBook& b) {
    for (const Side side : {Side::bid, Side::ask}) {
        if (a.level_count(side) != b.level_count(side)) {
            return false;
        }
        if (a.order_count(side) != b.order_count(side)) {
            return false;
        }
        if (a.aggregate_at(side).raw() != b.aggregate_at(side).raw()) {
            return false;
        }
        const std::vector<lob::LevelSnapshot> la = a.levels(side);
        const std::vector<lob::LevelSnapshot> lb = b.levels(side);
        if (la.size() != lb.size()) {
            return false;
        }
        for (std::size_t i = 0; i < la.size(); ++i) {
            if (la[i].price != lb[i].price || la[i].aggregate_size.raw() != lb[i].aggregate_size.raw() ||
                la[i].order_count != lb[i].order_count) {
                return false;
            }
        }
        const std::vector<lob::OrderSnapshot> oa = a.orders(side);
        const std::vector<lob::OrderSnapshot> ob = b.orders(side);
        if (oa.size() != ob.size()) {
            return false;
        }
        for (std::size_t i = 0; i < oa.size(); ++i) {
            if (oa[i].id != ob[i].id || oa[i].price != ob[i].price ||
                oa[i].size.raw() != ob[i].size.raw() || oa[i].state != ob[i].state) {
                return false;
            }
        }
    }
    return true;
}

void test_routing_matches_oracle() {
    std::printf("routing: fast ref index vs std::map oracle\n");

    constexpr std::size_t kSymbols = 12;
    constexpr std::size_t kRecords = 60'000;

    hft::feed::CaptureConfig config;
    config.record_count = kRecords;
    config.symbol_count = kSymbols;
    config.price_levels = 8;
    config.max_live_orders = 200;
    config.drift_raw = 400;
    config.reversion = 4;

    hft::feed::CaptureStats stats{};
    const std::vector<std::uint8_t> data = hft::feed::generate_capture(config, &stats);
    check_eq_size(stats.symbols, kSymbols, "the capture really does carry the symbols requested");

    const std::size_t pool = 4'000;

    // ORACLE. Reference number to symbol index, in a std::map. Correct by
    // construction: it cannot misprobe and it cannot be full.
    lob::ShardSet oracle_books(pool, 512);
    std::map<OrderId, std::size_t> oracle;

    // FAST PATH. The open-addressed table from shards.hpp.
    lob::ShardSet fast_books(pool, 512);
    lob::RefIndex fast_index(kRecords);

    std::size_t routed_by_symbol = 0;
    std::size_t routed_by_index = 0;
    std::size_t unknown_reference = 0;
    std::size_t index_full = 0;
    std::size_t divergence_step = 0;
    std::size_t divergence_symbol = 0;
    bool diverged = false;

    std::size_t offset = 0;
    std::size_t step = 0;
    while (offset < data.size()) {
        if (data.size() - offset < hft::feed::kCaptureSequenceSize + itch::kLengthPrefixSize) {
            break;
        }
        const std::size_t frame_at = offset + hft::feed::kCaptureSequenceSize;
        const itch::DecodeResult r = itch::decode(data.data() + frame_at, data.size() - frame_at);
        const std::size_t stride = itch::frame_stride(r);
        if (stride == 0) {
            break;
        }

        if (r.ok()) {
            const itch::AddOrder* add = std::get_if<itch::AddOrder>(&r.message.body);
            if (add != nullptr) {
                // Only an Add carries a symbol. Both paths learn the
                // mapping here and nowhere else, which is the whole
                // constraint.
                const lob::Symbol sym = lob::Symbol::from_wire(add->stock);
                const std::size_t oi = oracle_books.claim(sym);
                const std::size_t fi = fast_books.claim(sym);
                oracle[add->id] = oi;
                if (!fast_index.insert(add->id, fi)) {
                    ++index_full;
                }
                (void)lob::apply(r.message, oracle_books.book(oi));
                (void)lob::apply(r.message, fast_books.book(fi));
                ++routed_by_symbol;
            } else {
                const OrderId ref = reference_of(r.message);
                if (ref == hft::kInvalidOrderId) {
                    ++unknown_reference;
                } else {
                    std::size_t oi = 0;
                    std::size_t fi = 0;
                    const bool in_oracle = oracle.count(ref) != 0;
                    if (in_oracle) {
                        oi = oracle[ref];
                    }
                    const bool in_fast = fast_index.lookup(ref, fi);
                    if (in_oracle && in_fast) {
                        (void)lob::apply(r.message, oracle_books.book(oi));
                        (void)lob::apply(r.message, fast_books.book(fi));
                        ++routed_by_index;
                    } else {
                        ++unknown_reference;
                    }
                }
            }

            // Compare after EVERY record, not once at the end. A wrong
            // route that is later corrected would still be a bug, and
            // only per-step comparison sees it.
            ++step;
            if (!diverged) {
                for (std::size_t s = 0; s < oracle_books.symbol_count(); ++s) {
                    if (!books_equal(oracle_books.book(s), fast_books.book(s))) {
                        diverged = true;
                        divergence_step = step;
                        divergence_symbol = s;
                        break;
                    }
                }
            }
        }
        offset = frame_at + stride;
    }

    std::printf("    %zu records, %zu symbols, adds %zu, routed by index %zu, unknown %zu\n", step,
                oracle_books.symbol_count(), routed_by_symbol, routed_by_index, unknown_reference);

    if (diverged) {
        std::printf("    divergence at record %zu, symbol index %zu\n", divergence_step,
                    divergence_symbol);
    }

    check(!diverged, "the fast ref index and the oracle produce identical books at every step");
    check_eq_size(index_full, 0, "the ref index never filled");
    check_eq_size(unknown_reference, 0, "no mutation arrived for an unknown reference");
    check_eq_size(oracle_books.symbol_count(), fast_books.symbol_count(),
                  "both paths claim the same number of symbols");
    check(oracle_books.symbol_count() > 1, "the capture really is multi-symbol");
    check(routed_by_index > 0,
          "mutations were routed by reference number, not by decoding a symbol that was not there");
}

}  // namespace

int main() {
    std::printf("sharding tests\n----------------\n");
    test_symbol();
    test_shard_assignment();
    test_shard_set();
    test_ref_index();
    test_routing_matches_oracle();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}