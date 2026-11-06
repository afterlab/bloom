#pragma once

// #507 Lens Flare: the node's definition, its presets and where its elements land, shared by the
// renderer's table and the card's picture so both draw the same flare. The parameter vocabulary is
// lens_flare_nodes.hpp.

#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {

[[nodiscard]] std::string lensFlareSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isLensFlareParameterValueValid(std::string_view schemaKey,
                                                  const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition lensFlareDefinition();

// --- Presets ------------------------------------------------------------------------------------

// One starting look: every value of the element stack and the whole stack's size, colour, rotation
// and chromatic split. A preset writes values once (one undo step); nothing stays linked to it, and
// it leaves the light's position, its intensity and the occlusion and source readings alone.
struct LensFlarePresetValue final {
    std::string_view role;
    ParameterValue value;
};
struct LensFlarePreset final {
    std::string_view name;
    std::vector<LensFlarePresetValue> values;
};
[[nodiscard]] const std::vector<LensFlarePreset>& lensFlarePresets();

// --- Layout -------------------------------------------------------------------------------------

// Lengths are in frame heights, measured from the frame's centre; x runs right and y down.
struct LensFlareLayoutInput final {
    double lightX = 0.0;
    double lightY = 0.0;
    double size = 1.0;
    double rotationDegrees = 0.0;
    std::int64_t seed = 1;
    std::int64_t streakCount = 0;
    double streakLength = 0.0;
    std::int64_t ghostCount = 0;
    double ghostSize = 0.0;
    double ghostSpread = 1.0;
    std::int64_t irisCount = 0;
    std::int64_t irisBlades = 6;
    double irisSize = 0.0;
    double irisSpread = 1.0;
};
struct LensFlareDisc final {
    double x = 0.0;
    double y = 0.0;
    double radius = 0.0;
    // The tint this disc adds to the light's colour.
    std::array<double, 3> tint{1.0, 1.0, 1.0};
};
struct LensFlareStreak final {
    double directionX = 1.0;
    double directionY = 0.0;
    double length = 0.0;
};
struct LensFlareLayout final {
    std::vector<LensFlareStreak> streaks;
    std::vector<LensFlareDisc> ghosts;
    std::vector<LensFlareDisc> irises;
    // The unit normals of the iris polygon's edges.
    std::vector<std::array<double, 2>> irisNormals;
};
[[nodiscard]] LensFlareLayout lensFlareLayout(const LensFlareLayoutInput& input);

// The one-point perspective of the 3D mode: the scale from `position` to the projected point, or
// nothing behind the camera (zoom + distance <= 0).
[[nodiscard]] bool lensFlareProjection(double zoom, double distance, double& scale) noexcept;

} // namespace bloom::document
