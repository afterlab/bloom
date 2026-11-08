#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace bloom::output {

// A region of a frame in pixels from the top-left corner.
struct FrameRegion final {
    std::uint64_t x = 0;
    std::uint64_t y = 0;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
};

struct FrameReadback final {
    bool succeeded = false;
    std::string diagnostic;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
};

// Reads an exported OpenEXR frame as stored (no colour conversion) and passes the premultiplied
// red, green, blue and alpha of every pixel in `region` to `visit`, row by row. A region outside
// the image, or a file that cannot be read, fails with a diagnostic and visits nothing.
[[nodiscard]] FrameReadback
visitExportedFrame(const std::filesystem::path& path, const FrameRegion& region,
                   const std::function<void(float, float, float, float)>& visit);

} // namespace bloom::output
