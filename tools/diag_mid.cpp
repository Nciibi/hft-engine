// Diagnostic: is the book actually repricing?
//
// Not part of the build. Exists because an inferred diagnosis of "the
// mid never moves" was wrong twice in a row, and the only way to settle
// it is to print the thing.

#include <cstdio>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/types.hpp"

int main() {
    hft::feed::CaptureConfig cfg;
    cfg.record_count = 40'000;
    hft::feed::CaptureStats st{};
    const std::vector<std::uint8_t> data = hft::feed::generate_capture(cfg, &st);
    std::printf("records=%zu adds=%zu exec=%zu cxl=%zu del=%zu\n", st.records, st.adds,
                st.executes, st.cancels, st.deletes);

    hft::lob::OrderBook book(1u << 20, 1u << 16);
    std::size_t off = 0;
    std::uint64_t n = 0;
    std::int64_t prev_mid = 0;
    std::uint64_t changes = 0;
    std::int64_t lo = 0, hi = 0;

    while (off < data.size() && n < 40'000) {
        if (data.size() - off < hft::feed::kCaptureSequenceSize + hft::itch::kLengthPrefixSize) {
            break;
        }
        const std::size_t f = off + hft::feed::kCaptureSequenceSize;
        const auto r = hft::itch::decode(data.data() + f, data.size() - f);
        const std::size_t stride = hft::itch::frame_stride(r);
        if (stride == 0) break;
        if (r.ok()) {
            hft::lob::apply(r.message, book);
            ++n;
            const auto b = book.best_bid();
            const auto a = book.best_ask();
            if (b && a) {
                const std::int64_t mid = (b->raw() + a->raw()) / 2;
                if (n <= 15) {
                    std::printf("n=%4llu bid=%8lld ask=%8lld mid=%8lld  bLvl=%u aLvl=%u\n",
                                (unsigned long long)n, (long long)b->raw(), (long long)a->raw(),
                                (long long)mid, book.level_count(hft::Side::bid),
                                book.level_count(hft::Side::ask));
                }
                if (prev_mid != 0 && mid != prev_mid) ++changes;
                if (n == 1) { lo = hi = mid; }
                if (mid < lo) lo = mid;
                if (mid > hi) hi = mid;
                prev_mid = mid;
            }
        }
        off = f + stride;
    }
    std::printf("mid changes: %llu / %llu\n", (unsigned long long)changes,
                (unsigned long long)n);
    std::printf("mid range: %lld .. %lld  (span %lld)\n", (long long)lo, (long long)hi,
                (long long)(hi - lo));
    return 0;
}
