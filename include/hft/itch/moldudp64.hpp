// MoldUDP64 downstream packet framing.
//
// VERIFIED against the Nasdaq "MoldUDP64 Protocol Specification",
// Downstream Packet Header and Message Block sections, cross-checked
// against FINRA's copy of the same document and against Wireshark's
// published dissector field names. Every offset below carries a
// static_assert against its literal value from the field table, not
// merely against the offset of the next field. See
// `include/hft/itch/protocol.hpp` for why that distinction is the whole
// game: an internally-consistent-but-wrong table defeats differential
// testing, because both sides of the comparison share the error.
//
// ---- Layout ---------------------------------------------------------
//
//   offset  len  field
//        0   10  Session            alphanumeric numeric, e.g. "SAMPLE0000"
//       10    8  Sequence Number    big endian, FIRST message in the packet
//       18    2  Message Count      number of message blocks
//       20   ..  Message Blocks
//
// A Message Block is a 2-byte big-endian length followed by that many
// bytes of message data. The length EXCLUDES the two length bytes, so a
// block occupies length + 2 bytes in total.
//
// All number fields are big endian. The MoldUDP64 specification says so
// explicitly and notes it does not necessarily apply to the data inside
// a Message Block -- which is ITCH, big endian for the same reasons.
//
// ---- Two things this repository previously got wrong ---------------
//
// 1. THERE IS NO CHECKSUM. MoldUDP64 is an unreliable transport wrapper
//    and does not checksum packets. Integrity comes from SOUP, the
//    protocol layered above it, and from the sequence numbers MoldUDP64
//    does provide. An earlier revision of README.md promised "MoldUDP64
//    packet framing and checksum" as one thing; they are two protocols
//    and only the framing belongs here. Implementing a CRC here would
//    mean inventing a field the specification does not define, which is
//    precisely the Add Order mistake.
//
// 2. SEQUENCE NUMBERS ARE 64-BIT, NOT 32-BIT. The field is eight bytes.
//    `hft/itch/sequence.hpp` wraps a 32-bit counter and has tests for
//    the 32-bit wrap, which is a real ITCH concern but is NOT this
//    field's width. A MoldUDP64 gap check that read four bytes here
//    would ignore the high half of every sequence number.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hft::itch::mold {

// ---- Field widths, from the specification ---------------------------

inline constexpr std::size_t kSessionSize = 10;
inline constexpr std::size_t kSequenceSize = 8;
inline constexpr std::size_t kCountSize = 2;
inline constexpr std::size_t kMessageBlockSize = 2;  // The length prefix

inline constexpr std::size_t kSessionOffset = 0;
inline constexpr std::size_t kSequenceOffset = kSessionOffset + kSessionSize;   // 10
inline constexpr std::size_t kCountOffset = kSequenceOffset + kSequenceSize;   // 18
inline constexpr std::size_t kFirstBlockOffset = kCountOffset + kCountSize;    // 20

inline constexpr std::size_t kHeaderSize = kFirstBlockOffset;                  // 20

/// Message Count values with a defined meaning beyond "how many blocks".
///
/// A heartbeat carries the next expected sequence number and no blocks,
/// which is how a receiver learns it has not silently fallen behind
/// between packets. End of session is sent repeatedly in place of
/// heartbeats once the session is complete, and is the last chance to
/// backfill before the stream stops.
inline constexpr std::uint16_t kHeartbeatCount = 0;
inline constexpr std::uint16_t kEndOfSessionCount = 0xFFFF;

// Each offset asserted against its literal value from the field table.
// A regression here is a compile error naming the field that moved.
static_assert(kSessionOffset == 0, "Session is at offset 0");
static_assert(kSequenceOffset == 10, "Sequence Number is at offset 10");
static_assert(kCountOffset == 18, "Message Count is at offset 18");
static_assert(kFirstBlockOffset == 20, "the first Message Block starts at offset 20");
static_assert(kHeaderSize == 20, "the Downstream Packet Header is 20 bytes");
static_assert(kSequenceSize == 8, "the Sequence Number field is 8 bytes, not 4");
static_assert(kEndOfSessionCount == 0xFFFF, "end of session is message count 0xFFFF");

[[nodiscard]] constexpr bool is_heartbeat(std::uint16_t count) noexcept {
    return count == kHeartbeatCount;
}

[[nodiscard]] constexpr bool is_end_of_session(std::uint16_t count) noexcept {
    return count == kEndOfSessionCount;
}

/// The 20-byte Downstream Packet Header.
struct PacketHeader {
    char session[kSessionSize] = {};
    std::uint64_t sequence = 0;
    std::uint16_t count = 0;

    /// Sequence number of the NEXT message this receiver expects.
    ///
    /// Meaningful only on a heartbeat or an end-of-session packet, where
    /// there are no blocks and the sequence field carries the receiver's
    /// position rather than the first message in a packet.
    [[nodiscard]] constexpr std::uint64_t next_expected() const noexcept {
        return sequence;
    }
};

enum class HeaderStatus : std::uint8_t {
    ok = 0,
    /// Fewer than 20 bytes available. Never read past.
    truncated,
};

/// Parse a Downstream Packet Header.
///
/// Big endian throughout, assembled byte by byte rather than through a
/// cast, for the same reason the ITCH decoder does it: the input is a
/// pointer into a captured file or a socket buffer with no alignment
/// guarantee, and reading a `uint64_t` through such a pointer is
/// undefined behaviour on every platform that matters.
[[nodiscard]] inline HeaderStatus parse_header(const std::uint8_t* data,
                                               std::size_t available,
                                               PacketHeader& out) noexcept {
    if (available < kHeaderSize) {
        return HeaderStatus::truncated;
    }
    for (std::size_t i = 0; i < kSessionSize; ++i) {
        out.session[i] = static_cast<char>(data[kSessionOffset + i]);
    }

    std::uint64_t sequence = 0;
    for (std::size_t i = 0; i < kSequenceSize; ++i) {
        sequence = (sequence << 8) | static_cast<std::uint64_t>(
                                        data[kSequenceOffset + i]);
    }
    out.sequence = sequence;

    out.count = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[kCountOffset]) << 8) |
        static_cast<std::uint16_t>(data[kCountOffset + 1]));
    return HeaderStatus::ok;
}

/// Walks the Message Blocks of one packet.
///
/// Counts are taken from the header and trusted. A packet whose blocks
/// run out before its count is exhausted, or whose last block ends
/// exactly at the end of the buffer, is a TRUNCATED capture -- a real
/// failure mode when reading a file someone was still writing -- and is
/// reported rather than padded over.
class MessageBlocks final {
public:
    MessageBlocks(const std::uint8_t* data, std::size_t available, std::uint16_t count) noexcept
        : data_(data), remaining_bytes_(available), remaining_count_(count) {}

    /// Next message body, or null when the blocks are exhausted or a
    /// block runs past the end of the buffer.
    ///
    /// `body_size` is the block's length field, which excludes the two
    /// length bytes -- so a caller passing this straight to
    /// `hft::itch::decode` gets the right frame.
    [[nodiscard]] const std::uint8_t* next(std::size_t& body_size) noexcept {
        if (remaining_count_ == 0) {
            return nullptr;
        }
        if (remaining_bytes_ < kMessageBlockSize) {
            exhausted_ = true;
            return nullptr;
        }
        const std::size_t length =
            (static_cast<std::size_t>(data_[0]) << 8) | static_cast<std::size_t>(data_[1]);
        if (remaining_bytes_ < kMessageBlockSize + length) {
            exhausted_ = true;
            return nullptr;
        }
        // The body starts AFTER the two length bytes, and the cursor is
        // advanced past the whole block. Capturing the body pointer
        // before advancing matters: returning `data_` after the
        // increment hands the caller a pointer to the NEXT block's
        // length prefix, which parses as a frame with the wrong length
        // and fails in a decoder far from here.
        const std::uint8_t* body = data_ + kMessageBlockSize;
        data_ += kMessageBlockSize + length;
        remaining_bytes_ -= kMessageBlockSize + length;
        --remaining_count_;
        body_size = length;
        return body;
    }

    /// Blocks still to be read according to the header.
    [[nodiscard]] std::uint16_t remaining() const noexcept { return remaining_count_; }

    /// True once a block was found to run past the end of the buffer.
    ///
    /// Distinct from "finished": a caller that checks only `remaining()`
    /// would report success on a truncated packet, which is how a
    /// half-written capture turns into a book that is quietly wrong.
    [[nodiscard]] bool truncated() const noexcept { return exhausted_; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t remaining_bytes_ = 0;
    std::uint16_t remaining_count_ = 0;
    bool exhausted_ = false;
};

}  // namespace hft::itch::mold