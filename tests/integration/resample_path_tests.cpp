// #637 Resample Path through the real compiler and CPU evaluator (docs/architecture/path-processing.md):
// a keyed Path node feeding Resample Path feeding Shape Source. Per frame results, a fixed count that
// keeps two resampled paths interpolating, cache identity and invalidation, cold and warm replay,
// reduced render scale, cancellation, the retained-path budget, diagnostics that name the node, the
// Write to a Path operation (one undo step, groups kept, an animated target keyed, a driven one
// refused), and save and reopen. The GPU route has its own native test.

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
#include <bloom/document/resample_path_nodes.hpp>
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
#include <bloom/scripting/node_path_query.hpp>

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
    // Path node -> Resample Path -> Shape Source: the new Resample Path node.
    document::NodeId addResample(const document::NodeId producer, const document::NodeId shape) {
        const auto resample =
            *run<commands::AddNode>(composition, std::string(document::kResamplePathNodeType),
                                    Vec2d{200, 0})
                 .outputId<document::NodeId>(commands::kAddNodeOutput);
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{producer, std::string(document::kValuePortName)},
            document::NodeInputRef{resample, "path"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{resample, "path"},
                                    document::NodeInputRef{shape, "path"});
        return resample;
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

constexpr std::int64_t kFixedCount = 0, kGap = 1, kByLength = 2, kPercentage = 3;
constexpr std::int64_t kStraight = 0, kCurves = 1;

struct Chain final {
    Fixture fixture;
    document::NodeId producer, resample, shape;
    explicit Chain(const std::int64_t mode = kFixedCount, const std::int64_t count = 16,
                   const std::int64_t output = kCurves) {
        producer = fixture.addPathNode();
        shape = fixture.addShape();
        resample = fixture.addResample(producer, shape);
        fixture.set(resample, "mode", mode);
        fixture.set(resample, "count", count);
        fixture.set(resample, "output", output);
    }
};

// What the Resample Path node answers at a quarter second, from the compiled plan's value graph.
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

document::PathAnimationCurve curveOf(std::vector<std::pair<std::int64_t, PathValue>> keys) {
    document::PathAnimationCurve curve;
    curve.id = document::AnimationCurveId::fromRaw(1);
    std::uint64_t id = 1;
    for (auto& [second, value] : keys) {
        document::PathKeyframe key;
        key.id = document::KeyframeId::fromRaw(id++);
        key.time = core::RationalTime::fromInteger(second);
        key.value = std::move(value);
        curve.keyframes.push_back(std::move(key));
    }
    return curve;
}

// ---- The node, frame by frame ----

void aKeyedPathResamplesEveryFrame() {
    Chain chain(kFixedCount, 24, kStraight);
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
    // The layer's origin is the composition centre: the first key is the 16 px square about it.
    expect(start.alpha(24, 24) == 1.0F && start.alpha(10, 10) == 0.0F && end.alpha(10, 10) == 1.0F,
           "the first key's square");
    expect(end.alpha(30, 30) == 1.0F && !(start == end) && !(middle == start) && !(middle == end),
           "the resampled square grows with the keyed path");
    // Straight points on the square's outline, a corner kept, draw the square itself.
    Fixture plain;
    plain.addShape(PathValue::fromContour(square(4, 4, 40)));
    expect(render(runtime::CpuCompositionEvaluator{}, compile(plain), 0).frame != nullptr &&
               end == render(runtime::CpuCompositionEvaluator{}, compile(plain), 0),
           "24 straight points on a square draw the same pixels as the square");
    // Animated upstream: the answer follows the key each frame.
    const auto first = answerAt(chain.fixture, chain.resample, 0);
    const auto last = answerAt(chain.fixture, chain.resample, 4);
    expect(first.diagnostics.empty() && first.path.contours.size() == 1 &&
               first.path.contours[0].anchors.size() == 24 &&
               last.path.contours[0].anchors.size() == 24 && !(first.path == last.path),
           "the node's answer is computed at each frame from the keyed path");
}

void aFixedCountKeepsTwoResampledPathsInterpolating() {
    // A circle (4 anchors) and a star (10) are different structures; resampled to 24 each they are
    // the same structure and so interpolate. Length-based modes may change the count, and then they
    // do not.
    const auto answer = [](const PathContour& contour, const std::int64_t mode,
                           const std::int64_t count, const double gap) {
        Chain chain(mode, count);
        chain.fixture.set(chain.resample, "gap", gap);
        chain.fixture.key(chain.producer, 0, PathValue::fromContour(contour));
        return answerAt(chain.fixture, chain.resample, 0).path;
    };
    const auto round = PathValue::fromContour(circle(24, 24, 18));
    const auto pointy = PathValue::fromContour(star(24, 24, 20, 8));
    expect(!document::pathKeysInterpolate(round, pointy), "a circle and a star do not interpolate");
    const auto from = answer(circle(24, 24, 18), kFixedCount, 24, 10.0);
    const auto to = answer(star(24, 24, 20, 8), kFixedCount, 24, 10.0);
    expect(document::pathKeysInterpolate(from, to),
           "the same Fixed count gives both the same structure, so they interpolate");
    const auto curve = curveOf({{0, from}, {1, to}});
    const auto between = runtime::samplePathCurve(runtime::compileAnimationCurve(curve),
                                                  *core::RationalTime::create(1, 2));
    expect(between && between->contours[0].anchors.size() == 24, "the halfway sample has 24 points");
    bool halfway = between.has_value();
    for (std::size_t index = 0; halfway && index < 24; ++index) {
        const auto& a = from.contours[0].anchors[index].point;
        const auto& b = to.contours[0].anchors[index].point;
        const auto& m = between->contours[0].anchors[index].point;
        halfway = std::abs(m.x - (a.x + b.x) / 2.0) < 1e-9 && std::abs(m.y - (a.y + b.y) / 2.0) < 1e-9;
    }
    expect(halfway, "each point is halfway between its two resampled positions");
    // A gap on two lines of different lengths gives different counts: they hold, not interpolate.
    PathContour shortLine, longLine;
    shortLine.anchors = {{{0, 0}, {}, {}}, {{40, 0}, {}, {}}};
    longLine.anchors = {{{0, 0}, {}, {}}, {{80, 0}, {}, {}}};
    expect(!document::pathKeysInterpolate(answer(shortLine, kGap, 0, 10.0),
                                          answer(longLine, kGap, 0, 10.0)),
           "Gap mode on a longer path has more points, and the keys do not interpolate");
}

void theAnswerFollowsAnAnimatedPathInEveryMode() {
    PathContour thin, wide;
    thin.anchors = {{{2, 24}, {}, {}}, {{22, 24}, {}, {}}};
    wide.anchors = {{{2, 24}, {}, {}}, {{42, 24}, {}, {}}};
    for (const auto mode : {kGap, kByLength}) {
        Chain chain(mode, 0);
        chain.fixture.set(chain.resample, "gap", 5.0);
        chain.fixture.set(chain.resample, "density", 20.0);
        chain.fixture.key(chain.producer, 0, PathValue::fromContour(thin));
        chain.fixture.key(chain.producer, 1, PathValue::fromContour(wide));
        const auto early = answerAt(chain.fixture, chain.resample, 0).path.contours[0].anchors.size();
        const auto late = answerAt(chain.fixture, chain.resample, 4).path.contours[0].anchors.size();
        // 20 px at 5 px: 5 points; 40 px at 5 px: 9. 20 px at 20 per 100: 5; 40 px: 9.
        expect(early == 5 && late == 9, "a length-based mode follows the animated path's length");
    }
    Chain percent(kPercentage, 0);
    percent.fixture.set(percent.resample, "percentage", 50.0);
    percent.fixture.key(percent.producer, 0, PathValue::fromContour(square(4, 4, 10)));
    PathContour octagon;
    octagon.closed = true;
    for (int i = 0; i < 8; ++i)
        octagon.anchors.push_back({{24 + 10 * std::cos(i * std::numbers::pi / 4),
                                    24 + 10 * std::sin(i * std::numbers::pi / 4)}, {}, {}});
    percent.fixture.key(percent.producer, 1, PathValue::fromContour(octagon));
    expect(answerAt(percent.fixture, percent.resample, 0).path.contours[0].anchors.size() == 3 &&
               answerAt(percent.fixture, percent.resample, 4).path.contours[0].anchors.size() == 4,
           "Percentage follows the upstream point count frame by frame (50 percent of 4, then of 8)");
}

void aMovedOrHeldFrameIsServedByTheCache() {
    Chain chain(kFixedCount, 32, kCurves);
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

    // Invalidation: a changed parameter changes the picture and misses; so does an upstream edit.
    chain.fixture.set(chain.resample, "count", std::int64_t{5});
    chain.fixture.set(chain.resample, "output", kStraight);
    const auto fewer = render(cached, compile(chain.fixture), 2);
    expect(fewer.frame && !(fewer == cold) && fewer.statistics.misses > 0,
           "another point count draws another picture and is a cache miss");
    chain.fixture.key(chain.producer, 1, PathValue::fromContour(circle(24, 24, 12)));
    const auto edited = render(cached, compile(chain.fixture), 2);
    expect(edited.frame && !(edited == fewer) && edited.statistics.misses > 0,
           "an edited upstream key invalidates the resampled frame");
}

void aReducedRenderScaleIsDownscaleConsistent() {
    Chain chain(kFixedCount, 40, kCurves);
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
    expect(half.alpha(12, 12) == 1.0F && half.alpha(0, 0) == 0.0F, "the half-scale picture fills the circle");
}

void cancellationAndTheBudgetAreObserved() {
    Chain chain(kFixedCount, 64, kCurves);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(circle(24, 24, 10)));
    chain.fixture.key(chain.producer, 1, PathValue::fromContour(circle(24, 24, 20)));
    const auto plan = compile(chain.fixture);
    runtime::CpuCompositionEvaluator evaluator;
    expect(render(evaluator, plan, 2, cancelledToken()).frame == nullptr,
           "a cancelled evaluation publishes no frame");
    expect(render(evaluator, plan, 2).frame != nullptr,
           "the same evaluator renders the frame afterwards (nothing partial was kept)");
    // A genuinely cancelled token reaches the kernel: no resampled value is retained.
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
        if (operation.sourceNodeId == chain.resample)
            if (const auto* path = std::get_if<runtime::CompiledPathValue>(
                    &evaluated.outputs[operation.firstOutput.value()]))
                kept = !path->value->contours.empty();
    expect(!kept, "a cancelled evaluation keeps no partial resampled path");

    // The retained-anchor budget: a 64-point contour is charged, and over the budget the output
    // falls back to the empty path with the budget diagnostic.
    const auto roomy = answerAt(chain.fixture, chain.resample, 0, 64 + 4 + 64);
    expect(roomy.diagnostics.empty() && roomy.path.contours[0].anchors.size() == 64,
           "within the budget the resampled path is kept");
    const auto tight = answerAt(chain.fixture, chain.resample, 0, 40);
    expect(std::ranges::any_of(tight.diagnostics,
                               [](const auto& diagnostic) {
                                   return diagnostic.summary == "Value graph path budget exceeded";
                               }) &&
               tight.path.contours.empty(),
           "over the budget the evaluation says so and the output is the empty path");
}

void aCapOverflowIsADiagnosticThatNamesTheNode() {
    Chain chain(kFixedCount, 5000, kCurves);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(square(4, 4, 40)));
    const auto answer = answerAt(chain.fixture, chain.resample, 0);
    expect(answer.path.contours.empty(), "nothing is allocated past the cap");
    const auto found = std::ranges::find_if(answer.diagnostics, [&](const auto& diagnostic) {
        return diagnostic.nodeId == chain.resample;
    });
    expect(found != answer.diagnostics.end() &&
               found->detail.find("Resample Path") != std::string::npos &&
               found->detail.find("Fixed count") != std::string::npos &&
               found->detail.find("5000") != std::string::npos,
           "the diagnostic is on the Resample Path node and names the node, the mode and the count");
    // The frame still renders (the shape is empty), not a crash or a stall.
    expect(render(runtime::CpuCompositionEvaluator{}, compile(chain.fixture), 0).frame != nullptr,
           "the frame renders around the refused node");
    Chain gap(kGap, 0);
    gap.fixture.set(gap.resample, "gap", 0.001);
    PathContour line;
    line.anchors = {{{0, 0}, {}, {}}, {{100, 0}, {}, {}}};
    gap.fixture.key(gap.producer, 0, PathValue::fromContour(line));
    const auto refused = answerAt(gap.fixture, gap.resample, 0);
    expect(std::ranges::any_of(refused.diagnostics,
                               [&](const auto& diagnostic) {
                                   return diagnostic.nodeId == gap.resample &&
                                          diagnostic.detail.find("Gap") != std::string::npos &&
                                          diagnostic.detail.find("100001") != std::string::npos;
                               }),
           "a gap that would give 100001 points is refused, naming the mode and the count");
}

// ---- Write to a Path ----

void writeToAPathIsOneUndoStepAndKeepsGroups() {
    Chain chain(kFixedCount, 12, kCurves);
    PathValue compound;
    compound.contours = {square(4, 4, 20), circle(34, 34, 8)};
    compound.fillRule = PathFillRule::EvenOdd;
    compound.groups = {{"left", 0, 1, std::nullopt}, {"right", 1, 1, core::Color4d{1, 0, 0, 1}}};
    chain.fixture.key(chain.producer, 0, compound);
    chain.fixture.key(chain.producer, 1, compound);
    const auto snapshot = chain.fixture.document.snapshot();
    const auto queried = scripting::queryNodePath(snapshot, chain.fixture.composition,
                                                  chain.resample, {},
                                                  core::RationalTime::fromInteger(0), {});
    expect(queried.path.has_value() && queried.diagnostic.empty(), "the node's result is read");
    if (!queried.path)
        return;
    expect(queried.path->contours.size() == 2 && queried.path->contours[0].anchors.size() == 12 &&
               queried.path->groups == compound.groups &&
               queried.path->fillRule == PathFillRule::EvenOdd,
           "the read result is the node's answer, groups and fill rule kept");
    expect(*queried.path == answerAt(chain.fixture, chain.resample, 0).path,
           "and equals what the value graph answers at that time");

    const auto before = chain.fixture.commands.size();
    const auto nodesBefore = chain.fixture.main().graph().nodes().size();
    const auto result = chain.fixture.run<commands::WriteValueToPath>(
        chain.fixture.composition, std::nullopt, *queried.path, Vec2d{300, 0},
        core::RationalTime::fromInteger(0));
    const auto written = result.outputId<document::NodeId>(commands::kWriteToPathNodeOutput);
    expect(written.has_value(), "the command names the Path node it wrote");
    expect(chain.fixture.commands.size() == before + 1 &&
               chain.fixture.main().graph().nodes().size() == nodesBefore + 1,
           "one command adds one Path node");
    if (!written)
        return;
    const auto* node = chain.fixture.main().graph().findNode(*written);
    expect(node != nullptr && node->typeId == document::kPathValueNodeType, "it is a Path node");
    const auto stored = chain.fixture.main().parameters().find(
        chain.fixture.parameter(*written, document::kValueParameterRole));
    const auto* constant = stored ? std::get_if<document::ConstantValueSource>(&stored->source) : nullptr;
    const auto* points = constant ? std::get_if<PathValue>(&constant->value) : nullptr;
    expect(points != nullptr && *points == *queried.path,
           "the stored points equal the node's result at that time, groups included");
    // Nothing was rewired.
    expect(std::ranges::none_of(chain.fixture.main().graph().edges(),
                                [&](const auto& edge) { return edge.source.nodeId == *written; }),
           "the new Path node is wired to nothing");
    expect(chain.fixture.commands.undo().succeeded() &&
               chain.fixture.main().graph().nodes().size() == nodesBefore &&
               chain.fixture.main().graph().findNode(*written) == nullptr,
           "one undo removes it");
    expect(chain.fixture.commands.redo().succeeded() &&
               chain.fixture.main().graph().findNode(*written) != nullptr,
           "and one redo brings it back");

    // The points are real: moved by hand (a key at a time) they animate; the source node still feeds
    // its shape unchanged.
    const auto frameBefore = render(runtime::CpuCompositionEvaluator{}, compile(chain.fixture), 0);
    expect(frameBefore.frame != nullptr, "the project still renders with the new node beside it");
}

void writeToAnExistingPathNode() {
    Chain chain(kFixedCount, 8, kStraight);
    chain.fixture.key(chain.producer, 0, PathValue::fromContour(square(4, 4, 20)));
    chain.fixture.key(chain.producer, 1, PathValue::fromContour(square(4, 4, 40)));
    const auto at = [&](const std::int64_t second) {
        const auto queried = scripting::queryNodePath(
            chain.fixture.document.snapshot(), chain.fixture.composition, chain.resample, {},
            core::RationalTime::fromInteger(second), {});
        return queried.path.value_or(PathValue{});
    };
    // A constant target is replaced.
    const auto target = chain.fixture.addPathNode();
    chain.fixture.set(target, document::kValueParameterRole, PathValue::fromContour(square(0, 0, 3)));
    const auto start = at(0);
    chain.fixture.run<commands::WriteValueToPath>(chain.fixture.composition, target, start,
                                                  Vec2d{}, core::RationalTime::fromInteger(0));
    const auto read = [&](const document::NodeId node) {
        const auto* parameter = chain.fixture.main().parameters().find(
            chain.fixture.parameter(node, document::kValueParameterRole));
        const auto* constant = parameter ? std::get_if<document::ConstantValueSource>(&parameter->source)
                                         : nullptr;
        const auto* value = constant ? std::get_if<PathValue>(&constant->value) : nullptr;
        return value ? *value : PathValue{};
    };
    expect(read(target) == start, "a constant Path node takes the points");
    // An animated target gets a whole-path key at the time.
    const auto animated = chain.fixture.addPathNode();
    chain.fixture.key(animated, 0, PathValue::fromContour(square(0, 0, 3)));
    chain.fixture.key(animated, 2, PathValue::fromContour(square(0, 0, 5)));
    const auto later = at(1);
    chain.fixture.run<commands::WriteValueToPath>(chain.fixture.composition, animated, later, Vec2d{},
                                                  core::RationalTime::fromInteger(1));
    const auto* parameter = chain.fixture.main().parameters().find(
        chain.fixture.parameter(animated, document::kValueParameterRole));
    const auto* curve = parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source) : nullptr;
    const auto* keys = curve ? chain.fixture.main().animationCurves().findPath(curve->curveId) : nullptr;
    expect(keys != nullptr && keys->keyframes.size() == 3 &&
               keys->keyframes[1].time == core::RationalTime::fromInteger(1) &&
               keys->keyframes[1].value == later,
           "an animated Path node gets a whole-path key at the time and keeps its others");
    // A node that is not a Path node is refused.
    expect(chain.fixture.refused<commands::WriteValueToPath>(chain.fixture.composition, chain.resample,
                                                             start, Vec2d{}, core::RationalTime{}),
           "a node that is not a Path node is refused");
    // Any Path-producing node can be read: the Path node itself.
    const auto self = scripting::queryNodePath(chain.fixture.document.snapshot(),
                                               chain.fixture.composition, chain.producer, {},
                                               core::RationalTime::fromInteger(0), {});
    expect(self.path.has_value(), "a Path node itself can be read and written on");
    const auto nothing = scripting::queryNodePath(chain.fixture.document.snapshot(),
                                                  chain.fixture.composition, chain.shape, {},
                                                  core::RationalTime{}, {});
    expect(!nothing.path && !nothing.diagnostic.empty(), "a node with no Path output says so");
    const auto missing = scripting::queryNodePath(chain.fixture.document.snapshot(),
                                                  chain.fixture.composition,
                                                  document::NodeId::fromRaw(9999), {},
                                                  core::RationalTime{}, {});
    expect(!missing.path && !missing.diagnostic.empty(), "a missing node says so");
    const auto token = cancelledToken();
    const auto cancelled = scripting::queryNodePath(chain.fixture.document.snapshot(),
                                                    chain.fixture.composition, chain.resample, {},
                                                    core::RationalTime{}, token);
    expect(cancelled.cancelled && !cancelled.path, "a cancelled read keeps no path");
}

// ---- Save and reopen ----

void aResampleSavesAndReopens() {
    Chain chain(kGap, 0, kStraight);
    chain.fixture.set(chain.resample, "gap", 7.5);
    chain.fixture.set(chain.resample, "keepCorners", false);
    chain.fixture.set(chain.resample, "startOffset", 25.0);
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
           "every Resample Path parameter equals itself after reopening");
    const auto* node = after.graph().findNode(chain.resample);
    expect(node != nullptr && node->typeId == document::kResamplePathNodeType,
           "the Resample Path node survives");
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
        aKeyedPathResamplesEveryFrame();
        aFixedCountKeepsTwoResampledPathsInterpolating();
        theAnswerFollowsAnAnimatedPathInEveryMode();
        aMovedOrHeldFrameIsServedByTheCache();
        aReducedRenderScaleIsDownscaleConsistent();
        cancellationAndTheBudgetAreObserved();
        aCapOverflowIsADiagnosticThatNamesTheNode();
        writeToAPathIsOneUndoStepAndKeepsGroups();
        writeToAnExistingPathNode();
        aResampleSavesAndReopens();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
