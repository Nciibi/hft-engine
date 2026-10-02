// Sequence tracking for a MoldUDP64 stream.
//
// Every message in a MoldUDP64 stream carries a monotonically
// increasing sequence number, and the field is EIGHT BYTES WIDE --
// `hft/itch/moldudp64.hpp` asserts that against the specification's
// field table. A gap means messages were lost, which means the
// reconstructed book is missing orders the venue believes are resting.
// Continuing past a gap produces a book that looks healthy and is
// wrong, and the error surfaces later as unexplained inventory.
//
// The policy this type implements is therefore: detect, report, and
// never silently continue. Resynchronising is an explicit act.
//
// ---- Why 64 bits, and why that matters ---------------------------
//
// An earlier revision of this file wrapped 32-bit arithmetic, with
// careful tests for the 32-bit wrap, and its header comment described
// a "12-byte MoldUDP64 header". Neither was right: the header is 20
// bytes and the Sequence Number field in it is 8. Keeping a 32-bit
// counter meant the high half of every sequence number was discarded
// before this type ever saw it.
//
// The modular-arithmetic argument is unchanged by the width, which is
// the point: `observed == expected + 1` is wrong at exactly one moment
// per session, and that moment is the wrap. Signed comparison is wrong
// in the opposite direction. Modular arithmetic is correct at the wrap
// and nowhere else ambiguous, provided the gap being measured is
// smaller than half the sequence space -- 2^63 messages here, which is
// not a close call.

#pragma once

#include <cstdint>

namespace hft::itch {

/// Tracks sequence numbers across messages or whole packets.
///
/// NOT THREAD SAFE, and deliberately: one tracker per consumer, fed by
/// whichever thread owns that consumer. Sharing it would need
/// synchronisation, and a synchronised gap check is a gap check that
/// can be made to miss by the very ordering it has to preserve.
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
    explicit SequenceTracker(std::uint64_t first_sequence) noexcept
        : next_(first_sequence) {}

    /// Observe the sequence number of one message.
    [[nodiscard]] State observe(std::uint64_t observed) noexcept {
        const std::uint64_t delta = observed - next_;  // modular
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
        // Backwards, or exactly half the sequence space, which is
        // ambiguous by construction: a forward jump of 2^63 and a
        // backward jump of 2^63 look identical. Refuse to classify it
        // rather than pick one.
        //
        // Either way the message has already been accounted for, so
        // the expectation does not move.
        last_ = State::duplicate;
        ++rejects_;
        return last_;
    }

    /// Observe a whole packet: `first_sequence` is the sequence of its
    /// first message and `count` is how many messages it carries.
    ///
    /// This is the entry point a real handler uses, because a real
    /// handler is handed packets and not messages. The sequence field
    /// applies to the FIRST block only -- the specification says the
    /// rest are implicitly sequential -- so one observation covers the
    /// whole packet rather than the packet needing `count` separate
    /// ones. Observing per message would also be correct, just slower,
    /// and would report an intra-packet gap as though it were a lost
    /// packet.
    ///
    /// Returns the state of the packet's FIRST message, which is the
    /// one that carries the diagnostic. `count` of 0 is a heartbeat or
    /// an end-of-session packet: those carry the next expected sequence
    /// rather than a message number, so they are reported and not
    /// counted.
    [[nodiscard]] State observe_packet(std::uint64_t first_sequence, std::uint16_t count) noexcept {
        if (count == 0) {
            // A heartbeat states where the sender thinks we are. That
            // is a resynchronisation OFFER, not a message, so it is
            // deliberately not treated as one: a heartbeat arriving
            // mid-stream is a real signal that messages were lost, and
            // applying it silently would hide exactly the event this
            // class exists to surface.
            //
            // It is not counted either way -- a heartbeat contains no
            // messages -- so `accepted` does not move. Only the verdict
            // is computed.
            const std::uint64_t delta = first_sequence - next_;
            if (delta == 0) {
                // The sender agrees with us. That is the healthy case
                // and it must not be reported as a duplicate, or every
                // idle feed would look like a retransmission storm and
                // `clean()` would never be true.
                last_ = State::ok;
                return last_;
            }
            if (delta < kHalfSpace) {
                missing_ += delta;
                ++gaps_;
                last_ = State::gap;
                return last_;
            }
            last_ = State::duplicate;
            ++rejects_;
            return last_;
        }

        const State first = observe(first_sequence);
        // The remaining count-1 messages are implicit, so they are
        // accounted for without a second comparison: a forward jump
        // within a packet cannot happen, because the packet is one
        // contiguous run.
        if (first == State::ok || first == State::gap) {
            const std::uint64_t implicit = static_cast<std::uint64_t>(count) - 1u;
            accepted_ += implicit;
            next_ += implicit;
        }
        // A duplicate packet is not counted at all, including its
        // implicit tail: the whole packet has already been seen.
        return first;
    }

    /// Accept the current position as authoritative and continue.
    /// This is how a consumer recovers after a gap, and it is only
    /// correct if the book is rebuilt from a fresh snapshot first:
    /// resynchronising the counter does not repair the state the lost
    /// messages would have updated.
    void resync() noexcept { ++resyncs_; }

    [[nodiscard]] std::uint64_t expected() const noexcept { return next_; }
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

    // Plain constexpr, not [[nodiscard]]: it is a tuning constant, not
    // a call, and some compilers warn about the attribute here.
    static constexpr std::uint64_t kHalfSpace = 0x8000'0000'0000'0000ULL;

private:
    std::uint64_t next_ = 0;
    std::uint64_t accepted_ = 0;
    std::uint64_t missing_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t rejects_ = 0;
    std::uint64_t resyncs_ = 0;
    State last_ = State::ok;
};

}  // namespace hft::itch
