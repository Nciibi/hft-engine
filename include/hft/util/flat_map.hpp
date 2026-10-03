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
//     genuinely full rather than growing: a book that silently
//     reallocates under load has a latency cliff nobody budgeted for.
//   * Linear probing, power-of-two capacity, masked index.
//   * Tombstones on erase, with the reclamation bug that wedged the order
//     book documented at length below `insert`.
//   * Load factor is held at or below 0.5 by construction.
//
// ---- Why the entry is interleaved, not split across three arrays -------
//
// The first version of this stored the state, the key and the value in
// three parallel vectors:
//
//     std::vector<Slot> slots_;
//     std::vector<K>    keys_;
//     std::vector<V>    values_;
//
// Thirteen bytes per entry, which looks efficient. It is not. A single
// probe reads `slots_[i]`, then `keys_[i]`, then `values_[i]` -- three
// arrays at three addresses, so **three cache lines**, of which two are
// almost certainly misses on a table larger than L2.
//
// The order book probes `order_index_` twice per add (`contains`, then
// `insert`) and `level_index_` once, so a single order insertion was
// touching roughly nine cache lines to move thirteen bytes of state. On
// the development host that put the working set at ~157 MiB against 8 MiB
// of L3, and ingest ran at ~344 ns per message -- about 1,100 cycles,
// which is DRAM latency accounting for essentially all of it.
//
// Interleaving the three fields into one entry costs 3 bytes of padding
// and buys back the entire difference:
//
//     struct Entry { K key; V value; Slot state; };   // 16 bytes for
//                                                      // uint64 + uint32 + uint8
//
// Sixteen bytes is four entries per 64-byte line, so a probe touches one
// line instead of three. The static_assert at the bottom exists so a
// future field cannot quietly push this back over a line boundary and
// undo the reason the layout is the way it is.
//
// This map is not iterating and does not support deleting the key being
// visited, because it does not iterate.

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
    /// One probe slot: state, key and value together.
    ///
    /// Public because the cache-line guarantee below is part of this type's
    /// contract and has to be assertable from outside it. Field order is
    /// deliberate -- widest first -- so the padding lands at the end and
    /// `sizeof(Entry)` is 16 for the (uint64, uint32) instantiation the
    /// order book uses. Reordering these fields to put `state` first would
    /// make it 24 and undo the layout.
    struct Entry {
        K key{};
        V value{};
        Slot state = Slot::empty;
    };

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
        // `assign` value-initialises, so every entry arrives with
        // `state == Slot::empty` and a zeroed key and value.
        entries_.assign(capacity_, Entry{});
        size_ = 0;
        tombstones_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t tombstone_count() const noexcept { return tombstones_; }

    /// Slot index of `key`, or kNoHandle.
    ///
    /// A slot index rather than a pointer: the caller reaches the value
    /// through `value_at`, so one probe is enough and the second lookup a
    /// `find`-then-index design would need never happens.
    [[nodiscard]] std::uint32_t find(const K& key) const noexcept {
        if (capacity_ == 0) {
            return kNoHandle;
        }
        std::uint32_t i = index_for(key);
        for (std::uint32_t probes = 0; probes < capacity_; ++probes) {
            const Entry& e = entries_[(i + probes) & mask_];
            if (e.state == Slot::empty) {
                // An empty slot terminates the probe: the key is not
                // present, because insert always places a key at or
                // before the first empty slot in its run.
                return kNoHandle;
            }
            if (e.state == Slot::occupied && e.key == key) {
                return (i + probes) & mask_;
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
    /// stop. `order_index_` and `level_index_` are erased on every
    /// removal, so a table fills with tombstones as the book churns. Once
    /// every slot is occupied-or-tombstone, the next add of a new order
    /// reference returns `capacity_exhausted` and **every subsequent add
    /// does too, forever**, because the tombstones are never reclaimed and
    /// live occupancy is a small fraction of capacity. A book that traded
    /// a few hundred million shares would stop accepting orders while
    /// looking almost empty.
    ///
    /// Nothing in this repository's tests caught it, and the reason is
    /// worth recording: every one of them sizes capacity far above its
    /// operation count -- the differential test drives 200,000 operations
    /// through a book with 1,048,576 order slots -- so cumulative churn
    /// never once reaches capacity. `hft_ladder_bench` found it by sizing
    /// capacity tightly to the working set, which is the only way to see
    /// it.
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
            Entry& e = entries_[slot];
            if (e.state == Slot::occupied) {
                if (e.key == key) {
                    e.value = value;
                    return true;
                }
                continue;
            }
            if (e.state == Slot::tombstone) {
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

    /// Remove a key. Returns false if absent.
    bool erase(const K& key) noexcept {
        const std::uint32_t slot = find(key);
        if (slot == kNoHandle) {
            return false;
        }
        entries_[slot].state = Slot::tombstone;
        --size_;
        ++tombstones_;
        return true;
    }

    /// Erase a key whose slot is already known.
    ///
    /// Exists because `erase` otherwise costs a second probe, and the
    /// callers that erase usually looked the key up a moment earlier.
    /// Saving a probe in a cache-miss-bound structure is worth a line of
    /// duplication at the call site.
    bool erase_at(std::uint32_t slot) noexcept {
        if (slot >= capacity_ || entries_[slot].state != Slot::occupied) {
            return false;
        }
        entries_[slot].state = Slot::tombstone;
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
    [[nodiscard]] V& value_at(std::uint32_t slot) noexcept { return entries_[slot].value; }
    [[nodiscard]] const V& value_at(std::uint32_t slot) const noexcept {
        return entries_[slot].value;
    }

private:
    /// Write a key into a slot known to be empty or a tombstone, keeping
    /// the tombstone count honest.
    ///
    /// Split out because both call sites above must do it identically, and
    /// a `--tombstones_` that only one of them performs is a leak that
    /// eventually reintroduces the original symptom by a different route.
    bool place(const K& key, const V& value, std::uint32_t slot) noexcept {
        Entry& e = entries_[slot];
        e.key = key;
        e.value = value;
        if (e.state == Slot::tombstone) {
            --tombstones_;
        }
        e.state = Slot::occupied;
        ++size_;
        return true;
    }

    [[nodiscard]] std::uint32_t index_for(const K& key) const noexcept {
        // For the integral keys used here, std::hash is the identity
        // and masking with (capacity - 1) scatters sequential OrderIds
        // across distinct slots. That is the desired outcome, not a
        // clustering hazard: a prime-modulus table would be the one
        // that needed a mixing function.
        return static_cast<std::uint32_t>(Hash{}(key)) & mask_;
    }

    std::vector<Entry> entries_{};
    std::uint32_t capacity_ = 0;
    std::uint32_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t tombstones_ = 0;
};

/// The interleaved entry must stay within a quarter of a cache line.
///
/// This is the whole reason the layout is what it is: 16 bytes puts four
/// entries in each 64-byte line, so a probe reads one line. Grow the entry
/// past 16 and it becomes two, and the change that did it will look like
/// an innocuous field addition in review.
static_assert(sizeof(FlatMap<std::uint64_t, std::uint32_t>::Entry) <= 16,
              "FlatMap::Entry must stay <= 16 bytes or a probe touches two cache lines instead of "
              "one, which is the entire reason the fields are interleaved");

}  // namespace hft::util