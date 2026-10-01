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
};

class OrderBook final {
public:
    OrderBook()
        : OrderBook(kDefaultOrderCapacity, kDefaultLevelCapacity) {}

    OrderBook(std::size_t order_capacity, std::size_t level_capacity);

    // ---- Mutations ----------------------------------------------------

    /// Add a working order. Returns the handle, or kInvalidHandle on
    /// rejection; `status` always explains why.
    [[nodiscard]] Handle add(Side side, Price price, Quantity size, OrderId id,
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

    /// Append order `h` to the tail of level `level`.
    void link_order(Handle h, Handle level) noexcept;
    void unlink_order(Handle h) noexcept;

    /// Detach an order and release its handle. Assumes it is linked.
    void detach_order(Handle h) noexcept;

    [[nodiscard]] Handle find_level(Side side, Price price) const noexcept;
    [[nodiscard]] Handle find_order(OrderId id) const noexcept;

    std::vector<OrderNode> orders_;
    std::vector<LevelNode> levels_;
    std::vector<Handle> order_free_;
    std::vector<Handle> level_free_;

    util::FlatMap<OrderId, Handle> order_index_;
    util::FlatMap<std::uint64_t, Handle> level_index_;

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
