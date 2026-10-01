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

    /// True when the map is at its configured ceiling. Callers that
    /// must not lose entries check this before inserting.
    [[nodiscard]] bool full() const noexcept {
        return size_ + tombstones_ >= capacity_;
    }

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

    /// Insert or overwrite. Returns false only when the map is full.
    /// Overwriting an existing key is not a failure and reuses the
    /// existing slot, refreshing the tombstone if there was one.
    bool insert(const K& key, const V& value) noexcept {
        if (capacity_ == 0) {
            return false;
        }
        // Reuse a tombstone in this key's probe run if one exists, so
        // repeated erase/insert of the same key does not consume the
        // table.
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
            const std::uint32_t target =
                (first_free != kNoHandle) ? first_free : slot;
            keys_[target] = key;
            values_[target] = value;
            if (slots_[target] == Slot::tombstone) {
                --tombstones_;
            }
            slots_[target] = Slot::occupied;
            ++size_;
            return true;
        }
        return false;
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

    /// Read a value. The caller is responsible for having checked
    /// `contains`; there is no optional-returning accessor because
    /// that would cost a redundant probe on the hot path.
    [[nodiscard]] const V& operator[](const K& key) const noexcept {
        return values_[find(key)];
    }

    [[nodiscard]] V& value_at(std::uint32_t slot) noexcept { return values_[slot]; }
    [[nodiscard]] const V& value_at(std::uint32_t slot) const noexcept { return values_[slot]; }

    /// Total slots consumed, including tombstones. Useful for deciding
    /// when to rebuild a heavily churned table.
    [[nodiscard]] std::size_t occupied_slots() const noexcept {
        return size_ + tombstones_;
    }

private:
    [[nodiscard]] std::uint32_t index_for(const K& key) const noexcept {
        // A high-quality mix so that sequential OrderIds, which would
        // otherwise land in one probe run and degrade the table to a
        // linked list, spread across slots.
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
