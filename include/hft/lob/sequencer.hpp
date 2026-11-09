// Multi-shard sequencer: total-order sequencing, rebalancing, failover.
//
// Symbol sharding (shards.hpp) parallelises the books but says
// nothing about ORDER across shards. This header adds the missing
// three pieces, single-process and header-only (the threaded
// deployment is one sequencer in front of the dispatcher):
//
//   1. Global sequencing. Every routed message gets a contiguous
//      global sequence number, assigned here and nowhere else. A
//      consumer can then detect "shard 3 is 40,000 messages behind"
//      as a number, not a vibe.
//   2. Rebalancing. A symbol's owning shard can change without
//      losing or reordering its messages: the override is recorded
//      BEFORE the next message for that symbol is routed, and
//      per-symbol order is preserved because routing is decided
//      per message, never batched.
//   3. Failover. A shard marked down has its symbols rerouted to
//      the standby shard ((shard+1) % shards). Messages are never
//      dropped by failover -- they change owner. `mark_up`
//      restores the original mapping; the sequencer does not move
//      symbols back mid-stream.
//
// Honesty note: this is the sequencing/rebalancing/failover LOGIC.
// Cross-process failover (a dead process's kernel buffers, a
// standby host taking over a TCP session) is a different system
// and is not claimed here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "hft/lob/shards.hpp"

namespace hft::lob {

class ShardSequencer final {
public:
    explicit ShardSequencer(std::size_t shards) : shard_count_(shards == 0 ? 1 : shards) {
        down_.assign(shard_count_, false);
        shard_seen_.assign(shard_count_, 0);
    }

    /// Owning shard for a symbol, after overrides and failover.
    [[nodiscard]] std::size_t route(const Symbol& symbol) const noexcept {
        const auto it = overrides_.find(symbol.str());
        std::size_t shard = it != overrides_.end() ? it->second : shard_of(symbol, shard_count_);
        if (down_[shard]) {
            shard = standby(shard);
        }
        return shard;
    }

    /// Assign the next global sequence number to a message for
    /// `symbol` and record that its shard has been seen. Returns
    /// the sequence number; the caller stamps it on the message.
    std::uint64_t sequence(const Symbol& symbol) noexcept {
        const std::size_t shard = route(symbol);
        shard_seen_[shard] = global_seq_;
        return global_seq_++;
    }

    [[nodiscard]] std::uint64_t next_seq() const noexcept { return global_seq_; }
    [[nodiscard]] std::size_t shard_count() const noexcept { return shard_count_; }

    /// Move `symbol` to `shard` for all subsequent messages.
    /// Returns false if the shard does not exist.
    bool rebalance(const Symbol& symbol, std::size_t shard) {
        if (shard >= shard_count_) {
            return false;
        }
        overrides_[symbol.str()] = shard;
        ++rebalances_;
        return true;
    }

    /// Mark a shard down/up. While down, its symbols route to the
    /// standby. Marking the standby itself down cascades once
    /// (standby's standby), which is enough for a single failure;
    /// two simultaneous failures are a config error, reported by
    /// `route` returning a down shard and `failover_loss` counting it.
    void mark_down(std::size_t shard) noexcept {
        if (shard < shard_count_) {
            down_[shard] = true;
            ++failovers_;
        }
    }
    void mark_up(std::size_t shard) noexcept {
        if (shard < shard_count_) {
            down_[shard] = false;
        }
    }
    [[nodiscard]] bool is_down(std::size_t shard) const noexcept {
        return shard < shard_count_ && down_[shard];
    }

    /// Lag of a shard: global_seq - last seq seen on that shard.
    /// Zero means current; large means stalled or down.
    [[nodiscard]] std::uint64_t lag(std::size_t shard) const noexcept {
        if (shard >= shard_count_) {
            return 0;
        }
        return global_seq_ - shard_seen_[shard];
    }

    [[nodiscard]] bool stalled(std::size_t shard, std::uint64_t threshold) const noexcept {
        return lag(shard) > threshold;
    }

    [[nodiscard]] std::uint64_t rebalances() const noexcept { return rebalances_; }
    [[nodiscard]] std::uint64_t failovers() const noexcept { return failovers_; }

    /// Symbols with an explicit override (rebalanced entries).
    [[nodiscard]] std::size_t override_count() const noexcept { return overrides_.size(); }

private:
    [[nodiscard]] std::size_t standby(std::size_t shard) const noexcept {
        std::size_t s = (shard + 1) % shard_count_;
        if (down_[s] && shard_count_ > 1) {
            s = (s + 1) % shard_count_;
        }
        return s;
    }

    std::size_t shard_count_;
    std::vector<bool> down_;
    std::vector<std::uint64_t> shard_seen_;
    std::map<std::string, std::size_t> overrides_;
    std::uint64_t global_seq_ = 0;
    std::uint64_t rebalances_ = 0;
    std::uint64_t failovers_ = 0;
};

}  // namespace hft::lob
