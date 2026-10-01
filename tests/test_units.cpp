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

}  // namespace

int main() {
    std::printf("unit tests\n----------\n");
    test_price_parse();
    test_timestamp48();
    test_big_endian();
    test_decode();
    test_generated_feed();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
