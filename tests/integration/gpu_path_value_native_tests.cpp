// #589: a path as a graph value on the GPU (docs/architecture/path-value.md).
//
// Real documents are built through commands and compiled by the production SnapshotCompiler: a Path
// node, constant and keyed, drives Shape Source's `path`, with compound contours, a fill rule the
// value owns, several consumers of one output and a key that holds. Every state is prepared by the
// production CpuGpuSceneBuilder, run by the production GpuSceneExecutor and compared to the CPU
// reference (2e-6) cold and warm. The prepared identity changes exactly where the resolved path
// does: a held frame dispatches nothing new, an interpolated frame and an edited key do.
// Preparation and the native job cancel, and the budget refuses.
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
#include <bloom/document/value_nodes.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using document::PathContour;
using document::PathFillRule;
using document::PathValue;
using document::Vec2d;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

PathContour square(const double left, const double top, const double size) {
    PathContour contour;
    contour.closed = true;
    contour.anchors = {{{left, top}, {}, {}},
                       {{left + size, top}, {}, {}},
                       {{left + size, top + size}, Vec2d{left + size + 6.0, top + size - 6.0}, {}},
                       {{left, top + size}, {}, {}}};
    return contour;
}

PathValue framed(const PathFillRule rule, const double inner) {
    PathValue value;
    value.contours = {square(8, 8, 60), square(inner, inner, 24)};
    value.fillRule = rule;
    return value;
}

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Path value", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(96, 80, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};

    document::NodeId addPathNode() {
        execute<commands::AddNode>(document, commands, main,
                                   std::string(document::kPathValueNodeType), Vec2d{0, 0});
        return lastNode(document, main, document::kPathValueNodeType);
    }
    document::NodeId addShape(const PathValue& path, const core::Color4d fill,
                              const bool stroked = false) {
        commands::ShapeLayerGeometry geometry;
        geometry.path = path;
        commands::ShapeLayerStyle style;
        style.fillColor = fill;
        if (stroked) {
            style.strokeEnabled = true;
            style.strokeColor = core::Color4d{0.1, 0.8, 0.2, 1.0};
            style.strokeWidth = 2.0;
        }
        execute<commands::AddShapeLayer>(document, commands, main, document::ShapeKind::Path,
                                         geometry, style);
        return lastNode(document, main, document::kShapeSourceNodeType);
    }
    void drive(const document::NodeId producer, const document::NodeId shape) {
        execute<commands::ConnectPorts>(
            document, commands, main,
            document::OutputPortRef{producer, std::string(document::kValuePortName)},
            document::NodeInputRef{shape, "path"});
    }
    void set(const document::NodeId producer, PathValue value) {
        execute<commands::SetParameterSource>(
            document, commands, main,
            parameter(document, main, producer, document::kValueParameterRole),
            document::ConstantValueSource{std::move(value)});
    }
    void key(const document::NodeId producer, const std::int64_t second, PathValue value) {
        execute<commands::SetKeyframeAtTimeForParameter>(
            document, commands, main,
            parameter(document, main, producer, document::kValueParameterRole),
            core::RationalTime::fromInteger(second), std::move(value));
    }
};

// The CPU reference of the fixture at quarter seconds.
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

// The keyed graph: two matching keys, then a key of a different shape that the second holds up to.
struct KeyedStates final {
    Reference start, middle, end, holding;
};

[[nodiscard]] std::optional<KeyedStates> keyedStates(Expectations& expectations,
                                                     const Fixture& fixture) {
    auto start = referenceAt(expectations, fixture, 0, "keyed start");
    auto middle = referenceAt(expectations, fixture, 2, "keyed middle");
    auto end = referenceAt(expectations, fixture, 4, "keyed end");
    auto holding = referenceAt(expectations, fixture, 6, "keyed holding");
    if (!start || !middle || !end || !holding)
        return std::nullopt;
    return KeyedStates{std::move(*start), std::move(*middle), std::move(*end), std::move(*holding)};
}

void buildKeyed(Fixture& fixture) {
    const auto producer = fixture.addPathNode();
    const auto shape = fixture.addShape(PathValue::fromContour(square(1, 1, 2)),
                                        core::Color4d{0.8, 0.3, 0.1, 1.0}, true);
    fixture.drive(producer, shape);
    fixture.key(producer, 0, framed(PathFillRule::EvenOdd, 14.0));
    fixture.key(producer, 1, framed(PathFillRule::EvenOdd, 34.0));
    auto oneContour = framed(PathFillRule::EvenOdd, 34.0);
    oneContour.contours.pop_back();
    fixture.key(producer, 2, oneContour);
}

// Cache identity on the device: a frame whose path did not change runs nothing new; a frame whose
// path changed, and an edit that changes it, do.
void cacheIdentityAndInvalidation(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                                  const KeyedStates& keyed, Fixture& fixture) {
    const runtime::CpuGpuSceneBuilder builder;
    const auto end = builder.build(keyed.end.plan, keyed.end.request);
    const auto holding = builder.build(keyed.holding.plan, keyed.holding.request);
    const auto middle = builder.build(keyed.middle.plan, keyed.middle.request);
    expectations.expect(end.hasValue() && holding.hasValue() && middle.hasValue(),
                        "identity: every keyed state prepares");
    if (!end || !holding || !middle)
        return;
    expectations.expect(!runScene(executor, end.scene).empty(), "identity: the second key runs");
    const auto beforeHold = executor.counters();
    const auto held = runScene(executor, holding.scene);
    expectations.expect(parity(held, keyed.holding.frame->processImage().pixels()),
                        "identity: a held frame matches the CPU reference");
    expectations.expect(executor.counters().dispatches == beforeHold.dispatches,
                        "identity: a frame that holds the path dispatches nothing new");
    const auto beforeMove = executor.counters();
    const auto moved = runScene(executor, middle.scene);
    expectations.expect(executor.counters().dispatches > beforeMove.dispatches,
                        "identity: a frame whose path moved is dispatched");
    expectations.expect(parity(moved, keyed.middle.frame->processImage().pixels()),
                        "identity: an interpolated frame matches the CPU reference");

    // An edit of the key the middle frame reads.
    const auto producer = lastNode(fixture.document, fixture.main, document::kPathValueNodeType);
    fixture.key(producer, 0, framed(PathFillRule::EvenOdd, 20.0));
    const auto edited = referenceAt(expectations, fixture, 2, "edited middle");
    expectations.expect(edited && !samePixels(*edited, keyed.middle),
                        "invalidation: the edited key changes the CPU reference");
    if (!edited)
        return;
    const auto scene = builder.build(edited->plan, edited->request);
    expectations.expect(scene.hasValue(), "invalidation: the edited state prepares");
    if (!scene)
        return;
    const auto beforeEdit = executor.counters();
    const auto editedPixels = runScene(executor, scene.scene);
    expectations.expect(parity(editedPixels, edited->frame->processImage().pixels()),
                        "invalidation: the edited frame matches the CPU reference");
    expectations.expect(executor.counters().dispatches > beforeEdit.dispatches,
                        "invalidation: the edited frame is dispatched, not served stale");
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
        const auto collect = [&](const Fixture& fixture, const std::string& label) {
            if (auto reference = referenceAt(expectations, fixture, 0, label))
                references.emplace_back(label, std::move(*reference));
        };

        // A constant compound path on the shape itself, under each fill rule, with a stroke.
        Fixture evenOdd;
        evenOdd.addShape(framed(PathFillRule::EvenOdd, 14.0), {0.2, 0.4, 0.9, 1.0}, true);
        collect(evenOdd, "constant even-odd");
        Fixture nonZero;
        const auto ruled =
            nonZero.addShape(framed(PathFillRule::NonZero, 14.0), {0.2, 0.4, 0.9, 0.7});
        // The value's rule wins over this parameter.
        execute<commands::SetParameterSource>(
            nonZero.document, nonZero.commands, nonZero.main,
            parameter(nonZero.document, nonZero.main, ruled, "fillRule"),
            document::ConstantValueSource{document::ParameterValue{std::int64_t{1}}});
        collect(nonZero, "constant nonzero over an even-odd parameter");

        // A constant Path node driving two shapes.
        Fixture shared;
        const auto producer = shared.addPathNode();
        shared.set(producer, framed(PathFillRule::EvenOdd, 14.0));
        const auto left = shared.addShape(PathValue::fromContour(square(1, 1, 2)), {1, 0, 0, 0.5});
        const auto right = shared.addShape(PathValue::fromContour(square(1, 1, 2)), {0, 0, 1, 0.5});
        shared.drive(producer, left);
        shared.drive(producer, right);
        collect(shared, "one output read twice");

        // A one-contour path, driven, is the shape it always was.
        Fixture oneDriven;
        const auto simple = oneDriven.addPathNode();
        oneDriven.set(simple, PathValue::fromContour(square(10, 10, 40)));
        oneDriven.drive(simple, oneDriven.addShape(PathValue::fromContour(square(1, 1, 2)),
                                                   {0.6, 0.6, 0.1, 1.0}));
        collect(oneDriven, "one contour driven");
        Fixture oneConstant;
        oneConstant.addShape(PathValue::fromContour(square(10, 10, 40)), {0.6, 0.6, 0.1, 1.0});
        collect(oneConstant, "one contour constant");
        expectations.expect(references.size() == 5, "every constant state has a CPU reference");
        if (references.size() == 5) {
            expectations.expect(samePixels(references[3].second, references[4].second),
                                "a driven one-contour path is the constant shape, pixel for pixel");
            expectations.expect(alphaAt(references[0].second, 30, 30) == 0.0F &&
                                    alphaAt(references[1].second, 30, 30) > 0.0F,
                                "the value's fill rule decides the hole");
        }

        // Keyed: a compound path that animates, then holds.
        Fixture keyedFixture;
        buildKeyed(keyedFixture);
        const auto keyed = keyedStates(expectations, keyedFixture);
        if (keyed) {
            expectations.expect(!samePixels(keyed->start, keyed->middle) &&
                                    !samePixels(keyed->middle, keyed->end),
                                "the keyed path moves");
            expectations.expect(samePixels(keyed->end, keyed->holding), "and the second key holds");
        }
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " path value expectation(s) failed\n";
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
        gpuParity(expectations, *executor.executor, keyed->start, "keyed start");
        gpuParity(expectations, *executor.executor, keyed->middle, "keyed middle");
        gpuParity(expectations, *executor.executor, keyed->end, "keyed end");
        gpuParity(expectations, *executor.executor, keyed->holding, "keyed holding");

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
            cacheIdentityAndInvalidation(expectations, *identityExecutor.executor, *keyed,
                                         keyedFixture);
            cancellationAndBudget(expectations, *identityExecutor.executor, keyed->start);
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " path value expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: compound and driven paths on the GPU match the CPU reference, cold and "
                     "warm, with cache identity, invalidation, cancellation and budget\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
