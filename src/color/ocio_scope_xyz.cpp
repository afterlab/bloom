#include "ocio_scope_xyz.hpp"

#include "ocio_camera_log_inputs.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>

#include <string>

namespace bloom::color::detail {
namespace {

[[nodiscard]] bool hasRole(const OCIO::ConstConfigRcPtr& config, const char* const role) {
    return config->hasRole(role);
}

[[nodiscard]] std::string xyzDestinationName(const OCIO::ConstConfigRcPtr& config) {
    if (hasRole(config, OCIO::ROLE_INTERCHANGE_DISPLAY)) {
        const auto space = config->getColorSpace(OCIO::ROLE_INTERCHANGE_DISPLAY);
        if (space)
            return space->getName();
    }
    return std::string(kScopeXyzColorSpaceName);
}

} // namespace

std::string scopeXyzDefinitionIdentity() {
    return std::string("bloom-scope-xyz-d65-v1;ocio=") + OCIO::GetVersion();
}

OCIO::ConstConfigRcPtr withBloomScopeXyzSpace(const OCIO::ConstConfigRcPtr& config,
                                              const bool isNeutral) {
    if (hasRole(config, OCIO::ROLE_INTERCHANGE_DISPLAY) ||
        config->getColorSpace(std::string(kScopeXyzColorSpaceName).c_str()) != nullptr) {
        return config;
    }
    OCIO::TransformRcPtr toReference;
    if (isNeutral) {
        // The reference is linear Rec.709 (D65): XYZ -> Rec.709 is the inverse of the RP 177
        // matrix.
        auto matrix = OCIO::MatrixTransform::Create();
        const auto rec709ToXyz = rec709ToXyzMatrix();
        matrix->setMatrix(rec709ToXyz.data());
        matrix->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
        toReference = matrix;
    } else if (hasRole(config, OCIO::ROLE_INTERCHANGE_SCENE)) {
        // The reference is ACES2065-1 (AP0, D60): XYZ D65 -> AP0 is the inverse of OCIO's
        // Bradford-adapted built-in.
        auto builtin = OCIO::BuiltinTransform::Create();
        builtin->setStyle("UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD");
        builtin->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
        toReference = builtin;
    } else {
        return config;
    }
    auto space = OCIO::ColorSpace::Create(OCIO::REFERENCE_SPACE_SCENE);
    space->setName(std::string(kScopeXyzColorSpaceName).c_str());
    space->setFamily("Scope");
    space->setDescription("Bloom-synthesized CIE XYZ D65 for the scopes (linear, white Y = 1)");
    space->setTransform(toReference, OCIO::COLORSPACE_DIR_TO_REFERENCE);
    // The pinned config keeps its version; the copy with the added space is validated.
    auto editable = config->createEditableCopy();
    editable->addColorSpace(space);
    editable->validate();
    return editable;
}

ScopeXyzBuild buildScopeXyzProcessor(const OCIO::ConstConfigRcPtr& config, const bool isNeutral,
                                     const std::string_view colorSpaceId,
                                     const OCIO::ConstContextRcPtr& context) noexcept {
    ScopeXyzBuild build;
    try {
        const std::string id(colorSpaceId);
        const auto source = config->getColorSpace(id.c_str());
        if (!source) {
            build.error = ScopeXyzBuildError::MissingSource;
            return build;
        }
        if (source->isData()) {
            build.error = ScopeXyzBuildError::SourceIsData;
            return build;
        }
        const auto withXyz = withBloomScopeXyzSpace(config, isNeutral);
        const auto destination = xyzDestinationName(withXyz);
        if (withXyz->getColorSpace(destination.c_str()) == nullptr) {
            build.error = ScopeXyzBuildError::NoInterchange;
            return build;
        }
        // A scene-referred and a display-referred space meet through the config's interchange
        // roles; a config without them throws here.
        build.processor = withXyz->getProcessor(context, id.c_str(), destination.c_str());
        if (!build.processor)
            build.error = ScopeXyzBuildError::BuildFailed;
    } catch (const std::exception&) {
        build.processor = {};
        build.error = ScopeXyzBuildError::BuildFailed;
    }
    return build;
}

bool isBloomNeutralConfig(const ResolvedBloomNeutralConfig& resolved) noexcept {
    return resolved.expectedRevision() == kBloomNeutralV1ConfigDigest;
}

} // namespace bloom::color::detail
