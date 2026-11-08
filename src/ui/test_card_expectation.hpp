#pragma once

// #587 Test Card: the display value a Patch should produce. A patch's exact signal in its color space
// goes through the same two transforms any source's pixels do: the color space transform into the
// working space, then the Viewer's Display and View (with its tone map where it has one). This is
// that arithmetic on one color, for the readout beside the patch; the pipeline's own pixels are
// compared with it by the tests. Not a display calibration: a probe may differ from it through the
// desktop compositor, the GPU output settings or the monitor's profile.

#include <bloom/color/ocio_builtin_registry.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::ui {

struct PatchExpectation final {
    // The signal as authored, the working-space value it becomes (a straight color) and the
    // display value the Display and View produce, clamped to the display's 0 to 1.
    std::array<double, 3> signal{};
    std::array<double, 3> working{};
    std::array<double, 3> display{};
    // The display value as 8-bit codes.
    std::array<int, 3> code{};
};

// `signalSpace` empty is the working space itself. Absent when a transform cannot be built.
[[nodiscard]] std::optional<PatchExpectation>
patchExpectation(const color::ResolvedBloomNeutralConfig& config, std::string_view display,
                 std::string_view view, std::string_view signalSpace,
                 const std::array<double, 3>& signal);

} // namespace bloom::ui
