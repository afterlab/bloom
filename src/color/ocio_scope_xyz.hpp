#pragma once

// Private to bloom_color_ocio and bloom-color-worker: the OCIO transform from a color space to CIE
// XYZ with a D65 white that the scopes measure through (the CIE chromaticity scope and the luma
// coefficients). One definition serves the in-process CPU and GPU arms and the supervised helper,
// so every arm runs the same OCIO processor.

#include "ocio_internal.hpp"

#include <string>
#include <string_view>

namespace bloom::color::detail {

// The scene-referred XYZ D65 space added in memory to a config that names no
// cie_xyz_d65_interchange role.
inline constexpr std::string_view kScopeXyzColorSpaceName = "CIE XYZ D65 (Bloom scope)";

enum class ScopeXyzBuildError : std::uint8_t {
    None,
    MissingSource,
    SourceIsData,
    // The config names neither the CIE XYZ D65 nor the ACES interchange role, and is not Bloom
    // Neutral, so no reference-to-XYZ transform is known.
    NoInterchange,
    BuildFailed,
};

struct ScopeXyzBuild final {
    OCIO::ConstProcessorRcPtr processor;
    ScopeXyzBuildError error = ScopeXyzBuildError::None;
};

// Identity of the synthesized XYZ definition's math: its version and the linked OCIO version
// whose built-ins it uses. Part of every scope XYZ transform identity.
[[nodiscard]] std::string scopeXyzDefinitionIdentity();

// The config with kScopeXyzColorSpaceName added when it names no cie_xyz_d65_interchange role:
//  - Bloom Neutral (reference linear Rec.709, D65): the Rec.709 to XYZ matrix (SMPTE RP 177).
//  - A config with the aces_interchange role (reference ACES2065-1): OCIO's Bradford-adapted
//    "UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD" built-in.
//  - Otherwise unchanged.
// The copy is validated. Throws OCIO::Exception on failure.
[[nodiscard]] OCIO::ConstConfigRcPtr withBloomScopeXyzSpace(const OCIO::ConstConfigRcPtr& config,
                                                            bool isNeutral);

// OCIO's processor from `colorSpaceId` to CIE XYZ D65. For a display color space it removes the
// display encoding: the result is the XYZ of the displayed light. Never throws.
[[nodiscard]] ScopeXyzBuild buildScopeXyzProcessor(const OCIO::ConstConfigRcPtr& config,
                                                   bool isNeutral, std::string_view colorSpaceId,
                                                   const OCIO::ConstContextRcPtr& context) noexcept;

// True for the frozen Bloom Neutral built-in (identified by its content revision), the only
// config whose reference is linear Rec.709.
[[nodiscard]] bool isBloomNeutralConfig(const ResolvedBloomNeutralConfig& resolved) noexcept;

} // namespace bloom::color::detail
