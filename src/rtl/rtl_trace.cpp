// The reference capture walk: a capture, read frame by frame, rendered
// as a decode trace.
//
// This tool is the C++ side of the differential between the engine's
// decoder and the RTL model. It does three things:
//
//   1. Walks a capture the way the wire is structured: a MoldUDP64
//      packet header, then that packet's message blocks, each of which
//      is an ITCH frame.
//   2. Decodes each frame with the engine's decoder.
//   3. Writes the result as a trace record -- see `rtl/TRACE_FORMAT.md`
//      for the format, and `src/rtl/trace.hpp` for this side's
//      implementation of it.
//
// ---- Why the walk is written here and not reused -------------------
//
// The engine has a capture reader already, and this tool does not use
// it. That is deliberate.
//
// The differential is supposed to compare two INDEPENDENT
// implementations of the market data path. If this tool reused the
// engine's reader, the C++ side of the comparison would be the engine's
// framing plus the engine's decoder, and a framing mistake would be
// invisible: both sides of the comparison would be reading frames the
// same wrong way.
//
// Writing the walk out here means the framing is part of what is being
// compared. The RTL side writes its own walk from the same published
// specifications, and the two walks have to agree about where one
// message ends and the next begins before any field is even looked at.
//
// That makes this tool a specification as much as a program: it is the
// readable answer to "where exactly does a message end?", for anyone
// implementing the other side.
//
// Usage:
//   hft_rtl_trace [capture] --out <trace> [--hex <hex>] [--records N]
//                [--truncate-at N] [--seed <hex>]
//
//   capture        capture file to read; generated in memory if omitted
//   --out          trace file to write (required)
//   --hex          hex sidecar of the capture bytes, for the RTL side
//   --records      stop after this many frames
//   --truncate-at  cut the capture to this many bytes, to exercise the
//                  truncated path
//   --seed         generator seed, for a reproducible in-memory capture

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "feed/generator.hpp"
#include "hft/itch/decode.hpp"
#include "hft/itch/moldudp64.hpp"
#include "rtl/capture_walk.hpp"
#include "rtl/decode_trace.hpp"
#include "rtl/trace.hpp"

namespace {

using hft::rtl::TraceRecord;
using hft::rtl::TraceStatus;

[[nodiscard]] std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

[[nodiscard]] std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

/// Write the capture as hex, 64 bytes to a line.
///
/// The sidecar exists because the RTL testbench has to read the capture
/// and GHDL cannot read a binary file conveniently. A hex dump is the
/// cheapest thing that works: `textio` reads it with a two-character
/// loop, and a human can read it when a comparison fails.
///
/// It is a faithful dump -- every byte of the capture, in order, nothing
/// added -- so the RTL side is provably looking at the same bytes the
/// C++ side did.
bool write_hex(const std::string& path, const std::vector<std::uint8_t>& capture) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::printf("cannot write %s\n", path.c_str());
        return false;
    }
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string line;
    line.reserve(128U);
    std::size_t on_line = 0;
    for (const std::uint8_t byte : capture) {
        line.push_back(kDigits[byte >> 4U]);
        line.push_back(kDigits[byte & 0x0FU]);
        if (++on_line == 64U) {
            out << line << '\n';
            line.clear();
            on_line = 0;
        }
    }
    if (!line.empty()) {
        out << line << '\n';
    }
    return static_cast<bool>(out);
}

[[nodiscard]] bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::printf("cannot open %s\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
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

/// The walk.
///
/// A capture is a sequence of MoldUDP64 Downstream Packets. Each has a
/// 20-byte header -- session, 64-bit sequence number, message count --
/// followed by that many Message Blocks. A block is a 2-byte big-endian
/// length followed by that many bytes of message data, which makes it
/// byte-identical to an ITCH frame: the same two-byte length prefix in
/// front of the same body.
///
/// The count in the header is trusted, and a packet whose blocks run out
/// before the count is exhausted is a TRUNCATED capture, not a shorter
/// packet. The tool reports that as a truncated frame rather than
/// quietly yielding the blocks that did arrive.
class Walk final {
public:
    Walk(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    /// The next frame, or null at the end of the capture.
    ///
    /// `complete` says whether the frame is whole. A frame that is not
    /// whole is the truncated case, and its declared length is still
    /// reported so the trace can record it.
    [[nodiscard]] const std::uint8_t* next(std::size_t& frame_size,
                                           std::uint16_t& declared_length,
                                           bool& complete) noexcept {
        for (;;) {
            if (done_) {
                return nullptr;
            }
            if (remaining_blocks_ == 0U) {
                if (!load_packet()) {
                    return nullptr;
                }
                if (remaining_blocks_ == 0U) {
                    // A heartbeat: a packet with no message blocks. It
                    // is part of the transport, not the feed, so it
                    // carries no frame and the walk continues.
                    continue;
                }
            }
            break;
        }
        if (size_ - cursor_ < hft::itch::mold::kMessageBlockSize) {
            // A length prefix that runs off the end of the capture.
            done_ = true;
            complete = false;
            declared_length = 0;
            frame_size = 0;
            return data_ + cursor_;
        }
        const std::uint16_t length = be16(data_ + cursor_);
        cursor_ += hft::itch::mold::kMessageBlockSize;
        const std::size_t available = size_ - cursor_;
        if (available < length) {
            // The body is short. The frame is reported as truncated, and
            // the cursor does not advance past what is there.
            done_ = true;
            complete = false;
            declared_length = length;
            frame_size = available;
            return data_ + cursor_ - hft::itch::mold::kMessageBlockSize;
        }
        cursor_ += length;
        --remaining_blocks_;
        complete = true;
        declared_length = length;
        frame_size = static_cast<std::size_t>(length) + hft::itch::mold::kMessageBlockSize;
        return data_ + cursor_ - frame_size;
    }

private:
    [[nodiscard]] bool load_packet() noexcept {
        if (size_ - cursor_ < hft::itch::mold::kHeaderSize) {
            // An incomplete packet header. The capture ends here; there
            // is no frame to report, only the fact that the stream
            // stopped in the middle of a header.
            done_ = true;
            return false;
        }
        const std::uint8_t* header = data_ + cursor_;
        const std::uint16_t count = be16(header + hft::itch::mold::kCountOffset);
        cursor_ += hft::itch::mold::kHeaderSize;
        if (count == hft::itch::mold::kEndOfSessionCount) {
            done_ = true;
            return false;
        }
        if (count == hft::itch::mold::kHeartbeatCount) {
            // A heartbeat carries the next expected sequence number and
            // no messages. It is part of the transport, not the feed.
            return true;
        }
        remaining_blocks_ = count;
        return true;
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 0;
    std::uint16_t remaining_blocks_ = 0;
    bool done_ = false;
};

[[nodiscard]] bool parse_u64(const char* text, std::uint64_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string capture_path;
    std::string trace_path;
    std::string hex_path;
    std::size_t record_limit = 0;
    std::size_t truncate_at = 0;
    bool truncate = false;
    std::uint64_t seed = 0xC0FF'EE00'1234'5678ULL;
    bool have_seed = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--out") {
            trace_path = next("--out");
        } else if (arg == "--hex") {
            hex_path = next("--hex");
        } else if (arg == "--records") {
            record_limit = static_cast<std::size_t>(std::strtoull(next("--records"), nullptr, 10));
        } else if (arg == "--truncate-at") {
            truncate_at = static_cast<std::size_t>(std::strtoull(next("--truncate-at"), nullptr, 10));
            truncate = true;
        } else if (arg == "--seed") {
            if (!parse_u64(next("--seed"), seed)) {
                std::printf("--seed needs a number\n");
                return 2;
            }
            have_seed = true;
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

    if (trace_path.empty()) {
        std::printf("--out is required\n");
        return 2;
    }

    std::vector<std::uint8_t> capture;
    if (!capture_path.empty()) {
        if (!read_file(capture_path, capture)) {
            return 2;
        }
    } else {
        hft::feed::CaptureConfig config;
        config.record_count = record_limit > 0U ? record_limit : 20'000U;
        if (have_seed) {
            config.seed = seed;
        }
        hft::feed::CaptureStats stats{};
        capture = hft::feed::generate_capture(config, &stats);
    }

    if (truncate && truncate_at < capture.size()) {
        capture.resize(truncate_at);
    }

    if (!hex_path.empty() && !write_hex(hex_path, capture)) {
        return 2;
    }

    std::ofstream out(trace_path, std::ios::binary);
    if (!out) {
        std::printf("cannot write %s\n", trace_path.c_str());
        return 2;
    }
    out << hft::rtl::kTraceHeader << '\n';

    std::size_t decoded = 0;
    std::size_t skipped = 0;
    std::size_t bad_length = 0;
    std::size_t malformed = 0;
    std::size_t truncated = 0;
    std::size_t index = 0;
    bool failed = false;

    Walk walk(capture.data(), capture.size());
    for (;;) {
        if (record_limit > 0U && index >= record_limit) {
            break;
        }
        std::size_t frame_size = 0;
        std::uint16_t declared_length = 0;
        bool complete = false;
        const std::uint8_t* frame = walk.next(frame_size, declared_length, complete);
        if (frame == nullptr) {
            break;
        }

        TraceRecord record;
        std::string error;
        if (!complete) {
            // The frame is short. The decoder would say the same thing
            // if it were handed the bytes that are there, and the
            // declared length is recorded so the two sides agree on how
            // much was missing.
            record.index = index;
            record.status = TraceStatus::truncated;
            record.length = declared_length;
            record.has_type = false;
        } else {
            const hft::itch::DecodeResult result = hft::itch::decode(frame, frame_size);
            if (!hft::rtl::record_for(index, result, record, error)) {
                std::printf("record %zu could not be built: %s\n", index, error.c_str());
                failed = true;
                break;
            }
        }

        if (!hft::rtl::write_record(out, record)) {
            std::printf("write failed\n");
            return 2;
        }

        switch (record.status) {
            case TraceStatus::decoded: ++decoded; break;
            case TraceStatus::skipped: ++skipped; break;
            case TraceStatus::bad_length: ++bad_length; break;
            case TraceStatus::malformed: ++malformed; break;
            case TraceStatus::truncated: ++truncated; break;
        }
        ++index;
    }

    out.close();
    if (!out) {
        std::printf("write failed on %s\n", trace_path.c_str());
        return 2;
    }

    std::printf("trace\n");
    std::printf("-----\n");
    std::printf("source             %s\n",
                capture_path.empty() ? "(generated in memory)" : capture_path.c_str());
    std::printf("capture bytes      %zu\n", capture.size());
    std::printf("trace file         %s\n", trace_path.c_str());
    if (!hex_path.empty()) {
        std::printf("hex sidecar        %s\n", hex_path.c_str());
    }
    std::printf("records            %zu\n", index);
    std::printf("decoded            %zu\n", decoded);
    std::printf("skipped            %zu\n", skipped);
    std::printf("bad length         %zu\n", bad_length);
    std::printf("malformed          %zu\n", malformed);
    std::printf("truncated          %zu\n", truncated);
    std::printf("trace records      %zu\n", index);

    if (failed) {
        std::printf("\nFAIL: the trace could not be built.\n");
        return 1;
    }
    return 0;
}
