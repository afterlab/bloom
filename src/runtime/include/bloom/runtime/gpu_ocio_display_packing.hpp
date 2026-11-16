#pragma once

#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/gpu_display_pack.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <optional>

namespace bloom::runtime {

// #733: how a DisplayPacking program packs.
//   Rgba8    The long-standing pack of the export and output color routes: straight 8:8:8:8 and
//            the error word alone. Its text is unchanged.
//   Preview  The preview pack: the display pack's status block (render/gpu_display_pack.hpp) with
//            the layout flag and the frame counters, and the words in the layout the dispatch
//            asks for: straight 8:8:8:8, the same pixels as the long-standing pack, or
//            10:10:10:2 (render/display_pixel_packing.hpp) for a frame whose every pixel allows
//            it. A 10:10:10:2 dispatch of a frame that does not allow it is refused by the native
//            program, and the same program packs that frame again as 8:8:8:8.
// One layout holds for a whole frame and the device learns which only by packing, so the layout is
// a word of the dispatch and not a program of its own: a refused frame is packed again without a
// second shader. The luma coefficients of the counters are the display's, a constant of the
// program.
enum class GpuOcioDisplayPackingMode : std::uint8_t {
    Rgba8 = 0,
    Preview = 1,
};

struct GpuOcioDisplayPacking final {
    GpuOcioDisplayPackingMode mode = GpuOcioDisplayPackingMode::Rgba8;
    // Preview packs only. Empty: the luma counters are absent.
    std::optional<render::DisplayLumaCoefficients> luma;

    [[nodiscard]] constexpr bool preview() const noexcept {
        return mode != GpuOcioDisplayPackingMode::Rgba8;
    }
    // What the native display job is asked for a dispatch in `layout`: empty for the long-standing
    // pack, which has the one layout.
    [[nodiscard]] constexpr std::optional<render::GpuDisplayPackRequest>
    nativeRequest(const render::DisplayPixelLayout layout) const noexcept {
        if (!preview()) {
            return std::nullopt;
        }
        return render::GpuDisplayPackRequest{.layout = layout, .luma = luma};
    }
    // The layouts a dispatch of this program may ask for.
    [[nodiscard]] constexpr bool packs(const render::DisplayPixelLayout layout) const noexcept {
        return preview() || layout == render::DisplayPixelLayout::Straight8888;
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return mode == GpuOcioDisplayPackingMode::Rgba8
                   ? !luma.has_value()
                   : mode == GpuOcioDisplayPackingMode::Preview;
    }

    friend constexpr bool operator==(const GpuOcioDisplayPacking&,
                                     const GpuOcioDisplayPacking&) noexcept = default;
};

} // namespace bloom::runtime
