// The color side of the scopes (#724): OCIO's own transform to CIE XYZ D65 from a color space of a
// pinned config, the colorimetry measured through it (luma coefficients, primaries and white as
// xy), and the CIE 1931 spectral locus. Small and deterministic.
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/color/scope_colorimetry.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}
template <typename A, typename B>
[[nodiscard]] bool near(const A a, const B b, const double tolerance) {
    return std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= tolerance;
}
[[nodiscard]] double sum3(const float* const v) {
    return static_cast<double>(v[0]) + static_cast<double>(v[1]) + static_cast<double>(v[2]);
}

using bloom::color::OcioConfigLocatorKind;

[[nodiscard]] std::optional<bloom::color::ResolvedBloomNeutralConfig>
resolve(const std::string_view uri, const std::string_view working) {
    const auto revision =
        bloom::color::ocioBuiltInContentRevision(OcioConfigLocatorKind::BloomBuiltIn, uri);
    auto result = bloom::color::resolveOcioBuiltIn(
        OcioConfigLocatorKind::BloomBuiltIn, uri,
        revision.value_or(bloom::color::kBloomNeutralV1ConfigDigest), working);
    return std::move(result).takeResolved();
}

struct Expected final {
    std::array<float, 3> luma;
    std::array<double, 8> xy; // R, G, B, W
};
// Rec.709 primaries (ITU-R BT.709) with the D65 white (0.3127, 0.3290).
constexpr Expected kRec709{{0.2126F, 0.7152F, 0.0722F},
                           {0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290}};

[[nodiscard]] std::array<std::string, 4> printed(const bloom::color::ScopeColorimetry& value) {
    return {bloom::color::formatChromaticityXy(value.red),
            bloom::color::formatChromaticityXy(value.green),
            bloom::color::formatChromaticityXy(value.blue),
            bloom::color::formatChromaticityXy(value.white)};
}

void expectRec709(const bloom::color::ScopeColorimetry& value, const std::string& what) {
    for (std::size_t index = 0; index < 3; ++index) {
        expect(near(value.luma[index], kRec709.luma[index], 1.0e-4),
               what + ": Rec.709 luma coefficient " + std::to_string(index));
    }
    const auto text = printed(value);
    expect(text[0] == "0.6400 0.3300", what + ": red prints 0.6400 0.3300, got " + text[0]);
    expect(text[1] == "0.3000 0.6000", what + ": green prints 0.3000 0.6000, got " + text[1]);
    expect(text[2] == "0.1500 0.0600", what + ": blue prints 0.1500 0.0600, got " + text[2]);
    expect(text[3] == "0.3127 0.3290", what + ": white prints 0.3127 0.3290, got " + text[3]);
}

void testFormatting() {
    using bloom::color::ScopeChromaticity;
    expect(bloom::color::formatChromaticityXy({0.64, 0.33}) == "0.6400 0.3300", "plain value");
    expect(bloom::color::formatChromaticityXy({0.0, 1.0}) == "0.0000 1.0000", "zero and one");
    expect(bloom::color::formatChromaticityXy({0.31266, 0.32904}) == "0.3127 0.3290", "rounding");
    expect(bloom::color::formatChromaticityXy({0.00004, -0.01234}) == "0.0000 -0.0123",
           "negative value and tiny positive value");
    expect(bloom::color::formatChromaticityXy({std::numeric_limits<double>::quiet_NaN(), 0.5}) ==
               "nan 0.5000",
           "non-finite value");
}

void testBloomNeutral() {
    auto resolved = resolve(bloom::color::kBloomNeutralV1ConfigUri, "lin_rec709_scene");
    expect(resolved.has_value(), "Bloom Neutral resolves");
    if (!resolved)
        return;

    // The working space (linear Rec.709) and the display color space (sRGB encoded Rec.709).
    for (const std::string_view space :
         {std::string_view("lin_rec709_scene"), std::string_view(resolved->outputColorSpaceId())}) {
        const auto prepared = bloom::color::ScopeXyzProcessor::prepare(*resolved, space);
        expect(prepared.succeeded(), "Bloom Neutral prepares " + std::string(space) + " error " +
                                         std::to_string(static_cast<int>(prepared.error)));
        if (!prepared)
            continue;
        const auto& colorimetry = prepared.processor->colorimetry();
        expectRec709(colorimetry, std::string(space));
        expect(colorimetry.colorSpaceName == space, "the color space name is reported");
        expect(near(colorimetry.luma[0] + colorimetry.luma[1] + colorimetry.luma[2], 1.0, 1.0e-6),
               "luma coefficients sum to 1");

        // White lands on the white point, the primaries on the triangle corners, through apply().
        const std::array<float, 12> rgb{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                        0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F};
        std::array<float, 12> xyz{};
        expect(prepared.processor->apply(rgb, xyz), "the CPU arm applies finite pixels");
        for (std::size_t index = 0; index < 4; ++index) {
            const double sum = sum3(&xyz[index * 3]);
            expect(near(static_cast<double>(xyz[index * 3]) / sum, kRec709.xy[index * 2], 1.0e-4) &&
                       near(static_cast<double>(xyz[index * 3 + 1]) / sum,
                            kRec709.xy[index * 2 + 1], 1.0e-4),
                   "pixel " + std::to_string(index) + " lands on its triangle corner");
        }
        expect(near(xyz[10], 1.0F, 1.0e-4), "white has Y = 1");

        // Mid grey 0.5 of the display encoding is neutral: its chromaticity is the white point.
        const std::array<float, 3> grey{0.5F, 0.5F, 0.5F};
        std::array<float, 3> greyXyz{};
        expect(prepared.processor->apply(grey, greyXyz), "grey applies");
        const double greySum = sum3(greyXyz.data());
        expect(near(static_cast<double>(greyXyz[0]) / greySum, 0.3127, 1.0e-4) &&
                   near(static_cast<double>(greyXyz[1]) / greySum, 0.3290, 1.0e-4),
               "grey is on the white point");

        // Misuse is refused, never half applied.
        std::array<float, 3> wrongSize{};
        expect(!prepared.processor->apply(rgb, wrongSize), "mismatched spans are refused");
        const std::array<float, 3> notFinite{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F};
        std::array<float, 3> out{};
        expect(!prepared.processor->apply(notFinite, out), "a non-finite input is refused");
        expect(prepared.processor->apply(std::span<const float>{}, std::span<float>{}),
               "an empty span applies");
    }

    // The display space's linear light is the working space's: identical colorimetry.
    const auto working = bloom::color::ScopeXyzProcessor::prepare(*resolved, "lin_rec709_scene");
    const auto display =
        bloom::color::ScopeXyzProcessor::prepare(*resolved, resolved->outputColorSpaceId());
    expect(working.succeeded() && display.succeeded() &&
               working.processor->identity() != display.processor->identity(),
           "two different color spaces have different identities");
    const auto again = bloom::color::ScopeXyzProcessor::prepare(*resolved, "lin_rec709_scene");
    expect(again.succeeded() && working.succeeded() &&
               again.processor->identity() == working.processor->identity(),
           "the identity is stable across prepares");
    if (working.succeeded()) {
        expect(working.processor->identity().find("bloom-scope-xyz-d65-v1") == 0,
               "the identity starts with the definition version: " + working.processor->identity());
        expect(working.processor->configRevision() == resolved->expectedRevision(),
               "the processor records the config revision");
    }

    // Typed failures.
    expect(bloom::color::ScopeXyzProcessor::prepare(*resolved, "").error ==
               bloom::color::ScopeXyzError::InvalidColorSpaceId,
           "an empty id is invalid");
    expect(bloom::color::ScopeXyzProcessor::prepare(*resolved, "NotAColorSpace").error ==
               bloom::color::ScopeXyzError::MissingColorSpace,
           "an unknown color space is missing");
}

void testAces() {
    auto resolved = resolve(bloom::color::kAcesCgV1ConfigUri, "ACEScg");
    expect(resolved.has_value(), "the ACES config resolves");
    if (!resolved)
        return;
    // Scene-referred ACEScg (AP1 primaries, white adapted to D65 by OCIO's Bradford built-in).
    const auto acescg = bloom::color::ScopeXyzProcessor::prepare(*resolved, "ACEScg");
    expect(acescg.succeeded(), "ACEScg prepares");
    if (acescg) {
        const auto& value = acescg.processor->colorimetry();
        expect(near(value.luma[0], 0.2677F, 2.0e-3) && near(value.luma[1], 0.6743F, 2.0e-3) &&
                   near(value.luma[2], 0.0580F, 2.0e-3),
               "ACEScg luma coefficients (AP1 adapted to D65 by OCIO's Bradford built-in)");
        expect(printed(value)[3] == "0.3127 0.3290",
               "ACEScg white is D65 after adaptation, got " + printed(value)[3]);
    }
    // A display color space removes the display encoding: sRGB display is Rec.709 light.
    bool sawDisplay = false;
    for (const auto& entry : resolved->displays()) {
        if (entry.display != "sRGB - Display")
            continue;
        sawDisplay = true;
        const auto prepared = bloom::color::ScopeXyzProcessor::prepare(*resolved, entry.display);
        expect(prepared.succeeded(), "the sRGB display color space prepares");
        if (prepared)
            expectRec709(prepared.processor->colorimetry(), "ACES sRGB - Display");
        break;
    }
    expect(sawDisplay, "the ACES config lists an sRGB - Display view");
    const auto aces = bloom::color::ScopeXyzProcessor::prepare(*resolved, "ACES2065-1");
    expect(aces.succeeded(),
           "ACES2065-1 prepares error " + std::to_string(static_cast<int>(aces.error)));
    if (aces && acescg) {
        expect(aces.processor->identity() != acescg.processor->identity(),
               "different ACES spaces differ in identity");
    }
}

void testGpuArm() {
    auto neutral = resolve(bloom::color::kBloomNeutralV1ConfigUri, "lin_rec709_scene");
    auto aces = resolve(bloom::color::kAcesCgV1ConfigUri, "ACEScg");
    expect(neutral.has_value() && aces.has_value(), "both configs resolve for the GPU arm");
    if (!neutral || !aces)
        return;
    const auto working = bloom::color::buildOcioGpuProgramForScopeXyz(*neutral, "lin_rec709_scene");
    const auto display =
        bloom::color::buildOcioGpuProgramForScopeXyz(*neutral, neutral->outputColorSpaceId());
    expect(working.succeeded() && display.succeeded(), "Neutral scope XYZ programs extract");
    if (working.program() != nullptr) {
        expect(working.program()->stage == bloom::render::OcioGpuProgramStage::ProcessEffect &&
                   working.program()->semanticsId == bloom::color::kOcioGpuScopeXyzSemanticsId,
               "the program is a process effect with the scope XYZ semantics id");
        expect(bloom::render::validateOcioGpuProgram(*working.program(), {}) ==
                   bloom::render::OcioGpuProgramError::None,
               "the program satisfies the default limits");
    }
    if (working.program() != nullptr && display.program() != nullptr) {
        expect(working.program()->contentIdentity != display.program()->contentIdentity,
               "different color spaces give different GPU content identities");
    }
    const auto again = bloom::color::buildOcioGpuProgramForScopeXyz(*neutral, "lin_rec709_scene");
    expect(again.program() != nullptr && working.program() != nullptr &&
               again.program()->contentIdentity == working.program()->contentIdentity,
           "extraction is deterministic");
    expect(bloom::color::buildOcioGpuProgramForScopeXyz(*aces, "ACEScg").succeeded(),
           "ACEScg scope XYZ program extracts");
    expect(bloom::color::buildOcioGpuProgramForScopeXyz(*aces, "sRGB - Display").succeeded(),
           "the ACES sRGB display scope XYZ program extracts");
    expect(bloom::color::buildOcioGpuProgramForScopeXyz(*neutral, "NotAColorSpace").error() ==
               bloom::render::OcioGpuProgramError::UnsupportedColorSpace,
           "an unknown color space is a typed refusal");
    expect(bloom::color::buildOcioGpuProgramForScopeXyz(*neutral, "").error() ==
               bloom::render::OcioGpuProgramError::InvalidRequest,
           "an empty id is an invalid request");
}

void testSpectralLocus() {
    const auto table = bloom::color::cie1931ColorMatchingFunctions();
    expect(table.size() == 81, "the table has 81 rows (380 to 780 nm at 5 nm)");
    double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
    double peakY = 0.0;
    int peakAt = 0;
    int previous = bloom::color::kCieObserverFirstWavelengthNm - bloom::color::kCieObserverStepNm;
    for (const auto& row : table) {
        expect(row.wavelengthNm == previous + bloom::color::kCieObserverStepNm,
               "wavelengths ascend in 5 nm steps");
        previous = row.wavelengthNm;
        sumX += row.xBar;
        sumY += row.yBar;
        sumZ += row.zBar;
        if (row.yBar > peakY) {
            peakY = row.yBar;
            peakAt = row.wavelengthNm;
        }
    }
    expect(peakAt == 555 && peakY == 1.0, "y-bar peaks at 555 nm with value 1.0");
    // Published column sums of the 5 nm table (equal-energy normalization, about 21.3715).
    expect(near(sumX, 21.3715, 5.0e-4) && near(sumY, 21.3713, 5.0e-4) &&
               near(sumZ, 21.3715, 5.0e-4),
           "the column sums match the published equal-energy normalization");
    expect(near(sumX / (sumX + sumY + sumZ), 1.0 / 3.0, 1.0e-5) &&
               near(sumY / (sumX + sumY + sumZ), 1.0 / 3.0, 1.0e-5),
           "equal energy has chromaticity (1/3, 1/3)");

    const auto locus = bloom::color::cie1931SpectralLocus();
    expect(locus.size() == table.size(), "one locus point per table row");
    const auto point = [&](const int nm) {
        for (const auto& p : locus)
            if (p.wavelengthNm == nm)
                return p;
        return bloom::color::SpectralLocusPoint{};
    };
    expect(near(point(520).x, 0.0743, 5.0e-4) && near(point(520).y, 0.8338, 5.0e-4),
           "520 nm is (0.0743, 0.8338)");
    expect(near(point(700).x, 0.7347, 5.0e-4) && near(point(700).y, 0.2653, 5.0e-4),
           "700 nm is (0.7347, 0.2653)");
    expect(near(point(460).x, 0.1440, 5.0e-4) && near(point(460).y, 0.0297, 5.0e-4),
           "460 nm is (0.1440, 0.0297)");
    for (const auto& p : locus) {
        expect(p.x >= 0.0 && p.x <= 0.8 && p.y >= 0.0 && p.y <= 0.9,
               "locus point " + std::to_string(p.wavelengthNm) + " nm lies inside the graticule");
    }
}
} // namespace

int main() {
    testFormatting();
    testBloomNeutral();
    testAces();
    testGpuArm();
    testSpectralLocus();
    if (failures != 0) {
        std::cerr << failures << " scope colorimetry check(s) failed\n";
        return 1;
    }
    std::cout << "scope colorimetry checks passed\n";
    return 0;
}
