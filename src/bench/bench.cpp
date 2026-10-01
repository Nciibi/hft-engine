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

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/util/histogram.hpp"
#include "hft/util/timer.hpp"

namespace {

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
    std::printf("feed bytes          %s\n", humanize(feed.size()).c_str());
    std::printf("frames              %s\n",
                humanize(feed.size() / hft::itch::frame_size(hft::itch::off::kAddOrderSize))
                    .c_str());

    hft::lob::OrderBook book(/*order_capacity=*/1u << 20, /*level_capacity=*/1u << 16);

    // ---- Warmup: fault in the pages, prime the pools -----------------
    // Uses a throwaway book. Reusing the measured book would re-add the
    // same OrderIds, every one of which would then be rejected as a
    // duplicate, and the benchmark would silently be timing rejection
    // instead of insertion.
    {
        hft::lob::OrderBook warm(/*order_capacity=*/1u << 20, /*level_capacity=*/1u << 16);
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
                hft::lob::BookStatus st{};
                if (st == hft::lob::BookStatus::ok) {
                    warm.add(r.message.add_order.side, r.message.add_order.price,
                             r.message.add_order.size, r.message.add_order.id, st);
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
    LatencyHistogram decode_hist(10, 2'000'000);
    LatencyHistogram book_hist(10, 2'000'000);
    LatencyHistogram total_hist(10, 4'000'000);

    std::uint64_t decoded = 0;
    std::uint64_t applied = 0;
    std::uint64_t rejected = 0;
    std::uint64_t unknown = 0;
    std::uint64_t malformed = 0;

    Timer total_timer;
    const std::uint8_t* p = feed.data();
    std::size_t remaining = feed.size();

    while (remaining > 0) {
        const std::uint64_t t_start = Timer::now();

        const hft::itch::DecodeResult r = hft::itch::decode(p, remaining);
        const std::size_t stride = hft::itch::frame_stride(r);
        if (stride == 0) {
            break;
        }

        const std::uint64_t t_decoded = Timer::now();
        decode_hist.record(t_decoded - t_start);

        if (r.ok()) {
            ++decoded;
            hft::lob::BookStatus st{};
            const hft::lob::Handle h =
                book.add(r.message.add_order.side, r.message.add_order.price,
                         r.message.add_order.size, r.message.add_order.id, st);
            if (st == hft::lob::BookStatus::ok) {
                ++applied;
            } else {
                ++rejected;
            }
            (void)h;
        } else if (r.status == hft::itch::DecodeStatus::unknown_type) {
            ++unknown;
        } else {
            ++malformed;
        }

        const std::uint64_t t_done = Timer::now();
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
    std::printf("unknown type        %s\n", humanize(unknown).c_str());
    std::printf("malformed           %s\n", humanize(malformed).c_str());
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
