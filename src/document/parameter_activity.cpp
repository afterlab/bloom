#include <bloom/document/parameter_activity.hpp>

#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/denoise_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/resample_path_nodes.hpp>
#include <bloom/document/warp_path_nodes.hpp>
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/document/warp_nodes.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <variant>

namespace bloom::document {
namespace {

// A constant integer or switch of `node` (a switch reads 1 or 0); none when it is not a constant.
[[nodiscard]] std::optional<std::int64_t> constantOf(const Composition& composition,
                                                     const NodeRecord& node,
                                                     const std::string_view role) {
    for (const auto& binding : node.parameters)
        if (binding.role == role) {
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* constant =
                parameter ? std::get_if<ConstantValueSource>(&parameter->source) : nullptr;
            if (const auto* integer = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr)
                return *integer;
            if (const auto* flag = constant ? std::get_if<bool>(&constant->value) : nullptr)
                return *flag ? 1 : 0;
        }
    return std::nullopt;
}

} // namespace

bool isParameterInactive(const Composition& composition, const NodeRecord& node,
                         const std::string_view schemaKey) {
    if (schemaKey == "bloom.blur.radius-y")
        return constantOf(composition, node, "separate").value_or(0) == 0;
    if (schemaKey == "bloom.blur.red-scale" || schemaKey == "bloom.blur.green-scale" ||
        schemaKey == "bloom.blur.blue-scale" || schemaKey == "bloom.blur.alpha-scale")
        return constantOf(composition, node, "perChannel").value_or(0) == 0;
    // A mode that reads only some of a node's values: the others are not lowered (the compiler's
    // lowering of each node is the other half of these rules).
    // #637 Resample Path reads only the value its mode names.
    if (node.typeId == kResamplePathNodeType) {
        const auto mode = static_cast<ResampleMode>(
            constantOf(composition, node, "mode").value_or(selectorStoredValue(ResampleMode::FixedCount)));
        if (schemaKey == kResampleCountSchemaKey)
            return mode != ResampleMode::FixedCount;
        if (schemaKey == kResampleGapSchemaKey)
            return mode != ResampleMode::Gap;
        if (schemaKey == kResampleDensitySchemaKey)
            return mode != ResampleMode::ByLength;
        if (schemaKey == kResamplePercentageSchemaKey)
            return mode != ResampleMode::Percentage;
        return false;
    }
    // #598 Warp Path: only Wave and Ripple travel along a cycle, so only they read the phase.
    if (node.typeId == kWarpPathNodeType) {
        if (schemaKey == kWarpPhaseSchemaKey) {
            const auto function = static_cast<WarpFunction>(
                constantOf(composition, node, "function")
                    .value_or(selectorStoredValue(WarpFunction::Wave)));
            return function != WarpFunction::Wave && function != WarpFunction::Ripple;
        }
        return false;
    }
    if (node.typeId == kVignetteNodeType) {
        const bool exposure = constantOf(composition, node, "mode").value_or(kVignetteMultiply) ==
                              kVignetteExposure;
        if (schemaKey == kVignetteAmountSchemaKey)
            return exposure;
        if (schemaKey == kVignetteExposureSchemaKey)
            return !exposure;
        return false;
    }
    // #511 Tile & Mirror: each mode reads only its own values (the lowering reads the same ones).
    if (node.typeId == kTileMirrorNodeType) {
        constexpr std::string_view prefix = "bloom.tile-mirror.";
        if (!schemaKey.starts_with(prefix))
            return false;
        const auto role = schemaKey.substr(prefix.size());
        const auto mode = constantOf(composition, node, "mode").value_or(kTileMirrorModeTile);
        const bool tile = mode == kTileMirrorModeTile;
        if (role == "edges")
            return mode == kTileMirrorModeWrapOffset;
        if (role == "tileHeight")
            return !tile || constantOf(composition, node, "linkTileSize").value_or(0) != 0;
        if (role == "outputWidth" || role == "outputHeight")
            return !tile || constantOf(composition, node, "clampToOutput").value_or(1) == 0;
        if (role == "tileCenter" || role == "tileWidth" || role == "linkTileSize" ||
            role == "clampToOutput" || role == "mirrorEdges" || role == "horizontalPhase" ||
            role == "verticalPhase")
            return !tile;
        if (role == "offset")
            return mode != kTileMirrorModeWrapOffset;
        if (role == "mirrorPoint" || role == "mirrorAngle" || role == "mirrorSide")
            return mode != kTileMirrorModeMirror;
        if (role == "kaleidoscopeCenter" || role == "kaleidoscopeSegments" ||
            role == "kaleidoscopeAngle" || role == "kaleidoscopeSize")
            return mode != kTileMirrorModeKaleidoscope;
        return false;
    }
    if (node.typeId == kFilmGrainNodeType) {
        const auto* spec = filmGrainParameterSpec(schemaKey);
        if (spec == nullptr || spec->kind != FilmGrainParameterKind::Scalar)
            return false;
        const bool sensor = constantOf(composition, node, "mode").value_or(kFilmGrainFilm) ==
                            kFilmGrainSensorNoise;
        const bool filmOnly =
            spec->role == "shadows" || spec->role == "midtones" || spec->role == "highlights";
        return spec->optional ? !sensor : (filmOnly && sensor);
    }
    if (node.typeId == kColorizeNodeType) {
        const auto mode = constantOf(composition, node, "mode").value_or(kColorizeFill);
        const bool map = mode == kColorizeGradientMap;
        const bool stops = map && std::ranges::any_of(node.parameters, [](const auto& binding) {
                               return binding.role == "gradient";
                           });
        const bool ramp = mode == kColorizeTritone || (map && !stops);
        if (schemaKey == kColorizeColorSchemaKey)
            return mode != kColorizeFill;
        if (schemaKey == kColorizeShadowsSchemaKey || schemaKey == kColorizeHighlightsSchemaKey)
            return mode != kColorizeTint && !ramp;
        if (schemaKey == kColorizeMidtonesSchemaKey || schemaKey == kColorizeMidpointSchemaKey)
            return !ramp;
        if (schemaKey == kColorizeRepetitionsSchemaKey || schemaKey == kColorizePhaseSchemaKey ||
            schemaKey == kColorizeGradientSchemaKey)
            return !map;
        for (const auto weight :
             {kColorizeRedsSchemaKey, kColorizeYellowsSchemaKey, kColorizeGreensSchemaKey,
              kColorizeCyansSchemaKey, kColorizeBluesSchemaKey, kColorizeMagentasSchemaKey})
            if (schemaKey == weight)
                return mode != kColorizeBlackAndWhite;
        return false;
    }
    if (node.typeId != kDenoiseNodeType)
        return false;
    constexpr auto temporal = static_cast<std::int64_t>(DenoiseNodeMode::Temporal);
    const bool automatic = constantOf(composition, node, "autoStrength").value_or(0) != 0;
    if (schemaKey == kDenoiseFramesSchemaKey)
        return constantOf(composition, node, "mode").value_or(0) != temporal;
    // Temporal compares 3 x 3 patches at the same place in each frame: it has no search radius.
    if (schemaKey == kDenoiseRadiusSchemaKey)
        return constantOf(composition, node, "mode").value_or(0) == temporal;
    if (schemaKey == kDenoiseStrengthSchemaKey)
        return automatic;
    if (schemaKey == kDenoiseSampleFromSchemaKey || schemaKey == kDenoiseSampleToSchemaKey)
        return !automatic;
    return false;
}

} // namespace bloom::document
