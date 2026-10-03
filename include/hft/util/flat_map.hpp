// Fixed-capacity open-addressing hash map with no allocation after
// construction.
//
// Why this exists: the fast order book must not allocate on the hot
// path, and `std::unordered_map` allocates a node per element on
// insert. A book built on it would be a book that calls the allocator
// for every order, which is both a latency spike and a direct
// contradiction of the design this project claims to demonstrate.
//
// Properties:
//   * Capacity is fixed at construction. `insert` returns false when
//     full rather than growing: a book that silently reallocates
//     under load has a latency cliff nobody budgeted for.
//   * Linear probing, power-of-two capacity, masked index.
//   * Tombstones on erase, so a delete/insert churn cycle cannot
//     degrade probe lengths without bound.
//   * Load factor is held at or below 0.5 by construction.
//
// This is a hash map, not a general-purpose container. It does not
// iterate, and it does not support deletion of the key you are
// currently visiting during iteration, because you cannot iterate it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hft::util {

inline constexpr std::uint32_t kNoHandle = 0xFFFF'FFFFu;

enum class Slot : std::uint8_t {
    empty = 0,
    occupied = 1,
    tombstone = 2,
};

template <class K, class V, class Hash = std::hash<K>>
class FlatMap final {
public:
    FlatMap() = default;

    /// `max_entries` is a hard ceiling. Capacity is rounded up to the
    /// next power of two so the probe index can be masked, and is set
    /// to at least 2x `max_entries` to keep the load factor at or
    /// below 0.5.
    explicit FlatMap(std::size_t max_entries) { reset(max_entries); }

    void reset(std::size_t max_entries) {
        std::size_t cap = 1;
        while (cap < max_entries * 2) {
            cap <<= 1;
        }
        // A zero-entry map still needs one slot so the mask is valid.
        if (cap < 2) {
            cap = 2;
        }
        capacity_ = static_cast<std::uint32_t>(cap);
        mask_ = capacity_ - 1u;
        slots_.assign(capacity_, Slot::empty);
        keys_.assign(capacity_, K{});
        values_.assign(capacity_, V{});
        size_ = 0;
        tombstones_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] std::uint32_t find(const K& key) const noexcept {
        if (capacity_ == 0) {
            return kNoHandle;
        }
        std::uint32_t i = index_for(key);
        for (std::uint32_t probes = 0; probes < capacity_; ++probes) {
            const std::uint32_t slot = (i + probes) & mask_;
            if (slots_[slot] == Slot::empty) {
                // An empty slot terminates the probe: the key is not
                // present, because insert always places a key at or
                // before the first empty slot in its run.
                return kNoHandle;
            }
            if (slots_[slot] == Slot::occupied && keys_[slot] == key) {
                return slot;
            }
        }
        return kNoHandle;
    }

    [[nodiscard]] bool contains(const K& key) const noexcept {
        return find(key) != kNoHandle;
    }

    /// Insert or overwrite. Returns false only when the map is genuinely
    /// full of live entries.
    ///
    /// Tombstone handling is the whole subtlety here, and the obvious
    /// implementation of it is wrong in a way that wedges the order book.
    ///
    /// The wrong version records the first tombstone in the key's probe
    /// run and then *waits for an empty slot* before using it, on the
    /// theory that an empty slot is a better home because it ends the run
    /// cleanly. That works right up until the moment it matters: a run
    /// containing tombstones and occupied slots but **no empty slot**. Then
    /// the probe wraps the entire table, finds nothing, and returns false
    /// -- while `first_free` is sitting there holding a perfectly good
    /// free slot.
    ///
    /// In the order book that is not a slow degradation, it is a hard
    /// stop. `order_index_` and `level_index_` are erased on every removal,
    /// so a table fills with tombstones as the book churns. Once every
    /// slot is occupied-or-tombstone, the next add of a new order reference
    /// returns `capacity_exhausted` and **every subsequent add does too,
    /// forever**, because the tombstones are never reclaimed and the live
    /// occupancy is a small fraction of capacity. A book that traded a few
    /// hundred million shares would stop accepting orders while looking
    /// almost empty.
    ///
    /// Nothing in this repository's tests caught it, and the reason is
    /// worth recording: every one of them sizes capacity far above its
    /// operation count -- the differential test drives 200,000 operations
    /// through a book with 1,048,576 order slots -- so cumulative churn
    /// never once reaches capacity. `hft_ladder_bench` found it by sizing
    /// capacity tightly to the working set, which is the only way to see it.
    ///
    /// So: keep scanning for an empty slot to preserve the probe-chain
    /// distribution, but treat a remembered tombstone as a valid
    /// destination in its own right, and use it when the run ends without
    /// one. `insert` returns false only when there is no empty slot *and*
    /// no tombstone anywhere in the run, which is the only case where the
    /// table really cannot hold another entry.
    bool insert(const K& key, const V& value) noexcept {
        if (capacity_ == 0) {
            return false;
        }
        std::uint32_t first_free = kNoHandle;
        std::uint32_t i = index_for(key);
        for (std::uint32_t probes = 0; probes < capacity_; ++probes) {
            const std::uint32_t slot = (i + probes) & mask_;
            if (slots_[slot] == Slot::occupied) {
                if (keys_[slot] == key) {
                    values_[slot] = value;
                    return true;
                }
                continue;
            }
            if (slots_[slot] == Slot::tombstone) {
                if (first_free == kNoHandle) {
                    first_free = slot;
                }
                continue;
            }
            // Slot::empty: the key is absent. Claim the first tombstone
            // if one was seen, otherwise this slot.
            return place(key, value, (first_free != kNoHandle) ? first_free : slot);
        }
        // The run wrapped without finding an empty slot. A tombstone is
        // still a usable slot -- see the note above -- so this succeeds
        // whenever one was seen, and fails only when the table genuinely
        // has no room left.
        if (first_free != kNoHandle) {
            return place(key, value, first_free);
        }
        return false;
    }

    /// Write a key into a slot known to be empty or a tombstone, keeping
    /// the tombstone count honest.
    ///
    /// Split out because both call sites above must do it identically, and
    /// a `--tombstones_` that only one of them performs is a leak that
    /// eventually reintroduces the original symptom through a different
    /// route.
    bool place(const K& key, const V& value, std::uint32_t slot) noexcept {
        keys_[slot] = key;
        values_[slot] = value;
        if (slots_[slot] == Slot::tombstone) {
            --tombstones_;
        }
        slots_[slot] = Slot::occupied;
        ++size_;
        return true;
    }

    /// Find `key`, or report the slot where it should be inserted.
    ///
    /// One walk that answers both questions, which is the entire point:
    /// the order book needs "is this reference already live?" before it
    /// mutates anything, and then "claim a slot for it" a few dozen
    /// instructions later. Asking twice cost two probes of a table that
    /// does not fit in cache, with the whole of the level lookup and pool
    /// acquisition in between -- long enough for the line to be evicted,
    /// so the second probe was a fresh miss rather than an L1 hit.
    ///
    /// On the development host that measured as roughly a tenth of total
    /// add cost. It is the difference the optimisation log keeps coming
    /// back to: *removing* a dependent memory access pays, making the ones
    /// that remain cheaper does not.
    ///
    /// `found` distinguishes the two outcomes. When true, `slot` is where
    /// the key already lives. When false, `slot` is where an insert should
    /// go, or kNoHandle if the table has no room -- note that a table of
    /// nothing but tombstones is *not* full, and this is the same walk
    /// that made `insert` correct in the first place.
    ///
    /// The reservation is only valid until the next mutating call on this
    /// map. Callers that mutate something else in between are safe here
    /// because the order book is single-threaded and nothing else touches
    /// `order_index_` during an `add`, but that is a property of the
    /// caller, not of this function, and a concurrent caller would need a
    /// real reservation or a lock.
    [[nodiscard]] std::uint32_t find_or_reserve(const K& key, bool& found) noexcept {
        found = false;
        if (capacity_ == 0) {
            return kNoHandle;
        }
        std::uint32_t first_free = kNoHandle;
        std::uint32_t i = index_for(key);
        for (std::uint32_t probes = 0; probes < capacity_; ++probes) {
            const std::uint32_t slot = (i + probes) & mask_;
            const Slot state = slots_[slot];
            if (state == Slot::occupied) {
                if (keys_[slot] == key) {
                    found = true;
                    return slot;
                }
                continue;
            }
            if (state == Slot::tombstone) {
                if (first_free == kNoHandle) {
                    first_free = slot;
                }
                continue;
            }
            // Slot::empty terminates the run. The insertion point is the
            // first tombstone seen, if any, otherwise this slot.
            return (first_free != kNoHandle) ? first_free : slot;
        }
        // Ran off the end of the table. A remembered tombstone is still
        // usable -- see the note on `insert` -- so it is offered here too.
        return first_free;
    }

    /// Claim a slot previously returned by `find_or_reserve` with
    /// `found == false`.
    ///
    /// The write half of the pair, kept separate so the read walk stays
    /// free of side effects and the two cannot be confused.
    bool place_reserved(std::uint32_t slot, const K& key, const V& value) noexcept {
        if (slot == kNoHandle || slot >= capacity_) {
            return false;
        }
        return place(key, value, slot);
    }

    /// Start loading the slot `key` would probe, without waiting for it.
    ///
    /// A no-op on toolchains without the builtin, because a prefetch that
    /// has to be conditionally compiled is still better than one that does
    /// not exist, and this is called once per message from a latency path
    /// where the alternative is a stall.
    ///
    /// Only the FIRST slot of the probe run is prefetched, not the whole
    /// run. That is deliberate: prefetching a probe you have not measured
    /// is speculative, and for the load factors this map runs at the first
    /// slot holds the key with high probability. If a run turns out to be
    /// long the subsequent slots are still real misses -- which is the
    /// cost of being nearly-right rather than the cost of doubling the
    /// memory traffic on every message.
    void prefetch(const K& key) const noexcept {
        if (capacity_ == 0) {
            return;
        }
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(&keys_[index_for(key)]);
#endif
    }

    /// Remove a key. Returns false if absent.
    bool erase(const K& key) noexcept {
        const std::uint32_t slot = find(key);
        if (slot == kNoHandle) {
            return false;
        }
        slots_[slot] = Slot::tombstone;
        --size_;
        ++tombstones_;
        return true;
    }

    /// Read a value by slot index, from a `find` that returned
    /// something other than kNoHandle.
    ///
    /// There is deliberately no `operator[]`. A key-indexed accessor
    /// would either re-probe or index with kNoHandle on a miss, and
    /// kNoHandle is 0xFFFFFFFF, which is out of range for every real
    /// capacity. That is a silent out-of-bounds read waiting for the
    /// one caller who forgets to check.
    [[nodiscard]] V& value_at(std::uint32_t slot) noexcept { return values_[slot]; }
    [[nodiscard]] const V& value_at(std::uint32_t slot) const noexcept { return values_[slot]; }

private:
    [[nodiscard]] std::uint32_t index_for(const K& key) const noexcept {
        // For the integral keys used here, std::hash is the identity
        // and masking with (capacity - 1) scatters sequential OrderIds
        // across distinct slots. That is the desired outcome, not a
        // clustering hazard: a prime-modulus table would be the one
        // that needed a mixing function.
        return static_cast<std::uint32_t>(Hash{}(key)) & mask_;
    }

    std::vector<Slot> slots_{};
    std::vector<K> keys_{};
    std::vector<V> values_{};
    std::uint32_t capacity_ = 0;
    std::uint32_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t tombstones_ = 0;
};

}  // namespace hft::util
