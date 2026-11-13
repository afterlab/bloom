#pragma once

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/core/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The color side of the scopes: OCIO's own transform from a signal's color space to CIE XYZ with a
// D65 white, the colorimetry measured through that same transform, and the CIE 1931 spectral locus
// the CIE chromaticity scope draws. No OCIO type appears here.
//
// The signal's color space is a display color space for the Display signal (the transform removes
// the display encoding, so the XYZ is that of the displayed light) or the working space for the
// Working signal. The config's cie_xyz_d65_interchange and aces_interchange roles carry the
// conversion; a config without the XYZ role gets a scene-referred XYZ D65 space added in an
// in-memory copy (Bloom Neutral: the Rec.709 matrix; a config with aces_interchange: OCIO's
// Bradford-adapted AP0 to XYZ D65 built-in).
//
// The unit primaries are sent as (1,0,0), (0,1,0), (0,0,1) and white as (1,1,1) in the signal's own
// encoding. For a color space whose encoding is a per-channel curve that maps 0 to 0 (sRGB, gamma
// 2.4, PQ and HLG alike) each primary stays on its own channel axis, so its chromaticity is that
// of the linear-light primary; only its luminance scale differs, and the luma coefficients are
// normalized to sum 1, so the result does not depend on that scale.

namespace bloom::color {

enum class ScopeXyzError : std::uint8_t {
    None,
    InvalidColorSpaceId,
    MissingColorSpace,
    ColorSpaceIsData,
    // The config names neither the CIE XYZ D65 nor the ACES interchange role (and is not Bloom
    // Neutral), so no route to XYZ exists.
    NoXyzInterchange,
    TransformBuildFailed,
    CpuProcessorUnavailable,
    UnsupportedFloatingPointEnvironment,
    // The unit primaries or white did not produce a positive, finite XYZ.
    DegenerateColorimetry,
};

struct ScopeChromaticity final {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const ScopeChromaticity&, const ScopeChromaticity&) = default;
};

// What the CIE scope's graticule and the luma coefficients need, all measured through the
// processor: never from a table chosen by name.
struct ScopeColorimetry final {
    // The Y of red, green and blue, normalized to sum 1. Luma = Kr r + Kg g + Kb b.
    std::array<float, 3> luma{};
    ScopeChromaticity red;
    ScopeChromaticity green;
    ScopeChromaticity blue;
    ScopeChromaticity white;
    // The color space's name in the config, as the panel prints it.
    std::string colorSpaceName;
};

class ScopeXyzProcessor;

struct [[nodiscard]] ScopeXyzProcessorResult final {
    std::shared_ptr<const ScopeXyzProcessor> processor;
    ScopeXyzError error = ScopeXyzError::None;

    [[nodiscard]] bool succeeded() const noexcept { return processor != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

// The CPU arm: OCIO's CPU processor (default optimization, binary32) from the signal's color space
// to CIE XYZ D65. Immutable after prepare, holds no global state, and apply may run on any thread
// concurrently.
class ScopeXyzProcessor final {
  public:
    [[nodiscard]] static ScopeXyzProcessorResult prepare(const ResolvedBloomNeutralConfig& config,
                                                         std::string_view colorSpaceId) noexcept;

    // `rgb` holds whole pixels of three floats (r, g, b), `xyz` receives X, Y, Z for each, with
    // the same pixel count. Returns false, leaving `xyz` unspecified, for mismatched sizes, a
    // non-finite input component, a non-finite result or an unsupported floating-point
    // environment; callers drop non-finite pixels before the call.
    [[nodiscard]] bool apply(std::span<const float> rgb, std::span<float> xyz) const noexcept;

    [[nodiscard]] const ScopeColorimetry& colorimetry() const& noexcept { return colorimetry_; }
    [[nodiscard]] const ScopeColorimetry& colorimetry() const&& = delete;
    // The identity of this exact transform: the definition version, the linked OCIO version, the
    // color space and OCIO's own cache id of the processor. Stable across runs and processes; a
    // cache key adds configRevision().
    [[nodiscard]] const std::string& identity() const& noexcept { return identity_; }
    [[nodiscard]] const std::string& identity() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& configRevision() const& noexcept { return revision_; }
    [[nodiscard]] const core::Sha256Digest& configRevision() const&& = delete;
    [[nodiscard]] std::string_view colorSpaceId() const& noexcept { return colorSpaceId_; }
    [[nodiscard]] std::string_view colorSpaceId() const&& = delete;

    ~ScopeXyzProcessor();
    ScopeXyzProcessor(const ScopeXyzProcessor&) = delete;
    ScopeXyzProcessor& operator=(const ScopeXyzProcessor&) = delete;

  private:
    class Impl;
    ScopeXyzProcessor(std::unique_ptr<Impl> impl, core::Sha256Digest revision,
                      std::string colorSpaceId, std::string identity) noexcept;

    std::unique_ptr<Impl> impl_;
    core::Sha256Digest revision_;
    std::string colorSpaceId_;
    std::string identity_;
    ScopeColorimetry colorimetry_;
};

// "0.6400 0.3300": x and y rounded half away from zero to four decimals, plain C++ with no locale
// dependence. A non-finite value prints as "nan".
[[nodiscard]] std::string formatChromaticityXy(ScopeChromaticity value);

// One point of the CIE 1931 spectral locus.
struct SpectralLocusPoint final {
    int wavelengthNm = 0;
    double x = 0.0;
    double y = 0.0;
};

// One row of the CIE 1931 2-degree standard observer's color matching functions.
struct CieColorMatchingSample final {
    int wavelengthNm;
    double xBar;
    double yBar;
    double zBar;
};

inline constexpr int kCieObserverFirstWavelengthNm = 380;
inline constexpr int kCieObserverLastWavelengthNm = 780;
inline constexpr int kCieObserverStepNm = 5;

// The CIE 1931 2-degree color matching functions, 380 to 780 nm at 5 nm steps (81 rows).
[[nodiscard]] std::span<const CieColorMatchingSample> cie1931ColorMatchingFunctions() noexcept;

// The spectral locus as xy points in ascending wavelength: x = xBar / (xBar + yBar + zBar), y =
// yBar / (xBar + yBar + zBar), one point per table row.
[[nodiscard]] std::vector<SpectralLocusPoint> cie1931SpectralLocus();

} // namespace bloom::color
