// SPSC ring benchmark, against a mutex + condition_variable baseline.
//
// What is being compared
// ---------------------
// Two hand-off mechanisms with the same capacity semantics and the same
// payload, run on the same two cores in the same process:
//
//   1. The lock-free ring (`hft/concurrent/spsc_ring.hpp`).
//   2. A blocking queue guarded by a mutex and two condition variables.
//
// The honest expectation, stated before the numbers: the ring wins, and
// it wins for a reason that has nothing to do with fences. On x86-64 the
// acquire/release pairs in the ring compile to plain MOVs with no fence
// instruction at all -- the compiler is permitted to emit nothing more
// than a load and a store, and it does. The entire advantage is that
// neither thread ever makes a syscall: the mutex baseline has to
// futex-wake the peer when it blocks, and a syscall costs microseconds
// against a ring hand-off costing tens of nanoseconds.
//
// This is why the two measurement styles below are kept separate rather
// than merged into one table:
//
//   * THROUGHPUT uses the wall clock and takes no per-operation reading
//     at all. Two clock reads per operation would be roughly as
//     expensive as the operation being measured.
//   * LATENCY takes a clock pair per operation and therefore has the
//     measurement cost baked into it. The clock overhead is printed
//     above so the reader can see exactly how much of the p50 is the
//     instrument.
//
// Merging them would produce one tidy table of numbers that are wrong in
// opposite directions, which is worse than two honest tables.
//
// Usage:
//   hft_ring_bench [messages] [ring_capacity]

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "bench/report.hpp"
#include "hft/concurrent/spsc_ring.hpp"
#include "hft/itch/decode.hpp"
#include "hft/util/affinity.hpp"

namespace {

using hft::concurrent::SpscRing;

/// The payload is a decoded ITCH message rather than an integer.
///
/// A ring carrying a `uint64_t` moves eight bytes per slot and a ring
/// carrying a `Message` moves `sizeof(Message)`. Those are different
/// costs, and quoting the first while describing the second is how a
/// benchmark ends up not measuring the thing it claims to. The decoder's
/// own output type is used so the figure is the one a real market-data
/// handler would see. Its size is printed rather than assumed.
using Payload = hft::itch::Message;

/// Blocking queue with the same capacity semantics as the ring.
///
/// A real baseline, not a strawman: bounded, FIFO, blocking on both full
/// and empty. Both condition variables are used because a baseline that
/// only blocks one way measures a different queue.
template <std::size_t N>
class MutexQueue final {
public:
    void push(const Payload& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return count_ < N; });
        slots_[tail_] = value;
        tail_ = (tail_ + 1) % N;
        ++count_;
        lock.unlock();
        not_empty_.notify_one();
    }

    void pop(Payload& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return count_ > 0; });
        out = slots_[head_];
        head_ = (head_ + 1) % N;
        --count_;
        lock.unlock();
        not_full_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    Payload slots_[N]{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
};

/// Pre-built payload sequence, so the producer is not paying to build
/// messages inside the timed region and neither run measures allocation.
[[nodiscard]] std::vector<Payload> make_payloads(std::size_t count) {
    std::vector<Payload> payloads(count);
    hft::itch::AddOrder add;
    add.stock_locate = 1;
    add.side = hft::Side::bid;
    add.price = hft::Price::from_int(100);
    add.size = hft::Quantity::from_raw(100);
    add.order_type = '2';
    add.time_in_force = '0';
    add.display = '1';
    for (std::size_t i = 0; i < count; ++i) {
        add.id = static_cast<hft::OrderId>(1'000'000 + i);
        add.tracking = static_cast<hft::TrackingNumber>(i & 0xFFFFu);
        payloads[i].body = add;
    }
    return payloads;
}

struct ThroughputResult {
    std::uint64_t messages = 0;
    double elapsed_ns = 0.0;
    std::uint64_t spins = 0;
    bool consistent = false;
};

// ---- Throughput: wall clock, no per-operation clock reads ------------

template <std::size_t N>
[[nodiscard]] ThroughputResult ring_throughput(const std::vector<Payload>& payloads,
                                               std::size_t producer_core,
                                               std::size_t consumer_core) {
    SpscRing<Payload, N> ring;
    ThroughputResult result;
    std::uint64_t consumed = 0;
    std::uint64_t spins = 0;

    std::thread consumer([&] {
        (void)hft::util::pin_current_thread(consumer_core);
        Payload out;
        while (consumed < payloads.size()) {
            if (ring.try_pop(out)) {
                ++consumed;
            } else {
                // The consumer is faster than the producer here, so this
                // is where its idle time goes. Yielding rather than
                // sleeping: a timed sleep would floor the measured
                // throughput at the sleep interval.
                ++spins;
                std::this_thread::yield();
            }
        }
    });

    bench::Timer timer;
    std::thread producer([&] {
        (void)hft::util::pin_current_thread(producer_core);
        for (const Payload& p : payloads) {
            while (!ring.try_push(p)) {
                ++spins;
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    result.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    result.messages = consumed;
    result.spins = spins;
    result.consistent = consumed == payloads.size();
    return result;
}

template <std::size_t N>
[[nodiscard]] ThroughputResult mutex_throughput(const std::vector<Payload>& payloads,
                                                std::size_t producer_core,
                                                std::size_t consumer_core) {
    MutexQueue<N> queue;
    ThroughputResult result;

    std::thread consumer([&] {
        (void)hft::util::pin_current_thread(consumer_core);
        Payload out;
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            queue.pop(out);
        }
    });

    bench::Timer timer;
    std::thread producer([&] {
        (void)hft::util::pin_current_thread(producer_core);
        for (const Payload& p : payloads) {
            queue.push(p);
        }
    });

    producer.join();
    consumer.join();
    result.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    result.messages = payloads.size();
    result.consistent = true;
    return result;
}

// ---- Latency: one-way hand-off, clock pair per operation ------------

struct LatencyResult {
    bench::LatencyHistogram one_way;
    std::uint64_t round_trips = 0;
    bool echoed_correctly = true;
};

/// Round trip: push a token, wait for it to come back, halve the elapsed
/// time.
///
/// Half is a deliberate assumption, not a measurement of one direction.
/// It is the standard way to estimate one-way latency from a two-party
/// exchange, and it is an UPPER BOUND here: the round trip contains
/// both cache-line transfers plus the wakeup path, and each side's own
/// work is in there too.
template <std::size_t N, bool kUseMutex>
[[nodiscard]] LatencyResult ping_pong(std::size_t iterations, std::size_t producer_core,
                                       std::size_t consumer_core) {
    bench::LatencyHistogram hist(1, 4'000'000);
    SpscRing<Payload, N> ring;
    MutexQueue<N> mutex_queue;

    // The token carries the iteration number so the echo can be checked
    // for identity. A hand-off benchmark that only counts messages
    // cannot tell a correct echo from last round's value.
    Payload token;
    hft::itch::AddOrder& token_add = std::get<hft::itch::AddOrder>(token.body);

    std::atomic<bool> echoed_correctly{true};

    // The consumer runs exactly as many iterations as the producer, so
    // no stop flag is needed and none of the shutdown races that a stop
    // flag introduces have to be reasoned about. Each token is echoed
    // once and consumed once.
    std::thread consumer([&] {
        (void)hft::util::pin_current_thread(consumer_core);
        for (std::size_t i = 0; i < iterations; ++i) {
            Payload out;
            if constexpr (kUseMutex) {
                mutex_queue.pop(out);
                mutex_queue.push(out);
            } else {
                while (!ring.try_pop(out)) {
                    std::this_thread::yield();
                }
                while (!ring.try_push(out)) {
                    std::this_thread::yield();
                }
            }
        }
    });

    (void)hft::util::pin_current_thread(producer_core);

    for (std::size_t i = 0; i < iterations; ++i) {
        token_add.id = static_cast<hft::OrderId>(i + 1);
        const std::uint64_t start = hft::util::Timer::now();

        Payload back;
        if constexpr (kUseMutex) {
            mutex_queue.push(token);
            mutex_queue.pop(back);
        } else {
            while (!ring.try_push(token)) {
                std::this_thread::yield();
            }
            while (!ring.try_pop(back)) {
                std::this_thread::yield();
            }
        }
        const std::uint64_t elapsed = hft::util::Timer::now() - start;
        hist.record(elapsed / 2u);

        if (std::get<hft::itch::AddOrder>(back.body).id != token_add.id) {
            echoed_correctly.store(false, std::memory_order_relaxed);
        }
    }

    consumer.join();

    LatencyResult result;
    result.one_way = hist;
    result.round_trips = iterations;
    result.echoed_correctly = echoed_correctly.load(std::memory_order_relaxed);
    return result;
}

void report_throughput(const char* label, const ThroughputResult& r) {
    const double rate =
        r.elapsed_ns > 0.0 ? static_cast<double>(r.messages) * 1e9 / r.elapsed_ns : 0.0;
    std::printf("  %-18s %12s msg/s  %9.3f ms  idle-yields %10llu  %s\n", label,
                bench::humanize(static_cast<std::uint64_t>(rate)).c_str(), r.elapsed_ns / 1e6,
                bench::u64(r.spins),
                r.consistent ? "all messages transferred" : "MESSAGE LOSS -- RESULT INVALID");
    std::fflush(stdout);
}

/// Ring and mutex results for one instantiated capacity, run back to
/// back in the same process on the same cores.
template <std::size_t N>
void compare_at(const std::vector<Payload>& payloads,
                std::size_t producer_core, std::size_t consumer_core) {
    std::printf("\n  capacity %llu slots\n", bench::u64(N));

    // Warm the pages and the buffer before either measured run. A throwaway
    // run is the honest way to do it: reusing the measured buffers would
    // mean the first run pays for the fault-in and the second does not.
    (void)ring_throughput<N>(payloads, producer_core, consumer_core);

    const ThroughputResult ring = ring_throughput<N>(payloads, producer_core, consumer_core);
    report_throughput("lock-free ring", ring);

    const ThroughputResult mutex_run =
        mutex_throughput<N>(payloads, producer_core, consumer_core);
    report_throughput("mutex + condvar", mutex_run);

    if (ring.elapsed_ns > 0.0 && mutex_run.elapsed_ns > 0.0) {
        std::printf("  %-18s speedup %.2fx\n", "",
                    mutex_run.elapsed_ns / ring.elapsed_ns);
    }

    if (ring.spins * 4 > ring.messages) {
        std::printf("  %-18s note: yields dominated. The run measured scheduling,\n"
                    "  %-18s       not the hand-off.\n",
                    "", "");
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Line-buffered. This tool is normally run from a shell and its output
    // is often piped to a file, and block buffering means a run that hangs
    // or is interrupted loses every line printed before the problem. A
    // benchmark you cannot see into is a benchmark you cannot debug.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    std::size_t messages = 2'000'000;
    std::size_t capacity = 1024;
    if (argc > 1) {
        messages = std::strtoull(argv[1], nullptr, 10);
    }
    if (argc > 2) {
        capacity = std::strtoull(argv[2], nullptr, 10);
    }
    if (messages == 0) {
        messages = 1;
    }

    bench::print_environment("SPSC ring benchmark");
    std::printf("messages             %s\n", bench::humanize(messages).c_str());
    std::printf("payload              itch::Message, %zu bytes\n", sizeof(Payload));

    // A payload wider than a cache line costs more than one coherence
    // transfer per hand-off, which changes what the numbers below mean.
    // Said here because a wider decoder payload is a foreseeable change,
    // not because this one is.
    if (sizeof(Payload) > hft::concurrent::kCacheLineSize) {
        std::printf("WARNING: payload is %zu bytes, wider than a %zu-byte cache line.\n"
                    "         Each hand-off now moves more than one line and the\n"
                    "         figures below include that cost.\n",
                    sizeof(Payload), hft::concurrent::kCacheLineSize);
    }
    std::printf("\n");

    bench::section("MEASUREMENT COST");
    const bench::LatencyHistogram clock = bench::measure_clock_overhead();
    bench::print_clock_overhead(clock);

    // Two physical cores, never two SMT siblings. Asking the OS beats
    // assuming an enumeration order: this host lays its siblings out as
    // (0,1), (2,3), (4,5)... and a `core + 1` guess would put both
    // threads on one core and halve the result while looking entirely
    // plausible.
    const std::size_t producer_core = 0;
    const std::size_t consumer_core = hft::util::other_core(producer_core);

    bench::section("THROUGHPUT (wall clock, no per-operation clock reads)");
    std::printf("  producer            %s\n",
                hft::util::describe_affinity("producer").c_str());
    std::printf("  consumer            logical %zu\n", consumer_core);
    std::printf("  distinct physical cores: %s\n\n",
                hft::util::shares_physical_core(producer_core, consumer_core) ? "NO" : "yes");

    if (hft::util::shares_physical_core(producer_core, consumer_core)) {
        std::printf("WARNING: both threads resolved to the same physical core. Every\n"
                    "         figure below measures one core, not two. Pinning failed,\n"
                    "         or this host exposes fewer than two cores.\n\n");
    }

    const std::vector<Payload> payloads = make_payloads(messages);

    // Only three capacities are instantiated: a sweep would triple the
    // runtime of a tool whose point is a comparison, and capacity is not
    // the variable under test here.
    const std::size_t snap = SpscRing<Payload, 2>::recommended_capacity(capacity);
    if (snap >= 1024) {
        compare_at<1024>(payloads, producer_core, consumer_core);
    } else if (snap >= 256) {
        compare_at<256>(payloads, producer_core, consumer_core);
    } else {
        compare_at<64>(payloads, producer_core, consumer_core);
    }

    bench::section("ONE-WAY HAND-OFF (half a round trip, includes a clock pair)");
    // Far fewer iterations than the throughput pass: each one is a
    // synchronising round trip rather than a queue operation, so this
    // costs far more per message.
    const std::size_t rounds = messages / 20 + 1'000;
    std::printf("  round trips        %s\n\n", bench::humanize(rounds).c_str());
    std::printf("  ring capacity      2 slots, the minimum a ring can have\n\n");

    // Warm both variants before measuring either.
    (void)ping_pong<2, false>(rounds / 10 + 100, producer_core, consumer_core);
    (void)ping_pong<2, true>(rounds / 10 + 100, producer_core, consumer_core);

    const LatencyResult ring_lat = ping_pong<2, false>(rounds, producer_core, consumer_core);
    std::printf("  lock-free ring\n");
    bench::histogram_row("one-way", ring_lat.one_way);

    const LatencyResult mutex_lat = ping_pong<2, true>(rounds, producer_core, consumer_core);
    std::printf("  mutex + condvar\n");
    bench::histogram_row("one-way", mutex_lat.one_way);

    if (!ring_lat.echoed_correctly || !mutex_lat.echoed_correctly) {
        std::printf("\nWARNING: an echoed token did not match the one sent.\n"
                    "         The hand-off moved the wrong bytes and the figures\n"
                    "         above are meaningless.\n");
    }

    bench::note(
        "Half a round trip, not a measured one-way latency: it contains both\n"
        "cache-line transfers plus the wakeup path, so it overstates a single\n"
        "direction. It is the standard estimate and it is an upper bound.\n"
        "\n"
        "The mutex baseline blocks, so each hand-off can cost a futex wakeup and\n"
        "a syscall. That is the whole of the difference on x86-64: the ring's\n"
        "acquire/release pairs compile to plain loads and stores with no fence\n"
        "instruction, because there is exactly one producer and one consumer\n"
        "and nothing outside the ring needs ordering.\n");

    bench::print_publication_notice();
    return 0;
}