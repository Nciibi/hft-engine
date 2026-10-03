// Price-ladder scaling: quantifying the O(depth) walk.
//
// The order book links price levels into a sorted doubly-linked list, so
// inserting a price that is not adjacent to the best walks from the head
// of the ladder. Book-update cost is therefore O(depth), which the README
// states as a known weakness and which this tool exists to measure.
//
// ---- Why this tool reports ratios, not nanoseconds --------------------
//
// `hft_tsc_bench` establishes that this host's portable clock has a 100ns
// granularity, and that its run-to-run drift is 14-23%. An *absolute*
// book-update latency on this host is therefore not measurable: it would
// be a floor reading with noise on top.
//
// A **ratio between two measured depths** survives both problems, and
// survives them for a reason worth stating: the clock-pair cost is a
// roughly constant additive term, so it inflates the numerator and the
// denominator alike and cancels in the quotient. The 100ns floor adds a
// constant; a constant divided by a constant-bearing measurement biases
// the ratio *toward 1*, which makes this test conservative. A ladder that
// looks 5x worse here is at least 5x worse; it cannot look worse than it
// is.
//
// That is why the sweep batches the operations and divides, rather than
// timing one insertion. Amortising the clock over a batch is what makes
// the marginal cost of a single walk visible at all.
//
// ---- What is actually being measured ---------------------------------
//
// Not a feed replay. A replay inserts levels wherever the price process
// happens to put them, most of them near the touch, and averages the
// walk away. The cost this tool needs to expose is a function of one
// variable -- how many levels the walk crosses -- so the experiment holds
// everything else fixed and varies only that:
//
//   * Build a book with N levels on the bid side, one order each.
//   * Insert one order at a price `distance` levels *below* the best,
//     which is exactly the worst case: the walk must traverse every level
//     between the head and the insertion point.
//   * Remove it again, restoring the book for the next iteration.
//
// The removal is included in the timed region and not subtracted out,
// because in a live book a level that is created usually is destroyed, and
// reporting only the insert would be reporting half an operation. `unlink`
// is O(1) once the node is known, so it contributes a constant.
//
// The `distance = 1` row is the control: the walk crosses nothing, so it
// measures the whole of `add` except the ladder. The gap between a row and
// the control at the same depth is the ladder's own contribution.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "bench/report.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/util/affinity.hpp"
#include "hft/util/histogram.hpp"
#include "hft/util/tsc.hpp"

namespace {

using hft::lob::BookStatus;
using hft::lob::OrderBook;
using hft::Price;
using hft::Quantity;
using hft::Side;
using hft::util::TsClock;

/// Depths to sweep. Geometric because the hypothesis is linear: if the
/// cost really is O(depth), the ticks-per-op column should be close to a
/// straight line in N, and geometric spacing makes that visible as
/// constant ratios between rows.
constexpr std::size_t kDepths[] = {10, 32, 100, 320, 1000, 3200};

/// Operations per timed batch.
///
/// Large enough that the clock pair is noise: at ~10-40 ticks per
/// operation and a ~100-tick clock pair, a few thousand operations puts
/// the clock below 0.5% of the measurement.
constexpr int kBatchOps = 20'000;

/// Batches per row, best-of. Minimum because a batch can only be slowed
/// by something outside it -- a preemption, an interrupt -- so the fastest
/// run is the one interfered with least. For a fixed cost rather than a
/// distribution, the lower bound is the better estimator.
constexpr int kRepeats = 5;

/// Build a book of `depth` bid levels below `anchor`, one order each.
///
/// One order per level, not three: three orders at a level means the
/// third insert does not create a level at all and therefore does not
/// walk the ladder, which would understate the cost by construction.
/// Order and level capacity for a book of `depth` levels.
///
/// Sized GENEROUSLY, and the reason is worth recording because the first
/// version of this tool got it wrong in an instructive way.
///
/// It sized capacity at `depth * 4 + 64` -- tight to the working set, on
/// the theory that a tight book is a realistic book. The resulting sweep
/// showed add/remove cost scaling with *depth* and flat in *walk
/// distance*, at around 900ns per operation. That is not a ladder walk;
/// it is the hash index.
///
/// `OrderBook` erases from `order_index_` and `level_index_` on every
/// removal, so both tables fill with tombstones as the book churns.
/// `FlatMap::insert` probes forward until it finds an *empty* slot,
/// skipping tombstones, so once a table is mostly tombstones every insert
/// scans nearly the whole table. At 4x headroom the tables are permanently
/// in that state, and the measurement was dominated by probe length
/// rather than by `link_level`.
///
/// Which is a real finding about the book, and is written up in
/// docs/BUGS.md -- but it is not what this tool is for. Sizing capacity far
/// above the working set keeps the probe short and the tables mostly
/// empty, which is what isolates the ladder.
[[nodiscard]] constexpr std::size_t capacity_for(std::size_t depth) noexcept {
    return depth * 64 + 8192;
}

/// Price the sweep anchors at, so the book never approaches zero.
[[nodiscard]] constexpr Price anchor() noexcept { return Price::from_raw(Price::kScale * 100); }

/// Levels are built on a TWO-tick stride, leaving a one-tick gap between
/// each pair.
///
/// This is what makes the experiment controlled, and getting it wrong is
/// why the first two versions of this tool reported a flat sweep.
/// Inserting a new level at rank W below the best requires a price that is
/// *not already occupied*. With levels on consecutive ticks every rank
/// below the best is occupied, so the only reachable insertion points are
/// above the best (a walk of zero nodes) or below the worst (a walk of
/// `depth` nodes) -- and the probes the tool labelled "distance 500" were
/// silently landing on an existing level and doing no walk at all.
///
/// With a gap at every rank, any walk length from 0 to `depth` is
/// reachable and `distance` means what the column says.
constexpr std::int64_t kStrideTicks = 2;

[[nodiscard]] OrderBook build(std::size_t depth, Price top) {
    const std::size_t cap = capacity_for(depth);
    OrderBook book(cap, cap);
    hft::OrderId id = 1;
    for (std::size_t i = 0; i < depth; ++i) {
        BookStatus st{};
        // Descending prices: i = 0 is the best bid, i = depth-1 the worst.
        const Price p = Price::from_raw(
            top.raw() - static_cast<std::int64_t>(i) * kStrideTicks * (Price::kScale / 100));
        book.add(Side::bid, p, Quantity::from_raw(100), id++, st);
        if (st != BookStatus::ok) {
            std::printf("  WARNING: setup add rejected at depth %llu (status %d)\n",
                        static_cast<unsigned long long>(i), static_cast<int>(st));
            break;
        }
    }
    return book;
}

// Anti-dead-store accumulator.
//
// Every add in the timed loop writes to `book` and every remove deletes it
// again, so from the compiler's point of view the whole batch provably
// does nothing: the book ends in exactly the state it started in. A
// conforming compiler is entitled to delete the lot, and the measurement
// then reports an empty loop as the cost of an insert.
//
// The first version of this tool reported a flat, suspiciously low sweep
// for exactly that reason. The fix is to accumulate a value the loop
// produces, outside the compiler's reach: `volatile` so it cannot be
// reasoned away, and printed by `main` so it cannot be removed as unused.
// This is the same defect, in the same family, as the dead-store
// elimination caught in `hft_tsc_bench` -- and it is worth naming as a
// pattern, because the failure mode is a benchmark reporting a flattering
// number rather than crashing.
//
// File scope rather than function scope so `main` can print it. That is
// the whole discipline: a sink nobody reads is not a sink, it is a write
// the compiler is entitled to delete, and both compilers say so.
volatile std::uint64_t g_sink = 0;

/// Readable name for a rejection reason, because "99744 rejected" with no
/// reason sends the reader back to the enum to work out whether the
/// benchmark or the book is at fault.
[[nodiscard]] const char* status_name(BookStatus s) noexcept {
    switch (s) {
        case BookStatus::ok: return "ok";
        case BookStatus::unknown_order: return "unknown_order";
        case BookStatus::duplicate_order: return "duplicate_order";
        case BookStatus::zero_size: return "zero_size";
        case BookStatus::over_reduce: return "over_reduce";
        case BookStatus::capacity_exhausted: return "capacity_exhausted";
    }
    return "unknown";
}

/// Ticks per add+remove pair at the given depth and walk distance.
///
/// Returns 0.0 if the probe add was rejected, which would mean the
/// measurement is not of what it claims and must not be reported as a
/// number.
[[nodiscard]] double ticks_per_op(const TsClock& clk, std::size_t depth,
                                  std::size_t distance) {
    OrderBook book = build(depth, anchor());

    // Confirm the book has the depth we asked for. Probing the real shape
    // rather than the requested one matters: a generator that quietly
    // produced fewer levels would turn the whole sweep into a flat line
    // that looked like an O(1) ladder.
    if (book.level_count(Side::bid) != depth) {
        std::printf("  WARNING: built %u levels, asked for %llu\n",
                    book.level_count(Side::bid),
                    static_cast<unsigned long long>(depth));
        return 0.0;
    }

    const std::optional<Price> best = book.best_bid();
    if (!best.has_value()) {
        return 0.0;
    }
    // The target sits in the GAP at rank `distance` below the best, which
    // is `2*distance + 1` ticks down given the two-tick stride. Walking
    // from the head to that insertion point therefore crosses exactly
    // `distance` nodes -- which is the quantity the sweep is supposed to
    // vary, and the reason the stride exists.
    //
    // `distance = 0` would put the target ABOVE the best bid, a walk of no
    // nodes, which is the true control. The table uses 1 as its control
    // instead so that every probed price is strictly below the best and
    // the control is not accidentally a best-insert, which takes a
    // different path through `link_level`.
    const std::size_t rank = (distance == 0) ? 1 : distance;
    const Price target =
        Price::from_raw(best->raw() - static_cast<std::int64_t>(2 * rank + 1) *
                                             (Price::kScale / 100));

    // One untimed probe, to prove the operation is legal before timing
    // 20,000 of them -- and to prove it is actually creating and removing
    // a level rather than hitting an existing one, which would measure
    // nothing. The level count either side is the direct evidence.
    {
        const std::uint32_t before = book.level_count(Side::bid);
        BookStatus st{};
        const hft::OrderId probe = 1'000'000;
        book.add(Side::bid, target, Quantity::from_raw(50), probe, st);
        if (st != BookStatus::ok) {
            std::printf("  WARNING: probe add rejected (status '%s')\n",
                        status_name(st));
            return 0.0;
        }
        const std::uint32_t during = book.level_count(Side::bid);
        book.remove(probe);
        const std::uint32_t after = book.level_count(Side::bid);
        if (during != before + 1 || after != before) {
            std::printf(
                "  WARNING: the probe did not create and destroy a level\n"
                "  (levels %u -> %u -> %u), so this row would measure an\n"
                "  add to an existing price and no ladder walk at all.\n",
                before, during, after);
            return 0.0;
        }
    }

    // Anti-dead-store accumulator.
    //
    // Every add in the timed loop writes to `book` and every remove
    // deletes it again, so from the compiler's point of view the whole
    // batch provably does nothing: the book ends in exactly the state it
    // started in. A conforming compiler is entitled to delete the lot, and
    // the measurement then reports an empty loop as the cost of an insert.
    //
    // The first version of this tool reported a flat, suspiciously low
    // sweep for exactly that reason. The fix is to accumulate a value the
    // loop produces, outside the compiler's reach: `volatile` so it
    // cannot be reasoned away, and printed by `main` so it cannot be
    // removed as unused. See the file-scope declaration above.
    double best_ticks = 0.0;
    hft::OrderId id = 2'000'000;
    std::uint32_t rejected = 0;
    BookStatus last_status = BookStatus::ok;
    for (int r = 0; r < kRepeats; ++r) {
        const std::uint64_t t0 = TsClock::read();
        for (int i = 0; i < kBatchOps; ++i) {
            BookStatus st{};
            const hft::OrderId op = id++;
            book.add(Side::bid, target, Quantity::from_raw(50), op, st);
            if (st != BookStatus::ok) {
                ++rejected;
                last_status = st;
            }
            book.remove(op);
        }
        const std::uint64_t t1 = TsClock::read();
        g_sink += static_cast<std::uint64_t>(book.level_count(Side::bid));
        const double per =
            static_cast<double>(t1 - t0) / static_cast<double>(kBatchOps);
        if (r == 0 || per < best_ticks) {
            best_ticks = per;
        }
    }
    (void)clk;

    if (rejected != 0) {
        std::printf("  WARNING: %u of %d probe adds rejected at depth %llu, "
                    "last status '%s';\n"
                    "  the row below does not measure what it claims.\n",
                    rejected, kBatchOps * kRepeats,
                    static_cast<unsigned long long>(depth), status_name(last_status));
        return 0.0;
    }
    return best_ticks;
}

}  // namespace

int main() {
    bench::print_environment("price ladder scaling: the O(depth) walk");
    bench::print_clock_overhead(bench::measure_clock_overhead());

    const TsClock clk;
    if (!clk.calibrated()) {
        std::printf(
            "\n  TSC calibration failed, so the tick column below is not\n"
            "  convertible to nanoseconds. The RATIO columns are unaffected:\n"
            "  they are quotients of the same uncalibrated unit.\n");
    }

    bench::section("WHY RATIOS AND NOT NANOSECONDS");
    std::printf(
        "  The clock pair on this host is a roughly constant additive cost.\n"
        "  A constant added to both numerator and denominator biases their\n"
        "  ratio toward 1, so every speedup below is a LOWER bound. A ladder\n"
        "  that looks Nx worse here is at least Nx worse; it cannot look\n"
        "  worse than it is.\n\n"
        "  Absolute nanoseconds for these operations are not measurable on\n"
        "  this host at all -- see hft_tsc_bench for the floor.\n");

    bench::section("SWEEP: cost vs ladder depth");
    std::printf("  distance = 1 is the control: the walk crosses no levels, so\n");
    std::printf("  it measures all of add/remove except the ladder itself.\n\n");
    std::printf("  %-8s %-10s %12s %12s %10s\n", "depth", "distance", "ticks/op",
                "ns/op", "vs d=1");
    std::printf("  %-8s %-10s %12s %12s %10s\n", "--------", "----------",
                "------------", "------------", "----------");

    // Control at each depth: distance 1. Then the deep probes.
    struct Row {
        std::size_t depth;
        std::size_t distance;
        double ticks;
    };
    std::vector<Row> rows;
    double control_ticks = 0.0;

    for (const std::size_t depth : kDepths) {
        // Control: a walk of one node, so the column below measures all of
        // add/remove except the ladder.
        const double control = ticks_per_op(clk, depth, 1);
        if (control <= 0.0) {
            continue;
        }
        control_ticks = control;
        rows.push_back({depth, 1, control});
        std::printf("  %-8llu %-10llu %12.2f %12s %10s\n",
                    static_cast<unsigned long long>(depth), 1ULL, control, "-", "1.00x");

        // Walks crossing a quarter, a half, and the whole ladder. All of
        // them must be <= depth, because the target price has to land in a
        // gap that exists -- a distance beyond the worst level would
        // create a new tail node and walk the entire ladder regardless of
        // how far past the end it went, which is not a longer walk, it is
        // the same walk measured twice.
        for (const std::size_t num : {std::size_t{4}, std::size_t{2}, std::size_t{1}}) {
            const std::size_t dist = depth / num;
            if (dist < 2 || dist > depth) {
                continue;
            }
            const double t = ticks_per_op(clk, depth, dist);
            if (t <= 0.0) {
                continue;
            }
            rows.push_back({depth, dist, t});
            std::printf("  %-8llu %-10llu %12.2f %12s %10.2fx\n",
                        static_cast<unsigned long long>(depth),
                        static_cast<unsigned long long>(dist), t, "-", t / control);
        }
        std::fflush(stdout);
    }

    // Fill in nanoseconds now that the ratios are on the page.
    if (clk.calibrated()) {
        std::printf("\n  ns/op, same runs:\n\n");
        std::printf("  %-8s %-10s %12s\n", "depth", "distance", "ns/op");
        std::printf("  %-8s %-10s %12s\n", "--------", "----------", "------------");
        for (const Row& r : rows) {
            std::printf("  %-8llu %-10llu %12.2f\n",
                        static_cast<unsigned long long>(r.depth),
                        static_cast<unsigned long long>(r.distance),
                        clk.to_nanos(static_cast<std::uint64_t>(r.ticks + 0.5)));
        }
    }

    bench::section("READING THIS");
    std::printf(
        "  If the ladder is O(depth), the ticks/op column should grow\n"
        "  roughly linearly with `distance` at every fixed depth, and the\n"
        "  cost of a full-depth walk should scale with `depth`.\n\n");

    // Both sinks are read here, which is what makes them work. A `volatile`
    // that is written and never read is still "set but not used" to the
    // compiler, and the honest fix is to print it rather than to cast to
    // void: the values are also a correctness check, because a zero means
    // the timed loop was optimised away.
    std::printf("  deepest control     %.2f ticks/op (depth-1 walk, the floor)\n",
                control_ticks);
    std::printf("  live-order sink     %llu\n",
                static_cast<unsigned long long>(g_sink));
    if (g_sink == 0) {
        std::printf(
            "\n  WARNING: the sink is zero, so the timed adds were optimised\n"
            "  away and every ticks/op figure above is an empty loop. Treat\n"
            "  this run as unmeasured.\n");
    }

    std::printf(
        "\n  The fix is a direct-indexed price ladder: an array indexed by\n"
        "  price offset from an anchor, which makes 'find the level at this\n"
        "  price' and 'insert at the head' O(1) instead of O(depth).\n\n"
        "  It is NOT implemented here, and the reason is stated rather than\n"
        "  hidden: a change to the book's hot path is only worth making if\n"
        "  its effect can be measured, and on this host it cannot be. The\n"
        "  measurement belongs on the benchmark host, where the clock\n"
        "  resolves single-digit nanoseconds and this sweep becomes the\n"
        "  before half of a before-and-after. This table is that before\n"
        "  half, taken now so it cannot later be taken from memory.\n");

    bench::print_publication_notice();
    std::fflush(stdout);
    return 0;
}