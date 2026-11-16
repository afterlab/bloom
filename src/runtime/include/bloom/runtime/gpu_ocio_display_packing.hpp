#pragma once

#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/gpu_display_pack.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <optional>

namespace bloom::runtime {

// #733: how a DisplayPacking program packs.
//   Rgba8                The long-standing pack of the export and output color routes: straight
//                        8:8:8:8 and the error word alone. Its text is unchanged.
//   PreviewStraight8888  The preview pack of a frame that keeps its 8-bit alpha: the same pixels,
//                        and the display pack's status block (render/gpu_display_pack.hpp) with
//                        the layout flag and the frame counters.
//   PreviewRgb10A2       The preview pack of a frame whose every pixel allows 10:10:10:2
//                        (render/display_pixel_packing.hpp), with the same status block. A frame
//                        that does not allow it is refused by the native program.
// One layout holds for a whole frame, so each layout is a program of its own. The luma
// coefficients of the counters are the display's, a constant of the program.
enum class GpuOcioDisplayPackingMode : std::uint8_t {
    Rgba8 = 0,
    PreviewStraight8888 = 1,
    PreviewRgb10A2 = 2,
};

struct GpuOcioDisplayPacking final {
    GpuOcioDisplayPackingMode mode = GpuOcioDisplayPackingMode::Rgba8;
    // Preview packs only. Empty: the luma counters are absent.
    std::optional<render::DisplayLumaCoefficients> luma;

    [[nodiscard]] constexpr bool preview() const noexcept {
        return mode != GpuOcioDisplayPackingMode::Rgba8;
    }
    [[nodiscard]] constexpr render::DisplayPixelLayout layout() const noexcept {
        return mode == GpuOcioDisplayPackingMode::PreviewRgb10A2
                   ? render::DisplayPixelLayout::Rgb10A2
                   : render::DisplayPixelLayout::Straight8888;
    }
    // What the native display job is asked: empty for the long-standing pack.
    [[nodiscard]] constexpr std::optional<render::GpuDisplayPackRequest>
    nativeRequest() const noexcept {
        if (!preview()) {
            return std::nullopt;
        }
        return render::GpuDisplayPackRequest{.layout = layout(), .luma = luma};
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return mode == GpuOcioDisplayPackingMode::Rgba8
                   ? !luma.has_value()
                   : mode <= GpuOcioDisplayPackingMode::PreviewRgb10A2;
    }

    friend constexpr bool operator==(const GpuOcioDisplayPacking&,
                                     const GpuOcioDisplayPacking&) noexcept = default;
};

} // namespace bloom::runtime
