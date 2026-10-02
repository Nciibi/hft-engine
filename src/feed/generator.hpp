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
#include <string>
#include <vector>

#include "hft/itch/moldudp64.hpp"
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
    /// Number of price levels per side around the mid.
    std::size_t price_levels = 32;
    /// Spacing between price levels, in raw units. This is the tick
    /// the book is built on, and it is also the level spacing.
    ///
    /// The default is $0.01, the US equity tick. Level spacing and tick
    /// are the same number here because a real book rests orders on tick
    /// sizes; separating them would model a venue that quotes inside its
    /// own tick.
    std::int64_t tick_raw = 100;
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
/// Writes a 2-byte big-endian length prefix of 36, the 'A' tag, and the
/// body exactly as laid out in `hft/itch/protocol.hpp`. Exposed
/// separately from the bulk generator so tests can construct precise
/// frames, including deliberately malformed ones.
///
/// `symbol` is the eight-byte stock symbol field, which sits between the
/// share count and the price in the wire layout. It defaults to a fixed
/// single name because most callers want one symbol and should not have
/// to spell it out. The literal is exactly eight characters plus a
/// terminator, matching the field width.
void append_add_order(std::vector<std::uint8_t>& out, const hft::Side side,
                      hft::Price price, hft::Quantity size, hft::OrderId id,
                      hft::Nanos timestamp, hft::StockLocate locate = 1,
                      hft::TrackingNumber tracking = 0,
                      const char* symbol = "SIMTEST ") noexcept;

void append_order_cancel(std::vector<std::uint8_t>& out, hft::OrderId id, hft::Quantity shares,
                         hft::Nanos timestamp, hft::StockLocate locate = 1,
                         hft::TrackingNumber tracking = 0) noexcept;

void append_order_delete(std::vector<std::uint8_t>& out, hft::OrderId id, hft::Nanos timestamp,
                         hft::StockLocate locate = 1,
                         hft::TrackingNumber tracking = 0) noexcept;

void append_order_executed(std::vector<std::uint8_t>& out, hft::OrderId id, hft::Quantity shares,
                           hft::Nanos timestamp, std::uint64_t match_number = 0,
                           hft::StockLocate locate = 1, hft::TrackingNumber tracking = 0) noexcept;

void append_order_executed_at_price(std::vector<std::uint8_t>& out, hft::OrderId id,
                                    hft::Quantity shares, hft::Price execution_price,
                                    hft::Nanos timestamp, std::uint64_t match_number = 0,
                                    hft::StockLocate locate = 1,
                                    hft::TrackingNumber tracking = 0) noexcept;

/// Order Replace ('U'). Emitted only to exercise the decoder's
/// skip-by-length path; this build deliberately does not decode it, so
/// this is the emitter's business and not the decoder's.
void append_order_replace(std::vector<std::uint8_t>& out, hft::OrderId original, hft::OrderId next,
                          hft::Quantity shares, hft::Price price, hft::Nanos timestamp,
                          hft::StockLocate locate = 1,
                          hft::TrackingNumber tracking = 0) noexcept;

/// Append a spec-conformant MoldUDP64 Downstream Packet holding one
/// already-framed ITCH message.
///
/// `sequence` is the sequence number of the FIRST -- and here only --
/// message in the packet, so for a one-message packet it is that
/// message's number. Packing several messages per packet is the
/// generator's call, not the framing's: the sequence field applies to
/// the first block and the rest are implicitly sequential.
///
/// `session` must be exactly ten characters. The field is a fixed ten
/// bytes and the specification does not define a padding rule, so a
/// short name is a build error rather than a guess about what the venue
/// would have sent.
void append_downstream_packet(std::vector<std::uint8_t>& out, const char (&session)[11],
                              std::uint64_t sequence, const std::uint8_t* body,
                              std::size_t body_size, std::uint16_t message_count = 1) noexcept;

/// Write a raw 2-byte length prefix followed by `body`.
void append_frame(std::vector<std::uint8_t>& out, const std::uint8_t* body,
                  std::size_t body_size) noexcept;

/// Write a frame whose declared length disagrees with its body. Used
/// only by decoder tests.
void append_frame_with_length(std::vector<std::uint8_t>& out, const std::uint8_t* body,
                              std::size_t body_size,
                              std::uint16_t declared_length) noexcept;

/// Generate `config.message_count` Add Order frames. Reserves the
/// output up front so the generator itself is not the thing being
/// measured when it is timed.
std::vector<std::uint8_t> generate_add_orders(const GeneratorConfig& config);

// ---- Capture format -------------------------------------------------

/// A capture is a stream of MoldUDP64 Downstream Packets.
///
/// Each packet is a 20-byte header followed by a sequence of Message
/// Blocks, and each block is `[2-byte length][ITCH body]`:
///
///     [20 bytes  MoldUDP64 Downstream Packet Header]
///     [ 2 bytes  block length][N bytes  ITCH body]
///     [ 2 bytes  block length][N bytes  ITCH body]
///     ... `messages_per_packet` blocks in total
///
/// The header's 8-byte Sequence Number applies to the FIRST message in
/// the packet; the rest are implicitly sequential, per the
/// specification. Read it with `hft::itch::mold::parse_header` and walk
/// the blocks with `hft::itch::mold::MessageBlocks` -- which hands each
/// block back as a complete ITCH frame, because a Message Block and an
/// frame have the same layout.
///
/// The format CHANGED. It used to be one 4-byte sequence number per
/// record, which is not a wire format at all: it carried a sequence
/// number MoldUDP64 does not put there, at a width MoldUDP64 does not
/// use, with no packet boundaries and no heartbeats. The old layout is
/// retained below as a documented constant only so the difference is
/// visible in the diff.
///
/// There is still no checksum, and that is correct: MoldUDP64 does not
/// checksum packets. Integrity belongs to SOUP, one protocol up, and
/// its field table has not been verified here.
///
/// `messages_per_packet` exists because it is a real variable, not a
/// cosmetic one. One message per packet is what a naive implementation
/// does and it makes every message pay a 20-byte header; a hundred per
/// packet is what a real venue sends. Nothing else in this repository
/// changes behaviour between the two, which is itself worth being able
/// to say.
inline constexpr std::size_t kCaptureHeaderSize = hft::itch::mold::kHeaderSize;

/// Iterates the frames of a capture without allocating.
///
/// Every consumer in this repository used to hand-roll the walk, which
/// meant the packet format was re-implemented six times and a change to
/// it was six changes. This is the one walk.
///
/// It reads PACKETS and yields FRAMES, because that is the shape of the
/// problem: a handler is handed packets, and the sequence number lives
/// on the packet while the frames live in its blocks.
///
/// Diagnostics are collected as a side effect rather than asked for, so
/// a consumer that forgets to check them still cannot silently ignore a
/// truncated packet. `malformed()` is the one to assert on.
class CaptureReader final {
public:
    CaptureReader(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    /// Next ITCH frame, or false at the end of the capture.
    ///
    /// The returned pointer is a complete frame -- length prefix
    /// included -- so it can be handed straight to `itch::decode`.
    /// `sequence()` is the sequence number of the message just returned,
    /// which for a multi-message packet differs per frame even though
    /// the packet header carries only the first.
    [[nodiscard]] bool next(const std::uint8_t*& frame, std::size_t& frame_size) noexcept {
        namespace mold = hft::itch::mold;
        if (remaining_blocks_ == 0 && !load_packet()) {
            return false;
        }
        if (cursor_ + mold::kMessageBlockSize > size_) {
            truncated_ = true;
            return false;
        }
        const std::size_t length = (static_cast<std::size_t>(data_[cursor_]) << 8) |
                                   static_cast<std::size_t>(data_[cursor_ + 1]);
        if (cursor_ + mold::kMessageBlockSize + length > size_) {
            truncated_ = true;
            return false;
        }
        frame = data_ + cursor_;
        frame_size = mold::kMessageBlockSize + length;
        sequence_ = packet_sequence_ + block_index_;
        ++block_index_;
        --remaining_blocks_;
        cursor_ += frame_size;
        return true;
    }

    /// Sequence number of the frame most recently returned.
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] std::uint64_t packets() const noexcept { return packets_; }
    [[nodiscard]] std::uint64_t heartbeats() const noexcept { return heartbeats_; }
    [[nodiscard]] std::uint64_t end_of_session() const noexcept { return end_of_session_; }

    /// True when a packet header or a block ran past the end of the
    /// capture.
    ///
    /// A half-written file is a real thing -- a capture copied while the
    /// generator was still writing it -- and it reads as a clean
    /// shorter stream unless something notices. Every tool that consumes
    /// a capture asserts on this.
    [[nodiscard]] bool malformed() const noexcept { return truncated_; }

private:
    /// Advance to the next packet that actually carries messages.
    ///
    /// Heartbeats and end-of-session packets carry none, so they are
    /// counted and stepped over rather than returned: yielding them
    /// would give a caller a "frame" with no frame in it.
    [[nodiscard]] bool load_packet() noexcept {
        namespace mold = hft::itch::mold;
        for (;;) {
            if (cursor_ + mold::kHeaderSize > size_) {
                if (cursor_ < size_) {
                    truncated_ = true;
                }
                return false;
            }
            hft::itch::mold::PacketHeader header{};
            (void)hft::itch::mold::parse_header(data_ + cursor_, size_ - cursor_, header);
            cursor_ += mold::kHeaderSize;
            ++packets_;
            packet_sequence_ = header.sequence;

            if (hft::itch::mold::is_end_of_session(header.count)) {
                ++end_of_session_;
                return false;
            }
            if (hft::itch::mold::is_heartbeat(header.count)) {
                ++heartbeats_;
                continue;
            }
            remaining_blocks_ = header.count;
            block_index_ = 0;
            return remaining_blocks_ > 0;
        }
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 0;
    std::uint16_t remaining_blocks_ = 0;
    std::uint64_t block_index_ = 0;
    std::uint64_t packet_sequence_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t packets_ = 0;
    std::uint64_t heartbeats_ = 0;
    std::uint64_t end_of_session_ = 0;
    bool truncated_ = false;
};

/// A mixed feed: adds interleaved with the cancels, executes and
/// deletes that actually remove them.
///
/// The generator tracks its own live orders so every mutation it emits
/// refers to a real, still-resting order. A feed that emits cancels for
/// orders that were never added would exercise only the rejection
/// path, and a book that never loses an order is not a book under
/// load.
struct CaptureConfig {
    std::uint64_t seed = 0xC0FF'EE00'1234'5678ULL;
    std::size_t record_count = 1'000'000;
    /// Price levels per side.
    std::size_t price_levels = 32;
    /// Spacing between levels, in raw units ($0.01).
    std::int64_t tick_raw = 100;
    std::int64_t anchor_raw = 1'000'000;
    std::int64_t drift_raw = 60;
    std::int64_t reversion = 64;
    OrderId first_order_id = 1'000'000;
    /// Starting SOUP sequence number. Chosen near the 32-bit wrap so
    /// the replay path exercises wrap handling by default.
    std::uint32_t first_sequence = 0xFFFF'F000u;
    uint32_t min_shares = 1;
    uint32_t max_shares = 500;
    /// Probability weights, per cent. A live book is dominated by adds
    /// and deletes, with fewer partial cancels and fills.
    uint32_t pct_add = 60;
    uint32_t pct_execute = 15;
    uint32_t pct_cancel = 15;
    /// Remainder is delete.
    /// Cap on simultaneously live orders. Zero means unbounded.
    ///
    /// This is what makes the book REPRICE, and it is not a
    /// performance limit. A book that only ever accumulates never
    /// clears a price level, so the best bid and ask are set once and
    /// frozen: an unbounded run of 40,000 records moved its mid 24
    /// times, because roughly 24,000 adds against 16,000 removals
    /// across 32 levels means every level grows and none ever empties.
    /// A price that does not move is not a market, and a strategy can
    /// only be studied on one that does.
    ///
    /// Leave at zero for a deep static book, which is what the
    /// throughput benchmark wants. Set it for anything that studies
    /// behaviour over time.
    std::size_t max_live_orders = 0;

    /// Probability that a mutation targets a RECENTLY added order
    /// rather than a uniformly random one.
    ///
    /// Real order flow is not uniform across price levels. It clusters
    /// hard at the touch, because that is where market orders and
    /// aggressive quotes go. An earlier revision picked mutations
    /// uniformly from the live set, which made the book's extremes
    /// almost immortal: a level that held one of the first few orders
    /// was never the one randomly cancelled, so the best bid and ask
    /// were set in the first few hundred messages and never moved
    /// again. The mid was therefore constant, volatility was zero, and
    /// a market maker had nothing to price. A feed whose price never
    /// moves cannot be used to study a strategy that trades it.
    double pct_recent = 0.75;
    /// Emit Order Replace ('U') records. They are structurally valid
    /// but this build does not decode them, so they exercise the
    /// skip-by-length path. Off by default, so a clean replay is the
    /// default and the skip path is opted into deliberately.
    bool inject_order_replace = false;

    /// Number of distinct stock symbols to interleave.
    ///
    /// One is the original single-instrument behaviour and is the
    /// default. More than one produces a capture where a single handler
    /// must maintain several books at once, which is the only way to
    /// exercise routing: every symbol gets its own price walk, its own
    /// live-order set, and its own anchor price, and the records are
    /// interleaved in one sequence.
    ///
    /// The anchors are spaced far enough apart that a routing bug shows
    /// up as a book priced in another symbol's currency rather than as a
    /// subtly wrong level.
    std::size_t symbol_count = 1;

    /// Messages per MoldUDP64 packet in the generated capture.
    ///
    /// One is the naive layout and is the default because it makes a
    /// packet boundary visible at every message, which is the worst
    /// case for a reader and therefore the best one for a test. A real
    /// venue batches; the replay tool reports the count so a capture
    /// cannot be mistaken for a wire capture it is not.
    std::size_t messages_per_packet = 1;

    /// Session identifier in the packet header. Exactly ten characters.
    ///
    /// The field is ten bytes and the specification defines no padding
    /// rule for a short name, so a caller that needs a different
    /// session must supply exactly ten. `append_downstream_packet`
    /// takes it as a `char[11]` reference, which makes a wrong length a
    /// compile error rather than a ten-byte field with a short name in
    /// it.
    const char* session = "SAMPLE0000";
};

/// The default session, as a fixed-width literal for the generator.
inline constexpr char kDefaultSession[11] = "SAMPLE0000";

static_assert(sizeof(kDefaultSession) - 1 == hft::itch::mold::kSessionSize,
              "a MoldUDP64 session identifier is exactly ten bytes");

/// Eight-character symbol name for index `i`, as it appears on the wire.
///
/// Deterministic, and exactly the field width: `SYM` plus five digits.
/// A short name would leave the space padding untested, and a name that
/// overflowed eight bytes would silently truncate in a way that still
/// decoded cleanly.
[[nodiscard]] inline std::string symbol_name(std::size_t i) {
    std::string s = "SYM00000";
    for (int d = 7; d >= 3; --d) {
        s[static_cast<std::size_t>(d)] = static_cast<char>('0' + static_cast<int>(i % 10));
        i /= 10;
    }
    return s;
}

/// Per-record counts, so a caller can assert the generated mix rather
/// than trusting it.
struct CaptureStats {
    std::size_t adds = 0;
    std::size_t executes = 0;
    std::size_t cancels = 0;
    std::size_t deletes = 0;
    std::size_t order_replace = 0;
    std::size_t records = 0;
    /// Distinct symbols emitted. Always 1 unless `symbol_count` asked
    /// for more, and printed so a caller can assert the capture is the
    /// shape it meant to generate rather than the shape it assumed.
    std::size_t symbols = 1;
};

std::vector<std::uint8_t> generate_capture(const CaptureConfig& config,
                                           CaptureStats* stats = nullptr);

}  // namespace hft::feed
