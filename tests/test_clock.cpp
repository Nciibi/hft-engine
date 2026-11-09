// Clock sync + kernel-bypass portable core tests: offset/drift
// estimation, step vs slew, monotonic discipline, timespec
// conversion, SO_TIMESTAMPING payload parsing (incl. rejection).

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "hft/feed/kernel_bypass.hpp"
#include "hft/util/clock_sync.hpp"

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

void check_eq_i64(std::int64_t actual, std::int64_t expected, const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL  %s: expected %lld, got %lld\n", what,
                    static_cast<long long>(expected), static_cast<long long>(actual));
    }
}

namespace util = hft::util;
namespace feed = hft::feed;

}  // namespace

int main() {
    // Constant offset: disciplined time = local + offset.
    {
        util::ClockSync cs;
        cs.observe(util::ClockSync::Sample{1'000'000'000LL, 1'000'005'000LL});  // +5us
        check_eq_i64(cs.offset_ns(), 5'000, "constant offset estimated");
        check_eq_i64(cs.disciplined(2'000'000'000LL), 2'000'005'000LL,
                     "disciplined time applies offset");
        check(cs.samples() == 1, "sample counted");
    }

    // Drift: remote gains 1000ns per 1s local (1000 ppb... here
    // 1000ns/1e9ns = 1 ppm = 1000 ppb).
    {
        util::ClockSync cs;
        cs.observe(util::ClockSync::Sample{0, 0});
        cs.observe(util::ClockSync::Sample{1'000'000'000LL, 1'000'001'000LL});
        check_eq_i64(cs.drift_ppb(), 1'000, "drift estimated at 1000 ppb");
        // At local=2s the remote should read 2s + 2000ns.
        check_eq_i64(cs.disciplined(2'000'000'000LL), 2'000'002'000LL,
                     "drift correction extrapolates");
    }

    // Step vs slew: a gross offset steps, a small one slews, and
    // the disciplined clock never goes backwards under slew.
    {
        util::ClockSync cs(1'000'000);  // 1ms threshold
        cs.observe(util::ClockSync::Sample{0, 5'000'000});  // +5ms gross
        check(cs.stepped(), "gross offset steps");
        check_eq_i64(cs.disciplined(1'000), 5'001'000, "step applies full offset");

        util::ClockSync cs2(1'000'000);
        cs2.observe(util::ClockSync::Sample{0, 500});  // +500ns small
        check(!cs2.stepped(), "small offset slews");
        std::int64_t prev = cs2.disciplined(0);
        bool monotonic = true;
        for (std::int64_t t = 1; t < 1000; ++t) {
            const std::int64_t now = cs2.disciplined(t);
            if (now < prev) {
                monotonic = false;
            }
            prev = now;
        }
        check(monotonic, "slewed clock is monotonic");
    }

    // timespec conversion.
    {
        check_eq_i64(feed::timespec_to_ns(1, 500), 1'000'000'500LL, "timespec 1s+500ns");
        check_eq_i64(feed::timespec_to_ns(0, 0), 0, "timespec zero");
        check_eq_i64(feed::timespec_to_ns(1723, 999'999'999LL), 1723999999999LL,
                     "timespec large");
    }

    // SO_TIMESTAMPING payload parsing: sec=1723, nsec=123456789 LE.
    {
        std::uint8_t buf[16] = {};
        const std::uint64_t sec = 1723;
        const std::uint64_t nsec = 123456789;
        for (int i = 0; i < 8; ++i) {
            buf[i] = static_cast<std::uint8_t>((sec >> (i * 8)) & 0xFFu);
            buf[8 + i] = static_cast<std::uint8_t>((nsec >> (i * 8)) & 0xFFu);
        }
        feed::PacketTimestamp ts;
        check(feed::parse_timestamp_payload(buf, sizeof(buf), ts), "payload parses");
        check_eq_i64(ts.ns, 1723'123'456'789LL, "payload value correct");
    }

    // Rejections: short buffer, bad nsec.
    {
        feed::PacketTimestamp ts;
        const std::uint8_t short_buf[8] = {};
        check(!feed::parse_timestamp_payload(short_buf, sizeof(short_buf), ts),
              "short payload rejected");
        std::uint8_t bad[16] = {};
        // nsec = 2,000,000,000 (out of range)
        const std::uint64_t bad_nsec = 2'000'000'000ULL;
        for (int i = 0; i < 8; ++i) {
            bad[8 + i] = static_cast<std::uint8_t>((bad_nsec >> (i * 8)) & 0xFFu);
        }
        check(!feed::parse_timestamp_payload(bad, sizeof(bad), ts),
              "out-of-range nsec rejected");
        check(!feed::parse_timestamp_payload(nullptr, 0, ts), "null rejected");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
