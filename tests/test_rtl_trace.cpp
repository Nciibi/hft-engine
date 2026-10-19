// Tests for the decode trace: the format, the comparator, and the
// bridge from the engine's decoder to a trace record.
//
// Deliberately dependency-free, like every other test in this
// repository. The harness is twenty lines and reports the same
// information.
//
// The suite is built around one idea: **a comparison that cannot fail is
// not a comparison.** Every test that builds a trace also builds a
// corrupted copy of it and asserts that the comparator notices, because
// the whole purpose of the differential harness is to catch a
// disagreement between two implementations, and a comparator that
// silently returns "identical" would make every future differential
// result meaningless.

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/itch/moldudp64.hpp"
#include "hft/itch/protocol.hpp"

#include "../src/rtl/capture_walk.hpp"
#include "../src/rtl/decode_trace.hpp"
#include "../src/rtl/trace.hpp"

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

void check_eq_str(const std::string& actual, const std::string& expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL  %s:\n        expected %s\n        actual   %s\n", what, expected.c_str(),
                    actual.c_str());
    }
}

using hft::rtl::TraceRecord;
using hft::rtl::TraceStatus;

/// A frame: the two-byte length prefix and the body.
[[nodiscard]] std::vector<std::uint8_t> frame_of(const std::uint8_t* body, std::size_t length) {
    std::vector<std::uint8_t> frame;
    frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>(length & 0xFFU));
    frame.insert(frame.end(), body, body + length);
    return frame;
}

/// The record for one frame, built the way the tool builds it.
[[nodiscard]] bool record_for_frame(const std::vector<std::uint8_t>& frame, TraceRecord& out,
                                    std::string& error) {
    const hft::itch::DecodeResult result = hft::itch::decode(frame.data(), frame.size());
    return hft::rtl::record_for(0, result, out, error);
}

// ---- The format -----------------------------------------------------

void test_format() {
    std::printf("trace format\n");

    TraceRecord record;
    record.index = 42;
    record.status = TraceStatus::decoded;
    record.type = 'A';
    record.has_type = true;
    record.length = 36;
    record.fields = {"04d2", "0000", "1f1aced9f07b", "000000000000002a",
                     "42",   "00000064", "53494d5445535420", "0016ed24"};

    check_eq_str(hft::rtl::format_record(record),
                 "42 D 41 0036 04d2 0000 1f1aced9f07b 000000000000002a 42 00000064 "
                 "53494d5445535420 0016ed24",
                 "a decoded record renders as index, status, type, length, fields");

    // A record with no type says so with a double dash rather than with
    // a zero, because zero is a legal tag byte and "--" is not.
    TraceRecord untyped;
    untyped.index = 7;
    untyped.status = TraceStatus::truncated;
    untyped.length = 36;
    check_eq_str(hft::rtl::format_record(untyped), "7 T -- 0036",
                 "a record with no type renders the type as --");
}

// ---- Round trip -----------------------------------------------------

void test_round_trip() {
    std::printf("trace round trip\n");

    TraceRecord original;
    original.index = 123456;
    original.status = TraceStatus::decoded;
    original.type = 'E';
    original.has_type = true;
    original.length = 32;
    original.fields = {"0001", "0002", "000000000001", "0000000000000abc",
                       "00000064", "0000000000000001", "01"};

    std::istringstream in(hft::rtl::format_record(original) + "\n");
    TraceRecord parsed;
    std::string error;
    check(hft::rtl::read_record(in, parsed, error), "a formatted record parses back");
    check_eq_str(hft::rtl::format_record(parsed), hft::rtl::format_record(original),
                 "the round trip is lossless");
    check_eq_int(static_cast<long long>(parsed.fields.size()),
                 static_cast<long long>(original.fields.size()), "the field count survives");
    check_eq_int(static_cast<long long>(parsed.length), 32, "the length survives");
    check(parsed.has_type, "the type survives");
    check_eq_int(static_cast<long long>(parsed.type), static_cast<long long>('E'),
                 "the type byte survives");

    // Comments and blank lines are skipped, so a trace can be annotated.
    std::istringstream annotated("# a comment\n\n" + hft::rtl::format_record(original) + "\n");
    TraceRecord skipped;
    check(hft::rtl::read_record(annotated, skipped, error), "a commented trace still parses");
    check_eq_str(hft::rtl::format_record(skipped), hft::rtl::format_record(original),
                 "the record after a comment is the right one");

    // A malformed line is an error, not a silently accepted record.
    std::istringstream broken("not a trace record at all\n");
    TraceRecord ignored;
    check(!hft::rtl::read_record(broken, ignored, error), "a malformed line is rejected");
    check(!error.empty(), "a malformed line produces an error message");

    std::istringstream bad_status("0 X -- 0000\n");
    check(!hft::rtl::read_record(bad_status, ignored, error), "an unknown status is rejected");
}

// ---- Validation -----------------------------------------------------

void test_validation() {
    std::printf("trace validation\n");

    TraceRecord good;
    good.index = 0;
    good.status = TraceStatus::decoded;
    good.type = 'D';
    good.has_type = true;
    good.length = 19;
    good.fields = {"0001", "0002", "000000000001", "0000000000000abc"};

    std::string error;
    check(hft::rtl::validate_record(good, error), "a well-formed record validates");

    // A field count that does not match the type is the failure a
    // decoder that dropped a field would produce.
    TraceRecord short_record = good;
    short_record.fields.pop_back();
    check(!hft::rtl::validate_record(short_record, error), "a short field list is rejected");

    // A field rendered at the wrong width is worse than a missing one:
    // two implementations that made the same mistake would agree.
    TraceRecord wide = good;
    wide.fields[3] = "0000000000000abc00";
    check(!hft::rtl::validate_record(wide, error), "a field at the wrong width is rejected");

    // A non-decoded record carries no fields.
    TraceRecord with_fields = good;
    with_fields.status = TraceStatus::skipped;
    check(!hft::rtl::validate_record(with_fields, error), "a skipped record carries no fields");

    // A decoded record with no type is meaningless.
    TraceRecord untyped = good;
    untyped.has_type = false;
    check(!hft::rtl::validate_record(untyped, error), "a decoded record must have a type");
}

// ---- The comparator, and its ability to fail ------------------------

void test_comparator() {
    std::printf("trace comparator\n");

    TraceRecord a;
    a.index = 0;
    a.status = TraceStatus::decoded;
    a.type = 'A';
    a.has_type = true;
    a.length = 36;
    a.fields = {"04d2", "0000", "1f1aced9f07b", "000000000000002a",
                "42",   "00000064", "53494d5445535420", "0016ed24"};

    std::vector<TraceRecord> left{a};
    std::vector<TraceRecord> right{a};
    std::vector<hft::rtl::TraceDifference> differences;

    check(hft::rtl::compare_traces(left, right, differences, 20),
          "two identical traces compare equal");
    check_eq_int(static_cast<long long>(differences.size()), 0, "and report no differences");

    // THE NEGATIVE TEST. A corrupted field must be reported, and the
    // report must name the field. If this stopped working, every
    // future differential would be reporting agreement it had not
    // checked.
    right[0].fields[7] = "0016ed25";
    check(!hft::rtl::compare_traces(left, right, differences, 20),
          "a corrupted field value is detected");
    check_eq_int(static_cast<long long>(differences.size()), 1, "one difference is reported");
    if (!differences.empty()) {
        check_eq_str(differences[0].what, "field price", "the differing field is named");
        check_eq_str(differences[0].expected, "0016ed24", "the expected value is reported");
        check_eq_str(differences[0].actual, "0016ed25", "the actual value is reported");
    }

    // A status difference.
    right = left;
    right[0].status = TraceStatus::skipped;
    check(!hft::rtl::compare_traces(left, right, differences, 20), "a status difference is detected");
    if (!differences.empty()) {
        check_eq_str(differences[0].what, "status", "the status difference is named");
    }

    // A type difference.
    right = left;
    right[0].type = 'E';
    check(!hft::rtl::compare_traces(left, right, differences, 20), "a type difference is detected");
    if (!differences.empty()) {
        check_eq_str(differences[0].what, "type", "the type difference is named");
    }

    // A length difference.
    right = left;
    right[0].length = 35;
    check(!hft::rtl::compare_traces(left, right, differences, 20), "a length difference is detected");
    if (!differences.empty()) {
        check_eq_str(differences[0].what, "length", "the length difference is named");
    }

    // A field count difference: an implementation that extracted a
    // different set of fields.
    right = left;
    right[0].fields.pop_back();
    check(!hft::rtl::compare_traces(left, right, differences, 20),
          "a field count difference is detected");
    if (!differences.empty()) {
        check_eq_str(differences[0].what, "field count", "the field count difference is named");
    }

    // A record count difference. Two traces of different lengths cannot
    // be compared field by field, and saying so is more useful than
    // reporting every record past the shorter one as missing.
    right = left;
    right.push_back(a);
    right[1].index = 1;
    check(!hft::rtl::compare_traces(left, right, differences, 20),
          "a record count difference is detected");
    if (!differences.empty()) {
        check_eq_str(differences[0].what, "record count", "the record count difference is named");
    }
}

// ---- The decoder bridge ---------------------------------------------

void test_add_order_trace() {
    std::printf("Add Order trace\n");

    // Built by hand, byte by byte, from the Nasdaq TotalView-ITCH 5.0
    // field table and nowhere else -- the same discipline as the
    // hand-built frame in test_units.cpp, and for the same reason.
    //
    // An earlier revision of this repository read the share count as the
    // price, and every round-trip test passed, because the generator
    // wrote the same wrong layout the decoder read. A trace built from
    // a hand-written frame is the check that does not share an
    // assumption with the thing under test.
    //
    // Spec section 1.3.1, Add Order - No MPID Attribution:
    //   0 tag | 1 locate | 3 tracking | 5 timestamp | 11 reference
    //   19 side | 20 SHARES | 24 stock symbol | 32 PRICE
    const std::uint8_t body[hft::itch::off::kAddOrderSize] = {
        static_cast<std::uint8_t>('A'),           // 0
        0x04, 0xD2,                                 // 1  locate 1234
        0x00, 0x00,                                 // 3  tracking 0
        0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x7B,        // 5  timestamp
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,  // 11 reference 42
        static_cast<std::uint8_t>('B'),           // 19 side
        0x00, 0x00, 0x00, 0x64,                    // 20 SHARES 100
        'S', 'I', 'M', 'T', 'E', 'S', 'T', ' ',   // 24 stock symbol
        0x00, 0x16, 0xED, 0x24,                    // 32 PRICE 1502500
    };
    const std::vector<std::uint8_t> frame = frame_of(body, sizeof(body));

    TraceRecord record;
    std::string error;
    check(record_for_frame(frame, record, error), "the hand-built Add Order builds a record");
    check(record.status == TraceStatus::decoded, "the hand-built Add Order decodes");
    check_eq_int(static_cast<long long>(record.length), 36, "the declared length is 36");

    // The whole trace in one string. This pins the format, the field
    // order, the field widths, and the value at every position, in one
    // assertion.
    check_eq_str(hft::rtl::format_record(record),
                 "0 D 41 0036 04d2 0000 1f1aced9f07b 000000000000002a 42 00000064 "
                 "53494d5445535420 0016ed24",
                 "the Add Order trace matches the field table, position by position");

    // The two fields this repository once swapped, called out by name.
    check_eq_str(record.fields[5], "00000064", "field 5 is SHARES, not the price");
    check_eq_str(record.fields[7], "0016ed24", "field 7 is the PRICE, not the shares");
    check_eq_str(record.fields[6], "53494d5445535420", "field 6 is the stock symbol");
}

void test_cancel_and_delete_differ() {
    std::printf("Order Cancel against Order Delete\n");

    // 'X' and 'D' are different operations, and conflating them is the
    // most common ITCH book bug. The trace distinguishes them by their
    // shape, so an implementation that treated one as the other would
    // produce a different trace.
    const std::uint8_t cancel_body[hft::itch::off::kOrderCancelSize] = {
        static_cast<std::uint8_t>('X'),
        0x00, 0x01,
        0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,  // reference 42
        0x00, 0x00, 0x00, 0x32,                              // shares 50
    };
    const std::uint8_t delete_body[hft::itch::off::kOrderDeleteSize] = {
        static_cast<std::uint8_t>('D'),
        0x00, 0x01,
        0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,  // reference 42
    };

    TraceRecord cancel_record;
    TraceRecord delete_record;
    std::string error;
    check(record_for_frame(frame_of(cancel_body, sizeof(cancel_body)), cancel_record, error),
          "the hand-built Order Cancel builds a record");
    check(record_for_frame(frame_of(delete_body, sizeof(delete_body)), delete_record, error),
          "the hand-built Order Delete builds a record");

    check(cancel_record.status == TraceStatus::decoded, "Order Cancel decodes");
    check(delete_record.status == TraceStatus::decoded, "Order Delete decodes");
    check_eq_int(static_cast<long long>(cancel_record.fields.size()), 5,
                 "Order Cancel carries five fields");
    check_eq_int(static_cast<long long>(delete_record.fields.size()), 4,
                 "Order Delete carries four fields");
    check_eq_str(cancel_record.fields[4], "00000032", "the cancel quantity is the fifth field");
    check_eq_str(cancel_record.fields[3], "000000000000002a", "both carry the same reference");

    // The two records must not compare equal.
    std::vector<hft::rtl::TraceDifference> differences;
    check(!hft::rtl::compare_traces({cancel_record}, {delete_record}, differences, 20),
          "an Order Cancel and an Order Delete are different traces");
}

void test_non_decoded_statuses() {
    std::printf("non-decoded statuses\n");

    // An unknown type: a well-formed frame of a type this build does
    // not decode. The tag is recorded, because a handler that skipped a
    // message needs to know what it skipped.
    const std::uint8_t unknown_body[5] = {static_cast<std::uint8_t>('F'), 1, 2, 3, 4};
    TraceRecord record;
    std::string error;
    check(record_for_frame(frame_of(unknown_body, sizeof(unknown_body)), record, error),
          "an unknown type builds a record");
    check(record.status == TraceStatus::skipped, "an unknown type is skipped");
    check(record.has_type, "an unknown type records its tag");
    check_eq_int(static_cast<long long>(record.type), static_cast<long long>('F'),
                 "the skipped tag is 'F'");
    check_eq_int(static_cast<long long>(record.length), 5, "the skipped length is recorded");

    // A declared length that disagrees with the type. The tag IS read
    // here, because the body is present and the decoder got as far as
    // reading it.
    std::vector<std::uint8_t> wrong_length = frame_of(unknown_body, sizeof(unknown_body));
    wrong_length[0] = 0;
    wrong_length[1] = 99;
    // Rebuild with an 'A' tag and a length of 35, which is not 36.
    std::uint8_t add_body[hft::itch::off::kAddOrderSize] = {};
    add_body[0] = static_cast<std::uint8_t>('A');
    std::vector<std::uint8_t> short_add = frame_of(add_body, sizeof(add_body));
    short_add[1] = 35;  // declare 35 bytes of body, supply 36
    check(record_for_frame(short_add, record, error), "a bad length builds a record");
    check(record.status == TraceStatus::bad_length, "a length that disagrees is bad length");
    check(record.has_type, "a bad length records its tag");
    check_eq_int(static_cast<long long>(record.type), static_cast<long long>('A'),
                 "the bad length tag is 'A'");
    check_eq_int(static_cast<long long>(record.length), 35, "the declared length is recorded");

    // A truncated body: fewer bytes than the length prefix promised.
    // The tag is NOT read, because the decoder stops before the body.
    std::vector<std::uint8_t> cut = frame_of(add_body, sizeof(add_body));
    cut.resize(10);
    check(record_for_frame(cut, record, error), "a truncated frame builds a record");
    check(record.status == TraceStatus::truncated, "a short body is truncated");
    check(!record.has_type, "a truncated frame records no tag");

    // A truncated length prefix: fewer than two bytes.
    std::vector<std::uint8_t> one_byte = {0x00};
    check(record_for_frame(one_byte, record, error), "a one-byte frame builds a record");
    check(record.status == TraceStatus::truncated, "a one-byte frame is truncated");
    check_eq_int(static_cast<long long>(record.length), 0, "a one-byte frame has no length");
    check(!record.has_type, "a one-byte frame records no tag");
}

void test_malformed_is_recorded() {
    std::printf("malformed frames\n");

    // An Add Order with a zero order reference is malformed: the
    // reference is the key the order lives under, and zero is reserved.
    std::uint8_t body[hft::itch::off::kAddOrderSize] = {};
    body[0] = static_cast<std::uint8_t>('A');
    body[19] = static_cast<std::uint8_t>('B');
    body[20] = 0x00;
    body[21] = 0x00;
    body[22] = 0x00;
    body[23] = 0x64;

    TraceRecord record;
    std::string error;
    check(record_for_frame(frame_of(body, sizeof(body)), record, error),
          "a zero-reference Add Order builds a record");
    check(record.status == TraceStatus::malformed, "a zero reference is malformed");
    check(record.has_type, "a malformed frame records its tag");
    check_eq_int(static_cast<long long>(record.type), static_cast<long long>('A'),
                 "the malformed tag is 'A'");

    // A side byte that is neither 'B' nor 'S'.
    std::uint8_t bad_side[hft::itch::off::kAddOrderSize] = {};
    bad_side[0] = static_cast<std::uint8_t>('A');
    bad_side[11] = 0x2A;
    bad_side[19] = static_cast<std::uint8_t>('X');
    check(record_for_frame(frame_of(bad_side, sizeof(bad_side)), record, error),
          "a bad side byte builds a record");
    check(record.status == TraceStatus::malformed, "a bad side byte is malformed");
}

// ---- The capture walk ------------------------------------------------

void test_walk_agrees_with_the_engine_reader() {
    std::printf("capture walk\n");

    // The reference walk exists to be an INDEPENDENT implementation of
    // the framing, so that the differential compares framing as well as
    // decoding. That independence is only useful if it is also correct,
    // and the way to know that is to check it against the engine's own
    // reader: the two must agree on every frame boundary.
    hft::feed::CaptureConfig config;
    config.record_count = 5'000;
    hft::feed::CaptureStats stats{};
    const std::vector<std::uint8_t> capture = hft::feed::generate_capture(config, &stats);

    hft::feed::CaptureReader reader(capture.data(), capture.size());
    hft::rtl::CaptureWalk walk(capture.data(), capture.size());

    std::size_t reader_frames = 0;
    std::size_t walk_frames = 0;
    bool agree = true;

    for (;;) {
        const std::uint8_t* reader_frame = nullptr;
        std::size_t reader_size = 0;
        const bool reader_more = reader.next(reader_frame, reader_size);

        std::size_t walk_size = 0;
        std::uint16_t declared = 0;
        bool complete = false;
        const std::uint8_t* walk_frame = walk.next(walk_size, declared, complete);

        if (!reader_more && walk_frame == nullptr) {
            break;
        }
        if (reader_more != (walk_frame != nullptr)) {
            agree = false;
            break;
        }
        if (!complete || reader_size != walk_size) {
            agree = false;
            break;
        }
        for (std::size_t i = 0; i < reader_size; ++i) {
            if (reader_frame[i] != walk_frame[i]) {
                agree = false;
                break;
            }
        }
        if (!agree) {
            break;
        }
        ++reader_frames;
        ++walk_frames;
    }

    check(agree, "the reference walk and the engine's reader agree on every frame");
    check_eq_int(static_cast<long long>(walk_frames), static_cast<long long>(reader_frames),
                 "and on how many frames there are");
    check(walk_frames > 100, "the walk actually walked a capture");

    // A capture cut in the middle of a frame is a truncated frame, and
    // the walk says so rather than yielding the bytes that are there.
    const std::vector<std::uint8_t> cut(capture.begin(), capture.begin() + 97);
    hft::rtl::CaptureWalk cut_walk(cut.data(), cut.size());
    std::size_t cut_frames = 0;
    bool saw_truncation = false;
    for (;;) {
        std::size_t size = 0;
        std::uint16_t declared = 0;
        bool complete = false;
        const std::uint8_t* f = cut_walk.next(size, declared, complete);
        if (f == nullptr) {
            break;
        }
        ++cut_frames;
        if (!complete) {
            saw_truncation = true;
            break;
        }
    }
    check(saw_truncation, "a cut capture produces a truncated frame");
    check(cut_frames > 0, "a cut capture still yields the frames before the cut");
}

void test_trace_over_a_generated_capture() {
    std::printf("trace over a generated capture\n");

    hft::feed::CaptureConfig config;
    config.record_count = 5'000;
    hft::feed::CaptureStats stats{};
    const std::vector<std::uint8_t> capture = hft::feed::generate_capture(config, &stats);

    hft::rtl::CaptureWalk walk(capture.data(), capture.size());
    std::vector<TraceRecord> records;
    std::size_t index = 0;
    std::string error;
    bool all_valid = true;

    for (;;) {
        std::size_t size = 0;
        std::uint16_t declared = 0;
        bool complete = false;
        const std::uint8_t* frame = walk.next(size, declared, complete);
        if (frame == nullptr) {
            break;
        }
        if (!complete) {
            break;
        }
        const hft::itch::DecodeResult result = hft::itch::decode(frame, size);
        TraceRecord record;
        if (!hft::rtl::record_for(index, result, record, error)) {
            all_valid = false;
            break;
        }
        std::string invalid;
        if (!hft::rtl::validate_record(record, invalid)) {
            all_valid = false;
            break;
        }
        records.push_back(record);
        ++index;
    }

    check(all_valid, "every record in a generated capture is valid");
    check(records.size() > 100, "the generated capture produced a trace");
    check_eq_int(static_cast<long long>(records.front().index), 0, "the first record is index 0");

    // The index of every record is its position, so a comparison cannot
    // be satisfied by two traces that disagree about how many messages
    // there were.
    bool indices_sequential = true;
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].index != i) {
            indices_sequential = false;
            break;
        }
    }
    check(indices_sequential, "record indices are sequential");

    // A trace compared with itself agrees, and a trace compared with a
    // corrupted copy of itself does not.
    std::vector<hft::rtl::TraceDifference> differences;
    check(hft::rtl::compare_traces(records, records, differences, 20),
          "a trace agrees with itself");

    std::vector<TraceRecord> corrupted = records;
    corrupted[0].fields[0] = "ffff";
    check(!hft::rtl::compare_traces(records, corrupted, differences, 20),
          "a trace does NOT agree with a corrupted copy of itself");
}

}  // namespace

int main() {
    std::printf("rtl trace tests\n");
    std::printf("---------------\n");
    test_format();
    test_round_trip();
    test_validation();
    test_comparator();
    test_add_order_trace();
    test_cancel_and_delete_differ();
    test_non_decoded_statuses();
    test_malformed_is_recorded();
    test_walk_agrees_with_the_engine_reader();
    test_trace_over_a_generated_capture();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
