#include <bloom/host/export_background.hpp>

#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/input_color_context.hpp>

#include <cmath>
#include <utility>

namespace bloom::host {

std::optional<core::Color4d> exportBackgroundColor(const document::Composition& composition,
                                                   const runtime::EvaluationColorIntent& intent) {
    const auto resolved = runtime::detail::resolveInputColorConfig(intent);
    if (!resolved.has_value())
        return std::nullopt;
    auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    auto handle = std::move(built).takeHandle();
    if (!handle.has_value())
        return std::nullopt;
    auto display = composition.backgroundColor();
    // The Viewer draws the Background Color opaque, whatever alpha it was authored with.
    display.alpha = 1.0;
    const auto working = handle->displayToReference(display);
    if (!working || !std::isfinite(working->red) || !std::isfinite(working->green) ||
        !std::isfinite(working->blue))
        return std::nullopt;
    auto color = *working;
    color.alpha = 1.0;
    return color;
}

} // namespace bloom::host
