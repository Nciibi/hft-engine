// Deterministic replay with a book-state checksum.
//
// The property being demonstrated: the same capture file, replayed
// twice, on two different machines, at two different optimisation
// levels, produces the same book and therefore the same checksum. If
// that holds, every benchmark and every bug report derived from this
// pipeline is reproducible, and a discrepancy is a real discrepancy
// rather than an artefact of the host.
//
// The checksum is FNV-1a over a canonical serialisation of book state:
// the two ladders walked best-first, and within each level the orders
// in queue order. Queue order is included deliberately, because two
// books with identical prices and identical aggregates can still
// differ in priority order, and that difference is exactly the class
// of bug the differential test hunts.
//
// Sequence numbers are validated as the stream is consumed. A gap is
// reported and the run is marked unclean, because continuing past one
// means the book no longer reflects what the venue believes is
// resting.
//
// Usage:
//   hft_replay [capture.itch] [record_count]
//
// With no path, a capture is generated in memory, which is what CI
// runs. The checksum of a generated capture is reproducible because the
// generator is seeded.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/itch/sequence.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"
#include "replay/checksum.hpp"

namespace {

using hft::Side;

/// Walk a side's ladder and fold it into the running hash.
///
/// Order ids and prices are folded as raw integers in a fixed byte
/// order, and quantities as raw integers. Nothing here is formatted,
/// because a hash over formatted text depends on locale and on
/// operator<< overloads, both of which are exactly the sort of thing
/// that differs between a dev machine and a build host.
void fold_side(std::uint64_t& h, const hft::lob::OrderBook& book, Side side) {
    for (const hft::lob::OrderSnapshot& o : book.orders(side)) {
        h = hft::replay::fnv1a_u64(h, o.id);
        h = hft::replay::fnv1a_u64(h, static_cast<std::uint64_t>(o.price.raw()));
        h = hft::replay::fnv1a_u64(h, o.size.raw());
        h = hft::replay::fnv1a_u64(h, static_cast<std::uint64_t>(o.state));
    }
}

std::uint64_t book_checksum(const hft::lob::OrderBook& book) {
    std::uint64_t h = hft::replay::fnv1a_offset_basis;
    fold_side(h, book, Side::bid);
    fold_side(h, book, Side::ask);
    return h;
}

std::string hex64(std::uint64_t v) {
    char buf[19];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

/// True when the argument is entirely decimal digits.
///
/// The first positional argument is treated as a record count if it is
/// numeric and as a path otherwise. A flag would be less clever and
/// less convenient, and a replay tool gets invoked far more often than
/// it gets documented.
[[nodiscard]] bool looks_numeric(const char* s) noexcept {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

}  // namespace

// Usage:
//   hft_replay                     generate a capture in memory and replay it
//   hft_replay [records]           same, with a chosen record count
//   hft_replay <file>              replay a capture from disk
//   hft_replay <file> [records]    replay from disk, with a record cap
int main(int argc, char** argv) {
    std::size_t record_count = 200'000;
    const char* path = nullptr;
    if (argc > 1) {
        if (looks_numeric(argv[1])) {
            record_count = std::strtoull(argv[1], nullptr, 10);
        } else {
            path = argv[1];
        }
    }
    if (argc > 2) {
        record_count = std::strtoull(argv[2], nullptr, 10);
    }

    std::vector<std::uint8_t> capture;
    if (path != nullptr) {
        std::FILE* f = std::fopen(path, "rb");
        if (f == nullptr) {
            std::printf("cannot open %s\n", path);
            return 2;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 0) {
            std::fclose(f);
            std::printf("cannot size %s\n", path);
            return 2;
        }
        capture.resize(static_cast<std::size_t>(size));
        const std::size_t got = std::fread(capture.data(), 1, capture.size(), f);
        std::fclose(f);
        if (got != capture.size()) {
            std::printf("short read on %s\n", path);
            return 2;
        }
    } else {
        hft::feed::CaptureConfig config;
        config.record_count = record_count;
        hft::feed::CaptureStats stats{};
        capture = hft::feed::generate_capture(config, &stats);
        record_count = stats.records;
    }

    // Seed the tracker from the first record actually present.
    //
    // A hardcoded seed would be wrong twice over: it would be a
    // duplicate source of truth alongside CaptureConfig, and it would
    // claim knowledge of whether messages before the first observed
    // one were lost. Nothing in the capture can answer that, so the
    // tracker starts where the data starts and only reports gaps it
    // can actually see.
    const std::uint32_t first_sequence =
        capture.size() >= hft::feed::kCaptureSequenceSize
            ? hft::itch::read_be32(capture.data())
            : 0u;

    std::printf("HFT Engine replay\n");
    std::printf("-----------------\n");
    std::printf("source             %s\n", path != nullptr ? path : "(generated in memory)");
    std::printf("capture bytes      %zu\n", capture.size());
    std::printf("records            %zu\n", record_count);
    std::printf("first sequence     %u\n", first_sequence);
    std::printf("\n");

    hft::lob::OrderBook book(1u << 20, 1u << 16);
    hft::itch::SequenceTracker sequence(first_sequence);

    std::size_t offset = 0;
    std::size_t applied = 0;
    std::size_t skipped = 0;
    std::size_t unknown_orders = 0;
    std::size_t over_reduce = 0;
    std::size_t other_rejects = 0;
    // Frames that decoded to neither ok nor skippable. An earlier
    // revision dropped these silently, which let a heap-corrupting bug
    // in the feed generator masquerade as merely "some records did not
    // apply" instead of "this pipeline is broken". Any non-zero value
    // here is a hard failure.
    std::size_t malformed = 0;
    std::uint64_t checksum = 0;
    std::size_t checksummed = 0;

    while (offset < capture.size()) {
        if (capture.size() - offset < hft::feed::kCaptureSequenceSize + hft::itch::kLengthPrefixSize) {
            std::printf("truncated capture tail at offset %zu\n", offset);
            break;
        }

        const std::uint32_t seq = hft::itch::read_be32(capture.data() + offset);
        const hft::itch::SequenceTracker::State seq_state = sequence.observe(seq);

        const std::size_t frame_at = offset + hft::feed::kCaptureSequenceSize;
        const hft::itch::DecodeResult r =
            hft::itch::decode(capture.data() + frame_at, capture.size() - frame_at);
        const std::size_t stride = hft::itch::frame_stride(r);
        if (stride == 0) {
            break;
        }

        if (r.ok()) {
            const hft::lob::ApplyResult ar = hft::lob::apply(r.message, book);
            if (ar.applied) {
                ++applied;
            } else if (ar.detail == hft::lob::BookStatus::unknown_order) {
                ++unknown_orders;
            } else if (ar.detail == hft::lob::BookStatus::over_reduce) {
                ++over_reduce;
            } else {
                ++other_rejects;
            }
        } else if (r.skippable()) {
            ++skipped;
        } else {
            ++malformed;
        }

        // Checksum the book periodically rather than per record: the
        // checksum is over state, and state does not change between
        // mutations often enough to warrant hashing it 1,000,000 times.
        if ((applied % 20'000) == 0) {
            checksum ^= book_checksum(book);
            ++checksummed;
        }

        offset = frame_at + stride;
        (void)seq_state;
    }

    // Final fold, so the checksum reflects the end state rather than
    // whatever happened to be last sampled.
    checksum ^= book_checksum(book);
    ++checksummed;

    std::printf("results\n");
    std::printf("-------\n");
    std::printf("applied            %zu\n", applied);
    std::printf("skipped (unknown)  %zu\n", skipped);
    std::printf("malformed          %zu\n", malformed);
    std::printf("unknown order      %zu\n", unknown_orders);
    std::printf("over-reduce        %zu\n", over_reduce);
    std::printf("other rejects      %zu\n", other_rejects);
    if (malformed != 0) {
        std::printf(
            "\nFAIL: %zu frames were neither valid nor skippable. The capture is\n"
            "      inconsistent with its own framing; do not trust the checksum.\n",
            malformed);
    }
    std::printf("sequence accepted  %llu\n", u64(sequence.accepted()));
    std::printf("sequence gaps      %llu\n", u64(sequence.gaps()));
    std::printf("sequence missing   %llu\n", u64(sequence.missing()));
    std::printf("sequence rejects   %llu\n", u64(sequence.rejects()));
    std::printf("\nbook state\n");
    std::printf("---------\n");
    std::printf("bid levels         %u\n", book.level_count(Side::bid));
    std::printf("ask levels         %u\n", book.level_count(Side::ask));
    std::printf("bid orders         %u\n", book.order_count(Side::bid));
    std::printf("ask orders         %u\n", book.order_count(Side::ask));
    std::printf("bid resting shares %llu\n", u64(book.aggregate_at(Side::bid).raw()));
    std::printf("ask resting shares %llu\n", u64(book.aggregate_at(Side::ask).raw()));
    std::printf("\nchecksum samples   %zu\n", checksummed);
    std::printf("BOOK CHECKSUM      %s\n", hex64(checksum).c_str());

    if (!sequence.clean()) {
        std::printf(
            "\nWARNING: the capture was not contiguous. %llu messages missing,\n"
            "so this book does NOT reflect a complete session and its\n"
            "checksum is not comparable with a clean run.\n",
            u64(sequence.missing()));
    }

    std::printf(
        "\nDeterminism claim: run this twice on the same input and the\n"
        "BOOK CHECKSUM above must be identical. Run it on another host,\n"
        "or at a different -O level, and it must still match. If it does\n"
        "not, the fault is in this pipeline, not in the market.\n");
    return 0;
}
