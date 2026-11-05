#pragma once

// #483 Depth effects: Z Defocus and Depth Fog's node definitions, the depth reading and the depth
// input's checks. The parameter vocabulary is depth_effect_parameters.hpp.

#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string depthSchemaKey(std::string_view typeId, std::string_view role);
// Whether `value` lies in the document domain of the depth parameter `schemaKey`.
[[nodiscard]] bool isDepthParameterValueValid(std::string_view schemaKey,
                                              const ParameterValue& value) noexcept;
// The registered definition of Z Defocus or Depth Fog (before the effect mask is appended).
[[nodiscard]] NodeDefinition depthEffectDefinition(std::string_view typeId);
// The fog falloff curve's default: the identity (0, 0) (1, 1).
[[nodiscard]] PathValue defaultDepthFogCurve();

// --- The depth reading, in double precision (the Viewer focus picker and the card's analysis) ---

struct DepthReading final {
    std::int64_t interpretation = kDepthInterpretationZ;
    double nearDistance = 0.0;
    double farDistance = 1.0;
    bool invert = false;
    std::int64_t mistFalloff = kMistFalloffLinear;
    std::int64_t background = kDepthBackgroundInfinite;
};
struct DepthDistance final {
    // The distance in scene units; meaningless when `empty` or `infinite`.
    double distance = 0.0;
    bool empty = false;
    bool infinite = false;
};
// The distance a stored (straight) depth value means, before the background rule. Integer files
// arrive here as their normalised value.
[[nodiscard]] DepthDistance readDepth(const DepthReading& reading, double value) noexcept;
// Scene units per metre of a `units` choice.
[[nodiscard]] double sceneUnitsPerMetre(std::int64_t units) noexcept;

// --- The depth input's checks (the card's warnings) ----------------------------------------------

// The Image or Video source that gives the depth input a colour transform: its picture read through
// a colour space other than Raw (Preserve RGB), directly or through Reroutes and the channel nodes
// that keep its values. A source's `channels` output is data and has no transform. `colorSpace`
// names the transform for the warning ("sRGB", "Linear", an OCIO id, or "Automatic"). The fix sets
// the source's colorSpace to Raw and clears its inputColorSpaceId.
struct DepthColorTransform final {
    NodeId source;
    std::string colorSpace;
};
class Composition;
class Project;
[[nodiscard]] std::optional<DepthColorTransform>
depthInputColorTransform(const Project& project, const Composition& composition, NodeId node);
// The input colour space id that reads a file as data (no colour transform).
inline constexpr std::string_view kRawInputColorSpaceId = "raw";

} // namespace bloom::document
