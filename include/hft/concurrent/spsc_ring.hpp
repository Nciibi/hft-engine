// Single-producer / single-consumer lock-free ring buffer.
//
// This is the component that decodes market data on one thread and
// applies it to a book on another. It is written for that case and
// nothing else: exactly one producer and one consumer, by construction,
// not by a runtime check. An SPSC queue that must detect a second
// producer pays for a check it never needs, and a queue that is actually
// MPSC needs a different algorithm entirely (per-slot sequence numbers,
// or a compare-and-swap on the tail).
//
// ---- Why the indices are monotonic -------------------------------
//
// The obvious implementation keeps `head` and `tail` as indices into the
// buffer, incremented modulo the capacity. Then `head == tail` means the
// ring is BOTH empty and full, and the class has to burn a slot -- or
// consult a flag, or count -- to tell them apart.
//
// Here the counters only ever go up. `size` is `head - tail`, `full` is
// `head - tail == capacity`, and `empty` is `head == tail`. Every case
// is unambiguous with no reserved slot and no extra state. The only
// requirement is that the difference never reaches 2^63, which for a
// 1024-slot ring at 10^9 pushes a second is about 29,000 years of
// runtime.
//
// ---- Why acquire/release and nothing stronger ---------------------
//
// The producer writes a slot, then publishes the new head. The consumer
// reads the head, then reads the slot. For that to work, every write to
// a slot must be visible to whoever reads the index that makes the slot
// reachable. A release store and an acquire load are exactly that pair
// and nothing more: the store cannot be reordered after the index
// update, and the load cannot be reordered before it.
//
// No seq_cst fence is needed and none is used. A fence here would order
// these two atomics against every OTHER atomic in the program, which
// this queue has no business doing -- it is not synchronising anything
// outside itself. And on x86-64 the acquire/release pairs compile to
// plain MOVs with no fence instruction at all, which is the entire
// reason to use them here rather than seq_cst.
//
// ---- False sharing -----------------------------------------------
//
// The producer writes `head` and reads `tail`; the consumer does the
// reverse. If those two shared a cache line, every push would invalidate
// the line the consumer is reading and vice versa -- pure coherence
// traffic with no useful work done. Each index therefore gets its own
// cache line, and so does each producer-private cache, because those are
// written on opposite threads and a slow-path refresh that lands on a
// shared line costs exactly as much as a fast-path write would.
//
// 4 lines of indices for a 1024-slot ring of 56-byte messages is about
// 0.4% overhead. The alternative is paying coherence tax on every
// message, which is 50-100ns. There is no version of this trade where
// the padding loses.

#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "hft/concurrent/cache_line.hpp"

namespace hft::concurrent {

/// Bounded SPSC ring holding `N` slots of `T`.
///
/// `T` must be trivially copyable and trivially destructible. The queue
/// hands a slot's bytes from one thread to another and never runs a
/// constructor or destructor on them, which is what makes the handoff a
/// couple of stores. A `T` with a non-trivial destructor would leak, and
/// one with a non-trivial copy constructor would do more work inside the
/// ring than the ring exists to avoid; both are refused at compile time
/// rather than discovered as a leak under load.
template <typename T, std::size_t N>
class SpscRing final {
    static_assert(std::has_single_bit(N),
                  "SpscRing capacity must be a power of two: the index mask is N-1");
    static_assert(N >= 2, "SpscRing needs room for at least two slots to be a ring");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing hands slots between threads by copying bytes; T must be trivially "
                  "copyable or the handoff does more work than the queue");
    static_assert(std::is_trivially_destructible_v<T>,
                  "SpscRing never runs destructors on slots; a T with a non-trivial destructor "
                  "would leak when the queue is destroyed");
    static_assert(alignof(T) <= kCacheLineSize,
                  "over-aligned payload: storage alignment would exceed the cache line the "
                  "queue is built around");

public:
    using value_type = T;

    /// Slots held at once. Also the number of messages that may be in
    /// flight before the producer sees a full ring.
    static constexpr std::size_t kCapacity = N;

    /// Mask for wrapping a monotonic counter into a slot index.
    static constexpr std::size_t kMask = N - 1;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

    /// Smallest power of two that holds `minimum` slots.
    ///
    /// Exists so callers do not write their own rounding loop and get
    /// it subtly wrong at a size that is never tested.
    [[nodiscard]] static constexpr std::size_t recommended_capacity(std::size_t minimum) noexcept {
        std::size_t n = 2;
        // Bounded so an absurd request terminates instead of shifting
        // `n` left into zero and looping forever.
        while (n < minimum && n <= (static_cast<std::size_t>(-1) >> 1)) {
            n <<= 1;
        }
        return n;
    }

    // ---- Producer side ---------------------------------------------

    /// Append `value`. Returns false if the ring is full.
    ///
    /// False is the normal back-pressure signal, not an error: a
    /// consumer that cannot keep up is supposed to slow the producer
    /// down. What is NOT correct is to overwrite or drop, because both
    /// silently corrupt a book.
    ///
    /// Only the producer thread may call this.
    bool try_push(const T& value) noexcept {
        // Relaxed is sufficient and not merely convenient: this thread
        // is the only writer of head_, so no other thread's store can
        // be reordered against this load. There is nothing to
        // synchronise with yet.
        const std::size_t head = head_->load(std::memory_order_relaxed);

        if (head - *cached_tail_ >= N) {
            // Refresh against the consumer. Acquire, because it pairs
            // with the consumer's release store on tail_: the producer
            // must not overwrite a slot until the consumer's reads of
            // that slot have completed, and this is the edge that
            // guarantees it.
            *cached_tail_ = tail_->load(std::memory_order_acquire);
            if (head - *cached_tail_ >= N) {
                return false;
            }
        }

        storage_[head & kMask] = value;
        // Release: the slot write above must be visible to any thread
        // that sees this index.
        head_->store(head + 1, std::memory_order_release);
        return true;
    }

    /// Remove the oldest value into `out`. Returns false if empty.
    ///
    /// Only the consumer thread may call this.
    bool try_pop(T& out) noexcept {
        // Relaxed for the same reason as in try_push: this thread is
        // the only writer of tail_.
        const std::size_t tail = tail_->load(std::memory_order_relaxed);

        if (tail == *cached_head_) {
            // Acquire, pairing with the producer's release store on
            // head_. This is what makes the bytes written into the slot
            // visible before they are read below.
            *cached_head_ = head_->load(std::memory_order_acquire);
            if (tail == *cached_head_) {
                return false;
            }
        }

        out = storage_[tail & kMask];
        // Release: frees the slot. The producer's next acquire on tail_
        // cannot observe the increment until every read of this slot
        // has happened.
        tail_->store(tail + 1, std::memory_order_release);
        return true;
    }

    // ---- Observation -----------------------------------------------
    //
    // Safe to call from either thread. A snapshot only: two loads are
    // not one atomic read, so a concurrent push can land between them
    // and the answer describes no single instant. Nothing here is used
    // to make a correctness decision.

    /// Messages currently in the ring. Exact when quiescent.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t h = head_->load(std::memory_order_acquire);
        const std::size_t t = tail_->load(std::memory_order_acquire);
        return h - t;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool full() const noexcept { return size() >= N; }

    /// Slots with room right now. Snapshot; see the note above.
    [[nodiscard]] std::size_t free_slots() const noexcept { return N - size(); }

private:
    // Written by the producer, read by the consumer.
    Padded<std::atomic<std::size_t>> head_;
    // Written by the consumer, read by the producer.
    Padded<std::atomic<std::size_t>> tail_;

    // Producer-private and consumer-private respectively. Each is
    // padded for the same reason as the indices: they are written on
    // opposite threads, and the refresh happens on the full/empty path,
    // which in a saturated pipeline is every operation.
    Padded<std::size_t> cached_tail_;
    Padded<std::size_t> cached_head_;

    // A real T array rather than raw bytes with a launder cast. The
    // trade is a single construction pass when the queue is created --
    // once, outside any measured region -- against a cast that -Wcast-align
    // has an opinion about and that a reader would have to verify. The
    // construction pass is not a per-push cost, and per-push cost is the
    // only thing this class exists to minimise.
    alignas(T) T storage_[N];
};

/// Every index and cache line is whole, so no field straddles a line.
/// Without this the buffer could begin mid-line and the padding above
/// would buy nothing.
static_assert(sizeof(SpscRing<std::uint64_t, 2>) % kCacheLineSize == 0,
              "SpscRing must occupy whole cache lines");

}  // namespace hft::concurrent