// Symbol-sharded order books.
//
// The problem this exists to solve
// ------------------------------
// TotalView-ITCH carries a stock symbol in Add Order and in nothing
// else. Order Executed, Order Cancel and Order Delete name only the
// order reference number. So a handler maintaining several books at once
// cannot decide which book a mutation belongs to by reading the message.
// It has to remember.
//
// That memory -- order reference number to owning book -- is the actual
// data structure of a multi-symbol book, and it is the part that decides
// whether sharding works. The books themselves are embarrassingly
// parallel and need no help; the routing does.
//
// ---- How routing is done here, and why not the other ways -----------
//
// 1. SEARCH EVERY SHARD. Correct, and O(shards) per mutation. Fine at
//    two shards, unusable at sixty-four, and it gets worse exactly
//    where sharding was supposed to help.
//
// 2. ENCODE THE SHARD IN THE REFERENCE NUMBER. The cheapest thing that
//    works: the venue lets the client choose the reference, so a client
//    can put a shard id in the high bits. O(1) with no shared state at
//    all. It is also a real answer, and it is what many venues' own
//    documentation suggests.
//
//    It is NOT what this file does, for one reason: it puts a
//    correctness requirement into a number that arrives from outside.
//    If the reference does not carry the shard -- a venue-generated
//    reference, a different client on the same feed, a future venue
//    that assigns references -- the routing is silently wrong, and the
//    symptom is a mutation applied to a book that never saw the order.
//    That is not a crash. It is inventory created out of nothing.
//
// 3. KEEP THE INDEX. A table from reference number to shard, written
//    when an order is added and read when it is mutated.
//
// This is (3). It needs no cooperation from the venue, it degrades to
// "the order is unknown" rather than "the order is on the wrong book",
// and it makes the routing decision explicit and testable.
//
// ---- Concurrency ----------------------------------------------------
//
// Sharding by symbol alone is not enough to make the index safe. Two
// symbols can hash to the same shard, and then two threads write to the
// same book and the same index buckets.
//
// So the index is sharded by the SAME function that assigns books, and
// each index bucket is owned by the thread that owns the corresponding
// book. That is what makes the whole thing work without a lock:
//
//   symbol -> shard            (stable, computed once per symbol)
//   shard  -> book + index sub-table
//   ref    -> shard            (looked up in a per-shard sub-table)
//
// A message for symbol S is routed to shard(S) by the thread that owns
// shard(S). The same thread is the only writer of the ref -> shard entry
// for any order belonging to S, because only shard(S) can receive an
// Add for S. So no index entry is ever written by one thread and read by
// another, and the index needs no synchronisation at all.
//
// That is the whole design, and it is why it is worth writing down: the
// trick is not making the index concurrent, it is arranging for there to
// be exactly one thread per index entry.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hft/lob/order_book.hpp"
#include "hft/types.hpp"

namespace hft::lob {

/// Number of bytes in an ITCH stock symbol field.
inline constexpr std::size_t kSymbolSize = 8;

/// A stock symbol as carried on the wire: fixed width, space padded.
///
/// Fixed-width rather than a `std::string` because the field is
/// fixed-width, and comparing eight bytes with `memcmp` is both faster
/// than comparing strings and immune to the question of what a
/// right-padded name means. An unpadded comparison would treat
/// "AAPL     " and "AAPL" as the same symbol, which is correct, and
/// would also treat "AAPL " and "AAPL" as the same, which is why the
/// padding is normalised rather than merely stored.
class Symbol final {
public:
    static constexpr char kPad = ' ';

    constexpr Symbol() noexcept = default;

    /// Build from eight wire bytes.
    ///
    /// Trailing spaces are stripped and the remainder is blank filled,
    /// so the same symbol written with and without padding compares
    /// equal. Leading and interior spaces are preserved, because a
    /// symbol may legitimately contain one and silently normalising it
    /// would merge two different instruments.
    [[nodiscard]] static Symbol from_wire(const char (&bytes)[kSymbolSize]) noexcept {
        Symbol s;
        std::size_t end = kSymbolSize;
        while (end > 0 && bytes[end - 1] == kPad) {
            --end;
        }
        for (std::size_t i = 0; i < end; ++i) {
            s.bytes_[i] = bytes[i];
        }
        for (std::size_t i = end; i < kSymbolSize; ++i) {
            s.bytes_[i] = kPad;
        }
        s.length_ = static_cast<std::uint8_t>(end);
        return s;
    }

    [[nodiscard]] constexpr const char* c_str() const noexcept { return bytes_; }
    [[nodiscard]] constexpr std::size_t length() const noexcept { return length_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return length_ == 0; }

    /// Case-insensitive, per the venue's own convention: ITCH symbols
    /// are upper case on the wire and feeds are not consistent about it.
    /// Comparing raw bytes would split one instrument into two books and
    /// the resulting half-empty books would be very hard to diagnose.
    [[nodiscard]] bool operator==(const Symbol& other) const noexcept {
        for (std::size_t i = 0; i < length_; ++i) {
            if (upper(bytes_[i]) != upper(other.bytes_[i])) {
                return false;
            }
        }
        return length_ == other.length_;
    }

private:
    [[nodiscard]] static constexpr char upper(char c) noexcept {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }

    char bytes_[kSymbolSize] = {kPad, kPad, kPad, kPad, kPad, kPad, kPad, kPad};
    std::uint8_t length_ = 0;
};

static_assert(sizeof(Symbol) == kSymbolSize + 1,
              "a Symbol is the wire field plus its length, and nothing else");

/// Which shard owns a symbol. FNV-1a over the wire bytes.
///
/// The same function the benchmark's book fingerprint uses, deliberately:
/// one hash in the project rather than two, so a reader can check the
/// routing by eye.
[[nodiscard]] inline std::size_t shard_of(const Symbol& symbol, std::size_t shards) noexcept {
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
    for (std::size_t i = 0; i < symbol.length(); ++i) {
        hash ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(symbol.c_str()[i]));
        hash *= 0x0000'0100'0000'01B3ULL;
    }
    return static_cast<std::size_t>(hash % shards);
}

/// Order reference number to owning symbol index.
///
/// Open addressed, linear probing, no deletions.
///
/// ---- Why there is no erase -----------------------------------------
/// ITCH order reference numbers are unique for the trading DAY. A given
/// reference is written exactly once, by exactly one Add Order, and is
/// never reused until the next session. So there is nothing to delete:
/// the table only ever grows, and the day's work ends by throwing the
/// table away rather than by reclaiming entries.
///
/// That is not a simplification, it is the venue's own contract being
/// used. It removes the entire class of problems that deletion brings to
/// an open-addressed table -- tombstones filling the probe sequence,
/// backward-shift deletion getting the clustering subtly wrong, and the
/// failure mode where a table degrades to O(n) under long runs of
/// removals. None of that is reachable here, because there are no
/// removals.
///
/// The cost is memory that grows with orders SEEN rather than orders
/// LIVE. For a day-long feed that is the whole day. For this repository
/// it means sizing the table from the record count, which the callers
/// do, and treating a full table as an error rather than a hint.
///
/// NOT THREAD SAFE. One instance belongs to one thread, holding only
/// that thread's symbols' references. See the file header for why that
/// is sufficient.
class RefIndex final {
public:
    /// `capacity` is rounded up to a power of two and is the number of
    /// slots, not the number of entries. A full table holds
    /// `capacity / 2` entries before linear probing degrades, which is
    /// why `full()` fires at half rather than at the last slot.
    explicit RefIndex(std::size_t capacity) {
        std::size_t slots = 16;
        while (slots < capacity * 2 && slots < (static_cast<std::size_t>(1) << 40)) {
            slots <<= 1;
        }
        mask_ = slots - 1;
        table_.assign(slots, Slot{});
    }

    /// Record that `ref` belongs to `symbol_index`.
    ///
    /// Returns false if the table is full or the reference is already
    /// present with a different owner. Both are errors worth surfacing:
    /// a reference the handler cannot record is an order it will not be
    /// able to route, and the resulting mutation would be applied
    /// nowhere. That is silent data loss, so it is reported instead.
    bool insert(OrderId ref, std::size_t symbol_index) noexcept {
        if (symbol_index >= kMaxSymbolIndex) {
            return false;
        }
        if (2 * (size_ + 1) > table_.size()) {
            return false;  // at the load factor where probing degrades
        }
        const OrderId key = ref + 1;  // 0 is the empty marker
        std::size_t slot = hash(key) & mask_;
        for (std::size_t probe = 0; probe <= mask_; ++probe) {
            Slot& s = table_[slot];
            if (s.key == 0) {
                s.key = key;
                s.value = static_cast<std::uint32_t>(symbol_index);
                ++size_;
                return true;
            }
            if (s.key == key) {
                // Already present. Same owner is idempotent; a different
                // owner means two symbols claim one reference, which is
                // a generator or venue bug and must not be papered over.
                return s.value == symbol_index;
            }
            slot = (slot + 1) & mask_;
        }
        return false;
    }

    /// Find the symbol that owns `ref`.
    [[nodiscard]] bool lookup(OrderId ref, std::size_t& symbol_index) const noexcept {
        const OrderId key = ref + 1;
        std::size_t slot = hash(key) & mask_;
        for (std::size_t probe = 0; probe <= mask_; ++probe) {
            const Slot& s = table_[slot];
            if (s.key == 0) {
                return false;
            }
            if (s.key == key) {
                symbol_index = s.value;
                return true;
            }
            slot = (slot + 1) & mask_;
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /// True once another insert would be refused. Read this rather than
    /// assuming: a table that silently stopped recording references is a
    /// handler that silently stopped routing mutations.
    [[nodiscard]] bool full() const noexcept { return 2 * (size_ + 1) > table_.size(); }

    [[nodiscard]] std::size_t slot_count() const noexcept { return table_.size(); }

private:
    /// Value stored for `symbol_index`, biased by one so that 0 means
    /// "no entry" in the value array too. Keeping both arrays biased by
    /// one means a single zero test works for an empty slot.
    static constexpr std::uint32_t kMaxSymbolIndex = 0xFFFF'FFFEu;

    struct Slot {
        OrderId key = 0;  ///< ref + 1; 0 means empty
        std::uint32_t value = 0;
    };

    /// FNV-1a over the key. Cheap, well distributed for sequential
    /// reference numbers, and the same hash the rest of the project
    /// uses.
    [[nodiscard]] static std::size_t hash(OrderId key) noexcept {
        std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<std::uint64_t>((key >> (i * 8)) & 0xFFu);
            h *= 0x0000'0100'0000'01B3ULL;
        }
        return static_cast<std::size_t>(h);
    }

    std::vector<Slot> table_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

/// A set of books, partitioned by symbol, with the routing index that
/// makes partitioning possible.
///
/// NOT THREAD SAFE as a whole, and deliberately so: `ShardSet` owns all
/// the books, so a caller cannot accidentally touch one from the wrong
/// thread. Thread safety is obtained by constructing one `ShardSet` per
/// thread, each holding the subset of symbols that thread owns -- which
/// is exactly what the dispatcher decides, and why this class has no
/// locks and no atomics.
class ShardSet final {
public:
    /// `order_capacity` and `level_capacity` are PER BOOK, not total.
    /// Sized from the per-symbol share of the feed, not from the whole
    /// feed, or a hundred-symbol run allocates a hundred times too much.
    ///
    /// Memory is O(symbols x capacity) and that is inherent rather than
    /// an implementation detail: holding a hundred books means holding a
    /// hundred books. Sizing each one for the whole feed is the mistake
    /// to avoid, and it is why these are two numbers rather than one.
    ShardSet(std::size_t order_capacity, std::size_t level_capacity,
             std::size_t max_symbols = 1024)
        : order_capacity_(order_capacity), level_capacity_(level_capacity) {
        symbols_.reserve(max_symbols);
    }

    /// Claim a symbol for this set. Idempotent: claiming a symbol twice
    /// returns the same index and does not build a second book.
    ///
    /// Claims must be made from a single thread before any work starts.
    /// They are not synchronised, because doing them concurrently would
    /// mean a symbol could land on two indices, which is the one outcome
    /// nothing downstream can detect: both books would accept orders for
    /// one instrument and neither would ever be complete.
    std::size_t claim(const Symbol& symbol) noexcept {
        const std::size_t existing = find(symbol);
        if (existing != kNotFound) {
            return existing;
        }
        const std::size_t index = symbols_.size();
        symbols_.push_back(symbol);
        books_.emplace_back(order_capacity_, level_capacity_);
        return index;
    }

    [[nodiscard]] std::size_t find(const Symbol& symbol) const noexcept {
        for (std::size_t i = 0; i < symbols_.size(); ++i) {
            if (symbols_[i] == symbol) {
                return i;
            }
        }
        return kNotFound;
    }

    [[nodiscard]] bool owns(const Symbol& symbol) const noexcept {
        return find(symbol) != kNotFound;
    }

    [[nodiscard]] std::size_t symbol_count() const noexcept { return symbols_.size(); }

    /// Order pool size of EACH book, for reporting.
    [[nodiscard]] std::size_t order_capacity() const noexcept { return order_capacity_; }
    [[nodiscard]] std::size_t level_capacity() const noexcept { return level_capacity_; }

    /// Book for a symbol index previously returned by `claim`.
    /// Undefined if the index was never claimed.
    ///
    /// Deliberately an index and not a lookup by hash: the mapping is
    /// explicit and the caller chose it, which is what lets a thread own
    /// a contiguous run of symbols with no per-message hashing at all.
    [[nodiscard]] OrderBook& book(std::size_t symbol_index) noexcept {
        return books_[symbol_index];
    }
    [[nodiscard]] const OrderBook& book(std::size_t symbol_index) const noexcept {
        return books_[symbol_index];
    }

    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

private:
    std::size_t order_capacity_;
    std::size_t level_capacity_;
    std::vector<Symbol> symbols_;
    std::vector<OrderBook> books_;
};

}  // namespace hft::lob