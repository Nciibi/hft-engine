// Diagnostic: what is the market maker actually quoting?
//
// Not part of the build. Exists because "why does the strategy never
// fill" was guessed at wrongly several times in a row, and the only
// reliable answer is to print the resting quote next to the touch.

#include <cstdio>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/strategy/market_maker.hpp"
#include "hft/types.hpp"

int main() {
    hft::feed::CaptureConfig cfg;
    cfg.record_count = 20'000;
    cfg.price_levels = 4;
    cfg.max_live_orders = 60;
    cfg.drift_raw = 200;
    hft::feed::CaptureStats st{};
    const std::vector<std::uint8_t> data = hft::feed::generate_capture(cfg, &st);

    hft::strategy::MarketMakerConfig mc;
    mc.quote.gamma = 2.5e-3;
    mc.quote.horizon_ticks = 250.0;
    mc.quote.base_size = 100;
    hft::strategy::MarketMaker mm(mc);
    hft::lob::OrderBook book(1u << 20, 1u << 16);

    std::size_t off = 0;
    std::uint64_t n = 0;
    hft::Nanos now = 1'000'000'000ULL;

    while (off < data.size() && n < 20'000) {
        if (data.size() - off < hft::feed::kCaptureSequenceSize + hft::itch::kLengthPrefixSize) {
            break;
        }
        const std::size_t f = off + hft::feed::kCaptureSequenceSize;
        const auto r = hft::itch::decode(data.data() + f, data.size() - f);
        const std::size_t stride = hft::itch::frame_stride(r);
        if (stride == 0) break;
        if (r.ok()) {
            hft::lob::apply(r.message, book);
            now += 4'000;
            ++n;
            mm.on_book(book, now);
            if (n % 2000 == 0) {
                const auto b = book.best_bid();
                const auto a = book.best_ask();
                if (b && a) {
                    std::printf(
                        "n=%6llu bestBid=%9lld bestAsk=%9lld mid=%9lld | restBid=%9lld "
                        "restAsk=%9lld sprd=%8lld pos=%6lld active=%d\n",
                        (unsigned long long)n, (long long)b->raw(), (long long)a->raw(),
                        (long long)mm.last_mid().raw(), (long long)mm.resting_bid().raw(),
                        (long long)mm.resting_ask().raw(), (long long)mm.quoted_spread_raw(),
                        (long long)mm.position(), mm.quote_active() ? 1 : 0);
                }
            }
        }
        off = f + stride;
    }
    std::printf("fills=%llu checks=%llu\n", (unsigned long long)mm.fills(),
                (unsigned long long)mm.fill_checks());
    return 0;
}
