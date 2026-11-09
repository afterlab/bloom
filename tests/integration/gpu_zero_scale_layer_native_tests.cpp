// A layer whose scale is zero publishes no image: it is invisible and costs nothing. Its shape or
// text source must therefore not send the frame to the CPU route.
//
// Real documents are built through commands and compiled by the production SnapshotCompiler. A
// zero-scale shape layer and text layer, standalone and inside a nested composition, are prepared
// by the production CpuGpuSceneBuilder beside a visible Solid and, when a device exists, run by the
// production GpuSceneExecutor against the CPU reference (2e-6). The fallback diagnostic for an
// unconsumed vector source names the node and the layer.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"
#include "gpu_scene_vector_emission.hpp"

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/shape.hpp>

#include <iostream>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

document::CompositionFormat format() {
    return *document::CompositionFormat::create(64, 48, core::PixelAspectRatio::square(),
                                                *document::FrameRate::create(24, 1));
}

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Zero scale", "Main", core::RationalTime::fromInteger(2), format());
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};

    document::CompositionId addComposition(const std::string& name) {
        execute<commands::AddComposition>(document, commands, name, format(),
                                          core::RationalTime::fromInteger(2));
        return document.snapshot().project().compositions().back().id();
    }
    void addVisibleSolid(const document::CompositionId target) {
        execute<commands::AddSolidLayer>(document, commands, target, "Red",
                                         core::Color4d{0.9, 0.1, 0.1, 1.0},
                                         document::Vec2d{16, 12});
        const auto solid = lastNode(document, target, document::kSolidSourceNodeType);
        setParameter(target, parameter(document, target, solid, "width"), 8.0);
        setParameter(target, parameter(document, target, solid, "height"), 6.0);
    }
    document::LayerId addZeroScaleShape(const document::CompositionId target) {
        execute<commands::AddShapeLayer>(document, commands, target, document::ShapeKind::Rectangle,
                                         commands::ShapeLayerGeometry{},
                                         commands::ShapeLayerStyle{});
        return zeroScale(target);
    }
    document::LayerId addZeroScaleText(const document::CompositionId target) {
        execute<commands::AddTextLayer>(document, commands, target, "Label", "Hidden");
        return zeroScale(target);
    }
    void addNested(const document::CompositionId target, const document::CompositionId source) {
        execute<commands::AddCompositionSource>(document, commands, target, source,
                                                document::Vec2d{}, true);
    }
    void setParameter(const document::CompositionId target, const document::ParameterId id,
                      document::ParameterValue value) {
        execute<commands::SetParameterSource>(document, commands, target, id,
                                              document::ConstantValueSource{std::move(value)});
    }

  private:
    document::LayerId zeroScale(const document::CompositionId target) {
        const auto layer = lastLayer(document, target);
        setParameter(target,
                     parameter(document, target, layerNode(document, target, layer), "scale"),
                     document::Vec2d{0.0, 0.0});
        return layer;
    }
};

// Prepares a state and returns whether it prepared; the diagnostic is part of the expectation.
bool prepares(Expectations& expectations, const Reference& reference, const std::string& label) {
    const auto prepared = runtime::CpuGpuSceneBuilder{}.build(reference.plan, reference.request);
    expectations.expect(prepared.hasValue(),
                        label + ": prepares on the GPU route: " + prepared.diagnostic.message);
    return prepared.hasValue();
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
        const auto keep = [&](const Fixture& fixture, const std::string& label) {
            auto reference = cpuReference(expectations, fixture.document, fixture.main, label);
            if (reference && prepares(expectations, *reference, label))
                references.emplace_back(label, std::move(*reference));
        };

        {
            Fixture fixture;
            fixture.addVisibleSolid(fixture.main);
            fixture.addZeroScaleShape(fixture.main);
            keep(fixture, "standalone zero-scale shape");
        }
        {
            Fixture fixture;
            fixture.addVisibleSolid(fixture.main);
            fixture.addZeroScaleText(fixture.main);
            keep(fixture, "standalone zero-scale text");
        }
        {
            Fixture fixture;
            fixture.addZeroScaleShape(fixture.main);
            keep(fixture, "only a zero-scale shape");
        }
        {
            Fixture fixture;
            const auto inner = fixture.addComposition("Inner");
            fixture.addVisibleSolid(inner);
            fixture.addZeroScaleShape(inner);
            fixture.addZeroScaleText(inner);
            fixture.addNested(fixture.main, inner);
            keep(fixture, "nested zero-scale shape and text");
        }
        {
            Fixture fixture;
            const auto inner = fixture.addComposition("Inner");
            fixture.addVisibleSolid(inner);
            fixture.addNested(fixture.main, inner);
            fixture.addZeroScaleShape(fixture.main);
            fixture.addZeroScaleText(fixture.main);
            keep(fixture, "zero-scale layers beside a nested composition");
        }

        // The diagnostic for an unconsumed vector source names the leaf node and the layer.
        {
            Fixture fixture;
            fixture.addVisibleSolid(fixture.main);
            const auto layer = fixture.addZeroScaleShape(fixture.main);
            const auto reference =
                cpuReference(expectations, fixture.document, fixture.main, "diagnostic");
            if (reference) {
                const auto count = reference.value().plan->operations().size();
                const std::vector<bool> reachable(count, true);
                const std::vector<bool> none(count, false);
                const auto message = runtime::detail::describeUnconsumedVectorLeaf(
                    *reference->plan, reachable, none, none);
                const auto shape =
                    lastNode(fixture.document, fixture.main, document::kShapeSourceNodeType);
                expectations.expect(
                    message.has_value() &&
                        message->find("Shape source node " + std::to_string(shape.value())) !=
                            std::string::npos &&
                        message->find("layer " + std::to_string(layer.value())) !=
                            std::string::npos,
                    "diagnostic: the fallback names the shape node and its layer: " +
                        message.value_or("(none)"));
                const std::vector<bool> all(count, true);
                expectations.expect(
                    !runtime::detail::describeUnconsumedVectorLeaf(*reference->plan, reachable, all,
                                                                   all)
                         .has_value(),
                    "diagnostic: nothing is reported when every leaf is accounted for");
            }
        }
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " zero-scale expectation(s) failed\n";
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
            std::cout << "SKIP: CPU half passed; no compatible Vulkan device available: "
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
            std::cerr << expectations.failures() << " zero-scale expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: zero-scale shape and text layers stay on the GPU route and match the "
                     "CPU reference\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
