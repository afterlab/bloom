// #774: the bounds a path layer reports are the bounds of its drawn ink after trim, stroke and
// transform. The reference is the alpha of the delivered CPU frame itself: every layer here is
// the only thing in its composition, so its rendered coverage is the layer's ink.

#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <vector>

namespace {

using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

constexpr std::uint32_t kSide = 1000;

struct Ink final {
    double left = 0, top = 0, right = 0, bottom = 0;
    bool any = false;
};

struct Fixture final {
    std::unique_ptr<scripting::Session> session;
    document::CompositionId composition;
    document::LayerId layer;
    document::NodeId shapeNode;
    document::ParameterId rotation;
    document::ParameterId scaleParameter;

    Fixture() {
        auto created =
            scripting::Session::createNew("Path", "Main", core::RationalTime::fromInteger(10),
                                          document::CompositionFormat::create(kSide, kSide)
                                              .value_or(document::CompositionFormat{}));
        if (!created)
            std::abort();
        session = std::move(created).takeSession();
        composition = session->snapshot().project().compositions().front().id();
    }

    template <typename Operation, typename... Arguments>
    scripting::SessionCommandResult run(Arguments&&... args) {
        commands::Transaction transaction("Fixture", session->snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(args)...);
        return session->execute(std::move(transaction));
    }

    [[nodiscard]] const document::Composition& current() const {
        return *session->snapshot().project().findComposition(composition);
    }
    [[nodiscard]] document::ParameterId parameter(const document::NodeId node,
                                                  const std::string_view role) const {
        if (const auto* record = current().graph().findNode(node))
            for (const auto& binding : record->parameters)
                if (binding.role == role)
                    return binding.parameterId;
        return {};
    }
    void set(const document::NodeId node, const std::string_view role,
             document::ParameterValue value) {
        expect(run<commands::SetParameterSource>(composition, parameter(node, role),
                                                 document::ConstantValueSource{std::move(value)})
                   .succeeded(),
               "a parameter is set");
    }

    void addPath(const document::PathValue& path, const double strokeWidth, const bool fill,
                 const double rotationDegrees) {
        commands::ShapeLayerGeometry geometry;
        geometry.path = path;
        commands::ShapeLayerStyle style;
        style.fillEnabled = fill;
        style.fillColor = core::Color4d{1, 1, 1, 1};
        style.strokeEnabled = strokeWidth > 0;
        style.strokeColor = core::Color4d{1, 1, 1, 1};
        style.strokeWidth = strokeWidth;
        const auto added =
            run<commands::AddShapeLayer>(composition, document::ShapeKind::Path, geometry, style);
        expect(added.succeeded() && added.command.has_value(), "the path layer is added");
        const auto& result = *added.command;
        layer = result.outputId<document::LayerId>("layer").value();
        shapeNode = result.outputId<document::NodeId>("shapeNode").value();
        rotation = result.outputId<document::ParameterId>("rotationParameter").value();
        scaleParameter = result.outputId<document::ParameterId>("scaleParameter").value();
        expect(run<commands::SetParameterSource>(composition, rotation,
                                                 document::ConstantValueSource{rotationDegrees})
                   .succeeded(),
               "the layer is rotated");
    }

    // Trim Path between a path value node and the shape: keeps [start, end] percent of the length.
    void trim(const document::PathValue& path, const double start, const double end) {
        expect(run<commands::AddNode>(composition, std::string(document::kPathValueNodeType),
                                      document::Vec2d{})
                   .succeeded(),
               "a path value node is added");
        document::NodeId source;
        for (const auto& node : current().graph().nodes())
            if (node.typeId == document::kPathValueNodeType)
                source = node.id;
        expect(run<commands::AddNode>(composition, std::string(document::kTrimPathNodeType),
                                      document::Vec2d{})
                   .succeeded(),
               "a Trim Path node is added");
        document::NodeId trimNode;
        for (const auto& node : current().graph().nodes())
            if (node.typeId == document::kTrimPathNodeType)
                trimNode = node.id;
        set(source, document::kValueParameterRole, path);
        set(trimNode, "start", start);
        set(trimNode, "end", end);
        expect(run<commands::ConnectPorts>(
                   composition,
                   document::OutputPortRef{source, std::string(document::kValuePortName)},
                   document::NodeInputRef{trimNode, "path"})
                   .succeeded(),
               "the path feeds Trim Path");
        expect(run<commands::ConnectPorts>(composition, document::OutputPortRef{trimNode, "path"},
                                           document::NodeInputRef{shapeNode, "path"})
                   .succeeded(),
               "Trim Path drives the shape");
    }

    void scale(const document::Vec2d factor) {
        expect(run<commands::SetParameterSource>(composition, scaleParameter,
                                                 document::ConstantValueSource{factor})
                   .succeeded(),
               "the layer is scaled");
    }

    // The reported bounds and the rendered alpha's bounds of the layer.
    void compare(const std::string& label) {
        const auto snapshot = session->snapshot();
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const runtime::CpuCompositionEvaluator evaluator;
        const auto reported = runtime::queryLayerBounds(
            compiler, evaluator,
            {.snapshot = snapshot, .composition = composition, .time = {}, .layers = {}});
        expect(reported.succeeded() && reported.bounds.size() == 1,
               label + ": the query reports the layer: " + reported.diagnostic);
        const auto compiled =
            compiler.compile({.snapshot = snapshot, .compositionId = composition}, {});
        expect(compiled.plan != nullptr, label + ": the plan compiles");
        if (!reported.succeeded() || reported.bounds.empty() || compiled.plan == nullptr)
            return;
        const auto frame = evaluator.evaluate(
            compiled.plan,
            runtime::EvaluationRequest{
                .time = {},
                .output = compiled.plan->output(),
                .resolution = runtime::CompositionFormatResolution{},
                .quality = runtime::EvaluationQuality::Reference,
                .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                .pixelStorageByteLimit = runtime::kDefaultLayerBoundsPixelBudget},
            {});
        expect(frame.frame() != nullptr, label + ": the frame renders");
        if (frame.frame() == nullptr)
            return;
        const auto& image = frame.frame()->processImage();
        const auto window = image.descriptor()->dataWindow();
        const auto width = static_cast<std::int64_t>(window.extent().width());
        const auto pixels = image.pixels();
        Ink ink;
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            if (!(pixels[index].alpha() > 0.0F))
                continue;
            const auto flat = static_cast<std::int64_t>(index);
            const auto x = static_cast<double>(window.originX() + flat % width);
            const auto y = static_cast<double>(window.originY() + flat / width);
            if (!ink.any) {
                ink = {x, y, x + 1, y + 1, true};
            } else {
                ink.left = std::min(ink.left, x);
                ink.top = std::min(ink.top, y);
                ink.right = std::max(ink.right, x + 1);
                ink.bottom = std::max(ink.bottom, y + 1);
            }
        }
        expect(ink.any, label + ": the layer draws ink");
        // The GPU scene preparation reports the same box.
        const auto prepared = runtime::CpuGpuSceneBuilder{}.build(
            compiled.plan, {.time = {},
                            .output = compiled.plan->output(),
                            .resolution = runtime::CompositionFormatResolution{},
                            .pixelStorageByteLimit = runtime::kDefaultLayerBoundsPixelBudget});
        expect(prepared.hasValue(), label + ": the GPU scene prepares on the host");
        if (prepared) {
            const auto scene = runtime::layerOutputBounds(*compiled.plan, prepared.scene->bounds());
            expect(scene.size() == 1 &&
                       std::abs(scene[0].output.left - reported.bounds[0].output.left) < 1e-6 &&
                       std::abs(scene[0].output.top - reported.bounds[0].output.top) < 1e-6 &&
                       std::abs(scene[0].output.right - reported.bounds[0].output.right) < 1e-6 &&
                       std::abs(scene[0].output.bottom - reported.bounds[0].output.bottom) < 1e-6,
                   label + ": the GPU scene preparation reports the same bounds");
        }
        const auto& out = reported.bounds.front().output;
        std::cerr << label << ": reported " << out.left << ' ' << out.top << ' ' << out.right << ' '
                  << out.bottom << " ink " << ink.left << ' ' << ink.top << ' ' << ink.right << ' '
                  << ink.bottom << '\n';
        const auto within = [](const double a, const double b) { return std::abs(a - b) <= 1.0; };
        expect(within(out.left, ink.left) && within(out.top, ink.top) &&
                   within(out.right, ink.right) && within(out.bottom, ink.bottom),
               label + ": the reported bounds are the ink's within one pixel");
    }
};

document::PathValue ring(const double cx, const double cy, const double r, const bool closed) {
    constexpr double k = 0.5522847498;
    document::PathValue path;
    path.editableContour().closed = closed;
    using document::PathAnchor;
    using document::Vec2d;
    path.editableContour().anchors = {
        PathAnchor{Vec2d{cx + r, cy}, Vec2d{cx + r, cy - k * r}, Vec2d{cx + r, cy + k * r}},
        PathAnchor{Vec2d{cx, cy + r}, Vec2d{cx + k * r, cy + r}, Vec2d{cx - k * r, cy + r}},
        PathAnchor{Vec2d{cx - r, cy}, Vec2d{cx - r, cy + k * r}, Vec2d{cx - r, cy - k * r}},
        PathAnchor{Vec2d{cx, cy - r}, Vec2d{cx - k * r, cy - r}, Vec2d{cx + k * r, cy - r}}};
    return path;
}

document::PathValue wave(const double cx, const double cy) {
    document::PathValue path;
    path.editableContour().closed = false;
    using document::PathAnchor;
    using document::Vec2d;
    path.editableContour().anchors = {
        PathAnchor{Vec2d{cx - 220, cy + 40}, std::nullopt, Vec2d{cx - 140, cy - 160}},
        PathAnchor{Vec2d{cx, cy + 30}, Vec2d{cx - 60, cy + 150}, Vec2d{cx + 70, cy - 90}},
        PathAnchor{Vec2d{cx + 230, cy - 20}, Vec2d{cx + 150, cy + 110}, std::nullopt}};
    return path;
}

void testRotatedPaths() {
    for (const double angle : {0.0, 30.0, 45.0, 90.0}) {
        const std::string suffix = " at " + std::to_string(static_cast<int>(angle)) + " degrees";
        {
            Fixture f;
            f.addPath(ring(500, 500, 200, true), 14.0, false, angle);
            f.compare("a stroked closed ring" + suffix);
        }
        {
            Fixture f;
            f.addPath(ring(500, 500, 200, true), 0.0, true, angle);
            f.compare("a filled closed ring" + suffix);
        }
        {
            Fixture f;
            f.addPath(wave(500, 500), 18.0, false, angle);
            f.compare("a stroked open curve" + suffix);
        }
    }
}

void testScaledRotatedPaths() {
    for (const double angle : {20.0, 60.0}) {
        const std::string suffix = " at " + std::to_string(static_cast<int>(angle)) + " degrees";
        {
            Fixture f;
            f.addPath(ring(500, 500, 200, true), 14.0, false, angle);
            f.scale({1.6, 0.6});
            f.compare("a stroked ring scaled unevenly" + suffix);
        }
        {
            Fixture f;
            const auto path = wave(500, 500);
            f.addPath(path, 18.0, false, angle);
            f.trim(path, 10.0, 55.0);
            f.scale({0.7, 1.4});
            f.compare("a trimmed curve scaled unevenly" + suffix);
        }
    }
}

void testRotatedTrimmedPaths() {
    for (const double angle : {0.0, 30.0, 45.0, 135.0}) {
        const std::string suffix = " at " + std::to_string(static_cast<int>(angle)) + " degrees";
        {
            Fixture f;
            const auto path = ring(500, 500, 200, true);
            f.addPath(path, 14.0, false, angle);
            f.trim(path, 0.0, 30.0);
            f.compare("a ring trimmed to its first 30 percent" + suffix);
        }
        {
            Fixture f;
            const auto path = ring(500, 500, 200, true);
            f.addPath(path, 14.0, false, angle);
            f.trim(path, 20.0, 60.0);
            f.compare("a ring trimmed from 20 to 60 percent" + suffix);
        }
        {
            Fixture f;
            const auto path = wave(500, 500);
            f.addPath(path, 18.0, false, angle);
            f.trim(path, 10.0, 55.0);
            f.compare("an open curve trimmed from 10 to 55 percent" + suffix);
        }
    }
}

} // namespace

int main() {
    testRotatedPaths();
    testRotatedTrimmedPaths();
    testScaledRotatedPaths();
    return failures == 0 ? 0 : 1;
}
