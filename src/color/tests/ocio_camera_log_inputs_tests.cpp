// Interpret Footage (#381): the camera log input spaces Bloom synthesizes resolve in both pinned
// configs, decode their 18% grey code values to scene grey, and carry a synthesized identity.
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>

#include <array>
#include <cmath>
#include <iostream>
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
using bloom::color::OcioConfigLocatorKind;

[[nodiscard]] bloom::color::OcioBuiltInResolutionResult resolve(const std::string_view uri,
                                                                const std::string_view working) {
    const auto revision =
        bloom::color::ocioBuiltInContentRevision(OcioConfigLocatorKind::BloomBuiltIn, uri);
    return bloom::color::resolveOcioBuiltIn(
        OcioConfigLocatorKind::BloomBuiltIn, uri,
        revision.value_or(bloom::color::kBloomNeutralV1ConfigDigest), working);
}

struct Camera final {
    const char* id;
    // The published code value of 18% grey for the curve.
    float grey;
};
// Published 18% grey code values (S-Log3 41.0 IRE, LogC4 0.2784, V-Log 0.4233, Apple Log 0.4785,
// Log3G10 0.3333, C-Log3 0.3439 approximately).
constexpr std::array<Camera, 6> kCameras{{{"S-Log3 SGamut3 (Bloom)", 0.4105F},
                                          {"LogC4 AWG4 (Bloom)", 0.2784F},
                                          {"V-Log V-Gamut (Bloom)", 0.4233F},
                                          {"Apple Log (Bloom)", 0.4785F},
                                          {"Log3G10 RWG (Bloom)", 0.3333F},
                                          {"C-Log3 CGamut (Bloom)", 0.3439F}}};

void testConfig(const std::string_view uri, const std::string_view working,
                const std::string& label) {
    auto config = resolve(uri, working);
    const auto* resolved = config.resolved();
    expect(resolved != nullptr, label + ": the config resolves");
    if (resolved == nullptr)
        return;
    for (const auto& camera : kCameras) {
        const auto name = std::string(camera.id);
        const auto prepared =
            bloom::color::CpuColorSpaceProcessor::prepare(*resolved, camera.id, working);
        expect(prepared.succeeded(), label + ": " + name + " prepares");
        if (!prepared)
            continue;
        std::array<std::array<float, 4>, 1> pixel{{{camera.grey, camera.grey, camera.grey, 1.0F}}};
        expect(prepared.processor()->apply(pixel), label + ": " + name + " applies");
        // Grey stays neutral and lands near 0.18 (the curves' published values are rounded).
        const auto mean = (pixel[0][0] + pixel[0][1] + pixel[0][2]) / 3.0F;
        expect(std::abs(mean - 0.18F) < 0.02F,
               label + ": " + name + " grey decodes near 0.18 (" + std::to_string(mean) + ")");
        expect(std::abs(pixel[0][0] - pixel[0][2]) < 0.01F,
               label + ": " + name + " grey stays neutral");
        expect(!bloom::color::synthesizedColorSpaceIdentity(camera.id).empty(),
               name + " carries a synthesized identity");
    }
}
// #426: Cineon printing density decodes reference white (code 685) to 1.0, reference black (code 95)
// to 0 and a mid code on the density curve, and stays neutral, in both pinned configs.
void testCineon(const std::string_view uri, const std::string_view working,
                const std::string& label) {
    auto config = resolve(uri, working);
    const auto* resolved = config.resolved();
    expect(resolved != nullptr, label + ": the config resolves for Cineon");
    if (resolved == nullptr)
        return;
    const auto prepared = bloom::color::CpuColorSpaceProcessor::prepare(
        *resolved, bloom::color::kBloomCineonLogColorSpaceId, working);
    expect(prepared.succeeded(), label + ": Cineon prepares");
    if (!prepared)
        return;
    const auto decode = [&](const float code) {
        std::array<std::array<float, 4>, 1> pixel{{{code, code, code, 1.0F}}};
        expect(prepared.processor()->apply(pixel), label + ": Cineon applies");
        return pixel[0];
    };
    const auto white = decode(685.0F / 1023.0F);
    const auto black = decode(95.0F / 1023.0F);
    // One density step of 0.002 per code at gamma 0.6: code 470 is 215 codes under white.
    const float expectedMid =
        (std::pow(10.0F, -215.0F * 0.002F / 0.6F) - 0.010789F) / (1.0F - 0.010789F);
    const auto mid = decode(470.0F / 1023.0F);
    for (std::size_t c = 0; c < 3; ++c) {
        expect(std::abs(white[c] - 1.0F) < 0.01F,
               label + ": Cineon white decodes to 1.0 (" + std::to_string(white[c]) + ")");
        expect(std::abs(black[c]) < 0.002F, label + ": Cineon black decodes to 0");
        expect(std::abs(mid[c] - expectedMid) < 0.01F,
               label + ": Cineon code 470 decodes on the density curve (" +
                   std::to_string(mid[c]) + " vs " + std::to_string(expectedMid) + ")");
    }
    expect(std::abs(mid[0] - mid[2]) < 0.01F, label + ": Cineon grey stays neutral");
    expect(bloom::color::synthesizedColorSpaceIdentity(bloom::color::kBloomCineonLogColorSpaceId)
                   .starts_with("bloom-cineon-log-v1;"),
           label + ": Cineon carries a synthesized identity");
}
} // namespace

int main() {
    testCineon(bloom::color::kBloomNeutralV1ConfigUri, "lin_rec709_scene", "Bloom Neutral");
    testCineon(bloom::color::kAcesCgV1ConfigUri, "ACEScg", "ACES 1.3 CG");
    testConfig(bloom::color::kBloomNeutralV1ConfigUri, "lin_rec709_scene", "Bloom Neutral");
    testConfig(bloom::color::kAcesCgV1ConfigUri, "ACEScg", "ACES 1.3 CG");
    return failures == 0 ? 0 : 1;
}
