#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/document/lens_flare_definition.hpp>
#include <bloom/document/film_grain_definition.hpp>
#include <bloom/document/codec_degrade_definition.hpp>
#include <bloom/document/auto_exposure_definition.hpp>
#include <bloom/document/audio_effect_definition.hpp>
#include <bloom/document/lens_distortion_definition.hpp>
#include <bloom/document/rolling_shutter_definition.hpp>
#include <bloom/document/chromatic_aberration_definition.hpp>
#include <bloom/document/test_card_definition.hpp>
#include <bloom/document/light_rays_definition.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/sharpen_definition.hpp>
#include <bloom/document/field_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/image_channel_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/matte_nodes.hpp>

#include <bloom/document/image_preview.hpp>

#include "value_node_definitions.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/shape.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using bloom::document::InputPortDefinition;
using bloom::document::LayerSlotInputDefinition;
using bloom::document::NodeCardinality;
using bloom::document::NodeCategory;
using bloom::document::NodeDefinition;
using bloom::document::NodeLoweringKind;
using bloom::document::OutputPortDefinition;
using bloom::document::ParameterDefinition;
using bloom::document::ParameterValueKind;
using bloom::document::SocketValueKind;

struct NodeLookupKey {
    std::string_view typeId;
    std::uint32_t schemaVersion = 0;
};

[[nodiscard]] bool definitionPrecedesKey(const NodeDefinition& definition,
                                         const NodeLookupKey key) noexcept {
    const std::string_view definitionType{definition.key.typeId};
    return definitionType < key.typeId ||
           (definitionType == key.typeId && definition.key.schemaVersion < key.schemaVersion);
}

template <typename Definition>
[[nodiscard]] bool hasUniqueNonEmptyNames(const std::vector<Definition>& definitions) {
    std::unordered_set<std::string_view> names;
    for (const auto& definition : definitions) {
        if (definition.name.empty() || !names.insert(definition.name).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasValidParameters(const std::vector<ParameterDefinition>& definitions) {
    std::unordered_set<std::string_view> roles;
    for (const auto& definition : definitions) {
        if (definition.role.empty() || definition.schemaKey.empty() ||
            !roles.insert(definition.role).second) {
            return false;
        }
    }
    return true;
}

// The node's image transport input, which is always the FIRST one: task S7 gave every parameter
// role its own linkable socket, so a Layer Output's input list is now the content image followed by
// one operand socket per transform value. The image port's position is what the dissolve gesture,
// the mute bypass and the empty-image propagation all read, so it stays pinned at the front.
[[nodiscard]] bool hasLeadingImageInput(const NodeDefinition& definition,
                                        const std::string_view name,
                                        const bool required = true) noexcept {
    return !definition.inputs.empty() && definition.inputs.front().name == name &&
           definition.inputs.front().valueKind == SocketValueKind::Image &&
           definition.inputs.front().required == required;
}

// Task S7, item 3: every parameter role of a node is ALSO a linkable input socket of its kind.
// Checked here, generically, so no lowering can declare a parameter the editor cannot link or a
// socket that writes nothing -- and so the rule has one statement rather than one per lowering.
// `imageInputCount` is how many leading image transport ports precede the operand block.
[[nodiscard]] bool hasParameterSockets(const NodeDefinition& definition,
                                       const std::size_t imageInputCount) noexcept {
    if (definition.inputs.size() != imageInputCount + definition.parameters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < definition.parameters.size(); ++index) {
        const auto& declared = definition.parameters[index];
        const auto& socket = definition.inputs[imageInputCount + index];
        if (socket.name != declared.role || socket.required ||
            socket.valueKind != socketKindForParameterValueKind(declared.valueKind)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasImageOutput(const NodeDefinition& definition,
                                  const std::string_view name) noexcept {
    return std::ranges::count_if(definition.outputs, [&](const auto& output) {
               return output.name == name && output.valueKind == SocketValueKind::Image;
           }) == 1;
}

[[nodiscard]] bool hasOutput(const NodeDefinition& definition, const std::string_view name,
                             const SocketValueKind kind) noexcept {
    return std::ranges::count_if(definition.outputs, [&](const auto& output) {
               return output.name == name && output.valueKind == kind;
           }) == 1;
}

[[nodiscard]] bool hasParameter(const NodeDefinition& definition, const std::size_t index,
                                const std::string_view role, const std::string_view schemaKey,
                                const ParameterValueKind valueKind,
                                const bool supportsAnimation = false,
                                const bool required = true) noexcept {
    return definition.parameters.size() > index && definition.parameters[index].role == role &&
           definition.parameters[index].schemaKey == schemaKey &&
           definition.parameters[index].valueKind == valueKind &&
           definition.parameters[index].required == required &&
           definition.parameters[index].supportsAnimation == supportsAnimation;
}

[[nodiscard]] bool hasCanonicalKey(const NodeDefinition& definition, const std::string_view typeId,
                                   const std::uint32_t schemaVersion) noexcept {
    return definition.key.typeId == typeId && definition.key.schemaVersion == schemaVersion;
}

// #340 Levels: an Image in and out; per channel (RGB master, red, green, blue, alpha) five
// animatable/drivable Scalars, each its own socket, then the constant clamp switch (no socket, like
// Luma Key's invert).
[[nodiscard]] NodeDefinition levelsDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{{std::string(kLevelsNodeType), 1},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Grade};
    for (std::size_t channel = 0; channel < kGradeChannelCount; ++channel)
        for (std::size_t field = 0; field < kLevelsFieldCount; ++field) {
            definition.parameters.push_back(
                {levelsRole(channel, field), levelsSchemaKey(channel, field),
                 ParameterValueKind::Float64, true, true, kLevelsFieldDefaults[field]});
            definition.inputs.push_back(
                {levelsRole(channel, field), SocketValueKind::Scalar, false});
        }
    definition.parameters.push_back({"clamp", std::string(kLevelsClampSchemaKey),
                                     ParameterValueKind::Boolean, true, false, false});
    return definition;
}

// #493 Curves schema 2: one Image in and out; nine animatable Path parameters used as point lists
// (the five channel curves, then the four hue curves; grade_curves.hpp), with no sockets, then the
// constant clamp switch.
[[nodiscard]] NodeDefinition curvesDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{{std::string(kCurvesNodeType), kCurvesNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Grade};
    for (const auto curve : kGradeCurves)
        definition.parameters.push_back({std::string(gradeCurveRole(curve)),
                                         std::string(gradeCurveSchemaKey(curve)),
                                         ParameterValueKind::Path, true, true,
                                         defaultGradeCurve(curve)});
    definition.parameters.push_back({"clamp", std::string(kCurvesClampSchemaKey),
                                     ParameterValueKind::Boolean, true, false, false});
    return definition;
}

// #503 Alpha Association: an Image in and out; a constant mode and white-background switch (they
// choose the arithmetic, so they have no socket) and an animatable/drivable background colour for
// Remove Color Matting, with its own Color socket.
[[nodiscard]] NodeDefinition alphaAssociationDefinition() {
    using namespace bloom::document;
    return {{std::string(kAlphaAssociationNodeType), 1},
            NodeLoweringKind::ImageEffect,
            {{"input", SocketValueKind::Image}, {"background", SocketValueKind::Color, false}},
            {{"image", SocketValueKind::Image}},
            {{"mode", std::string(kAlphaAssociationModeSchemaKey), ParameterValueKind::Integer,
              true, false, kAlphaAssociationUnmult},
             {"white", std::string(kAlphaAssociationWhiteSchemaKey), ParameterValueKind::Boolean,
              true, false, false},
             {"background", std::string(kAlphaAssociationBackgroundSchemaKey),
              ParameterValueKind::Color4d, true, true, bloom::core::Color4d{0.0, 0.0, 0.0, 1.0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Compositing};
}

// #504 Colorize: an Image in and out (#525: the input also takes a field, drawn raw over the
// composition, so a scalar field's value is the grey the Colorize maps); the constant mode,
// source and cycle (they choose the arithmetic, so they have no socket), then one socket per
// animatable/drivable value in parameter order.
[[nodiscard]] NodeDefinition colorizeDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{{std::string(kColorizeNodeType), 1},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image, true, false, true}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Grade};
    const auto scalar = [&](std::string role, const std::string_view schemaKey) {
        definition.parameters.push_back({role, std::string(schemaKey), ParameterValueKind::Float64,
                                         true, true,
                                         colorizeScalarDomain(schemaKey)->defaultValue});
        definition.inputs.push_back({std::move(role), SocketValueKind::Scalar, false});
    };
    const auto color = [&](std::string role, const std::string_view schemaKey,
                           const bloom::core::Color4d value) {
        definition.parameters.push_back(
            {role, std::string(schemaKey), ParameterValueKind::Color4d, true, true, value});
        definition.inputs.push_back({std::move(role), SocketValueKind::Color, false});
    };
    const auto integer = [&](std::string role, const std::string_view schemaKey) {
        definition.parameters.push_back({std::move(role), std::string(schemaKey),
                                         ParameterValueKind::Integer, true, false,
                                         colorizeIntegerDomain(schemaKey)->defaultValue});
    };
    integer("mode", kColorizeModeSchemaKey);
    scalar("amount", kColorizeAmountSchemaKey);
    color("fillColor", kColorizeColorSchemaKey, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0});
    color("shadows", kColorizeShadowsSchemaKey, bloom::core::Color4d{0.0, 0.0, 0.0, 1.0});
    // A warm brown whose Rec. 709 luma is mid grey (0.18), as the ramp's middle stop.
    color("midtones", kColorizeMidtonesSchemaKey, bloom::core::Color4d{0.24, 0.17, 0.1, 1.0});
    color("highlights", kColorizeHighlightsSchemaKey, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0});
    scalar("midpoint", kColorizeMidpointSchemaKey);
    integer("source", kColorizeSourceSchemaKey);
    integer("cycle", kColorizeCycleSchemaKey);
    scalar("repetitions", kColorizeRepetitionsSchemaKey);
    scalar("phase", kColorizePhaseSchemaKey);
    scalar("reds", kColorizeRedsSchemaKey);
    scalar("yellows", kColorizeYellowsSchemaKey);
    scalar("greens", kColorizeGreensSchemaKey);
    scalar("cyans", kColorizeCyansSchemaKey);
    scalar("blues", kColorizeBluesSchemaKey);
    scalar("magentas", kColorizeMagentasSchemaKey);
    // #541: Gradient Map's gradient, optional (a node made before it maps through its ramp) and
    // animatable as a whole value, with no socket, like a Curves curve.
    definition.parameters.push_back({"gradient", std::string(kColorizeGradientSchemaKey),
                                     ParameterValueKind::Gradient, false, true,
                                     defaultColorizeGradient()});
    return definition;
}

// #505 Vignette: an Image in and out; the constant shape and mode (they choose the arithmetic, so
// they have no socket), then one socket per animatable/drivable value in parameter order.
[[nodiscard]] NodeDefinition vignetteDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{{std::string(kVignetteNodeType), 1},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Filters};
    const auto integer = [&](std::string role, const std::string_view schemaKey) {
        definition.parameters.push_back({std::move(role), std::string(schemaKey),
                                         ParameterValueKind::Integer, true, false,
                                         vignetteIntegerDomain(schemaKey)->defaultValue});
    };
    const auto scalar = [&](std::string role, const std::string_view schemaKey) {
        definition.parameters.push_back({role, std::string(schemaKey), ParameterValueKind::Float64,
                                         true, true,
                                         vignetteScalarDomain(schemaKey)->defaultValue});
        definition.inputs.push_back({std::move(role), SocketValueKind::Scalar, false});
    };
    integer("shape", kVignetteShapeSchemaKey);
    integer("mode", kVignetteModeSchemaKey);
    scalar("amount", kVignetteAmountSchemaKey);
    scalar("exposure", kVignetteExposureSchemaKey);
    scalar("midpoint", kVignetteMidpointSchemaKey);
    scalar("roundness", kVignetteRoundnessSchemaKey);
    scalar("feather", kVignetteFeatherSchemaKey);
    scalar("highlightProtection", kVignetteHighlightProtectionSchemaKey);
    definition.parameters.push_back({"center", std::string(kVignetteCenterSchemaKey),
                                     ParameterValueKind::Vec2d, true, true, Vec2d{}});
    definition.inputs.push_back({"center", SocketValueKind::Vector2, false});
    return definition;
}

// #340 Hue/Saturation: animatable/drivable hue rotation (degrees) and saturation factor.
[[nodiscard]] NodeDefinition hueSaturationDefinition() {
    using namespace bloom::document;
    return {{std::string(kHueSaturationNodeType), 1},
            NodeLoweringKind::ImageEffect,
            {{"input", SocketValueKind::Image},
             {"hue", SocketValueKind::Scalar, false},
             {"saturation", SocketValueKind::Scalar, false}},
            {{"image", SocketValueKind::Image}},
            {{"hue", std::string(kHueSaturationHueSchemaKey), ParameterValueKind::Float64, true,
              true, 0.0},
             {"saturation", std::string(kHueSaturationSaturationSchemaKey),
              ParameterValueKind::Float64, true, true, 1.0}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Grade};
}

// #494 Grade: an Image in and out; the four wheels (lift, gamma, gain, offset), each a master, red,
// green and blue Scalar, then the six primaries, all animatable/drivable and each its own socket.
[[nodiscard]] NodeDefinition gradeDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{{std::string(kGradeNodeType), 1},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Grade};
    const auto add = [&](std::string role, std::string schemaKey, const double neutral) {
        definition.parameters.push_back(
            {role, std::move(schemaKey), ParameterValueKind::Float64, true, true, neutral});
        definition.inputs.push_back({std::move(role), SocketValueKind::Scalar, false});
    };
    for (const auto wheel : kGradeWheels)
        for (std::size_t component = 0; component < kGradeWheelComponentCount; ++component)
            add(gradeWheelRole(wheel, component), gradeWheelSchemaKey(wheel, component),
                gradeWheelDomain(wheel).neutral);
    for (const auto primary : kGradePrimaries)
        add(std::string(gradePrimaryRole(primary)), gradePrimarySchemaKey(primary),
            gradePrimaryDomain(primary).neutral);
    return definition;
}

// #339 Drop Shadow, Glow and Light Wrap: compositing recipes over the existing image primitives.
// Their leading Image inputs come first, then one socket per animatable value in parameter order,
// so a value-graph driver can feed any of them; the switches (and Glow's octave count, mode and
// ray count) are constants without a socket.
[[nodiscard]] NodeDefinition recipeDefinition(const std::string_view typeId) {
    using namespace bloom::document;
    NodeDefinition definition{{std::string(typeId), typeId == kGlowNodeType
                                                        ? kGlowNodeSchemaVersion
                                                        : kRecipeNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Compositing};
    if (typeId == kGlowNodeType) {
        // An optional key pass glows instead of the input; an optional Mask limits the glow; an
        // optional lens-dirt image (#506) adds the glow modulated by its luma.
        definition.inputs.push_back({"key", SocketValueKind::Image, false});
        definition.inputs.push_back({"mask", SocketValueKind::Mask, false});
        definition.inputs.push_back({"dirt", SocketValueKind::Image, false});
    } else if (typeId == kLightWrapNodeType) {
        definition.inputs.push_back({"background", SocketValueKind::Image, false});
    }
    // #506: Glow's mode and shared controls are optional (`required` false), so a Glow saved
    // before them stays valid and lowers as Classic with their defaults.
    const auto scalar = [&](std::string role, const std::string_view schemaKey,
                            const bool required = true) {
        definition.parameters.push_back({role, std::string(schemaKey), ParameterValueKind::Float64,
                                         required, true,
                                         recipeScalarDomain(schemaKey)->defaultValue});
        definition.inputs.push_back({std::move(role), SocketValueKind::Scalar, false});
    };
    const auto color = [&](std::string role, const std::string_view schemaKey,
                           const bloom::core::Color4d value, const bool required = true) {
        definition.parameters.push_back(
            {role, std::string(schemaKey), ParameterValueKind::Color4d, required, true, value});
        definition.inputs.push_back({std::move(role), SocketValueKind::Color, false});
    };
    const auto flag = [&](std::string role, const std::string_view schemaKey,
                          const bool required = true) {
        definition.parameters.push_back({std::move(role), std::string(schemaKey),
                                         ParameterValueKind::Boolean, required, false, false});
    };
    const auto integer = [&](std::string role, const std::string_view schemaKey,
                             const bool required = true) {
        definition.parameters.push_back({std::move(role), std::string(schemaKey),
                                         ParameterValueKind::Integer, required, false,
                                         recipeIntegerDomain(schemaKey)->defaultValue});
    };
    if (typeId == kDropShadowNodeType) {
        color("color", kDropShadowColorSchemaKey, bloom::core::Color4d{0.0, 0.0, 0.0, 1.0});
        scalar("opacity", kDropShadowOpacitySchemaKey);
        scalar("direction", kDropShadowDirectionSchemaKey);
        scalar("distance", kDropShadowDistanceSchemaKey);
        scalar("softness", kDropShadowSoftnessSchemaKey);
        flag("shadowOnly", kDropShadowShadowOnlySchemaKey);
    } else if (typeId == kGlowNodeType) {
        integer("mode", kGlowModeSchemaKey, false);
        scalar("threshold", kGlowThresholdSchemaKey);
        scalar("knee", kGlowKneeSchemaKey);
        scalar("intensity", kGlowIntensitySchemaKey);
        scalar("size", kGlowSizeSchemaKey);
        scalar("falloff", kGlowFalloffSchemaKey);
        scalar("exponent", kGlowExponentSchemaKey, false);
        color("tint", kGlowTintSchemaKey, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0});
        flag("gradient", kGlowGradientSchemaKey, false);
        // #541: the tint gradient is a whole gradient value: animatable, with no socket.
        definition.parameters.push_back({"tintGradient", std::string(kGlowTintGradientSchemaKey),
                                         ParameterValueKind::Gradient, false, true,
                                         defaultGlowTintGradient()});
        scalar("radiusRed", kGlowRadiusRedSchemaKey, false);
        scalar("radiusGreen", kGlowRadiusGreenSchemaKey, false);
        scalar("radiusBlue", kGlowRadiusBlueSchemaKey, false);
        integer("octaves", kGlowOctavesSchemaKey);
        integer("points", kGlowPointsSchemaKey, false);
        scalar("angle", kGlowAngleSchemaKey, false);
        scalar("length", kGlowLengthSchemaKey, false);
        scalar("chromatic", kGlowChromaticSchemaKey, false);
        scalar("dirtAmount", kGlowDirtAmountSchemaKey, false);
        scalar("rollOff", kGlowRollOffSchemaKey, false);
        flag("glowOnly", kGlowGlowOnlySchemaKey);
    } else {
        scalar("radius", kLightWrapRadiusSchemaKey);
        scalar("amount", kLightWrapAmountSchemaKey);
        flag("wrapOnly", kLightWrapWrapOnlySchemaKey);
    }
    return definition;
}

// #479 Separate Channels: one Image in; red, green, blue and alpha Image outputs.
[[nodiscard]] NodeDefinition separateChannelsDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{
        {std::string(kSeparateChannelsNodeType), kImageChannelNodeSchemaVersion},
        NodeLoweringKind::ImageEffect,
        {{std::string(kSeparateChannelsInputPort), SocketValueKind::Image}},
        {},
        {},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Compositing};
    for (const auto port : kImageChannelPortNames)
        definition.outputs.push_back({std::string(port), SocketValueKind::Image});
    return definition;
}

// #479 Combine Channels: four optional Image inputs (red, green, blue, alpha), a constant source
// channel dropdown per input and the constant alpha mode, one Image out. No sockets: the choices
// are closed constants, like Luma Key's invert.
[[nodiscard]] NodeDefinition combineChannelsDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{
        {std::string(kCombineChannelsNodeType), kImageChannelNodeSchemaVersion},
        NodeLoweringKind::ImageEffect,
        {},
        {{std::string(kCombineChannelsOutputPort), SocketValueKind::Image}},
        {},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Compositing};
    for (std::size_t channel = 0; channel < kImageChannelCount; ++channel) {
        definition.inputs.push_back(
            {std::string(kImageChannelPortNames[channel]), SocketValueKind::Image, false});
        definition.parameters.push_back({std::string(kCombineChannelsSourceRoles[channel]),
                                         std::string(kCombineChannelsSourceSchemaKey),
                                         ParameterValueKind::Integer, true, false,
                                         kCombineChannelsSourceDefaults[channel]});
    }
    definition.parameters.push_back({std::string(kCombineChannelsAlphaModeRole),
                                     std::string(kCombineChannelsAlphaModeSchemaKey),
                                     ParameterValueKind::Integer, true, false,
                                     kCombineChannelsPremultiplied});
    return definition;
}

[[nodiscard]] NodeDefinition adjustmentDefinition();

[[nodiscard]] bool hasValidLoweringShape(const NodeDefinition& definition) {
    using namespace bloom::document;
    switch (definition.lowering) {
    case NodeLoweringKind::Field:
    case NodeLoweringKind::ValueFieldSample: {
        // #525: the exact generated shape of every Fields node.
        const auto* field = fieldNodeSpec(definition.key.typeId);
        return field != nullptr && definition == fieldNodeDefinition(*field);
    }
    case NodeLoweringKind::ImageEffect:
        if (definition.key.typeId == "bloom.gaussian-blur" ||
            definition.key.typeId == "bloom.box-blur") {
            return definition.key.schemaVersion == 1 &&
                   definition.inputs ==
                       std::vector<InputPortDefinition>{
                           {"input", SocketValueKind::Image},
                           {"radius", SocketValueKind::Scalar, false}} &&
                   definition.parameters.size() == 2 &&
                   hasParameter(definition, 0, "radius", "bloom.blur.radius",
                                ParameterValueKind::Float64, true) &&
                   hasParameter(definition, 1, "edge", "bloom.blur.edge",
                                ParameterValueKind::Integer) &&
                   definition.outputs ==
                       std::vector<OutputPortDefinition>{{"image", SocketValueKind::Image}} &&
                   !definition.layerSlotInput && !definition.audioLayerSlotInput;
        }
        // #337 Matte Blur, #338 Matte Erode and Matte Dilate filter an ADR 0028 Mask: Mask in,
        // Mask out, never an Image.
        if (definition.key.typeId == "bloom.matte-blur" ||
            definition.key.typeId == "bloom.matte-erode" ||
            definition.key.typeId == "bloom.matte-dilate") {
            return definition.key.schemaVersion == 1 &&
                   definition.inputs ==
                       std::vector<InputPortDefinition>{
                           {"input", SocketValueKind::Mask},
                           {"radius", SocketValueKind::Scalar, false}} &&
                   definition.parameters.size() == 2 &&
                   hasParameter(definition, 0, "radius", "bloom.blur.radius",
                                ParameterValueKind::Float64, true) &&
                   hasParameter(definition, 1, "edge", "bloom.blur.edge",
                                ParameterValueKind::Integer) &&
                   definition.outputs ==
                       std::vector<OutputPortDefinition>{{"mask", SocketValueKind::Mask}} &&
                   !definition.layerSlotInput && !definition.audioLayerSlotInput;
        }
        // #338 Luma Key: an Image in, an ADR 0028 Mask out, never an Image output.
        if (definition.key.typeId == "bloom.luma-key") {
            return definition.key.schemaVersion == 1 &&
                   definition.inputs ==
                       std::vector<InputPortDefinition>{{"input", SocketValueKind::Image},
                                                        {"low", SocketValueKind::Scalar, false},
                                                        {"high", SocketValueKind::Scalar, false}} &&
                   definition.parameters.size() == 3 &&
                   hasParameter(definition, 0, "low", "bloom.luma-key.low",
                                ParameterValueKind::Float64, true) &&
                   hasParameter(definition, 1, "high", "bloom.luma-key.high",
                                ParameterValueKind::Float64, true) &&
                   hasParameter(definition, 2, "invert", "bloom.luma-key.invert",
                                ParameterValueKind::Boolean) &&
                   definition.outputs ==
                       std::vector<OutputPortDefinition>{{"mask", SocketValueKind::Mask}} &&
                   !definition.layerSlotInput && !definition.audioLayerSlotInput;
        }
        // #340: the exact generated shapes, sockets and defaults.
        if (definition.key.typeId == kLevelsNodeType)
            return definition == levelsDefinition();
        if (definition.key.typeId == kCurvesNodeType)
            return definition == curvesDefinition();
        if (definition.key.typeId == kHueSaturationNodeType)
            return definition == hueSaturationDefinition();
        if (definition.key.typeId == kAlphaAssociationNodeType)
            return definition == alphaAssociationDefinition();
        if (definition.key.typeId == kColorizeNodeType)
            return definition == colorizeDefinition();
        if (definition.key.typeId == kVignetteNodeType)
            return definition == vignetteDefinition();
        // #483 Z Defocus and Depth Fog: the exact generated shapes.
        if (isDepthEffectNodeType(definition.key.typeId))
            return definition == depthEffectDefinition(definition.key.typeId);
        // #507 Lens Flare: the exact generated shape.
        if (definition.key.typeId == kLensFlareNodeType)
            return definition == lensFlareDefinition();
        // #486 Film Grain: the exact generated shape.
        if (definition.key.typeId == kFilmGrainNodeType)
            return definition == filmGrainDefinition();
        // #486 Lens Distortion: the exact generated shape.
        if (definition.key.typeId == kLensDistortionNodeType)
            return definition == lensDistortionDefinition();
        // #568 Rolling Shutter: the exact generated shape.
        if (definition.key.typeId == kRollingShutterNodeType)
            return definition == rollingShutterDefinition();
        // #486 Chromatic Aberration: the exact generated shape.
        if (definition.key.typeId == kChromaticAberrationNodeType)
            return definition == chromaticAberrationDefinition();
        // #508 Light Rays: the exact generated shape.
        if (definition.key.typeId == kLightRaysNodeType)
            return definition == lightRaysDefinition();
        // #563 Codec Degrade: the exact generated shape.
        if (definition.key.typeId == kCodecDegradeNodeType)
            return definition == codecDegradeDefinition();
        // #566 Auto Exposure: the exact generated shape.
        if (definition.key.typeId == kAutoExposureNodeType)
            return definition == autoExposureDefinition();
        // #484 the motion-blur nodes: the exact generated shapes.
        if (isMotionBlurNodeType(definition.key.typeId))
            return definition == motionBlurDefinition(definition.key.typeId);
        // #512 Camera Shake: the exact generated shape.
        if (definition.key.typeId == kCameraShakeNodeType)
            return definition == cameraShakeDefinition();
        // #486 Sharpen: the exact generated shape.
        if (definition.key.typeId == kSharpenNodeType)
            return definition == sharpenDefinition();
        if (definition.key.typeId == kGradeNodeType)
            return definition == gradeDefinition();
        if (definition.key.typeId == kSeparateChannelsNodeType)
            return definition == separateChannelsDefinition();
        if (definition.key.typeId == kCombineChannelsNodeType)
            return definition == combineChannelsDefinition();
        if (definition.key.typeId == kDropShadowNodeType ||
            definition.key.typeId == kGlowNodeType || definition.key.typeId == kLightWrapNodeType)
            return definition == recipeDefinition(definition.key.typeId);
        if (definition.key.typeId == "bloom.transform-2d") {
            return definition.key.schemaVersion == 1 && definition.inputs.size() == 5 &&
                   hasLeadingImageInput(definition, "input") &&
                   definition.parameters.size() ==
                       6 + bloom::document::transformMotionBlurParameterDefinitions().size() &&
                   std::equal(definition.parameters.begin() + 6, definition.parameters.end(),
                              bloom::document::transformMotionBlurParameterDefinitions().begin()) &&
                   hasParameter(definition, 0, "translate", "bloom.transform-2d.translate",
                                ParameterValueKind::Vec2d, true) &&
                   hasParameter(definition, 1, "pivot", "bloom.transform-2d.pivot",
                                ParameterValueKind::Vec2d, true) &&
                   hasParameter(definition, 2, "scale", "bloom.transform-2d.scale",
                                ParameterValueKind::Vec2d, true) &&
                   hasParameter(definition, 3, "rotation", "bloom.transform-2d.rotation",
                                ParameterValueKind::Float64, true) &&
                   hasParameter(definition, 4, "extent", "bloom.transform-2d.extent",
                                ParameterValueKind::Integer) &&
                   hasParameter(definition, 5, "filter", "bloom.transform-2d.filter",
                                ParameterValueKind::Integer) &&
                   [&] {
                       for (std::size_t index = 0; index < 4; ++index) {
                           const auto& socket = definition.inputs[index + 1];
                           const auto& parameter = definition.parameters[index];
                           if (socket.name != parameter.role || socket.required ||
                               socket.valueKind !=
                                   socketKindForParameterValueKind(parameter.valueKind))
                               return false;
                       }
                       return true;
                   }() &&
                   definition.outputs ==
                       std::vector<OutputPortDefinition>{{"image", SocketValueKind::Image}} &&
                   !definition.layerSlotInput && !definition.audioLayerSlotInput;
        }
        if (definition.key.typeId == "bloom.image-mix" ||
            definition.key.typeId == "bloom.set-alpha") {
            const bool mix = definition.key.typeId == "bloom.image-mix";
            return definition.key.schemaVersion == 1 &&
                   definition.inputs.size() == (mix ? 4U : 3U) &&
                   hasLeadingImageInput(definition, "input") &&
                   // #361: Set Alpha's matte is optional; Image Mix's second image is not.
                   definition.inputs[1] == InputPortDefinition{mix ? "second" : "matte",
                                                               SocketValueKind::Image, mix} &&
                   (!mix ||
                    definition.inputs[2] == InputPortDefinition{"mask", SocketValueKind::Mask,
                                                                false}) &&
                   hasParameterSockets(definition, mix ? 3 : 2) &&
                   hasParameter(definition, 0, mix ? "factor" : "channel",
                                mix ? "bloom.image-mix.factor" : "bloom.set-alpha.channel",
                                mix ? ParameterValueKind::Float64 : ParameterValueKind::Integer,
                                mix) &&
                   definition.outputs ==
                       std::vector<OutputPortDefinition>{{"image", SocketValueKind::Image}} &&
                   !definition.layerSlotInput && !definition.audioLayerSlotInput;
        }
        if (definition.key.typeId == "bloom.multiply-alpha") {
            return definition.key.schemaVersion == 1 && definition.inputs.size() == 4 &&
                   hasLeadingImageInput(definition, "input") &&
                   definition.inputs[1] ==
                       InputPortDefinition{"matte", SocketValueKind::Image, false} &&
                   hasParameterSockets(definition, 2) &&
                   hasParameter(definition, 0, "factor", "bloom.multiply-alpha.factor",
                                ParameterValueKind::Float64, true, true) &&
                   hasParameter(definition, 1, "channel", "bloom.multiply-alpha.channel",
                                ParameterValueKind::Integer) &&
                   definition.outputs ==
                       std::vector<OutputPortDefinition>{{"image", SocketValueKind::Image}} &&
                   !definition.layerSlotInput && !definition.audioLayerSlotInput;
        }
        return (definition.inputs ==
                    std::vector<InputPortDefinition>{{"input", SocketValueKind::Image}} ||
                (hasLeadingImageInput(definition, "input") &&
                 hasParameterSockets(definition, 1))) &&
               definition.outputs ==
                   std::vector<OutputPortDefinition>{{"image", SocketValueKind::Image}} &&
               !definition.layerSlotInput && !definition.audioLayerSlotInput;
    case NodeLoweringKind::Shape:
        return definition == shapeDefinition();
    case NodeLoweringKind::Mask:
        return definition == maskDefinition();
    // #383 Mask Combine and Image to Mask.
    case NodeLoweringKind::Matte:
        return definition == maskCombineDefinition() || definition == imageToMaskDefinition();
    case NodeLoweringKind::Adjustment:
        return definition == adjustmentDefinition();
    // #587 Test Card: the exact generated shape.
    case NodeLoweringKind::TestCard:
        return definition == testCardDefinition();
    case NodeLoweringKind::Solid:
        return hasImageOutput(definition, kSolidSourceOutputPort) &&
               (definition.parameters.size() == 3 &&
                hasParameter(definition, 1, kSolidWidthParameterRole, kSolidWidthParameterSchemaKey,
                             ParameterValueKind::Float64, true) &&
                hasParameter(definition, 2, kSolidHeightParameterRole,
                             kSolidHeightParameterSchemaKey, ParameterValueKind::Float64, true)) &&
               hasParameter(definition, 0, kSolidColorParameterRole, kSolidColorParameterSchemaKey,
                            ParameterValueKind::Color4d,
                            isAnimatableSchemaKey(kSolidColorParameterSchemaKey)) &&
               hasParameterSockets(definition, 0) && !definition.layerSlotInput.has_value();
    case NodeLoweringKind::Checker:
        return hasCanonicalKey(definition, kCheckerSourceNodeType,
                               kCheckerSourceNodeSchemaVersion) &&
               definition.inputs.size() == 2 && definition.parameters.size() == 6 &&
               hasImageOutput(definition, "image") &&
               hasParameter(definition, 0, "colorA", kSolidColorParameterSchemaKey,
                            ParameterValueKind::Color4d, true) &&
               hasParameter(definition, 1, "colorB", kSolidColorParameterSchemaKey,
                            ParameterValueKind::Color4d, true) &&
               hasParameter(definition, 2, "cellWidth", kCheckerCellWidthParameterSchemaKey,
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 3, "cellHeight", kCheckerCellHeightParameterSchemaKey,
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 4, "width", kCheckerWidthParameterSchemaKey,
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 5, "height", kCheckerHeightParameterSchemaKey,
                            ParameterValueKind::Integer) &&
               definition.inputs[0].name == "colorA" &&
               definition.inputs[0].valueKind == SocketValueKind::Color &&
               !definition.inputs[0].required && definition.inputs[1].name == "colorB" &&
               definition.inputs[1].valueKind == SocketValueKind::Color &&
               !definition.inputs[1].required && !definition.layerSlotInput;
    case NodeLoweringKind::Gradient:
        return (hasCanonicalKey(definition, kLinearGradientNodeType, kGradientNodeSchemaVersion) ||
                hasCanonicalKey(definition, kRadialGradientNodeType, kGradientNodeSchemaVersion)) &&
               definition.inputs.empty() && hasImageOutput(definition, "image") &&
               definition.outputs.size() == 1 && definition.parameters.size() == 6 &&
               hasParameter(definition, 0, "gradient", kGradientStopsParameterSchemaKey,
                            ParameterValueKind::Gradient, true) &&
               hasParameter(definition, 1, "start", kGradientStartParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 2, "end", kGradientEndParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 3, "width", kSolidWidthParameterSchemaKey,
                            ParameterValueKind::Float64, true) &&
               hasParameter(definition, 4, "height", kSolidHeightParameterSchemaKey,
                            ParameterValueKind::Float64, true) &&
               hasParameter(definition, 5, "repeat", kGradientRepeatParameterSchemaKey,
                            ParameterValueKind::Integer) &&
               !definition.layerSlotInput;
    case NodeLoweringKind::VideoSource:
        return hasCanonicalKey(definition, "bloom.video-source", 1) &&
               hasOutput(definition, "image", SocketValueKind::Image) &&
               hasOutput(definition, "audio", SocketValueKind::Audio) &&
               // #480: the decoded frame's R, G, B and A as a channel set.
               hasOutput(definition, kVideoSourceChannelsOutputPort, SocketValueKind::ChannelSet) &&
               definition.outputs.size() == 3 && definition.parameters.size() == 6 &&
               definition.inputs.empty() &&
               hasParameter(definition, 0, "asset", "bloom.video.asset",
                            ParameterValueKind::String) &&
               hasParameter(definition, 1, "startFrame", "bloom.video.start-frame",
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 2, "loopMode", "bloom.video.loop-mode",
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 3, "colorSpace", "bloom.video.color-space",
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 4, kVideoInputColorSpaceIdParameterRole,
                            kVideoInputColorSpaceIdParameterSchemaKey, ParameterValueKind::String,
                            false, false) &&
               hasParameter(definition, 5, kVideoAudioStreamParameterRole,
                            kVideoAudioStreamParameterSchemaKey, ParameterValueKind::Integer, false,
                            false);
    case NodeLoweringKind::ImageSource:
        // ADR 0026 v2: the colour `image` plus every channel of the selected part on `channels`.
        return hasCanonicalKey(definition, kImageSourceNodeType, kImageSourceNodeSchemaVersion) &&
               definition.outputs.size() == 2 && hasImageOutput(definition, "image") &&
               hasOutput(definition, kImageSourceChannelsOutputPort, SocketValueKind::ChannelSet) &&
               definition.parameters.size() == 8 && definition.inputs.empty() &&
               hasParameter(definition, 0, "asset", "bloom.image.asset",
                            ParameterValueKind::String) &&
               hasParameter(definition, 1, "startFrame", "bloom.image.start-frame",
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 2, "loopMode", "bloom.image.loop-mode",
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 3, "colorSpace", "bloom.image.color-space",
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 4, "premultiply", "bloom.image.premultiply",
                            ParameterValueKind::Boolean) &&
               hasParameter(definition, 5, kImageInputColorSpaceIdParameterRole,
                            kImageInputColorSpaceIdParameterSchemaKey, ParameterValueKind::String,
                            false, false) &&
               hasParameter(definition, 6, kImagePartParameterRole, kImagePartParameterSchemaKey,
                            ParameterValueKind::String, false, false) &&
               hasParameter(definition, 7, kImageLayerParameterRole,
                            kImageLayerParameterSchemaKey, ParameterValueKind::String, false,
                            false);
    case NodeLoweringKind::Cryptomatte:
        return definition == cryptomatteDefinition();
    case NodeLoweringKind::ImagePreview:
        return definition == imagePreviewDefinition();
    case NodeLoweringKind::Channel:
        return definition == channelShuffleDefinition() ||
               definition == channelCopyDefinition() || definition == channelRemoveDefinition() ||
               definition == channelPassDefinition() ||
               definition == imageToChannelSetDefinition();
    case NodeLoweringKind::CompositionSource:
        return hasCanonicalKey(definition, kCompositionSourceNodeType, 1) &&
               definition.inputs.empty() && definition.outputs.size() == 3 &&
               hasImageOutput(definition, "image") &&
               hasOutput(definition, "audio", SocketValueKind::Audio) &&
               // #480: the nested composite and channel layers as a channel set.
               hasOutput(definition, kCompositionSourceChannelsOutputPort,
                         SocketValueKind::ChannelSet) &&
               definition.parameters.size() == 4 &&
               hasParameter(definition, 0, "composition", "bloom.composition-source.composition",
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 1, "timeOffset", "bloom.composition-source.time-offset",
                            ParameterValueKind::Float64, true) &&
               hasParameter(definition, 2, "timeScale", "bloom.composition-source.time-scale",
                            ParameterValueKind::Float64, true) &&
               hasParameter(definition, 3, "loopMode", "bloom.composition-source.loop-mode",
                            ParameterValueKind::Integer);
    case NodeLoweringKind::AudioSource:
        return hasCanonicalKey(definition, kAudioSourceNodeType, kAudioSourceNodeSchemaVersion) &&
               definition.outputs.size() == 1 &&
               hasOutput(definition, kAudioSourceOutputPort, SocketValueKind::Audio) &&
               definition.inputs.empty() && definition.parameters.size() == 3 &&
               hasParameter(definition, 0, "asset", "bloom.audio.asset",
                            ParameterValueKind::String) &&
               hasParameter(definition, 1, "startFrame", "bloom.audio.start-frame",
                            ParameterValueKind::Integer) &&
               hasParameter(definition, 2, "level", kAudioLevelParameterSchemaKey,
                            ParameterValueKind::Float64, true);
    case NodeLoweringKind::AudioEffect:
        // #569 audio effects: the exact generated shape.
        return isAudioEffectNodeType(definition.key.typeId) &&
               definition == audioEffectDefinition(definition.key.typeId);
    case NodeLoweringKind::Text:
        // Parameter ORDER is part of the shape, like every other lowering here: content, size,
        // colour, typography, font reference, and the box layout controls. Font remains optional
        // at the binding boundary so a pre-FONT-1 text node can still lower to its old default.
        return hasCanonicalKey(definition, kTextSourceNodeType, kTextSourceNodeSchemaVersion) &&
               hasImageOutput(definition, kTextSourceOutputPort) &&
               (definition.parameters.size() == 12 &&
                hasParameter(definition, 3, kTextAlignmentParameterRole,
                             kTextAlignmentParameterSchemaKey, ParameterValueKind::Integer) &&
                hasParameter(definition, 4, kTextLineHeightParameterRole,
                             kTextLineHeightParameterSchemaKey, ParameterValueKind::Float64,
                             true) &&
                hasParameter(definition, 5, kTextLetterSpacingParameterRole,
                             kTextLetterSpacingParameterSchemaKey, ParameterValueKind::Float64,
                             true)) &&
               hasParameter(definition, 6, kTextFontParameterRole, kTextFontParameterSchemaKey,
                            ParameterValueKind::String, false, false) &&
               hasParameter(definition, 7, kTextBoxParameterRole, kTextBoxParameterSchemaKey,
                            ParameterValueKind::Vec2d, false, false) &&
               hasParameter(definition, 8, kTextWrapParameterRole, kTextWrapParameterSchemaKey,
                            ParameterValueKind::Boolean, false, false) &&
               hasParameter(definition, 9, kTextVerticalAlignmentParameterRole,
                            kTextVerticalAlignmentParameterSchemaKey, ParameterValueKind::Integer,
                            false, false) &&
               hasParameter(definition, 10, kTextAnchorModeParameterRole,
                            kTextAnchorModeParameterSchemaKey, ParameterValueKind::Integer, false,
                            false) &&
               hasParameter(definition, 11, kTextOverflowParameterRole,
                            kTextOverflowParameterSchemaKey, ParameterValueKind::Integer, false,
                            false) &&
               hasParameter(definition, 0, kTextParameterRole, kTextParameterSchemaKey,
                            ParameterValueKind::String,
                            isAnimatableSchemaKey(kTextParameterSchemaKey)) &&
               hasParameter(definition, 1, kTextSizeParameterRole, kTextSizeParameterSchemaKey,
                            ParameterValueKind::Float64,
                            isAnimatableSchemaKey(kTextSizeParameterSchemaKey)) &&
               hasParameter(definition, 2, kTextColorParameterRole, kTextColorParameterSchemaKey,
                            ParameterValueKind::Color4d,
                            isAnimatableSchemaKey(kTextColorParameterSchemaKey)) &&
               hasParameterSockets(definition, 0) && !definition.layerSlotInput.has_value();
    case NodeLoweringKind::LayerOutput:
        // Parameter ORDER is the authoring order the properties grid, the node card, and the
        // timeline all read: where the layer sits, the point it turns about, how big it is, how far
        // round it is turned, how much of it shows through, then how it combines with what is
        // beneath it. The two appearance values come after the four geometric ones, and the blend
        // mode comes last because it is the only one that is not a continuous value at all.
        return hasCanonicalKey(definition, kLayerOutputNodeType, kLayerOutputNodeSchemaVersion) &&
               hasLeadingImageInput(definition, kLayerOutputContentInputPort, false) &&
               hasImageOutput(definition, kLayerOutputOutputPort) &&
               definition.parameters.size() == 7 &&
               hasParameter(definition, 0, kPositionParameterRole, kPositionParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 1, kAnchorParameterRole, kAnchorParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 2, kScaleParameterRole, kScaleParameterSchemaKey,
                            ParameterValueKind::Vec2d, true) &&
               hasParameter(definition, 3, kRotationParameterRole, kRotationParameterSchemaKey,
                            ParameterValueKind::Float64, true) &&
               hasParameter(definition, 4, kOpacityParameterRole, kOpacityParameterSchemaKey,
                            ParameterValueKind::Float64, true) &&
               hasParameter(definition, 5, kBlendModeParameterRole, kBlendModeParameterSchemaKey,
                            ParameterValueKind::Integer) &&
               // Schema 5 (ADR 0024): the optional, animatable Time Remap comes last.
               hasParameter(definition, 6, kTimeRemapParameterRole, kTimeRemapParameterSchemaKey,
                            ParameterValueKind::Float64, true, false) &&
               (definition.inputs.size() == 9 && definition.outputs.size() == 2 &&
                definition.inputs[1].name == kLayerOutputAudioInputPort &&
                definition.inputs[1].valueKind == SocketValueKind::Audio &&
                !definition.inputs[1].required &&
                hasOutput(definition, kLayerOutputAudioOutputPort, SocketValueKind::Audio) &&
                hasParameterSockets(definition, 2)) &&
               !definition.layerSlotInput.has_value();
    case NodeLoweringKind::LayerStack:
        return hasCanonicalKey(definition, kLayerStackNodeType, kLayerStackNodeSchemaVersion) &&
               definition.cardinality == NodeCardinality::Many && definition.inputs.empty() &&
               hasImageOutput(definition, kLayerStackOutputPort) && definition.parameters.empty() &&
               definition.layerSlotInput.has_value() &&
               definition.layerSlotInput->role == kLayerStackContentInputRole &&
               definition.layerSlotInput->valueKind == SocketValueKind::Image &&
               definition.layerSlotInput->requiredPerSlot &&
               (definition.outputs.size() == 2 &&
                hasOutput(definition, kLayerStackAudioOutputPort, SocketValueKind::Audio) &&
                definition.audioLayerSlotInput.has_value() &&
                definition.audioLayerSlotInput->role == kLayerStackAudioInputRole &&
                definition.audioLayerSlotInput->valueKind == SocketValueKind::Audio &&
                definition.audioLayerSlotInput->requiredPerSlot);
    case NodeLoweringKind::CompositionOutput:
        return hasCanonicalKey(definition, kCompositionOutputNodeType,
                               kCompositionOutputNodeSchemaVersion) &&
               definition.cardinality == NodeCardinality::OnePerComposition &&
               // The image input is OPTIONAL, mirroring the Layer Output content port (task FIX1,
               // item B): a disconnected Composition Output is an ordinary empty composition that
               // renders transparent, not a topology failure. The lowering owns that empty path.
               hasLeadingImageInput(definition, kCompositionOutputInputPort, false) &&
               definition.inputs.size() == 3 &&
               definition.inputs[1].name == kCompositionOutputAudioInputPort &&
               definition.inputs[1].valueKind == SocketValueKind::Audio &&
               !definition.inputs[1].required &&
               // ADR 0026 §7: optional channel layers written beside RGBA by layered OpenEXR.
               definition.inputs[2].name == kCompositionOutputChannelsInputPort &&
               definition.inputs[2].valueKind == SocketValueKind::ChannelSet &&
               !definition.inputs[2].required && definition.parameters.size() == 1 &&
               hasParameter(definition, 0, kCompositionOutputLayersParameterRole,
                            kCompositionOutputLayersParameterSchemaKey, ParameterValueKind::String,
                            false, false) &&
               // Task FIX1, item H: a SINK. It declares no output port at all, because nothing may
               // connect from the end of the composition -- the compiler never followed such an
               // edge, and a socket that leads nowhere is an invitation to draw a wire that means
               // nothing.
               definition.outputs.empty() && !definition.layerSlotInput.has_value();
    case NodeLoweringKind::Unsupported:
        return true;
    // The value lowerings share ONE shape contract rather than fifteen bespoke ones; see
    // value_node_definitions.cpp for what it requires and why the asymmetry with the five image
    // lowerings above is deliberate.
    case NodeLoweringKind::ValueConstant:
    case NodeLoweringKind::ValueTime:
    case NodeLoweringKind::ValueScalarMath:
    case NodeLoweringKind::ValueVectorMath:
    case NodeLoweringKind::ValueVectorReduce:
    case NodeLoweringKind::ValueMapRange:
    case NodeLoweringKind::ValueClamp:
    case NodeLoweringKind::ValueMix:
    case NodeLoweringKind::ValueColorMix:
    case NodeLoweringKind::ValueCompare:
    case NodeLoweringKind::ValueSwitch:
    case NodeLoweringKind::ValueSeparate:
    case NodeLoweringKind::ValueCombine:
    case NodeLoweringKind::ValueRandom:
    case NodeLoweringKind::ValueReroute:
    case NodeLoweringKind::ValueBoundsReadout:
    case NodeLoweringKind::ValueUtility:
        return bloom::document::detail::hasValidValueLoweringShape(definition);
    }
    return false;
}

[[nodiscard]] bool isValidDefinition(const NodeDefinition& definition) {
    using namespace bloom::document;
    // #383: an effect mask is valid exactly on an effect that takes one, after which the rest of
    // the definition is checked as the effect's own shape.
    if (hasEffectMask(definition)) {
        const auto effect = withoutEffectMask(definition);
        return takesEffectMask(effect) && isValidDefinition(effect);
    }
    if (definition.key.typeId.empty() || definition.key.schemaVersion == 0 ||
        !hasUniqueNonEmptyNames(definition.inputs) || !hasUniqueNonEmptyNames(definition.outputs) ||
        !hasValidParameters(definition.parameters)) {
        return false;
    }
    if (definition.layerSlotInput.has_value() && definition.layerSlotInput->role.empty()) {
        return false;
    }
    return hasValidLoweringShape(definition);
}

[[nodiscard]] NodeDefinition solidDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{
        {std::string(kSolidSourceNodeType), kSolidSourceNodeSchemaVersion},
        NodeLoweringKind::Solid,
        // Task S7, item 3: the colour parameter is a linkable socket as well as an inline chip.
        // Optional, because the parameter IS the value when nothing is connected -- an
        // unconnected operand is an authored constant, not a missing input.
        {{std::string(kSolidColorParameterRole), SocketValueKind::Color, false}},
        {{std::string(kSolidSourceOutputPort), SocketValueKind::Image}},
        // Task S5, item 1: supportsAnimation is true now. It is NOT a second opinion about what
        // is animatable -- the shape check below asserts it agrees with
        // document::isColor4AnimatableSchemaKey(), so the schema predicates stay the single
        // authority and a registered definition cannot drift from them.
        {{std::string(kSolidColorParameterRole), std::string(kSolidColorParameterSchemaKey),
          ParameterValueKind::Color4d, true, true, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0}}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Sources};
    definition.inputs.push_back(
        {std::string(kSolidWidthParameterRole), SocketValueKind::Scalar, false});
    definition.inputs.push_back(
        {std::string(kSolidHeightParameterRole), SocketValueKind::Scalar, false});
    definition.parameters.push_back({std::string(kSolidWidthParameterRole),
                                     std::string(kSolidWidthParameterSchemaKey),
                                     ParameterValueKind::Float64, true, true, 1.0});
    definition.parameters.push_back({std::string(kSolidHeightParameterRole),
                                     std::string(kSolidHeightParameterSchemaKey),
                                     ParameterValueKind::Float64, true, true, 1.0});
    return definition;
}

// #502 Adjustment: no inputs and no parameters; one Image output, the stack below the Merge entry
// its chain lands in.
[[nodiscard]] NodeDefinition adjustmentDefinition() {
    using namespace bloom::document;
    return {{std::string(kAdjustmentNodeType), kAdjustmentNodeSchemaVersion},
            NodeLoweringKind::Adjustment,
            {},
            {{std::string(kAdjustmentOutputPort), SocketValueKind::Image}},
            {},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Layers};
}

[[nodiscard]] NodeDefinition checkerDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{
        {std::string(kCheckerSourceNodeType), kCheckerSourceNodeSchemaVersion},
        NodeLoweringKind::Checker,
        {},
        {{"image", SocketValueKind::Image}},
        {{"colorA", std::string(kSolidColorParameterSchemaKey), ParameterValueKind::Color4d, true,
          true, bloom::core::Color4d{0.18, 0.18, 0.18, 1.0}},
         {"colorB", std::string(kSolidColorParameterSchemaKey), ParameterValueKind::Color4d, true,
          true, bloom::core::Color4d{0.5, 0.5, 0.5, 1.0}},
         {"cellWidth", std::string(kCheckerCellWidthParameterSchemaKey),
          ParameterValueKind::Integer, true, false, std::int64_t{64}},
         {"cellHeight", std::string(kCheckerCellHeightParameterSchemaKey),
          ParameterValueKind::Integer, true, false, std::int64_t{64}},
         {"width", std::string(kCheckerWidthParameterSchemaKey), ParameterValueKind::Integer, true,
          false, std::int64_t{1920}},
         {"height", std::string(kCheckerHeightParameterSchemaKey), ParameterValueKind::Integer,
          true, false, std::int64_t{1080}}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Sources};
    for (const auto& parameter : std::span(definition.parameters).first(2))
        definition.inputs.push_back({parameter.role,
                                     parameter.valueKind == ParameterValueKind::Color4d
                                         ? SocketValueKind::Color
                                         : SocketValueKind::Scalar,
                                     false});
    return definition;
}

[[nodiscard]] NodeDefinition gradientDefinition(const std::string_view type) {
    using namespace bloom::document;
    return {{std::string(type), kGradientNodeSchemaVersion},
            NodeLoweringKind::Gradient,
            {},
            {{"image", SocketValueKind::Image}},
            {{"gradient", std::string(kGradientStopsParameterSchemaKey),
              ParameterValueKind::Gradient, true, true, defaultGeneratorGradient()},
             {"start", std::string(kGradientStartParameterSchemaKey), ParameterValueKind::Vec2d,
              true, true, Vec2d{0, 0}},
             {"end", std::string(kGradientEndParameterSchemaKey), ParameterValueKind::Vec2d, true,
              true, Vec2d{1, 0}},
             {"width", std::string(kSolidWidthParameterSchemaKey), ParameterValueKind::Float64,
              true, true, 1.0},
             {"height", std::string(kSolidHeightParameterSchemaKey), ParameterValueKind::Float64,
              true, true, 1.0},
             {"repeat", std::string(kGradientRepeatParameterSchemaKey), ParameterValueKind::Integer,
              true, false, std::int64_t{0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Sources};
}

[[nodiscard]] NodeDefinition layerOutputDefinition() {
    using namespace bloom::document;
    std::vector<InputPortDefinition> inputs{
        {std::string(kLayerOutputContentInputPort), SocketValueKind::Image, false}};
    inputs.push_back({std::string(kLayerOutputAudioInputPort), SocketValueKind::Audio, false});
    for (const auto& parameter : std::array<InputPortDefinition, 7>{
             InputPortDefinition{std::string(kPositionParameterRole), SocketValueKind::Vector2,
                                 false},
             InputPortDefinition{std::string(kAnchorParameterRole), SocketValueKind::Vector2,
                                 false},
             InputPortDefinition{std::string(kScaleParameterRole), SocketValueKind::Vector2, false},
             InputPortDefinition{std::string(kRotationParameterRole), SocketValueKind::Scalar,
                                 false},
             InputPortDefinition{std::string(kOpacityParameterRole), SocketValueKind::Scalar,
                                 false},
             InputPortDefinition{std::string(kBlendModeParameterRole), SocketValueKind::Integer,
                                 false},
             InputPortDefinition{std::string(kTimeRemapParameterRole), SocketValueKind::Scalar,
                                 false}})
        inputs.push_back(parameter);
    std::vector<OutputPortDefinition> outputs{
        {std::string(kLayerOutputOutputPort), SocketValueKind::Image}};
    outputs.push_back({std::string(kLayerOutputAudioOutputPort), SocketValueKind::Audio});
    return {{std::string(kLayerOutputNodeType), kLayerOutputNodeSchemaVersion},
            NodeLoweringKind::LayerOutput,
            std::move(inputs),
            std::move(outputs),
            {{std::string(kPositionParameterRole), std::string(kPositionParameterSchemaKey),
              ParameterValueKind::Vec2d, true, true, Vec2d{}},
             {std::string(kAnchorParameterRole), std::string(kAnchorParameterSchemaKey),
              ParameterValueKind::Vec2d, true, true, kDefaultAnchor},
             {std::string(kScaleParameterRole), std::string(kScaleParameterSchemaKey),
              ParameterValueKind::Vec2d, true, true, kDefaultScale},
             {std::string(kRotationParameterRole), std::string(kRotationParameterSchemaKey),
              ParameterValueKind::Float64, true, true, kDefaultRotationDegrees},
             {std::string(kOpacityParameterRole), std::string(kOpacityParameterSchemaKey),
              ParameterValueKind::Float64, true, true, 1.0},
             // supportsAnimation is false, and deliberately so: there is no meaningful value
             // between Multiply and Screen, so a curve over this parameter could only hold or jump,
             // which an enable/disable keyframe model would express and an interpolated curve
             // would not.
             {std::string(kBlendModeParameterRole), std::string(kBlendModeParameterSchemaKey),
              ParameterValueKind::Integer, true, false, kDefaultBlendModeValue},
             // ADR 0024: absent means Time Remap is disabled. EnableLayerTimeRemap binds it.
             {std::string(kTimeRemapParameterRole), std::string(kTimeRemapParameterSchemaKey),
              ParameterValueKind::Float64, false, true, 0.0}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Layers,
            std::nullopt};
}

[[nodiscard]] NodeDefinition layerStackDefinition() {
    using namespace bloom::document;
    std::vector<OutputPortDefinition> outputs{
        {std::string(kLayerStackOutputPort), SocketValueKind::Image}};
    outputs.push_back({std::string(kLayerStackAudioOutputPort), SocketValueKind::Audio});
    return {{std::string(kLayerStackNodeType), kLayerStackNodeSchemaVersion},
            NodeLoweringKind::LayerStack,
            {},
            std::move(outputs),
            {},
            LayerSlotInputDefinition{std::string(kLayerStackContentInputRole),
                                     SocketValueKind::Image, true},
            NodeCardinality::Many,
            NodeCategory::Compositing,
            LayerSlotInputDefinition{std::string(kLayerStackAudioInputRole), SocketValueKind::Audio,
                                     true}};
}

[[nodiscard]] NodeDefinition compositionOutputDefinition() {
    using namespace bloom::document;
    return {{std::string(kCompositionOutputNodeType), kCompositionOutputNodeSchemaVersion},
            NodeLoweringKind::CompositionOutput,
            // The image input is OPTIONAL, matching the Layer Output content port: a disconnected
            // output is an empty composition that draws nothing, not a failed compile.
            {{std::string(kCompositionOutputInputPort), SocketValueKind::Image, false},
             {std::string(kCompositionOutputAudioInputPort), SocketValueKind::Audio, false},
             {std::string(kCompositionOutputChannelsInputPort), SocketValueKind::ChannelSet,
              false}},
            {},
            // Comma-separated channel layer names written beside RGBA; empty writes every layer of
            // the connected channel set.
            {{std::string(kCompositionOutputLayersParameterRole),
              std::string(kCompositionOutputLayersParameterSchemaKey), ParameterValueKind::String,
              false, false, std::string{}}},
            std::nullopt,
            NodeCardinality::OnePerComposition,
            NodeCategory::Output};
}

[[nodiscard]] NodeDefinition videoDefinition() {
    using namespace bloom::document;
    return {{"bloom.video-source", 1},
            NodeLoweringKind::VideoSource,
            {},
            {{"image", SocketValueKind::Image},
             {"audio", SocketValueKind::Audio},
             {std::string(kVideoSourceChannelsOutputPort), SocketValueKind::ChannelSet}},
            {{"asset", "bloom.video.asset", ParameterValueKind::String, true, false, std::string{}},
             {"startFrame", "bloom.video.start-frame", ParameterValueKind::Integer, true, false,
              std::int64_t{0}},
             {"loopMode", "bloom.video.loop-mode", ParameterValueKind::Integer, true, false,
              std::int64_t{0}},
             {"colorSpace", "bloom.video.color-space", ParameterValueKind::Integer, true, false,
              std::int64_t{0}},
             {std::string(kVideoInputColorSpaceIdParameterRole),
              std::string(kVideoInputColorSpaceIdParameterSchemaKey), ParameterValueKind::String,
              false, false, std::string{}},
             {std::string(kVideoAudioStreamParameterRole),
              std::string(kVideoAudioStreamParameterSchemaKey), ParameterValueKind::Integer, false,
              false, std::int64_t{0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Sources};
}

[[nodiscard]] NodeDefinition fileTransformDefinition() {
    return {
        {"bloom.ocio-file-transform", 1},
        NodeLoweringKind::ImageEffect,
        {{"input", SocketValueKind::Image}},
        {{"image", SocketValueKind::Image}},
        {{"lut", "bloom.ocio-file.lut", ParameterValueKind::Integer, true, false, std::int64_t{0}},
         {"interpolation", "bloom.ocio-file.interpolation", ParameterValueKind::Integer, true,
          false, std::int64_t{0}},
         {"direction", "bloom.ocio-file.direction", ParameterValueKind::Integer, true, false,
          std::int64_t{0}},
         {"processSpace", "bloom.ocio-file.process-space", ParameterValueKind::String, true, false,
          std::string{}},
         {"bypass", "bloom.ocio-effect.bypass", ParameterValueKind::Boolean, true, false, false},
         {"look", "bloom.ocio-effect.look", ParameterValueKind::Boolean, true, false, false}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Color};
}

[[nodiscard]] NodeDefinition resizeFitDefinition(const bool fit) {
    NodeDefinition definition{
        {fit ? "bloom.fit" : "bloom.resize", 1},
        NodeLoweringKind::ImageEffect,
        {{"input", SocketValueKind::Image}},
        {{"image", SocketValueKind::Image}},
        {{"width", "bloom.resize-fit.width", ParameterValueKind::Integer, true, false,
          std::int64_t{1920}},
         {"height", "bloom.resize-fit.height", ParameterValueKind::Integer, true, false,
          std::int64_t{1080}},
         {"pixelAspectNumerator", "bloom.resize-fit.pixel-aspect-numerator",
          ParameterValueKind::Integer, true, false, std::int64_t{1}},
         {"pixelAspectDenominator", "bloom.resize-fit.pixel-aspect-denominator",
          ParameterValueKind::Integer, true, false, std::int64_t{1}},
         {"filter", "bloom.resize-fit.filter", ParameterValueKind::Integer, true, false,
          std::int64_t{0}},
         {"bypass", "bloom.resize-fit.bypass", ParameterValueKind::Boolean, true, false, false}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Transform};
    if (fit)
        definition.parameters.push_back({"policy", "bloom.resize-fit.policy",
                                         ParameterValueKind::Integer, true, false,
                                         std::int64_t{0}});
    return definition;
}

// #336 standalone Transform 2D. The four transform values are animatable and each has an operand
// socket; extent and filter are inline selectors with no socket (they choose the output window
// and resampler at compile time).
// #484: then its optional motion blur (transformMotionBlurParameterDefinitions), constants with no
// socket.
[[nodiscard]] NodeDefinition transform2DDefinition() {
    using bloom::document::Vec2d;
    NodeDefinition definition{{"bloom.transform-2d", 1},
            NodeLoweringKind::ImageEffect,
            {{"input", SocketValueKind::Image},
             {"translate", SocketValueKind::Vector2, false},
             {"pivot", SocketValueKind::Vector2, false},
             {"scale", SocketValueKind::Vector2, false},
             {"rotation", SocketValueKind::Scalar, false}},
            {{"image", SocketValueKind::Image}},
            {{"translate", "bloom.transform-2d.translate", ParameterValueKind::Vec2d, true, true,
              Vec2d{}},
             {"pivot", "bloom.transform-2d.pivot", ParameterValueKind::Vec2d, true, true, Vec2d{}},
             {"scale", "bloom.transform-2d.scale", ParameterValueKind::Vec2d, true, true,
              Vec2d{1.0, 1.0}},
             {"rotation", "bloom.transform-2d.rotation", ParameterValueKind::Float64, true, true,
              0.0},
             {"extent", "bloom.transform-2d.extent", ParameterValueKind::Integer, true, false,
              std::int64_t{0}},
             {"filter", "bloom.transform-2d.filter", ParameterValueKind::Integer, true, false,
              std::int64_t{0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Transform};
    for (auto& parameter : bloom::document::transformMotionBlurParameterDefinitions())
        definition.parameters.push_back(std::move(parameter));
    return definition;
}

[[nodiscard]] NodeDefinition cstDefinition() {
    return {
        {"bloom.ocio-colour-space-transform", 1},
        NodeLoweringKind::ImageEffect,
        {{"input", SocketValueKind::Image}},
        {{"image", SocketValueKind::Image}},
        {{"from", "bloom.ocio-cst.from", ParameterValueKind::String, true, false, std::string{}},
         {"to", "bloom.ocio-cst.to", ParameterValueKind::String, true, false, std::string{}},
         {"bypass", "bloom.ocio-effect.bypass", ParameterValueKind::Boolean, true, false, false}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Color};
}

[[nodiscard]] NodeDefinition pointwiseImageDefinition(const char* typeId, const char* parameterKey,
                                                      const double neutral) {
    NodeDefinition definition{{typeId, 1},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Grade};
    if (parameterKey != nullptr) {
        definition.parameters.push_back(
            {"amount", parameterKey, ParameterValueKind::Float64, true, true, neutral});
        definition.inputs.push_back({"amount", SocketValueKind::Scalar, false});
    }
    return definition;
}

[[nodiscard]] NodeDefinition imageCombineDefinition(const bool mix) {
    return {
        {mix ? "bloom.image-mix" : "bloom.set-alpha", 1},
        NodeLoweringKind::ImageEffect,
        mix ? std::vector<InputPortDefinition>{{"input", SocketValueKind::Image},
                                               {"second", SocketValueKind::Image, true},
                                               // ADR 0028: optional per-pixel factor scale.
                                               {"mask", SocketValueKind::Mask, false},
                                               {"factor", SocketValueKind::Scalar, false}}
            : std::vector<InputPortDefinition>{
                  {"input", SocketValueKind::Image},
                  // An unconnected matte is a transparent matte (zero coverage), never a compile
                  // failure.
                  {"matte", SocketValueKind::Image, false},
                  {"channel", SocketValueKind::Integer, false}},
        {{"image", SocketValueKind::Image}},
        {{mix ? "factor" : "channel", mix ? "bloom.image-mix.factor" : "bloom.set-alpha.channel",
          mix ? ParameterValueKind::Float64 : ParameterValueKind::Integer, true, mix,
          mix ? bloom::document::ParameterValue{1.0}
              : bloom::document::ParameterValue{std::int64_t{0}}}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Compositing};
}

[[nodiscard]] NodeDefinition canvasImageDefinition(const char* typeId) {
    NodeDefinition definition{{typeId, 1},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Transform};
    for (const char* side : {"left", "top", "right", "bottom"}) {
        definition.parameters.push_back(
            {side, std::string(typeId) + "." + side, ParameterValueKind::Float64, true, true, 0.0});
        definition.inputs.push_back({side, SocketValueKind::Scalar, false});
    }
    return definition;
}

[[nodiscard]] NodeDefinition multiplyAlphaDefinition() {
    return {
        {"bloom.multiply-alpha", 1},
        NodeLoweringKind::ImageEffect,
        {{"input", SocketValueKind::Image},
         {"matte", SocketValueKind::Image, false},
         {"factor", SocketValueKind::Scalar, false},
         {"channel", SocketValueKind::Integer, false}},
        {{"image", SocketValueKind::Image}},
        {{"factor", "bloom.multiply-alpha.factor", ParameterValueKind::Float64, true, true, 1.0},
         {"channel", "bloom.multiply-alpha.channel", ParameterValueKind::Integer, true, false,
          std::int64_t{0}}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Compositing};
}

[[nodiscard]] NodeDefinition discreteImageDefinition(const bool rotate) {
    NodeDefinition definition{{rotate ? "bloom.rotate-90" : "bloom.flip", 1},
                              NodeLoweringKind::ImageEffect,
                              {{"input", SocketValueKind::Image}},
                              {{"image", SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Transform};
    if (rotate) {
        definition.parameters.push_back({"quarterTurns", "bloom.rotate-90.quarter-turns",
                                         ParameterValueKind::Integer, true, false,
                                         std::int64_t{1}});
    } else {
        definition.parameters.push_back({"horizontal", "bloom.flip.horizontal",
                                         ParameterValueKind::Boolean, true, false, true});
        definition.parameters.push_back(
            {"vertical", "bloom.flip.vertical", ParameterValueKind::Boolean, true, false, false});
    }
    return definition;
}

[[nodiscard]] NodeDefinition blurDefinition(const char* typeId) {
    return {
        {typeId, 1},
        NodeLoweringKind::ImageEffect,
        {{"input", SocketValueKind::Image}, {"radius", SocketValueKind::Scalar, false}},
        {{"image", SocketValueKind::Image}},
        {{"radius", "bloom.blur.radius", ParameterValueKind::Float64, true, true, 0.0},
         {"edge", "bloom.blur.edge", ParameterValueKind::Integer, true, false, std::int64_t{0}}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Filters};
}

// #337 Matte Blur: the Gaussian Blur controls over Mask coverage. The input is required, like
// every image effect's; an empty upstream Mask propagates as an empty Mask. #338 Matte Erode and
// Matte Dilate share the same Mask sockets and radius/edge controls.
[[nodiscard]] NodeDefinition matteFilterDefinition(const char* typeId) {
    return {
        {typeId, 1},
        NodeLoweringKind::ImageEffect,
        {{"input", SocketValueKind::Mask}, {"radius", SocketValueKind::Scalar, false}},
        {{"mask", SocketValueKind::Mask}},
        {{"radius", "bloom.blur.radius", ParameterValueKind::Float64, true, true, 0.0},
         {"edge", "bloom.blur.edge", ParameterValueKind::Integer, true, false, std::int64_t{0}}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Filters};
}

// #338 Luma Key: animatable/drivable low and high luma thresholds and a constant invert.
[[nodiscard]] NodeDefinition lumaKeyDefinition() {
    return {{"bloom.luma-key", 1},
            NodeLoweringKind::ImageEffect,
            {{"input", SocketValueKind::Image},
             {"low", SocketValueKind::Scalar, false},
             {"high", SocketValueKind::Scalar, false}},
            {{"mask", SocketValueKind::Mask}},
            {{"low", "bloom.luma-key.low", ParameterValueKind::Float64, true, true, 0.0},
             {"high", "bloom.luma-key.high", ParameterValueKind::Float64, true, true, 1.0},
             {"invert", "bloom.luma-key.invert", ParameterValueKind::Boolean, true, false, false}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Compositing};
}

[[nodiscard]] NodeDefinition imageDefinition() {
    using namespace bloom::document;
    return {
        {std::string(kImageSourceNodeType), kImageSourceNodeSchemaVersion},
        NodeLoweringKind::ImageSource,
        {},
        {{"image", SocketValueKind::Image},
         {std::string(kImageSourceChannelsOutputPort), SocketValueKind::ChannelSet}},
        {{"asset", "bloom.image.asset", ParameterValueKind::String, true, false, std::string{}},
         {"startFrame", "bloom.image.start-frame", ParameterValueKind::Integer, true, false,
          std::int64_t{0}},
         {"loopMode", "bloom.image.loop-mode", ParameterValueKind::Integer, true, false,
          std::int64_t{0}},
         {"colorSpace", "bloom.image.color-space", ParameterValueKind::Integer, true, false,
          std::int64_t{0}},
         {"premultiply", "bloom.image.premultiply", ParameterValueKind::Boolean, true, false, true},
         {std::string(kImageInputColorSpaceIdParameterRole),
          std::string(kImageInputColorSpaceIdParameterSchemaKey), ParameterValueKind::String, false,
          false, std::string{}},
         // ADR 0026: the EXR part (empty: first) and colour layer (empty: root).
         {std::string(kImagePartParameterRole), std::string(kImagePartParameterSchemaKey),
          ParameterValueKind::String, false, false, std::string{}},
         {std::string(kImageLayerParameterRole), std::string(kImageLayerParameterSchemaKey),
          ParameterValueKind::String, false, false, std::string{}}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Sources};
}

[[nodiscard]] NodeDefinition compositionSourceDefinition() {
    using namespace bloom::document;
    return {{std::string(kCompositionSourceNodeType), kCompositionSourceNodeSchemaVersion},
            NodeLoweringKind::CompositionSource,
            {},
            {{"image", SocketValueKind::Image},
             {"audio", SocketValueKind::Audio},
             {std::string(kCompositionSourceChannelsOutputPort), SocketValueKind::ChannelSet}},
            {{"composition", "bloom.composition-source.composition", ParameterValueKind::Integer,
              true, false, std::int64_t{0}},
             {"timeOffset", "bloom.composition-source.time-offset", ParameterValueKind::Float64,
              true, true, 0.0},
             {"timeScale", "bloom.composition-source.time-scale", ParameterValueKind::Float64, true,
              true, 1.0},
             {"loopMode", "bloom.composition-source.loop-mode", ParameterValueKind::Integer, true,
              false, std::int64_t{0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Sources};
}

[[nodiscard]] NodeDefinition audioDefinition() {
    using namespace bloom::document;
    return {{std::string(kAudioSourceNodeType), kAudioSourceNodeSchemaVersion},
            NodeLoweringKind::AudioSource,
            {},
            {{std::string(kAudioSourceOutputPort), SocketValueKind::Audio}},
            {{"asset", "bloom.audio.asset", ParameterValueKind::String, true, false, std::string{}},
             {"startFrame", "bloom.audio.start-frame", ParameterValueKind::Integer, true, false,
              std::int64_t{0}},
             {"level", std::string(kAudioLevelParameterSchemaKey), ParameterValueKind::Float64,
              true, true, 1.0}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Sources,
            std::nullopt};
}

[[nodiscard]] NodeDefinition textDefinition() {
    using namespace bloom::document;
    NodeDefinition definition{
        {std::string(kTextSourceNodeType), kTextSourceNodeSchemaVersion},
        NodeLoweringKind::Text,
        // One operand socket per parameter, in the same order (task S7, item 3). The content
        // port is a String socket: a text layer whose words come from a String node is the
        // whole point of having a String kind at all.
        {{std::string(kTextParameterRole), SocketValueKind::String, false},
         {std::string(kTextSizeParameterRole), SocketValueKind::Scalar, false},
         {std::string(kTextColorParameterRole), SocketValueKind::Color, false}},
        {{std::string(kTextSourceOutputPort), SocketValueKind::Image}},
        // Content stays constant-only (a String has no interpolation); size and colour became
        // animatable in task S5, which the shape check below cross-checks against the schema
        // predicates rather than restating.
        {{std::string(kTextParameterRole), std::string(kTextParameterSchemaKey),
          ParameterValueKind::String, true, false, std::string{}},
         {std::string(kTextSizeParameterRole), std::string(kTextSizeParameterSchemaKey),
          ParameterValueKind::Float64, true, true, kDefaultTextSizePixels},
         {std::string(kTextColorParameterRole), std::string(kTextColorParameterSchemaKey),
          ParameterValueKind::Color4d, true, true, bloom::core::Color4d{1.0, 1.0, 1.0, 1.0}}},
        std::nullopt,
        NodeCardinality::Many,
        NodeCategory::Sources};
    definition.inputs.push_back(
        {std::string(kTextAlignmentParameterRole), SocketValueKind::Integer, false});
    definition.inputs.push_back(
        {std::string(kTextLineHeightParameterRole), SocketValueKind::Scalar, false});
    definition.inputs.push_back(
        {std::string(kTextLetterSpacingParameterRole), SocketValueKind::Scalar, false});
    definition.inputs.push_back(
        {std::string(kTextFontParameterRole), SocketValueKind::String, false});
    definition.inputs.push_back(
        {std::string(kTextBoxParameterRole), SocketValueKind::Vector2, false});
    definition.inputs.push_back(
        {std::string(kTextWrapParameterRole), SocketValueKind::Boolean, false});
    definition.inputs.push_back(
        {std::string(kTextVerticalAlignmentParameterRole), SocketValueKind::Integer, false});
    definition.inputs.push_back(
        {std::string(kTextAnchorModeParameterRole), SocketValueKind::Integer, false});
    definition.inputs.push_back(
        {std::string(kTextOverflowParameterRole), SocketValueKind::Integer, false});
    definition.parameters.push_back({std::string(kTextAlignmentParameterRole),
                                     std::string(kTextAlignmentParameterSchemaKey),
                                     ParameterValueKind::Integer, true, false, std::int64_t{0}});
    definition.parameters.push_back({std::string(kTextLineHeightParameterRole),
                                     std::string(kTextLineHeightParameterSchemaKey),
                                     ParameterValueKind::Float64, true, true, 1.0});
    definition.parameters.push_back({std::string(kTextLetterSpacingParameterRole),
                                     std::string(kTextLetterSpacingParameterSchemaKey),
                                     ParameterValueKind::Float64, true, true, 0.0});
    definition.parameters.push_back(
        {std::string(kTextFontParameterRole), std::string(kTextFontParameterSchemaKey),
         ParameterValueKind::String, false, false, std::string(kDefaultTextFontReference)});
    definition.parameters.push_back({std::string(kTextBoxParameterRole),
                                     std::string(kTextBoxParameterSchemaKey),
                                     ParameterValueKind::Vec2d, false, false, Vec2d{}});
    definition.parameters.push_back({std::string(kTextWrapParameterRole),
                                     std::string(kTextWrapParameterSchemaKey),
                                     ParameterValueKind::Boolean, false, false, false});
    definition.parameters.push_back({std::string(kTextVerticalAlignmentParameterRole),
                                     std::string(kTextVerticalAlignmentParameterSchemaKey),
                                     ParameterValueKind::Integer, false, false, std::int64_t{0}});
    definition.parameters.push_back({std::string(kTextAnchorModeParameterRole),
                                     std::string(kTextAnchorModeParameterSchemaKey),
                                     ParameterValueKind::Integer, false, false,
                                     kTextAnchorModeFollowsAlignment});
    definition.parameters.push_back({std::string(kTextOverflowParameterRole),
                                     std::string(kTextOverflowParameterSchemaKey),
                                     ParameterValueKind::Integer, false, false, std::int64_t{0}});
    return definition;
}

} // namespace

namespace bloom::document {

NodeRegistrationStatus NodeDefinitionRegistry::registerDefinition(NodeDefinition definition) {
    if (frozen_) {
        return NodeRegistrationStatus::Frozen;
    }
    if (!isValidDefinition(definition)) {
        return NodeRegistrationStatus::InvalidDefinition;
    }
    const auto duplicate = std::ranges::find_if(
        definitions_, [&](const auto& existing) { return existing.key == definition.key; });
    if (duplicate != definitions_.end()) {
        return NodeRegistrationStatus::DuplicateDefinition;
    }
    definitions_.push_back(std::move(definition));
    return NodeRegistrationStatus::Registered;
}

void NodeDefinitionRegistry::freeze() {
    if (frozen_) {
        return;
    }
    std::ranges::sort(definitions_, [](const auto& left, const auto& right) {
        if (left.key.typeId != right.key.typeId) {
            return left.key.typeId < right.key.typeId;
        }
        return left.key.schemaVersion < right.key.schemaVersion;
    });
    frozen_ = true;
}

const NodeDefinition*
NodeDefinitionRegistry::find(const std::string_view typeId,
                             const std::uint32_t schemaVersion) const noexcept {
    const auto match =
        frozen_ ? std::lower_bound(definitions_.begin(), definitions_.end(),
                                   NodeLookupKey{typeId, schemaVersion}, definitionPrecedesKey)
                : std::ranges::find_if(definitions_, [&](const auto& definition) {
                      return definition.key.typeId == typeId &&
                             definition.key.schemaVersion == schemaVersion;
                  });
    if (match == definitions_.end() || match->key.typeId != typeId ||
        match->key.schemaVersion != schemaVersion) {
        return nullptr;
    }
    return &*match;
}

bool NodeDefinitionRegistry::containsType(const std::string_view typeId) const noexcept {
    if (!frozen_) {
        return std::ranges::any_of(
            definitions_, [&](const auto& definition) { return definition.key.typeId == typeId; });
    }
    const auto match = std::lower_bound(
        definitions_.begin(), definitions_.end(), typeId,
        [](const NodeDefinition& definition, const std::string_view candidate) noexcept {
            return std::string_view{definition.key.typeId} < candidate;
        });
    return match != definitions_.end() && match->key.typeId == typeId;
}

bool registerBuiltInNodeDefinitions(NodeDefinitionRegistry& registry) {
    std::vector<NodeDefinition> definitions{
        shapeDefinition(),
        maskDefinition(),
        solidDefinition(),
        checkerDefinition(),
        gradientDefinition(kLinearGradientNodeType),
        gradientDefinition(kRadialGradientNodeType),
        layerOutputDefinition(),
        adjustmentDefinition(),
        layerStackDefinition(),
        compositionOutputDefinition(),
        imagePreviewDefinition(),
        textDefinition(),
        imageDefinition(),
        cstDefinition(),
        fileTransformDefinition(),
        pointwiseImageDefinition("bloom.exposure", "bloom.exposure.stops", 0.0),
        pointwiseImageDefinition("bloom.offset", "bloom.offset.amount", 0.0),
        pointwiseImageDefinition("bloom.multiply", "bloom.multiply.factor", 1.0),
        pointwiseImageDefinition("bloom.invert", nullptr, 0.0),
        imageCombineDefinition(true),
        imageCombineDefinition(false),
        canvasImageDefinition("bloom.crop"),
        canvasImageDefinition("bloom.pad"),
        multiplyAlphaDefinition(),
        alphaAssociationDefinition(),
        discreteImageDefinition(false),
        discreteImageDefinition(true),
        resizeFitDefinition(false),
        resizeFitDefinition(true),
        transform2DDefinition(),
        blurDefinition("bloom.gaussian-blur"),
        blurDefinition("bloom.box-blur"),
        matteFilterDefinition("bloom.matte-blur"),
        matteFilterDefinition("bloom.matte-erode"),
        matteFilterDefinition("bloom.matte-dilate"),
        lumaKeyDefinition(),
        levelsDefinition(),
        curvesDefinition(),
        hueSaturationDefinition(),
        gradeDefinition(),
        colorizeDefinition(),
        vignetteDefinition(),
        filmGrainDefinition(),
        codecDegradeDefinition(),
        autoExposureDefinition(),
        audioEffectDefinition(kAudioEqualizerNodeType),
        audioEffectDefinition(kAudioCompressorNodeType),
        audioEffectDefinition(kAudioAutoLevelNodeType),
        audioEffectDefinition(kAudioMonoNodeType),
        audioEffectDefinition(kAudioHissNodeType),
        lensDistortionDefinition(),
        chromaticAberrationDefinition(),
        rollingShutterDefinition(),
        testCardDefinition(),
        lightRaysDefinition(),
        sharpenDefinition(),
        cameraShakeDefinition(),
        depthEffectDefinition(kZDefocusNodeType),
        depthEffectDefinition(kDepthFogNodeType),
        lensFlareDefinition(),
        motionBlurDefinition(kMotionBlurNodeType),
        motionBlurDefinition(kVectorMotionBlurNodeType),
        motionBlurDefinition(kOpticalFlowNodeType),
        motionBlurDefinition(kOpticalFlowMotionBlurNodeType),
        motionBlurDefinition(kEchoNodeType),
        motionBlurDefinition(kFrameRateHoldNodeType),
        motionBlurDefinition(kDirectionalBlurNodeType),
        motionBlurDefinition(kSpinBlurNodeType),
        motionBlurDefinition(kZoomBlurNodeType),
        separateChannelsDefinition(),
        combineChannelsDefinition(),
        recipeDefinition(kDropShadowNodeType),
        recipeDefinition(kGlowNodeType),
        recipeDefinition(kLightWrapNodeType),
        videoDefinition(),
        audioDefinition(),
        compositionSourceDefinition(),
        channelShuffleDefinition(),
        channelCopyDefinition(),
        channelRemoveDefinition(),
        channelPassDefinition(),
        imageToChannelSetDefinition(),
        cryptomatteDefinition(),
        maskCombineDefinition(),
        imageToMaskDefinition()};
    // #383: every grade, colour-management and filter effect takes an optional mask.
    for (auto& definition : definitions)
        if (takesEffectMask(definition))
            appendEffectMask(definition);
    // #525: the Fields family, in menu order.
    for (const auto& field : fieldNodeSpecs())
        definitions.push_back(fieldNodeDefinition(field));
    // The value library is appended, not interleaved: the five above are the structural node types
    // a composition is built out of, and reading them first in one place is what makes the
    // registry's own contract legible.
    for (auto& definition : detail::valueNodeDefinitions()) {
        definitions.push_back(std::move(definition));
    }
    if (registry.isFrozen() || std::ranges::any_of(definitions, [&](const auto& definition) {
            return registry.find(definition.key.typeId, definition.key.schemaVersion) != nullptr;
        })) {
        return false;
    }
    for (auto& definition : definitions) {
        if (registry.registerDefinition(std::move(definition)) !=
            NodeRegistrationStatus::Registered) {
            return false;
        }
    }
    return true;
}

const ParameterDefinition* gradientParameterOfStopPort(const NodeDefinition& definition,
                                                       const std::string_view port) {
    const auto stop = parseGradientStopPort(port);
    if (!stop)
        return nullptr;
    const auto found =
        std::ranges::find(definition.parameters, stop->role, &ParameterDefinition::role);
    return found != definition.parameters.end() && found->valueKind == ParameterValueKind::Gradient
               ? &*found
               : nullptr;
}

std::optional<InputPortDefinition> findInputPort(const NodeDefinition& definition,
                                                 const std::string_view port) {
    const auto declared = std::ranges::find(definition.inputs, port, &InputPortDefinition::name);
    if (declared != definition.inputs.end())
        return *declared;
    if (gradientParameterOfStopPort(definition, port) == nullptr)
        return std::nullopt;
    const auto stop = parseGradientStopPort(port);
    return InputPortDefinition{std::string(port),
                               stop->component == GradientStopComponent::Color
                                   ? SocketValueKind::Color
                                   : SocketValueKind::Scalar,
                               false};
}

bool isSupportedNodeVersion(const std::string_view typeId, const std::uint32_t version) {
    const auto& registry = builtInNodeDefinitions();
    return !typeId.starts_with("bloom.reroute-") &&
           (!registry.containsType(typeId) || registry.find(typeId, version) != nullptr);
}

const NodeDefinitionRegistry& builtInNodeDefinitions() {
    struct BuiltIns final {
        NodeDefinitionRegistry registry;
        BuiltIns() {
            (void)registerBuiltInNodeDefinitions(registry);
            registry.freeze();
        }
    };
    static const BuiltIns builtIns;
    return builtIns.registry;
}

} // namespace bloom::document
