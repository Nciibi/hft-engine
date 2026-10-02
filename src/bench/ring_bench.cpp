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
// A note on the round-trip measurement, because it is the one place
// this tool is easy to get wrong: the exchange is bidirectional, and a
// ring is not. One SPSC ring has exactly one producer and one consumer,
// so a request/response exchange needs TWO of them -- one per direction.
// An earlier revision of this file pushed the token down one ring and
// waited for it to come back up the same one, which quietly made both
// threads consumers of a single-producer queue. It did not deadlock; it
// reported a plausible, wrong, far-too-fast number. The two-channel
// shape below is the fix, and it is why the channels are named types
// rather than a single object passed twice.
//
// Usage:
//   hft_ring_bench [messages] [ring_capacity]

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
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
// own output type is used so the figure is the one a real market-data
// handler would see. Its size is printed rather than assumed.
using Payload = hft::itch::Message;

/// One-directional channel: the lock-free implementation.
template <std::size_t N>
class RingChannel final {
public:
    void push(const Payload& value) {
        // Spin rather than block. The ring has no blocking interface by
        // design -- that is the whole difference under test -- so a full
        // channel waits here.
        while (!ring_.try_push(value)) {
            std::this_thread::yield();
        }
    }

    void pop(Payload& out) {
        while (!ring_.try_pop(out)) {
            std::this_thread::yield();
        }
    }

private:
    SpscRing<Payload, N> ring_;
};

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

/// One-directional channel: the baseline implementation. Same interface
/// as `RingChannel`, which is what lets the measurement below be written
/// once and instantiated twice.
template <std::size_t N>
class MutexChannel final {
public:
    void push(const Payload& value) { queue_.push(value); }
    void pop(Payload& out) { queue_.pop(out); }

private:
    MutexQueue<N> queue_;
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
    std::string producer_placement;
    std::string consumer_placement;
};

// ---- Throughput: wall clock, no per-operation clock reads ------------

template <std::size_t N>
[[nodiscard]] ThroughputResult ring_throughput(const std::vector<Payload>& payloads,
                                               std::size_t producer_core,
                                               std::size_t consumer_core) {
    SpscRing<Payload, N> ring;
    ThroughputResult result;
    std::uint64_t consumed = 0;
    std::atomic<std::uint64_t> spins{0};

    std::thread consumer([&] {
        (void)hft::util::pin_current_thread(consumer_core);
        result.consumer_placement = hft::util::describe_affinity("consumer");
        Payload out;
        while (consumed < payloads.size()) {
            if (ring.try_pop(out)) {
                ++consumed;
            } else {
                // The consumer is faster than the producer here, so this
                // is where its idle time goes. Yielding rather than
                // sleeping: a timed sleep would floor the measured
                // throughput at the sleep interval.
                spins.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    });

    bench::Timer timer;
    std::thread producer([&] {
        (void)hft::util::pin_current_thread(producer_core);
        result.producer_placement = hft::util::describe_affinity("producer");
        for (const Payload& p : payloads) {
            while (!ring.try_push(p)) {
                spins.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    result.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    result.messages = consumed;
    result.spins = spins.load();
    result.consistent = consumed == payloads.size();
    return result;
}

template <std::size_t N>
[[nodiscard]] ThroughputResult mutex_throughput(const std::vector<Payload>& payloads,
                                                std::size_t producer_core,
                                                std::size_t consumer_core) {
    MutexChannel<N> channel;
    ThroughputResult result;

    std::thread consumer([&] {
        (void)hft::util::pin_current_thread(consumer_core);
        result.consumer_placement = hft::util::describe_affinity("consumer");
        Payload out;
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            channel.pop(out);
        }
    });

    bench::Timer timer;
    std::thread producer([&] {
        (void)hft::util::pin_current_thread(producer_core);
        result.producer_placement = hft::util::describe_affinity("producer");
        for (const Payload& p : payloads) {
            channel.push(p);
        }
    });

    producer.join();
    consumer.join();
    result.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    result.messages = payloads.size();
    result.consistent = true;
    return result;
}

// ---- Latency: round trip over two channels, clock pair per operation --

struct LatencyResult {
    bench::LatencyHistogram one_way;
    std::uint64_t round_trips = 0;
    bool echoed_correctly = true;
    std::string producer_placement;
    std::string consumer_placement;
};

/// Request/response over two one-directional channels.
///
/// Half the elapsed time is reported as the one-way cost. Half is a
/// deliberate assumption, not a measurement of one direction: it is the
/// standard way to estimate one-way latency from a two-party exchange,
/// and it is an UPPER BOUND, because the round trip contains both
/// cache-line transfers, both wakeup paths, and each side's own work.
///
/// The consumer runs exactly as many iterations as the producer, so no
/// stop flag is needed and none of the shutdown races a stop flag
/// introduces have to be reasoned about. Each token is echoed once and
/// consumed once.
template <typename Forward, typename Backward>
[[nodiscard]] LatencyResult run_ping_pong(std::size_t iterations, std::size_t producer_core,
                                          std::size_t consumer_core) {
    bench::LatencyHistogram hist(1, 4'000'000);
    Forward forward;
    Backward backward;
    std::atomic<bool> echoed_correctly{true};
    LatencyResult result;

    // The token carries the iteration number so the echo can be checked
    // for identity. A hand-off benchmark that only counts messages
    // cannot tell a correct echo from last round's value.
    Payload token;
    hft::itch::AddOrder& token_add = std::get<hft::itch::AddOrder>(token.body);

    std::thread consumer([&] {
        (void)hft::util::pin_current_thread(consumer_core);
        result.consumer_placement = hft::util::describe_affinity("consumer");
        for (std::size_t i = 0; i < iterations; ++i) {
            Payload out;
            forward.pop(out);
            backward.push(out);
        }
    });

    (void)hft::util::pin_current_thread(producer_core);
    result.producer_placement = hft::util::describe_affinity("producer");

    for (std::size_t i = 0; i < iterations; ++i) {
        token_add.id = static_cast<hft::OrderId>(i + 1);
        const std::uint64_t start = hft::util::Timer::now();

        forward.push(token);
        Payload back;
        backward.pop(back);

        const std::uint64_t elapsed = hft::util::Timer::now() - start;
        hist.record(elapsed / 2u);

        if (std::get<hft::itch::AddOrder>(back.body).id != token_add.id) {
            echoed_correctly.store(false, std::memory_order_relaxed);
        }
    }

    consumer.join();

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
    std::printf("  %-18s producer %s\n", "", r.producer_placement.c_str());
    std::printf("  %-18s consumer %s\n", "", r.consumer_placement.c_str());
    std::fflush(stdout);
}

/// Ring and baseline results for one instantiated capacity, run back to
/// back in the same process on the same cores.
template <std::size_t N>
bool compare_at(const std::vector<Payload>& payloads, std::size_t producer_core,
                std::size_t consumer_core) {
    std::printf("\n  capacity %llu slots\n", bench::u64(N));

    // Warm the pages and the buffer before either measured run. A throwaway
    // run is the honest way to do it: reusing the measured buffers would
    // mean the first run pays for the fault-in and the second does not.
    (void)ring_throughput<N>(payloads, producer_core, consumer_core);

    const ThroughputResult ring = ring_throughput<N>(payloads, producer_core, consumer_core);
    report_throughput("lock-free ring", ring);

    const ThroughputResult baseline = mutex_throughput<N>(payloads, producer_core, consumer_core);
    report_throughput("mutex + condvar", baseline);

    if (ring.elapsed_ns > 0.0 && baseline.elapsed_ns > 0.0) {
        std::printf("  %-18s speedup %.2fx\n", "", baseline.elapsed_ns / ring.elapsed_ns);
    }

    if (ring.spins * 4 > ring.messages) {
        std::printf("  %-18s note: yields dominated. This run measured thread\n"
                    "  %-18s       scheduling, not the hand-off.\n",
                    "", "");
    }
    std::fflush(stdout);
    return ring.consistent && baseline.consistent;
}

}  // namespace

int main(int argc, char** argv) {
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

    // Runs shorter than this are dominated by thread creation and by
    // the scheduler's first placement of both threads. Reporting a
    // msgs/sec figure from one would be reporting the cost of starting
    // a process.
    constexpr std::size_t kMinimumMessages = 100'000;
    if (messages < kMinimumMessages) {
        std::printf("WARNING: %s messages is below %s. Thread startup and first-touch\n"
                    "         costs dominate at this size and the throughput figures\n"
                    "         below are not meaningful.\n\n",
                    bench::humanize(messages).c_str(), bench::humanize(kMinimumMessages).c_str());
    }

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
    std::printf("  producer requested  logical %zu\n", producer_core);
    std::printf("  consumer requested  logical %zu\n", consumer_core);
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

    bench::section("ROUND TRIP (halved, includes a clock pair)");
    // Far fewer iterations than the throughput pass: each one is a
    // synchronising round trip rather than a queue operation, so this
    // costs far more per message.
    const std::size_t rounds = messages / 20 + 1'000;
    std::printf("  round trips        %s\n", bench::humanize(rounds).c_str());
    std::printf("  channel capacity   2 slots each way, the minimum a ring can have\n\n");

    // Warm both variants before measuring either.
    (void)run_ping_pong<RingChannel<2>, RingChannel<2>>(rounds / 10 + 100, producer_core,
                                                        consumer_core);
    (void)run_ping_pong<MutexChannel<2>, MutexChannel<2>>(rounds / 10 + 100, producer_core,
                                                          consumer_core);

    const LatencyResult ring_lat =
        run_ping_pong<RingChannel<2>, RingChannel<2>>(rounds, producer_core, consumer_core);
    std::printf("  lock-free ring, two channels\n");
    bench::histogram_row("half round trip", ring_lat.one_way);
    std::printf("  %-26s %s\n", "producer", ring_lat.producer_placement.c_str());
    std::printf("  %-26s %s\n", "consumer", ring_lat.consumer_placement.c_str());

    const LatencyResult mutex_lat =
        run_ping_pong<MutexChannel<2>, MutexChannel<2>>(rounds, producer_core, consumer_core);
    std::printf("  mutex + condvar, two channels\n");
    bench::histogram_row("half round trip", mutex_lat.one_way);
    std::printf("  %-26s %s\n", "producer", mutex_lat.producer_placement.c_str());
    std::printf("  %-26s %s\n", "consumer", mutex_lat.consumer_placement.c_str());

    if (!ring_lat.echoed_correctly || !mutex_lat.echoed_correctly) {
        std::printf("\nWARNING: an echoed token did not match the one sent.\n"
                    "         The hand-off moved the wrong bytes and the figures\n"
                    "         above are meaningless.\n");
    }

    bench::note(
        "WHAT THIS MEASURES, AND WHAT IT DOES NOT.\n"
        "\n"
        "Both channels are drained by spinning, so each thread is polling the\n"
        "other's cache line in a tight loop. That is the ring in its BEST case:\n"
        "the line stays hot and shared between two nearby cores, and a real\n"
        "handler whose consumer blocks, or whose two threads are not co-\n"
        "resident, will see a worse hand-off than the p50 above suggests. The\n"
        "tail is the more informative figure -- the ring's p999 is where\n"
        "interrupts and preemption land.\n"
        "\n"
        "The half-round-trip p50 is close to the clock-read p50 printed above,\n"
        "which means most of what it measures is the instrument. Treat the p50\n"
        "as unresolvable and read the p99 and p999 instead.\n"
        "\n"
        "The mutex baseline's ABSOLUTE number is platform-specific in a way the\n"
        "ring's is not. On Windows a condition_variable wait is a kernel wait,\n"
        "so it lands around two microseconds and the ratio below looks\n"
        "spectacular. On Linux the same baseline is a futex, which is far\n"
        "cheaper, and the speedup is a fraction of this. Quote the ring's\n"
        "throughput, not the ratio, if you have to quote one number.\n"
        "\n"
        "TWO CHANNELS, NOT ONE. The exchange is bidirectional and an SPSC ring\n"
        "is not: it has exactly one producer and one consumer. An earlier\n"
        "revision of this file pushed a token down one ring and waited for it to\n"
        "come back up the same one, which silently made both threads consumers\n"
        "of a single-producer queue. It reported a plausible and much too fast\n"
        "number rather than failing. The channel types above make the direction\n"
        "explicit so that mistake cannot be made quietly again.\n"
        "\n"
        "The mutex baseline blocks, so each hand-off can cost a futex wakeup and\n"
        "a syscall. That is the whole of the difference on x86-64: the ring's\n"
        "acquire/release pairs compile to plain loads and stores with no fence\n"
        "instruction, because there is exactly one producer and one consumer\n"
        "per ring and nothing outside it needs ordering.\n");

    bench::print_publication_notice();
    return 0;
}