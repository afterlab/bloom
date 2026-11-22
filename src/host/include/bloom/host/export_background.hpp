#pragma once
// #760: an export output's background (docs/architecture/frame-output.md, "Background").

#include <bloom/core/color.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <optional>
#include <string_view>

namespace bloom::host {

// The composition's Background Color, which the Viewer draws as display-encoded RGB, as the
// straight working-space color of `intent` with alpha 1: the configuration's default display and
// view inverted, exactly as a color chip authors a Solid's color from the same swatch, so the
// background equals a full-frame solid layer of that color under the composition. Builds the CPU
// display processor (an external configuration goes through the supervised color helper): call it
// off the UI thread. Nothing when the configuration or the conversion is unavailable; the export
// then fails with that reason instead of writing another color.
inline constexpr std::string_view kExportBackgroundUnavailable =
    "The composition's Background Color could not be converted to the working space for the "
    "export background";
[[nodiscard]] std::optional<core::Color4d>
exportBackgroundColor(const document::Composition& composition,
                      const runtime::EvaluationColorIntent& intent);

} // namespace bloom::host
