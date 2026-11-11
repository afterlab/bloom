#pragma once
#include "lut_worker_protocol.hpp"
#if defined(__linux__) || defined(__APPLE__)
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace bloom::color::detail {
// Monotonic per-process generation for the abstract descriptor socket name. Shared so two callers
// in one process (CPU processor preparation and GPU program extraction) can never collide on a
// name.
inline std::uint64_t nextLutGeneration() noexcept {
    static std::atomic<std::uint64_t> generations{0};
    return ++generations;
}
#ifdef __linux__
// An abstract name: it exists only while the listener does and is in no filesystem.
inline sockaddr_un lutSocketAddress(const std::uint64_t pid, const std::uint64_t generation) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = "bloom-color-" + std::to_string(pid) + "-" + std::to_string(generation);
    std::copy(name.begin(), name.end(), address.sun_path + 1);
    return address;
}
inline constexpr int kLutSocketType = SOCK_SEQPACKET;
#else
// macOS has no abstract names and no sequenced-packet local sockets: the name is a file in the
// user's private temporary directory, removed as soon as the helper has connected, and the socket
// is a stream that carries one marker byte per descriptor message. An address with an empty path
// (no private directory, or a name too long) cannot be bound or connected.
inline sockaddr_un lutSocketAddress(const std::uint64_t pid, const std::uint64_t generation) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto directory = lutPrivateDirectory();
    const auto name =
        directory + "bloom-color-" + std::to_string(pid) + "-" + std::to_string(generation);
    if (!directory.empty() && name.size() < sizeof(address.sun_path))
        std::copy(name.begin(), name.end(), address.sun_path);
    return address;
}
inline constexpr int kLutSocketType = SOCK_STREAM;
#endif
// Marks a descriptor close-on-exec and, when asked, non-blocking, where the platform cannot do it
// in the call that makes the descriptor.
inline bool lutPrepareDescriptor(const int fd, const bool nonBlocking) {
    if (fd < 0)
        return false;
#ifdef __linux__
    static_cast<void>(nonBlocking);
    return true;
#else
    const int flags = ::fcntl(fd, F_GETFL);
    int noSignal = 1;
    return ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && flags >= 0 &&
           (!nonBlocking || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) &&
           ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal)) == 0;
#endif
}
// The host's listener for one helper generation. The helper connects to it while it answers the
// handshake; the host then accepts that one connection and checks who is on the other end.
class LutListener final {
  public:
    LutListener(const std::uint64_t pid, const std::uint64_t generation)
        : address_(lutSocketAddress(pid, generation)) {
#ifdef __linux__
        fd_ = ::socket(AF_UNIX, kLutSocketType | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
        fd_ = ::socket(AF_UNIX, kLutSocketType, 0);
        if (!lutPrepareDescriptor(fd_, true) || address_.sun_path[0] == '\0') {
            closeDescriptor();
            return;
        }
        // A name left by a crashed process with the same id and generation.
        ::unlink(address_.sun_path);
#endif
        if (fd_ < 0)
            return;
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address_), sizeof(address_)) != 0) {
            closeDescriptor();
            return;
        }
        bound_ = true;
        if (::listen(fd_, 1) != 0)
            closeDescriptor();
    }
    ~LutListener() { closeDescriptor(); }
    LutListener(const LutListener&) = delete;
    LutListener& operator=(const LutListener&) = delete;
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    // The helper's connection, close-on-exec and non-blocking, or -1.
    [[nodiscard]] int accept() {
#ifdef __linux__
        return ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
        const int connection = ::accept(fd_, nullptr, nullptr);
        // The name has done its work once the one helper is connected.
        removeName();
        if (connection >= 0 && !lutPrepareDescriptor(connection, true)) {
            ::close(connection);
            return -1;
        }
        return connection;
#endif
    }

  private:
    void removeName() noexcept {
#ifndef __linux__
        if (bound_)
            ::unlink(address_.sun_path);
#endif
        bound_ = false;
    }
    void closeDescriptor() noexcept {
        removeName();
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }
    sockaddr_un address_;
    int fd_ = -1;
    bool bound_ = false;
};
// Whether the process on the other end of a connected descriptor socket is `pid`.
inline bool lutPeerIs(const int socket, const std::int64_t pid) {
#ifdef __linux__
    ucred peer{};
    socklen_t peerSize = sizeof(peer);
    return ::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) == 0 && peer.pid == pid;
#else
    pid_t peer = 0;
    socklen_t peerSize = sizeof(peer);
    return ::getsockopt(socket, SOL_LOCAL, LOCAL_PEERPID, &peer, &peerSize) == 0 && peer == pid;
#endif
}
// The helper's end: a blocking, close-on-exec socket, connected by lutConnect during the handshake.
inline int lutHelperSocket() {
#ifdef __linux__
    return ::socket(AF_UNIX, kLutSocketType | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(AF_UNIX, kLutSocketType, 0);
    if (fd >= 0 && !lutPrepareDescriptor(fd, false)) {
        ::close(fd);
        return -1;
    }
    return fd;
#endif
}
inline bool lutConnect(const int socket, const std::uint64_t pid, const std::uint64_t generation) {
    const auto address = lutSocketAddress(pid, generation);
#ifndef __linux__
    if (address.sun_path[0] == '\0')
        return false;
#endif
    return ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
}
inline bool sendDescriptors(const int socket, const std::span<const int> descriptors) {
    if (descriptors.empty() || descriptors.size() > 2)
        return false;
    char marker = 'F';
    iovec io{&marker, 1};
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(2 * sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    // The length fields are size_t on Linux and socklen_t on macOS.
    message.msg_controllen =
        static_cast<decltype(message.msg_controllen)>(CMSG_SPACE(descriptors.size_bytes()));
    auto* header = CMSG_FIRSTHDR(&message);
    if (!header)
        return false;
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = static_cast<decltype(header->cmsg_len)>(CMSG_LEN(descriptors.size_bytes()));
    std::memcpy(CMSG_DATA(header), descriptors.data(), descriptors.size_bytes());
#ifdef __linux__
    return ::sendmsg(socket, &message, MSG_NOSIGNAL | MSG_DONTWAIT) == 1;
#else
    // SO_NOSIGPIPE is set on the socket (lutPrepareDescriptor).
    return ::sendmsg(socket, &message, MSG_DONTWAIT) == 1;
#endif
}
inline std::array<int, 2> receiveDescriptors(const int socket, const std::size_t count) {
    char marker = 0;
    iovec io{&marker, 1};
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(2 * sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
#ifdef __linux__
    constexpr int receiveFlags = MSG_CMSG_CLOEXEC;
#else
    constexpr int receiveFlags = 0;
#endif
    if (::recvmsg(socket, &message, receiveFlags) != 1 || marker != 'F')
        return {-1, -1};
    // Not const: macOS's CMSG_DATA casts the header to a mutable byte pointer.
    auto* header = CMSG_FIRSTHDR(&message);
    std::array<int, 2> descriptors{-1, -1};
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len < CMSG_LEN(0) ||
        header->cmsg_len > CMSG_LEN(descriptors.size() * sizeof(int)))
        return descriptors;
    const auto bytes = header->cmsg_len - CMSG_LEN(0);
    std::memcpy(descriptors.data(), CMSG_DATA(header), bytes);
#ifdef __linux__
    const bool whole = message.msg_flags == MSG_CMSG_CLOEXEC;
#else
    // No flag reports close-on-exec here; set it on each descriptor instead. Truncated control
    // data means descriptors were dropped.
    bool whole = (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) == 0;
    for (const auto fd : descriptors)
        if (fd >= 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
            whole = false;
#endif
    if (bytes != count * sizeof(int) || !whole) {
        for (const auto fd : descriptors)
            if (fd >= 0)
                ::close(fd);
        return {-1, -1};
    }
    return descriptors;
}
} // namespace bloom::color::detail
#endif
