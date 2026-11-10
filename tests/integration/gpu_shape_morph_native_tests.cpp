// #597 Shape Morph on the production GPU route (docs/architecture/path-processing.md).
//
// Real documents are built through commands and compiled by the production SnapshotCompiler: two
// Path nodes feed Shape Morph's `from` and `to`, whose answer feeds Shape Source's `path`, and the
// morph's amount is keyed. Shape Morph is host value computation (it answers a Path the existing fill
// draws), so the proof is that the morphed path reaches the same coverage producer on the device and
// matches the CPU reference (2e-6), cold and warm, at 512 px over keyed amounts for shapes of
// different anchor counts, contour counts, open and closed contours and both pairings, and that a
// 1080p frame prepares and runs on the production route without refusal. The prepared identity
// changes exactly where the morphed path does, an edit of the node or of an upstream path
// re-dispatches, preparation and the native job cancel, the budget refuses, and an evicted frame
// renders again.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/shape_morph_nodes.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <numbers>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using document::PathContour;
using document::PathValue;
using document::Vec2d;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

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

PathContour square(const double cx, const double cy, const double half) {
    PathContour contour;
    contour.closed = true;
    contour.anchors = {{{cx - half, cy - half}, {}, {}},
                       {{cx + half, cy - half}, {}, {}},
                       {{cx + half, cy + half}, {}, {}},
                       {{cx - half, cy + half}, {}, {}}};
    return contour;
}

// Two contours with a hole, a single contour, and three small ones: contour counts that differ.
PathValue ring(const double outer, const double inner) {
    PathValue value;
    value.contours = {circle(0, 0, outer), circle(0, 0, inner)};
    return value;
}
PathValue threeSquares() {
    PathValue value;
    value.contours = {square(-130, 0, 50), square(0, 0, 50), square(130, 0, 50)};
    return value;
}
PathValue openWave(const double amplitude) {
    PathContour contour;
    contour.anchors = {{{-200, 0}, {}, Vec2d{-150, -amplitude}},
                       {{0, 0}, Vec2d{-50, amplitude}, Vec2d{50, -amplitude}},
                       {{200, 0}, Vec2d{150, amplitude}, {}}};
    return PathValue::fromContour(contour);
}

struct Fixture final {
    document::NewProject initial;
    document::CompositionId main;
    document::Document document;
    commands::CommandStack commands;
    document::NodeId from, to, morph, shape;

    Fixture(const std::uint32_t width, const std::uint32_t height, const PathValue& first,
            const PathValue& second, const document::MorphPairing pairing =
                                         document::MorphPairing::ByGroupThenGeometry)
        : initial(document::makeNewProject(
              "Morph", "Main", core::RationalTime::fromInteger(4),
              *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))),
          main(initial.initialCompositionId), document(std::move(initial.project)),
          commands(document) {
        for (const auto* each : {&first, &second}) {
            execute<commands::AddNode>(document, commands, main,
                                       std::string(document::kPathValueNodeType),
                                       Vec2d{each == &first ? 0.0 : 100.0, 0});
            (each == &first ? from : to) = lastNode(document, main, document::kPathValueNodeType);
            setValue(each == &first ? from : to, *each);
        }
        commands::ShapeLayerGeometry geometry;
        geometry.path = PathValue::fromContour(circle(0, 0, 4));
        commands::ShapeLayerStyle style;
        style.fillColor = core::Color4d{0.8, 0.3, 0.1, 1.0};
        style.strokeEnabled = true;
        style.strokeColor = core::Color4d{0.1, 0.8, 0.2, 1.0};
        style.strokeWidth = 3.0;
        execute<commands::AddShapeLayer>(document, commands, main, document::ShapeKind::Path,
                                         geometry, style);
        shape = lastNode(document, main, document::kShapeSourceNodeType);
        execute<commands::AddNode>(document, commands, main,
                                   std::string(document::kShapeMorphNodeType), Vec2d{200, 0});
        morph = lastNode(document, main, document::kShapeMorphNodeType);
        for (const auto& [node, role] : {std::pair{from, "from"}, std::pair{to, "to"}})
            execute<commands::ConnectPorts>(
                document, commands, main,
                document::OutputPortRef{node, std::string(document::kValuePortName)},
                document::NodeInputRef{morph, role});
        execute<commands::ConnectPorts>(document, commands, main,
                                        document::OutputPortRef{morph, "path"},
                                        document::NodeInputRef{shape, "path"});
        setNumber(morph, "pairing", document::selectorStoredValue(pairing));
        // The amount runs from the first shape at 0 s to the second at 1 s.
        // The later key first: a key equal to the unkeyed default (0) would change nothing.
        keyAmount(1, 100.0);
        keyAmount(0, 0.0);
    }
    void setValue(const document::NodeId node, const PathValue& value) {
        commands::Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<commands::SetParameterSource>(
            main, parameter(document, main, node, document::kValueParameterRole),
            document::ConstantValueSource{value});
        (void)commands.execute(std::move(transaction));
    }
    void setNumber(const document::NodeId node, const std::string_view role,
                   document::ParameterValue value) {
        // A value equal to the one stored changes nothing, which is not an error here.
        commands::Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<commands::SetParameterSource>(
            main, parameter(document, main, node, role),
            document::ConstantValueSource{std::move(value)});
        (void)commands.execute(std::move(transaction));
    }
    void keyAmount(const std::int64_t second, const double percent) {
        execute<commands::SetKeyframeAtTimeForParameter>(
            document, commands, main, parameter(document, main, morph, "amount"),
            core::RationalTime::fromInteger(second), percent);
    }
};

[[nodiscard]] std::optional<Reference> referenceAt(Expectations& expectations,
                                                   const Fixture& fixture, const int quarter,
                                                   const std::string& label) {
    auto reference = cpuReference(expectations, fixture.document, fixture.main, label);
    if (!reference)
        return std::nullopt;
    reference->request.time = *core::RationalTime::create(quarter, 4);
    auto oracle = reference->request;
    oracle.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(reference->plan, oracle, {});
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    reference->frame = result.frame();
    return reference;
}

[[nodiscard]] bool samePixels(const Reference& a, const Reference& b) {
    const auto left = a.frame->processImage().pixels();
    const auto right = b.frame->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
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
        TaskRequest("gpu shape morph cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(96)}),
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


struct KeyedStates final {
    Reference start, middle, end;
};

[[nodiscard]] std::optional<KeyedStates> keyedStates(Expectations& expectations,
                                                     const Fixture& fixture,
                                                     const std::string& label) {
    auto start = referenceAt(expectations, fixture, 0, label + " start");
    auto middle = referenceAt(expectations, fixture, 2, label + " middle");
    auto end = referenceAt(expectations, fixture, 4, label + " end");
    if (!start || !middle || !end)
        return std::nullopt;
    return KeyedStates{std::move(*start), std::move(*middle), std::move(*end)};
}

// Cache identity on the device: a frame whose morphed path did not change runs nothing new; a
// frame whose path changed, an edited upstream path and an edited node operand do.
void cacheIdentityAndInvalidation(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                                  const KeyedStates& keyed, Fixture& fixture) {
    const runtime::CpuGpuSceneBuilder builder;
    const auto end = builder.build(keyed.end.plan, keyed.end.request);
    const auto middle = builder.build(keyed.middle.plan, keyed.middle.request);
    expectations.expect(end.hasValue() && middle.hasValue(), "identity: every keyed state prepares");
    if (!end || !middle)
        return;
    expectations.expect(!runScene(executor, end.scene).empty(), "identity: the second shape runs");
    const auto beforeAgain = executor.counters();
    expectations.expect(parity(runScene(executor, end.scene),
                               keyed.end.frame->processImage().pixels()),
                        "identity: the same frame again matches");
    expectations.expect(executor.counters().dispatches == beforeAgain.dispatches,
                        "identity: a frame whose morphed path is unchanged dispatches nothing new");
    const auto beforeMove = executor.counters();
    const auto moved = runScene(executor, middle.scene);
    expectations.expect(executor.counters().dispatches > beforeMove.dispatches,
                        "identity: a frame whose morphed path moved is dispatched");
    expectations.expect(parity(moved, keyed.middle.frame->processImage().pixels()),
                        "identity: an in-between frame matches the CPU reference");

    // An edit of the second shape the middle frame reads.
    fixture.setValue(fixture.to, PathValue::fromContour(circle(0, 0, 90)));
    const auto edited = referenceAt(expectations, fixture, 2, "edited shape");
    expectations.expect(edited && !samePixels(*edited, keyed.middle),
                        "invalidation: the edited upstream path changes the CPU reference");
    // An edit of an operand of the node itself.
    fixture.setNumber(fixture.morph, "pairing", document::selectorStoredValue(document::MorphPairing::ByOrder));
    fixture.keyAmount(1, 80.0);
    const auto other = referenceAt(expectations, fixture, 2, "edited amount key");
    expectations.expect(other && edited && !samePixels(*other, *edited),
                        "invalidation: the edited amount key changes the CPU reference");
    for (const auto* state : {&edited, &other}) {
        if (!*state)
            continue;
        const auto scene = builder.build((*state)->plan, (*state)->request);
        expectations.expect(scene.hasValue(), "invalidation: the edited state prepares");
        if (!scene)
            continue;
        const auto before = executor.counters();
        const auto pixels = runScene(executor, scene.scene);
        expectations.expect(parity(pixels, (*state)->frame->processImage().pixels()),
                            "invalidation: the edited frame matches the CPU reference");
        expectations.expect(executor.counters().dispatches > before.dispatches,
                            "invalidation: the edited frame is dispatched, not served stale");
    }
}

void cancellationAndBudget(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                           const Reference& reference) {
    const runtime::CpuGpuSceneBuilder builder;
    const auto cancelled = builder.build(reference.plan, reference.request, cancelledToken());
    expectations.expect(!cancelled.hasValue() &&
                            cancelled.diagnostic.code ==
                                runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                        "cancellation: a cancelled preparation reports Cancelled");
    auto tiny = reference.request;
    tiny.pixelStorageByteLimit = 64;
    expectations.expect(!builder.build(reference.plan, tiny).hasValue(),
                        "budget: a request below the frame's storage is refused");

    // A native job cancelled right after begin publishes no image and retires, and the same
    // executor then completes the scene with the right pixels.
    auto fresh = reference.request;
    fresh.time = core::RationalTime::fromInteger(3);
    const auto prepared = builder.build(reference.plan, fresh);
    expectations.expect(prepared.hasValue(), "native cancel: prepares");
    if (!prepared)
        return;
    const auto begun = executor.begin(prepared.scene, kSceneBudget);
    expectations.expect(begun.code == runtime::GpuSceneExecutorDiagnosticCode::None,
                        "native cancel: begins");
    executor.cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (executor.state() == runtime::GpuSceneExecutorJobState::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        (void)executor.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(executor.state() != runtime::GpuSceneExecutorJobState::Ready &&
                            executor.image() == nullptr,
                        "native cancel: no image is published after cancel");
    while (executor.hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline) {
        (void)executor.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto after = runScene(executor, prepared.scene);
    auto oracle = fresh;
    oracle.bypassOperationCache = true;
    const auto expected = runtime::CpuCompositionEvaluator{}.evaluate(reference.plan, oracle, {});
    expectations.expect(expected.frame() != nullptr &&
                            parity(after, expected.frame()->processImage().pixels()),
                        "native cancel: the same executor completes the scene afterwards");

    // A scene whose budget cannot hold its frame is refused by the executor.
    const auto refused = executor.begin(prepared.scene, 64);
    expectations.expect(refused.code != runtime::GpuSceneExecutorDiagnosticCode::None,
                        "budget: the executor refuses a scene over its byte budget");
    if (refused.code == runtime::GpuSceneExecutorDiagnosticCode::None) {
        auto settle = executor.poll();
        while (settle == runtime::GpuSceneExecutorPollResult::Pending)
            settle = executor.poll();
        static_cast<void>(executor.takeImage());
    }
}


// Retirement: the morphed-path geometry a frame prepared is retained by the scene cache under its
// operation key and nothing more. With room for two images, frames whose paths differ evict one
// another (the geometry of an earlier path is retired, not kept), a cleared cache holds nothing,
// and a frame whose geometry was retired renders again with the right pixels.
void retirement(Expectations& expectations, render::GpuDevice& device, const KeyedStates& keyed) {
    runtime::GpuSceneCacheBudgets budgets;
    budgets.maxRetainedImages = 2;
    auto cache = runtime::GpuSceneCache::create(device, budgets);
    expectations.expect(cache.hasValue(), "retirement: the small cache creates");
    if (!cache)
        return;
    auto executor = runtime::GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "retirement: the executor creates");
    if (!executor)
        return;
    const runtime::CpuGpuSceneBuilder builder;
    for (const auto* state : {&keyed.start, &keyed.middle, &keyed.end}) {
        const auto scene = builder.build(state->plan, state->request);
        expectations.expect(scene.hasValue(), "retirement: the frame prepares");
        if (!scene)
            return;
        expectations.expect(parity(runScene(*executor.executor, scene.scene),
                                   state->frame->processImage().pixels()),
                            "retirement: each frame matches the CPU reference in a small cache");
        expectations.expect(cache.cache->entryCount() <= budgets.maxRetainedImages,
                            "retirement: the cache holds no more than its image budget");
    }
    expectations.expect(cache.cache->counters().evictions > 0,
                        "retirement: the geometry of an earlier path is evicted, not kept");
    expectations.expect(!executor.executor->hasUnretiredSubmission(),
                        "retirement: no submission is left unretired");
    cache.cache->clear();
    expectations.expect(cache.cache->entryCount() == 0 && cache.cache->retainedBytes() == 0,
                        "retirement: a cleared cache retains no morphed-path geometry");
    const auto again = builder.build(keyed.start.plan, keyed.start.request);
    expectations.expect(again.hasValue() &&
                            parity(runScene(*executor.executor, again.scene),
                                   keyed.start.frame->processImage().pixels()),
                        "retirement: a frame whose geometry was retired renders again, equal to the "
                        "CPU reference");
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
        struct Variant final {
            const char* label;
            PathValue from, to;
            document::MorphPairing pairing;
        };
        // Each kind of difference the alignment bridges: anchor counts, contour counts both ways, a
        // contour that has no partner, open to closed, and the By order pairing.
        const auto grouped = document::MorphPairing::ByGroupThenGeometry;
        const Variant variants[] = {
            {"circle to star", PathValue::fromContour(circle(0, 0, 150)),
             PathValue::fromContour(star(0, 0, 200, 90)), grouped},
            {"one contour to three", PathValue::fromContour(circle(0, 0, 150)), threeSquares(), grouped},
            {"three contours to one", threeSquares(), PathValue::fromContour(circle(0, 0, 160)), grouped},
            {"ring to circle", ring(190, 90), PathValue::fromContour(circle(0, 0, 120)), grouped},
            {"open to closed", openWave(60), PathValue::fromContour(circle(0, 0, 140)), grouped},
            {"star to open", PathValue::fromContour(star(0, 0, 190, 100)), openWave(-50), grouped},
            {"square to star by order", PathValue::fromContour(square(0, 0, 120)),
             PathValue::fromContour(star(0, 0, 200, 80)), document::MorphPairing::ByOrder},
        };
        std::vector<std::pair<std::string, KeyedStates>> states;
        for (const auto& variant : variants) {
            Fixture fixture(512, 512, variant.from, variant.to, variant.pairing);
            auto keyed = keyedStates(expectations, fixture, variant.label);
            if (!keyed)
                continue;
            expectations.expect(!samePixels(keyed->start, keyed->middle) &&
                                    !samePixels(keyed->middle, keyed->end),
                                std::string(variant.label) + ": the morph moves");
            states.emplace_back(variant.label, std::move(*keyed));
        }
        // Large frame: prepares and runs on the production route below (no CPU reference compare).
        Fixture large(1920, 1080, PathValue::fromContour(circle(0, 0, 300)),
                      PathValue::fromContour(star(0, 0, 450, 200)));
        expectations.expect(states.size() == 7, "every kind of morph has a reference");
        // The reference for the cache, retirement and cancellation work.
        Fixture identityFixture(512, 512, PathValue::fromContour(circle(0, 0, 150)),
                                PathValue::fromContour(star(0, 0, 200, 90)));
        const auto identityKeyed = keyedStates(expectations, identityFixture, "identity");
        if (expectations.failures() != 0 || !identityKeyed) {
            std::cerr << expectations.failures() << " shape morph expectation(s) failed\n";
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

        for (const auto& [label, keyed] : states) {
            gpuParity(expectations, *executor.executor, keyed.start, label + " start");
            gpuParity(expectations, *executor.executor, keyed.middle, label + " middle");
            gpuParity(expectations, *executor.executor, keyed.end, label + " end");
        }

        // 1080p: the production preparation and native run complete with no refusal.
        {
            const auto plan = [&]() -> std::shared_ptr<const runtime::CompiledCompositionPlan> {
                const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
                return compiler.compile({large.document.snapshot(), large.main}, {}).plan;
            }();
            expectations.expect(plan != nullptr, "1080p: compiles");
            if (plan) {
                auto request = requestFor(*plan);
                request.time = *core::RationalTime::create(2, 4);
                const auto prepared = runtime::CpuGpuSceneBuilder{}.build(plan, request);
                expectations.expect(prepared.hasValue(),
                                    "1080p: prepares on the production route: " +
                                        prepared.diagnostic.message);
                if (prepared) {
                    const auto pixels = runScene(*executor.executor, prepared.scene);
                    expectations.expect(!pixels.empty() && executor.executor->diagnostic().code ==
                                                               runtime::GpuSceneExecutorDiagnosticCode::None,
                                        "1080p: the native run completes with no refusal or fallback");
                }
            }
        }

        // Its own cache: the identity proof starts cold.
        auto identityCache =
            runtime::GpuSceneCache::create(*device.device, runtime::GpuSceneCacheBudgets{});
        auto identityExecutor =
            identityCache ? runtime::GpuSceneExecutor::create(*device.device, *identityCache.cache)
                          : decltype(runtime::GpuSceneExecutor::create(*device.device,
                                                                       *identityCache.cache)){};
        expectations.expect(identityCache.hasValue() && identityExecutor.hasValue(),
                            "the identity executor creates");
        if (identityExecutor) {
            cacheIdentityAndInvalidation(expectations, *identityExecutor.executor, *identityKeyed,
                                         identityFixture);
            cancellationAndBudget(expectations, *identityExecutor.executor, identityKeyed->start);
        }
        retirement(expectations, *device.device, *identityKeyed);

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " shape morph expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: Shape Morph on the GPU matches the CPU reference, cold and warm, with "
                     "cache identity, invalidation, cancellation, budget and retirement\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
