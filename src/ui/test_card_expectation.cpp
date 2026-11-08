#include "test_card_expectation.hpp"

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>

#include <algorithm>
#include <cmath>
#include <span>

namespace bloom::ui {

std::optional<PatchExpectation>
patchExpectation(const color::ResolvedBloomNeutralConfig& config, const std::string_view display,
                 const std::string_view view, const std::string_view signalSpace,
                 const std::array<double, 3>& signal) {
    PatchExpectation result;
    result.signal = signal;
    result.working = signal;
    if (!signalSpace.empty()) {
        const auto prepared = color::CpuColorSpaceProcessor::prepare(
            config, signalSpace, config.processColorSpaceId());
        if (!prepared)
            return std::nullopt;
        std::array<std::array<float, 4>, 1> pixel{
            {{static_cast<float>(signal[0]), static_cast<float>(signal[1]),
              static_cast<float>(signal[2]), 1.0F}}};
        if (!prepared.processor()->apply(pixel))
            return std::nullopt;
        for (std::size_t i = 0; i < 3; ++i)
            result.working[i] = static_cast<double>(pixel[0][i]);
    }
    auto handle = color::buildCpuDisplayProcessorForView(config, display, view).takeHandle();
    if (!handle)
        return std::nullopt;
    const auto shown = handle->referenceToDisplay(
        core::Color4d{result.working[0], result.working[1], result.working[2], 1.0});
    if (!shown)
        return std::nullopt;
    result.display = {shown->red, shown->green, shown->blue};
    for (std::size_t i = 0; i < 3; ++i)
        result.code[i] = static_cast<int>(
            std::lround(std::clamp(result.display[i], 0.0, 1.0) * 255.0));
    return result;
}

} // namespace bloom::ui
