#pragma once

// Standard streams that carry the bytes a program writes.
//
// The C runtime on Windows opens stdin, stdout and stderr in text mode: every "\n" written becomes
// "\r\n" and a 0x1A byte read ends the input. Programs whose output is compared byte for byte
// across platforms (help and reference text, JSON-RPC over pipes) call this first thing in main.
// Other platforms have no text mode, so the call does nothing there.

#if defined(_WIN32)
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

namespace bloom::core {

inline void useBinaryStandardStreams() noexcept {
#if defined(_WIN32)
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
    static_cast<void>(_setmode(_fileno(stdout), _O_BINARY));
    static_cast<void>(_setmode(_fileno(stderr), _O_BINARY));
#endif
}

} // namespace bloom::core
