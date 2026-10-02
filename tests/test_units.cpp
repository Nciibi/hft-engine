// Unit tests for the wire codec and the value types.
//
// Deliberately dependency-free: a test framework would mean a third
// party in a repository whose selling point is zero dependencies. The
// harness below is twenty lines and reports the same information.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/itch/moldudp64.hpp"

#include "../src/bench/report.hpp"
#include "hft/itch/protocol.hpp"
#include "hft/itch/sequence.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "hft/types.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void check_eq_int(long long actual, long long expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL  %s: expected %lld, got %lld\n", what, expected, actual);
    }
}

// ---- Price ----------------------------------------------------------

void test_price_parse() {
    std::printf("Price::parse\n");

    check(hft::Price::parse("100").has_value(), "'100' parses");
    check_eq_int(hft::Price::parse("100").value().raw(), 100 * hft::Price::kScale, "'100' scales");

    check_eq_int(hft::Price::parse("123.45").value().raw(), 1234500, "'123.45' exact");
    check_eq_int(hft::Price::parse("123.4").value().raw(), 1234000, "'123.4' right-pads");
    check_eq_int(hft::Price::parse("0.0001").value().raw(), 1, "smallest representable");
    check_eq_int(hft::Price::parse("123.").value().raw(), 123 * hft::Price::kScale, "trailing dot");

    // The rejections matter more than the acceptances: each is a case
    // where silently rounding or defaulting would corrupt a price.
    check(!hft::Price::parse("").has_value(), "empty rejected");
    check(!hft::Price::parse("abc").has_value(), "letters rejected");
    check(!hft::Price::parse("1.23456").has_value(), "5 fractional digits rejected");
    check(!hft::Price::parse("-1.00").has_value(), "negative rejected");
    check(!hft::Price::parse("+1.00").has_value(), "explicit plus rejected");
    check(!hft::Price::parse("1e5").has_value(), "exponent rejected");
    check(!hft::Price::parse("1.2.3").has_value(), "double decimal point rejected");
    check(!hft::Price::parse("1.00 ").has_value(), "trailing space rejected");
    check(!hft::Price::parse(".50").has_value(), "no integer part rejected");
    check(!hft::Price::parse("99999999999999999999").has_value(), "overflow rejected");

    // Round trip through to_string.
    check(hft::Price::parse("123.4500").value().to_string() == "123.4500", "round trip 123.4500");
    check(hft::Price::from_raw(1).to_string() == "0.0001", "round trip 0.0001");
    check(hft::Price::from_raw(0).to_string() == "0.0000", "round trip zero");
    check(hft::Price::from_raw(-1).to_string() == "-0.0001", "round trip negative");
    // INT64_MIN must not invoke signed overflow during negation.
    check(hft::Price::from_raw(INT64_MIN).to_string().front() == '-', "INT64_MIN renders negative");
}

// ---- 48-bit timestamp ------------------------------------------------

void test_timestamp48() {
    std::printf("timestamp48\n");

    // Values chosen so a high/low mix-up is visible rather than
    // accidentally correct.
    const std::uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    check_eq_int(static_cast<long long>(hft::itch::read_timestamp48(zero)), 0, "all zero");

    const std::uint8_t one[6] = {0, 0, 0, 0, 0, 1};
    check_eq_int(static_cast<long long>(hft::itch::read_timestamp48(one)), 1, "low only");

    const std::uint8_t high_only[6] = {0, 1, 0, 0, 0, 0};
    check_eq_int(static_cast<long long>(hft::itch::read_timestamp48(high_only)), 1LL << 32,
                 "high half is shifted by 32");

    const std::uint8_t both[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    // 0xFFFFFFFFFFFF == (2^48 - 1), which must survive as one value.
    check_eq_int(static_cast<long long>(hft::itch::read_timestamp48(both)), (1LL << 48) - 1,
                 "max 48-bit value");

    // 1 second past midnight in nanoseconds.
    const std::uint8_t one_sec[6] = {0, 0, 0x3B, 0x9A, 0xCA, 0x00};
    check_eq_int(static_cast<long long>(hft::itch::read_timestamp48(one_sec)), 1'000'000'000,
                 "1e9 ns");
}

// ---- Big-endian readers ---------------------------------------------

void test_big_endian() {
    std::printf("big-endian readers\n");

    const std::uint8_t b16[2] = {0x12, 0x34};
    check_eq_int(hft::itch::read_be16(b16), 0x1234, "be16");

    const std::uint8_t b32[4] = {0x12, 0x34, 0x56, 0x78};
    check_eq_int(hft::itch::read_be32(b32), 0x12345678LL, "be32");

    const std::uint8_t b64[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    check_eq_int(static_cast<long long>(hft::itch::read_be64(b64)), 0x0123456789ABCDEFLL, "be64");
}

// ---- Frame decoding -------------------------------------------------

void test_add_order_layout_is_spec() {
    std::printf("Add Order layout against the published field table\n");
    using namespace hft;

    // This test exists because of a bug it would have caught.
    //
    // Every other decode test in this file builds its input with the
    // generator and reads it back with the decoder, so it only ever
    // proves the two agree. When the generator and the decoder both had
    // the Add Order layout wrong -- shares where the price goes, the
    // stock symbol where the shares go -- every one of those tests
    // passed. 340 checks, all green, and the decoder was reading the
    // share count as the price.
    //
    // So the frame below is built by hand, byte by byte, from the
    // Nasdaq TotalView-ITCH 5.0 field table and nowhere else. Nothing
    // in this function calls the generator. If the offsets in
    // protocol.hpp are ever wrong again, this fails and the round-trip
    // tests keep passing.
    //
    // Spec section 1.3.1, Add Order - No MPID Attribution:
    //   0 tag | 1 locate | 3 tracking | 5 timestamp | 11 reference
    //   19 side | 20 SHARES | 24 stock symbol | 32 PRICE
    constexpr std::uint32_t kShares = 100;
    constexpr std::uint32_t kPriceRaw = 1'502'500;  // $150.25
    const std::uint8_t body[itch::off::kAddOrderSize] = {
        static_cast<std::uint8_t>('A'),                        // 0
        0x04, 0xD2,                                              // 1  locate 1234
        0x00, 0x00,                                              // 3  tracking 0
        0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x7B,                     // 5  timestamp
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,          // 11 reference 42
        static_cast<std::uint8_t>('B'),                        // 19 side
        0x00, 0x00, 0x00, 0x64,                                 // 20 SHARES 100
        'S', 'I', 'M', 'T', 'E', 'S', 'T', ' ',                // 24 stock symbol
        0x00, 0x16, 0xED, 0x24,                                 // 32 PRICE 1502500
    };
    static_assert(sizeof(body) == 36, "the spec says the Add Order body is 36 bytes");

    // The stock symbol must occupy exactly offsets 24..31 and the price
    // exactly 32..35. If the two ever swap, these fail by position
    // rather than by value, which points at the layout instead of the
    // arithmetic.
    check_eq_int(static_cast<long long>(std::string(body + 24, body + 32).size()), 8,
                 "stock symbol is 8 bytes wide");
    check(std::string(body + 24, body + 32) == "SIMTEST ", "stock symbol is the literal written");
    check_eq_int(itch::read_be32(body + 20), static_cast<int>(kShares),
                 "offset 20 holds SHARES, not the price");
    check_eq_int(itch::read_be32(body + 32), static_cast<int>(kPriceRaw),
                 "offset 32 holds the PRICE, not the shares");

    // Prefix it and decode. Frame = 2-byte length + 36-byte body.
    std::vector<std::uint8_t> frame;
    frame.push_back(0);
    frame.push_back(static_cast<std::uint8_t>(itch::off::kAddOrderSize));
    frame.insert(frame.end(), body, body + itch::off::kAddOrderSize);

    const auto r = itch::decode(frame.data(), frame.size());
    check(r.ok(), "the hand-built spec frame decodes");
    const auto* ao = std::get_if<itch::AddOrder>(&r.message.body);
    check(ao != nullptr, "payload is an AddOrder");
    if (ao != nullptr) {
        // The two assertions that matter. They are deliberately written
        // so that a swapped layout cannot satisfy both: 100 and 1502500
        // are different numbers, and a decoder that reads one field as
        // the other gets exactly one of these two wrong.
        check_eq_int(static_cast<long long>(ao->size.raw()), 100,
                     "100 shares decode as the SIZE, not the price");
        check_eq_int(ao->price.raw(), 1'502'500,
                     "1502500 raw decodes as the PRICE, not the shares");

        check_eq_int(ao->stock_locate, 1234, "stock locate decodes");
        check_eq_int(ao->tracking, 0, "tracking number decodes");
        check(ao->timestamp == 0x0000'1F1A'CED9'F07BULL, "48-bit timestamp decodes");
        check(ao->id == 42, "order reference decodes");
        check(ao->side == Side::bid, "'B' decodes as a bid");
        check(std::string(ao->stock, 8) == "SIMTEST ", "stock symbol decodes");

        // A price of 100 raw is $0.01 and a share count of 1,502,500 is
        // absurd. Asserting the pair makes the failure legible: whoever
        // hits this knows immediately which field moved.
        check(ao->price.raw() > 1000,
              "decoded price is a price, not a share count (this is the Add Order bug)");
        check(ao->size.raw() < 100'000,
              "decoded size is a share count, not a price (this is the Add Order bug)");
    }

    // The generator must now produce the same layout, which is the only
    // reason the rest of this file can keep round-tripping.
    std::vector<std::uint8_t> generated;
    feed::append_add_order(generated, Side::bid, Price::from_raw(kPriceRaw),
                           Quantity::from_raw(kShares), 42, 0x0000'1F1A'CED9'F07BULL, 1234, 0);
    check_eq_int(static_cast<long long>(generated.size()), 38,
                 "the generator writes a 38-byte Add Order frame");
    check_eq_int(static_cast<long long>(generated.size()), static_cast<long long>(frame.size()),
                 "generator and hand-built spec frame are the same length");
    // Field by field, the generator's bytes must equal the spec's.
    bool identical = true;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        if (generated[i] != frame[i]) {
            identical = false;
        }
    }
    check(identical,
          "the generator emits exactly the bytes the specification lists, byte for byte");
}

// The MoldUDP64 frame below is hand-built from the field table, for the
// same reason the Add Order frame is: a round trip through this
// repository's own generator proves only that the two agree, and the
// whole Add Order bug lived in the space where they agreed and the
// specification did not.
//
// Two facts from the specification are asserted here that the rest of
// this repository previously had backwards, and both are easy to be
// wrong about:
//
//   * the Sequence Number field is EIGHT bytes, not four;
//   * there is no checksum. MoldUDP64 does not checksum packets --
//     integrity belongs to SOUP, which is a different protocol. A test
//     asserting a checksum field here would be asserting a field the
//     specification does not define.
void test_moldudp64_against_spec() {
    std::printf("MoldUDP64 framing against the published field table\n");
    using namespace hft;

    namespace mold = hft::itch::mold;

    // Offsets and widths, asserted as literals rather than as
    // relationships. See protocol.hpp for why.
    check_eq_int(static_cast<long long>(mold::kHeaderSize), 20, "header is 20 bytes");
    check_eq_int(static_cast<long long>(mold::kSessionOffset), 0, "session at offset 0");
    check_eq_int(static_cast<long long>(mold::kSequenceOffset), 10, "sequence at offset 10");
    check_eq_int(static_cast<long long>(mold::kCountOffset), 18, "count at offset 18");
    check_eq_int(static_cast<long long>(mold::kSequenceSize), 8, "sequence is 8 bytes");
    check_eq_int(static_cast<long long>(mold::kMessageBlockSize), 2, "block length prefix is 2 bytes");
    check_eq_int(static_cast<long long>(mold::kFirstBlockOffset), 20, "first block at offset 20");

    // The two special Message Count values.
    check(mold::is_heartbeat(0), "message count 0 is a heartbeat");
    check(!mold::is_heartbeat(1), "message count 1 is not a heartbeat");
    check(mold::is_end_of_session(0xFFFF), "message count 0xFFFF is end of session");
    check(!mold::is_end_of_session(0xFFFE), "0xFFFE is not end of session");

    // ---- A hand-built packet, byte by byte -------------------------
    //
    //   "SAMPLE0000"                          session, 10 bytes
    //   00 00 01 00 00 00 2A 2B                sequence, 8 bytes
    //   00 02                                count, 2 bytes
    //   00 24                                block length, 2 bytes
    //   <36 bytes of Add Order body>          one message block
    const std::uint8_t packet[] = {
        'S', 'A', 'M', 'P', 'L', 'E', '0', '0', '0', '0',  // session
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2A, 0x2B,    // sequence
        0x00, 0x02,                                          // count = 2
        0x00, 0x24,                                          // block 1 length = 36
        'A', 0x04, 0xD2, 0x00, 0x00, 0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x7B,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 'B',
        0x00, 0x00, 0x00, 0x64, 'S', 'I', 'M', 'T', 'E', 'S', 'T', ' ',
        0x00, 0x16, 0xED, 0x24,
        0x00, 0x17,                                          // block 2 length = 23
        'X', 0x04, 0xD2, 0x00, 0x00, 0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x7B,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,
        0x00, 0x00, 0x00, 0x32,                               // cancel 50 shares
    };
    // The block lengths above are 36 (Add) and 23 (Cancel) because those
    // are the spec body sizes for 'A' and 'X'. Writing a plausible-looking
    // length that disagrees with the body shifts every following field,
    // which is exactly the class of error this file exists to catch.

    mold::PacketHeader header{};
    check(mold::parse_header(packet, sizeof(packet), header) == mold::HeaderStatus::ok,
          "the hand-built packet header parses");
    check(std::string(header.session, 10) == "SAMPLE0000", "session decodes");
    check_eq_int(static_cast<long long>(header.count), 2, "message count decodes");

    // The eight-byte sequence is the assertion that matters most. A
    // four-byte read would return 0x2A2B and pass a naive comparison
    // against a 32-bit literal, while having silently discarded
    // 0x00000100.
    check(header.sequence == 0x0000'0100'0000'2A2BULL,
          "the full 64-bit sequence decodes, high half included");
    check(header.sequence != 0x2A2BULL, "a 32-bit read would have returned only the low half");
    check(header.next_expected() == header.sequence,
          "on a packet with blocks the sequence field is the first message, not next_expected");

    // ---- Walking the blocks ----------------------------------------
    mold::MessageBlocks blocks(packet + mold::kFirstBlockOffset,
                               sizeof(packet) - mold::kFirstBlockOffset, header.count);
    check_eq_int(blocks.remaining(), 2, "two blocks are expected");
    check(!blocks.truncated(), "the packet is not truncated");

    std::size_t size = 0;
    const std::uint8_t* first = blocks.next(size);
    // Each frame's size is captured into its OWN variable. `next()` does
    // not touch `size` when it returns null, so reusing one variable
    // across calls silently decodes the first block with the second
    // block's length -- which fails in the decoder and points at ITCH
    // rather than at the test.
    const std::size_t first_size = size;
    check(first != nullptr, "the first block is present");
    // The returned frame includes the two-byte length prefix, because a
    // MoldUDP64 Message Block and an ITCH frame have the same layout and
    // the block should feed `decode` unadjusted.
    check_eq_int(static_cast<long long>(first_size), 38,
                 "the first frame is 38 bytes: a 36-byte Add plus its 2-byte prefix");
    check(first != nullptr && static_cast<int>(first[0] * 256 + first[1]) == 36,
          "and it starts with the length prefix the framing reports");

    const std::uint8_t* second = blocks.next(size);
    const std::size_t second_size = size;
    check(second != nullptr, "the second block is present");
    check_eq_int(static_cast<long long>(second_size), 25,
                 "the second frame is 25 bytes: a 23-byte cancel plus its 2-byte prefix");

    check(blocks.next(size) == nullptr, "no third block, because the count said two");
    check_eq_int(blocks.remaining(), 0, "the count is exhausted");
    check(!blocks.truncated(), "a cleanly finished packet is not reported truncated");

    // A block IS an ITCH frame, with no adjustment at either layer.
    // That identity is the point of returning the prefix.
    if (first != nullptr) {
        const itch::DecodeResult r = itch::decode(first, first_size);
        check(r.ok(), "a block decodes as an ITCH frame with no adjustment");
        if (r.ok()) {
            check(r.message.type() == itch::MessageType::add_order, "and it is an Add Order");
            const auto* ao = std::get_if<itch::AddOrder>(&r.message.body);
            if (ao != nullptr) {
                check_eq_int(ao->price.raw(), 1'502'500, "with the price at the right offset");
                check_eq_int(static_cast<long long>(ao->size.raw()), 100, "and 100 shares");
            }
        }
    }
    if (second != nullptr) {
        const itch::DecodeResult r = itch::decode(second, second_size);
        check(r.ok(), "the second block decodes too");
        if (r.ok()) {
            check(r.message.type() == itch::MessageType::order_cancel,
                  "and it is an Order Cancel");
        }
    }

    // ---- Truncation, at every byte ---------------------------------
    for (std::size_t n = 0; n < mold::kHeaderSize; ++n) {
        mold::PacketHeader h{};
        check(mold::parse_header(packet, n, h) == mold::HeaderStatus::truncated,
              "a short header is reported, never read past");
    }

    // A header that parses but whose blocks are cut off is the case
    // that matters: it reads as a valid packet and then runs out of
    // data mid-block. `truncated()` is what distinguishes it from a
    // packet that simply finished.
    {
        mold::MessageBlocks cut(packet + mold::kFirstBlockOffset, 10, header.count);
        (void)cut.next(size);
        check(cut.truncated(), "a block running past the buffer is reported as truncated");
    }
    {
        mold::MessageBlocks cut(packet + mold::kFirstBlockOffset, 1, header.count);
        (void)cut.next(size);
        check(cut.truncated(), "so is a buffer that cannot even hold a length prefix");
    }

    // ---- Heartbeat --------------------------------------------------
    // A heartbeat carries the next expected sequence and no blocks. The
    // sequence field means something DIFFERENT there, which is exactly
    // why the accessor is named for it.
    const std::uint8_t heartbeat[mold::kHeaderSize] = {
        'S', 'A', 'M', 'P', 'L', 'E', '0', '0', '0', '0',
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00,
    };
    mold::PacketHeader hb{};
    check(mold::parse_header(heartbeat, sizeof(heartbeat), hb) == mold::HeaderStatus::ok,
          "a heartbeat header parses");
    check(mold::is_heartbeat(hb.count), "and is recognised as a heartbeat");
    check(hb.next_expected() == 0x0000'0000'0001'0000ULL,
          "a heartbeat's sequence is the next expected, not a message number");

    // ---- Round trip through the generator --------------------------
    std::vector<std::uint8_t> generated;
    feed::append_downstream_packet(generated, "SAMPLE0000", 0x0000'0100'0000'2A2BULL,
                                  packet + 22, 36, 1);
    check_eq_int(static_cast<long long>(generated.size()),
                 static_cast<long long>(mold::kHeaderSize + mold::kMessageBlockSize + 36),
                 "a one-message packet is header + length prefix + body");
    mold::PacketHeader gh{};
    check(mold::parse_header(generated.data(), generated.size(), gh) == mold::HeaderStatus::ok,
          "the generated packet parses");
    check(gh.sequence == 0x0000'0100'0000'2A2BULL, "with the same 64-bit sequence");
    check(std::string(gh.session, 10) == "SAMPLE0000", "and the same session");
    mold::MessageBlocks gb(generated.data() + mold::kFirstBlockOffset,
                           generated.size() - mold::kFirstBlockOffset, gh.count);
    const std::uint8_t* gb_first = gb.next(size);
    check(size == 38 && gb_first != nullptr, "and yields the same single Add Order frame");
    if (gb_first != nullptr) {
        check(itch::decode(gb_first, size).ok(),
              "which decodes as an ITCH frame straight out of the generator");
    }

    // No checksum. The packet is exactly header + block, with nothing
    // appended, because the specification defines no such field.
    check(generated.size() == 20 + 2 + 36,
          "MoldUDP64 packets carry no checksum: SOUP provides integrity, not this layer");
}

void test_decode() {
    std::printf("frame decode\n");
    using namespace hft;

    std::vector<std::uint8_t> buf;
    feed::append_add_order(buf, Side::bid, Price::from_raw(1234500), Quantity::from_raw(300),
                           0xABCD'EF01'2345'6789ULL, 0x0000'0001'2BAD'F00DULL);

    check_eq_int(static_cast<long long>(buf.size()), 38, "Add Order frame is 38 bytes");
    check_eq_int(static_cast<long long>(buf[0] * 256 + buf[1]), itch::off::kAddOrderSize,
                 "length prefix is 32 and counts the tag");
    check_eq_int(buf[2], static_cast<int>('A'), "tag byte is 'A'");

    const auto r = itch::decode(buf.data(), buf.size());
    check(r.ok(), "valid frame decodes");
    if (r.ok()) {
        const auto* ao = std::get_if<itch::AddOrder>(&r.message.body);
        check(ao != nullptr, "payload is an AddOrder");
        check(r.message.type() == itch::MessageType::add_order, "tag derived from variant");
        if (ao != nullptr) {
            check(ao->id == 0xABCD'EF01'2345'6789ULL, "order id round trips");
            check(ao->side == Side::bid, "side round trips");
            check_eq_int(ao->price.raw(), 1234500, "price round trips");
            check_eq_int(static_cast<long long>(ao->size.raw()), 300, "size round trips");
            check(ao->timestamp == 0x0000'0001'2BAD'F00DULL, "timestamp round trips");
        }
        check_eq_int(static_cast<long long>(itch::frame_stride(r)), 38, "stride is the full frame");
    }

    // Truncation at every length must be reported, never read past.
    for (std::size_t n = 0; n < buf.size(); ++n) {
        const auto t = itch::decode(buf.data(), n);
        check(t.status == itch::DecodeStatus::truncated ||
                  t.status == itch::DecodeStatus::truncated_body,
              "truncated frame is reported, not read past");
    }

    // A body tagged 'A' with a length that disagrees must be rejected
    // rather than read as if it were Add Order fields.
    {
        std::vector<std::uint8_t> bad;
        std::vector<std::uint8_t> body(buf.begin() + 2, buf.end());
        feed::append_frame_with_length(bad, body.data(), body.size(), 20);
        const auto br = itch::decode(bad.data(), bad.size());
        check(br.status == itch::DecodeStatus::bad_length, "length mismatch rejected");
    }

    // An unknown but well-formed type is skippable, not an error.
    {
        std::vector<std::uint8_t> unknown;
        const std::uint8_t body[8] = {'S', 0, 0, 0, 0, 0, 0, 0};
        feed::append_frame(unknown, body, sizeof(body));
        const auto ur = itch::decode(unknown.data(), unknown.size());
        check(ur.status == itch::DecodeStatus::unknown_type, "unknown type reported");
        check(ur.skippable(), "unknown type is skippable");
        check_eq_int(ur.length, 8, "unknown type reports its length");
        check_eq_int(static_cast<long long>(itch::frame_stride(ur)), 10, "skip stride is correct");
    }

    // A side byte that is neither 'B' nor 'S' must be malformed, never
    // silently coerced to a bid.
    {
        std::vector<std::uint8_t> bad;
        feed::append_add_order(bad, Side::ask, Price::from_raw(1000000), Quantity::from_raw(10),
                               42, 0);
        bad[2 + itch::off::add_order_side] = 'X';
        const auto br = itch::decode(bad.data(), bad.size());
        check(br.status == itch::DecodeStatus::malformed, "invalid side byte rejected");
    }

    // Order reference 0 is reserved as "none" and must not become a
    // live handle.
    {
        std::vector<std::uint8_t> bad;
        feed::append_add_order(bad, Side::bid, Price::from_raw(1000000), Quantity::from_raw(10),
                               0, 0);
        const auto br = itch::decode(bad.data(), bad.size());
        check(br.status == itch::DecodeStatus::malformed, "zero order reference rejected");
    }
}

// ---- Generated feed round trip --------------------------------------

void test_generated_feed() {
    std::printf("generated feed\n");
    using namespace hft;

    feed::GeneratorConfig config;
    config.message_count = 5'000;
    config.price_levels = 32;
    const std::vector<std::uint8_t> data = feed::generate_add_orders(config);

    std::size_t offset = 0;
    std::size_t ok = 0;
    std::size_t other = 0;
    while (offset < data.size()) {
        const auto r = itch::decode(data.data() + offset, data.size() - offset);
        const std::size_t stride = itch::frame_stride(r);
        if (stride == 0) {
            break;
        }
        if (r.ok()) {
            ++ok;
        } else {
            ++other;
        }
        offset += stride;
    }

    check_eq_int(static_cast<long long>(ok), 5000, "every generated frame decodes");
    check_eq_int(static_cast<long long>(other), 0, "no generated frame is skipped");
    check_eq_int(static_cast<long long>(offset), static_cast<long long>(data.size()),
                 "consumed the whole buffer exactly");
}

// ---- The capture format ------------------------------------------------
//
// These are round-trip and failure-mode tests for the packet capture
// path: the generator WRITES MoldUDP64 packets and the reader READS
// them. The point is that both halves agree, and -- more to the point --
// that a reader cannot be fooled by a capture that stops mid-packet.
//
// The dangerous failure mode here is not a wrong number, it is a
// capture that is one byte short and replays as a clean shorter stream.
// Every consumer would report success. That is what `malformed()`
// exists to catch, and it is tested here rather than assumed.

void test_capture_round_trip() {
    std::printf("capture round trip\n");
    using namespace hft;

    feed::CaptureConfig config;
    config.record_count = 10'000;
    config.max_live_orders = 64;
    config.reversion = 4;
    config.drift_raw = 400;
    const std::vector<std::uint8_t> data = feed::generate_capture(config);

    // Every packet must be well formed on its own terms, and the whole
    // buffer must be consumed to the last byte. If the reader stops
    // early, `ok` will not reach the record count.
    feed::CaptureReader reader(data.data(), data.size());
    const std::uint8_t* frame = nullptr;
    std::size_t frame_size = 0;

    std::size_t ok = 0;
    std::size_t other = 0;
    std::uint64_t expected = config.first_sequence;
    bool sequence_contiguous = true;
    bool sizes_consistent = true;

    while (reader.next(frame, frame_size)) {
        if (reader.sequence() != expected) {
            sequence_contiguous = false;
        }
        ++expected;

        const auto r = itch::decode(frame, frame_size);
        if (r.ok()) {
            ++ok;
        } else {
            ++other;
        }
        // A frame handed back by the reader must be exactly as long as
        // its own length prefix claims. If it were longer, the reader
        // would be reporting a frame that runs into the next block.
        if (frame_size >= itch::kLengthPrefixSize) {
            const std::size_t declared =
                (static_cast<std::size_t>(frame[0]) << 8) | static_cast<std::size_t>(frame[1]);
            if (declared + itch::kLengthPrefixSize != frame_size) {
                sizes_consistent = false;
            }
        } else {
            sizes_consistent = false;
        }
    }

    check_eq_int(static_cast<long long>(ok), 10000, "every captured frame decodes");
    check_eq_int(static_cast<long long>(other), 0, "no captured frame is skipped");
    check(sequence_contiguous, "per-message sequence numbers are contiguous from first_sequence");
    check(sizes_consistent, "every frame is exactly its declared length");
    check(!reader.malformed(), "an intact capture is not malformed");
    check(reader.packets() == 10'000, "one message per packet by default");
    check_eq_int(static_cast<long long>(reader.heartbeats()), 0, "no heartbeats generated");
    check_eq_int(static_cast<long long>(reader.end_of_session()), 0, "no end-of-session generated");

    // The session must survive the round trip: it is in every header,
    // and it is how a handler tells one feed from another. The headers
    // are visited by walking the capture through a second reader rather
    // than by assuming a stride -- a capture holds Add, Delete, Cancel
    // and Execute frames of different lengths, so any fixed stride here
    // would be a coincidence that happens to work for a single message
    // type and silently stops working for the next.
    bool session_ok = true;
    bool header_ok = true;
    std::size_t tiled_to = 0;
    {
        feed::CaptureReader packets(data.data(), data.size());
        const std::uint8_t* first = nullptr;
        std::size_t count = 0;
        std::uint64_t seq = 0;
        for (;;) {
            if (!packets.next_packet(first, count, seq)) {
                break;
            }
            if (count == 0) {
                header_ok = false;
                break;
            }
            // Walk back from the returned frames to the header that
            // introduced them. The frames are contiguous and begin
            // exactly kHeaderSize bytes after it.
            const std::size_t header_at = static_cast<std::size_t>(first - data.data()) -
                                          itch::mold::kHeaderSize;
            if (std::memcmp(data.data() + header_at, config.session,
                            itch::mold::kSessionSize) != 0) {
                session_ok = false;
                break;
            }
            tiled_to = static_cast<std::size_t>(first - data.data());
            // Sum the block lengths to find where the next packet starts.
            std::size_t span = 0;
            for (std::size_t b = 0; b < count; ++b) {
                std::size_t cursor = static_cast<std::size_t>(first - data.data()) + span;
                const std::size_t len = (static_cast<std::size_t>(data[cursor]) << 8) |
                                        static_cast<std::size_t>(data[cursor + 1]);
                span += itch::mold::kMessageBlockSize + len;
            }
            tiled_to += span;
        }
    }
    check(session_ok, "session id is present in every packet header");
    check(header_ok, "every packet header declares at least one block");
    check_eq_int(static_cast<long long>(tiled_to), static_cast<long long>(data.size()),
                 "packet headers and blocks tile the capture exactly");
}

void test_capture_multi_message_packets() {
    std::printf("capture multi-message packets\n");
    using namespace hft;

    feed::CaptureConfig config;
    config.record_count = 1'000;
    config.max_live_orders = 32;
    const std::size_t per_packet = 7;  // deliberately not a power of two
    config.messages_per_packet = per_packet;
    const std::vector<std::uint8_t> data = feed::generate_capture(config);

    feed::CaptureReader reader(data.data(), data.size());
    const std::uint8_t* frame = nullptr;
    std::size_t frame_size = 0;
    std::size_t frames = 0;
    std::uint64_t expected = config.first_sequence;
    bool contiguous = true;
    while (reader.next(frame, frame_size)) {
        if (reader.sequence() != expected) {
            contiguous = false;
        }
        ++expected;
        ++frames;
    }

    check_eq_int(static_cast<long long>(frames), 1000, "every message survives packing");
    check(contiguous, "sequence numbers stay contiguous across packet boundaries");
    check(!reader.malformed(), "packed capture is not malformed");
    // 1000 messages at 7 per packet: 142 full packets and a tail of 6.
    check(reader.packets() == 143, "packet count is ceil(records / per_packet)");

    // The packing must not change what the pipeline sees. The same
    // record count and seed through the record writer and the packet
    // writer must produce the same frames, otherwise a benchmark that
    // moved between the two would be measuring two different things.
    feed::CaptureConfig unpacked = config;
    unpacked.messages_per_packet = 1;
    const std::vector<std::uint8_t> other = feed::generate_capture(unpacked);
    feed::CaptureReader r1(data.data(), data.size());
    feed::CaptureReader r2(other.data(), other.size());
    const std::uint8_t* f1 = nullptr;
    const std::uint8_t* f2 = nullptr;
    std::size_t n1 = 0;
    std::size_t n2 = 0;
    bool identical = true;
    while (r1.next(f1, n1)) {
        if (!r2.next(f2, n2) || n1 != n2 || std::memcmp(f1, f2, n1) != 0) {
            identical = false;
            break;
        }
    }
    if (r2.next(f2, n2)) {
        identical = false;
    }
    check(identical, "packing changes framing but not the frames");
}

void test_capture_rejects_truncation() {
    std::printf("capture rejects truncation\n");
    using namespace hft;

    feed::CaptureConfig config;
    config.record_count = 64;
    config.max_live_orders = 16;
    config.messages_per_packet = 4;
    const std::vector<std::uint8_t> data = feed::generate_capture(config);

    // Cut the capture at every byte offset across the first few packets
    // and require that any cut which lands inside a packet or a block is
    // reported. A cut that lands exactly on a packet boundary is a
    // legitimate shorter capture and must NOT be flagged.
    //
    // Packet boundaries are found by walking the intact capture with a
    // reader, which advances past exactly one packet's worth of blocks
    // per header -- a reader is the only thing here that knows where a
    // packet ends, because frame lengths vary within a packet.
    std::vector<bool> boundary(data.size() + 1, false);
    boundary[0] = true;
    {
        feed::CaptureReader packets(data.data(), data.size());
        const std::uint8_t* first = nullptr;
        std::size_t count = 0;
        std::uint64_t seq = 0;
        while (packets.next_packet(first, count, seq)) {
            std::size_t cursor = static_cast<std::size_t>(first - data.data());
            const std::size_t packet_start = cursor - itch::mold::kHeaderSize;
            for (std::size_t b = 0; b < count; ++b) {
                const std::size_t len = (static_cast<std::size_t>(data[cursor]) << 8) |
                                        static_cast<std::size_t>(data[cursor + 1]);
                cursor += itch::mold::kMessageBlockSize + len;
            }
            boundary[packet_start] = true;
            boundary[cursor] = true;
        }
    }

    std::size_t reported = 0;
    std::size_t silent = 0;
    for (std::size_t cut = 1; cut < data.size() && cut < 400; ++cut) {
        if (boundary[cut]) {
            continue;
        }
        feed::CaptureReader reader(data.data(), cut);
        const std::uint8_t* frame = nullptr;
        std::size_t frame_size = 0;
        while (reader.next(frame, frame_size)) {
        }
        if (reader.malformed()) {
            ++reported;
        } else {
            ++silent;
        }
    }
    check(reported > 0, "some truncations are detected");
    check_eq_int(static_cast<long long>(silent), 0,
                 "no truncation inside a packet is silently accepted");

    // A capture ending exactly on a packet boundary is valid. Cutting to
    // a real header offset must not be flagged, or the flag would be
    // useless -- a consumer would have to ignore it.
    feed::CaptureReader whole(data.data(), data.size());
    const std::uint8_t* f = nullptr;
    std::size_t n = 0;
    while (whole.next(f, n)) {
    }
    // The length of the FIRST packet, measured with packet iteration.
    // Assumed strides do not survive contact with a capture: frames
    // here are Add, Delete, Cancel and Execute, of four different
    // lengths.
    feed::CaptureReader first_packet(data.data(), data.size());
    const std::uint8_t* pf = nullptr;
    std::size_t pf_count = 0;
    std::uint64_t pf_seq = 0;
    const bool have_first = first_packet.next_packet(pf, pf_count, pf_seq);
    check(have_first, "the first packet is readable");
    std::size_t first_span = 0;
    std::size_t frames_in_first = 0;
    if (have_first) {
        std::size_t cursor = static_cast<std::size_t>(pf - data.data());
        for (std::size_t b = 0; b < pf_count; ++b) {
            const std::size_t len = (static_cast<std::size_t>(data[cursor]) << 8) |
                                    static_cast<std::size_t>(data[cursor + 1]);
            cursor += itch::mold::kMessageBlockSize + len;
            ++frames_in_first;
        }
        first_span = cursor;
    }
    if (have_first && first_span < data.size()) {
        feed::CaptureReader partial(data.data(), first_span);
        const std::uint8_t* f2 = nullptr;
        std::size_t n2 = 0;
        std::size_t got = 0;
        while (partial.next(f2, n2)) {
            ++got;
        }
        check(!partial.malformed(), "a capture ending on a packet boundary is valid");
        check_eq_int(static_cast<long long>(partial.packets()), 1,
                     "a single whole packet is one packet");
        check_eq_int(static_cast<long long>(got), static_cast<long long>(frames_in_first),
                     "the lone packet yields all of its frames");
    }

    // Empty and zero-length buffers must not read as malformed.
    feed::CaptureReader empty(nullptr, 0);
    const std::uint8_t* f3 = nullptr;
    std::size_t n3 = 0;
    check(!empty.next(f3, n3), "an empty capture yields no frames");
    check(!empty.malformed(), "an empty capture is not malformed");

    // A header that is too short to hold a sequence number is a
    // truncated packet, not an empty one.
    const std::uint8_t short_header[8] = {0};
    feed::CaptureReader stub(short_header, sizeof short_header);
    const std::uint8_t* f4 = nullptr;
    std::size_t n4 = 0;
    check(!stub.next(f4, n4), "a stub header yields no frames");
    check(stub.malformed(), "a stub header is malformed");
}

void test_capture_handles_control_packets() {
    std::printf("capture handles control packets\n");
    using namespace hft;

    // A real session contains heartbeats and a graceful end. Neither
    // carries a message, and a reader that returned one as a frame would
    // hand a handler a "frame" with an empty body -- which decodes as
    // garbage rather than failing, so this has to be right rather than
    // merely non-crashing.
    const std::vector<std::uint8_t> frames = feed::generate_add_orders([] {
        feed::GeneratorConfig c;
        c.message_count = 1;
        return c;
    }());

    // Heartbeat and end-of-session packets are a bare header: no blocks.
    // The bytes are written by hand rather than with a helper so this
    // test checks big-endian byte order instead of trusting the same
    // code it is meant to be checking.
    auto append_header = [](std::vector<std::uint8_t>& out, std::uint64_t sequence,
                            std::uint16_t count) {
        for (std::size_t i = 0; i < itch::mold::kSessionSize; ++i) {
            out.push_back(static_cast<std::uint8_t>(feed::kDefaultSession[i]));
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>((sequence >> shift) & 0xFFu));
        }
        out.push_back(static_cast<std::uint8_t>((count >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>(count & 0xFFu));
    };

    std::vector<std::uint8_t> data;
    feed::append_downstream_packet(data, "SAMPLE0000", 7, frames.data() + 2, frames.size() - 2, 1);

    // The header just written, read back by hand, must be the header we
    // meant to write.
    check_eq_int(data[itch::mold::kSequenceOffset], 0x00, "sequence byte 0");
    check_eq_int(data[itch::mold::kSequenceOffset + 6], 0x00, "sequence byte 6");
    check_eq_int(data[itch::mold::kSequenceOffset + 7], 0x07, "sequence byte 7 is big endian");
    check_eq_int(data[itch::mold::kSequenceOffset + 8], 0x00, "count byte 0");
    check_eq_int(data[itch::mold::kSequenceOffset + 9], 0x01, "count byte 1 is big endian");

    append_header(data, 8, itch::mold::kHeartbeatCount);
    feed::append_downstream_packet(data, "SAMPLE0000", 8, frames.data() + 2, frames.size() - 2, 1);
    append_header(data, 9, itch::mold::kEndOfSessionCount);

    feed::CaptureReader reader(data.data(), data.size());
    const std::uint8_t* frame = nullptr;
    std::size_t frame_size = 0;
    std::size_t seen = 0;
    std::uint64_t sequences[2] = {0, 0};
    while (reader.next(frame, frame_size)) {
        if (seen < 2) {
            sequences[seen] = reader.sequence();
        }
        ++seen;
    }

    check_eq_int(static_cast<long long>(seen), 2,
                 "heartbeat and end-of-session packets yield no frames");
    check(sequences[0] == 7 && sequences[1] == 8,
               "sequence numbers come from the packets, skipping the heartbeat");
    check_eq_int(static_cast<long long>(reader.heartbeats()), 1, "the heartbeat is counted");
    check_eq_int(static_cast<long long>(reader.end_of_session()), 1, "end-of-session is counted");
    check(!reader.malformed(), "control packets are not malformed");

    // Iteration must STOP at end-of-session, not merely skip it. A
    // session id can be reused the next day, and a reader that walks past
    // the marker would splice two sessions into one sequence space.
    std::vector<std::uint8_t> trailing;
    feed::append_downstream_packet(trailing, "SAMPLE0000", 9, frames.data() + 2,
                                   frames.size() - 2, 1);
    data.insert(data.end(), trailing.begin(), trailing.end());
    feed::CaptureReader past(data.data(), data.size());
    std::size_t after_end = 0;
    while (past.next(frame, frame_size)) {
        ++after_end;
    }
    check_eq_int(static_cast<long long>(after_end), 2,
                 "messages after end-of-session are not replayed");
}

// ---- Order Replace and Broken Trade ----------------------------------
//
// Both layouts come from the TotalView-ITCH 5.0 specification: section
// 4.4.5 for 'U' and 4.5.3 for 'B'. 'U' was skipped in earlier revisions
// of this file on the grounds that its layout could not be verified. It
// could be, and the reason it read as unverifiable is instructive: the
// same message has DIFFERENT offsets in ITCH 3.1 and 4.0, both of which
// are published. A table copied from either is wrong in a way that looks
// right, which is precisely the Add Order failure mode.
//
// 'B' is Broken Trade, not order entry. An earlier revision of this
// repository described an unverified "ITCH Order Entry ('B')" encode
// stage; TotalView-ITCH is an outbound market data feed only and has no
// order entry at all. The frames below are built byte by byte rather
// than through the generator for the same reason the Add Order frame is:
// a layout that the generator and the decoder agree on proves nothing.

// Big-endian writers, local to this file on purpose: a spec-frame test
// that used the production writer could not catch a writer that is wrong
// in the same way the reader is. These are the two operations, written
// out.
void put_be32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) {
        b[at++] = static_cast<std::uint8_t>((v >> s) & 0xFFu);
    }
}

void put_be64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) {
        b[at++] = static_cast<std::uint8_t>((v >> s) & 0xFFu);
    }
}

void test_order_replace_layout_is_spec() {
    std::printf("Order Replace layout against the published field table\n");
    using namespace hft;

    //  0  1  'U'
    //  1  2  Stock Locate
    //  3  2  Tracking Number
    //  5  6  Timestamp
    // 11  8  Original Order Reference Number
    // 19  8  New Order Reference Number
    // 27  4  Shares (new total displayed quantity)
    // 31  4  Price
    std::vector<std::uint8_t> body(itch::off::kOrderReplaceSize, 0);
    body[0] = 'U';
    body[1] = 0x00;
    body[2] = 0x07;              // Stock Locate 7
    body[3] = 0x12;
    body[4] = 0x34;              // Tracking Number 0x1234
    body[5] = 0x01;
    body[6] = 0x02;
    body[7] = 0x03;
    body[8] = 0x04;
    body[9] = 0x05;
    body[10] = 0x06;             // Timestamp 0x010203040506
    put_be64(body, itch::off::order_replace_original_id, 0x1111'2222'3333'4444ULL);
    put_be64(body, itch::off::order_replace_new_id, 0xAAAA'BBBB'CCCC'DDDDULL);
    put_be32(body, itch::off::order_replace_shares, 300);
    put_be32(body, itch::off::order_replace_price, 1'000'250);

    std::vector<std::uint8_t> frame;
    hft::feed::append_frame(frame, body.data(), body.size());
    check_eq_int(static_cast<long long>(itch::off::kOrderReplaceSize), 35,
                 "Order Replace body is 35 bytes (spec 4.4.5)");
    check_eq_int(static_cast<long long>(frame.size()), 37,
                 "Order Replace frame is 37 bytes including the length prefix");

    const auto r = itch::decode(frame.data(), frame.size());
    check(r.ok(), "the hand-built Order Replace frame decodes");
    if (r.ok()) {
        const auto* u = std::get_if<itch::OrderReplace>(&r.message.body);
        check(u != nullptr, "it decodes to OrderReplace");
        if (u != nullptr) {
            check_eq_int(static_cast<long long>(u->stock_locate), 7, "stock locate");
            check_eq_int(static_cast<long long>(u->tracking), 0x1234, "tracking number");
            check_eq_int(static_cast<long long>(u->timestamp), 0x010203040506ULL, "timestamp");
            check(u->original_id == 0x1111'2222'3333'4444ULL, "original order reference");
            check(u->new_id == 0xAAAA'BBBB'CCCC'DDDDULL, "new order reference");
            check_eq_int(static_cast<long long>(u->shares.raw()), 300, "shares");
            check_eq_int(static_cast<long long>(u->price.raw()), 1'000'250, "price");
        }
    }

    // A frame one byte short must be rejected rather than read
    // partially. Order Replace is 35 bytes and 34 is a different
    // message entirely; accepting it would shift every field.
    std::vector<std::uint8_t> short_frame;
    hft::feed::append_frame(short_frame, body.data(), body.size() - 1);
    const auto rs = itch::decode(short_frame.data(), short_frame.size());
    check(!rs.ok(), "a 34-byte Order Replace frame is not accepted");

    // Order Replace must lose time priority: the replacement sorts
    // BEHIND everything already resting at its price. Getting this
    // backwards is invisible in a checksum that only sums the book and
    // obvious in a queue-depth or fill-sequence comparison.
    hft::lob::OrderBook book(1u << 12, 1u << 12);
    hft::lob::BookStatus status{};
    // Two resting orders at 100_000, then a replace onto the same price.
    book.add(Side::bid, Price::from_raw(100'000), Quantity::from_raw(10), 1'000'001, status);
    book.add(Side::bid, Price::from_raw(100'000), Quantity::from_raw(20), 1'000'002, status);

    itch::OrderReplace u2;
    u2.original_id = 1'000'001;
    u2.new_id = 1'000'003;
    u2.shares = Quantity::from_raw(30);
    u2.price = Price::from_raw(100'000);
    const hft::lob::ApplyResult ar = hft::lob::apply(itch::Message{u2}, book);
    check(ar.applied, "the replace is applied");
    check(!book.find(1'000'001).has_value(), "the original reference is gone");
    check(book.find(1'000'003).has_value(), "the new reference exists");
    check(!book.find(1'000'002).has_value() == false, "the untouched order survives");

    // New time priority means the replacement is last in the queue at
    // that price: order 2 precedes it.
    const std::vector<hft::lob::OrderSnapshot> resting = book.orders(Side::bid);
    check(resting.size() == 2, "the price level holds two orders after the replace");
    if (resting.size() == 2) {
        check(resting[0].id == 1'000'002,
              "the replacement sits BEHIND the order already resting");
        check(resting[1].id == 1'000'003, "the replacement is at the back of the queue");
    }

    // A replace for an unknown reference is reported, never guessed.
    itch::OrderReplace orphan;
    orphan.original_id = 999'999;
    orphan.new_id = 1'000'004;
    orphan.shares = Quantity::from_raw(1);
    orphan.price = Price::from_raw(100'000);
    const hft::lob::ApplyResult ao = hft::lob::apply(itch::Message{orphan}, book);
    check(!ao.applied, "a replace for an unknown reference is not applied");
    check(ao.detail == hft::lob::BookStatus::unknown_order,
          "and it reports unknown_order specifically");
}

void test_broken_trade_layout_is_spec() {
    std::printf("Broken Trade layout against the published field table\n");
    using namespace hft;

    //  0  1  'B'   Broken Trade Message -- NOT order entry
    //  1  2  Stock Locate
    //  3  2  Tracking Number
    //  5  6  Timestamp
    // 11  8  Match Number of the execution that was broken
    std::vector<std::uint8_t> body(itch::off::kBrokenTradeSize, 0);
    body[0] = 'B';
    body[1] = 0x00;
    body[2] = 0x2A;              // Stock Locate 42
    body[3] = 0xBE;
    body[4] = 0xEF;              // Tracking Number 0xBEEF
    body[5] = 0x11;
    body[6] = 0x22;
    body[7] = 0x33;
    body[8] = 0x44;
    body[9] = 0x55;
    body[10] = 0x66;
    put_be64(body, itch::off::broken_trade_match, 0xDEADBEEF00000001ULL);

    check_eq_int(static_cast<long long>(itch::off::kBrokenTradeSize), 19,
                 "Broken Trade body is 19 bytes (spec 4.5.3)");

    std::vector<std::uint8_t> frame;
    hft::feed::append_frame(frame, body.data(), body.size());
    check_eq_int(static_cast<long long>(frame.size()), 21,
                 "Broken Trade frame is 21 bytes including the length prefix");

    const auto r = itch::decode(frame.data(), frame.size());
    check(r.ok(), "the hand-built Broken Trade frame decodes");
    if (r.ok()) {
        const auto* b = std::get_if<itch::BrokenTrade>(&r.message.body);
        check(b != nullptr, "it decodes to BrokenTrade");
        if (b != nullptr) {
            check_eq_int(static_cast<long long>(b->stock_locate), 42, "stock locate");
            check_eq_int(static_cast<long long>(b->tracking), 0xBEEF, "tracking number");
            check(b->match == 0xDEAD'BEEF'0000'0001ULL, "match number");
            check(b->match == 0xDEADBEEF00000001ULL,
                  "match number is read as a full 64-bit big-endian value");
        }
    }

    // The book is untouched: the specification says a book builder "may
    // ignore these messages as they have no impact on the current book".
    hft::lob::OrderBook book(1u << 10, 1u << 10);
    hft::lob::BookStatus status{};
    book.add(Side::bid, Price::from_raw(50'000), Quantity::from_raw(10), 5'000'001, status);
    const std::uint64_t before = book.aggregate_at(Side::bid).raw();

    itch::BrokenTrade bt;
    bt.match = 0xDEAD'BEEF'0000'0001ULL;
    const hft::lob::ApplyResult ar = hft::lob::apply(itch::Message{bt}, book);
    check(ar.applied, "a Broken Trade is reported as processed");
    check_eq_int(static_cast<long long>(book.aggregate_at(Side::bid).raw()),
                 static_cast<long long>(before), "a Broken Trade does not move the book");

    // A Broken Trade is 19 bytes; so is Order Delete. They are different
    // messages and conflating them would delete an order that never
    // existed, so the tag must not be borrowed.
    check_eq_int(static_cast<long long>(itch::off::kBrokenTradeSize),
                 static_cast<long long>(itch::off::kOrderDeleteSize),
                 "Broken Trade and Order Delete are both 19 bytes");
    check(itch::off::broken_trade_match != itch::off::order_delete_id ||
              static_cast<int>(itch::MessageType::broken_trade) !=
                  static_cast<int>(itch::MessageType::order_delete),
          "but they are distinct tags, so identical size is not conflation");
}

// ---- The other decoded message types --------------------------------

void test_decode_mutations() {
    std::printf("decode X/D/E/C/U\n");
    using namespace hft;

    constexpr OrderId kId = 0x1111'2222'3333'4444ULL;

    // ITCH 'X' Order Cancel: partial, deducts shares.
    {
        std::vector<std::uint8_t> buf;
        feed::append_order_cancel(buf, kId, Quantity::from_raw(75), 0x0000'0000'0000'2710ULL);
        check_eq_int(static_cast<long long>(buf.size()), 25, "cancel frame is 25 bytes");
        const auto r = itch::decode(buf.data(), buf.size());
        check(r.ok(), "cancel decodes");
        const auto* c = std::get_if<itch::OrderCancel>(&r.message.body);
        check(c != nullptr, "payload is an OrderCancel");
        if (c != nullptr) {
            check(c->id == kId, "cancel id round trips");
            check_eq_int(static_cast<long long>(c->shares.raw()), 75, "cancel shares round trip");
            check(c->timestamp == 0x2710ULL, "cancel timestamp round trips");
        }
    }

    // ITCH 'D' Order Delete: whole order, no share field at all.
    {
        std::vector<std::uint8_t> buf;
        feed::append_order_delete(buf, kId, 999);
        check_eq_int(static_cast<long long>(buf.size()), 21, "delete frame is 21 bytes");
        const auto r = itch::decode(buf.data(), buf.size());
        check(r.ok(), "delete decodes");
        const auto* d = std::get_if<itch::OrderDelete>(&r.message.body);
        check(d != nullptr, "payload is an OrderDelete");
        if (d != nullptr) {
            check(d->id == kId, "delete id round trips");
        }
    }

    // ITCH 'E' Order Executed.
    {
        std::vector<std::uint8_t> buf;
        feed::append_order_executed(buf, kId, Quantity::from_raw(40), 0x2AULL,
                                    /*match_number=*/0xFEED'FACE'0000'0001ULL);
        check_eq_int(static_cast<long long>(buf.size()), 34, "executed frame is 34 bytes");
        const auto r = itch::decode(buf.data(), buf.size());
        check(r.ok(), "executed decodes");
        const auto* e = std::get_if<itch::OrderExecuted>(&r.message.body);
        check(e != nullptr, "payload is an OrderExecuted");
        if (e != nullptr) {
            check_eq_int(static_cast<long long>(e->shares.raw()), 40, "executed shares round trip");
            check(e->match_number == 0xFEED'FACE'0000'0001ULL, "match number round trips");
        }
    }

    // ITCH 'C' Order Executed With Price: carries the fill price, which
    // must NOT be applied to the resting order's own limit price.
    {
        std::vector<std::uint8_t> buf;
        feed::append_order_executed_at_price(buf, kId, Quantity::from_raw(12),
                                              Price::from_raw(1'000'500), 7,
                                              /*match_number=*/0xABCD'0000'0000'0001ULL);
        check_eq_int(static_cast<long long>(buf.size()), 38, "exec@price frame is 38 bytes");
        const auto r = itch::decode(buf.data(), buf.size());
        check(r.ok(), "exec@price decodes");
        const auto* e = std::get_if<itch::OrderExecutedAtPrice>(&r.message.body);
        check(e != nullptr, "payload is an OrderExecutedAtPrice");
        if (e != nullptr) {
            check_eq_int(e->execution_price.raw(), 1'000'500, "execution price round trips");
            check_eq_int(static_cast<long long>(e->shares.raw()), 12, "shares round trip");
        }
    }

    // ITCH 'U' Order Replace: deliberately NOT decoded. The field table
    // was not verified, so the frame must be skipped by length rather
    // than parsed on a guess.
    {
        std::vector<std::uint8_t> buf;
        feed::append_order_replace(buf, kId, kId + 1, Quantity::from_raw(10),
                                   Price::from_raw(100'000), 0);
check_eq_int(static_cast<long long>(buf.size()), 37,
                 "replace frame is 37 bytes (35 body plus length prefix)");
    const auto r = itch::decode(buf.data(), buf.size());
    // It used to be asserted that this frame is SKIPPED as an unknown
    // type, which was true while the layout was unverified and false as
    // soon as it was verified against section 4.4.5. The generator used
    // to write four phantom order-entry bytes here and the skip hid it.
    check(r.ok(), "order replace is now decoded, not skipped");
    if (r.ok()) {
        const auto* u = std::get_if<itch::OrderReplace>(&r.message.body);
        check(u != nullptr, "the generated replace decodes to OrderReplace");
        if (u != nullptr) {
            check(u->original_id == kId, "the generated replace carries the original reference");
            check(u->new_id == kId + 1, "and the new reference");
        }
    }
    check_eq_int(static_cast<long long>(itch::frame_stride(r)), 37,
                 "replace frame stride is the full frame");
    }

    // A known tag whose declared length disagrees with its own layout
    // must be rejected. The body has to be long enough to satisfy the
    // declared length, or the decoder would (correctly) report
    // truncation before ever reaching the length check.
    {
        std::vector<std::uint8_t> body;
        feed::append_add_order(body, Side::bid, Price::from_raw(1'000'000),
                               Quantity::from_raw(10), kId, 0);
        // Retag as 'D' (19-byte body) while the 32-byte length stands.
        body[2] = static_cast<std::uint8_t>(itch::MessageType::order_delete);
        std::vector<std::uint8_t> bad;
        feed::append_frame_with_length(bad, body.data() + 2, body.size() - 2, 32);
        const auto r = itch::decode(bad.data(), bad.size());
        check(r.status == itch::DecodeStatus::bad_length,
              "'D' tagged with an 'A' length is rejected");
    }
}

// ---- Sequence tracking ----------------------------------------------

void test_sequence() {
    std::printf("sequence tracker\n");
    using namespace hft;

    // Contiguous stream across the sequence wrap, which is where naive
    // `observed == expected + 1` comparisons fail.
    //
    // The wrap is 64 bits wide because that is the width of the field:
    // the MoldUDP64 Sequence Number is eight bytes, asserted in
    // include/hft/itch/moldudp64.hpp against the specification's field
    // table. An earlier revision of this test wrapped at 32 bits,
    // matching a tracker that was also 32 bits -- and both were wrong,
    // because the high half of every sequence number was being discarded
    // before anything downstream saw it.
    //
    // The arithmetic argument does not depend on the width: modular
    // comparison is correct at the wrap and nowhere else ambiguous,
    // provided the gap being measured is under half the space.
    {
        constexpr std::uint64_t kMax = 0xFFFF'FFFF'FFFF'FFFFULL;
        itch::SequenceTracker t(kMax - 1u);
        check(t.observe(kMax - 1u) == itch::SequenceTracker::State::ok, "first is ok");
        check(t.observe(kMax) == itch::SequenceTracker::State::ok, "second is ok");
        check(t.observe(0) == itch::SequenceTracker::State::ok,
              "wrap from the maximum to zero is ok, not a gap");
        check(t.observe(1) == itch::SequenceTracker::State::ok, "past the wrap is ok");
        check(t.clean(), "wrapped stream is clean");
        check_eq_int(static_cast<long long>(t.missing()), 0, "nothing missing across the wrap");
        check_eq_int(static_cast<long long>(t.accepted()), 4, "all four messages accepted");
    }

    // A gap that spans the wrap must still be counted, and counted with
    // the right magnitude. Losing the wrap makes a forward jump look
    // like a backwards one -- the one case modular arithmetic cannot
    // rescue, because the sign of the jump is genuinely ambiguous.
    {
        constexpr std::uint64_t kMax = 0xFFFF'FFFF'FFFF'FFFFULL;
        itch::SequenceTracker t(kMax);
        check(t.observe(kMax) == itch::SequenceTracker::State::ok, "at the maximum is ok");
        // Accepting the maximum advanced the expectation to 0. Seeing
        // sequence 2 therefore means 0 and 1 were lost: two, not three.
        check(t.observe(2) == itch::SequenceTracker::State::gap,
              "a forward jump across the wrap is a gap, not a duplicate");
        check_eq_int(static_cast<long long>(t.missing()), 2, "two messages missing");
        check_eq_int(static_cast<long long>(t.expected()), 3,
                     "and the expectation lands just past what was observed");
    }

    // Whole-packet observation, which is the entry point a real handler
    // uses: a handler is handed packets, not messages, and the sequence
    // field applies to the FIRST block of a packet.
    {
        itch::SequenceTracker t(1'000);
        check(t.observe_packet(1'000, 10) == itch::SequenceTracker::State::ok,
              "a packet starting where we expect is ok");
        check_eq_int(static_cast<long long>(t.accepted()), 10,
                     "and all ten of its messages are counted");
        check_eq_int(static_cast<long long>(t.expected()), 1'010,
                     "and the expectation advanced by the message count");
        check(t.clean(), "a contiguous packet stream is clean");
    }
    {
        // A gap of whole packets: 10 messages expected, next packet
        // starts 10 further on, so ten messages were lost.
        itch::SequenceTracker t(1'000);
        (void)t.observe_packet(1'000, 10);
        check(t.observe_packet(1'020, 5) == itch::SequenceTracker::State::gap,
              "a packet starting late is a gap");
        check_eq_int(static_cast<long long>(t.missing()), 10,
                     "and the loss is counted in messages, not packets");
        check_eq_int(static_cast<long long>(t.gaps()), 1, "one gap event");
    }
    {
        // A duplicate packet must not be counted, nor its implicit
        // tail: the whole packet has already been seen.
        itch::SequenceTracker t(1'000);
        (void)t.observe_packet(1'000, 10);
        (void)t.observe_packet(1'010, 10);
        check_eq_int(static_cast<long long>(t.accepted()), 20, "twenty messages so far");
        check(t.observe_packet(1'010, 10) == itch::SequenceTracker::State::duplicate,
              "a repeated packet is a duplicate");
        check_eq_int(static_cast<long long>(t.accepted()), 20,
                     "and contributes nothing to the accepted count");
    }
    {
        // A heartbeat carries the next expected sequence and no
        // messages. Reporting the difference is the whole point: a
        // heartbeat arriving mid-stream is the sender telling us we
        // have missed something, and accepting it silently would hide
        // exactly the event this class exists to surface.
        itch::SequenceTracker t(1'000);
        (void)t.observe_packet(1'000, 10);
        check(t.observe_packet(1'010, 0) == itch::SequenceTracker::State::ok,
              "a heartbeat agreeing with our position is ok");
        check_eq_int(static_cast<long long>(t.accepted()), 10,
                     "and contributes no messages of its own");
        check(t.observe_packet(1'100, 0) == itch::SequenceTracker::State::gap,
              "a heartbeat ahead of us means messages were lost");
        check_eq_int(static_cast<long long>(t.missing()), 90,
                     "and the shortfall is counted");
    }

    // Forward gap.
    {
        itch::SequenceTracker t(100);
        check(t.observe(100) == itch::SequenceTracker::State::ok, "100 ok");
        check(t.observe(105) == itch::SequenceTracker::State::gap, "105 is a gap");
        check_eq_int(static_cast<long long>(t.missing()), 4, "four messages missing");
        check_eq_int(static_cast<long long>(t.gaps()), 1, "one gap event");
        check(!t.clean(), "stream is not clean");
        check_eq_int(static_cast<long long>(t.expected()), 106, "expected advances past the gap");
    }

    // Retransmit and stale packet. Both are backwards jumps and the
    // sequence number cannot distinguish them, so both report
    // `duplicate` and neither advances the expectation.
    {
        itch::SequenceTracker t(100);
        check(t.observe(100) == itch::SequenceTracker::State::ok, "100 is ok");
        check(t.observe(99) == itch::SequenceTracker::State::duplicate, "99 is a duplicate");
        check_eq_int(static_cast<long long>(t.expected()), 101, "a duplicate does not advance the expectation");
        check(t.observe(50) == itch::SequenceTracker::State::duplicate, "50 is also a duplicate");
        check(!t.clean(), "a retransmitted packet makes the stream unclean");
    }
}

// ---- Book mutations through the apply path --------------------------

void test_apply_path() {
    std::printf("apply path\n");
    using namespace hft;

    lob::OrderBook book(64, 16);
    constexpr OrderId kId = 777;

    lob::BookStatus st{};
    book.add(Side::bid, Price::from_raw(1'000'000), Quantity::from_raw(100), kId, st);
    check(st == lob::BookStatus::ok, "add through book api");

    // Drive the same changes through decoded messages, so the apply
    // layer is covered rather than only the book API.
    {
        std::vector<std::uint8_t> buf;
        feed::append_order_cancel(buf, kId, Quantity::from_raw(40), 1);
        const auto r = itch::decode(buf.data(), buf.size());
        const auto ar = lob::apply(r.message, book);
        check(ar.applied, "cancel applied");
        const auto snap = book.find(kId);
        check(snap.has_value() && snap->size.raw() == 60,
              "partial cancel left 60, and the order kept its place");
    }
    {
        std::vector<std::uint8_t> buf;
        feed::append_order_executed(buf, kId, Quantity::from_raw(60), 2);
        const auto r = itch::decode(buf.data(), buf.size());
        const auto ar = lob::apply(r.message, book);
        check(ar.applied, "execute applied");
        check(!book.find(kId).has_value(), "fully executed order is gone");
    }

    // A mutation for an order that is not resting is reported, not
    // fatal: in a live feed the add that created it may have been lost
    // in an earlier gap.
    {
        std::vector<std::uint8_t> buf;
        feed::append_order_delete(buf, kId, 3);
        const auto r = itch::decode(buf.data(), buf.size());
        const auto ar = lob::apply(r.message, book);
        check(!ar.applied, "delete of an absent order does not apply");
        check(ar.detail == lob::BookStatus::unknown_order, "and reports unknown_order");
        check(ar.ok(), "which is not treated as an error");
    }

    // 'X' that consumes the whole remainder must remove the order, and
    // is still not the same operation as 'D'.
    {
        constexpr OrderId kId2 = 778;
        book.add(Side::ask, Price::from_raw(1'000'100), Quantity::from_raw(50), kId2, st);
        std::vector<std::uint8_t> buf;
        feed::append_order_cancel(buf, kId2, Quantity::from_raw(50), 4);
        const auto r = itch::decode(buf.data(), buf.size());
        check(lob::apply(r.message, book).applied, "full-size partial cancel applied");
        check(!book.find(kId2).has_value(), "full-size X removes the order");
    }
}

}  // namespace

// ---- Benchmark reporting ----------------------------------------------

// These are the functions that format the numbers this project reports.
// They were never tested, and `humanize` was wrong: an unsigned
// underflow put a thousands separator between the digits of every
// two-digit number, so "23 mid moves" printed as "2 3 mid moves". A
// formatting bug in the reporting layer is unusually bad, because it
// only shows up in the artifact somebody is meant to trust -- and a
// reader who sees "2 3 mid moves" has no way to know it means 23 rather
// than 2 and 3.

void test_humanize() {
    std::printf("humanize\n");
    using bench::humanize;

    // Every digit count, including the 1- and 2-digit cases that broke.
    struct Case {
        std::uint64_t value;
        const char* expected;
    };
    const Case cases[] = {
        {0, "0"},
        {1, "1"},
        {7, "7"},
        {9, "9"},
        {10, "10"},
        {23, "23"},
        {99, "99"},
        {100, "100"},
        {101, "101"},
        {999, "999"},
        {1000, "1 000"},
        {1471, "1 471"},
        {9999, "9 999"},
        {10000, "10 000"},
        {99999, "99 999"},
        {100000, "100 000"},
        {999999, "999 999"},
        {1000000, "1 000 000"},
        {1471000, "1 471 000"},
        {1999998, "1 999 998"},
        {2000000, "2 000 000"},
        {123456789, "123 456 789"},
    };

    bool ok = true;
    for (const Case& c : cases) {
        if (humanize(c.value) != c.expected) {
            std::printf("  FAIL  humanize(%llu): expected \"%s\", got \"%s\"\n",
                        static_cast<unsigned long long>(c.value), c.expected,
                        humanize(c.value).c_str());
            ok = false;
        }
    }
    check(ok, "humanize groups every magnitude correctly");

    // The property that actually matters, over a wide sweep: a number
    // never changes value when separators are removed, and the only
    // characters added are separators.
    bool property_ok = true;
    for (std::uint64_t v = 0; v < 20'000 && property_ok; ++v) {
        const std::string s = humanize(v);
        std::string stripped;
        for (const char ch : s) {
            if (ch != ' ') {
                if (ch < '0' || ch > '9') {
                    property_ok = false;
                    break;
                }
                stripped.push_back(ch);
            }
        }
        if (stripped != std::to_string(v)) {
            property_ok = false;
        }
    }
    check(property_ok, "humanize adds only separators and never changes the value");

    // Values beyond 32 bits. Grouping is positional, not arithmetic, so
    // width is no obstacle; what has to hold is that stripping the
    // separators recovers the digits exactly.
    bool large_ok = true;
    for (int shift = 3; shift <= 63; shift += 3) {
        const std::uint64_t v = 1ULL << shift;
        const std::string printed = humanize(v);
        std::string stripped;
        for (const char ch : printed) {
            if (ch != ' ') {
                stripped.push_back(ch);
            }
        }
        if (stripped != std::to_string(v)) {
            std::printf("  FAIL  humanize(2^%d): stripped \"%s\" != \"%s\"\n", shift,
                        stripped.c_str(), std::to_string(v).c_str());
            large_ok = false;
        }
    }
    check(large_ok, "humanize handles values up to 64 bits");

    check(humanize(4096) == "4 096", "four digits group from the right");
    check(humanize(32768) == "32 768", "five digits group from the right");
    check(humanize(0xFFFFFFFF) == "4 294 967 295", "the largest 32-bit value groups correctly");
    check(humanize(0xFFFFFFFFFFFFFFFFULL) == "18 446 744 073 709 551 615",
          "the largest 64-bit value groups correctly");
}

void test_report_percentiles() {
    std::printf("report percentiles\n");
    using hft::util::LatencyHistogram;

    // The percentile numbers are the headline of Phase 6, and they are
    // computed from a bucketed histogram rather than stored samples.
    // The bucket boundaries therefore decide what the reader is told,
    // so the boundaries are checked rather than assumed.
    // The histogram reports the UPPER BOUND of the bucket a sample landed
    // in, not the sample. That is deliberate: an upper bound can never
    // read optimistically low, whereas an interpolated value would be a
    // number that was never measured. A constant 500 in a one-nanosecond
    // histogram therefore reports 501. Asserted here so the choice is
    // visible rather than discovered later in a results table.
    {
        LatencyHistogram h(1, 100'000);
        for (int i = 0; i < 1000; ++i) {
            h.record(500);
        }
        check_eq_int(static_cast<long long>(h.percentile(0.50)), 501,
                     "a constant reports the upper bound of its bucket");
        check_eq_int(static_cast<long long>(h.percentile(0.99)), 501, "p99 of a constant");
        check_eq_int(static_cast<long long>(h.percentile(0.999)), 501, "p999 of a constant");
        check_eq_int(static_cast<long long>(h.count()), 1000, "sample count is exact");

        // The reported value is always at or above the truth, and never
        // more than one bucket above it. A histogram that reported
        // below the true value would be lying in the dangerous
        // direction: a reader would conclude the stage is faster than it
        // is.
        LatencyHistogram g(4, 10'000);
        for (int i = 0; i < 500; ++i) {
            g.record(777);
        }
        const std::uint64_t reported = g.percentile(0.50);
        check(reported >= 777 && reported <= 777 + 4,
              "a reported percentile is within one bucket above the true value");
    }

    {
        // Percentiles must be monotonic in the requested quantile.
        // A histogram that reported p999 below p50 would print a table
        // that looks plausible and is not.
        LatencyHistogram h(1, 100'000);
        for (int i = 0; i < 100'000; ++i) {
            h.record(static_cast<std::uint64_t>(i % 1000));
        }
        bool monotonic = true;
        std::uint64_t previous = 0;
        for (int q = 50; q <= 999; q += 1) {
            const std::uint64_t p = h.percentile(static_cast<double>(q) / 1000.0);
            if (p < previous) {
                monotonic = false;
                break;
            }
            previous = p;
        }
        check(monotonic, "percentiles are non-decreasing across the full range");

        // A uniform distribution over 0..999 has its median near 500.
        const std::uint64_t p50 = h.percentile(0.50);
        check(p50 >= 480 && p50 <= 520,
              "the median of a uniform 0..999 distribution is near 500");

        // p99 and p999 must land near the top of that range.
        check(h.percentile(0.99) >= 970, "p99 of a uniform 0..999 distribution is near 990");
        check(h.percentile(0.999) >= 990, "p999 of a uniform 0..999 distribution is near 999");
    }

    {
        // Overflow must be counted, not absorbed. An absorbed overflow
        // is a censored sample presented as a measurement.
        LatencyHistogram h(1, 100);
        for (int i = 0; i < 90; ++i) {
            h.record(10);
        }
        for (int i = 0; i < 10; ++i) {
            h.record(100'000);  // beyond the range
        }
        check_eq_int(static_cast<long long>(h.overflow_count()), 10,
                     "samples beyond the range are counted as overflow");

        // A percentile whose rank falls into the overflow region must
        // report the documented ceiling, not a low in-range value.
        // bucket_count is max_ns / width + 1, so for (1, 100) the
        // ceiling is 101.
        check_eq_int(static_cast<long long>(h.percentile(0.999)), 101,
                     "a censored p999 reports the ceiling, visibly high");

        // The in-range percentiles are unaffected by the overflow, and
        // report 11 for a true 10 because the bucket's upper bound is
        // 11. One nanosecond pessimistic, in the safe direction.
        check_eq_int(static_cast<long long>(h.percentile(0.50)), 11,
                     "an in-range p50 is exact to the bucket's upper bound");
    }

    {
        // An empty histogram must not divide by zero or invent a value.
        LatencyHistogram h(1, 1000);
        check_eq_int(static_cast<long long>(h.count()), 0, "an empty histogram has no samples");
        check_eq_int(static_cast<long long>(h.percentile(0.5)), 0,
                     "an empty histogram reports zero rather than guessing");
        check_eq_int(static_cast<long long>(h.overflow_count()), 0,
                     "an empty histogram has no overflow");
    }

    {
        // min() and max() are exact, not bucketed, and are what a reader
        // uses to confirm the distribution's shape. max() must see the
        // overflowed samples too -- an outlier that landed outside the
        // range is still the maximum, and hiding it would make a
        // pathological tail invisible.
        LatencyHistogram h(1, 1000);
        h.record(5);
        h.record(900);
        h.record(50'000);  // overflows
        check_eq_int(static_cast<long long>(h.min()), 5, "min is exact");
        check_eq_int(static_cast<long long>(h.max()), 50000,
                     "max includes samples beyond the histogram range");
        check_eq_int(static_cast<long long>(h.overflow_count()), 1, "the outlier overflowed");
        check(h.percentile(1.0) == 50000, "p100 reports max");
    }
}

int main() {
    std::printf("unit tests\n----------\n");
    test_price_parse();
    test_timestamp48();
    test_big_endian();
    test_moldudp64_against_spec();
    test_add_order_layout_is_spec();
    test_decode();
    test_decode_mutations();
    test_sequence();
    test_apply_path();
    test_generated_feed();
    test_capture_round_trip();
    test_capture_multi_message_packets();
    test_capture_rejects_truncation();
    test_capture_handles_control_packets();
    test_humanize();
    test_report_percentiles();
    test_order_replace_layout_is_spec();
    test_broken_trade_layout_is_spec();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
