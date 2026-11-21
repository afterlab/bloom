// #591 coordinate spaces (docs/architecture/coordinate-spaces.md): Coordinate Space defines a space,
// Space Readout reads its map as values, To Space and From Space convert points and vectors between
// it and composition pixels, and Place in Space answers a layer's Position, Scale and Rotation for a
// point of it. Pure functions of their operands; nothing here reads the time, so a space moves only
// when what drives it moves.

#include "value_utility_support.hpp"

#include <bloom/core/coordinate_space.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

using namespace bloom;
namespace space = core::space;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityOutcome;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;
using Kernel = document::ValueUtilityKernel;

[[nodiscard]] bool usable(const double value) noexcept {
    return std::isfinite(value) && std::abs(value) <= space::kMaximumMagnitude;
}
[[nodiscard]] bool usable(const document::Vec2d value) noexcept {
    return usable(value.x) && usable(value.y);
}
[[nodiscard]] space::Point2 point(const document::Vec2d value) noexcept {
    return {value.x, value.y};
}
[[nodiscard]] document::Vec2d vec(const space::Point2 value) noexcept {
    return {value.x, value.y};
}

void refuse(ValueUtilityOutcome& outcome, const char* summary, const char* detail) {
    outcome.failed = true;
    outcome.summary = summary;
    outcome.detail = detail;
}

ValueUtilityOutcome coordinateSpace(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto parent = reader.space(0);
    const auto name = reader.text(1);
    const auto origin = reader.vector2(2);
    const auto pixelsPerUnit = reader.vector2(3);
    const double rotation = reader.scalar(4);
    const bool yUp = reader.boolean(5);
    const auto rangeMin = reader.vector2(6);
    const auto rangeMax = reader.vector2(7);
    const auto center = reader.vector2(8);
    const auto size = reader.vector2(9);
    const std::array<document::Vec2d, 4> corners{reader.vector2(10), reader.vector2(11),
                                                 reader.vector2(12), reader.vector2(13)};
    if (reader.malformed()) {
        runtime::detail::noteMalformedOperand(outcome, reader);
        return outcome;
    }
    const auto mode = runtime::detail::selectorAt(invocation, 0, document::kSpaceModes,
                                                  document::SpaceMode::Scale);
    if (name.size() > document::kMaximumSpaceNameBytes) {
        refuse(outcome, "Coordinate Space name is too long",
               "A space's name holds at most 256 bytes.");
        return outcome;
    }
    std::size_t depth = 0;
    for (const auto* above = parent.get(); above != nullptr; above = above->parent.get())
        ++depth;
    if (depth >= document::kMaximumSpaceDepth) {
        refuse(outcome, "Coordinate Space is nested too deeply",
               "A space may have at most 15 spaces above it.");
        return outcome;
    }
    if (parent != nullptr && parent->dimensions != 2) {
        refuse(outcome, "Coordinate Space parent is not a 2D space",
               "A Coordinate Space is defined in a 2D space or in composition pixels.");
        return outcome;
    }
    // The range is published whenever it runs forward on both axes; Range and Corners need one.
    const bool rangeValid = usable(rangeMin) && usable(rangeMax) && rangeMax.x > rangeMin.x &&
                            rangeMax.y > rangeMin.y;
    const space::Range2 range{point(rangeMin), point(rangeMax)};
    if (!rangeValid && mode != document::SpaceMode::Scale) {
        refuse(outcome, "Coordinate Space range is empty",
               "The range's maximum must be above its minimum on both axes, and finite.");
        return outcome;
    }
    std::optional<space::Matrix3> local;
    switch (mode) {
    case document::SpaceMode::Scale:
        if (usable(origin) && usable(pixelsPerUnit) && usable(rotation))
            local = space::placement(point(origin), {0.0, 0.0}, point(pixelsPerUnit), rotation,
                                     yUp);
        break;
    case document::SpaceMode::Range:
        if (usable(center) && usable(size) && usable(rotation)) {
            const space::Point2 scale{size.x / (rangeMax.x - rangeMin.x),
                                      size.y / (rangeMax.y - rangeMin.y)};
            const space::Point2 middle{(rangeMin.x + rangeMax.x) * 0.5,
                                       (rangeMin.y + rangeMax.y) * 0.5};
            local = space::placement(point(center), middle, scale, rotation, yUp);
        }
        break;
    case document::SpaceMode::Corners: {
        bool cornersUsable = true;
        for (const auto& corner : corners)
            cornersUsable = cornersUsable && usable(corner);
        if (cornersUsable)
            local = space::rangeToQuad(range, {point(corners[0]), point(corners[1]),
                                               point(corners[2]), point(corners[3])});
        if (!local) {
            refuse(outcome, "Coordinate Space corners do not make a plane",
                   "The four corners must be finite and no three of them on one line.");
            return outcome;
        }
        break;
    }
    }
    if (!local) {
        refuse(outcome, "Coordinate Space is not finite",
               "The origin, the size, the rectangle and the rotation must be finite numbers.");
        return outcome;
    }
    if (!space::inverse(*local)) {
        refuse(outcome, "Coordinate Space is flat",
               "A space needs a non-zero size on both axes, so a pixel can be read back in it.");
        return outcome;
    }
    const auto embedded = space::embed2d(*local);
    auto defined = std::make_shared<space::Space>();
    defined->name = std::string(name);
    defined->dimensions = 2;
    defined->toPixels = parent == nullptr ? embedded : space::multiply(parent->toPixels, embedded);
    if (rangeValid)
        defined->range = range;
    defined->parent = parent;
    if (!space::isFinite(defined->toPixels) || !space::inverse(space::planar(defined->toPixels))) {
        refuse(outcome, "Coordinate Space is not finite",
               "The space and its parent together give a map too large or flat to use.");
        return outcome;
    }
    outcome.outputs[0] = runtime::CompiledSpaceValue{std::move(defined)};
    return outcome;
}

ValueUtilityOutcome spaceReadout(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto held = reader.space(0);
    if (reader.malformed()) {
        runtime::detail::noteMalformedOperand(outcome, reader);
        return outcome;
    }
    if (held == nullptr)
        return outcome; // Nothing connected: every readout is zero and not valid.
    const auto read = space::readout(held->toPixels);
    if (read) {
        outcome.outputs[0] = vec(read->origin);
        outcome.outputs[1] = vec(read->xAxis);
        outcome.outputs[2] = vec(read->yAxis);
        outcome.outputs[3] = vec(read->scale);
        outcome.outputs[4] = read->rotationDegrees;
    }
    if (held->range) {
        outcome.outputs[5] = vec(held->range->minimum);
        outcome.outputs[6] = vec(held->range->maximum);
    }
    std::vector<double> matrix;
    if (held->dimensions == 2) {
        const auto planar = space::planar(held->toPixels);
        matrix.assign(planar.begin(), planar.end());
    } else {
        matrix.assign(held->toPixels.begin(), held->toPixels.end());
    }
    outcome.outputs[7] = runtime::CompiledNumberList{std::move(matrix)};
    outcome.outputs[8] = held->name;
    outcome.outputs[9] = held->parent == nullptr ? std::string{} : held->parent->name;
    outcome.outputs[10] = !space::isAffine(held->toPixels);
    // A projective space's origin can be on its horizon: the matrix and names still read.
    outcome.outputs[11] = read.has_value();
    return outcome;
}

// The derivative at the space's origin, as a 2x2 (columns: one unit of x, one unit of y).
[[nodiscard]] std::optional<space::Jacobian2> originAxes(const space::Space& held) noexcept {
    return space::jacobian(held.toPixels, {0.0, 0.0});
}

ValueUtilityOutcome convert(const ValueUtilityInvocation& invocation, const bool toSpace) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto held = reader.space(0);
    const auto value = reader.vector2(1);
    if (reader.malformed()) {
        runtime::detail::noteMalformedOperand(outcome, reader);
        return outcome;
    }
    const auto conversion = runtime::detail::selectorAt(
        invocation, 0, document::kSpaceConversions, document::SpaceConversion::Point);
    if (held == nullptr || !usable(value))
        return outcome; // Not valid: (0, 0).
    std::optional<space::Point2> result;
    if (conversion == document::SpaceConversion::Point) {
        result = toSpace ? space::unmapPoint(held->toPixels, point(value))
                         : space::mapPoint(held->toPixels, point(value));
    } else if (const auto axes = originAxes(*held)) {
        const double a = axes->xAxis.x;
        const double b = axes->yAxis.x;
        const double c = axes->xAxis.y;
        const double d = axes->yAxis.y;
        if (!toSpace) {
            result = space::Point2{a * value.x + b * value.y, c * value.x + d * value.y};
        } else {
            const double determinant = a * d - b * c;
            if (std::isfinite(determinant) && determinant != 0.0)
                result = space::Point2{(d * value.x - b * value.y) / determinant,
                                       (a * value.y - c * value.x) / determinant};
        }
    }
    if (!result || !std::isfinite(result->x) || !std::isfinite(result->y))
        return outcome;
    outcome.outputs[0] = vec(*result);
    outcome.outputs[1] = true;
    return outcome;
}

ValueUtilityOutcome placeInSpace(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto held = reader.space(0);
    const auto position = reader.vector2(1); // The point, in the space's units.
    const auto offset = reader.vector2(2);
    const auto unitSize = reader.vector2(3);
    if (reader.malformed()) {
        runtime::detail::noteMalformedOperand(outcome, reader);
        return outcome;
    }
    // Not valid: the offset alone, at 100% and unturned, so a layer reading it stays where its
    // offset puts it rather than vanishing.
    outcome.outputs[0] = usable(offset) ? offset : document::Vec2d{};
    outcome.outputs[1] = document::Vec2d{1.0, 1.0};
    if (held == nullptr || !usable(position) || !usable(offset) || !usable(unitSize) ||
        !(unitSize.x > 0.0) || !(unitSize.y > 0.0))
        return outcome;
    const auto pixel = space::mapPoint(held->toPixels, point(position));
    const auto axes = space::jacobian(held->toPixels, point(position));
    if (!pixel || !axes)
        return outcome;
    const document::Vec2d placed{pixel->x + offset.x, pixel->y + offset.y};
    const document::Vec2d scale{std::hypot(axes->xAxis.x, axes->xAxis.y) / unitSize.x,
                                std::hypot(axes->yAxis.x, axes->yAxis.y) / unitSize.y};
    const double rotation =
        std::atan2(axes->xAxis.y, axes->xAxis.x) * 180.0 / std::numbers::pi;
    if (!usable(placed) || !usable(scale) || !std::isfinite(rotation))
        return outcome;
    outcome.outputs[0] = placed;
    outcome.outputs[1] = scale;
    outcome.outputs[2] = rotation;
    outcome.outputs[3] = true;
    return outcome;
}

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValueSpace(const ValueUtilityInvocation& invocation) {
    switch (invocation.operation) {
    case Kernel::CoordinateSpace:
        return coordinateSpace(invocation);
    case Kernel::SpaceReadout:
        return spaceReadout(invocation);
    case Kernel::ToSpace:
        return convert(invocation, true);
    case Kernel::FromSpace:
        return convert(invocation, false);
    case Kernel::PlaceInSpace:
        return placeInSpace(invocation);
    default:
        break;
    }
    return shapedOutcome(invocation.operation);
}

} // namespace bloom::runtime::detail
