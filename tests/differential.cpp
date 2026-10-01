// Differential test: fast book versus naive reference model.
//
// This is the correctness argument the project rests on. A p999 figure
// means nothing unless the thing producing it is demonstrably right,
// and the way to demonstrate that is to run two implementations that
// share no data structure over the same input and compare full state
// after every single operation.
//
// The generator below deliberately produces the situations that break
// order books:
//   * orders arriving out of price order, so level insertion has to
//     reorder the ladder rather than append
//   * repeated orders at the same price, so price-time priority is
//     actually exercised
//   * partial fills and partial cancels that leave the order working
//   * a partial cancel that fully consumes an order, which must remove
//     it and is the case most often confused with Order Delete
//   * reductions that would drive size below zero, which must be
//     rejected without corrupting aggregates
//   * deletes that empty a level while other orders share that price
//
// A seed that reproduces a failure is printed on mismatch, so any
// failure is directly replayable.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "feed/generator.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/lob/reference_book.hpp"
#include "hft/types.hpp"

namespace {

using hft::Price;
using hft::Quantity;
using hft::Side;

using Op = hft::lob::BookStatus;

struct Mismatch {
    const char* what = "";
    std::size_t step = 0;
    std::string detail;
};

bool compare_state(const hft::lob::OrderBook& fast, const hft::lob::ReferenceBook& ref,
                   std::size_t step, Mismatch& out) {
    for (Side side : {Side::bid, Side::ask}) {
        const auto fa = fast.levels(side);
        const auto ra = ref.levels(side);
        if (fa.size() != ra.size()) {
            out = Mismatch{"level count", step,
                           std::string(side == Side::bid ? "bid" : "ask") + ": fast=" +
                               std::to_string(fa.size()) + " ref=" + std::to_string(ra.size())};
            return false;
        }
        for (std::size_t i = 0; i < fa.size(); ++i) {
            if (fa[i].price != ra[i].price) {
                out = Mismatch{"level price order", step,
                               "index " + std::to_string(i) + ": fast=" +
                                   fa[i].price.to_string() + " ref=" + ra[i].price.to_string()};
                return false;
            }
            if (fa[i].aggregate_size != ra[i].aggregate_size) {
                out = Mismatch{"level aggregate", step,
                               fa[i].price.to_string() + ": fast=" +
                                   std::to_string(fa[i].aggregate_size.raw()) + " ref=" +
                                   std::to_string(ra[i].aggregate_size.raw())};
                return false;
            }
            if (fa[i].order_count != ra[i].order_count) {
                out = Mismatch{"level order count", step,
                               fa[i].price.to_string() + ": fast=" +
                                   std::to_string(fa[i].order_count) + " ref=" +
                                   std::to_string(ra[i].order_count)};
                return false;
            }
        }

        // Full order-level comparison in priority order.
        const auto fo = fast.orders(side);
        const auto ro = ref.orders(side);
        if (fo.size() != ro.size()) {
            out = Mismatch{"order count", step,
                           std::string(side == Side::bid ? "bid" : "ask") + ": fast=" +
                               std::to_string(fo.size()) + " ref=" + std::to_string(ro.size())};
            return false;
        }
        for (std::size_t i = 0; i < fo.size(); ++i) {
            if (fo[i].id != ro[i].id) {
                out = Mismatch{"priority order", step,
                               "index " + std::to_string(i) + ": fast id=" +
                                   std::to_string(fo[i].id) + " ref id=" +
                                   std::to_string(ro[i].id)};
                return false;
            }
            if (fo[i].size != ro[i].size) {
                out = Mismatch{"order size", step,
                               "id " + std::to_string(fo[i].id) + ": fast=" +
                                   std::to_string(fo[i].size.raw()) + " ref=" +
                                   std::to_string(ro[i].size.raw())};
                return false;
            }
            if (fo[i].state != ro[i].state) {
                out = Mismatch{"order state", step,
                               "id " + std::to_string(fo[i].id) + ": fast=" +
                                   std::to_string(static_cast<int>(fo[i].state)) + " ref=" +
                                   std::to_string(static_cast<int>(ro[i].state))};
                return false;
            }
        }
    }

    if (fast.best_bid() != ref.best_bid() || fast.best_ask() != ref.best_ask()) {
        out = Mismatch{"top of book", step, ""};
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 0xA5A5'1234'DEAD'0001ULL;
    std::size_t ops = 200'000;
    if (argc > 1) {
        seed = std::strtoull(argv[1], nullptr, 0);
    }
    if (argc > 2) {
        ops = std::strtoull(argv[2], nullptr, 10);
    }

    hft::lob::OrderBook fast(1u << 20, 1u << 16);
    hft::lob::ReferenceBook ref;
    hft::feed::SplitMix64 rng(seed);

    std::vector<hft::OrderId> live;
    live.reserve(4096);

    const std::int64_t mid = Price::kScale * 100;
    hft::OrderId next_id = 1;
    std::size_t adds = 0;
    std::size_t executes = 0;
    std::size_t cancels = 0;
    std::size_t removes = 0;
    std::size_t rejected_over_reduce = 0;

    for (std::size_t step = 0; step < ops; ++step) {
        const std::uint64_t roll = rng.below(100);

        if (roll < 55 || live.empty()) {
            // Add, deliberately at a price that jumps around so the
            // ladder has to reorder rather than only append.
            const int level = static_cast<int>(rng.below(24)) - 12;
            const std::int64_t raw = mid + level * Price::kScale / 2;
            const Side side = (rng.next() & 1u) == 0u ? Side::bid : Side::ask;
            const std::uint32_t shares = 1 + static_cast<std::uint32_t>(rng.below(500));

            hft::lob::BookStatus fs{};
            fast.add(side, Price::from_raw(raw), Quantity::from_raw(shares), next_id, fs);
            const Op rs = ref.add(side, Price::from_raw(raw), Quantity::from_raw(shares), next_id);
            if (fs != rs) {
                std::printf("MISMATCH step %zu: add status fast=%u ref=%u\n", step,
                            static_cast<unsigned>(fs), static_cast<unsigned>(rs));
                std::printf("replay with seed 0x%llx\n", static_cast<unsigned long long>(seed));
                return 1;
            }
            if (fs == hft::lob::BookStatus::ok) {
                live.push_back(next_id);
                ++adds;
            }
            ++next_id;
        } else {
            const std::size_t pick = static_cast<std::size_t>(rng.below(live.size()));
            const hft::OrderId id = live[pick];
            // Look up remaining size so reductions are sometimes exact,
            // sometimes partial, and sometimes deliberately too large.
            std::uint64_t remaining = 0;
            if (const auto snap = fast.find(id); snap.has_value()) {
                remaining = snap->size.raw();
            }
            const std::uint64_t amount = (rng.below(100) < 25)
                                            ? remaining + 1  // guaranteed over-reduce
                                            : 1 + rng.below(remaining == 0 ? 1 : remaining);

            Op fs{};
            Op rs{};
            if (roll < 80) {
                fs = fast.execute(id, Quantity::from_raw(amount));
                rs = ref.execute(id, Quantity::from_raw(amount));
                ++executes;
                if (fs == hft::lob::BookStatus::over_reduce) {
                    ++rejected_over_reduce;
                }
            } else if (roll < 90) {
                fs = fast.cancel_partial(id, Quantity::from_raw(amount));
                rs = ref.cancel_partial(id, Quantity::from_raw(amount));
                ++cancels;
                if (fs == hft::lob::BookStatus::over_reduce) {
                    ++rejected_over_reduce;
                }
            } else {
                Quantity discarded_fast{};
                Quantity discarded_ref{};
                fs = fast.remove(id, &discarded_fast);
                rs = ref.remove(id, &discarded_ref);
                ++removes;
                if (discarded_fast != discarded_ref) {
                    std::printf("MISMATCH step %zu: remove discarded fast=%llu ref=%llu\n", step,
                                static_cast<unsigned long long>(discarded_fast.raw()),
                                static_cast<unsigned long long>(discarded_ref.raw()));
                    return 1;
                }
            }

            if (fs != rs) {
                std::printf("MISMATCH step %zu: status fast=%u ref=%u (id %llu, amount %llu)\n", step,
                            static_cast<unsigned>(fs), static_cast<unsigned>(rs),
                            static_cast<unsigned long long>(id),
                            static_cast<unsigned long long>(amount));
                std::printf("replay with seed 0x%llx\n", static_cast<unsigned long long>(seed));
                return 1;
            }

            if (fs == hft::lob::BookStatus::ok) {
                const auto snap = fast.find(id);
                if (!snap.has_value()) {
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(pick));
                }
            }
        }

        Mismatch m;
        if (!compare_state(fast, ref, step, m)) {
            std::printf("MISMATCH at step %zu: %s -- %s\n", step, m.what, m.detail.c_str());
            std::printf("replay with seed 0x%llx\n", static_cast<unsigned long long>(seed));
            return 1;
        }
    }

    std::printf("differential test passed\n");
    std::printf("  seed          0x%llx\n", static_cast<unsigned long long>(seed));
    std::printf("  operations    %zu\n", ops);
    std::printf("  adds          %zu\n", adds);
    std::printf("  executes      %zu\n", executes);
    std::printf("  partial cxl   %zu\n", cancels);
    std::printf("  deletes       %zu\n", removes);
    std::printf("  over-reduces  %zu (rejected, as intended)\n", rejected_over_reduce);
    std::printf("  bid levels    %u\n", fast.level_count(Side::bid));
    std::printf("  ask levels    %u\n", fast.level_count(Side::ask));
    std::printf("  live orders   %zu\n", live.size());
    return 0;
}
