// Synthetic ITCH feed generator.
//
// TotalView-ITCH is a licensed product. A project that depends on a
// data agreement cannot be built by a reviewer, cannot be rebuilt in
// CI, and cannot be run on a rented benchmark host. This generator
// emits byte-conformant frames from a seeded PRNG so that the whole
// pipeline is reproducible and self-contained.
//
// The generated feed is not random noise. It walks a price with a
// drift term, emits adds and cancels in a realistic ratio, and keeps
// the book populated, because a benchmark run against a book that is
// always empty measures the loop rather than the data structure.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hft/types.hpp"

namespace hft::feed {

/// SplitMix64. Chosen over std::mt19937 because it is small, has no
/// platform-dependent implementation, and produces an identical stream
/// on MSVC and GCC. A benchmark that cannot reproduce its own input
/// data on the benchmarking host is not reproducible.
class SplitMix64 final {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t next() noexcept {
        state_ += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    /// Unbiased value in [0, bound). Rejection sampling, because a
    /// plain modulo biases the low end of the range and a biased feed
    /// would put a predictable structure into the benchmark.
    [[nodiscard]] constexpr std::uint64_t below(std::uint64_t bound) noexcept {
        if (bound <= 1) {
            return 0;
        }
        const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % bound) - 1;
        std::uint64_t r = next();
        while (r > limit) {
            r = next();
        }
        return r % bound;
    }

private:
    std::uint64_t state_;
};

struct GeneratorConfig {
    std::uint64_t seed = 0x5EED'1234'ABCD'0001ULL;
    /// Number of Add Order frames to emit.
    std::size_t message_count = 1'000'000;
    /// Price levels spanned around the mid. The book is built inside
    /// this window, so depth and width scale together.
    std::size_t price_levels = 64;
    /// Half-width of the initial mid, in raw 1/10000 units.
    std::int64_t half_spread_raw = 5'000;  // $0.50
    /// Per-step random walk magnitude on the mid, raw units.
    std::int64_t drift_raw = 250;  // $0.025
    /// Mid price the walk reverts toward, raw units ($100.00).
    std::int64_t anchor_raw = 1'000'000;
    /// Each step, move 1/reversion of the way back to the anchor.
    /// A pure random walk with no reversion invents a new price every
    /// few hundred messages, exhausts the book's price level pool, and
    /// turns a benchmark into a measurement of the rejection path.
    /// Real instruments mean-revert over a trading session, and so does
    /// this one.
    std::int64_t reversion = 64;
    /// Starting order reference. ITCH references are day-unique and
    /// nonzero.
    OrderId first_order_id = 1'000'000;
    /// Share sizes are drawn from this range.
    std::uint32_t min_shares = 1;
    std::uint32_t max_shares = 500;
};

/// Append a spec-conformant Add Order frame to `out`.
///
/// Writes a 2-byte big-endian length prefix of 32, the 'A' tag, and
/// the body exactly as laid out in `hft/itch/protocol.hpp`. Exposed
/// separately from the bulk generator so tests can construct precise
/// frames, including deliberately malformed ones.
void append_add_order(std::vector<std::uint8_t>& out, const hft::Side side,
                      hft::Price price, hft::Quantity size, hft::OrderId id,
                      hft::Nanos timestamp, hft::StockLocate locate = 1,
                      hft::TrackingNumber tracking = 0) noexcept;

/// Write a raw 2-byte length prefix followed by `body`.
void append_frame(std::vector<std::uint8_t>& out, const std::uint8_t* body,
                  std::size_t body_size) noexcept;

/// Write a frame whose declared length disagrees with its body. Used
/// only by decoder tests.
void append_frame_with_length(std::vector<std::uint8_t>& out,
                              const std::uint8_t* body, std::size_t body_size,
                              std::uint16_t declared_length) noexcept;

/// Generate `config.message_count` Add Order frames. Reserves the
/// output up front so the generator itself is not the thing being
/// measured when it is timed.
std::vector<std::uint8_t> generate_add_orders(const GeneratorConfig& config);

}  // namespace hft::feed
