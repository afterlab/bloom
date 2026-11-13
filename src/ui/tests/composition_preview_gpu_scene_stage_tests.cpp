// Focused tests for the GPU-scene stage seam: the compile -> prepare-immutable-scene -> select
// display processor half, exercised through the REAL SnapshotCompiler, CpuGpuSceneBuilder,
// QualifiedDisplayProcessorProvider and TaskScheduler. Every scene is compared against a genuine
// CpuCompositionEvaluator frame: process identity (time/output/resolution/quality/color/ROI/
// showLook), per-operation evaluated bounds and layer IDs, output descriptor, and -- by replaying
// the prepared commands with the EXISTING CPU primitives through the frozen
// gpu_scene_preparation_test_support.hpp oracle -- every output pixel bit for bit.
//
// The CPU-stage regression is covered separately by re-linking the existing
// composition_preview_cpu_stage test against the mirrored TU (proof/run_cpu_stage_regression).

#include "await.hpp"
#include "gpu_scene_preparation_test_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan_cache.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/image_statistics.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QApplication>
#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using StageValue = bloom::runtime::PreviewGpuSceneStageOutcomeHandle;
using bloom::runtime::PreviewGpuSceneStageStatus;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskState;

[[nodiscard]] bloom::runtime::TaskSchedulerConfig schedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

[[nodiscard]] bloom::document::NewProject makeProject() {
    const auto format = bloom::document::CompositionFormat::create(64, 48);
    if (!format.has_value()) {
        std::abort();
    }
    return bloom::document::makeNewProject("GPU Scene Stage", "Main",
                                           bloom::core::RationalTime::fromInteger(10), *format);
}

[[nodiscard]] bloom::runtime::PreviewRequestIdentity
identityFor(const bloom::document::Snapshot& snapshot, const bloom::document::CompositionId id) {
    return {.projectId = snapshot.project().id(),
            .compositionId = id,
            .sourceRevision = snapshot.revision(),
            .requestGeneration = 1,
            .time = bloom::core::RationalTime::fromInteger(0),
            .output = bloom::runtime::PreviewOutput::Composition,
            .resolution = bloom::runtime::CompositionFormatResolution{},
            .quality = bloom::runtime::EvaluationQuality::Reference,
            .colorIntent = bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
            .resolutionPolicy = bloom::runtime::PreviewResolutionPolicy::Auto,
            .displayName = {},
            .viewName = {},
            .showLook = true};
}

template <typename Value, typename Callable>
[[nodiscard]] std::optional<TaskResult<Value>> runOnWorker(bloom::runtime::TaskScheduler& scheduler,
                                                           Callable callable) {
    bloom::runtime::TaskRequest request("GPU scene stage task",
                                        {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                         .id = bloom::runtime::TaskOwnerId::fromRaw(1)},
                                        bloom::runtime::TaskPriority::Visible);
    auto submission = scheduler.submit<Value>(
        std::move(request), [callable = std::move(callable)](bloom::runtime::TaskContext& context) {
            return callable(context);
        });
    if (!submission.accepted()) {
        return std::nullopt;
    }
    std::optional<TaskResult<Value>> result;
    if (!bloom::test_support::awaitCondition("GPU scene stage task reaches a terminal result", [&] {
            result = submission.handle.tryTakeResult();
            return result.has_value();
        })) {
        return std::nullopt;
    }
    return result;
}

struct Fixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::QualifiedDisplayProcessorProvider provider;
    bloom::runtime::CpuGpuSceneBuilder builder;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::CompiledPlanCacheHandle planCache = std::make_shared<bloom::ui::CompiledPlanCache>();

    Fixture() : compiler(definitions), scheduler(schedulerConfig()) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
    }

    // #349: the device measure the stage hands Image Statistics to (empty: none).
    bloom::runtime::ImageStatisticsDeviceMeasure statisticsMeasure;

    [[nodiscard]] bloom::runtime::PreviewGpuSceneStageFunction stage() const {
        return bloom::ui::makeCompositionPreviewGpuSceneStage(
            compiler, builder, provider, planCache, nullptr, statisticsMeasure);
    }
};

// Runs the stage on a scheduler worker and returns the outcome handle for a succeeded stage, or
// null after recording the failure/cancellation.
[[nodiscard]] std::shared_ptr<const bloom::runtime::PreviewGpuSceneStageOutcome>
runStage(Fixture& fixture, const bloom::runtime::PreviewRequestIdentity& identity,
         const bloom::document::Snapshot& snapshot,
         const std::vector<bloom::runtime::SnapshotParameterOverride>& overrides,
         Expectations& expectations, const std::string& label) {
    auto function = fixture.stage();
    auto result = runOnWorker<StageValue>(
        fixture.scheduler,
        [&function, &snapshot, &identity, &overrides](bloom::runtime::TaskContext& context) {
            return function(snapshot, identity, bloom::ui::kDefaultPreviewPixelStorageByteLimit,
                            overrides, context);
        });
    expectations.expect(result.has_value() && result->state() == TaskState::Succeeded,
                        label + ": the stage completes successfully");
    if (!result.has_value() || result->state() != TaskState::Succeeded) {
        return nullptr;
    }
    const auto& outcome = result->value();
    expectations.expect(outcome.has_value() && *outcome != nullptr,
                        label + ": the stage returns an outcome");
    return outcome.has_value() ? *outcome : nullptr;
}

// The genuine, independent CPU oracle: compile the same snapshot with the same compiler and
// evaluate the same request with the same evaluator, then compare identity, per-operation bounds
// and every output pixel to the prepared scene.
void compareSceneToCpuOracle(Fixture& fixture, const bloom::runtime::PreparedGpuScene& scene,
                             const bloom::document::Snapshot& snapshot,
                             const bloom::runtime::PreviewRequestIdentity& identity,
                             Expectations& expectations, const std::string& label) {
    const auto compileResult = fixture.compiler.compile(
        {.snapshot = snapshot, .compositionId = identity.compositionId, .parameterOverrides = {}},
        {});
    expectations.expect(compileResult.status == bloom::runtime::SnapshotCompileStatus::Compiled &&
                            compileResult.plan != nullptr,
                        label + ": the oracle compiles the same graph");
    if (compileResult.plan == nullptr) {
        return;
    }
    const bloom::runtime::EvaluationRequest request{
        .time = identity.time,
        .output = compileResult.plan->output(),
        .resolution = identity.resolution,
        .quality = identity.quality,
        .colorIntent = identity.colorIntent,
        .pixelStorageByteLimit = bloom::ui::kDefaultPreviewPixelStorageByteLimit,
        .roi = identity.roi,
        .bypassLookNodes = !identity.showLook,
    };
    const auto evaluated = fixture.evaluator.evaluate(compileResult.plan, request, {});
    expectations.expect(evaluated.status() == bloom::runtime::EvaluationStatus::Evaluated &&
                            evaluated.frame() != nullptr,
                        label + ": the oracle evaluates a frame");
    if (evaluated.frame() == nullptr) {
        return;
    }
    const auto& frame = *evaluated.frame();
    expectations.expect(scene.processIdentity() == frame.identity(),
                        label + ": prepared identity matches the CPU frame identity");
    expectations.expect(scene.outputDescriptor() == *frame.processImage().descriptor(),
                        label + ": prepared output descriptor matches the CPU frame descriptor");
    expectations.expect(scene.bounds().size() == frame.evaluatedBounds().size(),
                        label + ": prepared operation bounds cover the CPU frame");
    std::size_t comparedBounds = 0;
    for (std::size_t index = 0;
         index < scene.bounds().size() && index < frame.evaluatedBounds().size(); ++index) {
        expectations.expect(scene.bounds()[index] == frame.evaluatedBounds()[index],
                            label + ": operation " + std::to_string(index) +
                                " geometry and layer id match the CPU frame");
        ++comparedBounds;
    }
    expectations.expect(comparedBounds == frame.evaluatedBounds().size(),
                        label + ": every evaluated operation was compared");

    std::vector<std::shared_ptr<const bloom::render::Rgba32fImage>> images;
    expectations.expect(replayScene(scene, images),
                        label + ": the prepared scene replays with the CPU primitives");
    const auto output = scene.outputCommand() == bloom::runtime::kInvalidGpuSceneCommand
                            ? std::shared_ptr<const bloom::render::Rgba32fImage>{}
                            : images[scene.outputCommand()];
    expectations.expect(output != nullptr, label + ": the replay produced the output image");
    if (output == nullptr) {
        return;
    }
    const auto& cpuPixels = frame.processImage().pixels();
    expectations.expect(output->pixels().size() == cpuPixels.size(),
                        label + ": output pixel count matches the CPU frame");
    if (output->pixels().size() == cpuPixels.size()) {
        expectations.expect(
            std::memcmp(output->pixels().data(), cpuPixels.data(), cpuPixels.size_bytes()) == 0,
            label + ": every output pixel is bit-identical to the CPU frame");
    }
}

void testPreparedSolidMatchesCpuOracle(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "the fixture creates a solid layer");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);

    const auto outcome = runStage(fixture, identity, snapshot, {}, expectations, "Prepared solid");
    expectations.expect(outcome != nullptr &&
                            outcome->status == PreviewGpuSceneStageStatus::Prepared,
                        "a supported solid graph is prepared");
    if (outcome == nullptr || outcome->stage == nullptr) {
        return;
    }
    const auto& stage = *outcome->stage;
    expectations.expect(stage.scene() != nullptr, "the prepared stage owns an immutable scene");
    expectations.expect(stage.desiredIdentity() == identity,
                        "the stage retains the request identity");
    expectations.expect(stage.ocioQualified() && stage.displayProcessor() != nullptr,
                        "the Ready provider selects the qualified display processor");
    if (stage.scene() != nullptr) {
        compareSceneToCpuOracle(fixture, *stage.scene(), snapshot, identity, expectations,
                                "Prepared solid");
    }
}

// Text is now a supported prepared operation: the stage must prepare it, not fake an unsupported
// fallback.
void testTextIsPrepared(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(session.addTextLayer(QStringLiteral("Title"), QStringLiteral("Bloom")),
                        "the fixture creates a text layer");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);
    const auto outcome = runStage(fixture, identity, snapshot, {}, expectations, "Text");
    expectations.expect(outcome != nullptr &&
                            outcome->status == PreviewGpuSceneStageStatus::Prepared &&
                            outcome->stage != nullptr,
                        "a text graph is prepared by the production GPU scene builder");
}

// Rotation is now a supported affine prepared operation.
void testRotationIsPrepared(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Rotated"), bloom::core::Color4d{0.7, 0.2, 0.1, 1.0}),
        "the fixture creates a solid layer");
    expectations.expect(session.setSelectedRotation(30.0), "the solid layer is rotated");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);
    const auto outcome = runStage(fixture, identity, snapshot, {}, expectations, "Rotation");
    expectations.expect(outcome != nullptr &&
                            outcome->status == PreviewGpuSceneStageStatus::Prepared &&
                            outcome->stage != nullptr,
                        "a rotated layer is prepared by the production GPU scene builder");
}

// A request ROI is now resolved by the production GPU scene builder exactly as the CPU evaluator
// resolves it: the stage prepares the clipped scene, and the prepared scene must match the CPU
// frame identity, output descriptor, evaluated geometry, and every output pixel through the
// existing oracle.
void testRoiIsPrepared(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "the fixture creates a solid layer");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = session.snapshot();
    auto identity = identityFor(snapshot, compositionId);
    const auto roi = bloom::render::ImageWindow::create(0, 0, 32, 24);
    expectations.expect(roi.hasValue(), "the ROI is created");
    if (roi.hasValue()) {
        identity.roi = *roi.value();
    }
    const auto outcome = runStage(fixture, identity, snapshot, {}, expectations, "ROI");
    expectations.expect(outcome != nullptr &&
                            outcome->status == PreviewGpuSceneStageStatus::Prepared &&
                            outcome->stage != nullptr,
                        "a supported ROI request is prepared by the production GPU scene builder");
    if (outcome == nullptr || outcome->stage == nullptr || outcome->stage->scene() == nullptr) {
        return;
    }
    compareSceneToCpuOracle(fixture, *outcome->stage->scene(), snapshot, identity, expectations,
                            "ROI");
}

// Genuine unsupported coverage is retained: the prepared GPU subset refuses a request whose quality
// is not Reference, and the stage must take the full original CPU fallback without fabricating a
// scene. EvaluationQuality currently has one enumerator, so this out-of-enum value exercises the
// builder's explicit quality guard exactly as a future non-Reference quality would.
void testNonReferenceQualityIsGpuSubsetFallback(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "the fixture creates a solid layer");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = session.snapshot();
    auto identity = identityFor(snapshot, compositionId);
    // This negative test deliberately supplies an out-of-range enum to exercise the quality guard.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- intentional invalid input.
    identity.quality = static_cast<bloom::runtime::EvaluationQuality>(255);
    const auto outcome =
        runStage(fixture, identity, snapshot, {}, expectations, "Non-Reference quality");
    expectations.expect(outcome != nullptr &&
                            outcome->status == PreviewGpuSceneStageStatus::UnsupportedGpuSubset,
                        "a non-Reference quality request takes the full original CPU fallback");
    expectations.expect(outcome != nullptr && outcome->stage == nullptr,
                        "an unsupported GPU subset never fabricates a stage or empty frame");
}

void testPendingReferencePreparesWithoutProcessor(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.1, 0.2, 0.3, 1.0}),
        "the fixture creates a solid layer");
    // The provider is never published: the honest Pending startup window.
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);
    const auto outcome = runStage(fixture, identity, snapshot, {}, expectations, "Pending");
    expectations.expect(outcome != nullptr &&
                            outcome->status == PreviewGpuSceneStageStatus::Prepared,
                        "a pending provider still prepares the scene");
    expectations.expect(outcome != nullptr && outcome->stage != nullptr &&
                            !outcome->stage->ocioQualified(),
                        "the pending window selects no qualified processor");
}

void testFailedProviderFailsClosed(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.1, 0.9, 0.4, 1.0}),
        "the fixture creates a solid layer");
    fixture.provider.publish(bloom::runtime::QualifiedDisplayProcessorBuildResult::failed(
        {.code = "bloom.test.gpu-scene-stage.forced-failure",
         .severity = bloom::runtime::DiagnosticSeverity::Error,
         .summary = "The Bloom Neutral display configuration could not be resolved",
         .detail = {},
         .suggestedAction = {}}));
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);
    auto function = fixture.stage();
    auto result = runOnWorker<StageValue>(
        fixture.scheduler, [&function, &snapshot, &identity](bloom::runtime::TaskContext& context) {
            return function(snapshot, identity, bloom::ui::kDefaultPreviewPixelStorageByteLimit, {},
                            context);
        });
    expectations.expect(result.has_value() && result->state() == TaskState::Failed,
                        "a permanently failed provider fails the stage closed");
    expectations.expect(result.has_value() && !result->diagnostics().empty(),
                        "the failure carries the provider's diagnostic");
}

void testOverrideChangesSceneWithoutPoisoningPlanCache(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.2, 0.2, 0.2, 1.0}),
        "the fixture creates a solid layer");
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);

    bloom::document::ParameterId colorParameter;
    const auto* liveComposition = snapshot.project().findComposition(compositionId);
    expectations.expect(liveComposition != nullptr, "the live composition resolves");
    if (liveComposition != nullptr) {
        for (const auto& record : liveComposition->parameters().records()) {
            if (record.schemaKey == bloom::document::kSolidColorParameterSchemaKey) {
                colorParameter = record.id;
            }
        }
    }
    expectations.expect(colorParameter.isValid(), "the solid exposes its color parameter");

    const auto baseline = runStage(fixture, identity, snapshot, {}, expectations, "Base");
    expectations.expect(baseline != nullptr && baseline->stage != nullptr &&
                            baseline->stage->scene() != nullptr,
                        "the baseline prepares a scene");
    const auto baselineStats = fixture.planCache->statistics();
    expectations.expect(baselineStats.compiles == 1 && baselineStats.hits == 0,
                        "the baseline compiles the revision exactly once");

    const std::vector<bloom::runtime::SnapshotParameterOverride> overrides{
        {snapshot.revision(), colorParameter, bloom::core::Color4d{0.9, 0.1, 0.1, 1.0}}};
    const auto overridden =
        runStage(fixture, identity, snapshot, overrides, expectations, "Override");
    expectations.expect(overridden != nullptr && overridden->stage != nullptr &&
                            overridden->stage->scene() != nullptr,
                        "the override prepares its own scene");
    const auto overrideStats = fixture.planCache->statistics();
    expectations.expect(
        overrideStats.compiles == 2 && overrideStats.hits == 0,
        "an override compiles its own plan and is never retained as the revision's");

    const auto reserved = runStage(fixture, identity, snapshot, {}, expectations, "Reserved");
    const auto reservedStats = fixture.planCache->statistics();
    expectations.expect(reservedStats.hits >= 1,
                        "the revision's own plan is still served from the cache afterwards");

    if (baseline != nullptr && baseline->stage != nullptr && baseline->stage->scene() != nullptr &&
        overridden != nullptr && overridden->stage != nullptr &&
        overridden->stage->scene() != nullptr && reserved != nullptr &&
        reserved->stage != nullptr && reserved->stage->scene() != nullptr) {
        const auto outputKey = [](const bloom::runtime::PreparedGpuScene& scene) {
            const auto command = scene.outputCommand();
            if (command == bloom::runtime::kInvalidGpuSceneCommand) {
                return std::string{};
            }
            return std::visit([](const auto& item) { return item.semanticKey; },
                              scene.commands()[command]);
        };
        const auto baselineKey = outputKey(*baseline->stage->scene());
        const auto overriddenKey = outputKey(*overridden->stage->scene());
        const auto reservedKey = outputKey(*reserved->stage->scene());
        expectations.expect(!baselineKey.empty() && baselineKey != overriddenKey,
                            "the override changes the prepared scene's semantic identity");
        expectations.expect(baselineKey == reservedKey,
                            "the cached revision still prepares the identical scene");
    }
}

// Pre-cancellation through the real scheduler: occupy the one CPU worker so the stage stays queued,
// cancel its handle, then release the worker. The stage must observe the cancellation before any
// work and return TaskResult::cancelled -- never a fake empty scene.
void testPreCancellationStaysCancelled(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.3, 0.3, 0.3, 1.0}),
        "the fixture creates a solid layer");
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);
    auto function = fixture.stage();

    std::atomic_bool release{false};
    auto blocker = fixture.scheduler.submit<int>(
        bloom::runtime::TaskRequest("blocker",
                                    {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(2)},
                                    bloom::runtime::TaskPriority::Visible),
        [&release](bloom::runtime::TaskContext&) {
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            return TaskResult<int>::succeeded(0);
        });
    expectations.expect(blocker.accepted(), "the blocker occupies the single worker");

    auto submission = fixture.scheduler.submit<StageValue>(
        bloom::runtime::TaskRequest("cancelled stage",
                                    {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(3)},
                                    bloom::runtime::TaskPriority::Visible),
        [&function, &snapshot, &identity](bloom::runtime::TaskContext& context) {
            return function(snapshot, identity, bloom::ui::kDefaultPreviewPixelStorageByteLimit, {},
                            context);
        });
    expectations.expect(submission.accepted(), "the stage is accepted behind the blocker");
    submission.handle.cancel();
    release.store(true);

    std::optional<TaskResult<StageValue>> stageResult;
    expectations.expect(bloom::test_support::awaitCondition(
                            "the cancelled stage reaches a terminal state",
                            [&] {
                                if (auto taken = submission.handle.tryTakeResult()) {
                                    stageResult = std::move(*taken);
                                }
                                return stageResult.has_value();
                            }) &&
                            stageResult.has_value(),
                        "the cancelled stage reaches a terminal state");
    expectations.expect(stageResult.has_value() && stageResult->state() == TaskState::Cancelled,
                        "a pre-cancelled stage returns TaskResult::cancelled");
    if (auto blockerResult = blocker.handle.tryTakeResult()) {
        (void)blockerResult;
    }
}

// #349: a Solid on a layer whose opacity is the Solid's Image Statistics maximum, built through
// commands as an artist's edits would be. (The native viewer and RAM preview run drives Levels and
// Denoise; these host-only tests use no image effect, which needs the packaged shader tools.)
struct StatisticsComposition final {
    bloom::document::Document document;
    bloom::commands::CommandStack commands;
    bloom::document::CompositionId compositionId;
    bloom::ui::CompositionSession session;
    bool built = false;

    explicit StatisticsComposition(bloom::document::NewProject project)
        : document(std::move(project.project)), commands(document),
          compositionId(project.initialCompositionId), session(document, commands, compositionId) {
        using namespace bloom;
        if (!session.addSolidLayer(QStringLiteral("Measured"), core::Color4d{0.6, 0.3, 0.15, 1.0}))
            return;
        const auto& graph = session.composition()->graph();
        const auto layer = graph.layerOutputs().front().nodeId;
        document::NodeId solid;
        for (const auto& edge : graph.edges())
            if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                input != nullptr && input->nodeId == layer && input->port == "image")
                solid = edge.source.nodeId;
        const auto add = [&](const std::string_view type) {
            commands::Transaction transaction("Add", session.snapshot().revision());
            transaction.emplace<commands::AddNode>(compositionId, std::string(type),
                                                   document::Vec2d{});
            return session.executeNodeTransaction(std::move(transaction))
                .outputId<document::NodeId>(commands::kAddNodeOutput)
                .value_or(document::NodeId{});
        };
        const auto connect = [&](const document::NodeId from, const std::string_view output,
                                 const document::NodeId to, const std::string_view input) {
            commands::Transaction transaction("Connect", session.snapshot().revision());
            transaction.emplace<commands::ConnectPorts>(
                compositionId, document::OutputPortRef{from, std::string(output)},
                document::NodeInputRef{to, std::string(input)});
            return session.executeNodeTransaction(std::move(transaction)).changed();
        };
        const auto statistics = add(document::kImageStatisticsNodeType);
        built = solid.isValid() && statistics.isValid() &&
                connect(solid, "image", statistics, "input") &&
                connect(statistics, "maximum", layer, document::kOpacityParameterRole);
    }
};

// The statistics the CPU reference measures for `query` in `frame`, labelled as `route` says (a
// stand-in for the device route in these host-only tests; the native viewer and RAM preview run
// measures them on the device).
[[nodiscard]] bloom::runtime::ImageStatisticsRun
measureOnCpu(const bloom::runtime::CpuCompositionEvaluator& evaluator,
             const bloom::runtime::ImageStatisticsQuery& query,
             const bloom::runtime::EvaluationRequest& frame,
             const bloom::runtime::CancellationToken& token,
             const bloom::runtime::ImageStatisticsRoute route) {
    const auto evaluated =
        evaluator.evaluate(query.plan, bloom::runtime::imageStatisticsRequest(frame, query), token);
    if (evaluated.frame() == nullptr)
        return {{bloom::runtime::ImageStatisticsOutcome::Unavailable, "not evaluated"}, {}};
    auto values = bloom::runtime::readImageStatistics(evaluated.frame()->processImage(), route);
    if (!values)
        return {{bloom::runtime::ImageStatisticsOutcome::Unavailable, "not readable"}, {}};
    return {{bloom::runtime::ImageStatisticsOutcome::Ready, {}}, *values};
}

[[nodiscard]] bool hasDiagnostic(const bloom::runtime::PreviewGpuSceneStageOutcome& outcome,
                                 const std::string_view code) {
    return std::ranges::any_of(outcome.diagnostics,
                               [&](const auto& diagnostic) { return diagnostic.code == code; });
}

// Without a device measure, a frame that reads Image Statistics is not prepared: it takes the CPU
// path, which measures them (the behaviour before the viewer measured them on the device).
void testStatisticsWithoutMeasureTakeTheCpuPath(Expectations& expectations) {
    bloom::runtime::clearImageStatistics();
    Fixture fixture;
    StatisticsComposition composition(makeProject());
    expectations.expect(composition.built, "the statistics composition is built");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = composition.session.snapshot();
    const auto identity = identityFor(snapshot, composition.compositionId);
    const auto outcome =
        runStage(fixture, identity, snapshot, {}, expectations, "Statistics without a measure");
    expectations.expect(
        outcome != nullptr && outcome->status == PreviewGpuSceneStageStatus::UnsupportedGpuSubset &&
            outcome->stage == nullptr &&
            hasDiagnostic(*outcome, "bloom.preview.gpu-scene.unsupported-operation"),
        "without a device measure the statistics frame takes the CPU path");
}

// With a device measure, the stage prepares the frame on the GPU with the statistics the measure
// gave for this frame (its own time), caches them under the frame's Device identity, and a second
// frame with the same identity measures nothing.
void testStatisticsMeasuredForTheFrameArePrepared(Expectations& expectations) {
    bloom::runtime::clearImageStatistics();
    Fixture fixture;
    std::atomic<int> measures{0};
    std::optional<bloom::core::RationalTime> measuredTime;
    fixture.statisticsMeasure = [&](const bloom::runtime::ImageStatisticsQuery& query,
                                    const bloom::runtime::EvaluationRequest& frame,
                                    const bloom::runtime::CancellationToken& token) {
        ++measures;
        measuredTime = frame.time;
        return measureOnCpu(fixture.evaluator, query, frame, token,
                            bloom::runtime::ImageStatisticsRoute::Device);
    };
    StatisticsComposition composition(makeProject());
    expectations.expect(composition.built, "the statistics composition is built");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = composition.session.snapshot();
    auto identity = identityFor(snapshot, composition.compositionId);
    identity.time = bloom::core::RationalTime::fromInteger(2);
    const auto outcome =
        runStage(fixture, identity, snapshot, {}, expectations, "Measured statistics");
    expectations.expect(outcome != nullptr &&
                            outcome->status == PreviewGpuSceneStageStatus::Prepared &&
                            outcome->stage != nullptr && outcome->stage->scene() != nullptr,
                        "a frame whose statistics the device measured is prepared on the GPU");
    expectations.expect(measures.load() == 1 && measuredTime == identity.time,
                        "the statistics were measured once, for the frame's own time");
    const auto afterFirst = bloom::runtime::imageStatisticsCacheStatistics();
    expectations.expect(afterFirst.computed == 1 && afterFirst.computedOnDevice == 1 &&
                            afterFirst.entries == 1,
                        "the measured statistics are cached as the device's");
    // The CPU oracle measures its own (CPU reference) statistics, kept apart from the device's.
    if (outcome != nullptr && outcome->stage != nullptr && outcome->stage->scene() != nullptr)
        compareSceneToCpuOracle(fixture, *outcome->stage->scene(), snapshot, identity, expectations,
                                "Measured statistics");
    const auto again = runStage(fixture, identity, snapshot, {}, expectations, "Measured again");
    expectations.expect(again != nullptr && again->status == PreviewGpuSceneStageStatus::Prepared,
                        "the same frame is prepared again");
    expectations.expect(measures.load() == 1 &&
                            bloom::runtime::imageStatisticsCacheStatistics().computedOnDevice == 1,
                        "the same frame's statistics are not measured again");
    identity.time = bloom::core::RationalTime::fromInteger(3);
    const auto later = runStage(fixture, identity, snapshot, {}, expectations, "Another frame");
    expectations.expect(later != nullptr && later->status == PreviewGpuSceneStageStatus::Prepared &&
                            measures.load() == 2 && measuredTime == identity.time,
                        "another frame's statistics are measured for that frame");
}

// A measure the device could not make (Unavailable, or values labelled with another route) never
// prepares the frame with unmeasured values: it is a GPU-subset refusal with the reason, and
// nothing is cached.
void testUnavailableStatisticsTakeTheCpuPath(Expectations& expectations) {
    for (const bool mislabelled : {false, true}) {
        bloom::runtime::clearImageStatistics();
        Fixture fixture;
        fixture.statisticsMeasure = [&](const bloom::runtime::ImageStatisticsQuery& query,
                                        const bloom::runtime::EvaluationRequest& frame,
                                        const bloom::runtime::CancellationToken& token)
            -> bloom::runtime::ImageStatisticsRun {
            if (mislabelled)
                return measureOnCpu(fixture.evaluator, query, frame, token,
                                    bloom::runtime::ImageStatisticsRoute::CpuReference);
            return {{bloom::runtime::ImageStatisticsOutcome::Unavailable,
                     "The device route is not ready to measure the picture."},
                    {}};
        };
        StatisticsComposition composition(makeProject());
        expectations.expect(composition.built, "the statistics composition is built");
        fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
        const auto snapshot = composition.session.snapshot();
        const auto identity = identityFor(snapshot, composition.compositionId);
        const std::string label = mislabelled ? "CPU-labelled statistics" : "Unavailable device";
        const auto outcome = runStage(fixture, identity, snapshot, {}, expectations, label);
        expectations.expect(
            outcome != nullptr &&
                outcome->status == PreviewGpuSceneStageStatus::UnsupportedGpuSubset &&
                outcome->stage == nullptr &&
                hasDiagnostic(*outcome, "bloom.preview.gpu-scene.statistics-unavailable"),
            label + ": the frame takes the CPU path and says why");
        const auto statistics = bloom::runtime::imageStatisticsCacheStatistics();
        expectations.expect(statistics.entries == 0 && statistics.computed == 0 &&
                                statistics.failed == 1,
                            label + ": nothing is cached");
    }
}

// Cancelling the frame while the device measures it cancels the measure: the stage is cancelled and
// nothing is cached.
void testCancelledStatisticsCacheNothing(Expectations& expectations) {
    bloom::runtime::clearImageStatistics();
    Fixture fixture;
    std::atomic_bool entered{false};
    std::atomic_bool sawCancellation{false};
    fixture.statisticsMeasure =
        [&](const bloom::runtime::ImageStatisticsQuery&, const bloom::runtime::EvaluationRequest&,
            const bloom::runtime::CancellationToken& token) -> bloom::runtime::ImageStatisticsRun {
        entered.store(true);
        sawCancellation.store(
            bloom::test_support::awaitCondition("the device measure sees the cancellation",
                                                [&] { return token.isCancellationRequested(); }));
        return {{bloom::runtime::ImageStatisticsOutcome::Cancelled, {}}, {}};
    };
    StatisticsComposition composition(makeProject());
    expectations.expect(composition.built, "the statistics composition is built");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = composition.session.snapshot();
    const auto identity = identityFor(snapshot, composition.compositionId);
    auto function = fixture.stage();
    auto submission = fixture.scheduler.submit<StageValue>(
        bloom::runtime::TaskRequest("statistics stage",
                                    {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(4)},
                                    bloom::runtime::TaskPriority::Visible),
        [&function, &snapshot, &identity](bloom::runtime::TaskContext& context) {
            return function(snapshot, identity, bloom::ui::kDefaultPreviewPixelStorageByteLimit, {},
                            context);
        });
    expectations.expect(submission.accepted(), "the statistics stage is accepted");
    expectations.expect(bloom::test_support::awaitCondition("the stage asks the device to measure",
                                                            [&] { return entered.load(); }) &&
                            entered.load(),
                        "the stage asked the device to measure the picture");
    submission.handle.cancel();
    std::optional<TaskResult<StageValue>> result;
    expectations.expect(bloom::test_support::awaitCondition(
                            "the cancelled statistics stage reaches a terminal state",
                            [&] {
                                if (auto taken = submission.handle.tryTakeResult())
                                    result = std::move(*taken);
                                return result.has_value();
                            }),
                        "the cancelled statistics stage reaches a terminal state");
    expectations.expect(sawCancellation.load(), "the cancellation reached the device measure");
    expectations.expect(result.has_value() && result->state() == TaskState::Cancelled,
                        "the stage is cancelled, not prepared and not a CPU fallback");
    const auto statistics = bloom::runtime::imageStatisticsCacheStatistics();
    expectations.expect(statistics.entries == 0 && statistics.computed == 0 &&
                            statistics.cancelled == 1,
                        "a cancelled measure caches nothing");
}

} // namespace

int runTests(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testPreparedSolidMatchesCpuOracle(expectations);
    testTextIsPrepared(expectations);
    testRotationIsPrepared(expectations);
    testRoiIsPrepared(expectations);
    testNonReferenceQualityIsGpuSubsetFallback(expectations);
    testPendingReferencePreparesWithoutProcessor(expectations);
    testFailedProviderFailsClosed(expectations);
    testOverrideChangesSceneWithoutPoisoningPlanCache(expectations);
    testPreCancellationStaysCancelled(expectations);
    testStatisticsWithoutMeasureTakeTheCpuPath(expectations);
    testStatisticsMeasuredForTheFrameArePrepared(expectations);
    testUnavailableStatisticsTakeTheCpuPath(expectations);
    testCancelledStatisticsCacheNothing(expectations);

    if (!expectations.ok() || bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << "GPU-scene-stage expectation(s) failed\n";
        return 1;
    }
    std::cout << "GPU-scene-stage: all expectations passed\n";
    return 0;
}

int main(int argc, char** argv) {
    try {
        return runTests(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
