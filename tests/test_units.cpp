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

void test_decode() {
    std::printf("frame decode\n");
    using namespace hft;

    std::vector<std::uint8_t> buf;
    feed::append_add_order(buf, Side::bid, Price::from_raw(1234500), Quantity::from_raw(300),
                           0xABCD'EF01'2345'6789ULL, 0x0000'0001'2BAD'F00DULL);

    check_eq_int(static_cast<long long>(buf.size()), 34, "Add Order frame is 34 bytes");
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
        check_eq_int(itch::frame_stride(r), 34, "stride is the full frame");
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
        check_eq_int(static_cast<long long>(buf.size()), 22, "cancel frame is 22 bytes");
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
        check_eq_int(static_cast<long long>(buf.size()), 18, "delete frame is 18 bytes");
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
        check_eq_int(static_cast<long long>(buf.size()), 38, "replace frame is 38 bytes");
        const auto r = itch::decode(buf.data(), buf.size());
        check(r.status == itch::DecodeStatus::unknown_type,
              "order replace is skipped, not guessed at");
        check(r.skippable(), "order replace is skippable");
        check_eq_int(itch::frame_stride(r), 38, "replace skip stride is the full frame");
    }

    // A length that disagrees with the tag must be rejected even though
    // the tag is known.
    {
        std::vector<std::uint8_t> good;
        feed::append_order_delete(good, kId, 0);
        std::vector<std::uint8_t> bad;
        feed::append_frame_with_length(bad, good.data() + 2, good.size() - 2, 32);
        const auto r = itch::decode(bad.data(), bad.size());
        check(r.status == itch::DecodeStatus::bad_length,
              "'D' tagged with an 'E' length is rejected");
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
        check_eq_int(t.expected(), 106, "expected advances past the gap");
    }

    // Retransmit and stale packet.
    {
        itch::SequenceTracker t(100);
        t.observe(100);
        check(t.observe(99) == itch::SequenceTracker::State::duplicate, "99 is a duplicate");
        check_eq_int(t.expected(), 101, "a duplicate does not advance the expectation");
        check(t.observe(50) == itch::SequenceTracker::State::out_of_order, "50 is out of order");
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
    test_decode();
    test_decode_mutations();
    test_sequence();
    test_apply_path();
    test_generated_feed();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
