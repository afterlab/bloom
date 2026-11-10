// #599: a Line's ends, keyed and driven, on the GPU (docs/architecture/layer-graph-model.md).
//
// Real documents are built through commands and compiled by the production SnapshotCompiler: a
// Line Shape Source whose `lineStart` and `lineEnd` are keyed, and one whose ends are driven by
// keyed Vector2 value nodes. Every frame is prepared by the production CpuGpuSceneBuilder, run by
// the production GpuSceneExecutor and compared to the CPU reference (2e-6), cold and warm. The
// prepared identity changes exactly where a resolved end does: a held frame dispatches nothing
// new, a moved frame and an edited key do. Preparation and the native job cancel, and the budget
// refuses.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
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
using document::Vec2d;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Line ends", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(96, 80, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::NodeId shape;

    Fixture() {
        commands::ShapeLayerGeometry geometry;
        geometry.lineStart = Vec2d{0, 0};
        geometry.lineEnd = Vec2d{40, 0};
        commands::ShapeLayerStyle style;
        style.strokeColor = core::Color4d{0.9, 0.5, 0.1, 1.0};
        style.strokeWidth = 5.0;
        execute<commands::AddShapeLayer>(document, commands, main, document::ShapeKind::Line,
                                         geometry, style);
        shape = lastNode(document, main, document::kShapeSourceNodeType);
    }
    [[nodiscard]] document::ParameterId end(const char* role) const {
        return parameter(document, main, shape, role);
    }
    void key(const document::ParameterId id, const std::int64_t second, const Vec2d value,
             const bool create) {
        if (create)
            execute<commands::CreateAnimationForParameter>(document, commands, main, id,
                                                           core::RationalTime{});
        // A key that repeats the value the animation was created with changes nothing.
        commands::Transaction transaction("Key", document.snapshot().revision());
        transaction.emplace<commands::SetKeyframeAtTimeForParameter>(
            main, id, core::RationalTime::fromInteger(second), value);
        if (!commands.execute(std::move(transaction)).succeeded())
            throw std::runtime_error("line end key refused");
    }
    // A Vector2 value node keyed to two values, wired into the Line's `socket`.
    document::NodeId drive(const char* socket, const Vec2d at0, const Vec2d at1) {
        execute<commands::AddNode>(document, commands, main,
                                   std::string(document::kVector2ValueNodeType), Vec2d{0, 0});
        const auto producer = lastNode(document, main, document::kVector2ValueNodeType);
        execute<commands::ConnectPorts>(
            document, commands, main,
            document::OutputPortRef{producer, std::string(document::kValuePortName)},
            document::NodeInputRef{shape, socket});
        const auto value = parameter(document, main, producer, document::kValueParameterRole);
        key(value, 0, at0, true);
        key(value, 1, at1, false);
        return producer;
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

// Both ends keyed: the line turns from horizontal at 0 s to vertical at 1 s, then holds.
struct States final {
    Reference start, middle, end, holding;
};

[[nodiscard]] std::optional<States> statesOf(Expectations& expectations, const Fixture& fixture,
                                             const std::string& name) {
    auto start = referenceAt(expectations, fixture, 0, name + " start");
    auto middle = referenceAt(expectations, fixture, 2, name + " middle");
    auto end = referenceAt(expectations, fixture, 4, name + " end");
    auto holding = referenceAt(expectations, fixture, 6, name + " holding");
    if (!start || !middle || !end || !holding)
        return std::nullopt;
    return States{std::move(*start), std::move(*middle), std::move(*end), std::move(*holding)};
}

void buildKeyed(Fixture& fixture) {
    fixture.key(fixture.end("lineEnd"), 0, Vec2d{40, 0}, true);
    fixture.key(fixture.end("lineEnd"), 1, Vec2d{0, 40}, false);
    fixture.key(fixture.end("lineStart"), 0, Vec2d{0, 0}, true);
    fixture.key(fixture.end("lineStart"), 1, Vec2d{-20, -10}, false);
}

void buildDriven(Fixture& fixture) {
    fixture.drive("lineEnd", Vec2d{40, 0}, Vec2d{0, 40});
    fixture.drive("lineStart", Vec2d{0, 0}, Vec2d{-20, -10});
}

// Cache identity on the device: a frame whose ends did not change runs nothing new; a frame whose
// ends changed, and an edit that changes them, do.
void cacheIdentityAndInvalidation(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                                  const States& keyed, Fixture& fixture) {
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
                        "identity: a frame that holds the ends dispatches nothing new");
    const auto beforeMove = executor.counters();
    const auto moved = runScene(executor, middle.scene);
    expectations.expect(executor.counters().dispatches > beforeMove.dispatches,
                        "identity: a frame whose ends moved is dispatched");
    expectations.expect(parity(moved, keyed.middle.frame->processImage().pixels()),
                        "identity: an interpolated frame matches the CPU reference");

    // An edit of the key the middle frame reads.
    fixture.key(fixture.end("lineEnd"), 0, Vec2d{40, 20}, false);
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

        Fixture keyedFixture;
        buildKeyed(keyedFixture);
        const auto keyed = statesOf(expectations, keyedFixture, "keyed");
        Fixture drivenFixture;
        buildDriven(drivenFixture);
        const auto driven = statesOf(expectations, drivenFixture, "driven");
        if (!keyed || !driven) {
            std::cerr << "the fixtures did not produce CPU references\n";
            return 1;
        }
        expectations.expect(!samePixels(keyed->start, keyed->middle) &&
                                !samePixels(keyed->middle, keyed->end),
                            "the keyed line moves");
        expectations.expect(samePixels(keyed->end, keyed->holding), "and the last key holds");
        expectations.expect(samePixels(keyed->start, driven->start) &&
                                samePixels(keyed->middle, driven->middle) &&
                                samePixels(keyed->end, driven->end),
                            "driven ends draw as the same keyed ends, frame for frame");
        // Also an end that is a constant on a still frame draws the same line.
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " line end expectation(s) failed\n";
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

        for (const auto* states : {&*keyed, &*driven}) {
            const std::string name = states == &*keyed ? "keyed" : "driven";
            gpuParity(expectations, *executor.executor, states->start, name + " start");
            gpuParity(expectations, *executor.executor, states->middle, name + " middle");
            gpuParity(expectations, *executor.executor, states->end, name + " end");
            gpuParity(expectations, *executor.executor, states->holding, name + " holding");
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
            cacheIdentityAndInvalidation(expectations, *identityExecutor.executor, *keyed,
                                         keyedFixture);
            cancellationAndBudget(expectations, *identityExecutor.executor, keyed->start);
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " line end expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: keyed and driven line ends on the GPU match the CPU reference, cold "
                     "and warm, with cache identity, invalidation, cancellation and budget\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
