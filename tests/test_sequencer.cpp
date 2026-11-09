// Multi-shard sequencer tests: contiguous global sequencing,
// per-symbol routing stability, rebalancing, failover reroute
// without loss, lag/stalled detection.

#include <cstdio>
#include <cstdint>
#include <string>

#include "hft/lob/sequencer.hpp"
#include "hft/lob/shards.hpp"

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

namespace lob = hft::lob;

lob::Symbol sym(const char* s) {
    char buf[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    std::size_t n = 0;
    while (s[n] != '\0' && n < 8) {
        buf[n] = s[n];
        ++n;
    }
    return lob::Symbol::from_wire(buf);
}

}  // namespace

int main() {
    // Global sequencing is contiguous and shared across shards.
    {
        lob::ShardSequencer seq(4);
        const auto aapl = sym("AAPL");
        const auto msft = sym("MSFT");
        check(seq.sequence(aapl) == 0, "first sequence is 0");
        check(seq.sequence(msft) == 1, "second sequence is 1 (other shard)");
        check(seq.sequence(aapl) == 2, "third sequence is 2");
        check(seq.next_seq() == 3, "next seq after three");
        check(seq.shard_count() == 4, "shard count");
    }

    // Routing is stable for a symbol before any rebalance.
    {
        lob::ShardSequencer seq(8);
        const auto aapl = sym("AAPL");
        const std::size_t home = seq.route(aapl);
        check(seq.route(aapl) == home, "routing stable across calls");
        check(home < 8, "home shard in range");
        seq.sequence(aapl);
        seq.sequence(aapl);
        check(seq.route(aapl) == home, "routing stable across sequencing");
    }

    // Rebalancing moves subsequent messages, preserves order.
    {
        lob::ShardSequencer seq(4);
        const auto aapl = sym("AAPL");
        const std::size_t home = seq.route(aapl);
        const std::size_t target = (home + 1) % 4;
        check(seq.rebalance(aapl, target), "rebalance to valid shard succeeds");
        check(!seq.rebalance(aapl, 99), "rebalance to invalid shard refused");
        check(seq.route(aapl) == target, "rebalanced symbol routes to target");
        const auto s0 = seq.sequence(aapl);
        const auto s1 = seq.sequence(aapl);
        check(s1 == s0 + 1, "per-symbol order preserved across rebalance");
        check(seq.rebalances() == 1, "rebalance counted");
        check(seq.override_count() == 1, "override recorded");
    }

    // Failover: down shard's symbols reroute to standby, no loss.
    {
        lob::ShardSequencer seq(4);
        const auto aapl = sym("AAPL");
        const std::size_t home = seq.route(aapl);
        const std::size_t standby = (home + 1) % 4;
        seq.mark_down(home);
        check(seq.is_down(home), "shard marked down");
        check(seq.route(aapl) == standby, "down shard's symbols reroute to standby");
        check(seq.failovers() == 1, "failover counted");
        // Sequencing continues for the rerouted symbol: no gap.
        const auto s0 = seq.sequence(aapl);
        const auto s1 = seq.sequence(aapl);
        check(s1 == s0 + 1, "no sequence loss during failover");
        // Recovery: mark up restores original mapping.
        seq.mark_up(home);
        check(!seq.is_down(home), "shard marked up");
        check(seq.route(aapl) == home, "routing restored after mark_up");
    }

    // Lag detection: a shard that stops being seen stalls.
    {
        lob::ShardSequencer seq(2);
        const auto aapl = sym("AAPL");
        const auto msft = sym("MSFT");
        // Drive only one symbol's shard; the other's lag grows.
        const std::size_t aapl_shard = seq.route(aapl);
        const std::size_t msft_shard = seq.route(msft);
        for (int i = 0; i < 100; ++i) {
            seq.sequence(aapl);
        }
        check(seq.lag(aapl_shard) == 0, "active shard has zero lag");
        if (msft_shard != aapl_shard) {
            check(seq.lag(msft_shard) == 100, "idle shard lag equals global seq");
            check(seq.stalled(msft_shard, 50), "idle shard reported stalled");
            check(!seq.stalled(aapl_shard, 50), "active shard not stalled");
        } else {
            check(true, "symbols share a shard; lag check skipped (same shard)");
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
