// #596 Axes and Axis Tick: the frame of a plot, as a Path, and the number, position and text of one
// mark on it. Pure functions of their operands; nothing here reads the time.
//
// Placement is in pixels with Y up on the plot, so a value's Y is subtracted from the origin (the
// path's Y grows downward). Tick `i` of an axis is at `min + i * step` while that value does not
// pass `max` (document::axisTickCount); Axes and Axis Tick share the rule, so labels set against
// the same range land on the marks Axes draws.

#include "value_utility_support.hpp"

#include <bloom/document/parameter.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using namespace bloom;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityOutcome;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;
using Kernel = document::ValueUtilityKernel;

[[nodiscard]] document::PathContour segment(const document::Vec2d from, const document::Vec2d to) {
    document::PathContour contour;
    contour.closed = false;
    contour.anchors.push_back({from, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
    contour.anchors.push_back({to, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
    return contour;
}

[[nodiscard]] bool finite(const document::Vec2d value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

// Fixed notation with `decimals` places, and never "-0": a value that rounds to zero is "0".
[[nodiscard]] std::string formatTick(const double value, const int decimals) {
    std::array<char, 400> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, value);
    if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size())
        return {};
    std::string text(buffer.data(), static_cast<std::size_t>(length));
    if (text.starts_with('-') &&
        std::ranges::all_of(text.substr(1), [](const char c) { return c == '0' || c == '.'; }))
        text.erase(0, 1);
    return text;
}

ValueUtilityOutcome axes(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const double xMin = reader.scalar(0);
    const double xMax = reader.scalar(1);
    const double yMin = reader.scalar(2);
    const double yMax = reader.scalar(3);
    const double xStep = reader.scalar(4);
    const double yStep = reader.scalar(5);
    const double tickLength = reader.scalar(6);
    const auto origin = reader.vector2(7);
    const auto scale = reader.vector2(8);
    if (reader.malformed())
        return outcome;
    if (!std::isfinite(xMin) || !std::isfinite(xMax) || !std::isfinite(yMin) ||
        !std::isfinite(yMax) || !std::isfinite(tickLength) || !finite(origin) || !finite(scale)) {
        outcome.failed = true;
        outcome.summary = "Axes range is not finite";
        outcome.detail = "The ranges, the tick length, the origin and the scale must be finite.";
        return outcome;
    }
    const auto xTicks = document::axisTickCount(xMin, xMax, xStep);
    const auto yTicks = document::axisTickCount(yMin, yMax, yStep);
    if (xTicks > document::kMaximumAxisTicks || yTicks > document::kMaximumAxisTicks) {
        outcome.failed = true;
        outcome.summary = "Axes holds too many ticks";
        outcome.detail = "An axis holds at most 1024 ticks; raise the step or narrow the range.";
        return outcome;
    }
    const auto point = [&](const double x, const double y) {
        return document::Vec2d{origin.x + x * scale.x, origin.y - y * scale.y};
    };
    const double half = tickLength * 0.5;
    document::PathValue value;
    // The two axes cross at zero where zero is in range, and at the range's nearest edge where it
    // is not, so the frame is always inside the plotted area.
    const double crossX = std::clamp(0.0, std::min(xMin, xMax), std::max(xMin, xMax));
    const double crossY = std::clamp(0.0, std::min(yMin, yMax), std::max(yMin, yMax));
    value.contours.push_back(segment(point(xMin, crossY), point(xMax, crossY)));
    value.contours.push_back(segment(point(crossX, yMin), point(crossX, yMax)));
    for (std::int64_t index = 0; index < xTicks; ++index) {
        const double x = xMin + static_cast<double>(index) * xStep;
        const auto at = point(x, crossY);
        value.contours.push_back(segment({at.x, at.y - half}, {at.x, at.y + half}));
    }
    for (std::int64_t index = 0; index < yTicks; ++index) {
        const double y = yMin + static_cast<double>(index) * yStep;
        const auto at = point(crossX, y);
        value.contours.push_back(segment({at.x - half, at.y}, {at.x + half, at.y}));
    }
    outcome.outputs[0] =
        runtime::CompiledPathValue{std::make_shared<document::PathValue>(std::move(value))};
    outcome.outputs[1] = xTicks;
    outcome.outputs[2] = yTicks;
    return outcome;
}

ValueUtilityOutcome axisTick(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const double minimum = reader.scalar(0);
    const double maximum = reader.scalar(1);
    const double step = reader.scalar(2);
    const std::int64_t index = reader.integer(3);
    const bool yAxis = reader.boolean(4);
    const auto origin = reader.vector2(5);
    const auto scale = reader.vector2(6);
    const auto offset = reader.vector2(7);
    const int decimals = static_cast<int>(
        std::clamp<std::int64_t>(reader.integer(8), 0, document::kMaximumAxisTickDecimals));
    if (reader.malformed())
        return outcome;
    const auto count = document::axisTickCount(minimum, maximum, step);
    const double value = minimum + static_cast<double>(index) * step;
    const document::Vec2d position =
        yAxis ? document::Vec2d{origin.x + offset.x, origin.y - value * scale.y + offset.y}
              : document::Vec2d{origin.x + value * scale.x + offset.x, origin.y + offset.y};
    // A tick outside the range, or on a range that has none, is not valid: its label is empty so a
    // text layer reading it shows nothing, and the numbers are the ones its index names.
    const bool valid = index >= 0 && index < count && std::isfinite(value) && finite(position);
    outcome.outputs[0] = std::isfinite(value) ? value : 0.0;
    outcome.outputs[1] = finite(position) ? position : document::Vec2d{};
    outcome.outputs[2] = valid ? formatTick(value, decimals) : std::string{};
    outcome.outputs[3] = valid;
    return outcome;
}

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValuePlot(const ValueUtilityInvocation& invocation) {
    switch (invocation.operation) {
    case Kernel::Axes:
        return axes(invocation);
    case Kernel::AxisTick:
        return axisTick(invocation);
    default:
        break;
    }
    return shapedOutcome(invocation.operation);
}

} // namespace bloom::runtime::detail
