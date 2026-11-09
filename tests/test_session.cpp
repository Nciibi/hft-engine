// SOUP session tests: login wire format, contiguous sequencing,
// gap -> retransmit request bytes, duplicate ignore, heartbeat
// echo + alive timeout, retransmit_done resume.

#include <cstdio>
#include <cstdint>
#include <vector>

#include "hft/feed/soup_session.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

namespace feed = hft::feed;

}  // namespace

int main() {
    // Login request: 'L' + 10-byte username + 10-byte password.
    {
        feed::SoupSession s("user", "pass");
        const auto req = s.login_request();
        check(s.state() == feed::SoupState::logging_in, "login enters logging_in");
        check(req.size() == 21, "login request is 21 bytes");
        check(req[0] == static_cast<std::uint8_t>('L'), "login tag is 'L'");
        check(req[1] == static_cast<std::uint8_t>('u') &&
                  req[4] == static_cast<std::uint8_t>('r') &&
                  req[5] == static_cast<std::uint8_t>(' '),
              "username is space-padded to 10");
        check(req[11] == static_cast<std::uint8_t>('p'), "password follows username");
        s.login_accepted(42, 100);
        check(s.state() == feed::SoupState::ready, "accepted enters ready");
        check(s.session_id() == 42, "session id recorded");
        check(s.expected_seq() == 101, "expected starts at heartbeat+1");
    }

    // Contiguous messages: no retransmit, expected advances.
    {
        feed::SoupSession s("user", "pass");
        s.login_request();
        s.login_accepted(7, 100);
        check(s.on_message(101, 1'000).empty(), "contiguous message: no retransmit");
        check(s.on_message(102, 2'000).empty(), "second contiguous: no retransmit");
        check(s.expected_seq() == 103, "expected advanced past both");
        check(s.retransmit_requests() == 0, "no retransmits requested");
    }

    // Gap: seq jumps to 105 -> Retransmit Request 'R' + session(8)
    // + first(8) + count(2) = 1+8+8+2 = 19 bytes, count = 2.
    {
        feed::SoupSession s("user", "pass");
        s.login_request();
        s.login_accepted(9, 100);
        s.on_message(101, 1'000);  // expected now 102
        const auto req = s.on_message(105, 2'000);
        check(req.size() == 19, "retransmit request is 19 bytes");
        check(req[0] == static_cast<std::uint8_t>('R'), "retransmit tag is 'R'");
        check(s.state() == feed::SoupState::retransmitting, "gap enters retransmitting");
        check(s.retransmit_requests() == 1, "retransmit counted");
        // Count field: bytes 17..18 big-endian == 3 (102,103,104)
        check(req[17] == 0 && req[18] == 3, "retransmit count is 3");
        // While retransmitting, further gaps do not re-request.
        check(s.on_message(110, 3'000).empty(), "no duplicate request while retransmitting");
        s.retransmit_done(105);
        check(s.state() == feed::SoupState::ready, "retransmit_done resumes ready");
        check(s.expected_seq() == 105, "expected set by retransmit_done");
        check(s.on_message(105, 4'000).empty(), "post-retransmit contiguous ok");
    }

    // Duplicate: seq below expected is ignored, expected unmoved.
    {
        feed::SoupSession s("user", "pass");
        s.login_request();
        s.login_accepted(1, 10);
        s.on_message(11, 0);
        check(s.on_message(11, 100).empty(), "duplicate ignored");
        check(s.expected_seq() == 12, "duplicate does not move expected");
    }

    // Heartbeat echo + alive timeout.
    {
        feed::SoupSession s("user", "pass", 1'000'000);
        s.login_request();
        s.login_accepted(3, 50);
        const auto echo = s.on_heartbeat(60, 5'000'000);
        check(echo.size() == 9 && echo[0] == static_cast<std::uint8_t>('H'),
              "heartbeat echo is 'H' + 8-byte seq");
        check(echo[8] == 60, "heartbeat echo carries heartbeat seq low byte");
        check(s.alive(5'500'000), "alive within timeout");
        check(!s.alive(7'000'000), "dead after timeout without heartbeat");
        s.on_message(51, 7'000'000);
        check(s.alive(7'500'000), "application message resets alive timer");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
