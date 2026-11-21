#pragma once

#include <bloom/runtime/font_resolver.hpp>
#include <bloom/runtime/font_transport.hpp>

#include <chrono>
#include <memory>

namespace bloom::runtime::fonts {

// The network transport of the command-line tools, the MCP server and Python (docs/architecture/
// fonts.md): a blocking, cancellable HTTPS GET with a size cap and a stall timeout, run on the font
// worker thread like the desktop application's. It speaks through the system's own HTTP client
// library, loaded when first needed, so no network library is linked into these programs. Returns
// null where that client is not available (every platform but Linux today, and a Linux machine
// without it); the resolver then reports the font as not downloadable here and draws the fallback.
[[nodiscard]] std::shared_ptr<FontTransport> makeHeadlessFontTransport();

// Gives the process font resolver the headless transport when it has none. Call once at start-up
// from a program that has no Qt application; a no-op when the transport is unavailable. The program
// ends the downloads with endHeadlessFontDownloads() before it ends; a program with a main() holds
// a HeadlessFontDownloads instead, which does both.
void installHeadlessFontTransport();

// Ends the process font resolver's downloads (FontResolver::stopDownloads) and says on the standard
// error when a transfer did not stop within the deadline; it is then left to end with the process.
// False in that case.
bool endHeadlessFontDownloads(std::chrono::milliseconds deadline = kEndFontDownloadsDeadline);

// The headless transport for the life of a program's main(): installs it on a resolver (the process
// resolver unless another is given) and, when it ends, ends that resolver's downloads. No transfer
// is then running when main() returns and the process's exit handlers tear down the libraries the
// transport uses (OpenSSL's, under libcurl), which a transfer still running would race (#750).
class HeadlessFontDownloads final {
  public:
    HeadlessFontDownloads();
    // For tests: this resolver and transport (either may be one the test made).
    HeadlessFontDownloads(FontResolver& resolver, std::shared_ptr<FontTransport> transport,
                          std::chrono::milliseconds deadline = kEndFontDownloadsDeadline);
    ~HeadlessFontDownloads();
    HeadlessFontDownloads(const HeadlessFontDownloads&) = delete;
    HeadlessFontDownloads& operator=(const HeadlessFontDownloads&) = delete;
    HeadlessFontDownloads(HeadlessFontDownloads&&) = delete;
    HeadlessFontDownloads& operator=(HeadlessFontDownloads&&) = delete;

  private:
    FontResolver& resolver_;
    std::chrono::milliseconds deadline_;
};

} // namespace bloom::runtime::fonts
