// SOUP sequence tracking.
//
// Every message in a MoldUDP64 stream carries a monotonically
// increasing sequence number. A gap means messages were lost, which
// means the reconstructed book is missing orders that the venue
// believes are resting. Continuing past a gap produces a book that
// looks healthy and is wrong, and the error surfaces later as
// unexplained inventory.
//
// The policy this type implements is therefore: detect, report, and
// never silently continue. Resynchronising is an explicit act.
//
// Scope, stated rather than implied: this tracks the sequence numbers
// that the transport hands us. It does not implement MoldUDP64 packet
// framing or its checksum, so a full feed capture is expected to carry
// one sequence number per message out of band. A production build
// would parse the 12-byte MoldUDP64 header here instead.

#pragma once

#include <cstdint>

namespace hft::itch {

/// Wraps 32-bit sequence arithmetic, which is where this class earns
/// its keep. A raw `observed == expected + 1` comparison is correct
/// across 99.99999% of a session and wrong exactly at the wrap, which
/// is the one moment a bug is hardest to reproduce. Signed comparison
/// makes it wrong in the opposite direction. Modular arithmetic is
/// correct at the wrap and nowhere else ambiguous, provided the gap
/// being measured is smaller than half the sequence space, which for a
/// live feed is not a close call.
class SequenceTracker final {
public:
    enum class State : std::uint8_t {
        /// Exactly the next expected sequence number.
        ok = 0,
        /// Forward jump: `missing` messages were lost.
        gap,
        /// Backwards jump. This covers BOTH a retransmit of a packet
        /// already seen and a genuinely stale packet, because the
        /// sequence number alone cannot tell them apart: both simply
        /// arrive lower than expected. An earlier revision of this
        /// enum split them on an arbitrary magnitude threshold, which
        /// made the distinction look meaningful while being
        /// unprincipled. Collapsing them is the honest answer, and a
        /// consumer that needs the difference has to look at the
        /// transport, not here.
        duplicate,
    };

    /// Sequence number of the first message in the session.
    explicit SequenceTracker(std::uint32_t first_sequence) noexcept
        : next_(first_sequence) {}

    /// Observe the sequence number of the next message.
    [[nodiscard]] State observe(std::uint32_t observed) noexcept {
        const std::uint32_t delta = observed - next_;  // modular
        if (delta == 0) {
            ++next_;
            ++accepted_;
            last_ = State::ok;
            return State::ok;
        }
        if (delta < kHalfSpace) {
            // Forward jump. Everything in [next_, observed) was lost.
            missing_ += delta;
            ++gaps_;
            next_ = observed + 1u;
            last_ = State::gap;
            return State::gap;
        }
        if (delta > kHalfSpace) {
            // Backwards: a retransmit or a stale packet. Either way the
            // message has already been accounted for, so the
            // expectation does not move.
            last_ = State::duplicate;
            ++rejects_;
            return last_;
        }
        // Exactly half the sequence space. Ambiguous by construction:
        // this could be a forward jump of 2^31 or a backward jump of
        // 2^31. Refuse to classify it rather than pick one.
        last_ = State::duplicate;
        ++rejects_;
        return last_;
    }

    /// Accept the current position as authoritative and continue.
    /// This is how a consumer recovers after a gap, and it is only
    /// correct if the book is rebuilt from a fresh snapshot first:
    /// resynchronising the counter does not repair the state the lost
    /// messages would have updated.
    void resync() noexcept { ++resyncs_; }

    [[nodiscard]] std::uint32_t expected() const noexcept { return next_; }
    [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }
    [[nodiscard]] std::uint64_t missing() const noexcept { return missing_; }
    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
    [[nodiscard]] std::uint64_t rejects() const noexcept { return rejects_; }
    [[nodiscard]] std::uint64_t resyncs() const noexcept { return resyncs_; }
    [[nodiscard]] State last() const noexcept { return last_; }

    /// True when the stream has been contiguous so far.
    [[nodiscard]] bool clean() const noexcept {
        return missing_ == 0 && rejects_ == 0;
    }

    [[nodiscard]] static constexpr std::uint32_t kHalfSpace = 0x8000'0000u;

private:
    std::uint32_t next_ = 0;
    std::uint64_t accepted_ = 0;
    std::uint64_t missing_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t rejects_ = 0;
    std::uint64_t resyncs_ = 0;
    State last_ = State::ok;
};

}  // namespace hft::itch
