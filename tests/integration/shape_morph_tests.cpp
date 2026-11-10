// #597 Shape Morph through the real compiler and CPU evaluator (docs/architecture/path-processing.md):
// two Path nodes feeding Shape Morph feeding Shape Source. Per frame results for a keyed amount and a
// driven one, exact ends, both pairings, cache identity and invalidation, cold and warm replay,
// reduced render scale, cancellation, the retained-path budget, the cap diagnostic that names the
// node, and save and reopen. The GPU route has its own native test.

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
#include <bloom/document/shape_morph_nodes.hpp>
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
#include "path_morph.hpp"

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

PathContour openWave(const double amplitude) {
    PathContour contour;
    contour.anchors = {{{4, 24}, {}, Vec2d{12, 24 - amplitude}},
                       {{24, 24}, Vec2d{16, 24 + amplitude}, Vec2d{32, 24 - amplitude}},
                       {{44, 24}, Vec2d{36, 24 + amplitude}, {}}};
    return contour;
}

// Two Path nodes feed `from` and `to` of one Shape Morph that feeds a Shape Source.
struct Chain final {
    Fixture fixture;
    document::NodeId from, to, morph, shape;
    explicit Chain(const PathValue& first, const PathValue& second,
                   const document::MorphPairing pairing = document::MorphPairing::ByGroupThenGeometry) {
        from = fixture.addPathNode();
        to = fixture.addPathNode();
        shape = fixture.addShape();
        morph = *fixture.run<commands::AddNode>(fixture.composition,
                                                std::string(document::kShapeMorphNodeType),
                                                Vec2d{200, 0})
                     .outputId<document::NodeId>(commands::kAddNodeOutput);
        fixture.wire(from, morph, "from");
        fixture.wire(to, morph, "to");
        fixture.run<commands::ConnectPorts>(fixture.composition, document::OutputPortRef{morph, "path"},
                                            document::NodeInputRef{shape, "path"});
        fixture.key(from, 0, first);
        fixture.key(to, 0, second);
        fixture.set(morph, "pairing", document::selectorStoredValue(pairing));
    }
    // The amount runs from 0 percent at 0 s to 100 percent at 1 s: quarter 0 is the first shape,
    // quarter 4 the second.
    void keyAmount() {
        // The later key first: a key equal to the unkeyed default (0) would change nothing, so the
        // parameter is made animated by the 100 and the 0 is added to it.
        fixture.run<commands::SetKeyframeAtTimeForParameter>(
            fixture.composition, fixture.parameter(morph, "amount"), core::RationalTime::fromInteger(1), 100.0);
        fixture.run<commands::SetKeyframeAtTimeForParameter>(
            fixture.composition, fixture.parameter(morph, "amount"), core::RationalTime::fromInteger(0), 0.0);
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

// A circle of four anchors with handles, and a star of ten corner anchors: anchor counts differ.
const PathValue kCircle = PathValue::fromContour(circle(24, 24, 16));
const PathValue kStar = PathValue::fromContour(star(24, 24, 20, 9));

// ---- The node, frame by frame ----

void aKeyedAmountMorphsEveryFrame() {
    Chain chain(kCircle, kStar);
    chain.keyAmount();
    const auto plan = compile(chain.fixture);
    runtime::CpuCompositionEvaluator evaluator;
    const auto start = render(evaluator, plan, 0);
    const auto q1 = render(evaluator, plan, 1);
    const auto middle = render(evaluator, plan, 2);
    const auto end = render(evaluator, plan, 4);
    expect(start.frame && q1.frame && middle.frame && end.frame, "every frame renders");
    if (!start.frame || !q1.frame || !middle.frame || !end.frame)
        return;
    expect(!(start == q1) && !(q1 == middle) && !(middle == end) && !(start == end),
           "the shape changes at every sampled amount: four frames, four pictures");
    // The ends are exactly the plain shapes: the node answers `from` at 0 and `to` at 100 (not the
    // aligned, subdivided form), and the frame equals a plain Shape Source drawing that path.
    Fixture plainFrom, plainTo;
    plainFrom.addShape(kCircle);
    plainTo.addShape(kStar);
    expect(start == render(evaluator, compile(plainFrom), 0), "amount 0 draws exactly the first shape");
    expect(end == render(evaluator, compile(plainTo), 0), "amount 100 draws exactly the second shape");
    const auto first = answerAt(chain.fixture, chain.morph, 0);
    const auto last = answerAt(chain.fixture, chain.morph, 4);
    expect(first.diagnostics.empty() && first.path == kCircle,
           "amount 0 answers the first path structurally (four anchors, not the subdivided form)");
    expect(last.path == kStar, "amount 100 answers the second path structurally (ten anchors)");
    const auto mid = answerAt(chain.fixture, chain.morph, 2);
    expect(mid.diagnostics.empty() && mid.path.contours.size() == 1 && mid.path.contours[0].closed &&
               mid.path.contours[0].anchors.size() == 10,
           "in between the shapes share the larger anchor count");
    // The pure step: the node's answer is the shared morph of the two values.
    const auto direct = runtime::pathmorph::morphPaths(kCircle, kStar, 0.5);
    expect(direct && *direct == mid.path, "the node's answer equals the shared alignment's morph");
}

void anAmountPastTheEndsOvershoots() {
    Chain chain(kCircle, kStar);
    chain.fixture.set(chain.morph, "amount", 130.0);
    const auto over = answerAt(chain.fixture, chain.morph, 0);
    const auto direct = runtime::pathmorph::morphPaths(kCircle, kStar, 1.3);
    expect(over.diagnostics.empty() && direct && *direct == over.path,
           "an amount of 130 percent overshoots the second shape the same way an eased key does");
    chain.fixture.set(chain.morph, "amount", -25.0);
    const auto under = answerAt(chain.fixture, chain.morph, 0);
    const auto directUnder = runtime::pathmorph::morphPaths(kCircle, kStar, -0.25);
    expect(under.diagnostics.empty() && directUnder && *directUnder == under.path,
           "a negative amount runs back past the first shape");
}

void aDrivenAmountFollowsItsValueNode() {
    Chain chain(kCircle, kStar);
    const auto driver = chain.fixture.driver(document::kScalarValueNodeType, chain.morph, "amount");
    chain.fixture.keyValue(driver, 1, 100.0);
    chain.fixture.keyValue(driver, 0, 0.0);
    const auto half = answerAt(chain.fixture, chain.morph, 2);
    const auto direct = runtime::pathmorph::morphPaths(kCircle, kStar, 0.5);
    expect(half.diagnostics.empty() && direct && *direct == half.path,
           "a Scalar node keyed 0 to 100 drives the amount: half way at half a second");
    const auto quarter = answerAt(chain.fixture, chain.morph, 1);
    expect(!(quarter.path == half.path), "the driven amount moves from frame to frame");
}

void contourCountsOpenAndClosedAndEmptyAllMorph() {
    PathValue three;
    three.contours = {square(2, 2, 12), square(18, 2, 12), square(34, 2, 12)};
    struct Case {
        const char* label;
        PathValue from, to;
    };
    PathContour wave = openWave(8);
    const Case cases[] = {
        {"one contour to three", kCircle, three},
        {"three contours to one", three, kCircle},
        {"open to closed", PathValue::fromContour(wave), kCircle},
        {"closed to open", kStar, PathValue::fromContour(wave)},
        {"nothing to a shape", PathValue{}, kStar},
        {"a shape to nothing", kStar, PathValue{}},
    };
    for (const auto& each : cases) {
        Chain chain(each.from, each.to);
        chain.fixture.set(chain.morph, "amount", 50.0);
        const auto answer = answerAt(chain.fixture, chain.morph, 0);
        const auto direct = runtime::pathmorph::morphPaths(each.from, each.to, 0.5);
        expect(answer.diagnostics.empty() && direct && *direct == answer.path &&
                   (!answer.path.contours.empty()),
               std::string(each.label) + ": the node answers the shared morph, no diagnostic");
        const auto frame = render(runtime::CpuCompositionEvaluator{}, compile(chain.fixture), 0);
        expect(frame.frame != nullptr, std::string(each.label) + ": the frame renders");
    }
}

void thePairingSelectorChangesWhichContoursMeet() {
    // Two parts the second value holds in the other order, keyed so the groups can say which is which.
    PathValue from;
    from.contours = {square(12, 24, 8), circle(36, 24, 9)};
    from.groups = {{"dot", 0, 1, std::nullopt}, {"bar", 1, 1, std::nullopt}};
    PathValue to;
    to.contours = {circle(36, 24, 9), square(12, 24, 8)};
    to.groups = {{"bar", 0, 1, std::nullopt}, {"dot", 1, 1, std::nullopt}};
    Chain keyed(from, to, document::MorphPairing::ByGroupThenGeometry);
    keyed.fixture.set(keyed.morph, "amount", 50.0);
    Chain ordered(from, to, document::MorphPairing::ByOrder);
    ordered.fixture.set(ordered.morph, "amount", 50.0);
    const auto byGroup = answerAt(keyed.fixture, keyed.morph, 0).path;
    const auto byOrder = answerAt(ordered.fixture, ordered.morph, 0).path;
    expect(!byGroup.contours.empty() && !byOrder.contours.empty() && !(byGroup == byOrder),
           "By group, then geometry and By order pair the contours differently");
    const auto directGroup = runtime::pathmorph::morphPaths(from, to, 0.5);
    runtime::pathmorph::AlignOptions order;
    order.pairing = runtime::pathmorph::Pairing::ByOrder;
    const auto directOrder = runtime::pathmorph::morphPaths(from, to, 0.5, order);
    expect(directGroup && directOrder && *directGroup == byGroup && *directOrder == byOrder,
           "each selector value answers what the shared step answers for that pairing");
    // Pairing by group keeps the square's own position (the dot stays a dot): its centroid at half
    // way is where it started, because the square meets the square.
    bool dotStays = false;
    for (const auto& contour : byGroup.contours) {
        double x = 0.0;
        for (const auto& anchor : contour.anchors)
            x += anchor.point.x;
        x /= static_cast<double>(contour.anchors.size());
        dotStays = dotStays || std::abs(x - 16.0) < 1.0;
    }
    expect(dotStays, "paired by group the square stays on the left, the disc on the right");
}

// ---- Cache identity, replay, scale ----

void aMovedOrHeldFrameIsServedByTheCache() {
    Chain chain(kCircle, kStar);
    chain.keyAmount();
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
           "frames past the last key hold the amount and draw the same picture");
    const auto moved = render(cached, plan, 1);
    expect(moved.statistics.misses > 0, "a frame whose amount moved is evaluated");

    // Invalidation: each changed operand changes the picture and misses; so does an upstream edit.
    chain.fixture.run<commands::SetKeyframeAtTimeForParameter>(
        chain.fixture.composition, chain.fixture.parameter(chain.morph, "amount"),
        core::RationalTime::fromInteger(1), 60.0);
    const auto retimed = render(cached, compile(chain.fixture), 2);
    expect(retimed.frame && !(retimed == cold) && retimed.statistics.misses > 0,
           "an edited amount key draws another picture and is a cache miss");
    chain.fixture.key(chain.to, 0, PathValue::fromContour(circle(24, 24, 8)));
    const auto edited = render(cached, compile(chain.fixture), 2);
    expect(edited.frame && !(edited == retimed) && edited.statistics.misses > 0,
           "an edited second shape invalidates the morphed frame");
    chain.fixture.key(chain.from, 0, PathValue::fromContour(square(24, 24, 18)));
    const auto firstEdited = render(cached, compile(chain.fixture), 2);
    expect(firstEdited.frame && !(firstEdited == edited) && firstEdited.statistics.misses > 0,
           "an edited first shape invalidates it too");
    chain.fixture.set(chain.morph, "pairing",
                      document::selectorStoredValue(document::MorphPairing::ByOrder));
    PathValue two;
    two.contours = {square(12, 24, 8), circle(36, 24, 9)};
    chain.fixture.key(chain.from, 0, two);
    chain.fixture.key(chain.to, 0, kStar);
    const auto byOrder = render(cached, compile(chain.fixture), 2);
    chain.fixture.set(chain.morph, "pairing",
                      document::selectorStoredValue(document::MorphPairing::ByGroupThenGeometry));
    const auto byGroup = render(cached, compile(chain.fixture), 2);
    expect(byOrder.frame && byGroup.frame && byGroup.statistics.misses > 0,
           "another pairing is a cache miss, not a stale hit");
}

void aReducedRenderScaleIsDownscaleConsistent() {
    Chain chain(kCircle, kStar);
    chain.fixture.set(chain.morph, "amount", 50.0);
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

// ---- Cancellation, the budget and the caps ----

void cancellationAndTheBudgetAreObserved() {
    Chain chain(kCircle, kStar);
    chain.fixture.set(chain.morph, "amount", 50.0);
    const auto plan = compile(chain.fixture);
    runtime::CpuCompositionEvaluator evaluator;
    expect(render(evaluator, plan, 0, cancelledToken()).frame == nullptr,
           "a cancelled evaluation publishes no frame");
    expect(render(evaluator, plan, 0).frame != nullptr,
           "the same evaluator renders the frame afterwards (nothing partial was kept)");
    runtime::ValueGraphMemoization memoization;
    const auto token = cancelledToken();
    memoization.cancellation = &token;
    const auto evaluated = runtime::evaluateValueGraph(
        plan->valueOperations(), plan->valueOutputCount(), core::RationalTime{},
        plan->format().frameRate(),
        runtime::ValueGraphCurves{plan->scalarCurves(), plan->vec2Curves(), plan->color4Curves(),
                                  plan->vec3Curves()},
        memoization);
    bool kept = false;
    for (const auto& operation : plan->valueOperations())
        if (operation.sourceNodeId == chain.morph)
            if (const auto* path = std::get_if<runtime::CompiledPathValue>(
                    &evaluated.outputs[operation.firstOutput.value()]))
                kept = !path->value->contours.empty();
    expect(!kept, "a cancelled evaluation keeps no partial morphed path");

    // The retained-anchor budget: the two shapes and the morphed contour are all charged (each
    // Path node's answer and the node's own), and over the budget the node's output falls back to
    // the empty path with the budget diagnostic that says so.
    const auto roomy = answerAt(chain.fixture, chain.morph, 0);
    const std::size_t count = roomy.path.contours.empty() ? 0 : roomy.path.contours[0].anchors.size();
    const std::size_t held = kCircle.anchorCount() + kStar.anchorCount();
    expect(roomy.diagnostics.empty() && count == 10, "within the budget the morphed path is kept");
    const auto fits = answerAt(chain.fixture, chain.morph, 0, held + count);
    expect(fits.diagnostics.empty() && fits.path == roomy.path,
           "a budget that holds both shapes and the morphed path keeps it");
    const auto tight = answerAt(chain.fixture, chain.morph, 0, held + count - 1);
    expect(std::ranges::any_of(tight.diagnostics,
                               [](const auto& diagnostic) {
                                   return diagnostic.summary == "Value graph path budget exceeded";
                               }) &&
               tight.path.contours.empty(),
           "one anchor over the budget: the evaluation says so and the morph output is the empty path");
    // The aligned form is bounded before it is built: the morph answer is charged by what it holds.
    const auto tiny = answerAt(chain.fixture, chain.morph, 0, kCircle.anchorCount());
    expect(tiny.path.contours.empty(), "a budget of the first shape alone cannot hold the morph");
}

void aCapOverflowIsADiagnosticThatNamesTheNode() {
    // A closed contour of 4096 anchors against an open one of 4096: the closed one is cut open and
    // gains its start again, so the aligned contour would hold 4097, one more than a contour may.
    PathContour big;
    big.closed = true;
    PathContour line;
    for (int index = 0; index < 4096; ++index) {
        const double angle = 2.0 * std::numbers::pi * index / 4096.0;
        big.anchors.push_back({{24 + 20 * std::cos(angle), 24 + 20 * std::sin(angle)}, {}, {}});
        line.anchors.push_back({{2.0 + 44.0 * index / 4095.0, 24.0}, {}, {}});
    }
    Chain chain(PathValue::fromContour(big), PathValue::fromContour(line));
    chain.fixture.set(chain.morph, "amount", 50.0);
    const auto answer = answerAt(chain.fixture, chain.morph, 0);
    expect(answer.path.contours.empty(), "nothing is kept past the cap");
    const auto found = std::ranges::find_if(answer.diagnostics, [&](const auto& diagnostic) {
        return diagnostic.nodeId == chain.morph;
    });
    expect(found != answer.diagnostics.end() &&
               found->detail.find("Shape Morph") != std::string::npos &&
               found->detail.find("By group, then geometry") != std::string::npos &&
               found->detail.find("4097") != std::string::npos &&
               found->detail.find("4096") != std::string::npos,
           "the diagnostic is on the Shape Morph node and names the node, the pairing, the count asked and the cap");
    expect(render(runtime::CpuCompositionEvaluator{}, compile(chain.fixture), 0).frame != nullptr,
           "the frame still renders around the refused node");
    // Amount 0 and 100 never align, so they answer their own shape even then.
    chain.fixture.set(chain.morph, "amount", 0.0);
    expect(answerAt(chain.fixture, chain.morph, 0).path == PathValue::fromContour(big),
           "amount 0 still answers the first shape when the shapes could not align");
}

// ---- Save and reopen ----

void aMorphSavesAndReopens() {
    PathValue from;
    from.contours = {square(12, 24, 8), circle(36, 24, 9)};
    from.groups = {{"dot", 0, 1, std::nullopt}, {"bar", 1, 1, std::nullopt}};
    Chain chain(from, kStar, document::MorphPairing::ByOrder);
    chain.keyAmount();
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
           "every Shape Morph parameter equals itself after reopening");
    const auto* node = after.graph().findNode(chain.morph);
    expect(node != nullptr && node->typeId == document::kShapeMorphNodeType, "the Shape Morph node survives");
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
        aKeyedAmountMorphsEveryFrame();
        anAmountPastTheEndsOvershoots();
        aDrivenAmountFollowsItsValueNode();
        contourCountsOpenAndClosedAndEmptyAllMorph();
        thePairingSelectorChangesWhichContoursMeet();
        aMovedOrHeldFrameIsServedByTheCache();
        aReducedRenderScaleIsDownscaleConsistent();
        cancellationAndTheBudgetAreObserved();
        aCapOverflowIsADiagnosticThatNamesTheNode();
        aMorphSavesAndReopens();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
