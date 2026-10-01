#include "hft/lob/order_book.hpp"

#include <algorithm>
#include <cassert>

namespace hft::lob {
namespace {

[[nodiscard]] util::FlatMap<Price, Handle, PriceHash>& level_index_for(
    util::FlatMap<Price, Handle, PriceHash>& bid,
    util::FlatMap<Price, Handle, PriceHash>& ask, Side side) noexcept {
    return side == Side::bid ? bid : ask;
}

[[nodiscard]] const util::FlatMap<Price, Handle, PriceHash>& level_index_for(
    const util::FlatMap<Price, Handle, PriceHash>& bid,
    const util::FlatMap<Price, Handle, PriceHash>& ask, Side side) noexcept {
    return side == Side::bid ? bid : ask;
}

}  // namespace

OrderBook::OrderBook(std::size_t order_capacity, std::size_t level_capacity)
    : orders_(order_capacity),
      levels_(level_capacity),
      order_index_(order_capacity),
      bid_level_index_(level_capacity),
      ask_level_index_(level_capacity) {
    // Populate the free lists in reverse so that the first acquire
    // returns handle 0. Deterministic handle assignment matters: it
    // makes the differential test's state comparison reproducible.
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
    OrderNode& n = orders_[h];
    n = OrderNode{};
    n.allocated = true;
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
    LevelNode& n = levels_[h];
    n = LevelNode{};
    n.allocated = true;
    n.side = side;
    n.price = price;
    return h;
}

void OrderBook::release_level(Handle h) noexcept {
    assert(h < levels_.size());
    levels_[h] = LevelNode{};
    level_free_.push_back(h);
}

// ---- Level ladder ---------------------------------------------------

void OrderBook::link_level(Handle h) noexcept {
    LevelNode& lv = levels_[h];
    Handle& head = (lv.side == Side::bid) ? bid_head_ : ask_head_;

    // Bids descend, asks ascend. In both cases "better" means the
    // price compares greater against the head, which is true for bids
    // (higher is better) and false for asks (lower is better), so the
    // two cases are genuinely different walks.
    if (head == kInvalidHandle) {
        lv.prev = kInvalidHandle;
        lv.next = kInvalidHandle;
        head = h;
        return;
    }

    if (lv.side == Side::bid) {
        if (lv.price > levels_[head].price) {
            // New level outbids the current best: becomes the head.
            lv.next = head;
            lv.prev = kInvalidHandle;
            levels_[head].prev = h;
            head = h;
            return;
        }
        // Walk to the first level this one should precede.
        Handle cur = head;
        while (cur != kInvalidHandle && levels_[cur].price > lv.price) {
            cur = levels_[cur].next;
        }
    } else {
        if (lv.price < levels_[head].price) {
            lv.next = head;
            lv.prev = kInvalidHandle;
            levels_[head].prev = h;
            head = h;
            return;
        }
        Handle cur = head;
        while (cur != kInvalidHandle && levels_[cur].price < lv.price) {
            cur = levels_[cur].next;
        }
    }

    // `cur` is the first level that ranks at or worse than this one, or
    // the end of the list. Insert immediately before it.
    lv.prev = cur == kInvalidHandle ? kInvalidHandle : levels_[cur].prev;
    lv.next = cur;
    if (cur == kInvalidHandle) {
        // Appending at the tail: the previous tail becomes `lv.prev`.
        // Recompute it, since the walk above only set `cur`.
        Handle tail = (lv.side == Side::bid) ? bid_head_ : ask_head_;
        while (tail != kInvalidHandle && levels_[tail].next != kInvalidHandle) {
            tail = levels_[tail].next;
        }
        lv.prev = tail;
    } else {
        levels_[cur].prev = h;
    }
    if (lv.prev != kInvalidHandle) {
        levels_[lv.prev].next = h;
    }
}

void OrderBook::unlink_level(Handle h) noexcept {
    LevelNode& lv = levels_[h];
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

    o.prev = lv.tail;
    o.next = kInvalidHandle;

    if (lv.tail == kInvalidHandle) {
        lv.head = h;
    } else {
        orders_[lv.tail].next = h;
    }
    // Appending at the tail is what makes this price-time priority: a
    // later order at the same price cannot jump ahead of an earlier one.
    lv.tail = h;

    lv.order_count += 1;
    lv.aggregate_size = Quantity::from_raw(lv.aggregate_size.raw() + o.size.raw());
    o.state = OrderState::new_order;

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
    const Handle level = find_level(o.side, o.price);
    if (level == kInvalidHandle) {
        // The level must exist for a linked order. If it does not, the
        // book's own invariants are broken, which is a bug we want to
        // hear about immediately rather than a feed condition to
        // tolerate.
        assert(false && "linked order has no price level");
        return;
    }
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

    lv.aggregate_size = lv.aggregate_size.saturating_sub(o.size);
    assert(lv.order_count > 0);
    lv.order_count -= 1;

    if (o.side == Side::bid) {
        bid_aggregate_ = bid_aggregate_.saturating_sub(o.size);
        assert(bid_orders_ > 0);
        bid_orders_ -= 1;
    } else {
        ask_aggregate_ = ask_aggregate_.saturating_sub(o.size);
        assert(ask_orders_ > 0);
        ask_orders_ -= 1;
    }
}

void OrderBook::detach_order(Handle h) noexcept {
    OrderNode& o = orders_[h];
    unlink_order(h);
    order_index_.erase(o.id);
    level_index_for(bid_level_index_, ask_level_index_, o.side).erase(o.price);

    // Drop the level once it is empty. Keeping empty levels in the
    // ladder would make best_bid()/best_ask() return a price with
    // nothing resting at it, which is a correctness bug, not a
    // tidiness issue.
    const Handle level = find_level(o.side, o.price);
    if (level != kInvalidHandle && levels_[level].order_count == 0) {
        if (o.side == Side::bid) {
            assert(bid_levels_ > 0);
            bid_levels_ -= 1;
        } else {
            assert(ask_levels_ > 0);
            ask_levels_ -= 1;
        }
        unlink_level(level);
        release_level(level);
    }
    release_order(h);
}

// ---- Lookups --------------------------------------------------------

Handle OrderBook::find_level(Side side, Price price) const noexcept {
    const auto& idx = level_index_for(bid_level_index_, ask_level_index_, side);
    const std::uint32_t slot = idx.find(price);
    return slot == util::kNoHandle ? kInvalidHandle : idx.value_at(slot);
}

Handle OrderBook::find_order(OrderId id) const noexcept {
    const std::uint32_t slot = order_index_.find(id);
    return slot == util::kNoHandle ? kInvalidHandle : order_index_.value_at(slot);
}

// ---- Mutations ------------------------------------------------------

Handle OrderBook::add(Side side, Price price, Quantity size, OrderId id,
                      BookStatus& status) noexcept {
    if (id == kInvalidOrderId) {
        status = BookStatus::duplicate_order;
        return kInvalidHandle;
    }
    if (size.is_zero()) {
        // A zero-size order rests no liquidity and carries no
        // information. ITCH venues drop these; so do we, rather than
        // creating a level that exists only to be empty.
        status = BookStatus::zero_size;
        return kInvalidHandle;
    }
    if (order_index_.contains(id)) {
        status = BookStatus::duplicate_order;
        return kInvalidHandle;
    }

    Handle level = find_level(side, price);
    if (level == kInvalidHandle) {
        level = acquire_level(side, price);
        if (level == kInvalidHandle) {
            status = BookStatus::capacity_exhausted;
            return kInvalidHandle;
        }
        link_level(level);
        if (!level_index_for(bid_level_index_, ask_level_index_, side).insert(price, level)) {
            unlink_level(level);
            release_level(level);
            status = BookStatus::capacity_exhausted;
            return kInvalidHandle;
        }
        if (side == Side::bid) {
            bid_levels_ += 1;
        } else {
            ask_levels_ += 1;
        }
    }

    const Handle h = acquire_order();
    if (h == kInvalidHandle) {
        // Roll the level back if we created it and it is now empty.
        if (levels_[level].order_count == 0) {
            level_index_for(bid_level_index_, ask_level_index_, side).erase(price);
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

    OrderNode& o = orders_[h];
    o.id = id;
    o.side = side;
    o.price = price;
    o.size = size;
    o.original_size = size;
    o.state = OrderState::new_order;

    link_order(h, level);

    if (!order_index_.insert(id, h)) {
        // Only reachable if the index filled between the contains()
        // check and here, which single-threaded use cannot do. Unwind
        // rather than leave the book and the index disagreeing.
        unlink_order(h);
        release_order(h);
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
        o.size = Quantity{};
        o.state = OrderState::filled;
        detach_order(h);
        return BookStatus::ok;
    }

    // Partial fill. Reducing size changes the level aggregate, so the
    // order must be unlinked and relinked rather than mutated in place.
    // Marking state before the relink keeps the new level aggregate
    // consistent with the remaining size.
    o.size = Quantity::from_raw(o.size.raw() - qty.raw());
    o.state = OrderState::partially_filled;
    const Handle level = find_level(o.side, o.price);
    const Side side = o.side;
    const Price price = o.price;
    unlink_order(h);
    link_order(h, level);
    // link_order resets state to new_order; restore the correct one.
    orders_[h].state = OrderState::partially_filled;
    (void)side;
    (void)price;
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
        o.size = Quantity{};
        o.state = OrderState::cancelled;
        detach_order(h);
        return BookStatus::ok;
    }

    // ITCH 'X': a partial cancel leaves the order working with a
    // reduced size, and it keeps its place in the queue. Treated
    // identically to a partial fill for bookkeeping, but the resulting
    // state is `new_order` because nothing was executed.
    o.size = Quantity::from_raw(o.size.raw() - qty.raw());
    o.state = OrderState::new_order;
    const Handle level = find_level(o.side, o.price);
    unlink_order(h);
    link_order(h, level);
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
    orders_[h].state = OrderState::cancelled;
    detach_order(h);
    return BookStatus::ok;
}

// ---- Queries --------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bid_head_ == kInvalidHandle) {
        return std::nullopt;
    }
    return levels_[bid_head_].price;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (ask_head_ == kInvalidHandle) {
        return std::nullopt;
    }
    return levels_[ask_head_].price;
}

std::optional<OrderSnapshot> OrderBook::find(OrderId id) const noexcept {
    const Handle h = find_order(id);
    if (h == kInvalidHandle) {
        return std::nullopt;
    }
    const OrderNode& o = orders_[h];
    return OrderSnapshot{o.id, o.price, o.size, o.state};
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
    Handle cur = (side == Side::bid) ? bid_head_ : ask_head_;
    while (cur != kInvalidHandle) {
        out.push_back(LevelSnapshot{levels_[cur].price, levels_[cur].aggregate_size,
                                    levels_[cur].order_count});
        cur = levels_[cur].next;
    }
    return out;
}

std::vector<OrderSnapshot> OrderBook::orders(Side side) const {
    std::vector<OrderSnapshot> out;
    Handle level = (side == Side::bid) ? bid_head_ : ask_head_;
    while (level != kInvalidHandle) {
        Handle o = levels_[level].head;
        while (o != kInvalidHandle) {
            out.push_back(OrderSnapshot{orders_[o].id, orders_[o].price, orders_[o].size,
                                        orders_[o].state});
            o = orders_[o].next;
        }
        level = levels_[level].next;
    }
    return out;
}

}  // namespace hft::lob
