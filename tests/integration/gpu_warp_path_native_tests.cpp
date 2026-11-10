// #598 Warp Path and Transform Path skew on the production GPU route
// (docs/architecture/path-processing.md).
//
// Real documents are built through commands and compiled by the production SnapshotCompiler: a
// keyed Path node feeds Warp Path (or Transform Path), which feeds Shape Source's `path`. Both are
// host value computation (they answer a Path the existing fill draws), so the proof is that the warped
// path reaches the same coverage producer on the device and matches the CPU reference (2e-6), cold and
// warm, at 512 px over keyed frames in every function and in a skewed, turned and scaled space, and
// that a 1080p frame prepares and runs on the production route without refusal. The prepared identity
// changes exactly where the warped path does, an edit of the node or of an upstream key re-dispatches,
// preparation and the native job cancel, the budget refuses, and an evicted frame renders again.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/warp_path_nodes.hpp>
#include <bloom/document/shape.hpp>
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

constexpr std::int64_t kWave = 0, kTwist = 1, kBulge = 2, kBend = 3, kRipple = 4;

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

// Two contours with a hole, so every contour of a compound path is resampled.
PathValue ring(const double outer, const double inner) {
    PathValue value;
    value.contours = {circle(0, 0, outer), circle(0, 0, inner)};
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
    document::NodeId producer, warp, shape;

    Fixture(const std::uint32_t width, const std::uint32_t height, const std::int64_t function,
            const std::string_view type = document::kWarpPathNodeType)
        : initial(document::makeNewProject(
              "Warp", "Main", core::RationalTime::fromInteger(4),
              *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))),
          main(initial.initialCompositionId), document(std::move(initial.project)),
          commands(document) {
        execute<commands::AddNode>(document, commands, main,
                                   std::string(document::kPathValueNodeType), Vec2d{0, 0});
        producer = lastNode(document, main, document::kPathValueNodeType);
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
        execute<commands::AddNode>(document, commands, main, std::string(type), Vec2d{200, 0});
        warp = lastNode(document, main, type);
        execute<commands::ConnectPorts>(
            document, commands, main,
            document::OutputPortRef{producer, std::string(document::kValuePortName)},
            document::NodeInputRef{warp, "path"});
        execute<commands::ConnectPorts>(document, commands, main,
                                        document::OutputPortRef{warp, "path"},
                                        document::NodeInputRef{shape, "path"});
        if (type == document::kWarpPathNodeType)
            setNumber(warp, "function", function);
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
    void key(const std::int64_t second, PathValue value) {
        execute<commands::SetKeyframeAtTimeForParameter>(
            document, commands, main,
            parameter(document, main, producer, document::kValueParameterRole),
            core::RationalTime::fromInteger(second), std::move(value));
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
        TaskRequest("gpu path value cancel token",
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

void buildKeyed(Fixture& fixture) {
    fixture.key(0, ring(150, 70));
    fixture.key(1, ring(220, 110));
}

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

// Cache identity on the device: a frame whose resampled path did not change runs nothing new; a
// frame whose path changed, an edited upstream key and an edited node operand do.
void cacheIdentityAndInvalidation(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                                  const KeyedStates& keyed, Fixture& fixture) {
    const runtime::CpuGpuSceneBuilder builder;
    const auto end = builder.build(keyed.end.plan, keyed.end.request);
    const auto middle = builder.build(keyed.middle.plan, keyed.middle.request);
    expectations.expect(end.hasValue() && middle.hasValue(), "identity: every keyed state prepares");
    if (!end || !middle)
        return;
    expectations.expect(!runScene(executor, end.scene).empty(), "identity: the second key runs");
    const auto beforeAgain = executor.counters();
    expectations.expect(parity(runScene(executor, end.scene),
                               keyed.end.frame->processImage().pixels()),
                        "identity: the same frame again matches");
    expectations.expect(executor.counters().dispatches == beforeAgain.dispatches,
                        "identity: a frame whose warped path is unchanged dispatches nothing new");
    const auto beforeMove = executor.counters();
    const auto moved = runScene(executor, middle.scene);
    expectations.expect(executor.counters().dispatches > beforeMove.dispatches,
                        "identity: a frame whose warped path moved is dispatched");
    expectations.expect(parity(moved, keyed.middle.frame->processImage().pixels()),
                        "identity: an interpolated frame matches the CPU reference");

    // An edit of the key the middle frame reads.
    fixture.key(0, ring(120, 60));
    const auto edited = referenceAt(expectations, fixture, 2, "edited key");
    expectations.expect(edited && !samePixels(*edited, keyed.middle),
                        "invalidation: the edited upstream key changes the CPU reference");
    // An edit of an operand of the node itself.
    fixture.setNumber(fixture.warp, "amount", 31.0);
    const auto fewer = referenceAt(expectations, fixture, 2, "edited amount");
    expectations.expect(fewer && edited && !samePixels(*fewer, *edited),
                        "invalidation: the edited amount changes the CPU reference");
    for (const auto* state : {&edited, &fewer}) {
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


// Retirement: the warped-path geometry a frame prepared is retained by the scene cache under its
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
                        "retirement: a cleared cache retains no warped-path geometry");
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
            std::int64_t function;
            double amount, size, phase;
            Vec2d position, scale;
            double rotation, skew, skewAxis;
        };
        // Each function, and the wave again in a space turned, slanted, stretched and moved.
        const Variant variants[] = {
            {"wave", kWave, 18, 120, 0, {0, 0}, {1, 1}, 0, 0, 0},
            {"wave with phase", kWave, 18, 120, 70, {0, 0}, {1, 1}, 0, 0, 0},
            {"twist", kTwist, 70, 200, 0, {0, 0}, {1, 1}, 0, 0, 0},
            {"bulge", kBulge, 35, 200, 0, {0, 0}, {1, 1}, 0, 0, 0},
            {"pinch", kBulge, -40, 200, 0, {0, 0}, {1, 1}, 0, 0, 0},
            {"bend", kBend, 90, 300, 0, {0, 0}, {1, 1}, 0, 0, 0},
            {"ripple", kRipple, 12, 60, 30, {0, 0}, {1, 1}, 0, 0, 0},
            {"wave in a shared space", kWave, 18, 120, 0, {10, -6}, {1.2, 0.8}, 20, 15, 30},
        };
        std::vector<std::pair<std::string, KeyedStates>> states;
        for (const auto& variant : variants) {
            Fixture fixture(512, 512, variant.function);
            fixture.setNumber(fixture.warp, "amount", variant.amount);
            fixture.setNumber(fixture.warp, "size", variant.size);
            fixture.setNumber(fixture.warp, "phase", variant.phase);
            fixture.setNumber(fixture.warp, "position", variant.position);
            fixture.setNumber(fixture.warp, "anchor", Vec2d{0, 0});
            fixture.setNumber(fixture.warp, "scale", variant.scale);
            fixture.setNumber(fixture.warp, "rotation", variant.rotation);
            fixture.setNumber(fixture.warp, "skew", variant.skew);
            fixture.setNumber(fixture.warp, "skewAxis", variant.skewAxis);
            buildKeyed(fixture);
            auto keyed = keyedStates(expectations, fixture, variant.label);
            if (!keyed)
                continue;
            expectations.expect(!samePixels(keyed->start, keyed->middle) &&
                                    !samePixels(keyed->middle, keyed->end),
                                std::string(variant.label) + ": the warped path moves");
            states.emplace_back(variant.label, std::move(*keyed));
        }
        // An open path, warped, still draws.
        {
            Fixture open(512, 512, kWave);
            open.setNumber(open.warp, "amount", 25.0);
            open.setNumber(open.warp, "size", 140.0);
            open.key(0, openWave(60));
            open.key(1, openWave(-60));
            auto keyed = keyedStates(expectations, open, "open path");
            if (keyed)
                states.emplace_back("open path", std::move(*keyed));
        }
        // Transform Path with a skew: exact matrix, no subdivision, drawn by the same fill.
        {
            Fixture skewed(512, 512, 0, document::kTransformPathNodeType);
            skewed.setNumber(skewed.warp, "skew", 22.0);
            skewed.setNumber(skewed.warp, "skewAxis", 25.0);
            skewed.setNumber(skewed.warp, "rotation", 12.0);
            skewed.setNumber(skewed.warp, "scale", Vec2d{1.1, 0.9});
            buildKeyed(skewed);
            auto keyed = keyedStates(expectations, skewed, "transform path skew");
            if (keyed)
                states.emplace_back("transform path skew", std::move(*keyed));
        }
        // Large frame: prepares and runs on the production route below (no CPU reference compare).
        Fixture large(1920, 1080, kWave);
        large.setNumber(large.warp, "amount", 30.0);
        large.setNumber(large.warp, "size", 200.0);
        buildKeyed(large);
        expectations.expect(states.size() == 10,
                            "every function, the shared space, the open path and the skewed Transform "
                            "Path have a reference");
        // The reference for the cache, retirement and cancellation work: the wave.
        Fixture identityFixture(512, 512, kWave);
        identityFixture.setNumber(identityFixture.warp, "amount", 20.0);
        identityFixture.setNumber(identityFixture.warp, "size", 130.0);
        buildKeyed(identityFixture);
        const auto identityKeyed = keyedStates(expectations, identityFixture, "identity");
        if (expectations.failures() != 0 || !identityKeyed) {
            std::cerr << expectations.failures() << " warp path expectation(s) failed\n";
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
            std::cerr << expectations.failures() << " warp path expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: Warp Path and Transform Path skew on the GPU match the CPU reference, cold and warm, with "
                     "cache identity, invalidation, cancellation, budget and retirement\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
