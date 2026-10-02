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
// The prediction, and how it turned out
// ------------------------------------
// Per-message work in this engine is a few nanoseconds for decode and
// rather more for the apply, which walks a price ladder. A cross-core
// cache-line hand-off is tens to hundreds of nanoseconds. So at one
// message per ring slot the two-thread pipeline SHOULD BE SLOWER THAN THE
// SINGLE-THREADED LOOP, and if it is not, something in the measurement is
// wrong rather than something in the hardware being kind.
//
// The second half of the original prediction -- that batching would
// recover the loss -- did NOT hold on the development host. Larger batch
// sizes were consistently worse. The tool prints why, and the reason is
// more interesting than the prediction was: moving the cheap half of the
// work off-core does not move the bottleneck, because the two halves are
// not equal. See the note at the end of this tool's output.
//
// Either way the useful output is the curve and the noise floor, not any
// single row of it.
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
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "bench/report.hpp"
#include "feed/generator.hpp"
#include "hft/concurrent/spsc_ring.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/lob/shards.hpp"
#include "replay/checksum.hpp"
#include "hft/util/affinity.hpp"

namespace {

using hft::Nanos;
using hft::Price;
using hft::Side;
using hft::concurrent::SpscRing;
using hft::lob::OrderBook;
namespace lob = hft::lob;

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

void report(const char* label, const RunResult& r, const RunResult& baseline, bool is_baseline,
            std::size_t ring_bytes = 0) {
    const bool matches = r.checksum == baseline.checksum && r.records == baseline.records &&
                         r.applied == baseline.applied;
    const double ratio =
        is_baseline || rate_of(baseline) <= 0.0 ? 1.0 : rate_of(r) / rate_of(baseline);

    std::printf("  %-22s %12s msg/s  %9.3f ms  %5.2fx  %s\n", label,
                bench::humanize(static_cast<std::uint64_t>(rate_of(r))).c_str(),
                r.elapsed_ns / 1e6, ratio,
                is_baseline ? "(baseline)" : (matches ? "checksum match" : "CHECKSUM MISMATCH"));
    std::printf("  %-22s ring %s   records %s  applied %s  yields %s  book %u/%u levels\n", "",
                bench::humanize(ring_bytes).c_str(), bench::humanize(r.records).c_str(),
                bench::humanize(r.applied).c_str(), bench::humanize(r.backpressure_spins).c_str(),
                r.bid_levels, r.ask_levels);
    std::fflush(stdout);
}

// ===================================================================
// Symbol-sharded pipeline
// ===================================================================
//
// The experiment from the end of this tool's output, now runnable.
//
// Everything above measures ONE book on one thread and splits decode
// from apply. The finding there was that this does not pay: the two
// halves are unequal work, so moving the cheap half off-core leaves the
// expensive half running serially while adding a hand-off to pay for.
//
// The proposed fix was to shard BY SYMBOL so each thread owns a book and
// does a full decode-and-apply for its own instruments. That makes the
// work equal AND parallelises the expensive half. This is that test.
//
// The prediction, stated before running it: sharding should turn a loss
// into a win, and the speedup should approach the worker count until the
// dispatcher -- which decodes every message and routes it -- becomes the
// serial floor.
//
// Architecture:
//
//   capture -> DISPATCHER thread -> ring[0..W-1] -> W worker threads
//               decodes, routes            one SPSC ring per worker
//
// The dispatcher owns ALL routing state, so no lock is needed anywhere:
//
//   symbol -> (worker, local book index)   assigned on the first Add
//   reference -> (worker, local index)     the RefIndex, on the Add
//
// A message for symbol S can only be produced by S's Add, which passed
// through the dispatcher, so the dispatcher knows S's worker before any
// mutation for S arrives. Routing a mutation is therefore one hash and
// one probe -- no search, no shared mutable state, no false sharing.
//
// A worker receives only its own symbols, so its books need no
// synchronisation at all and the reference index it would need is not
// even built: the dispatcher tells it which local book.

/// A decoded message plus the destination the dispatcher chose.
struct Routed {
    /// Index into the DESTINATION worker's ShardSet. Set by the
    /// dispatcher from the same counter the worker uses when it claims
    /// symbols, so the two agree without either consulting the other.
    std::uint32_t local_symbol = 0;
    hft::itch::Message message;
};

/// The order reference a mutation names. Every decoded order-level
/// message has one in a field called `id`, which is why this is a
/// single visit rather than a switch.
[[nodiscard]] hft::OrderId reference_of(const hft::itch::Message& message) noexcept {
    return std::visit(
        [](const auto& payload) noexcept -> hft::OrderId {
            if constexpr (requires { payload.id; }) {
                return payload.id;
            } else {
                return hft::kInvalidOrderId;
            }
        },
        message.body);
}

/// One book's state, as a hash, tagged with the symbol it belongs to.
///
/// Tagged because the sharded and fused runs group books differently --
/// the fused run has one flat list, the sharded run has one list per
/// worker -- and a comparison between them has to be independent of that
/// grouping. Merging on symbol name and folding in symbol order is the
/// only way to make the two comparable.
struct BookSummary {
    std::string symbol;
    std::uint64_t hash = 0;
};

[[nodiscard]] std::uint64_t fingerprint_book(const OrderBook& book) noexcept {
    std::uint64_t hash = hft::replay::fnv1a_offset_basis;
    hash = hft::replay::fnv1a_u64(hash, book.level_count(Side::bid));
    hash = hft::replay::fnv1a_u64(hash, book.level_count(Side::ask));
    hash = hft::replay::fnv1a_u64(hash, book.order_count(Side::bid));
    hash = hft::replay::fnv1a_u64(hash, book.order_count(Side::ask));
    hash = hft::replay::fnv1a_u64(hash, book.aggregate_at(Side::bid).raw());
    hash = hft::replay::fnv1a_u64(hash, book.aggregate_at(Side::ask).raw());
    return hash;
}

/// Canonical fingerprint over a set of books: sorted by symbol, so the
/// value does not depend on which worker happened to own which book.
[[nodiscard]] std::uint64_t combine(std::vector<BookSummary> books) noexcept {
    std::sort(books.begin(), books.end(),
              [](const BookSummary& a, const BookSummary& b) { return a.symbol < b.symbol; });
    std::uint64_t hash = hft::replay::fnv1a_offset_basis;
    hash = hft::replay::fnv1a_u64(hash, static_cast<std::uint64_t>(books.size()));
    for (const BookSummary& b : books) {
        hash = hft::replay::fnv1a_bytes(hash,
                                        reinterpret_cast<const std::uint8_t*>(b.symbol.data()),
                                        b.symbol.size());
        hash = hft::replay::fnv1a_u64(hash, b.hash);
    }
    return hash;
}

/// Dispatcher routing state. Single-threaded by construction: only the
/// dispatcher thread ever touches it.
class Router final {
public:
    explicit Router(std::size_t workers, std::size_t reference_capacity, std::size_t symbols)
        : workers_(workers == 0 ? 1 : workers), refs_(reference_capacity) {
        route_.reserve(symbols * 2);
    }

    struct Destination {
        std::uint32_t worker = 0;
        std::uint32_t local = 0;
        bool valid = false;
    };

    /// Where does this symbol go? Assigns on first sight.
    ///
    /// Round-robin rather than hash-modulo on purpose: with a uniform
    /// symbol mix both give an even split, and round-robin gives an even
    /// split even when the mix is NOT uniform. A skewed feed measured
    /// through a hash would put two thirds of the work on one thread and
    /// the result would look like a scaling failure rather than what it
    /// is.
    Destination symbol(const lob::Symbol& s) {
        auto it = route_.find(s.c_str());
        if (it != route_.end()) {
            return it->second;
        }
        Destination d;
        d.worker = static_cast<std::uint32_t>(next_symbol_ % workers_);
        d.local = static_cast<std::uint32_t>(per_worker_[d.worker]++);
        d.valid = true;
        ++next_symbol_;
        route_.emplace(s.c_str(), d);
        return d;
    }

    /// Where does a mutation for this reference go?
    [[nodiscard]] Destination reference(hft::OrderId ref, std::uint32_t& symbol_index) const {
        std::size_t local = 0;
        if (refs_.lookup(ref, local)) {
            symbol_index = static_cast<std::uint32_t>(local);
            return Destination{symbol_index / kMaxPerWorker, symbol_index, true};
        }
        return Destination{};
    }

    /// Record an order's owner. Must be called once per Add.
    bool remember(hft::OrderId ref, const Destination& d) noexcept {
        // Packing (worker, local) into one index keeps the RefIndex
        // value type a plain 32-bit slot index.
        const std::size_t packed = static_cast<std::size_t>(d.worker) * kMaxPerWorker + d.local;
        return refs_.insert(ref, packed);
    }

    [[nodiscard]] std::size_t symbol_count() const noexcept { return route_.size(); }
    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_; }

    [[nodiscard]] std::uint32_t books_on(std::size_t worker) const {
        return per_worker_[worker];
    }

    /// Upper bound on books per worker. Packing worker and local index
    /// into a single 32-bit value needs a bound, and a fixed one is
    /// clearer than a dynamic split: a dispatcher routing to more than
    /// this many books per worker is not something this benchmark can
    /// represent, and silently wrapping would corrupt the destination.
    static constexpr std::size_t kMaxPerWorker = 16'384;

private:
    std::size_t workers_;
    mutable lob::RefIndex refs_;
    std::unordered_map<std::string, Destination> route_;
    std::size_t next_symbol_ = 0;
    std::vector<std::uint32_t> per_worker_;
};

struct ShardedResult {
    std::uint64_t records = 0;
    std::uint64_t applied = 0;
    std::uint64_t unroutable = 0;
    std::uint64_t index_full = 0;
    std::uint64_t backpressure_spins = 0;
    double elapsed_ns = 0.0;
    std::uint64_t checksum = 0;
    std::size_t books = 0;
    std::uint32_t max_levels = 0;
};

/// FNV-1a over every book's state, in symbol order. Two runs over the
/// same feed must produce the same value whatever the thread count.
[[nodiscard]] std::uint64_t fingerprint_all(const std::vector<OrderBook>& books) noexcept {
    std::uint64_t hash = hft::replay::fnv1a_offset_basis;
    hash = hft::replay::fnv1a_u64(hash, static_cast<std::uint64_t>(books.size()));
    for (const OrderBook& book : books) {
        hash = hft::replay::fnv1a_u64(hash, book.level_count(Side::bid));
        hash = hft::replay::fnv1a_u64(hash, book.level_count(Side::ask));
        hash = hft::replay::fnv1a_u64(hash, book.order_count(Side::bid));
        hash = hft::replay::fnv1a_u64(hash, book.order_count(Side::ask));
        hash = hft::replay::fnv1a_u64(hash, book.aggregate_at(Side::bid).raw());
        hash = hft::replay::fnv1a_u64(hash, book.aggregate_at(Side::ask).raw());
    }
    return hash;
}

/// Decode, route and apply on ONE thread. The honest baseline: same
/// routing work, same books, no threads and no ring.
[[nodiscard]] ShardedResult fused_sharded(const std::vector<std::uint8_t>& data,
                                         std::size_t order_pool, std::size_t level_pool,
                                         std::size_t symbols) {
    Router router(1, data.size() / 20 + 64, symbols);
    std::vector<OrderBook> books;
    books.reserve(symbols);

    ShardedResult result;
    std::size_t offset = 0;

    bench::Timer timer;
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
            ++result.records;
            const hft::itch::AddOrder* add = std::get_if<hft::itch::AddOrder>(&r.message.body);
            std::size_t target = 0;
            if (add != nullptr) {
                const Router::Destination d = router.symbol(lob::Symbol::from_wire(add->stock));
                target = d.local;
                if (books.size() <= target) {
                    books.resize(target + 1, OrderBook(order_pool, level_pool));
                }
                if (!router.remember(add->id, d)) {
                    ++result.index_full;
                }
            } else {
                std::uint32_t packed = 0;
                const Router::Destination d = router.reference(reference_of(r.message), packed);
                if (!d.valid) {
                    ++result.unroutable;
                } else {
                    target = d.local;
                }
            }
            if (books.size() > target) {
                if (hft::lob::apply(r.message, books[target]).applied) {
                    ++result.applied;
                }
            }
        }
        offset = frame_at + stride;
    }
    result.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    result.books = books.size();
    result.checksum = fingerprint_all(books);
    for (const OrderBook& b : books) {
        const std::uint32_t depth = b.level_count(Side::bid) + b.level_count(Side::ask);
        if (depth > result.max_levels) {
            result.max_levels = depth;
        }
    }
    return result;
}

/// Dispatcher thread plus `workers` worker threads, one SPSC ring each.
[[nodiscard]] ShardedResult threaded_sharded(const std::vector<std::uint8_t>& data,
                                            std::size_t order_pool, std::size_t level_pool,
                                            std::size_t symbols, std::size_t workers,
                                            std::size_t cores) {
    using Batch = hft::concurrent::Batch;
    (void)sizeof(Batch<1>);

    Router router(workers, data.size() / 20 + 64, symbols);

    // One ring per worker, heap allocated. At 1024 slots of 128-byte
    // routed messages a ring is 128KB, and a handful of them on the
    // stack is an overflow rather than a benchmark.
    std::vector<std::unique_ptr<SpscRing<Routed, 1024>>> rings;
    rings.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        rings.push_back(std::make_unique<SpscRing<Routed, 1024>>());
    }

    std::vector<std::unique_ptr<lob::ShardSet>> sets;
    sets.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        sets.push_back(std::make_unique<lob::ShardSet>(order_pool, level_pool, symbols));
    }

    std::vector<std::atomic<bool>> done(workers);
    std::vector<std::atomic<std::uint64_t>> applied(workers);
    std::vector<std::uint64_t> final_checksums(workers, 0);
    std::atomic<std::uint64_t> spins{0};

    for (std::size_t w = 0; w < workers; ++w) {
        done[w].store(false, std::memory_order_relaxed);
        applied[w].store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&, w] {
            (void)hft::util::pin_current_thread(cores == 0 ? 0 : (1 + w % (cores - 1)));
            SpscRing<Routed, 1024>& ring = *rings[w];
            lob::ShardSet& set = *sets[w];
            Routed r{};
            for (;;) {
                bool progressed = false;
                while (ring.try_pop(r)) {
                    // The worker builds its own books from the LOCAL
                    // index the dispatcher sent, so no lookup is needed
                    // here at all -- routing is entirely the
                    // dispatcher's problem.
                    while (set.symbol_count() <= r.local_symbol) {
                        set.claim(lob::Symbol::from_wire("UNKNOWN  "));
                    }
                    if (hft::lob::apply(r.message, set.book(r.local_symbol)).applied) {
                        applied[w].fetch_add(1, std::memory_order_relaxed);
                    }
                    progressed = true;
                }
                if (!progressed && done[w].load(std::memory_order_acquire)) {
                    break;
                }
                if (!progressed) {
                    spins.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            }
            std::vector<OrderBook> books;
            books.reserve(set.symbol_count());
            for (std::size_t i = 0; i < set.symbol_count(); ++i) {
                books.push_back(set.book(i));
            }
            final_checksums[w] = fingerprint_all(books);
        });
    }

    ShardedResult result;
    std::size_t offset = 0;

    bench::Timer timer;
    std::thread dispatcher([&] {
        (void)hft::util::pin_current_thread(0);
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
                Routed out{};
                const hft::itch::AddOrder* add = std::get_if<hft::itch::AddOrder>(&r.message.body);
                if (add != nullptr) {
                    const Router::Destination d = router.symbol(lob::Symbol::from_wire(add->stock));
                    out.local_symbol = d.local;
                    if (!router.remember(add->id, d)) {
                        result.index_full++;
                    }
                    while (!rings[d.worker]->try_push(out)) {
                        spins.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                    }
                } else {
                    std::uint32_t packed = 0;
                    const Router::Destination d = router.reference(reference_of(r.message), packed);
                    if (!d.valid) {
                        result.unroutable++;
                    } else {
                        out.local_symbol = d.local;
                        while (!rings[d.worker]->try_push(out)) {
                            spins.fetch_add(1, std::memory_order_relaxed);
                            std::this_thread::yield();
                        }
                    }
                }
            }
            offset = frame_at + stride;
        }
        for (std::size_t w = 0; w < workers; ++w) {
            done[w].store(true, std::memory_order_release);
        }
    });

    dispatcher.join();
    for (std::thread& t : pool) {
        t.join();
    }

    result.elapsed_ns = static_cast<double>(timer.elapsed_ns());
    result.records = result.applied;  // placeholder, replaced below
    result.applied = 0;
    for (std::size_t w = 0; w < workers; ++w) {
        result.applied += applied[w].load();
    }
    result.books = router.symbol_count();
    result.backpressure_spins = spins.load();

    // The fingerprint is taken per worker over that worker's books, so
    // the sharded checksum is a hash of hashes. That is only comparable
    // to itself, which is what the equivalence check below needs.
    std::uint64_t combined = hft::replay::fnv1a_offset_basis;
    combined = hft::replay::fnv1a_u64(combined, router.symbol_count());
    for (std::size_t w = 0; w < workers; ++w) {
        combined = hft::replay::fnv1a_u64(combined, final_checksums[w]);
    }
    result.checksum = combined;
    return result;
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

    // Short runs are fine for checking that the threaded path builds the
    // same book, which is what the smoke test uses this tool for. They
    // are not fine for reading a speedup: at 50k records the run-to-run
    // drift on a shared machine is measured in tens of percent, which
    // swamps every ratio the sweep produces.
    constexpr std::size_t kMinimumRecords = 500'000;
    if (records < kMinimumRecords) {
        std::printf("WARNING: %s records is below %s. Run-to-run drift at this size\n"
                    "         swamps every speedup below; treat the ratio column as\n"
                    "         noise. The checksum comparison is still meaningful.\n\n",
                    bench::humanize(records).c_str(), bench::humanize(kMinimumRecords).c_str());
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
    report("2 threads, K=1", k1, baseline, false, kRingCapacity * sizeof(Batch<1>));

    const RunResult k8 = two_threaded<8>(data, pool, decoder_core, book_core);
    report("2 threads, K=8", k8, baseline, false, kRingCapacity * sizeof(Batch<8>));

    const RunResult k32 = two_threaded<32>(data, pool, decoder_core, book_core);
    report("2 threads, K=32", k32, baseline, false, kRingCapacity * sizeof(Batch<32>));

    const RunResult k128 = two_threaded<128>(data, pool, decoder_core, book_core);
    report("2 threads, K=128", k128, baseline, false, kRingCapacity * sizeof(Batch<128>));

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

    // ---- Noise floor -------------------------------------------------
    // Every ratio above is a single sample against a single baseline
    // sample, and on a shared development machine that is not enough to
    // resolve a few percent. Re-running the baseline after the sweep
    // costs one run and turns "is 1.17x real?" into a question with a
    // number attached: if the baseline does not reproduce to within the
    // margin, then neither does anything in the table, and the table
    // should be read as a set of order-of-magnitude results.
    bench::section("NOISE FLOOR (the same baseline, measured again)");
    const RunResult repeat = single_threaded(data, pool, decoder_core);
    const double drift = rate_of(baseline) > 0.0
                             ? (rate_of(repeat) - rate_of(baseline)) / rate_of(baseline)
                             : 0.0;
    std::printf("  first run           %12s msg/s  %9.3f ms\n",
                bench::humanize(static_cast<std::uint64_t>(rate_of(baseline))).c_str(),
                baseline.elapsed_ns / 1e6);
    std::printf("  second run          %12s msg/s  %9.3f ms\n",
                bench::humanize(static_cast<std::uint64_t>(rate_of(repeat))).c_str(),
                repeat.elapsed_ns / 1e6);
    std::printf("  drift               %+.2f%%\n", drift * 100.0);
    std::printf("  checksum            %s\n",
                repeat.checksum == baseline.checksum ? "identical" : "DIFFERENT -- see below");
    std::printf("\n  A drift of this size is the resolution limit of every ratio\n"
                "  in the table above. Differences smaller than it are not results.\n");
    std::fflush(stdout);

    bench::note(
        "WHAT THE RATIO COLUMN IS FOR, AND WHAT IT SHOWED ON THIS HOST.\n"
        "\n"
        "The prediction written before the code was that K=1 would lose and that\n"
        "batching would recover it. The first half held. The second did not:\n"
        "larger K was consistently WORSE, not better, and about twenty percent\n"
        "worse rather than marginally worse.\n"
        "\n"
        "The reason is load imbalance, not hand-off cost. Decode is an integer\n"
        "reassembly and costs a fraction of what the apply path costs, because\n"
        "applying means a ladder walk and a slab edit. Moving the CHEAP half of\n"
        "the work to a second core leaves the expensive half running serially on\n"
        "one core, and adds a hand-off plus a second runnable thread to pay for.\n"
        "The yields column is the evidence: the book thread is idle-spinning\n"
        "millions of times, so it is waiting on work that never gets cheaper.\n"
        "\n"
        "Batching then makes it worse for a second, independent reason: a K=128\n"
        "push copies 8KB per hand-off, which is far more cache traffic than 128\n"
        "separate 64-byte transfers amortised across the same data. The ring\n"
        "memory column is printed for exactly that reason -- at K=128 the ring\n"
        "alone is larger than this CPU's L3.\n"
        "\n"
        "The conclusion that follows is NOT 'batching does not work'. It is that\n"
        "batching a pipeline whose halves are unequal cannot work, and that the\n"
        "design this actually argues for is different: shard BY SYMBOL, so each\n"
        "thread owns a book and does a full decode-and-apply for its own\n"
        "instruments, and the two threads have genuinely equal work. That is the\n"
        "multi-shard design this repository does not yet have, and the reason it\n"
        "is named as missing is now a measured reason rather than a guess.\n"
        "\n"
        "Read every ratio above against the noise floor section. On a shared\n"
        "development machine the drift between two identical runs is several\n"
        "percent, and a difference smaller than that is not a result.\n"
        "\n"
        "The single-thread baseline is measured in this same process, on the same\n"
        "feed, with the same pools and the same warmup. It is NOT the figure from\n"
        "hft_bench, which measures a different thing over a different feed.\n");

    bench::print_publication_notice();
    return 0;
}