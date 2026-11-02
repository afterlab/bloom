// #429: a nested composition is placed by its frame (composition bounds), independent of where
// its content sits.
//
// Real documents are built through commands and compiled by the production SnapshotCompiler. The
// CPU reference must place each nested composition by its frame: off-centre content, a smaller
// nested frame, scaled placement about the frame centre, nesting depth two, and an EMPTY nested
// composition whose parented child must use the frame's top-left as its parent space. Each state
// is then prepared by the production CpuGpuSceneBuilder, run by the production GpuSceneExecutor,
// and must match the CPU reference pixels (2e-6) and bounds; a warm rerun dispatches nothing new.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>

#include <iostream>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

document::FrameRate rate() { return *document::FrameRate::create(24, 1); }
document::CompositionFormat format(const std::uint32_t width, const std::uint32_t height) {
    return *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                rate());
}
constexpr core::Color4d kRed{0.9, 0.1, 0.1, 1.0};

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Nested placement", "Main", core::RationalTime::fromInteger(2), format(64, 48));
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};

    document::CompositionId addComposition(const std::string& name, const std::uint32_t width,
                                           const std::uint32_t height) {
        execute<commands::AddComposition>(document, commands, name, format(width, height),
                                          core::RationalTime::fromInteger(2));
        return document.snapshot().project().compositions().back().id();
    }
    // A Solid of the given size centred on a composition-space point.
    document::LayerId addSolid(const document::CompositionId target, const document::Vec2d centre,
                               const double width, const double height) {
        execute<commands::AddSolidLayer>(document, commands, target, "Red", kRed, centre);
        const auto solid = lastNode(document, target, document::kSolidSourceNodeType);
        set(target, parameter(document, target, solid, "width"), width);
        set(target, parameter(document, target, solid, "height"), height);
        return lastLayer(document, target);
    }
    document::LayerId addNested(const document::CompositionId target,
                                const document::CompositionId source) {
        execute<commands::AddCompositionSource>(document, commands, target, source,
                                                document::Vec2d{}, true);
        return lastLayer(document, target);
    }
    void set(const document::CompositionId target, const document::ParameterId id,
             document::ParameterValue value) {
        execute<commands::SetParameterSource>(document, commands, target, id,
                                              document::ConstantValueSource{std::move(value)});
    }
    void setLayer(const document::CompositionId target, const document::LayerId layer,
                  const std::string_view role, document::ParameterValue value) {
        set(target, parameter(document, target, layerNode(document, target, layer), role),
            std::move(value));
    }
};

// The evaluated Layer Output bounds of a layer in the reference frame.
[[nodiscard]] std::optional<runtime::EvaluatedOperationBounds>
layerBounds(const Reference& reference, const document::LayerId layer) {
    const auto& operations = reference.plan->operations();
    for (std::size_t index = 0; index < operations.size(); ++index)
        if (const auto* output = std::get_if<runtime::CompiledLayerOutput>(&operations[index]);
            output != nullptr && output->layerId == layer)
            return reference.frame->evaluatedBounds()[index];
    return std::nullopt;
}

void expectRect(Expectations& expectations, const Reference& reference, const PixelRect expected,
                const std::string& label) {
    const auto actual = coverage(reference);
    expectations.expect(actual == expected, label + ": content lands at its frame position (got " +
                                                std::to_string(actual.left) + "," +
                                                std::to_string(actual.top) + "-" +
                                                std::to_string(actual.right) + "," +
                                                std::to_string(actual.bottom) + ")");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        std::vector<std::pair<std::string, Reference>> references;
        const auto keep = [&](const std::string& label, const std::optional<Reference>& reference) {
            if (reference)
                references.emplace_back(label, *reference);
            return reference;
        };

        // Off-centre content: an 8x6 Solid centred on (44, 39) of a 64x48 nested frame. Placed by
        // its frame at the main centre, the nested layer is the identity, so the Solid keeps its
        // own pixels; placing by content bounds would centre it on (32, 24).
        {
            Fixture fixture;
            const auto inner = fixture.addComposition("Inner", 64, 48);
            fixture.addSolid(inner, {44, 39}, 8, 6);
            const auto nested = fixture.addNested(fixture.main, inner);
            const auto reference = keep("off-centre", cpuReference(expectations, fixture.document,
                                                                   fixture.main, "off-centre"));
            if (reference) {
                expectRect(expectations, *reference, {40, 36, 48, 42}, "off-centre");
                const auto bounds = layerBounds(*reference, nested);
                expectations.expect(
                    bounds && bounds->local == runtime::ContentBounds{0, 0, 64, 48} &&
                        bounds->output == runtime::ContentBounds{0, 0, 64, 48} &&
                        bounds->anchor == document::Vec2d{32, 24},
                    "off-centre: the nested layer's bounds and anchor are its frame");
            }
            // Scale 0.5 pivots on the frame centre (32, 24): the Solid maps to [36,40)x[30,33).
            fixture.setLayer(fixture.main, nested, "scale", document::Vec2d{0.5, 0.5});
            const auto scaled = keep(
                "scaled", cpuReference(expectations, fixture.document, fixture.main, "scaled"));
            if (scaled)
                expectations.expect(
                    alphaAt(*scaled, 37, 31) > 0.99F && alphaAt(*scaled, 38, 31) > 0.99F &&
                        alphaAt(*scaled, 32, 24) == 0.0F && alphaAt(*scaled, 34, 28) == 0.0F,
                    "scaled: the nested layer scales about its frame centre");
        }

        // Content partly outside its frame is clipped by the frame, and the frame still places it.
        {
            Fixture fixture;
            const auto inner = fixture.addComposition("Inner", 64, 48);
            fixture.addSolid(inner, {62, 46}, 8, 6);
            fixture.addNested(fixture.main, inner);
            if (const auto reference = keep("clipped", cpuReference(expectations, fixture.document,
                                                                    fixture.main, "clipped")))
                expectRect(expectations, *reference, {58, 43, 64, 48}, "clipped");
        }

        // A smaller nested frame (32x24) with content in its lower-left region: the frame centre
        // lands on the main centre, offsetting nested pixels by (16, 12).
        {
            Fixture fixture;
            const auto inner = fixture.addComposition("Small", 32, 24);
            fixture.addSolid(inner, {6, 18}, 4, 4);
            fixture.addNested(fixture.main, inner);
            if (const auto reference =
                    keep("smaller frame", cpuReference(expectations, fixture.document, fixture.main,
                                                       "smaller frame")))
                expectRect(expectations, *reference, {20, 28, 24, 32}, "smaller frame");
        }

        // Depth two: main <- middle <- inner, where middle contains only the nested inner layer.
        {
            Fixture fixture;
            const auto inner = fixture.addComposition("Inner", 64, 48);
            fixture.addSolid(inner, {44, 39}, 8, 6);
            const auto middle = fixture.addComposition("Middle", 64, 48);
            fixture.addNested(middle, inner);
            fixture.addNested(fixture.main, middle);
            if (const auto reference =
                    keep("depth two",
                         cpuReference(expectations, fixture.document, fixture.main, "depth two")))
                expectRect(expectations, *reference, {40, 36, 48, 42}, "depth two");
        }

        // An empty nested composition still has its frame: a 4x4 Solid at (10, 10) parented to it
        // lands at (10, 10) because the parent's space starts at the nested frame's top-left.
        {
            Fixture fixture;
            const auto inner = fixture.addComposition("Empty", 64, 48);
            const auto nested = fixture.addNested(fixture.main, inner);
            const auto child = fixture.addSolid(fixture.main, {10, 10}, 4, 4);
            execute<commands::SetLayerParent>(fixture.document, fixture.commands, fixture.main,
                                              child, nested);
            if (const auto reference =
                    keep("empty parent", cpuReference(expectations, fixture.document, fixture.main,
                                                      "empty parent"))) {
                expectRect(expectations, *reference, {8, 8, 12, 12}, "empty parent");
                const auto bounds = layerBounds(*reference, nested);
                expectations.expect(bounds && bounds->local == runtime::ContentBounds{0, 0, 64, 48},
                                    "empty parent: an empty nested layer keeps its frame bounds");
            }
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " nested placement expectation(s) failed\n";
            return 1;
        }

        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = render::GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: CPU placement passed; no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 77;
        }
        auto cache =
            runtime::GpuSceneCache::create(*device.device, runtime::GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "the scene cache creates");
        if (!cache)
            return 1;
        auto executor = runtime::GpuSceneExecutor::create(*device.device, *cache.cache);
        expectations.expect(executor.hasValue(), "the scene executor creates");
        if (!executor)
            return 1;
        for (const auto& [label, reference] : references)
            gpuParity(expectations, *executor.executor, reference, label);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " nested placement expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: nested compositions are placed by their frame on the GPU and the CPU "
                     "reference alike\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
