// The fast order book.
//
// Structure, and why each piece is what it is:
//
//  * Orders and price levels both live in preallocated pools and are
//    referenced by 32-bit handles, never pointers. A handle keeps the
//    pool relocatable, makes "is this a live order" a bounds check
//    rather than a dereference, and costs 4 bytes instead of 8.
//  * Each side keeps its price levels in an intrusive doubly-linked
//    list sorted by price: bids descending (best = highest), asks
//    ascending (best = lowest). Best bid and best ask are therefore
//    O(1) reads of the list heads, which is the only thing a market
//    maker asks the book on every tick.
//  * Each level holds its orders in an intrusive list in arrival
//    order, giving price-time priority without a sort.
//  * OrderId -> handle and price -> level both use a fixed-capacity
//    flat hash table. Nothing in the update path allocates.
//
// Update semantics follow ITCH, and the distinction between the two
// cancellation messages is load-bearing:
//   * Order Executed  ('E') reduces remaining size; zero size removes.
//   * Order Cancel    ('X') is a PARTIAL cancel: shares are deducted
//     from the quantity stated in the original Add Order. Reaching zero
//     removes the order.
//   * Order Delete    ('D') removes the entire order outright.
// Conflating 'X' with 'D' is the most common ITCH book bug.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "hft/types.hpp"
#include "hft/util/flat_map.hpp"

namespace hft::lob {

using Handle = std::uint32_t;
inline constexpr Handle kInvalidHandle = util::kNoHandle;

/// Capacity defaults are sized for a single-instrument book. A real
/// multi-instrument deployment partitions by symbol and runs one book
/// per core, which is why a bounded per-book capacity is a feature
/// rather than a limitation.
inline constexpr std::size_t kDefaultOrderCapacity = 1u << 20;
inline constexpr std::size_t kDefaultLevelCapacity = 1u << 16;

/// Outcome of an order mutation. Distinct statuses exist so the caller
/// can distinguish "rejected by the venue's rules" from "you asked for
/// something the protocol does not permit", which are different bugs.
enum class BookStatus : std::uint8_t {
    ok = 0,
    /// No such order on the book.
    unknown_order,
    /// OrderId is already live. ITCH OrderIds are day-unique, so a
    /// duplicate is a sequence or replay fault, not a new order.
    duplicate_order,
    /// A zero-size order. Dropped at the venue, so dropped here too.
    zero_size,
    /// Reduce or delete would drive remaining size below zero.
    over_reduce,
    /// The pool or the index was full. Never silently grows.
    capacity_exhausted,
};

struct OrderNode {
    OrderId id = kInvalidOrderId;
    Price price{};
    Quantity size{};           ///< Remaining
    Quantity original_size{};  ///< As added
    Side side = Side::bid;
    OrderState state = OrderState::new_order;
    /// Owning price level. Cached so that unlinking an order does not
    /// need a hash lookup to find the level it belongs to.
    Handle level = kInvalidHandle;
    Handle prev = kInvalidHandle;  ///< Within the price level
    Handle next = kInvalidHandle;
    bool allocated = false;
};

struct LevelNode {
    Price price{};
    Side side = Side::bid;
    Handle head = kInvalidHandle;  ///< Oldest order: highest priority
    Handle tail = kInvalidHandle;  ///< Newest order
    std::uint32_t order_count = 0;
    Quantity aggregate_size{};
    Handle prev = kInvalidHandle;  ///< Within the side's price ladder
    Handle next = kInvalidHandle;
    bool allocated = false;
};

/// Hash a Price by its raw value. Price is a strong type with no
/// std::hash specialisation, and inventing one in namespace std would
/// be the wrong place to do it.
struct PriceHash {
    [[nodiscard]] std::size_t operator()(Price p) const noexcept {
        return std::hash<std::int64_t>{}(p.raw());
    }
};

/// One price level's worth of state, for cross-implementation
/// comparison in tests.
struct LevelSnapshot {
    Price price{};
    Quantity aggregate_size{};
    std::uint32_t order_count = 0;
};

/// One order's worth of state, for cross-implementation comparison.
struct OrderSnapshot {
    OrderId id = kInvalidOrderId;
    Price price{};
    Quantity size{};
    OrderState state = OrderState::new_order;
    /// Which side the order rests on.
    ///
    /// Added for Order Replace. The 'U' message carries no side, no
    /// stock and no MPID -- the specification has the replacement
    /// inherit them from the original Add Order -- so applying one
    /// means looking the original up and reading its side back out.
    /// Without this field the apply layer would have to infer the side
    /// from the price, which is a guess that fails on any book whose
    /// mid is not where you expect it.
    Side side = Side::bid;
};

class OrderBook final {
public:
    OrderBook()
        : OrderBook(kDefaultOrderCapacity, kDefaultLevelCapacity) {}

    OrderBook(std::size_t order_capacity, std::size_t level_capacity);

    // ---- Mutations ----------------------------------------------------

    /// Add a working order. Returns the handle, or kInvalidHandle on
    /// rejection.
    ///
    /// Deliberately NOT [[nodiscard]]: the status out-param is the
    /// primary channel, and a caller that only needs to know whether
    /// the add was accepted has no use for the handle. Marking it
    /// nodiscard would force a cast-to-void on every such call, which
    /// is noise that trains people to ignore the attribute.
    Handle add(Side side, Price price, Quantity size, OrderId id,
               BookStatus& status) noexcept;

    /// ITCH 'E': consume `qty` from the order. Removes the order when
    /// its remaining size reaches zero.
    BookStatus execute(OrderId id, Quantity qty) noexcept;

    /// ITCH 'X': deduct `qty` from remaining size. Removes the order
    /// when it reaches zero. This is a partial cancellation and is not
    /// equivalent to `remove`.
    BookStatus cancel_partial(OrderId id, Quantity qty) noexcept;

    /// ITCH 'D': remove the entire order. Reports the remaining size
    /// that was discarded, which the caller needs to reconcile
    /// aggregate quantities.
    BookStatus remove(OrderId id, Quantity* discarded = nullptr) noexcept;

    // ---- Queries ------------------------------------------------------

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;

    [[nodiscard]] std::optional<OrderSnapshot> find(OrderId id) const noexcept;

    [[nodiscard]] std::optional<Quantity> size_at(Side side, Price price) const noexcept;

    [[nodiscard]] std::uint32_t level_count(Side side) const noexcept;
    [[nodiscard]] std::uint32_t order_count(Side side) const noexcept;
    [[nodiscard]] Quantity aggregate_at(Side side) const noexcept;

    /// Levels for one side, best first. Allocates; intended for tests,
    /// reporting and the reference comparison, not the hot path.
    [[nodiscard]] std::vector<LevelSnapshot> levels(Side side) const;

    /// Orders for one side, ordered by price then arrival, which is
    /// exactly the book's priority order. Allocates; test use only.
    [[nodiscard]] std::vector<OrderSnapshot> orders(Side side) const;

private:
    [[nodiscard]] Handle acquire_order() noexcept;
    void release_order(Handle h) noexcept;
    [[nodiscard]] Handle acquire_level(Side side, Price price) noexcept;
    void release_level(Handle h) noexcept;

    /// Insert level `h` into its side's ladder, keeping price order.
    void link_level(Handle h) noexcept;
    void unlink_level(Handle h) noexcept;

    /// Append order `h` to the tail of level `level` and roll the
    /// aggregate counters. Does NOT set `state`: the caller owns the
    /// order's lifecycle and must set it, so that a relink for a
    /// partial fill does not silently reset the state to new_order.
    void link_order(Handle h, Handle level) noexcept;
    void unlink_order(Handle h) noexcept;

    /// Change an order's remaining size, rolling the level and side
    /// aggregates to match while leaving the order exactly where it is
    /// in the queue.
    ///
    /// This is the correct path for a partial fill and for a partial
    /// cancel. Neither event changes priority: an order that is half
    /// filled keeps its position, and an order that is partially
    /// cancelled is not promoted past orders ahead of it. Unlinking and
    /// relinking to refresh the aggregate would move it to the tail of
    /// its level, which is a real and expensive bug that a book which
    /// only ever appends would never surface.
    void reduce_size(Handle h, Quantity new_size) noexcept;

    /// Detach an order, release its handle, and drop its price level
    /// if that level became empty.
    void detach_order(Handle h) noexcept;

    [[nodiscard]] Handle find_level(Side side, Price price) const noexcept;
    [[nodiscard]] Handle find_order(OrderId id) const noexcept;

    std::vector<OrderNode> orders_;
    std::vector<LevelNode> levels_;
    std::vector<Handle> order_free_;
    std::vector<Handle> level_free_;

    util::FlatMap<OrderId, Handle> order_index_;
    // One table per side rather than a single (side, price) composite
    // key. Packing a side bit into a 64-bit price would require
    // shifting the price left by one, which discards its sign bit; two
    // tables cost a little memory and remove that class of bug
    // entirely.
    util::FlatMap<Price, Handle, PriceHash> bid_level_index_;
    util::FlatMap<Price, Handle, PriceHash> ask_level_index_;

    /// Ladder heads. Bids descend from best to worst, asks ascend.
    Handle bid_head_ = kInvalidHandle;
    Handle ask_head_ = kInvalidHandle;

    std::uint32_t bid_levels_ = 0;
    std::uint32_t ask_levels_ = 0;
    std::uint32_t bid_orders_ = 0;
    std::uint32_t ask_orders_ = 0;
    Quantity bid_aggregate_{};
    Quantity ask_aggregate_{};
};

}  // namespace hft::lob
