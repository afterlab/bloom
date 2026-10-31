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
    const auto revision = bloom::color::ocioBuiltInContentRevision(OcioConfigLocatorKind::BloomBuiltIn,
                                                                   uri);
    return bloom::color::resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn, uri,
                                            revision.value_or(bloom::color::kBloomNeutralV1ConfigDigest),
                                            working);
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
        expect(std::abs(mean - 0.18F) < 0.02F, label + ": " + name + " grey decodes near 0.18 (" +
                                                   std::to_string(mean) + ")");
        expect(std::abs(pixel[0][0] - pixel[0][2]) < 0.01F,
               label + ": " + name + " grey stays neutral");
        expect(!bloom::color::synthesizedColorSpaceIdentity(camera.id).empty(),
               name + " carries a synthesized identity");
    }
}
} // namespace

int main() {
    testConfig(bloom::color::kBloomNeutralV1ConfigUri, "lin_rec709_scene", "Bloom Neutral");
    testConfig(bloom::color::kAcesCgV1ConfigUri, "ACEScg", "ACES 1.3 CG");
    return failures == 0 ? 0 : 1;
}
