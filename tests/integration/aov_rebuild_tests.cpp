// The AOV rebuild a compositor does with every multi-pass render, on a real compiled document and
// the CPU reference evaluator:
//
//   beauty = (diffuse direct + diffuse indirect) x diffuse colour + specular + emission
//
// The test writes a small synthetic multilayer OpenEXR whose root RGBA is that beauty, computed by
// the "renderer" in Float64 and stored as Float32, and whose named layers are the passes. It
// imports the file once, adds one image layer per pass, picks each pass with the Image source's
// layer parameter, and stacks them with Normal, Add, Multiply, Add and Add -- the Layer Stack folds
// bottom to top, so the stack IS the formula. The rebuilt frame must match the stored beauty within
// a Float32 tolerance on every pixel, including HDR highlights far above 1, a black diffuse colour,
// and a small negative indirect sample a denoiser can leave behind. Nothing may be clamped.

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr int kWidth = 8;
constexpr int kHeight = 4;
constexpr std::size_t kSamples = static_cast<std::size_t>(kWidth) * kHeight;

// One RGB pass: three planes, one per channel.
struct Pass final {
    std::string layer;
    std::array<std::vector<float>, 3> planes;
};

// Deterministic per-pixel pass values. Pixel 5 is a black diffuse colour, pixel 9 a small negative
// indirect sample, pixels 12 and 27 HDR specular highlights, and pixel 20 an emitter.
struct Passes final {
    Pass diffuseDirect{"DiffDir", {}};
    Pass diffuseIndirect{"DiffInd", {}};
    Pass diffuseColour{"DiffCol", {}};
    Pass specular{"Spec", {}};
    Pass emission{"Emit", {}};
    std::array<std::vector<float>, 3> beauty;
};

[[nodiscard]] Passes makePasses() {
    Passes passes;
    std::mt19937 random(482U);
    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    for (auto* pass : {&passes.diffuseDirect, &passes.diffuseIndirect, &passes.diffuseColour,
                       &passes.specular, &passes.emission})
        for (auto& plane : pass->planes)
            plane.resize(kSamples);
    for (auto& plane : passes.beauty)
        plane.resize(kSamples);
    for (std::size_t index = 0; index < kSamples; ++index) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            passes.diffuseDirect.planes[channel][index] = 4.0F * unit(random);
            passes.diffuseIndirect.planes[channel][index] = 0.5F * unit(random);
            passes.diffuseColour.planes[channel][index] = 0.05F + 0.9F * unit(random);
            passes.specular.planes[channel][index] = 0.25F * unit(random);
            passes.emission.planes[channel][index] = 0.0F;
        }
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
        passes.diffuseColour.planes[channel][5] = 0.0F;
        passes.diffuseIndirect.planes[channel][9] = -0.0625F;
        passes.specular.planes[channel][12] = 37.5F + static_cast<float>(channel);
        passes.specular.planes[channel][27] = 1200.0F;
        passes.emission.planes[channel][20] = 16.0F - static_cast<float>(channel);
    }
    for (std::size_t index = 0; index < kSamples; ++index) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double lighting =
                static_cast<double>(passes.diffuseDirect.planes[channel][index]) +
                static_cast<double>(passes.diffuseIndirect.planes[channel][index]);
            passes.beauty[channel][index] = static_cast<float>(
                lighting * static_cast<double>(passes.diffuseColour.planes[channel][index]) +
                static_cast<double>(passes.specular.planes[channel][index]) +
                static_cast<double>(passes.emission.planes[channel][index]));
        }
    }
    return passes;
}

// One scanline part: root RGBA is the beauty, every pass is a named layer "<pass>.R/G/B".
void writeMultilayerExr(const std::filesystem::path& path, Passes& passes) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(kWidth - 1, kHeight - 1));
    std::vector<float> alpha(kSamples, 1.0F);
    Imf::Header header(window, window);
    Imf::FrameBuffer frameBuffer;
    const auto insert = [&](const std::string& name, std::vector<float>& plane) {
        header.channels().insert(name, Imf::Channel(Imf::FLOAT));
        frameBuffer.insert(name, Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(plane.data()),
                                            sizeof(float), kWidth * sizeof(float)));
    };
    constexpr std::array<const char*, 3> kChannels{"R", "G", "B"};
    for (std::size_t channel = 0; channel < 3; ++channel)
        insert(kChannels[channel], passes.beauty[channel]);
    insert("A", alpha);
    for (auto* pass : {&passes.diffuseDirect, &passes.diffuseIndirect, &passes.diffuseColour,
                       &passes.specular, &passes.emission})
        for (std::size_t channel = 0; channel < 3; ++channel)
            insert(pass->layer + "." + kChannels[channel], pass->planes[channel]);
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(kHeight);
}

struct Fixture final {
    std::filesystem::path folder;
    document::NewProject initial = document::makeNewProject(
        "AOV", "Main", core::RationalTime::fromInteger(1),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::AssetId asset;

    template <typename Operation, typename... Arguments> void run(Arguments&&... arguments) {
        commands::Transaction transaction("AOV", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        const auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            for (const auto& failure : result.operationFailures)
                std::cerr << failure.operationType << ": " << failure.issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
    }

    explicit Fixture(Passes& passes) {
        std::mt19937_64 random{std::random_device{}()};
        folder = std::filesystem::temp_directory_path() /
                 ("bloom-aov-rebuild-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        writeMultilayerExr(folder / "render.exr", passes);
        run<commands::ImportAssets>(std::vector{folder / "render.exr"}, folder);
        asset = document.snapshot().project().assets().front().id;
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] const document::CanonicalGraph& graph() const {
        return document.snapshot().project().findComposition(composition)->graph();
    }

    [[nodiscard]] document::ParameterId parameter(const document::NodeId node,
                                                  const std::string_view role) const {
        for (const auto& binding : graph().findNode(node)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }

    template <typename Value>
    void set(const document::NodeId node, const std::string_view role, Value value) {
        run<commands::SetParameterSource>(
            composition, parameter(node, role),
            document::ConstantValueSource{document::ParameterValue{std::move(value)}});
    }

    // One image layer on top of the stack that shows `layer` of the render under `mode`.
    void addPassLayer(const std::string& layer, const core::BlendMode mode) {
        std::set<std::uint64_t> before;
        for (const auto& node : graph().nodes())
            before.insert(node.id.value());
        run<commands::AddImageLayer>(composition, asset);
        for (const auto& node : graph().nodes()) {
            if (before.contains(node.id.value()))
                continue;
            if (node.typeId == document::kImageSourceNodeType && !layer.empty())
                set(node.id, document::kImageLayerParameterRole, layer);
            if (node.typeId == document::kLayerOutputNodeType)
                set(node.id, document::kBlendModeParameterRole, core::blendModeStoredValue(mode));
        }
    }

    [[nodiscard]] std::vector<render::Rgba32f> frame() const {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({document.snapshot(), composition, {}, {}}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("AOV compilation failed");
        }
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(folder);
        const auto result = evaluator.evaluate(compiled.plan,
                                               {{},
                                                compiled.plan->output(),
                                                runtime::CompositionFormatResolution{},
                                                runtime::EvaluationQuality::Reference,
                                                runtime::EvaluationColorIntent::LinearRec709Scene,
                                                1U << 24U},
                                               {});
        if (!result.frame()) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("AOV evaluation failed");
        }
        const auto span = result.frame()->processImage().pixels();
        return {span.begin(), span.end()};
    }
};

// Float32 tolerance for one rebuilt sample against the stored beauty: the stack rounds each fold to
// Float32 where the renderer rounded the whole sum once, so a few units in the last place per step.
[[nodiscard]] bool matches(const float actual, const float expected) {
    const float tolerance = 1e-6F + 4e-7F * std::max(std::fabs(actual), std::fabs(expected));
    return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
}

void aovRebuildRoundTripsToBeauty() {
    auto passes = makePasses();
    Fixture fixture(passes);
    // The plain image layer shows the root RGBA, which is the beauty itself.
    fixture.addPassLayer("", core::BlendMode::Normal);
    const auto beauty = fixture.frame();
    expect(beauty.size() == kSamples, "the composition frame covers the whole render");
    bool beautyExact = beauty.size() == kSamples;
    for (std::size_t index = 0; beautyExact && index < kSamples; ++index)
        beautyExact = beauty[index].red() == passes.beauty[0][index] &&
                      beauty[index].green() == passes.beauty[1][index] &&
                      beauty[index].blue() == passes.beauty[2][index] &&
                      beauty[index].alpha() == 1.0F;
    expect(beautyExact, "the root RGBA decodes as the stored beauty, unclamped");

    // The rebuild, bottom to top, OVER the beauty layer: Normal on the first pass replaces it.
    fixture.addPassLayer("DiffDir", core::BlendMode::Normal);
    fixture.addPassLayer("DiffInd", core::BlendMode::Add);
    fixture.addPassLayer("DiffCol", core::BlendMode::Multiply);
    fixture.addPassLayer("Spec", core::BlendMode::Add);
    fixture.addPassLayer("Emit", core::BlendMode::Add);
    const auto rebuilt = fixture.frame();
    expect(rebuilt.size() == kSamples, "the rebuilt frame covers the whole render");
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < std::min(rebuilt.size(), kSamples); ++index) {
        const auto& pixel = rebuilt[index];
        const bool ok = matches(pixel.red(), passes.beauty[0][index]) &&
                        matches(pixel.green(), passes.beauty[1][index]) &&
                        matches(pixel.blue(), passes.beauty[2][index]) && pixel.alpha() == 1.0F;
        if (!ok) {
            ++mismatches;
            std::cerr << "pixel " << index << ": rebuilt (" << pixel.red() << ", " << pixel.green()
                      << ", " << pixel.blue() << ", " << pixel.alpha() << ") beauty ("
                      << passes.beauty[0][index] << ", " << passes.beauty[1][index] << ", "
                      << passes.beauty[2][index] << ")\n";
        }
    }
    expect(mismatches == 0, "diffuse x colour + specular + emission round-trips to the beauty");
    if (rebuilt.size() == kSamples) {
        expect(rebuilt[27].red() > 1000.0F && rebuilt[20].blue() > 13.0F,
               "HDR specular and emission survive the stack unclamped");
        expect(matches(rebuilt[9].red(), passes.beauty[0][9]),
               "a negative indirect sample is carried, not clipped");
    }

    // The same passes recover the lighting from the beauty: beauty - specular - emission, divided
    // by the diffuse colour, is the summed diffuse lighting -- and a black diffuse colour divides
    // to zero rather than an infinity.
    Passes unused = makePasses();
    Fixture inverse(unused);
    inverse.addPassLayer("", core::BlendMode::Normal);
    inverse.addPassLayer("Spec", core::BlendMode::Subtract);
    inverse.addPassLayer("Emit", core::BlendMode::Subtract);
    inverse.addPassLayer("DiffCol", core::BlendMode::Divide);
    const auto lighting = inverse.frame();
    std::size_t lightingMismatches = 0;
    for (std::size_t index = 0; index < std::min(lighting.size(), kSamples); ++index) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float colour = passes.diffuseColour.planes[channel][index];
            const float expected = colour == 0.0F
                                       ? 0.0F
                                       : passes.diffuseDirect.planes[channel][index] +
                                             passes.diffuseIndirect.planes[channel][index];
            const float actual = lighting[index].components()[channel];
            // Removing an HDR highlight cancels large terms, so the tolerance follows the beauty.
            const float tolerance =
                1e-5F + 2e-6F * std::fabs(passes.beauty[channel][index]) / std::max(colour, 0.05F);
            if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance)
                ++lightingMismatches;
        }
    }
    expect(lighting.size() == kSamples && lightingMismatches == 0,
           "Subtract and Divide recover the diffuse lighting, with x/0 = 0 on a black colour");
}

} // namespace

int main() {
    try {
        aovRebuildRoundTripsToBeauty();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " AOV rebuild expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: AOV rebuild round-trips to the beauty\n";
    return 0;
}
