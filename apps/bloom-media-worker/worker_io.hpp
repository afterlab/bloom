#pragma once
#include <array>
#include <bloom/media/provider/protocol.hpp>
#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace bloom::media::worker {
#if defined(__linux__) || defined(__APPLE__)
// The worker's input is read from the descriptor itself, exactly as many bytes as the message
// has, into the message's own storage. std::cin is not used for it: a standard library may read
// a synchronized std::cin one character at a time through the C stream, taking its lock for
// each (libc++ does), and a frame is tens of megabytes (issue #672). Nothing is buffered here,
// so the descriptor poll below sees every byte that has not been read.
inline bool bufferedInput() { return false; }
inline bool readInput(std::byte* data, std::size_t size) {
    while (size != 0) {
        const auto count = ::read(STDIN_FILENO, data, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        data += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}
#else
// Bytes already in std::cin's buffer are invisible to the pipe check below, so a request the
// previous read pulled in ahead of its own message is never waited for.
inline bool bufferedInput() { return std::cin.rdbuf()->in_avail() > 0; }
inline bool readInput(std::byte* data, std::size_t size) {
    return static_cast<bool>(
        std::cin.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size)));
}
#endif
inline provider::Result<provider::Message>
read(std::chrono::milliseconds idleTimeout = std::chrono::milliseconds{0}) {
#if defined(__linux__) || defined(__APPLE__)
    if (idleTimeout.count() > 0 && !bufferedInput()) {
        const auto deadline = std::chrono::steady_clock::now() + idleTimeout;
        pollfd input{STDIN_FILENO, POLLIN, 0};
        while (true) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return provider::Unavailable{provider::Error::Timeout,
                                             "Worker idle lifetime expired"};
            const int status = ::poll(&input, 1, static_cast<int>(remaining.count()));
            if (status > 0)
                break;
            if (status == 0)
                return provider::Unavailable{provider::Error::Timeout,
                                             "Worker idle lifetime expired"};
            if (errno != EINTR)
                return provider::Unavailable{provider::Error::Io, "Worker input poll failed"};
        }
    }
#elif defined(_WIN32)
    // Anonymous/named pipe stdin: wait for readable bytes or a closed pipe in short steps.
    if (idleTimeout.count() > 0 && !bufferedInput()) {
        const auto deadline = std::chrono::steady_clock::now() + idleTimeout;
        auto* input = GetStdHandle(STD_INPUT_HANDLE);
        while (true) {
            DWORD available = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) || available > 0)
                break; // data, or a closed pipe the read below reports
            if (std::chrono::steady_clock::now() >= deadline)
                return provider::Unavailable{provider::Error::Timeout,
                                             "Worker idle lifetime expired"};
            Sleep(5);
        }
    }
#else
    (void)idleTimeout;
#endif
    std::array<std::byte, 4> prefix{};
    if (!readInput(prefix.data(), prefix.size()))
        return provider::Unavailable{provider::Error::Truncated, "Worker input closed"};
    const auto length = provider::messageLength(prefix);
    if (const auto* e = std::get_if<provider::Unavailable>(&length))
        return *e;
    provider::Bytes bytes(prefix.begin(), prefix.end());
    bytes.resize(4U + std::get<std::uint32_t>(length));
    if (!readInput(bytes.data() + 4, bytes.size() - 4))
        return provider::Unavailable{provider::Error::Truncated, "Worker input truncated"};
    return provider::decodeMessage(bytes);
}
inline bool write(const provider::Message& message);
// #718: the worker's side of a progressive source. A read of the snapshot comes here first
// (platform::WorkerSnapshotInput's range gate): a range the host has said is local is read at
// once; any other is asked for with NeedRange and read when RangeReady arrives. Outside a call
// nobody is listening, so an unknown range is refused there.
class SourceRanges final {
  public:
    // A call begins: `progressive` is the request's flag for the snapshot this process holds.
    void beginCall(const std::uint64_t session, const std::uint64_t sequence,
                   const bool progressive) {
        session_ = session;
        sequence_ = sequence;
        progressive_ = progressive;
        inCall_ = true;
    }
    void endCall() { inCall_ = false; }
    // True when every range is local (or the source is an ordinary snapshot or a path).
    [[nodiscard]] bool whole() const { return !progressive_ || complete_; }
    [[nodiscard]] bool ready(const std::uint64_t offset, const std::uint64_t length) {
        if (whole() || covered(offset, length))
            return true;
        if (!inCall_)
            return false;
        // Ask for a little more than the read: demuxers read forward in small steps.
        constexpr std::uint64_t kAskBytes = 4ULL * 1024 * 1024;
        if (!write({provider::MessageKind::NeedRange, session_, sequence_,
                    provider::SourceRangeV1{offset, std::max(length, kAskBytes), false}}))
            return false;
        auto incoming = read();
        const auto* message = std::get_if<provider::Message>(&incoming);
        if (message == nullptr || message->session != session_)
            return false;
        if (message->kind == provider::MessageKind::Cancel ||
            message->kind == provider::MessageKind::Shutdown) {
            // The host gave the call up while the range was awaited: acknowledge and end, as the
            // worker's main loop does for the same message.
            static_cast<void>(write({provider::MessageKind::Ack, message->session,
                                     message->sequence, std::monostate{}}));
            std::_Exit(0);
        }
        const auto* range = std::get_if<provider::SourceRangeV1>(&message->payload);
        if (message->kind != provider::MessageKind::RangeReady || message->sequence != sequence_ ||
            range == nullptr)
            return false;
        complete_ = range->complete;
        add(range->offset, range->offset + range->length);
        return whole() || covered(offset, length);
    }

  private:
    [[nodiscard]] bool covered(const std::uint64_t offset, const std::uint64_t length) const {
        for (const auto& [begin, end] : ready_)
            if (offset >= begin && offset + length <= end)
                return true;
        return false;
    }
    void add(std::uint64_t begin, std::uint64_t end) {
        // Merge with every range it touches; the list stays short (ranges grow into each other).
        for (auto it = ready_.begin(); it != ready_.end();)
            if (it->first <= end && begin <= it->second) {
                begin = std::min(begin, it->first);
                end = std::max(end, it->second);
                it = ready_.erase(it);
            } else {
                ++it;
            }
        ready_.emplace_back(begin, end);
    }
    std::uint64_t session_ = 0, sequence_ = 0;
    bool progressive_ = false, complete_ = false, inCall_ = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ready_;
};
inline bool write(const provider::Message& message) {
    const auto encoded = provider::encodeMessage(message);
    const auto* bytes = std::get_if<provider::Bytes>(&encoded);
    if (!bytes)
        return false;
    std::cout.write(reinterpret_cast<const char*>(bytes->data()),
                    static_cast<std::streamsize>(bytes->size()));
    std::cout.flush();
    return static_cast<bool>(std::cout);
}
} // namespace bloom::media::worker
