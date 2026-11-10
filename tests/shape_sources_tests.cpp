#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
using namespace bloom;
namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
document::CompositionFormat testFormat() {
    const auto format = document::CompositionFormat::create(8, 8);
    if (!format)
        throw std::runtime_error("invalid test format");
    return *format;
}
struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Shapes", "Main", core::RationalTime::fromInteger(2), testFormat());
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    std::map<std::string, document::ParameterId> parameters;
    document::NodeId node;
    runtime::CpuCompositionEvaluator evaluator;
    explicit Fixture(document::ShapeKind kind) {
        commands::Transaction add("Add shape", document.snapshot().revision());
        add.emplace<commands::AddShapeLayer>(initial.initialCompositionId, kind);
        const auto result = commands.execute(std::move(add));
        if (!result.changed())
            throw std::runtime_error("shape creation failed");
        const auto created = result.outputId<document::NodeId>("shapeNode");
        if (!created)
            throw std::runtime_error("missing shape output");
        node = *created;
        const auto snapshot = document.snapshot();
        const auto* composition = snapshot.project().findComposition(initial.initialCompositionId);
        for (const auto& binding : composition->graph().findNode(node)->parameters)
            parameters[binding.role] = binding.parameterId;
        set("size", document::Vec2d{4, 4});
        if (kind == document::ShapeKind::Line) {
            set("lineEnd", document::Vec2d{4, 0});
            set("strokeWidth", 2.0);
        }
    }
    void set(const std::string& role, document::ParameterValue value) {
        commands::Transaction edit("Edit shape", document.snapshot().revision());
        edit.emplace<commands::SetParameterSource>(initial.initialCompositionId,
                                                   parameters.at(role),
                                                   document::ConstantValueSource{std::move(value)});
        if (!commands.execute(std::move(edit)).succeeded())
            throw std::runtime_error("shape edit failed: " + role);
    }
    std::shared_ptr<const runtime::CompiledCompositionPlan> compile() {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled =
            compiler.compile({document.snapshot(), initial.initialCompositionId}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << '\n';
            throw std::runtime_error("shape compilation failed");
        }
        return compiled.plan;
    }
    runtime::EvaluationResult evaluate(core::RationalTime time = {},
                                       std::size_t budget = 1U << 20U) {
        const auto plan = compile();
        return evaluator.evaluate(plan,
                                  {time, plan->output(), runtime::CompositionFormatResolution{},
                                   runtime::EvaluationQuality::Reference,
                                   runtime::EvaluationColorIntent::LinearRec709Scene, budget},
                                  {});
    }
};
float alpha(const runtime::EvaluationResult& result, int x, int y) {
    if (!result.frame()) {
        for (const auto& diagnostic : result.diagnostics())
            std::cerr << diagnostic.summary << '\n';
        ++failures;
        return -1;
    }
    const auto pixel = result.frame()->processImage().read(x, y);
    return pixel ? pixel.value()->alpha() : 0;
}
void pixelsAndBounds() {
    Fixture rectangle(document::ShapeKind::Rectangle);
    const auto rect = rectangle.evaluate();
    expect(alpha(rect, 2, 2) == 1 && alpha(rect, 1, 2) == 0, "rectangle pixel pin");
    expect(rect.frame() &&
               rect.frame()->evaluatedBounds()[0].local == runtime::ContentBounds{0, 0, 4, 4},
           "content-sized shape bounds");
    const auto cached = rectangle.evaluate();
    expect(alpha(cached, 2, 2) == 1 && cached.frame()->operationCacheStatistics().hits > 0,
           "cached shape equals uncached pixels");
    rectangle.set("fillColor", core::Color4d{1, 0, 0, 0.5});
    rectangle.set("strokeEnabled", true);
    rectangle.set("strokeWidth", 2.0);
    rectangle.set("strokeAlign", std::int64_t{1});
    rectangle.set("strokeColor", core::Color4d{0, 0, 1, 0.5});
    const auto layered = rectangle.evaluate();
    const auto pixel = layered.frame()->processImage().read(2, 2);
    expect(pixel && pixel.value()->alpha() == 0.75F && pixel.value()->red() == 0.25F &&
               pixel.value()->blue() == 0.5F,
           "stroke is premultiplied source-over on fill");
    Fixture circle(document::ShapeKind::Ellipse);
    const auto round = circle.evaluate();
    expect(std::abs(alpha(round, 2, 2) - 96.0F / 255.0F) < 1e-6F && alpha(round, 3, 3) == 1,
           "circle coverage pixel pin");
    Fixture star(document::ShapeKind::Star);
    const auto pointed = star.evaluate();
    expect(alpha(pointed, 2, 2) == 0 && std::abs(alpha(pointed, 3, 2) - 64.0F / 255.0F) < 1e-6F &&
               alpha(pointed, 3, 4) == 1,
           "five-point star pixel pins");
    Fixture line(document::ShapeKind::Line);
    const auto stroked = line.evaluate();
    expect(alpha(stroked, 2, 3) == 1 && alpha(stroked, 2, 2) == 0, "stroked line pixel pins");
    Fixture path(document::ShapeKind::Path);
    document::PathValue twice;
    twice.editableContour().closed = true;
    for (int loop = 0; loop < 2; ++loop)
        for (auto p : {document::Vec2d{0, 0}, document::Vec2d{4, 0}, document::Vec2d{4, 4},
                       document::Vec2d{0, 4}})
            twice.editableContour().anchors.push_back({p, {}, {}});
    path.set("path", twice);
    expect(alpha(path.evaluate(), 3, 3) == 1, "nonzero path pixel pin");
    path.set("fillRule", std::int64_t{1});
    expect(alpha(path.evaluate(), 3, 3) == 0, "evenodd path pixel pin and cache invalidation");
    expect(rectangle.evaluate({}, 1).status() == runtime::EvaluationStatus::Failed,
           "shape budget refusal");
}
void animation() {
    Fixture shape(document::ShapeKind::Rectangle);
    auto plan = shape.compile();
    expect(!plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "static shape is time independent");
    commands::Transaction animate("Animate shape", shape.document.snapshot().revision());
    animate.emplace<commands::CreateAnimationForParameter>(
        shape.initial.initialCompositionId, shape.parameters.at("size"), core::RationalTime{});
    animate.emplace<commands::SetKeyframeAtTimeForParameter>(
        shape.initial.initialCompositionId, shape.parameters.at("size"),
        core::RationalTime::fromInteger(1), document::Vec2d{6, 6});
    expect(shape.commands.execute(std::move(animate)).changed(),
           "shape size animates through normal commands");
    plan = shape.compile();
    expect(plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "animated shape is time dependent");
    expect(alpha(shape.evaluate(), 1, 1) == 0 &&
               alpha(shape.evaluate(core::RationalTime::fromInteger(1)), 1, 1) == 1,
           "animated size changes pixels");
}

bool sameImage(const runtime::EvaluationResult& a, const runtime::EvaluationResult& b) {
    if (!a.frame() || !b.frame())
        return false;
    const auto left = a.frame()->processImage().pixels();
    const auto right = b.frame()->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
}
bool shapeEvaluated(const runtime::EvaluationResult& result, const document::NodeId node) {
    if (!result.frame())
        return true;
    const auto& evaluated = result.frame()->operationCacheStatistics().evaluatedNodes;
    return std::ranges::find(evaluated, node) != evaluated.end();
}
// #599: a Line's two ends are animatable and drivable like `size`.
void keyEnd(Fixture& fixture, const std::string& role, const std::int64_t second,
            const document::Vec2d value, const bool create) {
    commands::Transaction animate("Key line end", fixture.document.snapshot().revision());
    if (create)
        animate.emplace<commands::CreateAnimationForParameter>(
            fixture.initial.initialCompositionId, fixture.parameters.at(role),
            core::RationalTime{});
    animate.emplace<commands::SetKeyframeAtTimeForParameter>(
        fixture.initial.initialCompositionId, fixture.parameters.at(role),
        core::RationalTime::fromInteger(second), value);
    expect(fixture.commands.execute(std::move(animate)).changed(), "line end keys through commands");
}
void lineEndsAnimate() {
    Fixture line(document::ShapeKind::Line);
    auto plan = line.compile();
    expect(!plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "constant line ends are time independent");
    const auto still = line.evaluate();
    // The two ends are keyed: (0,0)-(4,0) at 0 s, (0,0)-(0,4) at 1 s.
    keyEnd(line, "lineEnd", 0, document::Vec2d{4, 0}, true);
    keyEnd(line, "lineEnd", 1, document::Vec2d{0, 4}, false);
    keyEnd(line, "lineStart", 0, document::Vec2d{0, 0}, true);
    keyEnd(line, "lineStart", 1, document::Vec2d{0, 0}, false);
    plan = line.compile();
    expect(plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "a plan reading a keyed line end is time dependent");
    const auto first = line.evaluate();
    const auto half = line.evaluate(*core::RationalTime::create(1, 2));
    const auto last = line.evaluate(core::RationalTime::fromInteger(1));
    expect(sameImage(first, still), "the keyed line at its first key is the constant line");
    expect(!sameImage(first, half) && !sameImage(half, last) && !sameImage(first, last),
           "the keyed line moves between its keys");
    // The value read at 0.5 s is the interpolated end: a constant line at (0,0)-(2,2) is the same
    // picture, on the CPU reference and through the cache.
    Fixture constant(document::ShapeKind::Line);
    constant.set("lineEnd", document::Vec2d{2, 2});
    expect(sameImage(half, constant.evaluate()), "the keyed end is read at the frame's time");
    // Cache identity: the resolved ends are in the geometry key. Re-reading a frame reuses it, a
    // frame the keys changed re-evaluates, and an edit of a key invalidates exactly its frames.
    expect(!shapeEvaluated(line.evaluate(*core::RationalTime::create(1, 2)), line.node),
           "a frame whose ends are unchanged reuses the cached image");
    expect(shapeEvaluated(line.evaluate(core::RationalTime::fromInteger(1)), line.node) ||
               sameImage(last, line.evaluate(core::RationalTime::fromInteger(1))),
           "the last frame renders");
    keyEnd(line, "lineEnd", 1, document::Vec2d{0, 3}, false);
    const auto edited = line.evaluate(core::RationalTime::fromInteger(1));
    expect(shapeEvaluated(edited, line.node) && !sameImage(edited, last),
           "an edited key re-evaluates the frame it changes");
    expect(!shapeEvaluated(line.evaluate(), line.node) &&
               sameImage(line.evaluate(), first),
           "an edit at 1 s leaves 0 s cached");
    // A held key: equal neighbouring keys do not change the picture or the identity.
    Fixture held(document::ShapeKind::Line);
    keyEnd(held, "lineEnd", 0, document::Vec2d{3, 1}, true);
    keyEnd(held, "lineEnd", 1, document::Vec2d{3, 1}, false);
    const auto h0 = held.evaluate();
    const auto h1 = held.evaluate(core::RationalTime::fromInteger(1));
    expect(sameImage(h0, h1), "a held line end draws the same picture at both keys");
    // The CPU reference keys a time-dependent operation by its time as well as its resolved values
    // (as it does for `size`), so a held end reuses at the same frame; the GPU geometry key holds
    // across frames (tests/integration/gpu_shape_line_ends_native_tests.cpp).
    expect(!shapeEvaluated(held.evaluate(core::RationalTime::fromInteger(1)), held.node),
           "a held line end reuses the cached image at a frame it has rendered");
}
void lineEndsDriven() {
    Fixture line(document::ShapeKind::Line);
    commands::Transaction add("Add vector", line.document.snapshot().revision());
    add.emplace<commands::AddNode>(line.initial.initialCompositionId,
                                   std::string(document::kVector2ValueNodeType),
                                   document::Vec2d{0, 0});
    const auto added = line.commands.execute(std::move(add));
    const auto producer = added.outputId<document::NodeId>(commands::kAddNodeOutput);
    expect(added.changed() && producer.has_value(), "a Vector2 value node is added");
    if (!producer)
        return;
    const auto connect = [&](const std::string& socket) {
        commands::Transaction wire("Drive line end", line.document.snapshot().revision());
        wire.emplace<commands::ConnectPorts>(
            line.initial.initialCompositionId,
            document::OutputPortRef{*producer, std::string(document::kValuePortName)},
            document::NodeInputRef{line.node, socket});
        return line.commands.execute(std::move(wire)).changed();
    };
    expect(connect("lineEnd"), "a Vector2 value drives lineEnd");
    const auto valueParameter = [&] {
        const auto snapshot = line.document.snapshot();
        const auto* composition = snapshot.project().findComposition(line.initial.initialCompositionId);
        for (const auto& binding : composition->graph().findNode(*producer)->parameters)
            if (binding.role == document::kValueParameterRole)
                return binding.parameterId;
        throw std::runtime_error("Vector2 value parameter is missing");
    }();
    const auto setValue = [&](const document::Vec2d value) {
        commands::Transaction edit("Set vector", line.document.snapshot().revision());
        edit.emplace<commands::SetParameterSource>(line.initial.initialCompositionId, valueParameter,
                                                   document::ConstantValueSource{value});
        return line.commands.execute(std::move(edit)).changed();
    };
    expect(setValue(document::Vec2d{2, 2}), "the driver value is edited");
    const auto driven = line.evaluate();
    // A different driver value moves the end, the picture and re-evaluates the shape.
    expect(setValue(document::Vec2d{0, 4}), "the driver value changes");
    const auto moved = line.evaluate();
    expect(!sameImage(driven, moved) && shapeEvaluated(moved, line.node),
           "a driven end change re-evaluates the shape");
    expect(!shapeEvaluated(line.evaluate(), line.node) && sameImage(line.evaluate(), moved),
           "an unchanged driven end reuses the cached image");
    // A driven lineEnd equals the same constant end.
    Fixture constant(document::ShapeKind::Line);
    constant.set("lineEnd", document::Vec2d{0, 4});
    expect(sameImage(moved, constant.evaluate()), "a driven end draws as the same constant end");
}
void creationAndPathOverrides() {
    auto initial = document::makeNewProject("Creation", "Main", core::RationalTime::fromInteger(2),
                                            testFormat());
    document::Document document(std::move(initial.project));
    commands::CommandStack stack(document);
    const auto triangle = document::PathValue::fromContour(
        {{{2, 2}, {}, {}}, {{6, 2}, {}, {}}, {{4, 6}, {}, {}}}, true);
    commands::ShapeLayerGeometry geometry;
    geometry.position = document::Vec2d{4, 4};
    geometry.path = triangle;
    commands::Transaction add("Draw Path", document.snapshot().revision());
    add.emplace<commands::AddShapeLayer>(initial.initialCompositionId, document::ShapeKind::Path,
                                         geometry);
    const auto created = stack.execute(std::move(add));
    expect(created.changed() && stack.size() == 1,
           "path creation and geometry are one transaction");
    const auto pathId = created.outputId<document::ParameterId>("path");
    if (!pathId)
        throw std::runtime_error("missing created path");
    const auto snapshot = document.snapshot();
    const auto* composition = snapshot.project().findComposition(initial.initialCompositionId);
    expect(std::get<document::ConstantValueSource>(composition->parameters().find(*pathId)->source)
                   .value == document::ParameterValue(triangle),
           "creation stores complete path value");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto edited = triangle;
    edited.editableContour().anchors.front().point = {1, 2};
    const auto preview = compiler.compile(
        {snapshot, initial.initialCompositionId, {{snapshot.revision(), *pathId, edited}}}, {});
    expect(preview.plan != nullptr, "Path override admitted on reachable shape source");
    expect(std::get<document::ConstantValueSource>(composition->parameters().find(*pathId)->source)
                   .value == document::ParameterValue(triangle),
           "Path override does not mutate snapshot");
    auto invalid = edited;
    invalid.editableContour().anchors.front().point.x = std::numeric_limits<double>::infinity();
    expect(!compiler
                .compile({snapshot,
                          initial.initialCompositionId,
                          {{snapshot.revision(), *pathId, invalid}}},
                         {})
                .plan,
           "nonfinite Path override refused");
    expect(!compiler
                .compile(
                    {snapshot, initial.initialCompositionId, {{snapshot.revision(), *pathId, 1.0}}},
                    {})
                .plan,
           "wrong override kind refused for path");
    expect(stack.undo().changed(), "one undo removes drawn path");
    expect(stack.redo().changed(), "one redo restores drawn path");
    const auto count = stack.size();
    commands::ShapeLayerGeometry invalidGeometry;
    invalidGeometry.size = document::Vec2d{-1, 10};
    commands::Transaction bad("Invalid shape", document.snapshot().revision());
    bad.emplace<commands::AddShapeLayer>(initial.initialCompositionId,
                                         document::ShapeKind::Rectangle, invalidGeometry);
    expect(!stack.execute(std::move(bad)).succeeded() && stack.size() == count,
           "invalid initial shape geometry is rejected atomically");
}
void transformedStroke() {
    Fixture fixture(document::ShapeKind::Line);
    auto definition = fixture.compile()->copyDefinition();
    for (auto& operation : definition.operations)
        if (auto* layer = std::get_if<runtime::CompiledLayerOutput>(&operation))
            layer->scale.source = document::Vec2d{2, 2};
    const auto plan =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    const auto result =
        fixture.evaluator.evaluate(plan,
                                   {{},
                                    plan->output(),
                                    runtime::CompositionFormatResolution{},
                                    runtime::EvaluationQuality::Reference,
                                    runtime::EvaluationColorIntent::LinearRec709Scene,
                                    1U << 20U},
                                   {});
    expect(alpha(result, 4, 1) == 0 && alpha(result, 4, 2) == 1 && alpha(result, 4, 5) == 1 &&
               alpha(result, 4, 6) == 0,
           "transform Scale doubles the native two-unit stroke without softening its edge");
}
} // namespace
int main() {
    try {
        transformedStroke();
        pixelsAndBounds();
        animation();
        lineEndsAnimate();
        lineEndsDriven();
        creationAndPathOverrides();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures ? 1 : 0;
}
