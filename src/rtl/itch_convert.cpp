// The raw-ITCH to MoldUDP64 converter.
//
// The exchange sends a raw SOUP stream: message after message, each a
// two-byte length followed by a body, with no envelope. This repository's
// readers -- the engine's and the RTL model's -- take a MoldUDP64
// capture, which is the same messages inside a transport envelope.
//
// This tool performs that framing, so that both sides of the
// differential can read the SAME real messages.
//
// ---- What is faithful and what is not --------------------------------
//
// The MESSAGES are copied byte for byte. Nothing about them is
// synthesised, and the field tables are exercised on exactly what the
// exchange sent.
//
// The ENVELOPE is synthesised, and it is worth being precise about
// which parts:
//
//   * The Message Count is the number of Message Blocks in the packet,
//     which is exact.
//   * The Sequence Number is the SOUP sequence number of the FIRST
//     message in the packet. SOUP's sequence number counts messages, so
//     the running count of messages is the right value and the
//     per-message sequence numbers the reader derives from it are
//     contiguous and correct.
//   * The Session is a name, and the exchange's real one is not
//     published in the raw stream, so this one is made up. It is
//     ten bytes of ASCII and nothing reads it.
//
// The alternative was to teach both readers a third framing, and a
// third framing in two implementations is a third thing to get right.
//
// Usage:
//   hft_itch_convert <raw.itch> <capture.itch> [--session NAME]
//                    [--skip-to TYPE] [--max-bytes N]
//
//   --skip-to TYPE  start at the first message of that type, and skip
//                   everything before it. A real session opens with a
//                   few megabytes of directory and system messages
//                   before any order is added, and a capture of only
//                   those exercises nothing. The type is a single
//                   character, 'A' for Add Order.
//   --max-bytes N   stop after N bytes of output. A capture that ends
//                   mid-message is not a capture, so the last complete
//                   message is the last one.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hft/itch/moldudp64.hpp"

namespace {

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

void put_be16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

void put_be64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFU));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string raw_path;
    std::string capture_path;
    std::string session = "SAMPLE0000";
    char skip_to = '\0';
    std::size_t max_bytes = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--session") {
            session = next("--session");

        } else if (!arg.empty() && arg[0] == '-') {
            std::printf("unknown option %s\n", arg.c_str());
            return 2;
        } else if (raw_path.empty()) {
            raw_path = arg;
        } else if (capture_path.empty()) {
            capture_path = arg;
        } else {
            std::printf("unexpected argument %s\n", arg.c_str());
            return 2;
        }
    }

    if (raw_path.empty() || capture_path.empty()) {
        std::printf("usage: hft_itch_convert <raw.itch> <capture.itch> "
                    "[--session NAME] [--packets N]\n");
        return 2;
    }
    if (session.size() != hft::itch::mold::kSessionSize) {
        std::printf("the session name must be %zu bytes\n", hft::itch::mold::kSessionSize);
        return 2;
    }

    std::vector<std::uint8_t> raw;
    if (!read_file(raw_path, raw)) {
        return 2;
    }

    // The frames of the raw stream, as offsets and lengths. Nothing is
    // copied until the whole stream has been walked, so a truncated
    // tail is dropped rather than framed.
    struct Frame {
        std::size_t offset;
        std::uint16_t length;
    };
    std::vector<Frame> frames;
    std::size_t cursor = 0;
    while (cursor + 2 <= raw.size()) {
        const std::uint16_t length = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(raw[cursor]) << 8) |
            static_cast<std::uint16_t>(raw[cursor + 1]));
        if (length < 1 || cursor + 2 + length > raw.size()) {
            break;
        }
        frames.push_back({cursor, length});
        cursor += 2 + length;
    }

    if (frames.empty()) {
        std::printf("no complete messages in %s\n", raw_path.c_str());
        return 2;
    }

    // The capture: MoldUDP64 Downstream Packets, each carrying a run of
    // messages. A real feed's packets hold a few hundred messages;
    // this uses a fixed number so that the packet boundaries are
    // reproducible.
    constexpr std::size_t kBlocksPerPacket = 64;
    std::vector<std::uint8_t> out;
    out.reserve(raw.size() + raw.size() / 32 + 64);

    // Skip to the first message of the requested type, if one was
    // asked for. The frames are already frame-aligned -- the walk
    // starts at a frame boundary and every frame's length is known --
    // so the result is a capture that begins at a message boundary.
    std::size_t first_frame = 0;
    if (skip_to != '\0') {
        while (first_frame < frames.size() &&
               raw[frames[first_frame].offset + 2] != static_cast<std::uint8_t>(skip_to)) {
            ++first_frame;
        }
        if (first_frame >= frames.size()) {
            std::printf("no message of type '%c' in %s\n", skip_to, raw_path.c_str());
            return 2;
        }
    }

    std::size_t packet = 0;
    std::size_t index = first_frame;
    while (index < frames.size()) {
        const std::size_t first = index;
        std::size_t count = 0;
        // The block count is decided BEFORE the header is written, so
        // the count in the header always matches the number of blocks
        // that follow it. A header that claims more blocks than it
        // carries is a truncated packet, and the reader would report it
        // as one.
        while (index < frames.size() && count < kBlocksPerPacket) {
            if (max_bytes > 0 && out.size() + 2 + frames[index].length > max_bytes) {
                break;
            }
            ++index;
            ++count;
        }
        if (count == 0) {
            break;
        }

        out.insert(out.end(), session.begin(), session.end());
        put_be64(out, static_cast<std::uint64_t>(first));
        put_be16(out, static_cast<std::uint16_t>(count));
        for (std::size_t i = first; i < first + count; ++i) {
            // A MoldUDP64 Message Block is byte-identical to an ITCH
            // frame: the same two-byte length in front of the same
            // body. So the frame is copied whole.
            out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(frames[i].offset),
                       raw.begin() + static_cast<std::ptrdiff_t>(frames[i].offset) + 2 +
                           frames[i].length);
        }
        ++packet;
    }

    std::FILE* f = std::fopen(capture_path.c_str(), "wb");
    if (f == nullptr) {
        std::printf("cannot write %s\n", capture_path.c_str());
        return 2;
    }
    const std::size_t written = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (written != out.size()) {
        std::printf("short write on %s\n", capture_path.c_str());
        return 2;
    }

    std::printf("converted\n");
    std::printf("---------\n");
    std::printf("raw file          %s\n", raw_path.c_str());
    std::printf("capture file      %s\n", capture_path.c_str());
    std::printf("raw bytes         %zu\n", raw.size());
    std::printf("messages          %zu\n", frames.size());
    std::printf("packets           %zu\n", packet);
    std::printf("blocks per packet %zu\n", kBlocksPerPacket);
    std::printf("capture bytes     %zu\n", out.size());
    std::printf("session           %s\n", session.c_str());

    return 0;
}




