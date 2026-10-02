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
        check_eq_int(static_cast<long long>(buf.size()), 41, "replace frame is 41 bytes");
        const auto r = itch::decode(buf.data(), buf.size());
        check(r.status == itch::DecodeStatus::unknown_type,
              "order replace is skipped, not guessed at");
        check(r.skippable(), "order replace is skippable");
        check_eq_int(static_cast<long long>(itch::frame_stride(r)), 41,
                   "replace skip stride is the full frame");
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

    // Contiguous stream across the 32-bit wrap, which is where naive
    // `observed == expected + 1` comparisons fail.
    {
        itch::SequenceTracker t(0xFFFF'FFFEu);
        check(t.observe(0xFFFF'FFFEu) == itch::SequenceTracker::State::ok, "first is ok");
        check(t.observe(0xFFFF'FFFFu) == itch::SequenceTracker::State::ok, "second is ok");
        check(t.observe(0x0000'0000u) == itch::SequenceTracker::State::ok,
              "wrap from FFFFFFFF to 00000000 is ok, not a gap");
        check(t.observe(0x0000'0001u) == itch::SequenceTracker::State::ok, "past the wrap is ok");
        check(t.clean(), "wrapped stream is clean");
        check_eq_int(static_cast<long long>(t.missing()), 0, "nothing missing across the wrap");
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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
