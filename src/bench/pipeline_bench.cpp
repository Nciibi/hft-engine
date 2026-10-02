// Market data pipeline: one thread versus two, with a batching sweep.
//
// What this measures
// ------------------
// The ring in the job it exists for. One thread decodes ITCH frames and
// applies them to an order book; the alternative splits that into a
// decoder thread that pushes into a ring and a book thread that pops and
// applies. Everything else -- the feed, the pools, the warmup, the core
// placement -- is identical, and the single-thread baseline is measured
// IN THIS SAME PROCESS AND RUN rather than quoted from another tool,
// because a comparison spanning two runs on different conditions is
// exactly the kind of number this repository exists to avoid.
//
// The prediction, stated before the numbers
// ----------------------------------------
// Per-message work in this engine is a few nanoseconds: decode is an
// integer reassembly and the book update is a ladder walk. A cross-core
// cache-line hand-off is tens to hundreds of nanoseconds. So at one
// message per ring slot the two-thread pipeline SHOULD BE SLOWER THAN THE
// SINGLE-THREADED LOOP, and if it is not, something in the measurement is
// wrong rather than something in the hardware being kind.
//
// That is not a disappointing result, it is the reason real market-data
// handlers batch. Which is what the rest of this tool measures: the same
// pipeline at batch sizes 1, 8, 32 and 128. The argument is the curve --
// where the hand-off stops dominating the work it is carrying -- and not
// any single row of it.
//
// Correctness argument
// --------------------
// Both paths consume the same feed in the same order, so they must build
// the same book. Every run's final state is fingerprinted with the same
// FNV-1a checksum the replay tool uses, and a run whose checksum differs
// from the single-threaded baseline is reported as INVALID regardless of
// how fast it was. A threaded pipeline that drops or reorders a message
// is not faster, it is wrong, and it would look exactly like a win in
// every other column of the table.
//
// Usage:
//   hft_pipeline_bench [records]

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "bench/report.hpp"
#include "feed/generator.hpp"
#include "hft/concurrent/spsc_ring.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "replay/checksum.hpp"
#include "hft/util/affinity.hpp"

namespace {

using hft::Nanos;
using hft::Price;
using hft::Side;
using hft::concurrent::SpscRing;
using hft::lob::OrderBook;

/// Slot capacity: enough to ride out a scheduling hiccup on either side
/// without letting the queue grow large enough to hide latency in a
/// backlog. Deep enough that a full ring is rare, shallow enough that
/// filling it means the consumer is genuinely behind.
inline constexpr std::size_t kRingCapacity = 1024;

/// A batch of decoded messages.
///
/// `itch::Message` is 64 bytes on this build, so K=128 is an 8KB copy per
/// hand-off. That is deliberate: the whole question is whether paying a
/// few hundred nanoseconds of memcpy once buys back dozens of individual
/// hand-offs, and a batch too small to measure the tradeoff would not
/// answer it.
template <std::size_t K>
struct Batch {
    hft::itch::Message items[K]{};
    std::uint32_t count = 0;
};

static_assert(std::is_trivially_copyable_v<Batch<1>>,
              "the ring hands a batch over as bytes; a Batch that is not trivially "
              "copyable cannot be moved between threads by this queue");
static_assert(std::is_trivially_destructible_v<Batch<128>>,
              "the ring never runs destructors on its slots");

/// Book fingerprint.
///
/// FNV-1a over a fully ordered, self-delimiting stream of fixed-width
/// integers -- never formatted text -- exactly as the replay tool does
/// it. The point is reproducibility, not collision resistance: this makes
/// "the two-thread run built the same book" a checkable statement.
[[nodiscard]] std::uint64_t fingerprint(const OrderBook& book) noexcept {
    std::uint64_t hash = hft::replay::fnv1a_offset_basis;
    hash = hft::replay::fnv1a_u64(hash, book.level_count(Side::bid));
    hash = hft::replay::fnv1a_u64(hash, book.level_count(Side::ask));
    hash = hft::replay::fnv1a_u64(hash, book.order_count(Side::bid));
    hash = hft::replay::fnv1a_u64(hash, book.order_count(Side::ask));
    hash = hft::replay::fnv1a_u64(hash, book.aggregate_at(Side::bid).raw());
    hash = hft::replay::fnv1a_u64(hash, book.aggregate_at(Side::ask).raw());

    // Level price and aggregate, in ladder order, both sides. The ladder
    // is ordered, so this stream is deterministic; a book that differs
    // only in how its orders are grouped within a level would not be
    // distinguished here, which is a known limit of fingerprinting
    // state rather than a property of this checksum.
    for (const Side side : {Side::bid, Side::ask}) {
        const std::vector<hft::lob::LevelSnapshot> levels = book.levels(side);
        hash = hft::replay::fnv1a_u64(hash, static_cast<std::uint64_t>(levels.size()));
        for (const hft::lob::LevelSnapshot& level : levels) {
            hash = hft::replay::fnv1a_u64(hash, static_cast<std::uint64_t>(level.price.raw()));
            hash = hft::replay::fnv1a_u64(hash, level.aggregate_size.raw());
            hash = hft::replay::fnv1a_u64(hash, level.order_count);
        }
    }
    return hash;
}

struct RunResult {
    std::uint64_t records = 0;
    std::uint64_t applied = 0;
    double elapsed_ns = 0.0;
    std::uint64_t backpressure_spins = 0;
    std::uint64_t checksum = 0;
    std::uint32_t bid_levels = 0;
    std::uint32_t ask_levels = 0;
};

[[nodiscard]] double rate_of(const RunResult& r) {
    return r.elapsed_ns > 0.0 ? static_cast<double>(r.records) * 1e9 / r.elapsed_ns : 0.0;
}

/// Decode and apply on one thread. This is the number everything else is
/// measured against.
[[nodiscard]] RunResult single_threaded(const std::vector<std::uint8_t>& data, std::size_t pool,
                                        std::size_t core) {
    (void)hft::util::pin_current_thread(core);
    OrderBook book(pool, pool);
    RunResult result;

    bench::Timer timer;
    std::size_t offset = 0;
    std::uint64_t decoded = 0;

    while (offset < data.size()) {
        if (data.size() - offset < hft::feed::kCaptureSequenceSize + hft::itch::kLengthPrefixSize) {
            break;
        }
        const std::size_t frame_at = offset + hft::feed::kCaptureSequenceSize;
        const hft::itch::DecodeResult r =
            hft::itch::decode(data.data() + frame_at, data.size() - frame_at);
        const std::size_t stride = hft::itch::frame_stride(r);
        if (stride == 0) {
            break;
        }
        if (r.ok()) {
            ++decoded;
            const hft::lob::ApplyResult applied = hft::lob::apply(r.message, book);
            if (applied.applied) {
                ++result.applied;
            }
        }
        offset = frame_at + stride;
    }

    result.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    result.records = decoded;
    result.checksum = fingerprint(book);
    result.bid_levels = book.level_count(Side::bid);
    result.ask_levels = book.level_count(Side::ask);
    return result;
}

/// Decode on one thread, apply on another, through a ring of batches.
template <std::size_t K>
[[nodiscard]] RunResult two_threaded(const std::vector<std::uint8_t>& data, std::size_t pool,
                                     std::size_t decoder_core, std::size_t book_core) {
    // The ring is heap-allocated. At K=128 a 1024-slot ring is 8MB of
    // batch storage, which overflows a thread stack -- and a benchmark
    // that dies with a stack overflow two thirds of the way through its
    // own output is worse than no benchmark. Construction, including
    // zeroing the slots, happens before the timer starts, so neither the
    // allocation nor the zeroing is inside the measured region.
    auto ring = std::make_unique<SpscRing<Batch<K>, kRingCapacity>>();
    OrderBook book(pool, pool);
    RunResult result;
    std::atomic<std::uint64_t> spins{0};
    std::atomic<bool> decoder_done{false};

    std::thread book_thread([&] {
        (void)hft::util::pin_current_thread(book_core);
        Batch<K> batch;
        for (;;) {
            bool progressed = false;
            while (ring->try_pop(batch)) {
                for (std::uint32_t i = 0; i < batch.count; ++i) {
                    const hft::lob::ApplyResult applied = hft::lob::apply(batch.items[i], book);
                    if (applied.applied) {
                        ++result.applied;
                    }
                }
                batch.count = 0;
                progressed = true;
            }
            if (!progressed && decoder_done.load(std::memory_order_acquire)) {
                // The acquire above synchronises with the decoder's
                // release store, which follows its last push, so a
                // failed pop after it means the ring is empty for good.
                // Without this second observation a batch that landed
                // between the pop above and the flag read is dropped,
                // which is precisely a lost-message bug.
                break;
            }
            if (!progressed) {
                spins.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    });

    bench::Timer timer;
    std::thread decoder([&] {
        (void)hft::util::pin_current_thread(decoder_core);
        Batch<K> batch{};
        std::uint64_t decoded = 0;
        std::size_t offset = 0;

        while (offset < data.size()) {
            if (data.size() - offset < hft::feed::kCaptureSequenceSize + hft::itch::kLengthPrefixSize) {
                break;
            }
            const std::size_t frame_at = offset + hft::feed::kCaptureSequenceSize;
            const hft::itch::DecodeResult r =
                hft::itch::decode(data.data() + frame_at, data.size() - frame_at);
            const std::size_t stride = hft::itch::frame_stride(r);
            if (stride == 0) {
                break;
            }
            if (r.ok()) {
                ++decoded;
                batch.items[batch.count++] = r.message;
                if (batch.count == K) {
                    while (!ring->try_push(batch)) {
                        spins.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                    }
                    batch.count = 0;
                }
            }
            offset = frame_at + stride;
        }

        // Flush the tail. A partial batch left unflushed is a silent
        // truncation of the feed, and it would show up only as a
        // checksum mismatch.
        if (batch.count > 0) {
            while (!ring->try_push(batch)) {
                spins.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }

        result.records = decoded;
        decoder_done.store(true, std::memory_order_release);
    });

    decoder.join();
    book_thread.join();

    result.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    result.checksum = fingerprint(book);
    result.bid_levels = book.level_count(Side::bid);
    result.ask_levels = book.level_count(Side::ask);
    result.backpressure_spins = spins.load();
    return result;
}

void report(const char* label, const RunResult& r, const RunResult& baseline, bool is_baseline) {
    const bool matches = r.checksum == baseline.checksum && r.records == baseline.records &&
                         r.applied == baseline.applied;
    const double ratio = is_baseline || rate_of(baseline) <= 0.0
                             ? 1.0
                             : rate_of(r) / rate_of(baseline);

    std::printf("  %-22s %12s msg/s  %9.3f ms  %5.2fx  %s\n", label,
                bench::humanize(static_cast<std::uint64_t>(rate_of(r))).c_str(),
                r.elapsed_ns / 1e6, ratio,
                is_baseline ? "(baseline)" : (matches ? "checksum match" : "CHECKSUM MISMATCH"));
    std::printf("  %-22s records %s  applied %s  yields %s  book %u/%u levels\n", "",
                bench::humanize(r.records).c_str(), bench::humanize(r.applied).c_str(),
                bench::humanize(r.backpressure_spins).c_str(), r.bid_levels, r.ask_levels);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t records = 2'000'000;
    if (argc > 1) {
        records = std::strtoull(argv[1], nullptr, 10);
    }
    if (records == 0) {
        records = 1;
    }

    bench::print_environment("market data pipeline benchmark");

    // A mixed capture with a bounded live-order count, so the book both
    // reprices and holds a realistic depth. Left unbounded, every level
    // only ever grows, the touch is set once and never moves, and the
    // midpoint is frozen -- the generator's own note explains why, and a
    // frozen midpoint makes the apply path unrealistically cheap because
    // the ladder stops changing shape.
    hft::feed::CaptureConfig config;
    config.record_count = records;
    config.price_levels = 32;
    config.max_live_orders = 2'000;
    config.drift_raw = 60;

    hft::feed::CaptureStats stats{};
    const std::vector<std::uint8_t> data = hft::feed::generate_capture(config, &stats);

    const std::size_t pool = records;
    const std::size_t order_bytes = pool * sizeof(hft::lob::OrderNode);
    const std::size_t level_bytes = pool * sizeof(hft::lob::LevelNode);

    std::printf("records requested    %s\n", bench::humanize(records).c_str());
    std::printf("records generated    %s\n", bench::humanize(stats.records).c_str());
    std::printf("capture mix          %s adds, %s executes, %s cancels, %s deletes\n",
                bench::humanize(stats.adds).c_str(), bench::humanize(stats.executes).c_str(),
                bench::humanize(stats.cancels).c_str(), bench::humanize(stats.deletes).c_str());
    std::printf("capture bytes        %s\n", bench::humanize(data.size()).c_str());
    std::printf("price levels/side    %s\n", bench::humanize(config.price_levels).c_str());
    std::printf("live order cap       %s\n", bench::humanize(config.max_live_orders).c_str());
    std::printf("pool orders          %s (%s MiB)\n", bench::humanize(pool).c_str(),
                bench::humanize(order_bytes / (1024 * 1024)).c_str());
    std::printf("pool levels          %s (%s MiB)\n", bench::humanize(pool).c_str(),
                bench::humanize(level_bytes / (1024 * 1024)).c_str());
    std::printf("ring capacity        %s slots\n\n", bench::humanize(kRingCapacity).c_str());

    if (pool * (sizeof(hft::lob::OrderNode) + sizeof(hft::lob::LevelNode)) > (1u << 30)) {
        std::printf("WARNING: pool memory exceeds 1 GiB. This machine may start paging,\n"
                    "         and a paged run measures the page fault.\n\n");
    }

    bench::section("MEASUREMENT COST");
    const bench::LatencyHistogram clock = bench::measure_clock_overhead();
    bench::print_clock_overhead(clock);

    const std::size_t decoder_core = 0;
    const std::size_t book_core = hft::util::other_core(decoder_core);

    bench::section("PLACEMENT");
    std::printf("  decoder requested   logical %zu\n", decoder_core);
    std::printf("  book thread         logical %zu\n", book_core);
    std::printf("  distinct physical cores: %s\n\n",
                hft::util::shares_physical_core(decoder_core, book_core) ? "NO" : "yes");
    if (hft::util::shares_physical_core(decoder_core, book_core)) {
        std::printf("WARNING: both threads resolved to the same physical core. Every\n"
                    "         figure below measures one core, not two.\n\n");
    }

    // Warm the feed pages and both pools before anything is measured. The
    // generator runs here, outside the timed region, so the tool is not
    // measuring its own input generation.
    (void)single_threaded(data, pool, decoder_core);

    bench::section("SINGLE-THREADED BASELINE (decode + apply, one core)");
    const RunResult baseline = single_threaded(data, pool, decoder_core);
    report("1 thread", baseline, baseline, true);
    std::printf("\n");

    bench::section("TWO THREADS THROUGH THE RING, BY BATCH SIZE");
    std::printf("  %-22s %12s         %9s      %5s  %12s\n", "", "rate", "elapsed", "speedup",
                "ring memory");
    std::printf("  (K = decoded messages carried per ring slot; a hand-off costs\n"
                "   one cache-line transfer, so larger K amortises it -- at the\n"
                "   price of a larger copy. The crossing point is the result.)\n\n");

    const RunResult k1 = two_threaded<1>(data, pool, decoder_core, book_core);
    report("2 threads, K=1", k1, baseline, false);

    const RunResult k8 = two_threaded<8>(data, pool, decoder_core, book_core);
    report("2 threads, K=8", k8, baseline, false);

    const RunResult k32 = two_threaded<32>(data, pool, decoder_core, book_core);
    report("2 threads, K=32", k32, baseline, false);

    const RunResult k128 = two_threaded<128>(data, pool, decoder_core, book_core);
    report("2 threads, K=128", k128, baseline, false);

    // A checksum mismatch invalidates the row it appears on. Reporting a
    // speed for a pipeline that corrupted the book would be the worst
    // possible outcome for this tool.
    const RunResult all[] = {k1, k8, k32, k128};
    const char* names[] = {"K=1", "K=8", "K=32", "K=128"};
    bool any_mismatch = false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (all[i].checksum != baseline.checksum || all[i].records != baseline.records ||
            all[i].applied != baseline.applied) {
            std::printf("  %-22s CHECKSUM MISMATCH vs baseline: checksum %016llx vs %016llx, "
                        "records %llu vs %llu\n",
                        names[i],
                        static_cast<unsigned long long>(all[i].checksum),
                        static_cast<unsigned long long>(baseline.checksum),
                        bench::u64(all[i].records), bench::u64(baseline.records));
            any_mismatch = true;
        }
    }
    if (any_mismatch) {
        std::printf("\n  A threaded run built a different book from the same feed.\n"
                    "  The speeds above are meaningless: a pipeline that drops or\n"
                    "  reorders a message is not faster, it is wrong.\n");
    } else {
        std::printf("\n  All four threaded runs produced a book identical to the\n"
                    "  single-threaded baseline: same checksum, same record count,\n"
                    "  same applied count.\n");
    }
    std::fflush(stdout);

    bench::note(
        "READ THE RATIO COLUMN AS A CLAIM TO BE FALSIFIED, NOT A RESULT.\n"
        "\n"
        "At K=1 the prediction is that two threads LOSE. Per-message work here\n"
        "is single-digit nanoseconds; a cross-core cache-line hand-off is tens\n"
        "to hundreds. There is no arrangement of these two numbers that makes a\n"
        "per-message hand-off pay for itself, and a measured win at K=1 would\n"
        "mean the measurement is wrong rather than the hardware being kind.\n"
        "\n"
        "Where the two threads break even is the useful number, because that is\n"
        "the batch size a real market-data handler would pick. It is also\n"
        "specific to this host: a bigger L3, a narrower NUMA link or a slower\n"
        "cross-core transfer all move it.\n"
        "\n"
        "The single-thread baseline is measured in this same process, on the same\n"
        "feed, with the same pools and the same warmup, immediately above the\n"
        "threaded runs. It is NOT the figure from hft_bench, which measures a\n"
        "different thing over a different feed.\n");

    bench::print_publication_notice();
    return 0;
}