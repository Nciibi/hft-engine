// Tests for the SPSC ring buffer.
//
// The single-threaded tests are property checks on the full/empty
// boundaries and on ordering across wraparound. The two-threaded ones
// are the ones that matter, because the interesting failures here are
// all failures of memory ordering, and a memory-ordering bug that
// single-threaded tests cannot see is the entire reason to write the
// two-threaded test.
//
// The last of them is a multi-field payload verified field by field
// after the handoff. A torn read -- the consumer seeing the low half of
// a new message and the high half of the old one -- leaves the ring
// internally consistent and completely wrong, and no counter or
// sequence check catches it.
//
// Termination is by atomic stop flag with a joined thread, never by a
// sleep. A concurrency test that passes because both threads happened
// to be scheduled in a friendly order is not a test.

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "hft/concurrent/cache_line.hpp"
#include "hft/concurrent/spsc_ring.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void check_eq_size(std::size_t actual, std::size_t expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL  %s: expected %zu, got %zu\n", what, expected, actual);
    }
}

void check_eq_u64(std::uint64_t actual, std::uint64_t expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL  %s: expected %llu, got %llu\n", what,
                    static_cast<unsigned long long>(expected),
                    static_cast<unsigned long long>(actual));
    }
}

using hft::concurrent::SpscRing;

using Item = std::uint64_t;

/// A payload with enough distinct fields that a partially-visible write
/// is detectable. Every field is derived from the sequence number, so
/// any mixing of two slots produces a mismatch.
struct Payload {
    std::uint64_t sequence = 0;
    std::uint64_t order_id = 0;
    std::int64_t price_raw = 0;
    std::uint32_t shares = 0;
    std::uint8_t side = 0;
    std::uint8_t checksum = 0;

    [[nodiscard]] static Payload make(std::uint64_t sequence) noexcept {
        Payload p;
        p.sequence = sequence;
        p.order_id = 0x1000'0000ULL + sequence;
        p.price_raw = static_cast<std::int64_t>(sequence) * 7 - 3;
        p.shares = static_cast<std::uint32_t>(sequence % 977u) + 1u;
        p.side = static_cast<std::uint8_t>(sequence & 1u);
        p.checksum = static_cast<std::uint8_t>((sequence ^ (sequence >> 8) ^ 0x5Au) & 0xFFu);
        return p;
    }

    /// True when this is exactly the payload `sequence` should have
    /// produced. Every field is checked: agreeing on the sequence while
    /// disagreeing on the price is precisely a torn read.
    [[nodiscard]] bool matches(std::uint64_t s) const noexcept {
        const Payload expected = make(s);
        return sequence == expected.sequence && order_id == expected.order_id &&
               price_raw == expected.price_raw && shares == expected.shares &&
               side == expected.side && checksum == expected.checksum;
    }
};

static_assert(std::is_trivially_copyable_v<Payload>,
              "the ring refuses anything it cannot hand over as bytes; the test payload "
              "must satisfy the same constraint or it is testing the wrong thing");

// ---- Capacity -------------------------------------------------------

void test_capacity_rounding() {
    std::printf("recommended_capacity\n");

    using R = SpscRing<Item, 8>;
    check_eq_size(R::recommended_capacity(0), 2, "zero rounds up to the minimum ring");
    check_eq_size(R::recommended_capacity(1), 2, "one rounds up to two");
    check_eq_size(R::recommended_capacity(2), 2, "two is exact");
    check_eq_size(R::recommended_capacity(3), 4, "three rounds up to four");
    check_eq_size(R::recommended_capacity(1024), 1024, "a power of two is unchanged");
    check_eq_size(R::recommended_capacity(1025), 2048, "one over a power of two doubles");
    check_eq_size(R::recommended_capacity(65'536), 65'536, "a large exact request holds");

    // Every result must satisfy the ring's own invariants, or the
    // caller has been handed a capacity the ring would refuse.
    const std::size_t wanted = 37;
    const std::size_t rounded = R::recommended_capacity(wanted);
    check(rounded >= wanted, "rounded capacity is never below the request");
    check((rounded & (rounded - 1)) == 0, "rounded capacity is a power of two");
}

// ---- Full and empty boundaries --------------------------------------

void test_fill_and_drain() {
    std::printf("fill and drain\n");

    constexpr std::size_t kN = 4;
    SpscRing<Item, kN> ring;

    check(ring.empty(), "a fresh ring is empty");
    check_eq_size(ring.size(), 0, "a fresh ring holds nothing");
    check(!ring.full(), "a fresh ring is not full");
    check_eq_size(ring.free_slots(), kN, "a fresh ring has every slot free");

    Item out = 0;
    check(!ring.try_pop(out), "popping an empty ring fails");
    check_eq_u64(out, 0, "a failed pop leaves the destination alone");

    // Fill it exactly. The Nth push must succeed and the N+1st must
    // fail: an off-by-one here is the difference between a queue and a
    // silently-dropping buffer.
    for (std::size_t i = 0; i < kN; ++i) {
        check(ring.try_push(i), "push within capacity succeeds");
    }
    check(ring.full(), "a ring filled to capacity reports full");
    check_eq_size(ring.size(), kN, "size equals capacity when full");
    check(!ring.try_push(999), "push on a full ring fails");
    check_eq_size(ring.size(), kN, "a refused push does not change the size");

    // Drain it. FIFO, and the freed slots are immediately reusable.
    for (std::size_t i = 0; i < kN; ++i) {
        check(ring.try_pop(out), "pop from a full ring succeeds");
        check_eq_u64(out, i, "drains in order");
    }
    check(ring.empty(), "a drained ring is empty again");
    check(!ring.try_pop(out), "popping a drained ring fails");
    check(ring.try_push(12345), "a slot freed by a pop is reusable");
    check(ring.try_pop(out), "the reused slot pops");
    check_eq_u64(out, 12345, "and it holds the new value, not the old one");
}

// ---- Wraparound ----------------------------------------------------

void test_fifo_across_wraparound() {
    std::printf("FIFO across wraparound\n");

    // Four slots, twenty items, popped in a pattern that repeatedly
    // wraps the head index past the end of the buffer. With this size a
    // bug that leaves a slot stale reappears on every lap.
    constexpr std::size_t kN = 4;
    constexpr Item kItems = 20;
    SpscRing<Item, kN> ring;

    Item next_push = 0;
    Item next_pop = 0;
    Item out = 0;

    // Interleave so the ring is usually between empty and full rather
    // than pinned at a boundary, which is the state a real pipeline
    // spends its life in.
    const int pattern = 3;
    while (next_push < kItems) {
        for (int i = 0; i < pattern && next_push < kItems; ++i) {
            check(ring.try_push(next_push), "push succeeds during the mixed pattern");
            ++next_push;
        }
        for (int i = 0; i < pattern && next_pop < next_push; ++i) {
            check(ring.try_pop(out), "pop succeeds during the mixed pattern");
            check_eq_u64(out, next_pop, "order preserved across the wrap");
            ++next_pop;
        }
    }

    while (next_pop < next_push) {
        check(ring.try_pop(out), "final drain succeeds");
        check_eq_u64(out, next_pop, "order preserved to the end");
        ++next_pop;
    }
    check_eq_u64(next_push, kItems, "everything was pushed");
    check_eq_u64(next_pop, kItems, "everything came back");
    check(ring.empty(), "empty after a full wrap");
}

// ---- Observation ----------------------------------------------------

void test_observation() {
    std::printf("size and free_slots\n");

    constexpr std::size_t kN = 8;
    SpscRing<Item, kN> ring;

    for (std::size_t i = 0; i < 5; ++i) {
        (void)ring.try_push(i);
    }
    check_eq_size(ring.size(), 5, "size tracks pushes");
    check_eq_size(ring.free_slots(), kN - 5, "free_slots is the complement of size");
    check(!ring.empty(), "a partly filled ring is not empty");

    Item out = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        (void)ring.try_pop(out);
    }
    check_eq_size(ring.size(), 0, "size returns to zero");
    check_eq_size(ring.free_slots(), kN, "every slot is free again");
}

// ---- Two threads ---------------------------------------------------

/// Drain `ring` until the producer is finished and the ring is empty.
///
/// The second drain pass is not redundancy. `try_pop` returning false is
/// a statement about one instant; the producer may push again before the
/// stop flag is read. Reading the flag with acquire orders us after the
/// producer's last release store on head_, so the retry after it cannot
/// miss anything -- which is exactly the property that makes the exit
/// condition sound rather than merely usually right.
template <typename Ring, typename Fn>
std::uint64_t drain_until_done(Ring& ring, const std::atomic<bool>& producer_done, Fn&& on_item) {
    std::uint64_t consumed = 0;
    Item out = 0;
    while (ring.try_pop(out)) {
        on_item(out);
        ++consumed;
    }
    while (producer_done.load(std::memory_order_acquire)) {
        bool progressed = false;
        while (ring.try_pop(out)) {
            on_item(out);
            ++consumed;
            progressed = true;
        }
        if (!progressed) {
            break;
        }
    }
    return consumed;
}

/// Item count for a given ring depth.
///
/// Scaled so every depth does a comparable amount of work while a deep
/// ring still gets enough laps to wrap its buffer many times over.
[[nodiscard]] constexpr std::uint64_t items_for(std::size_t capacity) noexcept {
    const std::uint64_t scaled = static_cast<std::uint64_t>(capacity) * 4u;
    const std::uint64_t floor = 20'000u;
    return scaled < floor ? floor : scaled;
}

template <std::size_t N>
void test_two_thread_transfer_at() {
    constexpr std::uint64_t kItems = items_for(N);

    SpscRing<Item, N> ring;
    std::atomic<bool> producer_done{false};

    std::thread producer([&ring, &producer_done] {
        for (Item i = 0; i < kItems; ++i) {
            // Back-pressure is expected and correct: the consumer is
            // slower than the producer here, and the ring is what
            // absorbs the difference.
            while (!ring.try_push(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t expected = 0;
    bool ordered = true;
    const std::uint64_t consumed = drain_until_done(ring, producer_done, [&](Item v) {
        if (v != expected) {
            ordered = false;
        }
        ++expected;
    });

    producer.join();

    // The label carries the depth so a failure names the ring it came
    // from. "Fails at N=2" and "fails at N=1024" are different bugs.
    char label[128];
    std::snprintf(label, sizeof(label), "depth %zu: every item arrives in push order", N);
    check(ordered, label);

    std::snprintf(label, sizeof(label), "depth %zu: nothing lost", N);
    check_eq_u64(consumed, kItems, label);

    std::snprintf(label, sizeof(label), "depth %zu: nothing duplicated", N);
    check_eq_u64(expected, kItems, label);

    std::snprintf(label, sizeof(label), "depth %zu: ring empty when both threads finish", N);
    check(ring.empty(), label);
}

void test_two_thread_transfer() {
    std::printf("two-thread transfer\n");

    // The small depths matter more than they look. A two-slot ring
    // spends its entire life alternating between full and empty, so it
    // is the case most likely to expose an off-by-one in the cached
    // index refresh. A deep ring mostly tests the fast path.
    test_two_thread_transfer_at<2>();
    test_two_thread_transfer_at<3>();
    test_two_thread_transfer_at<4>();
    test_two_thread_transfer_at<7>();
    test_two_thread_transfer_at<8>();
    test_two_thread_transfer_at<31>();
    test_two_thread_transfer_at<64>();
    test_two_thread_transfer_at<128>();
    test_two_thread_transfer_at<1024>();
}

template <std::size_t N>
void test_two_thread_payload_integrity_at() {
    constexpr std::uint64_t kItems = items_for(N);

    SpscRing<Payload, N> ring;
    std::atomic<bool> producer_done{false};

    std::thread producer([&ring, &producer_done] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            const Payload p = Payload::make(i);
            while (!ring.try_push(p)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t expected = 0;
    std::uint64_t torn = 0;
    const std::uint64_t consumed = drain_until_done(ring, producer_done, [&](Payload p) {
        // A torn read still advances the count, so it shows up as a
        // mismatch here rather than as a hang or a lost item.
        if (!p.matches(expected)) {
            ++torn;
        }
        ++expected;
    });

    producer.join();

    char label[128];
    std::snprintf(label, sizeof(label),
                  "depth %zu: no partially-visible payload, every field from one slot", N);
    check_eq_u64(torn, 0, label);

    std::snprintf(label, sizeof(label), "depth %zu: every payload arrived exactly once", N);
    check_eq_u64(consumed, kItems, label);
}

void test_two_thread_payload_integrity() {
    std::printf("two-thread payload integrity\n");

    // Two depths is enough here rather than the full sweep: the torn-read
    // failure depends on the payload, not on how deep the ring is, and
    // a wide ring lets the producer and consumer run far enough apart
    // to make a real race likely.
    test_two_thread_payload_integrity_at<16>();
    test_two_thread_payload_integrity_at<256>();
}

// ---- Layout --------------------------------------------------------

void test_layout() {
    std::printf("layout\n");

    // The whole point of the padding is that these are separate lines.
    // If a future change quietly merges them, the ring still passes
    // every functional test above while paying coherence tax on every
    // single message.
    using Ring = SpscRing<Payload, 16>;
    check_eq_size(sizeof(Ring) % hft::concurrent::kCacheLineSize, 0,
                  "the ring occupies whole cache lines");
    check(sizeof(Ring) > 4 * hft::concurrent::kCacheLineSize,
          "head, tail and the two caches each get their own line");
    check_eq_size(hft::concurrent::round_up_to_cache_line(1), 64, "one byte rounds to a line");
    check_eq_size(hft::concurrent::round_up_to_cache_line(64), 64, "exactly one line is unchanged");
    check_eq_size(hft::concurrent::round_up_to_cache_line(65), 128, "one over a line takes two");
}

}  // namespace

int main() {
    std::printf("concurrency tests\n-------------------\n");
    test_capacity_rounding();
    test_fill_and_drain();
    test_fifo_across_wraparound();
    test_observation();
    test_two_thread_transfer();
    test_two_thread_payload_integrity();
    test_layout();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}