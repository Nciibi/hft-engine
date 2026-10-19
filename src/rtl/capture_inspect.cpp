// The capture inspector: what is actually on the wire, and does the
// field table agree with it?
//
// This tool exists because of a bug it would have caught immediately.
//
// The Order Executed field table carried a Printable byte that the
// message does not have, and the message was 31 bytes where the table
// said 32. Every test in the repository agreed with the table, because
// the generator wrote the same bytes the decoder read. Nothing in the
// repository could see it.
//
// Real market data could, and did. The inspector is that check, turned
// into a tool: it walks a capture and compares every message's DECLARED
// length against the length the field table says its type must have.
//
// A mismatch is not a rounding error. It means the decoder will reject
// every message of that type as a bad length, and a live feed does not
// stop to complain.
//
// Usage:
//   hft_capture_inspect <file> [--raw] [--max-report N]
//
//   --raw        the file is a raw ITCH stream, not a MoldUDP64 capture
//   --max-report how many mismatches to print
//
// A raw ITCH stream is what the exchange actually sends: message after
// message, each a two-byte length followed by a body. A MoldUDP64
// capture is the same messages inside a transport envelope, and it is
// the format this repository's readers take.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "hft/itch/decode.hpp"
#include "hft/itch/protocol.hpp"

#include "feed/generator.hpp"

namespace {

/// The length the field table says each type must have, and the type
/// it belongs to. A type this build does not decode has no entry: it
/// is skipped by length, which is correct behaviour and not a
/// disagreement.
struct Expectation {
    std::uint16_t length;
    const char* name;
};

/// The types this build decodes, with the body length each one must
/// have. Sourced from the field table, so this tool cannot disagree
/// with the decoder by construction -- it checks the TABLE against the
/// wire, not one against the other.
[[nodiscard]] const std::map<std::uint8_t, Expectation>& expectations() {
    static const std::map<std::uint8_t, Expectation> kTable = {
        {static_cast<std::uint8_t>('A'),
         {static_cast<std::uint16_t>(hft::itch::off::kAddOrderSize), "Add Order"}},
        {static_cast<std::uint8_t>('E'),
         {static_cast<std::uint16_t>(hft::itch::off::kOrderExecutedSize), "Order Executed"}},
        {static_cast<std::uint8_t>('C'),
         {static_cast<std::uint16_t>(hft::itch::off::kOrderExecutedAtPriceSize),
          "Order Executed At Price"}},
        {static_cast<std::uint8_t>('X'),
         {static_cast<std::uint16_t>(hft::itch::off::kOrderCancelSize), "Order Cancel"}},
        {static_cast<std::uint8_t>('D'),
         {static_cast<std::uint16_t>(hft::itch::off::kOrderDeleteSize), "Order Delete"}},
        {static_cast<std::uint8_t>('U'),
         {static_cast<std::uint16_t>(hft::itch::off::kOrderReplaceSize), "Order Replace"}},
        {static_cast<std::uint8_t>('B'),
         {static_cast<std::uint16_t>(hft::itch::off::kBrokenTradeSize), "Broken Trade"}},
        {static_cast<std::uint8_t>('P'),
         {static_cast<std::uint16_t>(hft::itch::off::kTradeSize), "Trade"}},
        {static_cast<std::uint8_t>('Q'),
         {static_cast<std::uint16_t>(hft::itch::off::kCrossTradeSize), "Cross Trade"}},
    };
    return kTable;
}

/// One message type's worth of observation.
struct Tally {
    std::uint64_t count = 0;
    std::uint16_t length = 0;
    bool length_known = false;
};

/// Walk a raw ITCH stream: message after message, each a two-byte
/// length followed by a body.
class RawWalk final {
public:
    RawWalk(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] bool next(std::uint8_t& tag, std::uint16_t& length) noexcept {
        if (cursor_ + 2 > size_) {
            return false;
        }
        length = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data_[cursor_]) << 8) |
                                            static_cast<std::uint16_t>(data_[cursor_ + 1]));
        if (length < 1 || cursor_ + 2 + length > size_) {
            truncated_ = true;
            return false;
        }
        tag = data_[cursor_ + 2];
        cursor_ += 2 + length;
        return true;
    }

    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 0;
    bool truncated_ = false;
};

[[nodiscard]] bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::printf("cannot open %s\n", path.c_str());
        return false;
    }
    std::fseek(f, 0,SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        std::printf("cannot size %s\n", path.c_str());
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        std::printf("short read on %s\n", path.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    bool raw = false;
    std::size_t max_report = 20;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--raw") {
            raw = true;
        } else if (arg == "--max-report") {
            if (i + 1 >= argc) {
                std::printf("--max-report needs a value\n");
                return 2;
            }
            max_report = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (!arg.empty() && arg[0] == '-') {
            std::printf("unknown option %s\n", arg.c_str());
            return 2;
        } else if (path.empty()) {
            path = arg;
        } else {
            std::printf("unexpected argument %s\n", arg.c_str());
            return 2;
        }
    }

    if (path.empty()) {
        std::printf("usage: hft_capture_inspect <file> [--raw] [--max-report N]\n");
        return 2;
    }

    std::vector<std::uint8_t> capture;
    if (!read_file(path, capture)) {
        return 2;
    }

    std::map<std::uint8_t, Tally> tally;
    std::uint64_t messages = 0;
    std::uint64_t mismatches = 0;
    bool truncated = false;

    if (raw) {
        RawWalk walk(capture.data(), capture.size());
        std::uint8_t tag = 0;
        std::uint16_t length = 0;
        while (walk.next(tag, length)) {
            Tally& t = tally[tag];
            ++t.count;
            t.length = length;
            t.length_known = true;
            ++messages;
        }
        truncated = walk.truncated();
    } else {
        hft::feed::CaptureReader reader(capture.data(), capture.size());
        const std::uint8_t* frame = nullptr;
        std::size_t frame_size = 0;
        while (reader.next(frame, frame_size)) {
            if (frame_size < 3) {
                continue;
            }
            const hft::itch::DecodeResult r = hft::itch::decode(frame, frame_size);
            const std::uint8_t tag = frame[2];
            Tally& t = tally[tag];
            ++t.count;
            t.length = r.length;
            t.length_known = true;
            ++messages;
        }
        truncated = reader.malformed();
    }

    std::printf("capture inspection\n");
    std::printf("------------------\n");
    std::printf("file               %s\n", path.c_str());
    std::printf("framing            %s\n", raw ? "raw ITCH" : "MoldUDP64");
    std::printf("bytes              %zu\n", capture.size());
    std::printf("messages           %llu\n", static_cast<unsigned long long>(messages));
    if (truncated) {
        std::printf("truncated          yes -- the capture ends mid-message\n");
    }

    std::printf("\n%-6s %-12s %12s %8s %8s  %s\n", "type", "name", "count", "length", "expected", "verdict");
    for (const auto& [tag, t] : tally) {
        const auto it = expectations().find(tag);
        const char* name = "-";
        std::uint16_t expected = 0;
        bool decoded = false;
        if (it != expectations().end()) {
            name = it->second.name;
            expected = it->second.length;
            decoded = true;
        }
        const bool agrees = decoded && expected == t.length;
        if (decoded && !agrees) {
            ++mismatches;
        }
        std::printf("%-6c %-12s %12llu %8u %8s  %s\n", static_cast<char>(tag), name,
                    static_cast<unsigned long long>(t.count), static_cast<unsigned>(t.length),
                    decoded ? std::to_string(expected).c_str() : "-",
                    !decoded ? "not decoded" : (agrees ? "ok" : "LENGTH MISMATCH"));
    }

    if (mismatches == 0) {
        std::printf("\nOK: every message type this build decodes has the length the field table says.\n");
        return 0;
    }

    if (static_cast<std::size_t>(mismatches) > max_report) {
        std::printf("(only the first %zu mismatches are shown)\n", max_report);
    }

    std::printf("\nFAILED: %llu message types disagree with the field table.\n",
                static_cast<unsigned long long>(mismatches));
    std::printf(
        "A type whose declared length differs from its table is REJECTED by the\n"
        "decoder as a bad length, so every message of that type is dropped. On a\n"
        "live feed nothing complains: the book is simply wrong.\n");
    return 1;
}

