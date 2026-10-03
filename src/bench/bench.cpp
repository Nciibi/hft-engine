// Benchmark harness.
//
// Reports per-stage latency percentiles and end-to-end throughput.
//
// Measurement discipline, which is most of what makes a number in this
// project worth quoting:
//
//  * Decode and book-update are timed separately, so a regression can
//    be attributed rather than guessed at.
//  * The per-message cost of clock reads is measured and reported
//    first. If timing overhead is a large fraction of the signal, every
//    number below it is suspect, and a reader should be able to see
//    that rather than take it on trust.
//  * The feed is generated and the buffer touched before timing starts,
//    so page faults and the generator do not land inside the measured
//    region.
//  * Book depth is reported alongside throughput. A msgs/sec figure
//    against a book that never holds more than a handful of orders
//    measures the harness, not the book.
//  * Nothing here warms up silently: a warmup pass runs and is
//    reported, and its results are discarded.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bench/report.hpp"
#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/util/histogram.hpp"
#include "hft/util/timer.hpp"

namespace {

using bench::Stopwatch;
using hft::util::LatencyHistogram;
using hft::util::Timer;

void print_histogram(const char* label, const LatencyHistogram& h) {
    std::printf("  %-22s p50 %7.0f  p99 %7.0f  p999 %7.0f  max %7.0f  n %llu\n", label,
                static_cast<double>(h.percentile(0.50)),
                static_cast<double>(h.percentile(0.99)),
                static_cast<double>(h.percentile(0.999)),
                static_cast<double>(h.max()),
                static_cast<unsigned long long>(h.count()));
    if (h.overflow_count() != 0) {
        std::printf("  %-22s WARNING: %llu samples beyond histogram range\n", "",
                    static_cast<unsigned long long>(h.overflow_count()));
    }
}

std::string humanize(std::uint64_t v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    return std::string(buf);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t message_count = 1'000'000;
    if (argc > 1) {
        message_count = std::strtoull(argv[1], nullptr, 10);
        if (message_count == 0) {
            message_count = 1'000'000;
        }
    }

    std::printf("HFT Engine benchmark\n");
    std::printf("--------------------\n");
    std::printf("messages            %s\n", humanize(message_count).c_str());

    // ---- Clock overhead, measured first and reported first ---------
    {
        constexpr int kClockSamples = 100'000;
        LatencyHistogram clock_hist(1, 100'000);
        for (int i = 0; i < kClockSamples; ++i) {
            const std::uint64_t a = Timer::now();
            const std::uint64_t b = Timer::now();
            clock_hist.record(b - a);
        }
        print_histogram("clock read", clock_hist);
        std::printf("\n  Every per-stage figure below includes one clock pair.\n\n");
    }

    // ---- Generate and warm the feed outside the timed region -------
    hft::feed::GeneratorConfig config;
    config.message_count = message_count;
    const std::vector<std::uint8_t> feed = hft::feed::generate_add_orders(config);
    // Pool capacity is derived from the feed, not hard-coded. An
    // add-only benchmark keeps every order it accepts, so the book can
    // hold at most `message_count` orders and at most that many
    // distinct prices. Sizing the pools any smaller saturates them
    // partway through the run, at which point the benchmark silently
    // stops measuring insertion and starts measuring rejection.
    const std::size_t pool = message_count;
    const std::size_t order_bytes = pool * sizeof(hft::lob::OrderNode);
    const std::size_t level_bytes = pool * sizeof(hft::lob::LevelNode);
    std::printf("pool orders        %s (%zu bytes)\n", humanize(pool).c_str(), order_bytes);
    std::printf("pool levels        %s (%zu bytes)\n", humanize(pool).c_str(), level_bytes);
    std::printf("pool memory        %.1f MiB\n",
                static_cast<double>(order_bytes + level_bytes) / (1024.0 * 1024.0));
    std::printf("feed bytes         %s\n", humanize(feed.size()).c_str());
    std::printf("frames             %s\n",
                humanize(feed.size() / hft::itch::frame_size(hft::itch::off::kAddOrderSize))
                    .c_str());

    hft::lob::OrderBook book(pool, pool);

    // ---- Warmup: fault in the pages, prime the pools -----------------
    // Uses a throwaway book. Reusing the measured book would re-add the
    // same OrderIds, every one of which would then be rejected as a
    // duplicate, and the benchmark would silently be timing rejection
    // instead of insertion.
    {
        hft::lob::OrderBook warm(pool, pool);
        const std::uint8_t* p = feed.data();
        std::size_t remaining = feed.size();
        std::size_t applied = 0;
        while (remaining > 0) {
            const hft::itch::DecodeResult r = hft::itch::decode(p, remaining);
            const std::size_t stride = hft::itch::frame_stride(r);
            if (stride == 0) {
                break;
            }
            if (r.ok()) {
                const auto& ao = std::get<hft::itch::AddOrder>(r.message.body);
                hft::lob::BookStatus st{};
                if (st == hft::lob::BookStatus::ok) {
                    warm.add(ao.side, ao.price, ao.size, ao.id, st);
                }
                if (st == hft::lob::BookStatus::ok) {
                    ++applied;
                }
            }
            p += stride;
            remaining -= stride;
        }
        std::printf("warmup applied      %s (discarded)\n\n", humanize(applied).c_str());
    }

    // ---- Measured pass ----------------------------------------------
    // 1ns buckets. The measured clock-read overhead above has a 1ns
    // median, so anything coarser than 1ns would quantise away the very
    // signal being measured and report a flat p50/p99/p999.
    constexpr std::uint32_t kBucketNs = 1;
    constexpr std::uint32_t kMaxNs = 4'000'000;  // 4ms ceiling
    LatencyHistogram decode_hist(kBucketNs, kMaxNs);
    LatencyHistogram book_hist(kBucketNs, kMaxNs);
    LatencyHistogram total_hist(kBucketNs, kMaxNs);

    std::uint64_t decoded = 0;
    std::uint64_t applied = 0;
    std::uint64_t rejected = 0;
    std::uint64_t unknown = 0;
    std::uint64_t malformed = 0;

    // Rejection reasons are counted individually. A run where most adds
    // are refused is measuring the refusal path, and a single
    // "rejected" total would let that pass as a throughput win.
    std::uint64_t rej_capacity = 0;
    std::uint64_t rej_duplicate = 0;
    std::uint64_t rej_zero_size = 0;
    std::uint64_t unexpected = 0;

    Timer total_timer;
    const std::uint8_t* p = feed.data();
    std::size_t remaining = feed.size();

    while (remaining > 0) {
        // Boundaries are captured in nanoseconds, not platform ticks, so
        // every subtraction below is in the unit the histograms are
        // reported in. See the units note in timer.hpp: recording raw
        // QPC ticks here made every figure in this tool wrong by 100x on
        // the development host.
        const std::uint64_t t_start = Stopwatch::now_ns();

        const hft::itch::DecodeResult r = hft::itch::decode(p, remaining);
        const std::size_t stride = hft::itch::frame_stride(r);
        if (stride == 0) {
            break;
        }

        const std::uint64_t t_decoded = Stopwatch::now_ns();
        decode_hist.record(t_decoded - t_start);

        if (r.ok()) {
            ++decoded;
            // This feed is Add Order only, so a successful decode is
            // necessarily an Add Order. Anything else reaching here
            // would be a generator bug, and is counted rather than
            // reinterpreted.
            const auto* ao = std::get_if<hft::itch::AddOrder>(&r.message.body);
            if (ao == nullptr) {
                ++unexpected;
            } else {
                hft::lob::BookStatus st{};
                book.add(ao->side, ao->price, ao->size, ao->id, st);
                if (st == hft::lob::BookStatus::ok) {
                    ++applied;
                } else {
                    ++rejected;
                    switch (st) {
                        case hft::lob::BookStatus::capacity_exhausted: ++rej_capacity; break;
                        case hft::lob::BookStatus::duplicate_order:   ++rej_duplicate; break;
                        case hft::lob::BookStatus::zero_size:         ++rej_zero_size; break;
                        default: break;
                    }
                }
            }
        } else if (r.status == hft::itch::DecodeStatus::unknown_type) {
            ++unknown;
        } else {
            ++malformed;
        }

        const std::uint64_t t_done = Stopwatch::now_ns();
        book_hist.record(t_done - t_decoded);
        total_hist.record(t_done - t_start);

        p += stride;
        remaining -= stride;
    }

    const std::uint64_t elapsed_ns = total_timer.elapsed_ns();

    std::printf("results\n");
    std::printf("-------\n");
    print_histogram("decode", decode_hist);
    print_histogram("book update", book_hist);
    print_histogram("total", total_hist);
    std::printf("\n");
    std::printf("decoded             %s\n", humanize(decoded).c_str());
    std::printf("applied             %s\n", humanize(applied).c_str());
    std::printf("rejected            %s\n", humanize(rejected).c_str());
    if (rejected != 0) {
        std::printf("  capacity          %s\n", humanize(rej_capacity).c_str());
        std::printf("  duplicate id      %s\n", humanize(rej_duplicate).c_str());
        std::printf("  zero size         %s\n", humanize(rej_zero_size).c_str());
        if (rej_capacity != 0) {
            std::printf(
                "  WARNING: the level or order pool saturated. This run does\n"
                "           NOT measure sustained insertion throughput.\n");
        }
    }
    std::printf("unknown type        %s\n", humanize(unknown).c_str());
    std::printf("malformed           %s\n", humanize(malformed).c_str());
    if (unexpected != 0) {
        std::printf("unexpected type     %s (generator emitted a non-add)\n",
                    humanize(unexpected).c_str());
    }
    std::printf("elapsed             %.6f s\n", static_cast<double>(elapsed_ns) / 1e9);
    if (elapsed_ns > 0) {
        std::printf("throughput          %.0f msg/s\n",
                    static_cast<double>(decoded) * 1e9 / static_cast<double>(elapsed_ns));
    }
    std::printf("\nbook state\n");
    std::printf("---------\n");
    std::printf("bid levels          %u\n", book.level_count(hft::Side::bid));
    std::printf("ask levels          %u\n", book.level_count(hft::Side::ask));
    std::printf("bid orders          %u\n", book.order_count(hft::Side::bid));
    std::printf("ask orders          %u\n", book.order_count(hft::Side::ask));
    // Ladder depth is the single most important structural number in
    // this run. The price ladder is a sorted linked list, so inserting
    // a price that is not adjacent to the best walks from the head. The
    // cost of book update is therefore proportional to ladder depth,
    // and a figure quoted without it is not interpretable.
    std::printf("ladder depth        %u bid, %u ask\n", book.level_count(hft::Side::bid),
                book.level_count(hft::Side::ask));
    // Aggregate resting size is a share count, not a price, so it is
    // printed raw. Rendering it through Price::to_string would show
    // "100.0000" for 100 shares and quietly mislead.
    std::printf("bid resting shares  %llu\n",
                static_cast<unsigned long long>(book.aggregate_at(hft::Side::bid).raw()));
    std::printf("ask resting shares  %llu\n",
                static_cast<unsigned long long>(book.aggregate_at(hft::Side::ask).raw()));

    std::printf("\nNOTE: this run is from a development machine.\n");
    std::printf("Committed numbers must come from the rented metal host with\n");
    std::printf("the environment recorded in results/ENVIRONMENT.md.\n");
    return 0;
}
