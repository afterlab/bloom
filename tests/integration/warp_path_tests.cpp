// #598 Warp Path and Transform Path skew through the real compiler and CPU evaluator
// (docs/architecture/path-processing.md): a keyed Path node feeding Warp Path feeding Shape Source.
// Per frame results, one set of driver value nodes moving three paths together, cache identity and
// invalidation, cold and warm replay, reduced render scale, cancellation, the retained-path budget,
// diagnostics that name the node, a Transform Path saved before skew, and save and reopen. The GPU
// route has its own native test.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/path_write_operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/warp_path_nodes.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/curve_compilation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include "path_warp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using document::PathAnchor;
using document::PathContour;
using document::PathFillRule;
using document::PathValue;
using document::Vec2d;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

PathContour square(const double left, const double top, const double size) {
    PathContour contour;
    contour.closed = true;
    contour.anchors = {{{left, top}, {}, {}},
                       {{left + size, top}, {}, {}},
                       {{left + size, top + size}, {}, {}},
                       {{left, top + size}, {}, {}}};
    return contour;
}

PathContour circle(const double cx, const double cy, const double radius) {
    const double k = 0.5522847498307936 * radius;
    PathContour contour;
    contour.closed = true;
    contour.anchors = {
        {{cx + radius, cy}, Vec2d{cx + radius, cy - k}, Vec2d{cx + radius, cy + k}},
        {{cx, cy + radius}, Vec2d{cx + k, cy + radius}, Vec2d{cx - k, cy + radius}},
        {{cx - radius, cy}, Vec2d{cx - radius, cy + k}, Vec2d{cx - radius, cy - k}},
        {{cx, cy - radius}, Vec2d{cx - k, cy - radius}, Vec2d{cx + k, cy - radius}}};
    return contour;
}

PathContour star(const double cx, const double cy, const double outer, const double inner) {
    PathContour contour;
    contour.closed = true;
    for (int index = 0; index < 10; ++index) {
        const double angle = std::numbers::pi * 2.0 * static_cast<double>(index) / 10.0;
        const double radius = index % 2 == 0 ? outer : inner;
        contour.anchors.push_back(
            {{cx + radius * std::cos(angle), cy + radius * std::sin(angle)}, {}, {}});
    }
    return contour;
}

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Paths", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(48, 48, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Paths", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused: " +
                                     (result.operationFailures.empty()
                                          ? std::string("no detail")
                                          : result.operationFailures.front().issue.message));
        return result;
    }
    template <typename Operation, typename... Arguments> bool refused(Arguments&&... arguments) {
        commands::Transaction transaction("Paths", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        return !commands.execute(std::move(transaction)).succeeded();
    }
    [[nodiscard]] const document::Composition& main() const {
        return *document.snapshot().project().findComposition(composition);
    }
    [[nodiscard]] document::ParameterId parameter(const document::NodeId node,
                                                  const std::string_view role) const {
        for (const auto& binding : main().graph().findNode(node)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("missing parameter");
    }
    document::NodeId addPathNode() {
        return *run<commands::AddNode>(composition, std::string(document::kPathValueNodeType),
                                       Vec2d{0, 0})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    document::NodeId addShape(const PathValue& path = PathValue::fromContour(square(0, 0, 1)),
                              const core::Color4d color = {1, 1, 1, 1}) {
        commands::ShapeLayerGeometry geometry;
        geometry.path = path;
        commands::ShapeLayerStyle style;
        style.fillColor = color;
        return *run<commands::AddShapeLayer>(composition, document::ShapeKind::Path, geometry,
                                             style)
                    .outputId<document::NodeId>("shapeNode");
    }
    void drive(const document::NodeId producer, const document::NodeId shape) {
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{producer, std::string(document::kValuePortName)},
            document::NodeInputRef{shape, "path"});
    }
    void key(const document::NodeId producer, const std::int64_t second, PathValue value) {
        run<commands::SetKeyframeAtTimeForParameter>(
            composition, parameter(producer, document::kValueParameterRole),
            core::RationalTime::fromInteger(second), std::move(value));
    }
    // Path node -> a node of `type` -> Shape Source.
    document::NodeId addBetween(const std::string_view type, const document::NodeId producer,
                                const document::NodeId shape) {
        const auto node = *run<commands::AddNode>(composition, std::string(type), Vec2d{200, 0})
                               .outputId<document::NodeId>(commands::kAddNodeOutput);
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{producer, std::string(document::kValuePortName)},
            document::NodeInputRef{node, "path"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{node, "path"},
                                    document::NodeInputRef{shape, "path"});
        return node;
    }
    document::NodeId addWarp(const document::NodeId producer, const document::NodeId shape) {
        return addBetween(document::kWarpPathNodeType, producer, shape);
    }
    // A value node of `type` driving `role` of `target`.
    document::NodeId driver(const std::string_view type, const document::NodeId target,
                            const std::string_view role) {
        const auto node = *run<commands::AddNode>(composition, std::string(type), Vec2d{-200, 0})
                               .outputId<document::NodeId>(commands::kAddNodeOutput);
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{node, std::string(document::kValuePortName)},
            document::NodeInputRef{target, std::string(role)});
        return node;
    }
    // Another node's operand wired to an existing driver.
    void wire(const document::NodeId source, const document::NodeId target, const std::string_view role) {
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{source, std::string(document::kValuePortName)},
            document::NodeInputRef{target, std::string(role)});
    }
    template <typename Value>
    void keyValue(const document::NodeId node, const std::int64_t second, const Value value) {
        run<commands::SetKeyframeAtTimeForParameter>(
            composition, parameter(node, document::kValueParameterRole),
            core::RationalTime::fromInteger(second), value);
    }
    void set(const document::NodeId node, const std::string_view role,
             document::ParameterValue value) {
        run<commands::SetParameterSource>(composition, parameter(node, role),
                                          document::ConstantValueSource{std::move(value)});
    }
};

struct Frame final {
    std::shared_ptr<const runtime::ProcessFrame> frame;
    runtime::OperationCacheStatistics statistics;

    [[nodiscard]] float alpha(const std::int64_t x, const std::int64_t y) const {
        const auto& image = frame->processImage();
        const auto window = image.descriptor()->dataWindow();
        if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
            y >= window.maxYExclusive())
            return 0.0F;
        return image
            .pixels()[static_cast<std::size_t>(
                (y - window.originY()) * static_cast<std::int64_t>(window.extent().width()) +
                (x - window.originX()))]
            .alpha();
    }
    [[nodiscard]] bool operator==(const Frame& other) const {
        const auto a = frame->processImage().pixels();
        const auto b = other.frame->processImage().pixels();
        return a.size_bytes() == b.size_bytes() &&
               std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
    }
};

std::shared_ptr<const runtime::CompiledCompositionPlan> compile(const Fixture& fixture) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto compiled = compiler.compile({fixture.document.snapshot(), fixture.composition}, {});
    if (!compiled.plan)
        throw std::runtime_error(
            "the fixture does not compile: " +
            runtime::describeCompileFailure(compiled, fixture.document.snapshot().project(),
                                            fixture.composition));
    return compiled.plan;
}

Frame render(const runtime::CpuCompositionEvaluator& evaluator,
             const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan, const int quarter,
             const runtime::CancellationToken& cancellation = {}) {
    Frame out;
    const auto result = evaluator.evaluate(
        plan,
        {*core::RationalTime::create(quarter, 4), plan->output(),
         runtime::CompositionFormatResolution{}, runtime::EvaluationQuality::Reference,
         runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 26U},
        cancellation, {}, nullptr, &out.statistics);
    out.frame = result.frame();
    return out;
}

// A genuinely cancelled token, taken from a scheduler task that is cancelled while it runs.
runtime::CancellationToken cancelledToken() {
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
        TaskRequest("path value cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(97)}),
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


project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}


// ---- Helpers ----

constexpr std::int64_t kWave = 0, kTwist = 1, kBulge = 2, kBend = 3, kRipple = 4;

struct Chain final {
    Fixture fixture;
    document::NodeId producer, warp, shape;
    explicit Chain(const std::int64_t function = kWave, const double amount = 6.0) {
        producer = fixture.addPathNode();
        shape = fixture.addShape();
        warp = fixture.addWarp(producer, shape);
        fixture.set(warp, "function", function);
        fixture.set(warp, "amount", amount);
        fixture.set(warp, "size", 24.0);
    }
};

// What a path node answers at a quarter second, from the compiled plan's value graph.
struct Answer final {
    PathValue path;
    std::vector<runtime::ValueGraphDiagnostic> diagnostics;
};
Answer answerAt(const Fixture& fixture, const document::NodeId node, const int quarter,
                const std::size_t maxRetainedPathAnchors = 8U * document::kMaximumPathValueAnchors) {
    const auto plan = compile(fixture);
    runtime::ValueGraphMemoization memoization;
    memoization.maxRetainedPathAnchors = maxRetainedPathAnchors;
    const auto evaluated = runtime::evaluateValueGraph(
        plan->valueOperations(), plan->valueOutputCount(), *core::RationalTime::create(quarter, 4),
        plan->format().frameRate(),
        runtime::ValueGraphCurves{plan->scalarCurves(), plan->vec2Curves(), plan->color4Curves(),
                                  plan->vec3Curves()},
        memoization);
    Answer answer;
    answer.diagnostics = evaluated.diagnostics;
    for (const auto& operation : plan->valueOperations()) {
        if (operation.sourceNodeId != node)
            continue;
        if (const auto* path = std::get_if<runtime::CompiledPathValue>(
                &evaluated.outputs[operation.firstOutput.value()]))
            answer.path = *path->value;
    }
    return answer;
}



// The value a scalar or vector node answers at a quarter second.
template <typename T>
T valueAt(const Fixture& fixture, const document::NodeId node, const int quarter) {
    const auto plan = compile(fixture);
    runtime::ValueGraphMemoization memoization;
    const auto evaluated = runtime::evaluateValueGraph(
        plan->valueOperations(), plan->valueOutputCount(), *core::RationalTime::create(quarter, 4),
        plan->format().frameRate(),
        runtime::ValueGraphCurves{plan->scalarCurves(), plan->vec2Curves(), plan->color4Curves(),
                                  plan->vec3Curves()},
        memoization);
    for (const auto& operation : plan->valueOperations())
        if (operation.sourceNodeId == node)
            if (const auto* value =
                    std::get_if<T>(&evaluated.outputs[operation.firstOutput.value()]))
                return *value;
    throw std::runtime_error("the node answered no value");
}

// ---- The node, frame by frame ----

void aKeyedPathWarpsEveryFrame() {
    Chain chain(kWave, 5.0);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(square(4, 4, 16)));
    chain.fixture.key(chain.producer, 1, PathValue::fromContour(square(4, 4, 40)));
    const auto plan = compile(chain.fixture);
    runtime::CpuCompositionEvaluator evaluator;
    const auto start = render(evaluator, plan, 0);
    const auto middle = render(evaluator, plan, 2);
    const auto end = render(evaluator, plan, 4);
    expect(start.frame && middle.frame && end.frame, "every frame renders");
    if (!start.frame || !middle.frame || !end.frame)
        return;
    expect(!(start == middle) && !(middle == end) && !(start == end),
           "the warped shape follows the keyed path: three frames, three pictures");
    expect(end.alpha(24, 24) == 1.0F && end.alpha(0, 0) == 0.0F,
           "the warped square covers the middle and leaves the corner clear");
    // The node's answer at each frame: the square's sides gained points where the wave bends them.
    const auto first = answerAt(chain.fixture, chain.warp, 0);
    const auto last = answerAt(chain.fixture, chain.warp, 4);
    expect(first.diagnostics.empty() && first.path.contours.size() == 1 &&
               first.path.contours[0].closed && first.path.contours[0].anchors.size() > 4 &&
               last.path.contours[0].anchors.size() > 4 && !(first.path == last.path),
           "the node's answer is computed at each frame, closed, with the sides subdivided");
    // An amount of zero draws the unwarped shape.
    chain.fixture.set(chain.warp, "amount", 0.0);
    Fixture plain;
    plain.addShape(PathValue::fromContour(square(4, 4, 40)));
    const auto flat = render(runtime::CpuCompositionEvaluator{}, compile(chain.fixture), 4);
    const auto bare = render(runtime::CpuCompositionEvaluator{}, compile(plain), 0);
    double worst = 0.0;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
            worst = std::max(worst, static_cast<double>(std::abs(flat.alpha(x, y) - bare.alpha(x, y))));
    expect(flat.frame && bare.frame && worst < 1.0e-3,
           "an amount of zero draws the square (largest pixel difference " + std::to_string(worst) + ")");
}

void everyFunctionDrawsAndDiffers() {
    // Five functions on the same keyed circle give five pictures, none empty, none the plain circle.
    std::vector<Frame> frames;
    for (const auto function : {kWave, kTwist, kBulge, kBend, kRipple}) {
        Chain chain(function, function == kTwist ? 90.0 : (function == kBulge ? 40.0 : 5.0));
        chain.fixture.set(chain.warp, "size", function == kBend ? 60.0 : 20.0);
        chain.fixture.key(chain.producer, 0, PathValue::fromContour(circle(24, 24, 14)));
        // The warp's space centres on the path's own origin (0, 0): move the circle there.
        chain.fixture.set(chain.warp, "anchor", Vec2d{24, 24});
        chain.fixture.set(chain.warp, "position", Vec2d{24, 24});
        const auto answer = answerAt(chain.fixture, chain.warp, 0);
        expect(answer.diagnostics.empty() && answer.path.contours.size() == 1 &&
                   !answer.path.contours[0].anchors.empty(),
               "each function answers a path");
        frames.push_back(render(runtime::CpuCompositionEvaluator{}, compile(chain.fixture), 0));
        expect(frames.back().frame != nullptr && frames.back().alpha(24, 24) == 1.0F, "each function draws");
    }
    bool distinct = true;
    for (std::size_t a = 0; a < frames.size(); ++a)
        for (std::size_t b = a + 1; b < frames.size(); ++b)
            distinct = distinct && frames[a].frame && frames[b].frame && !(frames[a] == frames[b]);
    expect(distinct, "the five functions draw five different pictures");
}

// ---- One driver set, three paths ----

void oneDriverSetMovesThreePathsTogether() {
    Fixture f;
    const auto producerA = f.addPathNode();
    const auto producerB = f.addPathNode();
    const auto producerC = f.addPathNode();
    const auto shapeA = f.addShape();
    const auto shapeB = f.addShape();
    const auto shapeC = f.addShape();
    const auto warpA = f.addWarp(producerA, shapeA);
    const auto warpB = f.addWarp(producerB, shapeB);
    const auto transformC = f.addBetween(document::kTransformPathNodeType, producerC, shapeC);
    f.set(warpA, "function", kWave);
    f.set(warpA, "amount", 4.0);
    f.set(warpA, "size", 30.0);
    f.set(warpB, "function", kTwist);
    f.set(warpB, "amount", 40.0);
    f.set(warpB, "size", 30.0);
    f.key(producerA, 0, PathValue::fromContour(square(2, 2, 20)));
    f.key(producerB, 0, PathValue::fromContour(circle(12, 12, 9)));
    f.key(producerC, 0, PathValue::fromContour(star(12, 12, 10, 5)));
    // One set of value nodes: where, how far turned, how slanted, which way, how big.
    const auto position = f.driver(document::kVector2ValueNodeType, warpA, "position");
    const auto rotation = f.driver(document::kScalarValueNodeType, warpA, "rotation");
    const auto skew = f.driver(document::kScalarValueNodeType, warpA, "skew");
    const auto axis = f.driver(document::kScalarValueNodeType, warpA, "skewAxis");
    const auto scale = f.driver(document::kVector2ValueNodeType, warpA, "scale");
    for (const auto target : {warpB, transformC}) {
        f.wire(position, target, "position");
        f.wire(rotation, target, "rotation");
        f.wire(skew, target, "skew");
        f.wire(axis, target, "skewAxis");
        f.wire(scale, target, "scale");
    }
    f.keyValue(position, 0, Vec2d{0, 0});
    f.keyValue(position, 1, Vec2d{8, -6});
    f.keyValue(rotation, 0, 0.0);
    f.keyValue(rotation, 1, 40.0);
    f.keyValue(skew, 0, 0.0);
    f.keyValue(skew, 1, 25.0);
    f.keyValue(axis, 0, 10.0);
    f.keyValue(axis, 1, 30.0);
    f.keyValue(scale, 0, Vec2d{1, 1});
    f.keyValue(scale, 1, Vec2d{1.5, 0.75});

    // The same three nodes with the driver's value at each time written in as constants.
    const auto reference = [&](const int quarter) {
        Fixture r;
        const auto a = r.addPathNode(), b = r.addPathNode(), c = r.addPathNode();
        const auto sa = r.addShape(), sb = r.addShape(), sc = r.addShape();
        const auto wa = r.addWarp(a, sa), wb = r.addWarp(b, sb);
        const auto tc = r.addBetween(document::kTransformPathNodeType, c, sc);
        r.set(wa, "function", kWave);
        r.set(wa, "amount", 4.0);
        r.set(wa, "size", 30.0);
        r.set(wb, "function", kTwist);
        r.set(wb, "amount", 40.0);
        r.set(wb, "size", 30.0);
        r.key(a, 0, PathValue::fromContour(square(2, 2, 20)));
        r.key(b, 0, PathValue::fromContour(circle(12, 12, 9)));
        r.key(c, 0, PathValue::fromContour(star(12, 12, 10, 5)));
        const auto p = valueAt<Vec2d>(f, position, quarter);
        const auto s = valueAt<Vec2d>(f, scale, quarter);
        const auto rot = valueAt<double>(f, rotation, quarter);
        const auto sk = valueAt<double>(f, skew, quarter);
        const auto ax = valueAt<double>(f, axis, quarter);
        for (const auto node : {wa, wb, tc}) {
            r.set(node, "position", p);
            r.set(node, "scale", s);
            r.set(node, "rotation", rot);
            r.set(node, "skew", sk);
            r.set(node, "skewAxis", ax);
        }
        struct Result final {
            PathValue a, b, c;
            Vec2d position, scale;
            double rotation, skew, axis;
        };
        return Result{answerAt(r, wa, 0).path, answerAt(r, wb, 0).path, answerAt(r, tc, 0).path,
                      p, s, rot, sk, ax};
    };
    bool moved = false, alike = true, exact = true;
    PathValue earlierC;
    for (const int quarter : {0, 2, 4}) {
        const auto expected = reference(quarter);
        const auto a = answerAt(f, warpA, quarter);
        const auto b = answerAt(f, warpB, quarter);
        const auto c = answerAt(f, transformC, quarter);
        expect(a.diagnostics.empty() && b.diagnostics.empty() && c.diagnostics.empty(),
               "the driven nodes evaluate without a diagnostic");
        alike = alike && a.path == expected.a && b.path == expected.b && c.path == expected.c;
        // Transform Path is the matrix itself: each anchor is the matrix's image of the original's.
        const auto matrix = runtime::pathwarp::Matrix::make(expected.position, {0, 0}, expected.scale,
                                                            expected.rotation, expected.skew,
                                                            expected.axis);
        const auto original = star(12, 12, 10, 5);
        exact = exact && c.path.contours.size() == 1 &&
                c.path.contours[0].anchors.size() == original.anchors.size();
        for (std::size_t index = 0; exact && index < original.anchors.size(); ++index) {
            const auto want = matrix.forward(original.anchors[index].point);
            const auto got = c.path.contours[0].anchors[index].point;
            exact = std::abs(want.x - got.x) < 1e-9 && std::abs(want.y - got.y) < 1e-9;
        }
        if (quarter == 0)
            earlierC = c.path;
        else
            moved = moved || !(c.path == earlierC);
    }
    expect(alike, "three nodes on one driver set each answer what their own constants of the same values answer");
    expect(exact, "Transform Path on the shared set is exactly the matrix of the driver values");
    expect(moved, "keying the shared values moves the paths");
    // Edit one driver key: all three follow.
    const auto before = answerAt(f, warpB, 4).path;
    f.keyValue(rotation, 1, 70.0);
    const auto afterB = answerAt(f, warpB, 4).path;
    const auto afterC = answerAt(f, transformC, 4).path;
    const auto afterA = answerAt(f, warpA, 4).path;
    expect(!(afterB == before) && !(afterC == earlierC), "editing the shared rotation key moves the other paths");
    expect(valueAt<double>(f, rotation, 4) == 70.0, "the shared rotation is now 70 degrees at the end");
    expect(afterA.contours.size() == 1 && afterB.contours.size() == 1 && afterC.contours.size() == 1,
           "all three still answer one contour each");
}

// ---- Cache identity, replay, scale ----

void aMovedOrHeldFrameIsServedByTheCache() {
    Chain chain(kWave, 4.0);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(circle(24, 24, 10)));
    chain.fixture.key(chain.producer, 1, PathValue::fromContour(circle(24, 24, 20)));
    const auto plan = compile(chain.fixture);
    runtime::CpuCompositionEvaluator cached;
    const auto cold = render(cached, plan, 2);
    const auto warm = render(cached, plan, 2);
    expect(cold.frame && warm.frame && cold == warm, "cold and warm replay are the same picture");
    expect(warm.statistics.misses == 0 && warm.statistics.hits > 0,
           "the same frame is served from the operation cache");
    const auto held = render(cached, plan, 6);
    const auto heldAgain = render(cached, plan, 7);
    expect(held.frame && heldAgain.frame && held == heldAgain,
           "frames past the last key hold the value and draw the same picture");
    const auto moved = render(cached, plan, 1);
    expect(moved.statistics.misses > 0, "a frame whose path moved is evaluated");

    // Invalidation: each changed operand changes the picture and misses; so does an upstream edit.
    chain.fixture.set(chain.warp, "amount", 9.0);
    const auto stronger = render(cached, compile(chain.fixture), 2);
    expect(stronger.frame && !(stronger == cold) && stronger.statistics.misses > 0,
           "another amount draws another picture and is a cache miss");
    chain.fixture.set(chain.warp, "function", kRipple);
    const auto other = render(cached, compile(chain.fixture), 2);
    expect(other.frame && !(other == stronger) && other.statistics.misses > 0,
           "another function draws another picture and is a cache miss");
    chain.fixture.set(chain.warp, "skew", 15.0);
    const auto slanted = render(cached, compile(chain.fixture), 2);
    expect(slanted.frame && !(slanted == other) && slanted.statistics.misses > 0,
           "a skew in the warp's space draws another picture and is a cache miss");
    chain.fixture.key(chain.producer, 1, PathValue::fromContour(circle(24, 24, 12)));
    const auto edited = render(cached, compile(chain.fixture), 2);
    expect(edited.frame && !(edited == slanted) && edited.statistics.misses > 0,
           "an edited upstream key invalidates the warped frame");
    // Tolerance is part of the answer too: a coarse tolerance keeps fewer points.
    chain.fixture.set(chain.warp, "tolerance", 5.0);
    const auto coarse = answerAt(chain.fixture, chain.warp, 2).path.contours[0].anchors.size();
    chain.fixture.set(chain.warp, "tolerance", 0.01);
    const auto fine = answerAt(chain.fixture, chain.warp, 2).path.contours[0].anchors.size();
    expect(coarse < fine, "a tighter tolerance keeps more points than a coarse one");
}

void aReducedRenderScaleIsDownscaleConsistent() {
    Chain chain(kRipple, 3.0);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(circle(24, 24, 16)));
    const auto plan = compile(chain.fixture);
    const runtime::CpuCompositionEvaluator evaluator;
    const auto full = render(evaluator, plan, 0);
    Frame half;
    const auto extent = render::ImageExtent::create(24, 24);
    const auto result = evaluator.evaluate(
        plan, {core::RationalTime{}, plan->output(), runtime::ProxyResolution{*extent.value()},
               runtime::EvaluationQuality::Reference,
               runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 26U},
        {}, {}, nullptr, &half.statistics);
    half.frame = result.frame();
    expect(full.frame && half.frame, "full and half scale render");
    if (!full.frame || !half.frame)
        return;
    double worst = 0.0;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            const double average = static_cast<double>(
                                       full.alpha(2 * x, 2 * y) + full.alpha(2 * x + 1, 2 * y) +
                                       full.alpha(2 * x, 2 * y + 1) + full.alpha(2 * x + 1, 2 * y + 1)) /
                                   4.0;
            worst = std::max(worst, std::abs(average - static_cast<double>(half.alpha(x, y))));
        }
    expect(worst < 0.2, "half scale matches a 2x2 downscale of the full picture (largest edge difference " +
                            std::to_string(worst) + ")");
    expect(half.alpha(12, 12) == 1.0F && half.alpha(0, 0) == 0.0F, "the half-scale picture fills the shape");
}

void cancellationAndTheBudgetAreObserved() {
    Chain chain(kWave, 6.0);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(circle(24, 24, 10)));
    chain.fixture.key(chain.producer, 1, PathValue::fromContour(circle(24, 24, 20)));
    const auto plan = compile(chain.fixture);
    runtime::CpuCompositionEvaluator evaluator;
    expect(render(evaluator, plan, 2, cancelledToken()).frame == nullptr,
           "a cancelled evaluation publishes no frame");
    expect(render(evaluator, plan, 2).frame != nullptr,
           "the same evaluator renders the frame afterwards (nothing partial was kept)");
    runtime::ValueGraphMemoization memoization;
    const auto token = cancelledToken();
    memoization.cancellation = &token;
    const auto evaluated = runtime::evaluateValueGraph(
        plan->valueOperations(), plan->valueOutputCount(), *core::RationalTime::create(2, 4),
        plan->format().frameRate(),
        runtime::ValueGraphCurves{plan->scalarCurves(), plan->vec2Curves(), plan->color4Curves(),
                                  plan->vec3Curves()},
        memoization);
    bool kept = false;
    for (const auto& operation : plan->valueOperations())
        if (operation.sourceNodeId == chain.warp)
            if (const auto* path = std::get_if<runtime::CompiledPathValue>(
                    &evaluated.outputs[operation.firstOutput.value()]))
                kept = !path->value->contours.empty();
    expect(!kept, "a cancelled evaluation keeps no partial warped path");

    // The retained-anchor budget: the warped contour is charged, and over the budget the output falls
    // back to the empty path with the budget diagnostic.
    const auto roomy = answerAt(chain.fixture, chain.warp, 0);
    const auto count = roomy.path.contours.empty() ? 0 : roomy.path.contours[0].anchors.size();
    expect(roomy.diagnostics.empty() && count > 4, "within the budget the warped path is kept");
    const auto fits = answerAt(chain.fixture, chain.warp, 0, count + 4 + count);
    expect(fits.diagnostics.empty() && fits.path == roomy.path, "a budget that holds the warped path keeps it");
    const auto tight = answerAt(chain.fixture, chain.warp, 0, count - 1);
    expect(std::ranges::any_of(tight.diagnostics,
                               [](const auto& diagnostic) {
                                   return diagnostic.summary == "Value graph path budget exceeded";
                               }) &&
               tight.path.contours.empty(),
           "over the budget the evaluation says so and the output is the empty path");
}

void aCapOverflowIsADiagnosticThatNamesTheNode() {
    Chain chain(kWave, 1.0e7);
    chain.fixture.set(chain.warp, "size", 1.0);
    chain.fixture.set(chain.warp, "tolerance", 0.001);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(square(4, 4, 40)));
    const auto answer = answerAt(chain.fixture, chain.warp, 0);
    expect(answer.path.contours.empty(), "nothing is kept past the cap");
    const auto found = std::ranges::find_if(answer.diagnostics, [&](const auto& diagnostic) {
        return diagnostic.nodeId == chain.warp;
    });
    expect(found != answer.diagnostics.end() &&
               found->detail.find("Warp Path") != std::string::npos &&
               found->detail.find("Wave") != std::string::npos &&
               found->detail.find("4096") != std::string::npos &&
               found->detail.find("0.001") != std::string::npos,
           "the diagnostic is on the Warp Path node and names the node, the function, the tolerance and the cap");
    expect(render(runtime::CpuCompositionEvaluator{}, compile(chain.fixture), 0).frame != nullptr,
           "the frame still renders around the refused node");
    // A skew of a quarter turn and a zero scale are refused by the node, with the operand named.
    Chain bad(kWave, 5.0);
    bad.fixture.key(bad.producer, 0, PathValue::fromContour(square(4, 4, 40)));
    bad.fixture.set(bad.warp, "scale", Vec2d{0.0, 1.0});
    const auto flat = answerAt(bad.fixture, bad.warp, 0);
    expect(flat.path.contours.empty() &&
               std::ranges::any_of(flat.diagnostics, [&](const auto& diagnostic) {
                   return diagnostic.nodeId == bad.warp &&
                          diagnostic.summary.find("scale") != std::string::npos;
               }),
           "a scale of zero is refused on the node");
}

// ---- Transform Path skew, and a project saved before it ----

void transformPathSkewsExactlyAndOlderNodesReadNoSkew() {
    Fixture f;
    const auto producer = f.addPathNode();
    const auto shape = f.addShape();
    const auto transform = f.addBetween(document::kTransformPathNodeType, producer, shape);
    PathContour curve;
    curve.anchors = {{{0, 0}, {}, Vec2d{0, 30}}, {{40, 10}, Vec2d{20, -20}, {}}};
    f.key(producer, 0, PathValue::fromContour(curve));
    f.set(transform, "skew", 25.0);
    f.set(transform, "skewAxis", 15.0);
    f.set(transform, "rotation", 10.0);
    f.set(transform, "scale", Vec2d{1.5, 0.5});
    const auto skewed = answerAt(f, transform, 0).path;
    const auto matrix = runtime::pathwarp::Matrix::make({0, 0}, {0, 0}, {1.5, 0.5}, 10.0, 25.0, 15.0);
    bool exact = skewed.contours.size() == 1 && skewed.contours[0].anchors.size() == 2;
    for (std::size_t index = 0; exact && index < 2; ++index) {
        const auto& got = skewed.contours[0].anchors[index];
        const auto& was = curve.anchors[index];
        exact = std::abs(got.point.x - matrix.forward(was.point).x) < 1e-9 &&
                std::abs(got.point.y - matrix.forward(was.point).y) < 1e-9 &&
                (!was.outHandle || (got.outHandle && std::abs(got.outHandle->x - matrix.forward(*was.outHandle).x) < 1e-9)) &&
                (!was.inHandle || (got.inHandle && std::abs(got.inHandle->y - matrix.forward(*was.inHandle).y) < 1e-9));
    }
    expect(exact, "Transform Path skews anchors and handles exactly, with no new points");

    // A node saved before the skew existed has no skew bindings: it reads no skew, and renders the
    // same frame as a node whose skew is zero.
    f.set(transform, "skew", 0.0);
    f.set(transform, "skewAxis", 0.0);
    f.set(transform, "rotation", 10.0);
    const auto zero = answerAt(f, transform, 0).path;
    document::Project older = f.document.snapshot().project();
    auto* composition = older.findComposition(f.composition);
    auto* node = composition->graph().findNode(transform);
    for (const char* role : {"skew", "skewAxis"}) {
        const auto binding = std::ranges::find(node->parameters, std::string_view(role),
                                               &document::ParameterBinding::role);
        if (binding == node->parameters.end())
            throw std::runtime_error("the node has no skew binding to remove");
        (void)composition->parameters().erase(binding->parameterId);
        node->parameters.erase(binding);
    }
    document::Document oldDocument(std::move(older));
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({oldDocument.snapshot(), f.composition}, {});
    expect(static_cast<bool>(compiled.plan),
           "a Transform Path saved before skew still compiles: " +
               runtime::describeCompileFailure(compiled, oldDocument.snapshot().project(),
                                               f.composition));
    if (!compiled.plan)
        return;
    runtime::ValueGraphMemoization memoization;
    const auto evaluated = runtime::evaluateValueGraph(
        compiled.plan->valueOperations(), compiled.plan->valueOutputCount(), core::RationalTime{},
        compiled.plan->format().frameRate(),
        runtime::ValueGraphCurves{compiled.plan->scalarCurves(), compiled.plan->vec2Curves(),
                                  compiled.plan->color4Curves(), compiled.plan->vec3Curves()},
        memoization);
    PathValue answered;
    for (const auto& operation : compiled.plan->valueOperations())
        if (operation.sourceNodeId == transform)
            if (const auto* path = std::get_if<runtime::CompiledPathValue>(
                    &evaluated.outputs[operation.firstOutput.value()]))
                answered = *path->value;
    expect(evaluated.diagnostics.empty() && answered == zero,
           "that node answers exactly what a skew of zero answers");
}

// ---- Save and reopen ----

void aWarpSavesAndReopens() {
    Chain chain(kBend, 70.0);
    chain.fixture.set(chain.warp, "size", 45.0);
    chain.fixture.set(chain.warp, "tolerance", 0.1);
    chain.fixture.set(chain.warp, "skew", 12.0);
    chain.fixture.set(chain.warp, "skewAxis", 33.0);
    chain.fixture.set(chain.warp, "rotation", 15.0);
    const auto skewDriver = chain.fixture.driver(document::kScalarValueNodeType, chain.warp, "phase");
    chain.fixture.keyValue(skewDriver, 0, 0.0);
    chain.fixture.keyValue(skewDriver, 1, 60.0);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(square(4, 4, 30)));
    chain.fixture.key(chain.producer, 1, PathValue::fromContour(square(4, 4, 40)));
    const auto snapshot = chain.fixture.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto saved = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(saved), "the project saves and verifies");
    if (!saved)
        return;
    auto opening = project::openProjectArchive(saved.archive()->bytes(), {}, memory());
    expect(opening.outcome() == project::OpenArchiveOutcome::Opened, "it reopens");
    if (opening.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(opening).takeOpened();
    const auto again = opened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(chain.fixture.composition);
    const auto& after = *again.project().findComposition(chain.fixture.composition);
    expect(std::ranges::equal(before.parameters().records(), after.parameters().records()),
           "every Warp Path parameter equals itself after reopening");
    const auto* node = after.graph().findNode(chain.warp);
    expect(node != nullptr && node->typeId == document::kWarpPathNodeType, "the Warp Path node survives");
    const auto first = render(runtime::CpuCompositionEvaluator{}, compile(chain.fixture), 2);
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto recompiled = compiler.compile({again, chain.fixture.composition}, {});
    expect(static_cast<bool>(recompiled.plan), "the reopened project compiles");
    if (recompiled.plan)
        expect(first.frame && render(runtime::CpuCompositionEvaluator{}, recompiled.plan, 2) == first,
               "the reopened project renders the same frame");
}

} // namespace

int main() {
    try {
        aKeyedPathWarpsEveryFrame();
        everyFunctionDrawsAndDiffers();
        oneDriverSetMovesThreePathsTogether();
        aMovedOrHeldFrameIsServedByTheCache();
        aReducedRenderScaleIsDownscaleConsistent();
        cancellationAndTheBudgetAreObserved();
        aCapOverflowIsADiagnosticThatNamesTheNode();
        transformPathSkewsExactlyAndOlderNodesReadNoSkew();
        aWarpSavesAndReopens();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
