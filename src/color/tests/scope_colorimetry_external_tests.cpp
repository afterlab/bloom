// #724 Scopes: the XYZ transform and the luma coefficients through the supervised helper.
//
// A project with an external OCIO config resolves its transforms in the confined bloom-color-worker
// helper, never in the application's process (docs/architecture/color-management.md, "Supervised
// OCIO Execution"). `ScopeXyzProcessor::prepare` takes that route for such a config: the helper
// builds the processor, holds it, and applies it a chunk of pixels at a time. Asserted, for a
// loose copy of the ACES CG config resolved through the helper:
//   * the display color spaces of the config that are Rec. 709 give the Rec. 709 luma coefficients
//     (0.2126, 0.7152, 0.0722 within 1e-4) and the Rec. 709 primaries and D65 white as printed;
//   * the helper's processor equals the in-process built-in's, pixel for pixel;
//   * a config with no CIE XYZ D65 or ACES interchange role answers NoXyzInterchange for a color
//     space of it, never a wrong answer; a data space and an unknown space have their own errors.

#include "ocio_config_session.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_config_resolution.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/color/scope_colorimetry.hpp>

#include <OpenColorIO/OpenColorIO.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

using namespace bloom::color;
namespace OCIO = OCIO_NAMESPACE;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

void write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << contents;
}

[[nodiscard]] OcioConfigLocation loose(const std::filesystem::path& root) {
    return {OcioConfigLocatorKind::ExternalLooseConfig, (root / "config.ocio").string(), {}};
}

// The config at `root`, resolved through the helper at its current revision.
[[nodiscard]] std::optional<ResolvedBloomNeutralConfig>
resolveThroughHelper(const std::filesystem::path& root, const std::string& working) {
    const auto location = loose(root);
    const auto unknown = resolveOcioConfig(location, bloom::core::Sha256Digest{}, working);
    if (!unknown.recomputedRevision())
        return std::nullopt;
    return resolveOcioConfig(ocioConfigLocationText(location), *unknown.recomputedRevision(),
                             working)
        .takeResolved();
}

void expectRec709(const ScopeColorimetry& value, const std::string& what) {
    constexpr std::array<float, 3> kLuma{0.2126F, 0.7152F, 0.0722F};
    for (std::size_t index = 0; index < 3; ++index)
        expect(
            near(static_cast<double>(value.luma[index]), static_cast<double>(kLuma[index]), 1.0e-4),
            what + ": Rec.709 luma coefficient " + std::to_string(index));
    expect(formatChromaticityXy(value.red) == "0.6400 0.3300",
           what + ": red prints 0.6400 0.3300, got " + formatChromaticityXy(value.red));
    expect(formatChromaticityXy(value.green) == "0.3000 0.6000",
           what + ": green prints 0.3000 0.6000, got " + formatChromaticityXy(value.green));
    expect(formatChromaticityXy(value.blue) == "0.1500 0.0600",
           what + ": blue prints 0.1500 0.0600, got " + formatChromaticityXy(value.blue));
    expect(formatChromaticityXy(value.white) == "0.3127 0.3290",
           what + ": white prints 0.3127 0.3290, got " + formatChromaticityXy(value.white));
}

// A loose copy of the ACES CG built-in: it reads no file, so what the helper answers must equal
// what the built-in answers in this process.
void testRec709DisplayThroughTheHelper(const std::filesystem::path& root) {
    const auto builtIn =
        OCIO::Config::CreateFromBuiltinConfig(std::string(kAcesCgV1BuiltinConfigName).c_str());
    std::ostringstream text;
    builtIn->serialize(text);
    write(root / "aces" / "config.ocio", text.str());
    const std::string working(kAcesCgV1SceneLinearColorSpaceId);

    const auto helper = resolveThroughHelper(root / "aces", working);
    expect(helper.has_value(), "the loose ACES copy resolves through the helper");
    const auto builtInRevision =
        ocioBuiltInContentRevision(OcioConfigLocatorKind::BloomBuiltIn, kAcesCgV1ConfigUri);
    auto reference = builtInRevision
                         ? resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                              kAcesCgV1ConfigUri, *builtInRevision, working)
                               .takeResolved()
                         : std::nullopt;
    expect(reference.has_value(), "the built-in reference resolves");
    if (!helper || !reference)
        return;

    // The config lives in the helper: a display processor built from it is a helper token.
    const auto display =
        buildCpuDisplayProcessorForView(*helper, helper->displayName(), helper->viewName())
            .takeHandle();
    expect(display.has_value() && display->lease().kind() == DisplayProcessorLeaseKind::HelperToken,
           "the loose config is held by the helper, not parsed in this process");

    std::size_t rec709Spaces = 0;
    for (const char* const space : {"sRGB - Display", "Rec.1886 Rec.709 - Display"}) {
        const auto prepared = ScopeXyzProcessor::prepare(*helper, space);
        expect(prepared.succeeded(), std::string("the helper prepares ") + space + " (error " +
                                         std::to_string(static_cast<int>(prepared.error)) + ")");
        if (!prepared)
            continue;
        ++rec709Spaces;
        expectRec709(prepared.processor->colorimetry(), std::string("helper ") + space);
        expect(prepared.processor->colorimetry().colorSpaceName == space,
               "the helper's processor reports the color space name");
        expect(prepared.processor->configRevision() == helper->expectedRevision(),
               "the processor carries the loose config's own revision");

        // The same transform the built-in prepares in this process, pixel for pixel.
        const auto builtInProcessor = ScopeXyzProcessor::prepare(*reference, space);
        expect(builtInProcessor.succeeded(), "the built-in prepares the same space");
        if (!builtInProcessor)
            continue;
        expect(prepared.processor->colorimetry().luma ==
                   builtInProcessor.processor->colorimetry().luma,
               "the helper's luma coefficients equal the built-in's");
        constexpr std::array<float, 15> kPixels{0.0F, 0.0F,  0.0F,  0.18F, 0.5F, 0.9F, 1.0F, 1.0F,
                                                1.0F, 0.25F, 0.75F, 0.1F,  0.6F, 0.4F, 0.8F};
        std::array<float, 15> mine{};
        std::array<float, 15> theirs{};
        expect(prepared.processor->apply(kPixels, mine) &&
                   builtInProcessor.processor->apply(kPixels, theirs) && mine == theirs,
               "the helper's XYZ for sample pixels equals the built-in's, bit for bit");
        // A pixel the helper must refuse, as the in-process arm does.
        const std::array<float, 3> notFinite{std::nanf(""), 0.0F, 0.0F};
        std::array<float, 3> out{};
        expect(!prepared.processor->apply(notFinite, out), "a non-finite pixel is refused");
        // More pixels than one helper call carries (256): every chunk is applied.
        std::array<float, 3 * 600> many{};
        std::array<float, 3 * 600> manyOut{};
        std::array<float, 3 * 600> manyReference{};
        for (std::size_t index = 0; index < many.size(); ++index)
            many[index] = static_cast<float>(index % 97U) / 96.0F;
        expect(prepared.processor->apply(many, manyOut) &&
                   builtInProcessor.processor->apply(many, manyReference) &&
                   manyOut == manyReference,
               "600 pixels (three helper chunks) equal the built-in's");
    }
    expect(rec709Spaces == 2, "both Rec. 709 display spaces of the config were checked");
}

// A config with a scene-linear role and a data space, and neither the CIE XYZ D65 nor the ACES
// interchange role: no route from its spaces to XYZ exists.
void testNoRouteToXyz(const std::filesystem::path& root) {
    write(root / "plain" / "config.ocio", "ocio_profile_version: 2\n"
                                          "strictparsing: true\n"
                                          "roles:\n"
                                          "  default: raw\n"
                                          "  scene_linear: lin\n"
                                          "displays:\n"
                                          "  sRGB:\n"
                                          "    - !<View> {name: Raw, colorspace: raw}\n"
                                          "active_displays: [sRGB]\n"
                                          "active_views: [Raw]\n"
                                          "colorspaces:\n"
                                          "  - !<ColorSpace>\n"
                                          "    name: lin\n"
                                          "    encoding: scene-linear\n"
                                          "    isdata: false\n"
                                          "  - !<ColorSpace>\n"
                                          "    name: raw\n"
                                          "    isdata: true\n");
    const auto helper = resolveThroughHelper(root / "plain", "lin");
    expect(helper.has_value(), "the plain config resolves through the helper");
    if (!helper)
        return;
    const auto linear = ScopeXyzProcessor::prepare(*helper, "lin");
    expect(!linear.succeeded() && linear.error == ScopeXyzError::NoXyzInterchange,
           "a space of a config with no route to XYZ is NoXyzInterchange, not a guess (error " +
               std::to_string(static_cast<int>(linear.error)) + ")");
    const auto data = ScopeXyzProcessor::prepare(*helper, "raw");
    expect(!data.succeeded() && (data.error == ScopeXyzError::ColorSpaceIsData ||
                                 data.error == ScopeXyzError::NoXyzInterchange),
           "a data space has no XYZ either (error " + std::to_string(static_cast<int>(data.error)) +
               ")");
    const auto missing = ScopeXyzProcessor::prepare(*helper, "No Such Space");
    expect(!missing.succeeded() && missing.error == ScopeXyzError::MissingColorSpace,
           "an unknown space is MissingColorSpace (error " +
               std::to_string(static_cast<int>(missing.error)) + ")");
    expect(ScopeXyzProcessor::prepare(*helper, "").error == ScopeXyzError::InvalidColorSpaceId,
           "an empty id is invalid");
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("bloom-scope-xyz-external-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    testRec709DisplayThroughTheHelper(root);
    testNoRouteToXyz(root);
    releaseExternalOcioConfigs();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (failures != 0) {
        std::cerr << failures << " external scope colorimetry check(s) failed\n";
        return 1;
    }
    std::cout << "external scope colorimetry checks passed\n";
    return 0;
}
