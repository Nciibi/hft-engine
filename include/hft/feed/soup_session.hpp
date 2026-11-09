// SOUP session logic: login, heartbeat, gap detection, retransmit
// requests.
//
// This is the session layer the capture path assumes but never
// implemented: before any message flows, the client logs in and
// negotiates a session id; while messages flow, a sequence gap is
// answered with a retransmit request for the missing range -- not
// silently continued past, which is what the old capture reader did
// ("the gap detector is only as good as the packets it is given").
//
// In-process and transport-agnostic: the caller owns the socket;
// this class owns the state machine and produces the exact wire
// bytes for each control message (login request, heartbeat response,
// retransmit request), big-endian, per the SOUP spec. A live
// exchange needs a Nasdaq agreement; this implements the protocol
// against captured/generated feeds exactly as the rest of the repo
// does.
//
// State machine:
//   idle -> logging in -> ready -> (gap) -> retransmitting -> ready
//                           \-> (logout/EOF) -> closed
// Heartbeats: a server heartbeat (no data) is answered with a
// client heartbeat echoing the heartbeat sequence field; an
// application message resets the server-alive timer. If no
// heartbeat arrives within `heartbeat_timeout_ns`, the session is
// declared dead -- silently waiting forever is how a handler
// misses a dead feed for an hour.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hft::feed {

enum class SoupState : std::uint8_t {
    idle = 0,
    logging_in,
    ready,
    retransmitting,
    closed,
};

/// Big-endian append helpers (same discipline as persist/wal.hpp).
inline void soup_put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}
inline void soup_put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}
inline void soup_put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}
inline void soup_put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

class SoupSession final {
public:
    /// `username`/`password`: the login credentials as fixed-width
    /// 10-byte fields, space padded (SOUP Login Request layout).
    /// `heartbeat_timeout_ns`: server-alive deadline, default 1s.
    SoupSession(const char* username, const char* password,
                std::int64_t heartbeat_timeout_ns = 1'000'000'000LL) noexcept
        : heartbeat_timeout_ns_(heartbeat_timeout_ns) {
        std::memcpy(username_, username, 10);
        std::memcpy(password_, password, 10);
    }

    /// Build the Login Request wire bytes and enter `logging_in`.
    std::vector<std::uint8_t> login_request() {
        state_ = SoupState::logging_in;
        std::vector<std::uint8_t> out;
        soup_put_u8(out, 'L');
        out.insert(out.end(), username_, username_ + 10);
        out.insert(out.end(), password_, password_ + 10);
        return out;
    }

    /// The server's Login Accepted / Login Rejected.
    void login_accepted(std::uint64_t session_id, std::uint64_t heartbeat_seq) noexcept {
        state_ = SoupState::ready;
        session_id_ = session_id;
        heartbeat_seq_ = heartbeat_seq;
        expected_seq_ = heartbeat_seq + 1;
        last_alive_ns_ = 0;
        self_alive_set_ = false;
    }
    void login_rejected() noexcept {
        state_ = SoupState::idle;
    }

    /// An application message arrived with sequence `seq` at time
    /// `now_ns`. Returns the Retransmit Request wire bytes if
    /// `seq` jumps past the expected sequence (a gap), empty
    /// otherwise. Entering `retransmitting` pauses gap detection
    /// until `retransmit_done()` -- re-requesting in a loop is how
    /// a client storms a server with duplicate requests.
    std::vector<std::uint8_t> on_message(std::uint64_t seq, std::int64_t now_ns) {
        last_alive_ns_ = now_ns;
        self_alive_set_ = true;
        std::vector<std::uint8_t> out;
        if (state_ != SoupState::ready) {
            return out;
        }
        if (seq == expected_seq_) {
            expected_seq_ = seq + 1;
        } else if (seq > expected_seq_) {
            // Gap: request [expected, seq). `count` is number of
            // messages requested (SOUP Retransmit Request count
            // field is u16; the range here must fit -- longer
            // gaps are requested in chunks by the caller).
            state_ = SoupState::retransmitting;
            soup_put_u8(out, 'R');
            soup_put_u64(out, session_id_);
            soup_put_u64(out, expected_seq_);
            const std::uint64_t count = seq - expected_seq_;
            soup_put_u16(out, static_cast<std::uint16_t>(count > 0xFFFFu ? 0xFFFFu : count));
            ++retransmit_requests_;
        }
        // seq < expected: a retransmit/duplicate already accounted
        // for; ignored, expected does not move backwards.
        return out;
    }

    /// Retransmitted messages arrived; resume normal sequencing.
    void retransmit_done(std::uint64_t next_seq) noexcept {
        if (state_ == SoupState::retransmitting) {
            state_ = SoupState::ready;
        }
        expected_seq_ = next_seq;
    }

    /// Server heartbeat (no data). Echo it back and reset the
    /// alive timer. The echo carries the heartbeat's sequence
    /// field, which is where the request/response pair meets.
    std::vector<std::uint8_t> on_heartbeat(std::uint64_t heartbeat_seq, std::int64_t now_ns) {
        last_alive_ns_ = now_ns;
        self_alive_set_ = true;
        heartbeat_seq_ = heartbeat_seq;
        std::vector<std::uint8_t> out;
        soup_put_u8(out, 'H');
        soup_put_u64(out, heartbeat_seq);
        return out;
    }

    /// True if no heartbeat/application message arrived within the
    /// timeout. Only meaningful after login; before that the
    /// session has not started and cannot be dead.
    [[nodiscard]] bool alive(std::int64_t now_ns) const noexcept {
        if (state_ == SoupState::idle || state_ == SoupState::logging_in ||
            !self_alive_set_) {
            return state_ != SoupState::idle;
        }
        return now_ns - last_alive_ns_ <= heartbeat_timeout_ns_;
    }

    void logout() noexcept {
        state_ = SoupState::closed;
    }

    [[nodiscard]] SoupState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t session_id() const noexcept { return session_id_; }
    [[nodiscard]] std::uint64_t expected_seq() const noexcept { return expected_seq_; }
    [[nodiscard]] std::uint64_t retransmit_requests() const noexcept {
        return retransmit_requests_;
    }

private:
    char username_[10] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    char password_[10] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    std::int64_t heartbeat_timeout_ns_;
    SoupState state_ = SoupState::idle;
    std::uint64_t session_id_ = 0;
    std::uint64_t heartbeat_seq_ = 0;
    std::uint64_t expected_seq_ = 0;
    std::uint64_t retransmit_requests_ = 0;
    std::int64_t last_alive_ns_ = 0;
    bool self_alive_set_ = false;
};

}  // namespace hft::feed
