// The book dump: a replayed capture, and the book it leaves behind.
//
// This is the C++ side of the order-book differential. The RTL side
// writes the same format, and `hft_book_check` compares them.
//
// It exists because a checksum says "these two states are equal" and a
// dump says "these two states are equal, and if they are not, here is
// the order and the field that differ". The difference matters when the
// answer is no -- and this repository's worst bug was a decoder that
// read the share count as the price, which is a difference of one
// field in one order that a checksum would have reported as "these
// differ" and nothing else.
//
// ---- The format -----------------------------------------------------
//
//   # hft-book-dump v1
//   <index> <side> <id> <price> <size> <state>
//   ...
//   # checksum <16 hex digits>
//
// The orders are in the book's priority order: the bid side first,
// highest price first; then the ask side, lowest price first; and
// within a price level, in queue order. That is the order
// price-time priority defines, and it is the order the checksum is
// folded in, so a difference in the dump is a difference in the
// checksum's input.
//
// `state` is the C++ `OrderState`: 0 new, 1 partially filled, 2
// filled, 3 cancelled, 4 replaced. Only 0 and 1 can appear in a dump
// of a live book -- a filled, cancelled or replaced order has been
// detached from it -- but the field is carried in full so the two
// implementations cannot disagree about which is which.
//
// Usage:
//   hft_book_dump [capture] [--out <file>]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/lob/apply.hpp"
#include "hft/lob/order_book.hpp"

#include "replay/checksum.hpp"

namespace {

using hft::Side;

/// The state of one order, in the C++ enumeration's order.
[[nodiscard]] const char* state_name(hft::OrderState state) {
    switch (state) {
        case hft::OrderState::new_order: return "0";
        case hft::OrderState::partially_filled: return "1";
        case hft::OrderState::filled: return "2";
        case hft::OrderState::cancelled: return "3";
        case hft::OrderState::replaced: return "4";
    }
    return "0";
}

[[nodiscard]] std::string hex64(std::uint64_t v) {
    char buf[19];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

}  // namespace

int main(int argc, char** argv) {
    std::string capture_path;
    std::string out_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out") {
            if (i + 1 >= argc) {
                std::printf("--out needs a value\n");
                return 2;
            }
            out_path = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::printf("unknown option %s\n", arg.c_str());
            return 2;
        } else if (capture_path.empty()) {
            capture_path = arg;
        } else {
            std::printf("unexpected argument %s\n", arg.c_str());
            return 2;
        }
    }

    std::vector<std::uint8_t> capture;
    if (!capture_path.empty()) {
        std::FILE* f = std::fopen(capture_path.c_str(), "rb");
        if (f == nullptr) {
            std::printf("cannot open %s\n", capture_path.c_str());
            return 2;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 0) {
            std::fclose(f);
            std::printf("cannot size %s\n", capture_path.c_str());
            return 2;
        }
        capture.resize(static_cast<std::size_t>(size));
        const std::size_t got = std::fread(capture.data(), 1, capture.size(), f);
        std::fclose(f);
        if (got != capture.size()) {
            std::printf("short read on %s\n", capture_path.c_str());
            return 2;
        }
    } else {
        hft::feed::CaptureConfig config;
        config.record_count = 20'000;
        hft::feed::CaptureStats stats{};
        capture = hft::feed::generate_capture(config, &stats);
    }

    hft::lob::OrderBook book(1u << 20, 1u << 16);
    hft::feed::CaptureReader reader(capture.data(), capture.size());

    const std::uint8_t* frame = nullptr;
    std::size_t frame_size = 0;
    while (reader.next(frame, frame_size)) {
        const hft::itch::DecodeResult r = hft::itch::decode(frame, frame_size);
        if (r.ok()) {
            // The result is deliberately ignored: a message the book
            // rejects is a normal event on a live feed, and the dump
            // records the book that results either way.
            (void)hft::lob::apply(r.message, book);
        }
    }

    // The dump, in the book's priority order: the bid side first, and
    // within a side the levels in price order -- highest first for the
    // bid, lowest first for the ask -- and within a level the queue
    // order.
    //
    // Two record kinds, and both matter. The order records are what
    // the checksum folds, so they are the book state. The level
    // records are the aggregate and the order count, which the
    // checksum does NOT cover: a book can have the right orders in the
    // right order and still have an inflated aggregate, and that is a
    // bug the checksum would not report.
    std::vector<std::string> lines;
    std::uint64_t checksum = hft::replay::fnv1a_offset_basis;
    std::size_t index = 0;

    for (const Side side : {Side::bid, Side::ask}) {
        const std::vector<hft::lob::OrderSnapshot> orders = book.orders(side);
        const std::vector<hft::lob::LevelSnapshot> levels = book.levels(side);

        std::size_t order_cursor = 0;
        for (const hft::lob::LevelSnapshot& level : levels) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%zu L %c %s %u %s", index,
                          side == Side::bid ? 'B' : 'A',
                          hex64(static_cast<std::uint64_t>(level.price.raw())).c_str(),
                          level.order_count,
                          hex64(level.aggregate_size.raw()).c_str());
            lines.emplace_back(buf);
            ++index;

            // The orders of this level, in queue order. The flat order
            // list is grouped by level in price order, so the boundary
            // is where the price changes.
            std::size_t position = 0;
            while (order_cursor < orders.size() &&
                   orders[order_cursor].price.raw() == level.price.raw()) {
                const hft::lob::OrderSnapshot& o = orders[order_cursor];
                checksum = hft::replay::fnv1a_u64(checksum, o.id);
                checksum = hft::replay::fnv1a_u64(
                    checksum, static_cast<std::uint64_t>(o.price.raw()));
                checksum = hft::replay::fnv1a_u64(checksum, o.size.raw());
                checksum = hft::replay::fnv1a_u64(
                    checksum, static_cast<std::uint64_t>(o.state));

                std::snprintf(buf, sizeof(buf), "%zu O %c %s %s %s %s %zu", index,
                              side == Side::bid ? 'B' : 'A', hex64(o.id).c_str(),
                              hex64(static_cast<std::uint64_t>(o.price.raw())).c_str(),
                              hex64(o.size.raw()).c_str(), state_name(o.state), position);
                lines.emplace_back(buf);
                ++index;
                ++position;
                ++order_cursor;
            }
        }
    }

    char sum[64];
    std::snprintf(sum, sizeof(sum), "# checksum %016llx",
                  static_cast<unsigned long long>(checksum));

    if (out_path.empty()) {
        std::printf("# hft-book-dump v1\n");
        for (const std::string& line : lines) {
            std::printf("%s\n", line.c_str());
        }
        std::printf("%s\n", sum);
    } else {
        std::FILE* f = std::fopen(out_path.c_str(), "wb");
        if (f == nullptr) {
            std::printf("cannot write %s\n", out_path.c_str());
            return 2;
        }
        std::fprintf(f, "# hft-book-dump v1\n");
        for (const std::string& line : lines) {
            std::fprintf(f, "%s\n", line.c_str());
        }
        std::fprintf(f, "%s\n", sum);
        std::fclose(f);
        std::printf("book dump         %s\n", out_path.c_str());
    }

    std::printf("orders            %zu\n", index);
    std::printf("bid levels        %u\n", book.level_count(Side::bid));
    std::printf("ask levels        %u\n", book.level_count(Side::ask));
    std::printf("bid orders        %u\n", book.order_count(Side::bid));
    std::printf("ask orders        %u\n", book.order_count(Side::ask));
    std::printf("BOOK CHECKSUM     %016llx\n", static_cast<unsigned long long>(checksum));
    return 0;
}
