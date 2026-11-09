// Linux kernel-bypass runtime: SO_TIMESTAMPING reception and raw
// io_uring setup. The portable core (timestamp math, payload
// parsing) is header-only in hft/feed/kernel_bypass.hpp and is
// what Windows/clang CI exercises; this file only exists on Linux.
// It depends on nothing beyond libc: io_uring is entered through
// the syscall directly, so there is no liburing dependency to
// install, version, or link.

#if defined(__linux__)

#include "hft/feed/kernel_bypass.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>

#include <linux/io_uring.h>
#include <linux/net_tstamp.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

namespace hft::feed::linux_bypass {

bool enable_hardware_timestamps(int fd) noexcept {
    int flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
                SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
    return ::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0;
}

ssize_t recv_timestamped(int fd, void* buf, std::size_t size, PacketTimestamp& out) noexcept {
    struct iovec iov = {buf, size};
    // Two timespecs of slack: SO_TIMESTAMPING can deliver both a
    // hardware and a software timestamp in one cmsg.
    std::uint8_t control[CMSG_SPACE(2 * sizeof(struct timespec))];
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    const ssize_t n = ::recvmsg(fd, &msg, 0);
    if (n < 0) {
        return -1;
    }
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_TIMESTAMPING) {
            continue;
        }
        PacketTimestamp ts;
        if (parse_timestamp_payload(CMSG_DATA(cmsg), CMSG_LEN(cmsg) - CMSG_LEN(0), ts)) {
            out = ts;
            out.hardware = true;  // SCM_TIMESTAMPING: kernel-written stamp
            return n;
        }
    }
    return -1;  // packet arrived without a timestamp cmsg
}

int create_ring(unsigned entries) noexcept {
    struct io_uring_params params{};
    const long fd = ::syscall(SYS_io_uring_setup, entries, &params);
    return fd < 0 ? -1 : static_cast<int>(fd);
}

bool poll_ring(int ring_fd, long timeout_ms) noexcept {
    struct timespec ts{};
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1'000'000L;
    const long res = ::syscall(SYS_io_uring_enter, ring_fd, 0, 0,
                               IORING_ENTER_GETEVENTS, nullptr, &ts);
    return res > 0;
}

}  // namespace hft::feed::linux_bypass

#endif  // defined(__linux__)
