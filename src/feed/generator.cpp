#include "feed/generator.hpp"

#include <algorithm>

#include "hft/itch/moldudp64.hpp"
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

/// The symbol every generated record carries when none is specified.
///
/// Eight bytes, space padded, because that is the field width the Add
/// Order message specifies and because a short symbol makes the padding
/// rule untested -- a truncated symbol would decode as a different
/// string without ever being wrong about the bytes.
inline constexpr char kGeneratedSymbol[] = "SIMTEST ";

/// Write an eight-byte space-padded alpha field from an 8-character
/// name.
///
/// The name is copied as-is, with NO truncation and NO padding: a
/// caller that passes a nine-character string gets a build-time check
/// rather than a silently shortened symbol, because two symbols that
/// truncate to the same eight bytes route to the same book and the bug
/// would only appear as an order arriving on the wrong shard.
template <std::size_t N>
void write_stock_symbol(std::vector<std::uint8_t>& out, const char (&symbol)[N]) noexcept {
    static_assert(N == 9,
                  "an ITCH stock symbol is exactly 8 characters plus a terminator; a name of "
                  "another length is padded or truncated by this function and that is a bug, "
                  "not a convenience");
    static_assert(N - 1 == hft::itch::off::kStockSymbolSize,
                  "the symbol literal must fill the stock field exactly");
    for (std::size_t i = 0; i < hft::itch::off::kStockSymbolSize; ++i) {
        out.push_back(static_cast<std::uint8_t>(symbol[i]));
    }
}

/// Overload for a runtime symbol pointer, length-checked by the caller.
void write_stock_symbol(std::vector<std::uint8_t>& out, const char* symbol) noexcept {
    for (std::size_t i = 0; i < hft::itch::off::kStockSymbolSize; ++i) {
        out.push_back(static_cast<std::uint8_t>(symbol[i]));
    }
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

void append_downstream_packet(std::vector<std::uint8_t>& out, const char (&session)[11],
                              std::uint64_t sequence, const std::uint8_t* body,
                              std::size_t body_size, std::uint16_t message_count) noexcept {
    using namespace hft::itch::mold;

    // No checksum, deliberately. MoldUDP64 is an unreliable transport
    // and does not checksum its packets; integrity is SOUP's job, one
    // layer up. Writing a CRC here would be inventing a field the
    // specification does not define.

    // Session, fixed ten bytes, written verbatim. No padding rule is
    // defined for a short name, so the caller's ten characters are the
    // ten bytes.
    for (std::size_t i = 0; i < kSessionSize; ++i) {
        out.push_back(static_cast<std::uint8_t>(session[i]));
    }

    // Eight bytes, big endian. Writing the full 64 bits rather than
    // truncating to 32 is the whole point: a gap check that reads four
    // bytes of this field ignores the high half of every sequence
    // number.
    write_be64(out, sequence);
    write_be16(out, message_count);

    // The Message Block length EXCLUDES its own two bytes, so a block
    // occupies length + 2 bytes. Getting that backwards produces a
    // packet that parses one block short and then reads the next
    // packet's header as message data.
    write_be16(out, static_cast<std::uint16_t>(body_size));
    out.insert(out.end(), body, body + body_size);
}

void append_add_order(std::vector<std::uint8_t>& out, const hft::Side side,
                      hft::Price price, hft::Quantity size, hft::OrderId id,
                      hft::Nanos timestamp, hft::StockLocate locate,
                      hft::TrackingNumber tracking, const char* symbol) noexcept {
    using namespace hft::itch;

    // The length prefix counts the tag, so it is 36, not 35.
    write_be16(out, static_cast<std::uint16_t>(off::kAddOrderSize));
    out.push_back(static_cast<std::uint8_t>(MessageType::add_order));
    write_be16(out, locate);
    write_be16(out, tracking);
    write_timestamp48(out, timestamp);
    write_be64(out, id);
    out.push_back(side == hft::Side::bid ? static_cast<std::uint8_t>('B')
                                         : static_cast<std::uint8_t>('S'));
    // Shares, then the eight-byte stock symbol, then the price. The
    // order of the first two is the opposite of what this function used
    // to write, which is the whole of the Add Order bug: it agreed with
    // the decoder and disagreed with the specification.
    write_be32(out, static_cast<std::uint32_t>(size.raw()));
    write_stock_symbol(out, symbol);
    write_be32(out, static_cast<std::uint32_t>(price.raw()));
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
    const std::int64_t tick = config.tick_raw < 1 ? 1 : config.tick_raw;
    const std::int64_t reversion = config.reversion < 1 ? 1 : config.reversion;

    const std::size_t levels = config.price_levels == 0 ? 1 : config.price_levels;
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

        const hft::Side side = (rng.next() & 1u) == 0u ? hft::Side::bid : hft::Side::ask;

        // Bids strictly BELOW the mid, asks strictly ABOVE it. See the
        // long note in generate_capture: choosing the side and the
        // offset independently produces overlapping sides and a book
        // whose mid never moves.
        const std::int64_t offset =
            1 + static_cast<std::int64_t>(rng.below(static_cast<std::uint64_t>(levels)));
        const std::int64_t raw_price =
            side == hft::Side::bid ? mid - offset * tick : mid + offset * tick;
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

void append_order_cancel(std::vector<std::uint8_t>& out, hft::OrderId id, hft::Quantity shares,
                         hft::Nanos timestamp, hft::StockLocate locate,
                         hft::TrackingNumber tracking) noexcept {
    using namespace hft::itch;
    write_be16(out, static_cast<std::uint16_t>(off::kOrderCancelSize));
    out.push_back(static_cast<std::uint8_t>(MessageType::order_cancel));
    write_be16(out, locate);
    write_be16(out, tracking);
    write_timestamp48(out, timestamp);
    write_be64(out, id);
    write_be32(out, static_cast<std::uint32_t>(shares.raw()));
}

void append_order_delete(std::vector<std::uint8_t>& out, hft::OrderId id, hft::Nanos timestamp,
                         hft::StockLocate locate, hft::TrackingNumber tracking) noexcept {
    using namespace hft::itch;
    write_be16(out, static_cast<std::uint16_t>(off::kOrderDeleteSize));
    out.push_back(static_cast<std::uint8_t>(MessageType::order_delete));
    write_be16(out, locate);
    write_be16(out, tracking);
    write_timestamp48(out, timestamp);
    write_be64(out, id);
}

void append_order_executed(std::vector<std::uint8_t>& out, hft::OrderId id, hft::Quantity shares,
                           hft::Nanos timestamp, std::uint64_t match_number,
                           hft::StockLocate locate, hft::TrackingNumber tracking) noexcept {
    using namespace hft::itch;
    write_be16(out, static_cast<std::uint16_t>(off::kOrderExecutedSize));
    out.push_back(static_cast<std::uint8_t>(MessageType::order_executed));
    write_be16(out, locate);
    write_be16(out, tracking);
    write_timestamp48(out, timestamp);
    write_be64(out, id);
    write_be32(out, static_cast<std::uint32_t>(shares.raw()));
    write_be64(out, match_number);
    out.push_back('Y');  // printable
}

void append_order_executed_at_price(std::vector<std::uint8_t>& out, hft::OrderId id,
                                    hft::Quantity shares, hft::Price execution_price,
                                    hft::Nanos timestamp, std::uint64_t match_number,
                                    hft::StockLocate locate, hft::TrackingNumber tracking) noexcept {
    using namespace hft::itch;
    write_be16(out, static_cast<std::uint16_t>(off::kOrderExecutedAtPriceSize));
    out.push_back(static_cast<std::uint8_t>(MessageType::order_executed_at_price));
    write_be16(out, locate);
    write_be16(out, tracking);
    write_timestamp48(out, timestamp);
    write_be64(out, id);
    write_be32(out, static_cast<std::uint32_t>(shares.raw()));
    write_be64(out, match_number);
    out.push_back('Y');  // printable
    write_be32(out, static_cast<std::uint32_t>(execution_price.raw()));
}

/// Order Replace ('U'). Emitted only to exercise the decoder's
/// skip-by-length path; this build deliberately does not decode it.
void append_order_replace(std::vector<std::uint8_t>& out, hft::OrderId original, hft::OrderId next,
                          hft::Quantity shares, hft::Price price, hft::Nanos timestamp,
                          hft::StockLocate locate, hft::TrackingNumber tracking) noexcept {
    using namespace hft::itch;
    // Body: tag(1) + locate(2) + track(2) + ts(6) + original(8)
    //     + new(8) + shares(4) + price(4) + type(1) + tif(1)
    //     + display(1) + participant(1) = 39
    constexpr std::uint16_t kReplaceBodySize = 39;
    write_be16(out, kReplaceBodySize);
    out.push_back(static_cast<std::uint8_t>(MessageType::order_replace));
    write_be16(out, locate);
    write_be16(out, tracking);
    write_timestamp48(out, timestamp);
    write_be64(out, original);
    write_be64(out, next);
    write_be32(out, static_cast<std::uint32_t>(shares.raw()));
    write_be32(out, static_cast<std::uint32_t>(price.raw()));
    out.push_back('2');
    out.push_back('0');
    out.push_back('1');
    out.push_back('N');
}

namespace {

/// Buffers one packet's worth of messages and flushes it as a
/// MoldUDP64 Downstream Packet.
///
/// A packet cannot be written as it is assembled, because the header
/// carries the message count and the sequence number of its first
/// message, neither of which is known until the packet is full. So the
/// blocks are collected and the header is written in front of them.
///
/// Flushing a short packet at the end of the stream matters: a tail left
/// unflushed is a silent truncation, and the only symptom would be a
/// checksum mismatch much later.
class PacketWriter final {
public:
    PacketWriter(std::vector<std::uint8_t>& out, const char* session, std::size_t per_packet,
                 std::uint64_t first_sequence)
        : out_(out), session_(session), per_packet_(per_packet == 0 ? 1 : per_packet),
          next_sequence_(first_sequence) {}

    void append(const std::vector<std::uint8_t>& frame) {
        if (frames_.empty()) {
            packet_first_sequence_ = next_sequence_;
        }
        frames_.push_back(frame);
        ++next_sequence_;
        if (frames_.size() >= per_packet_) {
            flush();
        }
    }

    /// Emit whatever is left, even if it is a partial packet.
    ///
    /// Idempotent: flushing an empty buffer does nothing, so calling
    /// this twice is harmless. That matters because the destructor also
    /// flushes.
    void finish() { flush(); }

    /// A tail packet is flushed here rather than at a call site.
    ///
    /// This class had a `finish()` that nothing called, and the symptom
    /// was that the last partial packet was silently dropped: 1,000
    /// messages at 7 per packet produced 994 frames, not 1,000. Nothing
    /// reported an error, because from every reader's point of view the
    /// stream simply ended -- earlier than it should have, and in a way
    /// that looked intentional. A missing `finish()` call is a bug that
    /// costs six messages and explains itself as nothing at all.
    ~PacketWriter() { flush(); }

    [[nodiscard]] std::uint64_t packets() const noexcept { return packets_; }

private:
    void flush() {
        if (frames_.empty()) {
            return;
        }
        // The header carries the COUNT, so the blocks cannot go first.
        // The count is the number of blocks in this packet; the sequence
        // is that of the FIRST message, and the specification says the
        // rest are implicitly sequential.
        reserve_blocks();
        std::size_t at = out_.size();
        out_.resize(at + hft::itch::mold::kHeaderSize);
        // Write the header directly into the reserved space, then the
        // blocks after it.
        write_header(at);
        for (const std::vector<std::uint8_t>& frame : frames_) {
            out_.insert(out_.end(), frame.begin(), frame.end());
        }
        frames_.clear();
        ++packets_;
    }

    void reserve_blocks() {
        std::size_t bytes = hft::itch::mold::kHeaderSize;
        for (const std::vector<std::uint8_t>& frame : frames_) {
            bytes += frame.size();
        }
        out_.reserve(out_.size() + bytes);
    }

    void write_header(std::size_t at) {
        namespace mold = hft::itch::mold;
        std::vector<std::uint8_t> header;
        header.reserve(mold::kHeaderSize);
        for (std::size_t i = 0; i < mold::kSessionSize; ++i) {
            header.push_back(static_cast<std::uint8_t>(session_[i]));
        }
        write_be64(header, packet_first_sequence_);
        write_be16(header, static_cast<std::uint16_t>(frames_.size()));
        std::copy(header.begin(), header.end(), out_.begin() + static_cast<std::ptrdiff_t>(at));
    }

    std::vector<std::uint8_t>& out_;
    const char* session_;
    std::size_t per_packet_;
    std::uint64_t next_sequence_;
    std::uint64_t packet_first_sequence_ = 0;
    std::uint64_t packets_ = 0;
    std::vector<std::vector<std::uint8_t>> frames_;
};

}  // namespace

std::vector<std::uint8_t> generate_capture(const CaptureConfig& config, CaptureStats* stats) {
    std::vector<std::uint8_t> out;
    // 20-byte packet header plus a frame, amortised over the packet.
    const std::size_t per_packet = config.messages_per_packet == 0 ? 1 : config.messages_per_packet;
    out.reserve(config.record_count * (40 + 20 / per_packet));

    SplitMix64 rng(config.seed);
    CaptureStats local{};

    const std::int64_t tick = config.tick_raw < 1 ? 1 : config.tick_raw;
    const std::int64_t drift = config.drift_raw < 0 ? 0 : config.drift_raw;
    const std::int64_t reversion = config.reversion < 1 ? 1 : config.reversion;
    const std::size_t levels = config.price_levels == 0 ? 1 : config.price_levels;

    // The mid is per symbol and lives with that symbol's live set below;
    // declaring one here would shadow it and every record would be
    // priced against symbol 0's walk.
    hft::OrderId next_id = config.first_order_id;
    PacketWriter packets(out, config.session != nullptr ? config.session : kDefaultSession,
                      config.messages_per_packet,
                      static_cast<std::uint64_t>(config.first_sequence));
    std::uint64_t clock = 0;
    std::uint64_t match_number = 0;

    // Live orders the generator believes are resting, with the size it
    // believes remains. Emitting a mutation for an order that is not
    // here would produce a feed that only ever exercises rejection.
    struct Live {
        hft::OrderId id;
        hft::Price price;
        hft::Side side;
        std::uint32_t remaining;
    };

    // One price walk and one live set per symbol.
    //
    // These are deliberately NOT shared. A single shared live set would
    // let a mutation for symbol A's order be emitted while symbol B is
    // the active book, which produces a feed where most mutations name
    // an order the receiving book has never seen -- a capture that
    // exercises only the rejection path and reports a healthy record
    // count while measuring nothing.
    struct SymbolState {
        std::int64_t mid = 0;
        std::vector<Live> live;
    };

    const std::size_t symbols = config.symbol_count == 0 ? 1 : config.symbol_count;
    std::vector<SymbolState> state(symbols);
    std::vector<std::string> names(symbols);
    for (std::size_t s = 0; s < symbols; ++s) {
        // Anchors spaced $100 apart. Far enough that a message routed to
        // the wrong book lands in a different price decade and is
        // obvious, rather than one tick away where it would look like a
        // plausible book.
        state[s].mid = config.anchor_raw + static_cast<std::int64_t>(s) * 1'000'000;
        state[s].live.reserve(config.max_live_orders != 0
                                  ? config.max_live_orders + 8
                                  : 8192);
        names[s] = symbol_name(s);
    }
    local.symbols = symbols;

    const std::uint32_t span = config.max_shares - config.min_shares + 1;

    for (std::size_t i = 0; i < config.record_count; ++i) {
        // Choose the symbol for this record, then work entirely within
        // it. Interleaving is uniform by choice: real feeds are not, and
        // a uniform mix is the harder case for a sharded handler because
        // it gives every shard an equal share rather than concentrating
        // the work on one.
        const std::size_t sym = symbols == 1 ? 0 : static_cast<std::size_t>(rng.below(symbols));
        SymbolState& sym_state = state[sym];
        std::int64_t& mid = sym_state.mid;
        std::vector<Live>& live = sym_state.live;

        const std::uint64_t roll = rng.below(100);
        // The sequence number is assigned by the PacketWriter when the
        // packet is flushed, not here, because a packet's sequence is
        // that of its FIRST message and the count is not known until
        // the packet is full. Incrementing a counter per record and
        // stamping it on each one was the old format's approach and it
        // put the sequence number where MoldUDP64 does not put it.
        ++local.records;

        const bool may_mutate = !live.empty();
        // A full live set forces a mutation. Without this the book only
        // accumulates, no price level ever empties, and the mid is
        // frozen for the whole run.
        const bool at_capacity = config.max_live_orders != 0 &&
                                 live.size() >= config.max_live_orders;
        if (!may_mutate || (!at_capacity && roll < config.pct_add)) {
            mid += static_cast<std::int64_t>(
                       rng.below(2 * static_cast<std::uint64_t>(drift) + 1)) -
                   drift;
            mid += (config.anchor_raw - mid) / reversion;

            const hft::Side side = (rng.next() & 1u) == 0u ? hft::Side::bid : hft::Side::ask;
            const std::uint32_t shares =
                config.min_shares + static_cast<std::uint32_t>(rng.below(span));

            // Bids strictly BELOW the mid and asks strictly ABOVE it.
            //
            // This is the whole definition of a book, and getting it
            // wrong is invisible until something downstream depends on
            // it. An earlier revision chose the side and the price
            // offset independently, so bids and asks were drawn from
            // the same wide band and overlapped: the best ask sat
            // $15 BELOW the best bid, the mid moved 20 times in 40,000
            // ticks, and a market maker had zero volatility to price.
            const std::int64_t offset = draw_offset(rng, levels, config);
            std::int64_t raw_price =
                side == hft::Side::bid ? mid - offset * tick : mid + offset * tick;

            // NEVER cross the book. This is an invariant, not a
            // parameter choice, and it is enforced here rather than
            // hoped for via tuning.
            //
            // The generator's mid is a random walk while the book
            // spans only `levels * tick`. Any walk wider than the book
            // guarantees a crossing: an ask placed when the mid was
            // high survives while a bid placed after it walked down
            // arrives beneath it. A crossed book cannot occur in a real
            // market, and every measurement taken from one is
            // meaningless rather than merely wrong.
            //
            // Scanning the live set is O(n) on a book capped in the
            // tens of orders. The alternative is relying on the
            // parameters happening to satisfy the invariant, which is
            // not an invariant.
            if (!live.empty()) {
                std::int64_t min_ask = 0;
                std::int64_t max_bid = 0;
                for (const Live& l : live) {
                    if (l.side == hft::Side::ask) {
                        if (min_ask == 0 || l.price.raw() < min_ask) {
                            min_ask = l.price.raw();
                        }
                    } else {
                        if (max_bid == 0 || l.price.raw() > max_bid) {
                            max_bid = l.price.raw();
                        }
                    }
                }
                if (side == hft::Side::bid) {
                    if (min_ask != 0 && raw_price >= min_ask - tick) {
                        raw_price = min_ask - tick;
                    }
                } else {
                    if (max_bid != 0 && raw_price <= max_bid + tick) {
                        raw_price = max_bid + tick;
                    }
                }
#ifdef HFT_DEBUG_CROSS
                if (side == hft::Side::bid && min_ask != 0 && raw_price >= min_ask) {
                    std::fprintf(stderr,
                                 "CROSS-ADD bid=%lld min_ask=%lld max_bid=%lld live=%zu\n",
                                 (long long)raw_price, (long long)min_ask, (long long)max_bid,
                                 live.size());
                }
                if (side == hft::Side::ask && max_bid != 0 && raw_price <= max_bid) {
                    std::fprintf(stderr,
                                 "CROSS-ADD ask=%lld max_bid=%lld min_ask=%lld live=%zu\n",
                                 (long long)raw_price, (long long)max_bid, (long long)min_ask,
                                 live.size());
                }
#endif
            }

            if (raw_price <= 0) {
                continue;  // only reachable if the mid walked to zero
            }

            const hft::Price price = hft::Price::from_raw(raw_price);
            std::vector<std::uint8_t> frame;
            frame.reserve(itch::frame_size(itch::off::kAddOrderSize));
            clock += 1 + rng.below(4'000);
            append_add_order(frame, side, price, hft::Quantity::from_raw(shares), next_id, clock, 1,
                             static_cast<hft::TrackingNumber>(i & 0xFFFFu),
                             names[sym].c_str());
            packets.append(frame);

            live.push_back(Live{next_id, price, side, shares});
            ++next_id;
            ++local.adds;
        } else {
            // Choose WHICH live order to mutate.
            //
            // When the book is at capacity, remove the order furthest
            // from the current mid: that is the stale one, and in a
            // real market it is either cancelled or traded through
            // first. Without this the book CROSSES.
            //
            // Crossing is the failure that matters here, and it is
            // silent. Orders placed when the generator's mid was high
            // survive as asks; orders placed after it walked down
            // arrive as bids below them. The best bid ends up above the
            // best ask by thousands of raw units, which cannot happen
            // in a real market, and every downstream number computed
            // from that book is meaningless: the mid, the volatility,
            // the markout, the PnL. A crossed book is not a slightly
            // wrong book, it is not a book.
            //
            // Removing the stalest order also keeps the book tight
            // around the mid, which is what makes the touch move and
            // gives a market maker something to trade against.
            std::size_t pick = 0;
            if (at_capacity) {
                std::int64_t worst_distance = -1;
                for (std::size_t k = 0; k < live.size(); ++k) {
                    const std::int64_t distance =
                        live[k].price.raw() - mid;
                    const std::int64_t magnitude = distance < 0 ? -distance : distance;
                    if (magnitude > worst_distance) {
                        worst_distance = magnitude;
                        pick = k;
                    }
                }
            } else {
                const std::size_t tail_start = (live.size() * 9) / 10;
                const bool prefer_recent =
                    tail_start > 0 &&
                    static_cast<double>(rng.below(1'000'000)) / 1'000'000.0 < config.pct_recent;
                const std::size_t lo = prefer_recent ? tail_start : 0;
                const std::size_t window = live.size() - lo;
                pick = lo + static_cast<std::size_t>(rng.below(window));
            }

            Live target = live[pick];
            live[pick] = live.back();
            live.pop_back();

            clock += 1 + rng.below(4'000);
            std::vector<std::uint8_t> frame;
            frame.reserve(itch::frame_size(itch::off::kOrderExecutedAtPriceSize));

            if (config.inject_order_replace && roll >= 99) {
                append_order_replace(frame, target.id, next_id, hft::Quantity::from_raw(
                                                                    target.remaining),
                                     target.price, clock, 1,
                                     static_cast<hft::TrackingNumber>(i & 0xFFFFu));
                ++local.order_replace;
                // The replaced order leaves the book under its new
                // reference, which this build does not track, so the
                // generator's live set does not gain an entry for it.
            } else if (roll < config.pct_add + config.pct_execute) {
                // Execute 1..remaining. Never zero: a zero-share
                // execution is not a thing a venue sends, and allowing
                // it would make the fill path look idempotent when it
                // is not.
                const std::uint32_t shares =
                    1 + static_cast<std::uint32_t>(rng.below(target.remaining));
                ++match_number;
                if ((rng.next() & 3u) == 0u) {
                    append_order_executed_at_price(frame, target.id,
                                                   hft::Quantity::from_raw(shares),
                                                   target.price, clock, match_number, 1,
                                                   static_cast<hft::TrackingNumber>(i & 0xFFFFu));
                } else {
                    append_order_executed(frame, target.id, hft::Quantity::from_raw(shares), clock,
                                          match_number, 1,
                                          static_cast<hft::TrackingNumber>(i & 0xFFFFu));
                }
                ++local.executes;
                if (shares < target.remaining) {
                    target.remaining -= shares;
                    live.push_back(target);
                }
            } else if (roll < config.pct_add + config.pct_execute + config.pct_cancel) {
                const std::uint32_t shares =
                    1 + static_cast<std::uint32_t>(rng.below(target.remaining));
                append_order_cancel(frame, target.id, hft::Quantity::from_raw(shares), clock, 1,
                                    static_cast<hft::TrackingNumber>(i & 0xFFFFu));
                ++local.cancels;
                if (shares < target.remaining) {
                    target.remaining -= shares;
                    live.push_back(target);
                }
            } else {
                append_order_delete(frame, target.id, clock, 1,
                                    static_cast<hft::TrackingNumber>(i & 0xFFFFu));
                ++local.deletes;
            }

            // Emit the record. An earlier revision of this loop built
            // the mutation frame and then forgot to write it, so every
            // cancel, execute and delete was discarded after already
            // consuming a sequence number. The replay tool reported it
            // as 79,707 missing messages, which is precisely the
            // failure the sequence check exists to surface.
            packets.append(frame);
        }

#ifdef HFT_DEBUG_CROSS
        {
            std::int64_t lo_ask = 0;
            std::int64_t hi_bid = 0;
            for (const Live& l : live) {
                if (l.side == hft::Side::ask) {
                    if (lo_ask == 0 || l.price.raw() < lo_ask) {
                        lo_ask = l.price.raw();
                    }
                } else {
                    if (hi_bid == 0 || l.price.raw() > hi_bid) {
                        hi_bid = l.price.raw();
                    }
                }
            }
            if (lo_ask != 0 && hi_bid != 0 && hi_bid >= lo_ask) {
                std::fprintf(stderr, "LIVE CROSSED hi_bid=%lld lo_ask=%lld live=%zu\n",
                             (long long)hi_bid, (long long)lo_ask, live.size());
                break;
            }
        }
#endif
    }

    if (stats != nullptr) {
        *stats = local;
    }
    return out;
}

}  // namespace hft::feed
