#pragma once

// #484 Motion blur: the node definitions of the motion-blur nodes, Transform 2D's motion blur
// parameters and their value checks. The parameter vocabulary is motion_blur_parameters.hpp.

#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string motionSchemaKey(std::string_view typeId, std::string_view role);
// Whether `value` lies in the document domain of the motion parameter `schemaKey`.
[[nodiscard]] bool isMotionParameterValueValid(std::string_view schemaKey,
                                               const ParameterValue& value) noexcept;
// The registered definition of a motion-blur node (before the effect mask is appended).
[[nodiscard]] NodeDefinition motionBlurDefinition(std::string_view typeId);
// Transform 2D's optional motion blur parameters, appended to its six (not required: a node saved
// before #484 has none and renders without motion blur; opening it binds them at their defaults).
[[nodiscard]] std::vector<ParameterDefinition> transformMotionBlurParameterDefinitions();
// The custom shutter curve's default: flat, every moment of the open time weighted the same.
[[nodiscard]] PathValue defaultShutterCurve();
// The shutter angle (degrees) as the open time in frames.
[[nodiscard]] constexpr double shutterFrames(const double degrees) noexcept {
    return degrees / 360.0;
}

// --- Vector conventions (docs/architecture/motion-blur.md, "Vector conventions") ---------------

// How a convention reads its vector pass: the decode the Custom controls spell out.
struct VectorConvention final {
    double scale = 1.0;
    double offset = 0.0;
    bool flipY = false;
    std::int64_t layout = kVectorLayoutSingle;
    std::int64_t units = kVectorUnitsPixels;
    bool alphaValid = false;
    // The channel names or layer the convention writes, for the picker's first guess.
    std::string_view channels{};
};
[[nodiscard]] VectorConvention vectorConvention(std::int64_t convention) noexcept;

} // namespace bloom::document
