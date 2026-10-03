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
    Side side = Side::bid;
    OrderState state = OrderState::new_order;
    /// Owning price level. Cached so that unlinking an order does not
    /// need a hash lookup to find the level it belongs to.
    Handle level = kInvalidHandle;
    Handle prev = kInvalidHandle;  ///< Within the price level
    Handle next = kInvalidHandle;
    bool allocated = false;
    // `original_size` used to live here and was written on every add and
    // read by nothing: the book tracks the as-added quantity only where a
    // cancel needs to distinguish a partial from a full reduction, and it
    // does that from `size` and the level aggregate. Eight bytes per order
    // in the one structure that every single add writes, purely as dead
    // weight, so it is gone. sizeof(OrderNode) is now 48 rather than 56.
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
/// Dense price-ladder geometry.
///
/// A dense ladder replaces "find the price, then walk a sorted linked list
/// to position it" with an array index. It is the structure every serious
/// low-latency book converges on, for reasons that are worth recording
/// because they are the opposite of what the linked list was chosen for:
///
///   * **O(1) insert and O(1) find, with no hashing.** The index is
///     `(price - floor) / tick`. There is no probe, no collision, and no
///     probe-length distribution.
///   * **Contiguous, so it is cache-friendly in the way the hash map is
///     not.** A 4096-tick band is 16 KB per side at 4 bytes per entry --
///     it lives in L2, or mostly in L1. The hash map it replaces was
///     sized from the *order* count, which at 1.6M records meant 125 MiB
///     of table holding ~2,000 live levels: 99.9% empty, and every single
///     lookup still a DRAM miss because the miss is on the address, not
///     the contents.
///   * **No pointer chasing to position a level.** This is what removes the
///     O(depth) walk the linked list imposed, measured at 178x a touch
///     insert at 3,200 levels.
///
/// The cost, and it is a real one, is the **bounded band**. A price outside
/// `[floor, floor + ticks*tick)` has no slot. That is not silently
/// truncated: such a price falls back to the hash index, so a sparse
/// instrument keeps working and a liquid one never touches the fallback.
/// What the band buys is bounded memory and constant-time access; what it
/// costs is that the band must be configured to cover the instrument.
///
/// ### Why the occupancy bitmap is not optional
///
/// The documented failure of the pure-array variant is that removing the
/// last order at the inside limit becomes O(M): to find the new best you
/// must scan downward for the next occupied tick. The fix is a per-side
/// bitmap of occupied ticks, maintained on every level create and destroy,
/// so best-bid is the highest set bit and best-ask is the lowest -- one
/// `ctz`/`clz` per query regardless of book depth.
///
/// Without it this structure trades an O(depth) insert for an O(depth)
/// delete-at-the-touch, and a market maker cancels at the touch constantly.
struct LadderConfig {
    /// Master switch. Off by default, so every existing construction site
    /// keeps its current behaviour and the differential test still compares
    /// like with like.
    bool dense = false;

    /// Lowest price the band can represent. Anything below is out of band.
    Price floor_price = Price::from_int(0);

    /// Tick size in raw price units. A book straddling two ticks has no
    /// slot, because its levels are not addressable.
    std::int64_t tick = 1;

    /// Ticks per side. 4096 is enough for a liquid equity quoted at one
    /// cent and keeps each side's index array at 16 KB.
    std::size_t ticks_per_side = 4096;
};

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

    /// `index_capacity` bounds the three hash tables -- order reference and
    /// the two price indexes -- independently of the pools, and defaults
    /// to `order_capacity` so existing callers are unaffected.
    ///
    /// It exists because sizing the indexes to the order count is a trap,
    /// and an expensive one. `hft_stage_bench` at 1.6M records was building
    /// two 54 MiB price indexes and one 17 MiB order index -- 125 MiB of
    /// table -- to hold the ~2,000 live levels and orders its book actually
    /// contains. Those tables were 99.9% empty and *every* add still paid a
    /// DRAM miss to probe them, because `index_for` masks the hash to the
    /// table capacity and the capacity was enormous. The emptiness does not
    /// help: the miss is on the address, not on the contents.
    ///
    /// The pools must still be sized for the worst case, since any order
    /// can arrive at any price. The *indexes* do not have to be: a book
    /// that outgrows its index gets a clean `capacity_exhausted`, which is
    /// the correct and loud failure, rather than a quarter-gigabyte of
    /// sparse table that makes every lookup slow forever.
    OrderBook(std::size_t order_capacity, std::size_t level_capacity,
              std::size_t index_capacity = 0, LadderConfig ladder = LadderConfig{});

    ~OrderBook() = default;

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

    /// True when `price` is addressable by the dense ladder.
    ///
    /// Exposed so a benchmark can assert that its band actually covers the
    /// prices it is about to use. Without this, a mis-sized band is
    /// invisible: every probe falls out of band, takes the sparse fallback,
    /// and the tool reports the fallback's numbers under a dense heading.
    [[nodiscard]] bool dense_covers(Price price) const noexcept {
        return ladder_slot(price) != util::kNoHandle;
    }

    /// Warm the cache lines that `add` is about to touch.
    ///
    /// Purely advisory: it changes no state and is safe to call with keys
    /// that will never be added, or out of order, or not at all. It exists
    /// because a real market-data handler knows its next message before it
    /// finishes the current one, and the two hash probes plus the order-pool
    /// write are all random accesses into structures far larger than any
    /// level cache.
    ///
    /// A hint, not a correctness mechanism. Nothing checks that a prefetched
    /// key was the key eventually added, because the whole point is that it
    /// may not be -- an order can be rejected between the prefetch and the
    /// add, and the prefetched line is simply unused.
    void prefetch_add(Price price, OrderId id, Side side) noexcept;

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

    // ---- Dense ladder -------------------------------------------------
    //
    // One index space per side, shared range, separate occupancy. An entry
    // stores `handle + 1` so that zero means "no level here" and a real
    // handle of 0 is representable. The bitmaps exist so best-bid and
    // best-ask are a bit scan rather than a scan of slots -- see the note
    // on `LadderConfig` for why that is not optional.
    static constexpr std::size_t kSlotEmpty = 0;

    [[nodiscard]] bool ladder_enabled() const noexcept { return ladder_.dense; }

    /// Ladder slot for `price`, or `kNoHandle` if it is out of band or
    /// does not sit on a tick boundary.
    [[nodiscard]] std::uint32_t ladder_slot(Price price) const noexcept;

    void ladder_set(Side side, std::uint32_t slot, Handle h) noexcept;
    void ladder_clear(Side side, std::uint32_t slot) noexcept;

    /// Handle at `slot`, or `kInvalidHandle`.
    [[nodiscard]] Handle ladder_get(Side side, std::uint32_t slot) const noexcept;

    /// Best occupied slot on `side`, or `kNoHandle` when the side is empty.
    [[nodiscard]] std::uint32_t ladder_best_slot(Side side) const noexcept;

    /// Every occupied slot on `side`, best first. Test and reporting use
    /// only; it is a bit walk, not a hot path.
    [[nodiscard]] std::vector<std::uint32_t> ladder_occupied(Side side) const;

    /// Insert level `h` into its side's ladder, keeping price order.
    /// Insert level `h` into its side's ladder.
    ///
    /// `slot` is the dense slot, already computed by the caller, or
    /// `kNoHandle` to use the sorted list. Passing it matters more than it
    /// looks: recomputing it here costs an integer division by a runtime
    /// value -- roughly 30 cycles -- and the add path touches this three
    /// times over. Measured, that division was the entire difference
    /// between the dense ladder running 2.6x *slower* than the hash map it
    /// replaces and running slightly faster than it.
    void link_level(Handle h, std::uint32_t slot) noexcept;
    void unlink_level(Handle h, std::uint32_t slot) noexcept;

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

    /// Dense ladder state. Empty and untouched unless `ladder_.dense`, so
    /// the default construction costs nothing.
    ///
    /// Indexed by ladder slot, not by handle. `ticks_per_side` entries per
    /// side, holding `handle + 1`, plus one occupancy word per 64 slots.
    std::vector<std::uint32_t> bid_slots_;
    std::vector<std::uint32_t> ask_slots_;
    // 64-bit, not 32. A narrower word would silently truncate the shift
    // `1ULL << (slot & 63)` and set the wrong bit for every slot whose bit
    // index exceeds 31 -- which is half of them, and would surface as a
    // best_bid pointing at the wrong price. `-Wconversion` catches this at
    // compile time, which is the only reason it was caught at all.
    std::vector<std::uint64_t> bid_bits_;
    std::vector<std::uint64_t> ask_bits_;
    LadderConfig ladder_{};

    /// Cached index of the highest (bid) or lowest (ask) bitmap word that
    /// can contain an occupied slot.
    ///
    /// A full bit scan is O(band/64) -- 1,024 words for a 65,536-tick band
    /// -- and `best_bid()` is read on nearly every message, so scanning made
    /// the dense ladder **2.6x slower than the hash map it replaced**. That
    /// is the single most expensive mistake available when implementing a
    /// price grid, and it is invisible until measured.
    ///
    /// The hint turns the common case into a couple of instructions:
    /// `ladder_set` raises it when a word beyond it becomes occupied, and
    /// `ladder_clear` only rescans downward when the cleared slot was in
    /// the hinted word, which for a liquid book finds the next occupied
    /// word immediately.
    std::uint32_t bid_hint_ = 0;
    std::uint32_t ask_hint_ = 0;

    /// Ladder heads, used only by the sparse path. Bids descend from best
    /// to worst, asks ascend. The dense path does not maintain these --
    /// that is the entire point of the occupancy bitmap.
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
