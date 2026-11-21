// #591 Coordinate spaces on real documents (docs/architecture/coordinate-spaces.md).
//
// A graph scene is built through commands: one Coordinate Space whose origin and size per unit are
// keyed Vector2 values, a Function Plot (y = x / 2) and an Axes drawn by two Shape Sources that are
// entries of the composition's Merge (so their paths' pixels are frame pixels), an Axis Tick whose
// label drives a Text layer's words and Position, two Solid layers placed in the space by Place in
// Space, and a Space Readout. Every node reads the one space.
//
// Over several frames of the animated space:
//   - the readouts equal the map the keyed values define (origin, axes, scale, rotation, matrix);
//   - the plot's anchors, the axes' anchors, the tick's position and both placements are the map of
//     their own units, computed independently here;
//   - in the CPU reference frame each dot's centroid lands on its point, the plot's stroke and the
//     x axis are under probes taken through the map, and the label moves with its mark;
//   - the production GPU scene (CpuGpuSceneBuilder -> GpuSceneExecutor -> one readback) matches the
//     CPU reference at 2e-6 with exact alpha, cold and warm, full frame, region of interest and
//     reduced render scale; a held frame dispatches nothing new, a moved frame and an edited key
//     do; preparation and the native job cancel; the budget refuses; the scene cache retires.
// A save and reopen evaluates the same frame, and undo of the space's link restores the plot's own
// placement.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/group_definition_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/scalar_primitives.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_operations.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using document::NodeId;
using document::Vec2d;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint32_t kWidth = 512;
constexpr std::uint32_t kHeight = 384;

// The two keys of the space: at 0 s and at 1 s, then held.
constexpr Vec2d kOrigin0{256.0, 200.0};
constexpr Vec2d kOrigin1{216.0, 236.0};
constexpr Vec2d kScale0{40.0, 40.0};
constexpr Vec2d kScale1{56.0, 44.0};
// The two placed points, on the plotted line y = x / 2.
constexpr Vec2d kPointA{1.0, 0.5};
constexpr Vec2d kPointB{-2.0, -1.0};
// The tick the label reads: index 6 of -4..4 by 1 is the mark at x = 2.
constexpr std::int64_t kTickIndex = 6;
constexpr double kTickValue = 2.0;
constexpr Vec2d kLabelOffset{0.0, 24.0};
// A dot is a square this many pixels across.
constexpr double kDot = 8.0;

struct Scene final {
    NodeId origin, scale, space, readout, plot, axes, tick, placeA, placeB, plotShape, axesShape,
        text, textLayer, dotA, dotB;
    document::CompositionId group;
};

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Coordinate spaces", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    Scene scene;

    commands::CommandResult run(commands::Transaction transaction) {
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            std::string why = "fixture command refused";
            for (const auto& failure : result.operationFailures)
                why += ": " + failure.issue.message;
            for (const auto& issue : result.validation.issues())
                why += "; " + issue.message;
            throw std::runtime_error(why);
        }
        return result;
    }
    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        return run(std::move(transaction));
    }
    NodeId addNode(const std::string_view type, const document::CompositionId in) {
        const auto made = run<commands::AddNode>(in, std::string(type), Vec2d{0, 0});
        const auto id = made.outputId<NodeId>(commands::kAddNodeOutput);
        if (!id)
            throw std::runtime_error("fixture node was not added");
        return *id;
    }
    NodeId addNode(const std::string_view type) { return addNode(type, main); }
    [[nodiscard]] document::ParameterId parameterOf(const NodeId node, const std::string_view role,
                                                    const document::CompositionId in) const {
        return parameter(document, in, node, role);
    }
    void set(const NodeId node, const std::string_view role, document::ParameterValue value,
             const document::CompositionId in) {
        commands::Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<commands::SetParameterSource>(
            in, parameterOf(node, role, in), document::ConstantValueSource{std::move(value)});
        const auto outcome = commands.execute(std::move(transaction));
        if (!outcome.succeeded())
            throw std::runtime_error("fixture parameter refused: " + std::string(role));
    }
    void set(const NodeId node, const std::string_view role, document::ParameterValue value) {
        set(node, role, std::move(value), main);
    }
    void connect(const NodeId from, const std::string_view output, const NodeId to,
                 const std::string_view input, const document::CompositionId in) {
        run<commands::ConnectPorts>(in, document::OutputPortRef{from, std::string(output)},
                                    document::NodeInputRef{to, std::string(input)});
    }
    void connect(const NodeId from, const std::string_view output, const NodeId to,
                 const std::string_view input) {
        connect(from, output, to, input, main);
    }
    void key(const NodeId node, const std::int64_t second, const Vec2d value, const bool create) {
        const auto id = parameterOf(node, document::kValueParameterRole, main);
        if (create)
            run<commands::CreateAnimationForParameter>(main, id, core::RationalTime{});
        commands::Transaction transaction("Key", document.snapshot().revision());
        transaction.emplace<commands::SetKeyframeAtTimeForParameter>(
            main, id, core::RationalTime::fromInteger(second), value);
        if (!commands.execute(std::move(transaction)).succeeded())
            throw std::runtime_error("space key refused");
    }
    // Adds `source`'s image to the Merge `layer` is an entry of, at the bottom, at the entry's
    // defaults: the identity, so a path's pixels are frame pixels.
    void enterBelow(const NodeId source, const NodeId layer) {
        NodeId merge;
        for (const auto& edge : composition(document, main).graph().edges())
            if (const auto* entry = std::get_if<document::LayerStackInputRef>(&edge.destination);
                entry != nullptr && edge.source.nodeId == layer)
                merge = entry->stackNodeId;
        run<commands::ConnectPorts>(
            main, document::OutputPortRef{source, "image"},
            document::InputPortRef{document::LayerStackInputRef{merge, {}, "content"}});
    }
    NodeId stroked(const core::Color4d color, const double width) {
        const auto shape = addNode(document::kShapeSourceNodeType);
        set(shape, "kind", static_cast<std::int64_t>(document::ShapeKind::Path));
        set(shape, "fillEnabled", false);
        set(shape, "strokeEnabled", true);
        set(shape, "strokeColor", color);
        set(shape, "strokeWidth", width);
        return shape;
    }
    NodeId dot(const char* name, const core::Color4d color) {
        run<commands::AddSolidLayer>(main, name, color, Vec2d{0, 0});
        const auto boundary = composition(document, main).graph().layerOutputs().back();
        const auto solid = lastNode(document, main, document::kSolidSourceNodeType);
        set(solid, document::kSolidWidthParameterRole, kDot);
        set(solid, document::kSolidHeightParameterRole, kDot);
        return boundary.nodeId;
    }

    void build() {
        auto& s = scene;
        // y = x / 2, as a Function group.
        const auto made = run<commands::CreateGroupDefinition>(main, "Half");
        s.group = *made.outputId<document::CompositionId>(commands::kCreateGroupDefinitionOutput);
        const auto input =
            *made.outputId<NodeId>(commands::kCreateGroupDefinitionInputNodeOutput);
        const auto output =
            *made.outputId<NodeId>(commands::kCreateGroupDefinitionOutputNodeOutput);
        const auto half = addNode(document::kScalarMathNodeType, s.group);
        set(half, "operation",
            document::scalarOperationStoredValue(core::primitives::ScalarPrimitive::Multiply),
            s.group);
        set(half, "b", 0.5, s.group);
        connect(input, document::kFunctionInputSlot, half, "a", s.group);
        connect(half, document::kResultPortName, output, document::kFunctionOutputSlot, s.group);

        // The layers first (each new layer goes on top), then the paths below them.
        s.dotA = dot("Dot A", core::Color4d{1.0, 0.0, 0.0, 1.0});
        s.dotB = dot("Dot B", core::Color4d{1.0, 0.0, 0.0, 1.0});
        run<commands::AddTextLayer>(main, "Label", "0", Vec2d{0, 0}, 1.0, 20.0);
        s.text = lastNode(document, main, document::kTextSourceNodeType);
        s.textLayer = layerNode(document, main, lastLayer(document, main));

        // The space, its origin and size per unit keyed.
        s.origin = addNode(document::kVector2ValueNodeType);
        key(s.origin, 0, kOrigin0, true);
        key(s.origin, 1, kOrigin1, false);
        s.scale = addNode(document::kVector2ValueNodeType);
        key(s.scale, 0, kScale0, true);
        key(s.scale, 1, kScale1, false);
        s.space = addNode(document::kCoordinateSpaceNodeType);
        connect(s.origin, document::kValuePortName, s.space, "origin");
        connect(s.scale, document::kValuePortName, s.space, "pixelsPerUnit");
        const auto inSpace = [&](const NodeId consumer) {
            connect(s.space, document::kSpacePortName, consumer, document::kSpacePortName);
        };

        s.readout = addNode(document::kSpaceReadoutNodeType);
        inSpace(s.readout);
        // The readout is read by something, as an artist's would be: its rotation (0 for this
        // unturned space) turns the label layer. A value nothing reads is not evaluated.

        s.plot = addNode(document::kFunctionPlotNodeType);
        set(s.plot, "group", static_cast<std::int64_t>(s.group.value()));
        set(s.plot, document::kFunctionPlotStartRole, -3.0);
        set(s.plot, document::kFunctionPlotEndRole, 3.0);
        set(s.plot, document::kFunctionPlotSamplesRole, std::int64_t{7});
        set(s.plot, document::kFunctionPlotSmoothRole, false);
        inSpace(s.plot);
        s.plotShape = stroked(core::Color4d{0.0, 1.0, 0.0, 1.0}, 3.0);
        connect(s.plot, document::kFunctionPlotPathPort, s.plotShape, "path");

        s.axes = addNode(document::kAxesNodeType);
        set(s.axes, "xMin", -4.0);
        set(s.axes, "xMax", 4.0);
        set(s.axes, "yMin", -3.0);
        set(s.axes, "yMax", 3.0);
        set(s.axes, "tickLength", 10.0);
        inSpace(s.axes);
        s.axesShape = stroked(core::Color4d{0.0, 0.0, 1.0, 1.0}, 2.0);
        connect(s.axes, "path", s.axesShape, "path");

        s.tick = addNode(document::kAxisTickNodeType);
        set(s.tick, "min", -4.0);
        set(s.tick, "max", 4.0);
        set(s.tick, "index", kTickIndex);
        set(s.tick, "labelOffset", kLabelOffset);
        inSpace(s.tick);
        connect(s.tick, "label", s.text, document::kTextParameterRole);
        connect(s.tick, "position", s.textLayer, "position");
        connect(s.readout, "rotation", s.textLayer, "rotation");

        s.placeA = addNode(document::kPlaceInSpaceNodeType);
        set(s.placeA, "point", kPointA);
        inSpace(s.placeA);
        connect(s.placeA, "position", s.dotA, "position");
        s.placeB = addNode(document::kPlaceInSpaceNodeType);
        set(s.placeB, "point", kPointB);
        inSpace(s.placeB);
        connect(s.placeB, "position", s.dotB, "position");

        enterBelow(s.axesShape, s.dotA);
        enterBelow(s.plotShape, s.dotA);
    }
};

// What the keyed values are at a time, read from the plan's own value graph (the keys' easing is
// the animation system's, which this test does not restate).
struct Evaluated final {
    std::vector<runtime::CompiledValue> outputs;
    const runtime::CompiledCompositionPlan* plan = nullptr;

    [[nodiscard]] const runtime::CompiledValue* output(const NodeId node,
                                                       const std::size_t slot = 0) const {
        // A keyed value lowers its curves' components first; the node's own answer is its last
        // operation.
        const runtime::CompiledValueOperation* found = nullptr;
        for (const auto& operation : plan->valueOperations())
            if (operation.sourceNodeId == node && slot < operation.outputCount)
                found = &operation;
        return found == nullptr ? nullptr : &outputs.at(found->firstOutput.value() + slot);
    }
    template <typename T> [[nodiscard]] T get(const NodeId node, const std::size_t slot = 0) const {
        const auto* value = output(node, slot);
        const auto* typed = value == nullptr ? nullptr : std::get_if<T>(value);
        if (typed == nullptr)
            throw std::runtime_error("an evaluated output is missing: node " +
                                     std::to_string(node.value()) + " slot " +
                                     std::to_string(slot) +
                                     (value == nullptr ? " (no operation)" : " (kind " + std::to_string(value->index()) + ")"));
        return *typed;
    }
};

[[nodiscard]] Evaluated evaluate(const runtime::CompiledCompositionPlan& plan,
                                 const core::RationalTime time) {
    const auto evaluation = runtime::evaluateValueGraph(
        plan.valueOperations(), plan.valueOutputCount(), time, plan.format().frameRate(),
        runtime::ValueGraphCurves{plan.scalarCurves(), plan.vec2Curves(), plan.color4Curves(),
                                  plan.vec3Curves()});
    return {evaluation.outputs, &plan};
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}
[[nodiscard]] bool near(const Vec2d a, const Vec2d b, const double tolerance = 1e-9) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance);
}

// The map, independently: Y up, origin o, s pixels a unit.
struct Map final {
    Vec2d origin;
    Vec2d scale;
    [[nodiscard]] Vec2d operator()(const double x, const double y) const {
        return {origin.x + x * scale.x, origin.y - y * scale.y};
    }
};

[[nodiscard]] std::optional<Reference> referenceAt(Expectations& expectations,
                                                   const Fixture& fixture,
                                                   const core::RationalTime time,
                                                   const std::string& label) {
    auto reference = cpuReference(expectations, fixture.document, fixture.main, label);
    if (!reference)
        return std::nullopt;
    reference->request.time = time;
    auto oracle = reference->request;
    oracle.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(reference->plan, oracle, {});
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    reference->frame = result.frame();
    return reference;
}

// A pixel of the CPU reference at composition coordinates (the data window may not start at 0).
[[nodiscard]] render::Rgba32f pixelAt(const Reference& reference, const std::int64_t x,
                                      const std::int64_t y) {
    const auto& image = reference.frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
        y >= window.maxYExclusive())
        return render::Rgba32f::transparent();
    const auto width = static_cast<std::int64_t>(window.extent().width());
    return image.pixels()[static_cast<std::size_t>((y - window.originY()) * width +
                                                   (x - window.originX()))];
}

// The centroid of the pixels where `weight` is positive, within a box about `near`.
[[nodiscard]] Vec2d centroid(const Reference& reference, const Vec2d about, const int radius,
                             const auto& weight) {
    double total = 0.0;
    Vec2d sum{};
    const auto cx = static_cast<std::int64_t>(std::floor(about.x));
    const auto cy = static_cast<std::int64_t>(std::floor(about.y));
    for (std::int64_t y = cy - radius; y <= cy + radius; ++y)
        for (std::int64_t x = cx - radius; x <= cx + radius; ++x) {
            const double w = weight(pixelAt(reference, x, y));
            if (w <= 0.0)
                continue;
            total += w;
            sum.x += w * (static_cast<double>(x) + 0.5);
            sum.y += w * (static_cast<double>(y) + 0.5);
        }
    return total > 0.0 ? Vec2d{sum.x / total, sum.y / total} : Vec2d{-1e9, -1e9};
}

// The pixel whose square holds a composition point.
[[nodiscard]] render::Rgba32f at(const Reference& reference, const Vec2d point) {
    return pixelAt(reference, static_cast<std::int64_t>(std::floor(point.x)),
                   static_cast<std::int64_t>(std::floor(point.y)));
}

// Values and pixels at one time agree with the map the space's keys define there.
void alignmentAt(Expectations& expectations, const Fixture& fixture, const Reference& reference,
                 const std::string& label, Vec2d& labelCentroid, Vec2d& labelMark) {
    const auto& s = fixture.scene;
    const auto values = evaluate(*reference.plan, reference.request.time);
    const Map map{values.get<Vec2d>(s.origin), values.get<Vec2d>(s.scale)};
    const auto space = values.get<runtime::CompiledSpaceValue>(s.space);
    expectations.expect(space.value != nullptr && space.value->name == "graph",
                        label + ": the Coordinate Space answers the named space");

    // Readouts equal the map.
    const auto matrix = values.get<runtime::CompiledNumberList>(s.readout, 7);
    const std::vector<double> expectedMatrix{map.scale.x, 0.0,          map.origin.x,
                                             0.0,         -map.scale.y, map.origin.y,
                                             0.0,         0.0,          1.0};
    expectations.expect(near(values.get<Vec2d>(s.readout, 0), map.origin) &&
                            near(values.get<Vec2d>(s.readout, 1), Vec2d{map.scale.x, 0.0}) &&
                            near(values.get<Vec2d>(s.readout, 2), Vec2d{0.0, -map.scale.y}) &&
                            near(values.get<Vec2d>(s.readout, 3), map.scale) &&
                            near(values.get<double>(s.readout, 4), 0.0) &&
                            near(values.get<Vec2d>(s.readout, 5), Vec2d{-5.0, -5.0}) &&
                            near(values.get<Vec2d>(s.readout, 6), Vec2d{5.0, 5.0}) &&
                            *matrix.values == expectedMatrix &&
                            values.get<std::string>(s.readout, 8) == "graph" &&
                            !values.get<bool>(s.readout, 10) && values.get<bool>(s.readout, 11),
                        label + ": the readouts are the map's origin, axes, scale, rotation, "
                                "range and matrix");

    // Every node places its own units through the one map.
    const auto plot = values.get<runtime::CompiledPathValue>(s.plot).value;
    bool plotFollows = plot->contours.size() == 1 && plot->contours[0].anchors.size() == 7;
    for (std::size_t index = 0; plotFollows && index < 7; ++index) {
        const double x = -3.0 + static_cast<double>(index);
        plotFollows = near(plot->contours[0].anchors[index].point, map(x, x * 0.5));
    }
    expectations.expect(plotFollows, label + ": every plot anchor is the map of (x, x / 2)");
    const auto axes = values.get<runtime::CompiledPathValue>(s.axes).value;
    expectations.expect(axes->contours.size() == 2 + 9 + 7 &&
                            near(axes->contours[0].anchors[0].point, map(-4, 0)) &&
                            near(axes->contours[0].anchors[1].point, map(4, 0)) &&
                            near(axes->contours[1].anchors[0].point, map(0, -3)) &&
                            near(axes->contours[1].anchors[1].point, map(0, 3)) &&
                            near(axes->contours[2 + 6].anchors[0].point,
                                 Vec2d{map(2, 0).x, map(2, 0).y - 5.0}),
                        label + ": the axes and their ticks are the map of their units, ticks "
                                "10 px across");
    const auto mark = map(kTickValue, 0.0);
    const Vec2d tickPosition{mark.x + kLabelOffset.x, mark.y + kLabelOffset.y};
    expectations.expect(near(values.get<Vec2d>(s.tick, 1), tickPosition) &&
                            values.get<std::string>(s.tick, 2) == "2" && values.get<bool>(s.tick, 3),
                        label + ": the tick is at its mark plus the label offset");
    const auto a = map(kPointA.x, kPointA.y);
    const auto b = map(kPointB.x, kPointB.y);
    const auto scaleA = values.get<Vec2d>(s.placeA, 1);
    expectations.expect(near(values.get<Vec2d>(s.placeA, 0), a) &&
                            near(values.get<Vec2d>(s.placeB, 0), b) &&
                            near(scaleA, Vec2d{map.scale.x / 100.0, map.scale.y / 100.0}) &&
                            near(values.get<double>(s.placeA, 2), 0.0) &&
                            values.get<bool>(s.placeA, 3),
                        label + ": both placements are the map of their points");

    // Pixels: each dot's centroid is its point; the plot and the x axis are under probes through
    // the map and absent a little off them.
    const auto red = [](const render::Rgba32f& p) { return static_cast<double>(p.red()); };
    const auto green = [](const render::Rgba32f& p) { return static_cast<double>(p.green()); };
    const auto blue = [](const render::Rgba32f& p) { return static_cast<double>(p.blue()); };
    const auto dotA = centroid(reference, a, 8, red);
    const auto dotB = centroid(reference, b, 8, red);
    expectations.expect(near(dotA, a, 0.02) && near(dotB, b, 0.02),
                        label + ": each dot's pixels are centred on its point (" +
                            std::to_string(dotA.x - a.x) + ", " + std::to_string(dotA.y - a.y) +
                            ")");
    bool onPlot = true;
    bool onAxis = true;
    for (const double x : {-2.6, -1.5, 0.7, 2.5}) {
        const auto point = map(x, x * 0.5);
        // The line's normal in pixels: the plot runs along (sx, -sy / 2).
        const double nx = map.scale.y * 0.5;
        const double ny = map.scale.x;
        const double length = std::hypot(nx, ny);
        const Vec2d off{point.x + nx / length * 6.0, point.y + ny / length * 6.0};
        onPlot = onPlot && green(at(reference, point)) > 0.5 && green(at(reference, off)) < 1e-6;
    }
    for (const double x : {-3.5, -0.5, 1.5, 3.5}) {
        const auto point = map(x, 0.0);
        onAxis = onAxis && blue(at(reference, point)) > 0.5 &&
                 blue(at(reference, Vec2d{point.x, point.y + 4.0})) < 1e-6;
    }
    expectations.expect(onPlot, label + ": the plot's stroke is where the map puts the line");
    expectations.expect(onAxis, label + ": the x axis is where the map puts y = 0");
    // The label: white text, read by its green where neither the plot nor anything else is.
    labelCentroid = centroid(reference, tickPosition, 16, [](const render::Rgba32f& p) {
        return p.red() > 0.0F && p.green() > 0.0F && p.blue() > 0.0F
                   ? static_cast<double>(p.alpha())
                   : 0.0;
    });
    labelMark = tickPosition;
}

// A genuinely cancelled token, taken from a scheduler task that is cancelled while it runs.
[[nodiscard]] runtime::CancellationToken cancelledToken() {
    using namespace runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("coordinate space cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(591)}),
        [&](TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted())
        throw std::logic_error("cancel token task was refused");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return token;
}

[[nodiscard]] bool samePixels(const Reference& a, const Reference& b) {
    const auto left = a.frame->processImage().pixels();
    const auto right = b.frame->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
}

// The frames the scene is checked at: across the move (0, 1/4, 1/2, 1 s) and held after it (2 s).
[[nodiscard]] std::vector<core::RationalTime> frameTimes() {
    return {core::RationalTime{}, *core::RationalTime::create(1, 4),
            *core::RationalTime::create(1, 2), core::RationalTime::fromInteger(1),
            core::RationalTime::fromInteger(2)};
}

// A plan variant: ROI or reduced render scale, with its own CPU reference.
[[nodiscard]] std::optional<Reference> variant(Expectations& expectations, const Reference& base,
                                               const bool roi, const std::string& label) {
    Reference result = base;
    if (roi) {
        const auto window = render::ImageWindow::create(96, 80, 240, 200);
        result.request.roi = *window.value();
    } else {
        const auto half = render::ImageExtent::create(kWidth / 2, kHeight / 2);
        result.request.resolution = runtime::ProxyResolution{*half.value()};
    }
    auto oracle = result.request;
    oracle.bypassOperationCache = true;
    const auto evaluated = runtime::CpuCompositionEvaluator{}.evaluate(result.plan, oracle, {});
    expectations.expect(evaluated.frame() != nullptr, label + ": the CPU reference evaluates");
    if (evaluated.frame() == nullptr)
        return std::nullopt;
    result.frame = evaluated.frame();
    return result;
}

void saveReopenAndUndo(Expectations& expectations, Fixture& fixture, const Reference& moving) {
    // Save and reopen: the space and its links are ordinary document state.
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    const auto memory = [&] {
        auto operation = coordinator->createOperation();
        if (!operation)
            throw std::runtime_error("memory operation");
        return std::move(*operation);
    };
    const auto snapshot = fixture.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expectations.expect(static_cast<bool>(archive), "save: the graph scene saves and verifies");
    if (archive) {
        auto opened = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
        expectations.expect(opened.outcome() == project::OpenArchiveOutcome::Opened,
                            "reopen: the graph scene reopens");
        if (opened.outcome() == project::OpenArchiveOutcome::Opened) {
            auto reopened = std::move(opened).takeOpened();
            const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
            const auto compiled =
                compiler.compile({reopened.document->snapshot(), fixture.main}, {});
            expectations.expect(compiled.plan != nullptr, "reopen: the reopened scene compiles");
            if (compiled.plan) {
                auto oracle = moving.request;
                oracle.bypassOperationCache = true;
                const auto frame =
                    runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, oracle, {});
                const auto pixels = frame.frame() != nullptr
                                        ? frame.frame()->processImage().pixels()
                                        : std::span<const render::Rgba32f>{};
                expectations.expect(
                    frame.frame() != nullptr &&
                        parity(std::vector<render::Rgba32f>(pixels.begin(), pixels.end()),
                               moving.frame->processImage().pixels()),
                    "reopen: the reopened scene draws the same frame");
            }
        }
    }
    // Undo of the plot's link: the plot is back at its own origin and scale; redo restores it.
    const auto& s = fixture.scene;
    fixture.run<commands::DisconnectInput>(
        fixture.main, document::NodeInputRef{s.plot, std::string(document::kSpacePortName)});
    const auto unlinked = referenceAt(expectations, fixture, moving.request.time, "unlinked");
    if (unlinked) {
        const auto values = evaluate(*unlinked->plan, unlinked->request.time);
        const auto plot = values.get<runtime::CompiledPathValue>(s.plot).value;
        expectations.expect(plot->contours.size() == 1 &&
                                near(plot->contours[0].anchors[0].point, Vec2d{-300.0, 150.0}),
                            "unlinked: the plot places by its own origin and scale");
    }
    expectations.expect(fixture.commands.undo().succeeded(), "undo: the unlink undoes");
    const auto relinked = referenceAt(expectations, fixture, moving.request.time, "relinked");
    expectations.expect(relinked && samePixels(*relinked, moving),
                        "undo: the space's link is back and the frame is the same");
}


void gpuHalf(Expectations& expectations, render::GpuDevice& device, Fixture& fixture,
             const std::vector<Reference>& frames) {
    auto cache = runtime::GpuSceneCache::create(device, runtime::GpuSceneCacheBudgets{});
    expectations.expect(cache.hasValue(), "the scene cache creates");
    if (!cache)
        return;
    auto executor = runtime::GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "the scene executor creates");
    if (!executor)
        return;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto label = "frame " + std::to_string(index);
        gpuParity(expectations, *executor.executor, frames[index], label);
        for (const bool roi : {true, false})
            if (const auto other =
                    variant(expectations, frames[index], roi, label + (roi ? " roi" : " proxy")))
                gpuParity(expectations, *executor.executor, *other,
                          label + (roi ? " region of interest" : " reduced render scale"));
    }

    // Identity on the device: the held frame (2 s) is the 1 s frame's, so it dispatches nothing
    // new; a frame at a time not yet rendered, where the space has moved, is dispatched; an edited
    // key is dispatched, never served stale.
    const runtime::CpuGpuSceneBuilder builder;
    const auto last = builder.build(frames[3].plan, frames[3].request);
    const auto held = builder.build(frames[4].plan, frames[4].request);
    // A time none of the frames above rendered, with the space between its keys.
    const auto fresh =
        referenceAt(expectations, fixture, *core::RationalTime::create(3, 8), "moved");
    expectations.expect(fresh.has_value(), "identity: the moved frame has a CPU reference");
    if (!fresh)
        return;
    const auto moved = builder.build(fresh->plan, fresh->request);
    expectations.expect(last.hasValue() && held.hasValue() && moved.hasValue(),
                        "identity: the frames prepare");
    if (last && held && moved) {
        static_cast<void>(runScene(*executor.executor, last.scene));
        const auto beforeHold = executor.executor->counters();
        expectations.expect(parity(runScene(*executor.executor, held.scene),
                                   frames[4].frame->processImage().pixels()) &&
                                executor.executor->counters().dispatches ==
                                    beforeHold.dispatches,
                            "identity: a frame whose space holds dispatches nothing new");
        const auto beforeMove = executor.executor->counters();
        expectations.expect(parity(runScene(*executor.executor, moved.scene),
                                   fresh->frame->processImage().pixels()) &&
                                executor.executor->counters().dispatches > beforeMove.dispatches,
                            "identity: a frame whose space moved is dispatched");
    }
    fixture.key(fixture.scene.scale, 1, Vec2d{70.0, 30.0}, false);
    const auto edited = referenceAt(expectations, fixture, frames[3].request.time, "edited");
    if (edited) {
        expectations.expect(!samePixels(*edited, frames[3]),
                            "invalidation: the edited key changes the CPU reference");
        const auto scene = builder.build(edited->plan, edited->request);
        const auto before = executor.executor->counters();
        expectations.expect(scene.hasValue() &&
                                parity(runScene(*executor.executor, scene.scene),
                                       edited->frame->processImage().pixels()) &&
                                executor.executor->counters().dispatches > before.dispatches,
                            "invalidation: the edited frame is dispatched and matches");
    }
    expectations.expect(fixture.commands.undo().changed(), "invalidation: the key edit undoes");

    // Cancellation and budget.
    const auto cancelled = builder.build(frames[2].plan, frames[2].request, cancelledToken());
    expectations.expect(!cancelled.hasValue() &&
                            cancelled.diagnostic.code ==
                                runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                        "cancellation: a cancelled preparation reports Cancelled");
    auto tiny = frames[2].request;
    tiny.pixelStorageByteLimit = 64;
    expectations.expect(!builder.build(frames[2].plan, tiny).hasValue(),
                        "budget: a request below the frame's storage is refused");
    const auto prepared = builder.build(frames[2].plan, frames[2].request);
    if (prepared) {
        const auto begun = executor.executor->begin(prepared.scene, kSceneBudget);
        expectations.expect(begun.code == runtime::GpuSceneExecutorDiagnosticCode::None,
                            "native cancel: begins");
        executor.executor->cancel();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (executor.executor->state() == runtime::GpuSceneExecutorJobState::Pending &&
               std::chrono::steady_clock::now() < deadline) {
            (void)executor.executor->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expectations.expect(executor.executor->image() == nullptr,
                            "native cancel: no image is published after cancel");
        while (executor.executor->hasUnretiredSubmission() &&
               std::chrono::steady_clock::now() < deadline) {
            (void)executor.executor->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expectations.expect(parity(runScene(*executor.executor, prepared.scene),
                                   frames[2].frame->processImage().pixels()),
                            "native cancel: the same executor completes the scene afterwards");
        const auto refused = executor.executor->begin(prepared.scene, 64);
        expectations.expect(refused.code != runtime::GpuSceneExecutorDiagnosticCode::None,
                            "budget: the executor refuses a scene over its byte budget");
        if (refused.code == runtime::GpuSceneExecutorDiagnosticCode::None) {
            auto settle = executor.executor->poll();
            while (settle == runtime::GpuSceneExecutorPollResult::Pending)
                settle = executor.executor->poll();
            static_cast<void>(executor.executor->takeImage());
        }
    }

    // Retirement: with room for two images, frames whose space differs evict one another, a
    // cleared cache holds nothing, and a retired frame renders again.
    runtime::GpuSceneCacheBudgets budgets;
    budgets.maxRetainedImages = 2;
    auto small = runtime::GpuSceneCache::create(device, budgets);
    auto smallExecutor =
        small ? runtime::GpuSceneExecutor::create(device, *small.cache)
              : decltype(runtime::GpuSceneExecutor::create(device, *small.cache)){};
    expectations.expect(small.hasValue() && smallExecutor.hasValue(),
                        "retirement: the small cache creates");
    if (small && smallExecutor) {
        for (std::size_t index = 0; index < 4; ++index) {
            const auto scene = builder.build(frames[index].plan, frames[index].request);
            expectations.expect(scene.hasValue() &&
                                    parity(runScene(*smallExecutor.executor, scene.scene),
                                           frames[index].frame->processImage().pixels()) &&
                                    small.cache->entryCount() <= budgets.maxRetainedImages,
                                "retirement: each frame matches in a small cache within budget");
        }
        expectations.expect(small.cache->counters().evictions > 0,
                            "retirement: an earlier frame's images are evicted, not kept");
        small.cache->clear();
        expectations.expect(small.cache->entryCount() == 0 && small.cache->retainedBytes() == 0,
                            "retirement: a cleared cache retains nothing");
        const auto again = builder.build(frames[0].plan, frames[0].request);
        expectations.expect(again.hasValue() &&
                                parity(runScene(*smallExecutor.executor, again.scene),
                                       frames[0].frame->processImage().pixels()),
                            "retirement: a retired frame renders again, equal to the CPU "
                            "reference");
    }
    // A large frame (1080p) renders on the production route with no refusal or fallback.
    {
        auto large = frames[2];
        const auto largeExtent = render::ImageExtent::create(1920, 1440);
        large.request.resolution = runtime::ProxyResolution{*largeExtent.value()};
        const auto scene = builder.build(large.plan, large.request);
        expectations.expect(scene.hasValue() && !runScene(*executor.executor, scene.scene).empty(),
                            "large: the scene at 1920x1440 renders on the GPU route");
    }
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
        Fixture fixture;
        fixture.build();

        std::vector<Reference> frames;
        std::vector<Vec2d> labels;
        std::vector<Vec2d> marks;
        for (const auto time : frameTimes()) {
            const auto label = "t " + std::to_string(time.toSeconds());
            auto reference = referenceAt(expectations, fixture, time, label);
            if (!reference) {
                std::cerr << "the scene did not produce a CPU reference at " << label << '\n';
                return 1;
            }
            Vec2d labelCentroid;
            Vec2d labelMark;
            alignmentAt(expectations, fixture, *reference, label, labelCentroid, labelMark);
            labels.push_back(labelCentroid);
            marks.push_back(labelMark);
            frames.push_back(std::move(*reference));
        }
        // The label moves with its mark: its pixels' centroid keeps one offset from the mark.
        bool labelFollows = true;
        for (std::size_t index = 1; index < labels.size(); ++index) {
            const Vec2d moved{labels[index].x - labels[0].x, labels[index].y - labels[0].y};
            const Vec2d expected{marks[index].x - marks[0].x, marks[index].y - marks[0].y};
            labelFollows = labelFollows && near(moved, expected, 0.35);
        }
        expectations.expect(labelFollows, "the label's pixels move with its tick mark");
        expectations.expect(!samePixels(frames[0], frames[2]) && samePixels(frames[3], frames[4]),
                            "the space moves the scene and then holds it");
        saveReopenAndUndo(expectations, fixture, frames[2]);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " coordinate space expectation(s) failed\n";
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
        gpuHalf(expectations, *device.device, fixture, frames);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " coordinate space expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: a graph scene in one animated coordinate space stays aligned over "
                  << frames.size()
                  << " frames, and the GPU route matches the CPU reference (full frame, region "
                     "of interest, reduced render scale, cold and warm) with identity, "
                     "invalidation, cancellation, budget and retirement\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
