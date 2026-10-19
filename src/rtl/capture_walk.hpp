// The reference capture walk: MoldUDP64 packets, message blocks, ITCH
// frames.
//
// This is the C++ side's answer to "where exactly does a message end?",
// written out so that the other side of the differential -- the RTL
// model -- has something readable to implement from.
//
// The engine has a capture reader of its own, and this deliberately is
// not it. The differential is meant to compare two INDEPENDENT
// implementations of the market data path, so the framing has to be
// part of what is compared. Reusing the engine's reader would put the
// engine's framing on both sides of the comparison, where a mistake in
// it is invisible: both sides would be reading frames the same wrong
// way.
//
// The layout, from the Nasdaq MoldUDP64 specification:
//
//   Downstream Packet Header, 20 bytes
//     0  10  Session
//    10   8  Sequence Number, big endian
//    18   2  Message Count
//    20  ..  Message Blocks
//
//   Message Block
//     0   2  Length, big endian, EXCLUDING these two bytes
//     2  ..  Message data
//
// A Message Block is byte-identical to an ITCH frame: the same two-byte
// length prefix in front of the same body. That is why a block can be
// handed to the decoder with no adjustment at either layer, and it is
// why the length read here is the length the decoder will read again.
//
// There is NO CHECKSUM in MoldUDP64. It is an unreliable transport
// wrapper; integrity comes from SOUP, which is layered above it.
// Implementing one here would mean inventing a field the specification
// does not define.

#pragma once

#include <cstddef>
#include <cstdint>

#include "hft/itch/moldudp64.hpp"

namespace hft::rtl {

/// Walks a capture, one ITCH frame at a time.
class CaptureWalk final {
public:
    CaptureWalk(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    /// The next frame, or null at the end of the capture.
    ///
    /// `complete` says whether the frame is whole. A frame that is not
    /// whole is the truncated case: the capture stopped in the middle
    /// of it, and `declared_length` says how many bytes the frame
    /// claimed so a consumer can say how much was missing.
    ///
    /// A heartbeat packet carries no frames and is stepped over. An
    /// end-of-session packet ends the walk.
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
            // The body is short. The frame is reported as truncated and
            // the walk stops, because there is nothing after it.
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

    /// Packets seen so far, for a diagnostic.
    [[nodiscard]] std::uint64_t packets() const noexcept { return packets_; }

private:
    [[nodiscard]] static std::uint16_t be16(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                          static_cast<std::uint16_t>(p[1]));
    }

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
        ++packets_;
        if (count == hft::itch::mold::kEndOfSessionCount) {
            done_ = true;
            return false;
        }
        if (count == hft::itch::mold::kHeartbeatCount) {
            return true;
        }
        remaining_blocks_ = count;
        return true;
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 0;
    std::uint16_t remaining_blocks_ = 0;
    std::uint64_t packets_ = 0;
    bool done_ = false;
};

}  // namespace hft::rtl
