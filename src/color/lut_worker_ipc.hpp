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

#include <bloom/platform/process_supervisor.hpp>
#include <optional>

// What the host and the helper each do with a shared file, under one set of names for the three
// systems. On Linux and macOS a shared file is a descriptor that travels over the descriptor
// socket and is named by its inode; on Windows it is a section the supervisor hands to the helper
// and is named by the helper's handle value.
namespace bloom::color::detail {
// Windows confines the helper when it is launched; Linux and macOS helpers confine themselves
// once they hold their files.
#ifdef _WIN32
inline constexpr bool kLutLaunchConfined = true;
#else
inline constexpr bool kLutLaunchConfined = false;
#endif
// --- helper side ---
#ifdef _WIN32
using LutHelperFile = LutSection;
inline std::array<LutHelperFile, 2> receiveFiles(const int, const std::size_t count,
                                                 const std::array<std::uint64_t, 2> identities) {
    const auto handle = [](const std::uint64_t value) {
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    };
    return {LutSection(handle(identities[0])),
            LutSection(count > 1 ? handle(identities[1]) : nullptr)};
}
inline bool validHelperFile(const LutHelperFile& file, const std::uint64_t,
                            const std::uint64_t bytes, const bool input) {
    return validSharedSection(file.get(), bytes, input);
}
inline bool readHelperFile(const LutHelperFile& file, const std::span<std::byte> bytes) {
    return readSection(file.get(), bytes);
}
inline bool writeHelperFile(const LutHelperFile& file, const std::span<const std::byte> bytes) {
    return writeSection(file.get(), bytes);
}
#else
using LutHelperFile = LutFd;
inline std::array<LutHelperFile, 2> receiveFiles(const int socket, const std::size_t count,
                                                 const std::array<std::uint64_t, 2>) {
    const auto descriptors = receiveDescriptors(socket, count);
    return {LutFd(descriptors[0]), LutFd(descriptors[1])};
}
inline bool validHelperFile(const LutHelperFile& file, const std::uint64_t identity,
                            const std::uint64_t bytes, const bool input) {
    return file.get() >= 0 && validSharedFile(file.get(), identity, bytes, input);
}
inline bool readHelperFile(const LutHelperFile& file, const std::span<std::byte> bytes) {
    return ::lseek(file.get(), 0, SEEK_SET) == 0 && readFd(file.get(), bytes);
}
inline bool writeHelperFile(const LutHelperFile& file, const std::span<const std::byte> bytes) {
    return ::lseek(file.get(), 0, SEEK_SET) == 0 && writeFd(file.get(), bytes);
}
#endif

// --- host side ---
// One file the host shares with its helper: an input the helper reads, or an output of a fixed
// size the helper fills.
class LutSharedFile final {
  public:
    explicit LutSharedFile(const std::span<const std::byte> content)
#ifdef _WIN32
        : file_(createSection(content.size())), input_(true) {
        valid_ = file_.valid() && writeSection(file_.get(), content);
    }
#else
        : file_(sharedFile()), input_(true) {
        valid_ = file_.get() >= 0 && writeFd(file_.get(), content) && sealInput(file_.get());
    }
#endif
    explicit LutSharedFile(const std::uint64_t outputBytes)
#ifdef _WIN32
        : file_(createSection(outputBytes)), input_(false) {
        valid_ = file_.valid();
    }
#else
        : file_(sharedFile()), input_(false) {
        valid_ = file_.get() >= 0 &&
                 ::ftruncate(file_.get(), static_cast<off_t>(outputBytes)) == 0 &&
                 sealExtent(file_.get());
    }
#endif
    [[nodiscard]] bool valid() const { return valid_; }
    // Stops further change to an output where the system can (Linux); the host reads its own
    // copy once either way.
    [[nodiscard]] bool freeze() const {
#ifdef _WIN32
        return true;
#else
        return sealInput(file_.get());
#endif
    }
    // The first bytes of the file, read into the host's own memory.
    [[nodiscard]] bool read(const std::span<std::byte> bytes) const {
        return readHelperFile(file_, bytes);
    }

  private:
    friend class LutFileChannel;
    LutHelperFile file_;
    [[maybe_unused]] bool input_; // read where access is granted per file (Windows)
    bool valid_ = false;
};

// The host's way of giving shared files to one helper.
class LutFileChannel final {
  public:
#ifdef _WIN32
    LutFileChannel() = default;
    [[nodiscard]] bool valid() const { return true; }
    void announce(LutPacket& hello) const { hello[5] = GetCurrentProcessId(); }
    // After the handshake.
    [[nodiscard]] bool attach(platform::ProcessSupervisor& worker) {
        worker_ = &worker;
        return true;
    }
    // Gives the helper one or two files; the identities to name them by in the request.
    [[nodiscard]] std::optional<std::array<std::uint64_t, 2>>
    send(const LutSharedFile& first, const LutSharedFile* const second = nullptr) const {
        const auto share = [&](const LutSharedFile& file) {
            return worker_->shareHandle(file.file_.get(),
                                        SECTION_QUERY | SECTION_MAP_READ |
                                            (file.input_ ? 0U : SECTION_MAP_WRITE));
        };
        if (worker_ == nullptr || !first.valid() || (second != nullptr && !second->valid()))
            return std::nullopt;
        const auto one = share(first);
        const auto two = second != nullptr ? share(*second) : std::optional<std::uint64_t>(0);
        if (!one || !two)
            return std::nullopt;
        return std::array{*one, *two};
    }

  private:
    platform::ProcessSupervisor* worker_ = nullptr;
#else
    LutFileChannel() : generation_(nextLutGeneration()) {
        listener_.emplace(static_cast<std::uint64_t>(::getpid()), generation_);
    }
    ~LutFileChannel() {
        if (socket_ >= 0)
            ::close(socket_);
    }
    LutFileChannel(const LutFileChannel&) = delete;
    LutFileChannel& operator=(const LutFileChannel&) = delete;
    [[nodiscard]] bool valid() const { return listener_ && listener_->valid(); }
    void announce(LutPacket& hello) const {
        hello[5] = static_cast<std::uint64_t>(::getpid());
        hello[6] = generation_;
    }
    // After the handshake: the helper has connected; accept it and check who it is.
    [[nodiscard]] bool attach(platform::ProcessSupervisor& worker) {
        if (!listener_)
            return false;
        socket_ = listener_->accept();
        listener_.reset();
        return socket_ >= 0 && lutPeerIs(socket_, worker.processId());
    }
    // Gives the helper one or two files; the identities to name them by in the request.
    [[nodiscard]] std::optional<std::array<std::uint64_t, 2>>
    send(const LutSharedFile& first, const LutSharedFile* const second = nullptr) const {
        if (socket_ < 0 || !first.valid() || (second != nullptr && !second->valid()))
            return std::nullopt;
        const std::array descriptors{first.file_.get(),
                                     second != nullptr ? second->file_.get() : -1};
        if (!sendDescriptors(socket_, std::span(descriptors).first(second != nullptr ? 2 : 1)))
            return std::nullopt;
        return std::array{inode(first.file_.get()),
                          second != nullptr ? inode(second->file_.get()) : std::uint64_t{0}};
    }

  private:
    std::uint64_t generation_;
    std::optional<LutListener> listener_;
    int socket_ = -1;
#endif
};
} // namespace bloom::color::detail
