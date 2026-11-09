#pragma once

#include <cstdint>

namespace bloom::runtime::fonts {

// What to do when a Google font is not installed and not yet in the per-user cache.
enum class FontFetch : std::uint8_t {
    // Never touch the network.
    CachedOnly,
    // Start the download on the font worker and answer with the fallback now (the Viewer; the
    // picture is rebuilt when the font arrives).
    Background,
    // Start the download and wait for it, honoring cancellation (export and headless render).
    Wait,
};

} // namespace bloom::runtime::fonts
