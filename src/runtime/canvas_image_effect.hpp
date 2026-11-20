#pragma once

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::runtime::detail {

// Insets are authored in full-resolution pixels. At a proxy resolution each side is rounded
// independently to the nearest device pixel. Pixel aspect stays unchanged. Pad supplies transparent
// pixels outside the input data window and grows both windows by the margins. Crop is measured
// against the picture it crops, its data window (#739): see canvasOutputWindows.
struct CanvasMargins final {
    CanvasImageEffect effect = CanvasImageEffect::Crop;
    std::array<double, 4> sides{}; // left, top, right, bottom
    [[nodiscard]] bool identity() const noexcept {
        return sides == std::array<double, 4>{};
    }
};

[[nodiscard]] inline std::optional<render::ImageWindow>
canvasOutputWindow(const render::ImageWindow source, const CanvasMargins& margins,
                   const double horizontalScale, const double verticalScale) noexcept {
    if (static_cast<std::uint8_t>(margins.effect) >
        static_cast<std::uint8_t>(CanvasImageEffect::Pad))
        return std::nullopt;
    if (!(horizontalScale > 0.0) || !(verticalScale > 0.0) ||
        !std::isfinite(horizontalScale) || !std::isfinite(verticalScale))
        return std::nullopt;
    std::array<std::int64_t, 4> pixels{};
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const double amount = margins.sides[i];
        const double scaled = amount * (i == 0 || i == 2 ? horizontalScale : verticalScale);
        if (!std::isfinite(amount) || amount < 0.0 || amount > 16777216.0 ||
            !std::isfinite(scaled) || scaled > 16777216.0)
            return std::nullopt;
        pixels[i] = static_cast<std::int64_t>(std::round(scaled));
    }
    const auto left = pixels[0], top = pixels[1], right = pixels[2], bottom = pixels[3];
    const auto width = static_cast<std::int64_t>(source.extent().width());
    const auto height = static_cast<std::int64_t>(source.extent().height());
    if (margins.effect == CanvasImageEffect::Crop) {
        if (left + right >= width || top + bottom >= height)
            return std::nullopt;
        const auto output = render::ImageWindow::create(
            source.originX() + left, source.originY() + top,
            static_cast<std::uint64_t>(width - left - right),
            static_cast<std::uint64_t>(height - top - bottom));
        return output ? std::optional{*output.value()} : std::nullopt;
    }
    if (source.originX() < std::numeric_limits<std::int64_t>::min() + left ||
        source.originY() < std::numeric_limits<std::int64_t>::min() + top)
        return std::nullopt;
    const auto output = render::ImageWindow::create(
        source.originX() - left, source.originY() - top,
        static_cast<std::uint64_t>(width + left + right),
        static_cast<std::uint64_t>(height + top + bottom));
    return output ? std::optional{*output.value()} : std::nullopt;
}

// A canvas effect's output data and display windows.
struct CanvasWindows final {
    render::ImageWindow data;
    render::ImageWindow display;
};

// The output windows of a canvas effect on an input with `data` and `display` windows. Pad grows
// each window by the margins. Crop (#739) cuts the picture along four lines measured from the edges
// of its data window -- the picture itself, an image source's file, not the composition frame its
// display window may be -- and both windows keep what lies between the lines: the data window is
// the cropped picture, the display window the part of the input's display window between the lines
// (the cropped picture itself when none of the display window lies between them). When the two
// windows are the same, both are cropped alike. Nullopt when the margins are invalid or a Crop
// removes its whole picture (canvasCropRefusal names which).
[[nodiscard]] inline std::optional<CanvasWindows>
canvasOutputWindows(const render::ImageWindow data, const render::ImageWindow display,
                    const CanvasMargins& margins, const double horizontalScale,
                    const double verticalScale) noexcept {
    const auto output = canvasOutputWindow(data, margins, horizontalScale, verticalScale);
    if (!output)
        return std::nullopt;
    if (margins.effect == CanvasImageEffect::Pad) {
        const auto grown = canvasOutputWindow(display, margins, horizontalScale, verticalScale);
        return grown ? std::optional{CanvasWindows{*output, *grown}} : std::nullopt;
    }
    const auto left = std::max(display.originX(), output->originX());
    const auto top = std::max(display.originY(), output->originY());
    const auto right = std::min(display.maxXExclusive(), output->maxXExclusive());
    const auto bottom = std::min(display.maxYExclusive(), output->maxYExclusive());
    if (left >= right || top >= bottom)
        return CanvasWindows{*output, *output};
    const auto kept = render::ImageWindow::create(left, top, static_cast<std::uint64_t>(right - left),
                                                  static_cast<std::uint64_t>(bottom - top));
    return kept ? std::optional{CanvasWindows{*output, *kept.value()}} : std::nullopt;
}

// #739: why a Crop is refused, in words that name the margins, or nullopt when it is not refused.
// `side` is the margin (0 left, 1 top, 2 right, 3 bottom) the refusal is filed against: of a pair
// that removes the whole picture, the larger.
struct CanvasCropRefusal final {
    std::size_t side = 0;
    std::string reason;
};

[[nodiscard]] inline std::optional<CanvasCropRefusal>
canvasCropRefusal(const render::ImageWindow data, const CanvasMargins& margins,
                  const double horizontalScale, const double verticalScale) {
    constexpr std::array<std::string_view, 4> names{"left", "top", "right", "bottom"};
    const auto px = [](const double value) {
        auto text = std::to_string(value);
        while (text.find('.') != std::string::npos && (text.back() == '0' || text.back() == '.')) {
            const bool dot = text.back() == '.';
            text.pop_back();
            if (dot)
                break;
        }
        return text + " px";
    };
    for (std::size_t i = 0; i < 4; ++i) {
        const double amount = margins.sides[i];
        if (!std::isfinite(amount) || amount < 0.0 || amount > 16777216.0)
            return CanvasCropRefusal{i, "Crop " + std::string(names[i]) + " is " + px(amount) +
                                            "; a margin is from 0 to 16777216 px"};
    }
    if (margins.effect != CanvasImageEffect::Crop ||
        canvasOutputWindow(data, margins, horizontalScale, verticalScale))
        return std::nullopt;
    for (const std::size_t first : {std::size_t{0}, std::size_t{1}}) {
        const auto second = first + 2;
        const double scale = first == 0 ? horizontalScale : verticalScale;
        const auto extent = first == 0 ? data.extent().width() : data.extent().height();
        const double removed = std::round(margins.sides[first] * scale) +
                               std::round(margins.sides[second] * scale);
        if (removed < static_cast<double>(extent))
            continue;
        const auto side = margins.sides[second] > margins.sides[first] ? second : first;
        return CanvasCropRefusal{
            side, "Crop " + std::string(names[first]) + " " + px(margins.sides[first]) + " and " +
                      std::string(names[second]) + " " + px(margins.sides[second]) +
                      " remove the whole " + std::to_string(extent) + " px " +
                      (first == 0 ? "width" : "height") +
                      " of its picture; together they must be less than its " +
                      (first == 0 ? "width" : "height")};
    }
    return CanvasCropRefusal{0, "Crop margins leave no picture"};
}

} // namespace bloom::runtime::detail
