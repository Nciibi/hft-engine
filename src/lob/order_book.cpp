#include "hft/lob/order_book.hpp"

#include <algorithm>
#include <cassert>

namespace hft::lob {
namespace {

[[nodiscard]] inline util::FlatMap<Price, Handle, PriceHash>& level_index_for(
    util::FlatMap<Price, Handle, PriceHash>& bid,
    util::FlatMap<Price, Handle, PriceHash>& ask, Side side) noexcept {
    return side == Side::bid ? bid : ask;
}

[[nodiscard]] inline const util::FlatMap<Price, Handle, PriceHash>& level_index_for(
    const util::FlatMap<Price, Handle, PriceHash>& bid,
    const util::FlatMap<Price, Handle, PriceHash>& ask, Side side) noexcept {
    return side == Side::bid ? bid : ask;
}

/// True when `candidate` ranks strictly better than `reference` for
/// `side`: higher price for a bid, lower for an ask.
[[nodiscard]] inline bool is_better(Side side, Price candidate, Price reference) noexcept {
    return side == Side::bid ? candidate > reference : candidate < reference;
}

}  // namespace

OrderBook::OrderBook(std::size_t order_capacity, std::size_t level_capacity,
                     std::size_t index_capacity, LadderConfig ladder)
    : orders_(order_capacity),
      levels_(level_capacity),
      order_index_(order_capacity),
      // Zero means "no separate opinion", i.e. size the index to the
      // level pool as before. Anything else is the caller's stated
      // expectation of how many price levels will be live at once.
      bid_level_index_(index_capacity == 0 ? level_capacity : index_capacity),
      ask_level_index_(index_capacity == 0 ? level_capacity : index_capacity),
      ladder_(ladder) {
    if (ladder_.dense) {
        const std::size_t slots = ladder_.ticks_per_side;
        bid_slots_.assign(slots, kSlotEmpty);
        ask_slots_.assign(slots, kSlotEmpty);
        bid_bits_.assign((slots + 63) / 64, 0);
        ask_bits_.assign((slots + 63) / 64, 0);
    }
    // Populate the free lists in reverse so the first acquire returns
    // handle 0. Deterministic handle assignment keeps the differential
    // test's state comparison reproducible run to run.
    order_free_.reserve(order_capacity);
    for (std::size_t i = order_capacity; i-- > 0;) {
        order_free_.push_back(static_cast<Handle>(i));
    }
    level_free_.reserve(level_capacity);
    for (std::size_t i = level_capacity; i-- > 0;) {
        level_free_.push_back(static_cast<Handle>(i));
    }
}

// ---- Pool management ------------------------------------------------

Handle OrderBook::acquire_order() noexcept {
    if (order_free_.empty()) {
        return kInvalidHandle;
    }
    const Handle h = order_free_.back();
    order_free_.pop_back();
    orders_[h] = OrderNode{};
    orders_[h].allocated = true;
    return h;
}

void OrderBook::release_order(Handle h) noexcept {
    assert(h < orders_.size());
    orders_[h] = OrderNode{};
    order_free_.push_back(h);
}

Handle OrderBook::acquire_level(Side side, Price price) noexcept {
    if (level_free_.empty()) {
        return kInvalidHandle;
    }
    const Handle h = level_free_.back();
    level_free_.pop_back();
    levels_[h] = LevelNode{};
    levels_[h].allocated = true;
    levels_[h].side = side;
    levels_[h].price = price;
    return h;
}

void OrderBook::release_level(Handle h) noexcept {
    assert(h < levels_.size());
    levels_[h] = LevelNode{};
    level_free_.push_back(h);
}

// ---- Dense ladder ----------------------------------------------------
//
// Six small functions that together replace the price hash map and the
// O(depth) ladder walk, for the in-band case. They are deliberately
// separate from the sparse path rather than woven into it: the sparse path
// stays exactly as it was, so a book configured for a sparse instrument
// behaves identically to before, and the dense path is something a
// reviewer can read in one sitting.

namespace {

/// Index of the lowest set bit. Undefined for zero, which is why every
/// caller below checks the word first.
[[nodiscard]] inline std::uint32_t lowest_bit(std::uint64_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<std::uint32_t>(__builtin_ctzll(v));
#else
    unsigned long index = 0;
    _BitScanForward64(&index, v);
    return static_cast<std::uint32_t>(index);
#endif
}

/// Index of the highest set bit. Undefined for zero.
[[nodiscard]] inline std::uint32_t highest_bit(std::uint64_t v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return 63u - static_cast<std::uint32_t>(__builtin_clzll(v));
#else
    unsigned long index = 0;
    _BitScanReverse64(&index, v);
    return static_cast<std::uint32_t>(index);
#endif
}

}  // namespace

std::uint32_t OrderBook::ladder_slot(Price price) const noexcept {
    if (!ladder_.dense || ladder_.tick <= 0) {
        return util::kNoHandle;
    }
    const std::int64_t offset = price.raw() - ladder_.floor_price.raw();
    // Negative offsets would divide toward zero and alias onto slot 0,
    // which is the single most dangerous bug this structure can have: a
    // price below the floor would silently overwrite the level at the
    // bottom of the band. Reject before dividing, not after.
    if (offset < 0) {
        return util::kNoHandle;
    }
    if (offset % ladder_.tick != 0) {
        // A level that is not on a tick boundary is not addressable in a
        // grid. It falls back to the hash index rather than being rounded.
        return util::kNoHandle;
    }
    const std::int64_t slot = offset / ladder_.tick;
    if (slot < 0 || static_cast<std::size_t>(slot) >= ladder_.ticks_per_side) {
        return util::kNoHandle;
    }
    return static_cast<std::uint32_t>(slot);
}

Handle OrderBook::ladder_get(Side side, std::uint32_t slot) const noexcept {
    const std::vector<std::uint32_t>& slots =
        (side == Side::bid) ? bid_slots_ : ask_slots_;
    if (slot >= slots.size()) {
        return kInvalidHandle;
    }
    const std::uint32_t stored = slots[slot];
    return stored == kSlotEmpty ? kInvalidHandle : static_cast<Handle>(stored - 1);
}

void OrderBook::ladder_set(Side side, std::uint32_t slot, Handle h) noexcept {
    std::vector<std::uint32_t>& slots = (side == Side::bid) ? bid_slots_ : ask_slots_;
    std::vector<std::uint64_t>& bits = (side == Side::bid) ? bid_bits_ : ask_bits_;
    if (slot >= slots.size()) {
        return;
    }
    slots[slot] = h + 1;
    bits[slot >> 6] |= (1ULL << (slot & 63u));
}

void OrderBook::ladder_clear(Side side, std::uint32_t slot) noexcept {
    std::vector<std::uint32_t>& slots = (side == Side::bid) ? bid_slots_ : ask_slots_;
    std::vector<std::uint64_t>& bits = (side == Side::bid) ? bid_bits_ : ask_bits_;
    if (slot >= slots.size()) {
        return;
    }
    slots[slot] = kSlotEmpty;
    bits[slot >> 6] &= ~(1ULL << (slot & 63u));
}

std::uint32_t OrderBook::ladder_best_slot(Side side) const noexcept {
    const std::vector<std::uint64_t>& bits = (side == Side::bid) ? bid_bits_ : ask_bits_;
    if (bits.empty()) {
        return util::kNoHandle;
    }
    if (side == Side::ask) {
        // Best ask is the lowest occupied slot: scan words upward, take the
        // first non-empty one.
        for (std::size_t w = 0; w < bits.size(); ++w) {
            if (bits[w] != 0) {
                return static_cast<std::uint32_t>(w * 64u + lowest_bit(bits[w]));
            }
        }
        return util::kNoHandle;
    }
    // Best bid is the highest occupied slot: scan words downward.
    for (std::size_t w = bits.size(); w-- > 0;) {
        if (bits[w] != 0) {
            return static_cast<std::uint32_t>(w * 64u + highest_bit(bits[w]));
        }
    }
    return util::kNoHandle;
}

std::vector<std::uint32_t> OrderBook::ladder_occupied(Side side) const {
    std::vector<std::uint32_t> out;
    const std::vector<std::uint64_t>& bits = (side == Side::bid) ? bid_bits_ : ask_bits_;
    for (std::size_t w = 0; w < bits.size(); ++w) {
        std::uint64_t word = bits[w];
        while (word != 0) {
            const std::uint32_t b = lowest_bit(word);
            out.push_back(static_cast<std::uint32_t>(w * 64u + b));
            word &= word - 1;
        }
    }
    // Bits within a word come out ascending; bids read best-first, so
    // reverse them. Asks are already ascending.
    if (side == Side::bid) {
        std::reverse(out.begin(), out.end());
    }
    return out;
}

// ---- Level ladder ---------------------------------------------------

void OrderBook::link_level(Handle h, std::uint32_t slot) noexcept {
    LevelNode& lv = levels_[h];

    // Dense path: the slot index IS the ordering, so linking is a single
    // store plus one bit. No walk, and nothing to rebalance -- which is
    // the entire reason this structure replaces the linked list.
    if (slot != util::kNoHandle) {
        ladder_set(lv.side, slot, h);
        return;
    }

    // Out of band, or straddles ticks: fall back to the sorted list so a
    // sparse instrument still works. This is the documented tradeoff of a
    // bounded grid, and it degrades to exactly the previous behaviour
    // rather than to a wrong answer.
    Handle& head = (lv.side == Side::bid) ? bid_head_ : ask_head_;

    if (head == kInvalidHandle) {
        lv.prev = kInvalidHandle;
        lv.next = kInvalidHandle;
        head = h;
        return;
    }

    // Walk from the head until `cur` is the first level that ranks at
    // or worse than this one, tracking the node before it. Keeping the
    // predecessor from the walk avoids a second pass to find the tail.
    Handle cur = head;
    Handle prev = kInvalidHandle;
    while (cur != kInvalidHandle && is_better(lv.side, levels_[cur].price, lv.price)) {
        prev = cur;
        cur = levels_[cur].next;
    }

    lv.prev = prev;
    lv.next = cur;

    if (prev != kInvalidHandle) {
        levels_[prev].next = h;
    } else {
        // Ranks better than everything: new head.
        head = h;
    }
    if (cur != kInvalidHandle) {
        levels_[cur].prev = h;
    }
}

void OrderBook::unlink_level(Handle h, std::uint32_t slot) noexcept {
    LevelNode& lv = levels_[h];

    // Dense path: clear the slot and its occupancy bit. Nothing else to
    // touch -- the ordering was the array, not a list.
    if (slot != util::kNoHandle && ladder_get(lv.side, slot) == h) {
        ladder_clear(lv.side, slot);
        return;
    }

    if (lv.prev != kInvalidHandle) {
        levels_[lv.prev].next = lv.next;
    } else {
        Handle& head = (lv.side == Side::bid) ? bid_head_ : ask_head_;
        head = lv.next;
    }
    if (lv.next != kInvalidHandle) {
        levels_[lv.next].prev = lv.prev;
    }
    lv.prev = kInvalidHandle;
    lv.next = kInvalidHandle;
}

// ---- Order lists ----------------------------------------------------

void OrderBook::link_order(Handle h, Handle level) noexcept {
    OrderNode& o = orders_[h];
    LevelNode& lv = levels_[level];

    o.level = level;
    o.prev = lv.tail;
    o.next = kInvalidHandle;

    if (lv.tail == kInvalidHandle) {
        lv.head = h;
    } else {
        orders_[lv.tail].next = h;
    }
    // Appending at the tail is what makes this price-time priority: a
    // later order at the same price cannot jump an earlier one.
    lv.tail = h;

    lv.order_count += 1;
    lv.aggregate_size = Quantity::from_raw(lv.aggregate_size.raw() + o.size.raw());

    if (o.side == Side::bid) {
        bid_aggregate_ = Quantity::from_raw(bid_aggregate_.raw() + o.size.raw());
        bid_orders_ += 1;
    } else {
        ask_aggregate_ = Quantity::from_raw(ask_aggregate_.raw() + o.size.raw());
        ask_orders_ += 1;
    }
}

void OrderBook::unlink_order(Handle h) noexcept {
    OrderNode& o = orders_[h];
    const Handle level = o.level;
    assert(level != kInvalidHandle && "unlinking an order with no level");
    assert(level < levels_.size() && "level handle out of range");
    LevelNode& lv = levels_[level];

    if (o.prev != kInvalidHandle) {
        orders_[o.prev].next = o.next;
    } else {
        lv.head = o.next;
    }
    if (o.next != kInvalidHandle) {
        orders_[o.next].prev = o.prev;
    } else {
        lv.tail = o.prev;
    }
    o.prev = kInvalidHandle;
    o.next = kInvalidHandle;

    lv.aggregate_size = lv.aggregate_size.saturating_sub(o.size);
    assert(lv.order_count > 0 && "level order_count underflow");
    lv.order_count -= 1;

    if (o.side == Side::bid) {
        bid_aggregate_ = bid_aggregate_.saturating_sub(o.size);
        assert(bid_orders_ > 0 && "bid order count underflow");
        bid_orders_ -= 1;
    } else {
        ask_aggregate_ = ask_aggregate_.saturating_sub(o.size);
        assert(ask_orders_ > 0 && "ask order count underflow");
        ask_orders_ -= 1;
    }
}

void OrderBook::reduce_size(Handle h, Quantity new_size) noexcept {
    OrderNode& o = orders_[h];
    const Handle level = o.level;
    assert(level != kInvalidHandle && "reducing an order with no level");

    const std::uint64_t before = o.size.raw();
    const std::uint64_t after = new_size.raw();
    assert(after <= before && "reduce_size may only shrink an order");

    const std::uint64_t delta = before - after;
    o.size = new_size;

    LevelNode& lv = levels_[level];
    lv.aggregate_size = Quantity::from_raw(
        lv.aggregate_size.raw() >= delta ? lv.aggregate_size.raw() - delta : 0);

    if (o.side == Side::bid) {
        bid_aggregate_ = Quantity::from_raw(
            bid_aggregate_.raw() >= delta ? bid_aggregate_.raw() - delta : 0);
    } else {
        ask_aggregate_ = Quantity::from_raw(
            ask_aggregate_.raw() >= delta ? ask_aggregate_.raw() - delta : 0);
    }
}

void OrderBook::detach_order(Handle h) noexcept {
    OrderNode& o = orders_[h];
    const Side side = o.side;
    const Price price = o.price;

    unlink_order(h);
    order_index_.erase(o.id);

    // Only drop the level, and only then remove the price from the
    // level index, if this order was the last one at the price.
    // Erasing the index entry unconditionally is the subtle bug this
    // function exists to avoid: if a second order remained at the same
    // price, removing the index entry would make the level
    // unreachable while orders still pointed at it.
    const Handle level = o.level;
    if (level != kInvalidHandle && levels_[level].order_count == 0) {
        // One division here, on the detach path. The add path threads its
        // slot through instead of recomputing; this path has no slot to
        // thread, and a cancel is not the hot loop the benchmark drives.
        const std::uint32_t slot = ladder_slot(price);
        if (slot == util::kNoHandle) {
            level_index_for(bid_level_index_, ask_level_index_, side).erase(price);
        }
        unlink_level(level, slot);
        release_level(level);
        if (side == Side::bid) {
            assert(bid_levels_ > 0 && "bid level count underflow");
            bid_levels_ -= 1;
        } else {
            assert(ask_levels_ > 0 && "ask level count underflow");
            ask_levels_ -= 1;
        }
    }
    release_order(h);
}

// ---- Lookups --------------------------------------------------------

Handle OrderBook::find_level(Side side, Price price) const noexcept {
    // Dense path first: an array index and a load, with no hashing and no
    // probe. This is the call the whole structure exists to make cheap.
    if (const std::uint32_t slot = ladder_slot(price); slot != util::kNoHandle) {
        const Handle h = ladder_get(side, slot);
        if (h != kInvalidHandle) {
            return h;
        }
        // An empty in-band slot is authoritative: there is no level at
        // this price, so do NOT fall through to the hash index. Falling
        // through would be harmless for correctness only because `add`
        // inserts into both, and relying on that invariant to keep the two
        // structures consistent is how they stop being consistent.
        return kInvalidHandle;
    }
    const auto& idx = level_index_for(bid_level_index_, ask_level_index_, side);
    const std::uint32_t slot = idx.find(price);
    return slot == util::kNoHandle ? kInvalidHandle : idx.value_at(slot);
}

Handle OrderBook::find_order(OrderId id) const noexcept {
    const std::uint32_t slot = order_index_.find(id);
    return slot == util::kNoHandle ? kInvalidHandle : order_index_.value_at(slot);
}

// ---- Mutations ------------------------------------------------------

void OrderBook::prefetch_add(Price price, OrderId id, Side side) noexcept {
    if (id != kInvalidOrderId) {
        order_index_.prefetch(id);
    }
    level_index_for(bid_level_index_, ask_level_index_, side).prefetch(price);
}

Handle OrderBook::add(Side side, Price price, Quantity size, OrderId id,
                      BookStatus& status) noexcept {
    // One walk answers both "is this reference already live?" and "where
    // would it go?". It used to be two: a `contains` here and an `insert`
    // below, both probing a table far larger than any cache on this
    // machine, separated by the level lookup and the pool acquisition --
    // long enough that the second probe missed again rather than hitting
    // L1. Collapsing them is the "remove a dependent memory access" case;
    // making the probe itself cheaper was measured and did nothing. See
    // results/OPTIMIZATION.md.
    bool already_present = false;
    std::uint32_t reserved_slot = order_index_.find_or_reserve(id, already_present);
    if (id == kInvalidOrderId || already_present) {
        // ITCH order references are day-unique. A repeat is a replay
        // or sequence fault, never a second live order.
        status = BookStatus::duplicate_order;
        return kInvalidHandle;
    }
    if (reserved_slot == util::kNoHandle) {
        status = BookStatus::capacity_exhausted;
        return kInvalidHandle;
    }
    if (size.is_zero()) {
        // Zero size rests no liquidity. Venues drop these; creating a
        // level that exists only to be empty would make best_bid()
        // return a price with nothing behind it.
        status = BookStatus::zero_size;
        return kInvalidHandle;
    }

    auto& index = level_index_for(bid_level_index_, ask_level_index_, side);

    // One division, once. The slot is needed by the lookup, by the insert
    // and by the destroy, and computing it three times cost more than the
    // hash probe it replaced -- see the note on `link_level`.
    const std::uint32_t slot = ladder_slot(price);
    const bool in_ladder = slot != util::kNoHandle;

    Handle level;
    if (in_ladder) {
        // An empty in-band slot is authoritative: there is no level at
        // this price, so do NOT fall through to the hash index. Falling
        // through would work only because `add` writes to both, and
        // relying on that to keep them consistent is how they stop being
        // consistent.
        level = ladder_get(side, slot);
    } else {
        const std::uint32_t found = index.find(price);
        level = found == util::kNoHandle ? kInvalidHandle : index.value_at(found);
    }
    bool created_level = false;

    if (level == kInvalidHandle) {
        level = acquire_level(side, price);
        bool placed = false;
        if (level != kInvalidHandle) {
            if (in_ladder) {
                // `link_level` writes the slot and the bit. Nothing else.
                placed = true;
            } else {
                placed = index.insert(price, level);
            }
        }
        if (!placed) {
            if (level != kInvalidHandle) {
                release_level(level);
            }
            status = BookStatus::capacity_exhausted;
            return kInvalidHandle;
        }
        link_level(level, slot);
        if (side == Side::bid) {
            bid_levels_ += 1;
        } else {
            ask_levels_ += 1;
        }
        created_level = true;
    }

    const Handle h = acquire_order();
    if (h == kInvalidHandle) {
        if (created_level) {
            // Unwind the level we speculatively created.
            if (!in_ladder) {
                index.erase(price);
            }
            unlink_level(level, slot);
            release_level(level);
            if (side == Side::bid) {
                bid_levels_ -= 1;
            } else {
                ask_levels_ -= 1;
            }
        }
        status = BookStatus::capacity_exhausted;
        return kInvalidHandle;
    }

    OrderNode& o = orders_[h];
    o.id = id;
    o.side = side;
    o.price = price;
    o.size = size;
    o.state = OrderState::new_order;

    link_order(h, level);

    if (!order_index_.place_reserved(reserved_slot, id, h)) {
        // The slot was reserved above and nothing between here and there
        // inserts into the order index, so this cannot fire
        // single-threaded. Handled anyway, and fully, because leaving the
        // book and the index disagreeing is the failure mode worth being
        // pedantic about.
        unlink_order(h);
        release_order(h);
        if (created_level) {
            if (!in_ladder) {
                index.erase(price);
            }
            unlink_level(level);
            release_level(level);
            if (side == Side::bid) {
                bid_levels_ -= 1;
            } else {
                ask_levels_ -= 1;
            }
        }
        status = BookStatus::capacity_exhausted;
        return kInvalidHandle;
    }

    status = BookStatus::ok;
    return h;
}

BookStatus OrderBook::execute(OrderId id, Quantity qty) noexcept {
    const Handle h = find_order(id);
    if (h == kInvalidHandle) {
        return BookStatus::unknown_order;
    }
    OrderNode& o = orders_[h];
    if (qty.raw() > o.size.raw()) {
        return BookStatus::over_reduce;
    }

    if (qty.raw() == o.size.raw()) {
        // Do NOT zero o.size before detaching. detach_order unlinks the
        // order and unlink_order subtracts o.size from the level
        // aggregate, so clearing the size first would subtract zero and
        // leave the level permanently inflated by this order's size.
        // The node is released a moment later regardless, so its size
        // field never needs clearing.
        o.state = OrderState::filled;
        detach_order(h);
        return BookStatus::ok;
    }

    // Partial fill. The order keeps its position in the queue: a
    // partially filled order is not demoted to the back of its price
    // level, and treating it as though it were would change the
    // priority of every order behind it.
    reduce_size(h, Quantity::from_raw(o.size.raw() - qty.raw()));
    orders_[h].state = OrderState::partially_filled;
    return BookStatus::ok;
}

BookStatus OrderBook::cancel_partial(OrderId id, Quantity qty) noexcept {
    const Handle h = find_order(id);
    if (h == kInvalidHandle) {
        return BookStatus::unknown_order;
    }
    OrderNode& o = orders_[h];
    if (qty.raw() > o.size.raw()) {
        return BookStatus::over_reduce;
    }

    if (qty.raw() == o.size.raw()) {
        // Same constraint as the full-fill path: leave o.size intact so
        // that unlink_order subtracts the true remaining size from the
        // level aggregate.
        o.state = OrderState::cancelled;
        detach_order(h);
        return BookStatus::ok;
    }

    // ITCH 'X' partial cancel. Nothing was executed, so the order
    // returns to `new_order`, and it keeps its queue position: a
    // cancellation is not a promotion.
    reduce_size(h, Quantity::from_raw(o.size.raw() - qty.raw()));
    orders_[h].state = OrderState::new_order;
    return BookStatus::ok;
}

BookStatus OrderBook::remove(OrderId id, Quantity* discarded) noexcept {
    const Handle h = find_order(id);
    if (h == kInvalidHandle) {
        return BookStatus::unknown_order;
    }
    if (discarded != nullptr) {
        *discarded = orders_[h].size;
    }
    // ITCH 'D' discards whatever remains, which is why the caller
    // needs the discarded size to reconcile aggregates.
    orders_[h].state = OrderState::cancelled;
    detach_order(h);
    return BookStatus::ok;
}

// ---- Queries --------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const noexcept {
    std::optional<Price> best;

    // Sparse list first. In dense-only operation this is empty and the
    // cost is one comparison against kInvalidHandle.
    if (bid_head_ != kInvalidHandle) {
        best = levels_[bid_head_].price;
    }

    // Dense path: the best bid is the highest occupied slot, one bit scan
    // regardless of book depth. This is what makes deleting the last order
    // at the inside limit cheap -- without it the dense ladder would trade
    // an O(depth) insert for an O(depth) delete-at-the-touch, which is the
    // documented failure of the pure-array variant.
    if (ladder_.dense) {
        const std::uint32_t slot = ladder_best_slot(Side::bid);
        if (slot != util::kNoHandle) {
            const Handle h = ladder_get(Side::bid, slot);
            if (h != kInvalidHandle) {
                const Price p = levels_[h].price;
                if (!best.has_value() || p > *best) {
                    best = p;
                }
            }
        }
    }
    return best;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    std::optional<Price> best;

    if (ask_head_ != kInvalidHandle) {
        best = levels_[ask_head_].price;
    }

    if (ladder_.dense) {
        const std::uint32_t slot = ladder_best_slot(Side::ask);
        if (slot != util::kNoHandle) {
            const Handle h = ladder_get(Side::ask, slot);
            if (h != kInvalidHandle) {
                const Price p = levels_[h].price;
                if (!best.has_value() || p < *best) {
                    best = p;
                }
            }
        }
    }
    return best;
}

std::optional<OrderSnapshot> OrderBook::find(OrderId id) const noexcept {
    const Handle h = find_order(id);
    if (h == kInvalidHandle) {
        return std::nullopt;
    }
    const OrderNode& o = orders_[h];
    return OrderSnapshot{o.id, o.price, o.size, o.state, o.side};
}

std::optional<Quantity> OrderBook::size_at(Side side, Price price) const noexcept {
    const Handle h = find_level(side, price);
    if (h == kInvalidHandle) {
        return std::nullopt;
    }
    return levels_[h].aggregate_size;
}

std::uint32_t OrderBook::level_count(Side side) const noexcept {
    return side == Side::bid ? bid_levels_ : ask_levels_;
}

std::uint32_t OrderBook::order_count(Side side) const noexcept {
    return side == Side::bid ? bid_orders_ : ask_orders_;
}

Quantity OrderBook::aggregate_at(Side side) const noexcept {
    return side == Side::bid ? bid_aggregate_ : ask_aggregate_;
}

std::vector<LevelSnapshot> OrderBook::levels(Side side) const {
    std::vector<LevelSnapshot> out;

    // Sparse list: out-of-band levels, in price order already.
    Handle cur = (side == Side::bid) ? bid_head_ : ask_head_;
    while (cur != kInvalidHandle) {
        out.push_back(LevelSnapshot{levels_[cur].price, levels_[cur].aggregate_size,
                                    levels_[cur].order_count});
        cur = levels_[cur].next;
    }

    // Dense path: walk the occupancy bits, best first. `ladder_occupied`
    // already returns descending slots for bids and ascending for asks, so
    // appending in that order puts the dense levels in price order -- but
    // they have to be *merged* with the sparse ones, which are not
    // interleaved, so the combined vector is sorted below.
    if (ladder_.dense) {
        for (const std::uint32_t slot : ladder_occupied(side)) {
            const Handle h = ladder_get(side, slot);
            if (h == kInvalidHandle) {
                continue;
            }
            out.push_back(LevelSnapshot{levels_[h].price, levels_[h].aggregate_size,
                                        levels_[h].order_count});
        }
        const bool ascending = (side == Side::ask);
        std::sort(out.begin(), out.end(),
                  [ascending](const LevelSnapshot& a, const LevelSnapshot& b) {
                      return ascending ? (a.price < b.price) : (b.price < a.price);
                  });
    }
    return out;
}

std::vector<OrderSnapshot> OrderBook::orders(Side side) const {
    std::vector<OrderSnapshot> out;

    // Collect level handles in PRICE order first, then walk each level's
    // FIFO queue. The price order is the book's priority order, so it has
    // to be established before the queues are walked -- collecting orders
    // level by level and sorting afterwards would be wrong, because
    // price-time priority is a total order across levels.
    //
    // Both sources have to be visited. In dense-only operation the sparse
    // head is empty and costs one comparison; in sparse-only operation the
    // bitmaps are empty and cost one branch. Only a mixed book pays for the
    // merge, which is the case that arises when a price strays outside the
    // configured band.
    std::vector<Handle> level_handles;

    Handle cur = (side == Side::bid) ? bid_head_ : ask_head_;
    for (; cur != kInvalidHandle; cur = levels_[cur].next) {
        level_handles.push_back(cur);
    }

    if (ladder_.dense) {
        for (const std::uint32_t slot : ladder_occupied(side)) {
            const Handle h = ladder_get(side, slot);
            if (h != kInvalidHandle) {
                level_handles.push_back(h);
            }
        }
        const bool ascending = (side == Side::ask);
        std::sort(level_handles.begin(), level_handles.end(),
                  [this, ascending](Handle a, Handle b) {
                      return ascending ? (levels_[a].price < levels_[b].price)
                                       : (levels_[b].price < levels_[a].price);
                  });
    }

    for (const Handle level : level_handles) {
        Handle o = levels_[level].head;
        while (o != kInvalidHandle) {
            out.push_back(OrderSnapshot{orders_[o].id, orders_[o].price, orders_[o].size,
                                        orders_[o].state});
            o = orders_[o].next;
        }
    }
    return out;
}

}  // namespace hft::lob
