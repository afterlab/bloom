#pragma once
#include <array>
#include <bloom/media/provider/protocol.hpp>
#include <chrono>
#include <cstddef>
#include <iostream>
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
