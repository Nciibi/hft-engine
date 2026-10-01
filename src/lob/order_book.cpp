#include "hft/lob/order_book.hpp"

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

OrderBook::OrderBook(std::size_t order_capacity, std::size_t level_capacity)
    : orders_(order_capacity),
      levels_(level_capacity),
      order_index_(order_capacity),
      bid_level_index_(level_capacity),
      ask_level_index_(level_capacity) {
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

// ---- Level ladder ---------------------------------------------------

void OrderBook::link_level(Handle h) noexcept {
    LevelNode& lv = levels_[h];
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
        level_index_for(bid_level_index_, ask_level_index_, side).erase(price);
        unlink_level(level);
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
    if (id == kInvalidOrderId || order_index_.contains(id)) {
        // ITCH order references are day-unique. A repeat is a replay
        // or sequence fault, never a second live order.
        status = BookStatus::duplicate_order;
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
    Handle level = find_level(side, price);
    bool created_level = false;

    if (level == kInvalidHandle) {
        level = acquire_level(side, price);
        if (level == kInvalidHandle || !index.insert(price, level)) {
            if (level != kInvalidHandle) {
                release_level(level);
            }
            status = BookStatus::capacity_exhausted;
            return kInvalidHandle;
        }
        link_level(level);
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
            index.erase(price);
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
        // Unreachable single-threaded: contains() was checked above and
        // nothing between here and there removes entries. Handled
        // anyway, and fully, because leaving the book and the index
        // disagreeing is the failure mode worth being pedantic about.
        unlink_order(h);
        release_order(h);
        if (created_level) {
            index.erase(price);
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
    return bid_head_ == kInvalidHandle ? std::nullopt
                                       : std::optional<Price>{levels_[bid_head_].price};
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    return ask_head_ == kInvalidHandle ? std::nullopt
                                       : std::optional<Price>{levels_[ask_head_].price};
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
