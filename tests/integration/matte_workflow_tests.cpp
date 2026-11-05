// #383 matte workflow on a real compiled document, evaluated by the CPU reference: Mask Combine,
// Image to Mask, the effect mask, matte layer masks (alone and among path masks) and node track
// mattes, with their commands, refusals and node removal. A 16x16 composition of full-frame solids
// reads back the coverage in alpha and an effect's reach in red.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <cmath>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
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
bool near(const float value, const float expected, const float tolerance = 1e-4F) {
    return std::fabs(value - expected) <= tolerance;
}

document::PathValue rectangle(const double left, const double top, const double right,
                              const double bottom) {
    document::PathValue path;
    path.closed = true;
    for (const auto& [x, y] : {std::pair{left, top}, std::pair{right, top},
                               std::pair{right, bottom}, std::pair{left, bottom}})
        path.anchors.push_back({{x, y}, std::nullopt, std::nullopt});
    return path;
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Mattes", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(16, 16, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Mattes", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            for (const auto& failure : result.operationFailures)
                std::cerr << failure.operationType << ": " << failure.issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
        return result;
    }
    template <typename Operation, typename... Arguments> bool refused(Arguments&&... arguments) {
        commands::Transaction transaction("Mattes", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        return !commands.execute(std::move(transaction)).succeeded();
    }
    const document::CanonicalGraph& graph() const {
        return document.snapshot().project().findComposition(composition)->graph();
    }
    document::ParameterId parameterOf(const document::NodeId node, const std::string_view role) {
        for (const auto& binding : graph().findNode(node)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("missing parameter " + std::string(role));
    }
    void set(const document::ParameterId id, document::ParameterValue value) {
        run<commands::SetParameterSource>(composition, id,
                                          document::ConstantValueSource{std::move(value)});
    }
    document::NodeId node(const std::string_view type) {
        return *run<commands::AddNode>(composition, std::string(type), document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    void connect(const document::NodeId from, const std::string& port, const document::NodeId to,
                 const std::string& input) {
        run<commands::ConnectPorts>(composition, document::OutputPortRef{from, port},
                                    document::NodeInputRef{to, input});
    }
    struct Layer {
        document::LayerId id;
        document::NodeId layerNode;
        document::NodeId solid;
    };
    Layer solid(const core::Color4d color) {
        const auto result = run<commands::AddSolidLayer>(composition, "Solid", color);
        const auto solidNode =
            *result.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        set(parameterOf(solidNode, "width"), 16.0);
        set(parameterOf(solidNode, "height"), 16.0);
        const auto id = *result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        return {id, graph().findLayer(id)->nodeId, solidNode};
    }
    document::NodeId mask(document::PathValue path) {
        const auto id = node(document::kMaskNodeType);
        set(parameterOf(id, "path"), std::move(path));
        return id;
    }
    std::vector<render::Rgba32f> pixels() {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({document.snapshot(), composition, {}}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("matte compilation failed");
        }
        runtime::CpuCompositionEvaluator evaluator;
        const auto result = evaluator.evaluate(
            compiled.plan,
            {{}, compiled.plan->output(), runtime::CompositionFormatResolution{},
             runtime::EvaluationQuality::Reference,
             runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 20U},
            {});
        if (!result.frame()) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("matte evaluation failed");
        }
        const auto span = result.frame()->processImage().pixels();
        return {span.begin(), span.end()};
    }
};

float alphaAt(const std::vector<render::Rgba32f>& pixels, const int x, const int y = 8) {
    return pixels[static_cast<std::size_t>(y * 16 + x)].alpha();
}
float redAt(const std::vector<render::Rgba32f>& pixels, const int x, const int y = 8) {
    return pixels[static_cast<std::size_t>(y * 16 + x)].red();
}

// Two overlapping Mask nodes (left 0..8, middle 4..12) through Mask Combine into a matte mask.
void maskCombine() {
    Fixture f;
    const auto layer = f.solid({1, 1, 1, 1});
    const auto a = f.mask(rectangle(0, 0, 8, 16));
    const auto b = f.mask(rectangle(4, 0, 12, 16));
    const auto combine = f.node(document::kMaskCombineNodeType);
    f.connect(a, "mask", combine, "a");
    f.connect(b, "mask", combine, "b");
    f.run<commands::AddLayerMask>(f.composition, layer.id, document::PathValue{},
                                  document::LayerMaskMode::Add, std::string{},
                                  document::OutputPortRef{combine, "mask"});
    expect(f.document.snapshot().project().validate().ok(), "a matte mask validates");
    struct Case {
        std::int64_t mode;
        std::array<float, 4> expected; // x = 2, 6, 10, 14
        const char* name;
    };
    for (const auto& [mode, expected, name] :
         {Case{0, {1, 1, 1, 0}, "Add is the union"}, Case{1, {1, 0, 0, 0}, "Subtract cuts b"},
          Case{2, {0, 1, 0, 0}, "Intersect keeps the overlap"},
          Case{3, {1, 0, 1, 0}, "Difference is exclusive"}}) {
        f.set(f.parameterOf(combine, "mode"), mode);
        const auto pixels = f.pixels();
        bool ok = true;
        for (int i = 0; i < 4; ++i)
            ok = ok && near(alphaAt(pixels, 2 + 4 * i), expected[static_cast<std::size_t>(i)]);
        expect(ok, std::string("Mask Combine: ") + name);
    }
    // An unconnected input is zero coverage: Add with only b is b.
    f.run<commands::DisconnectInput>(f.composition, document::NodeInputRef{combine, "a"});
    f.set(f.parameterOf(combine, "mode"), std::int64_t{0});
    auto pixels = f.pixels();
    expect(near(alphaAt(pixels, 2), 0) && near(alphaAt(pixels, 6), 1),
           "an unconnected Mask Combine input is empty");
    // Muted, Mask Combine carries its first Mask through (here none: empty).
    f.connect(a, "mask", combine, "a");
    f.run<commands::SetNodeMuted>(f.composition, combine, true);
    pixels = f.pixels();
    expect(near(alphaAt(pixels, 2), 1) && near(alphaAt(pixels, 10), 0),
           "a muted Mask Combine passes its first Mask through");
}

// Matte masks among path masks: modes, order, invert and opacity.
void matteAmongPathMasks() {
    Fixture f;
    const auto layer = f.solid({1, 1, 1, 1});
    const auto left = f.mask(rectangle(0, 0, 8, 16));
    f.run<commands::AddLayerMask>(f.composition, layer.id, rectangle(0, 0, 16, 8));
    const auto matte = *f.run<commands::AddLayerMask>(f.composition, layer.id,
                                                      document::PathValue{},
                                                      document::LayerMaskMode::Intersect,
                                                      std::string{},
                                                      document::OutputPortRef{left, "mask"})
                            .outputId<document::LayerMaskId>("mask");
    auto pixels = f.pixels();
    expect(near(alphaAt(pixels, 2, 2), 1) && near(alphaAt(pixels, 12, 2), 0) &&
               near(alphaAt(pixels, 2, 12), 0),
           "a top-half path mask intersected with a left-half matte keeps the top left");
    f.run<commands::SetLayerMaskInverted>(f.composition, layer.id, matte, true);
    pixels = f.pixels();
    expect(near(alphaAt(pixels, 2, 2), 0) && near(alphaAt(pixels, 12, 2), 1),
           "an inverted matte mask covers the outside of its Mask");
    f.run<commands::SetLayerMaskInverted>(f.composition, layer.id, matte, false);
    f.set(f.graph().findLayer(layer.id)->masks.back().opacity, 0.5);
    pixels = f.pixels();
    expect(near(alphaAt(pixels, 2, 2), 0.5F), "a matte mask's opacity scales its coverage");
    // A matte mask first with Subtract starts from full coverage.
    Fixture g;
    const auto other = g.solid({1, 1, 1, 1});
    const auto hole = g.mask(rectangle(4, 4, 12, 12));
    g.run<commands::AddLayerMask>(g.composition, other.id, document::PathValue{},
                                  document::LayerMaskMode::Subtract, std::string{},
                                  document::OutputPortRef{hole, "mask"});
    pixels = g.pixels();
    expect(near(alphaAt(pixels, 1, 1), 1) && near(alphaAt(pixels, 8, 8), 0),
           "a first Subtract matte mask cuts a hole in full coverage");
    // Switching the mask back to its (empty) path, then to the matte again.
    const auto id = g.graph().findLayer(other.id)->masks.front().id;
    g.run<commands::SetLayerMaskMatte>(g.composition, other.id, id, std::nullopt);
    pixels = g.pixels();
    expect(near(alphaAt(pixels, 8, 8), 1), "an empty path subtracts nothing");
    g.run<commands::SetLayerMaskMatte>(g.composition, other.id, id,
                                       document::OutputPortRef{hole, "mask"});
    // Removing the node removes its matte masks and their parameters.
    g.run<commands::RemoveNodes>(g.composition, std::set<document::NodeId>{hole});
    expect(g.graph().findLayer(other.id)->masks.empty(),
           "a removed node takes its matte masks with it");
    expect(g.document.snapshot().project().validate().ok(),
           "the project validates after the matte node is removed");
    pixels = g.pixels();
    expect(near(alphaAt(pixels, 8, 8), 1), "the layer is unmasked again");
}

// Image to Mask over the canvas, every channel and inverted.
void imageToMask() {
    Fixture f;
    const auto layer = f.solid({1, 1, 1, 1});
    const auto source = f.node(document::kSolidSourceNodeType);
    f.set(f.parameterOf(source, "color"), core::Color4d{0.25, 0.5, 1.0, 1.0});
    f.set(f.parameterOf(source, "width"), 16.0);
    f.set(f.parameterOf(source, "height"), 16.0);
    const auto convert = f.node(document::kImageToMaskNodeType);
    f.connect(source, "image", convert, "input");
    f.run<commands::AddLayerMask>(f.composition, layer.id, document::PathValue{},
                                  document::LayerMaskMode::Add, std::string{},
                                  document::OutputPortRef{convert, "mask"});
    for (const auto& [channel, expected] :
         {std::pair{std::int64_t{0}, 1.0F}, std::pair{std::int64_t{1}, 0.25F},
          std::pair{std::int64_t{2}, 0.5F}, std::pair{std::int64_t{6}, 0.75F},
          std::pair{std::int64_t{5}, 0.0F}}) {
        f.set(f.parameterOf(convert, "channel"), channel);
        expect(near(alphaAt(f.pixels(), 8), expected),
               "Image to Mask channel " + std::to_string(channel));
    }
}

// The effect mask: an Exposure of +1 stop on a red solid, limited to the left half.
void effectMask() {
    Fixture f;
    const auto layer = f.solid({1, 0, 0, 1});
    const auto exposure = f.node("bloom.exposure");
    f.set(f.parameterOf(exposure, "amount"), 1.0);
    f.connect(layer.solid, "image", exposure, "input");
    f.connect(exposure, "image", layer.layerNode, "image");
    auto pixels = f.pixels();
    expect(near(redAt(pixels, 2), 2) && near(redAt(pixels, 12), 2), "unmasked, Exposure is whole");
    const auto left = f.mask(rectangle(0, 0, 8, 16));
    f.connect(left, "mask", exposure, "mask");
    pixels = f.pixels();
    expect(near(redAt(pixels, 2), 2) && near(redAt(pixels, 12), 1) &&
               near(alphaAt(pixels, 12), 1),
           "a mask limits the effect to its coverage");
    f.set(f.parameterOf(exposure, std::string(document::kEffectMaskChannelRole)), std::int64_t{5});
    pixels = f.pixels();
    expect(near(redAt(pixels, 2), 1) && near(redAt(pixels, 12), 2),
           "an inverted mask selector applies the effect outside");
    // An Image is read through the selector: a solid whose red is 0.5 half-applies the effect.
    const auto half = f.node(document::kSolidSourceNodeType);
    f.set(f.parameterOf(half, "color"), core::Color4d{0.5, 0.0, 0.0, 1.0});
    f.set(f.parameterOf(half, "width"), 16.0);
    f.set(f.parameterOf(half, "height"), 16.0);
    f.connect(half, "image", exposure, "mask");
    f.set(f.parameterOf(exposure, std::string(document::kEffectMaskChannelRole)), std::int64_t{1});
    pixels = f.pixels();
    expect(near(redAt(pixels, 8), 1.5F), "an Image mask's red channel is its coverage");
    // A muted effect ignores its mask.
    f.run<commands::SetNodeMuted>(f.composition, exposure, true);
    expect(near(redAt(f.pixels(), 8), 1), "a muted effect passes its input");

    // A filter keeps its own result as the base: a Gaussian Blur of the full-frame solid with a
    // transparent edge, masked to the left half, softens only the left border.
    Fixture g;
    const auto blurred = g.solid({1, 0, 0, 1});
    const auto blur = g.node("bloom.gaussian-blur");
    g.set(g.parameterOf(blur, "radius"), 3.0);
    g.connect(blurred.solid, "image", blur, "input");
    g.connect(blur, "image", blurred.layerNode, "image");
    g.connect(g.mask(rectangle(0, 0, 8, 16)), "mask", blur, "mask");
    pixels = g.pixels();
    expect(alphaAt(pixels, 0) < 0.99F && near(alphaAt(pixels, 15), 1.0F, 1e-6F),
           "a masked filter applies inside its mask and passes its input outside");
}

// A node track matte: composition space, Alpha and inverted, cleared with its node.
void nodeTrackMatte() {
    Fixture f;
    const auto layer = f.solid({1, 1, 1, 1});
    const auto left = f.mask(rectangle(0, 0, 8, 16));
    f.run<commands::SetTrackMatte>(
        f.composition, layer.id,
        document::TrackMatte{document::LayerId{}, document::TrackMatteChannel::Alpha, false,
                             document::OutputPortRef{left, "mask"}});
    expect(f.document.snapshot().project().validate().ok(), "a node track matte validates");
    auto pixels = f.pixels();
    expect(near(alphaAt(pixels, 2), 1) && near(alphaAt(pixels, 12), 0),
           "a node track matte keeps the target inside its Mask");
    f.run<commands::SetTrackMatte>(
        f.composition, layer.id,
        document::TrackMatte{document::LayerId{}, document::TrackMatteChannel::Luma, true,
                             document::OutputPortRef{left, "mask"}});
    pixels = f.pixels();
    expect(near(alphaAt(pixels, 2), 0) && near(alphaAt(pixels, 12), 1),
           "an inverted Luma node matte keeps the outside");
    // Refusals: an Image output, a missing node, a node computed from the layer itself.
    expect(f.refused<commands::SetTrackMatte>(
               f.composition, layer.id,
               document::TrackMatte{document::LayerId{}, document::TrackMatteChannel::Alpha, false,
                                    document::OutputPortRef{layer.solid, "image"}}),
           "an Image output is not a matte");
    const auto own = f.node(document::kImageToMaskNodeType);
    f.connect(layer.layerNode, "image", own, "input");
    expect(f.refused<commands::AddLayerMask>(f.composition, layer.id, document::PathValue{},
                                             document::LayerMaskMode::Add, std::string{},
                                             document::OutputPortRef{own, "mask"}),
           "a matte computed from the layer it masks is refused");
    f.run<commands::RemoveNodes>(f.composition, std::set<document::NodeId>{left});
    expect(!f.graph().findLayer(layer.id)->trackMatte.has_value(),
           "removing the matte node clears the node track matte");
    expect(near(alphaAt(f.pixels(), 12), 1), "the target is whole again");
}

} // namespace

int main() {
    try {
        maskCombine();
        matteAmongPathMasks();
        imageToMask();
        effectMask();
        nodeTrackMatte();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "matte workflow PASS\n";
    return failures == 0 ? 0 : 1;
}
