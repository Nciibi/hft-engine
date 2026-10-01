#include "feed/generator.hpp"

#include "hft/itch/protocol.hpp"

namespace hft::feed {
namespace {

void write_be16(std::vector<std::uint8_t>& out, std::uint16_t v) noexcept {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void write_be32(std::vector<std::uint8_t>& out, std::uint32_t v) noexcept {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void write_be64(std::vector<std::uint8_t>& out, std::uint64_t v) noexcept {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

/// Write ITCH's split 48-bit timestamp: 2 high bytes then 4 low bytes.
void write_timestamp48(std::vector<std::uint8_t>& out, std::uint64_t nanos) noexcept {
    write_be16(out, static_cast<std::uint16_t>((nanos >> 32) & 0xFFFFu));
    write_be32(out, static_cast<std::uint32_t>(nanos & 0xFFFF'FFFFu));
}

}  // namespace

void append_frame(std::vector<std::uint8_t>& out, const std::uint8_t* body,
                  std::size_t body_size) noexcept {
    write_be16(out, static_cast<std::uint16_t>(body_size));
    out.insert(out.end(), body, body + body_size);
}

void append_frame_with_length(std::vector<std::uint8_t>& out, const std::uint8_t* body,
                              std::size_t body_size,
                              std::uint16_t declared_length) noexcept {
    write_be16(out, declared_length);
    out.insert(out.end(), body, body + body_size);
}

void append_add_order(std::vector<std::uint8_t>& out, const hft::Side side,
                      hft::Price price, hft::Quantity size, hft::OrderId id,
                      hft::Nanos timestamp, hft::StockLocate locate,
                      hft::TrackingNumber tracking) noexcept {
    using namespace hft::itch;

    // The length prefix counts the tag, so it is 32, not 31.
    write_be16(out, static_cast<std::uint16_t>(off::kAddOrderSize));
    out.push_back(static_cast<std::uint8_t>(MessageType::add_order));
    write_be16(out, locate);
    write_be16(out, tracking);
    write_timestamp48(out, timestamp);
    write_be64(out, id);
    out.push_back(side == hft::Side::bid ? static_cast<std::uint8_t>('B')
                                         : static_cast<std::uint8_t>('S'));
    write_be32(out, static_cast<std::uint32_t>(price.raw()));
    write_be32(out, static_cast<std::uint32_t>(size.raw()));
    out.push_back('2');  // Order Type: Limit
    out.push_back('0');  // Time in Force: Immediate or Cancel
    out.push_back('1');  // Displayed
    out.push_back('N');  // Participant: NSDQ
}

std::vector<std::uint8_t> generate_add_orders(const GeneratorConfig& config) {
    namespace ic = hft::itch;

    std::vector<std::uint8_t> out;
    out.reserve(config.message_count * ic::frame_size(ic::off::kAddOrderSize));

    SplitMix64 rng(config.seed);

    // A negative drift or half-spread would make the level-offset
    // arithmetic produce prices at or below zero, which the walker
    // would then silently skip. Clamp once, loudly, rather than let a
    // config mistake quietly shrink the feed.
    const std::int64_t drift_raw = config.drift_raw < 0 ? 0 : config.drift_raw;
    const std::int64_t half_spread_raw = config.half_spread_raw < 0 ? 1 : config.half_spread_raw;
    const std::int64_t reversion = config.reversion < 1 ? 1 : config.reversion;

    const std::int64_t levels = static_cast<std::int64_t>(config.price_levels);
    const std::int64_t half = levels / 2;
    const std::int64_t anchor = config.anchor_raw;
    std::int64_t mid = anchor;
    OrderId next_id = config.first_order_id;
    std::uint64_t clock = 0;

    for (std::size_t i = 0; i < config.message_count; ++i) {
        // Random walk the mid, then pull it back toward the anchor.
        // Without the pull-back the walk random-walks to a new price
        // region and the book's price level pool is exhausted long
        // before the feed is.
        const std::int64_t step =
            static_cast<std::int64_t>(rng.below(2 * static_cast<std::uint64_t>(drift_raw) + 1)) -
            drift_raw;
        mid += step;
        mid += (anchor - mid) / reversion;

        // Choose a level offset within the window, then a side. Keeping
        // the side choice independent of the offset gives a book with
        // both sides populated at every level.
        const std::int64_t offset = static_cast<std::int64_t>(rng.below(
                                       static_cast<std::uint64_t>(levels))) -
                                    half;
        const hft::Side side = (rng.next() & 1u) == 0u ? hft::Side::bid : hft::Side::ask;

        const std::int64_t raw_price = mid + offset * half_spread_raw;
        if (raw_price <= 0) {
            continue;
        }

        const std::uint32_t span = config.max_shares - config.min_shares + 1;
        const std::uint32_t shares =
            config.min_shares + static_cast<std::uint32_t>(rng.below(span));

        // Advance a nanosecond clock. Real ITCH timestamps come from
        // the venue; synthesising monotonic ones is enough to exercise
        // the 48-bit reassembly path.
        clock += 1 + rng.below(4'000);

        append_add_order(out, side, hft::Price::from_raw(raw_price),
                         hft::Quantity::from_raw(shares), next_id, clock,
                         /*locate=*/1, /*tracking=*/static_cast<hft::TrackingNumber>(i & 0xFFFFu));
        ++next_id;
    }
    return out;
}

}  // namespace hft::feed
