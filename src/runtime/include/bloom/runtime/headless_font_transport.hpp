#pragma once

#include <bloom/runtime/font_transport.hpp>

#include <memory>

namespace bloom::runtime::fonts {

// The network transport of the command-line tools, the MCP server and Python (docs/architecture/
// fonts.md): a blocking, cancellable HTTPS GET with a size cap and a stall timeout, run on the font
// worker thread like the desktop application's. It speaks through the system's own HTTP client
// library, loaded when first needed, so no network library is linked into these programs. Returns
// null where that client is not available (every platform but Linux today, and a Linux machine
// without it); the resolver then reports the font as not downloadable here and draws the fallback.
[[nodiscard]] std::shared_ptr<FontTransport> makeHeadlessFontTransport();

// Gives the process font resolver the headless transport when it has none. Call once at start-up from
// a program that has no Qt application; a no-op when the transport is unavailable.
void installHeadlessFontTransport();

} // namespace bloom::runtime::fonts
