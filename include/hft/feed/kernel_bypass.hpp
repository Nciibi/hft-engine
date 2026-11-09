// Kernel-bypass / hardware-timestamp capture path.
//
// The portable core (always compiled, always tested): timestamp
// arithmetic and SO_TIMESTAMPING control-message parsing as pure
// functions over byte buffers. The Linux runtime (io_uring recv,
// SO_TIMESTAMPING socket options) is declared under
// `#if defined(__linux__)` and does not exist on other hosts --
// this repository's CI runs Windows and Linux, and a header that
// fails to compile on Windows is not a portable interface.
//
// What is NOT here, honestly: DPDK (needs a separate build system
// and root-owned hugepages), AF_XDP (needs BPF toolchain), and any
// claim that userspace capture beats the kernel on this host --
// that is a measurement for the benchmark host, not a header.

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <sys/types.h>
#endif

namespace hft::feed {

/// A packet timestamped by the kernel/NIC, in nanoseconds.
struct PacketTimestamp {
    std::int64_t ns = 0;   // SO_TIMESTAMPING hardware/software timestamp
    bool hardware = false; // true if from SO_TIMESTAMPING NIC path
};

/// Convert a (seconds, nanoseconds) pair to total nanoseconds.
/// Pure function: the same conversion the Linux cmsg parser uses,
/// testable on any host.
[[nodiscard]] inline std::int64_t timespec_to_ns(std::int64_t sec, std::int64_t nsec) noexcept {
    return sec * 1'000'000'000LL + nsec;
}

/// Parse a SO_TIMESTAMPING control message buffer for a timespec.
///
/// Layout (Linux, x86-64): cmsg_header is 16 bytes (len:8, level:4,
/// type:4), followed by the payload. SO_TIMESTAMPING payloads are
/// `struct timespec` (16 bytes: sec:8, nsec:8), possibly several.
/// Returns false if the buffer does not contain a usable payload.
/// The level/type check is done by the caller on Linux; this
/// function only validates the shape of what it is handed.
[[nodiscard]] inline bool parse_timestamp_payload(const std::uint8_t* data,
                                                  std::size_t size,
                                                  PacketTimestamp& out) noexcept {
    if (data == nullptr || size < 16) {
        return false;
    }
    const std::int64_t sec = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(data[0]) | (static_cast<std::uint64_t>(data[1]) << 8) |
        (static_cast<std::uint64_t>(data[2]) << 16) | (static_cast<std::uint64_t>(data[3]) << 24) |
        (static_cast<std::uint64_t>(data[4]) << 32) | (static_cast<std::uint64_t>(data[5]) << 40) |
        (static_cast<std::uint64_t>(data[6]) << 48) | (static_cast<std::uint64_t>(data[7]) << 56));
    const std::int64_t nsec = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(data[8]) | (static_cast<std::uint64_t>(data[9]) << 8) |
        (static_cast<std::uint64_t>(data[10]) << 16) | (static_cast<std::uint64_t>(data[11]) << 24) |
        (static_cast<std::uint64_t>(data[12]) << 32) | (static_cast<std::uint64_t>(data[13]) << 40) |
        (static_cast<std::uint64_t>(data[14]) << 48) | (static_cast<std::uint64_t>(data[15]) << 56));
    if (nsec < 0 || nsec >= 1'000'000'000LL) {
        return false;
    }
    out.ns = timespec_to_ns(sec, nsec);
    return true;
}

#if defined(__linux__)
/// Linux-only runtime declarations. Defined in src/feed/kernel_bypass.cpp;
/// on other hosts these functions do not exist and must not be
/// referenced (the portable core above is the portable interface).
namespace linux_bypass {
/// Enable SO_TIMESTAMPING (hardware if available) on `fd`.
/// Returns false on any setsockopt failure; never throws.
bool enable_hardware_timestamps(int fd) noexcept;

/// Receive one packet on a SO_TIMESTAMPING socket (recvmsg) and
/// extract its kernel/hardware timestamp into `out`. Returns the
/// packet size in bytes, or -1 on any failure (including a packet
/// with no timestamp cmsg). Never throws.
ssize_t recv_timestamped(int fd, void* buf, std::size_t size, PacketTimestamp& out) noexcept;

/// Create an io_uring instance with `entries` SQ entries via the
/// raw syscall (no liburing dependency). Returns the ring fd, or
/// -1 on failure. The ring must be submitted/polled by the caller.
int create_ring(unsigned entries) noexcept;

/// Wait for io_uring completions, up to `timeout_ms`. Returns true
/// if at least one completion is ready, false on timeout/error.
bool poll_ring(int ring_fd, long timeout_ms) noexcept;
}  // namespace linux_bypass
#endif

}  // namespace hft::feed
