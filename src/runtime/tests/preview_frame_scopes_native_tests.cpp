// #730, #733 Scopes: the Working-signal scope tables of a preview frame, counted by the frame's
// own job on the preview display service's device.
//
// The real GpuPreviewDisplayService runs with the GPU-prepared packed product, presentation
// Disabled and a declared card (no window system). A request whose identity asks for scope tables
// has its frame's job count them on the scene picture it computed (the XYZ color step and the
// count kernel, one table readback) before the frame's final transfer, and the frame is published
// carrying them.
//
// What is asserted, at the full table size and at a reduced one (the size a pane draws):
//   (1) the frame is a GPU-prepared packed frame carrying Measured tables of the device route,
//       counted under the request's settings at the asked size;
//   (2) parity: the tables equal the CPU reference's cell for cell; with the CIE table every count
//       is within one cell along every axis (OCIO's CPU and GPU processors may round a pixel into
//       the neighbouring cell) and every pixel is counted;
//   (3) one table readback per frame moving the layout's bytes, and one frame measured;
//   (4) a region equals the CPU reference's region; a region outside the picture settles EmptyArea
//       with no readback;
//   (5) a request that asks for no tables gets a frame without them, and nothing is counted;
//   (6) #733: a request for the Display signal gets a frame with a Failed record and nothing is
//       counted: that signal is counted from the shown frame, at display time.
//
// Requires the Vulkan backend and the pinned shader tools; skips (77) without a device and fails
// under --require-device.

#include "gpu_preview_display_service_packed_support.hpp"
#include "scope_tables_compare_support.hpp"

#include "await.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/preview_frame_scopes.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <memory>
#include <optional>

namespace {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

using bloom::render::kScopeTableAll;
using bloom::render::kScopeTableHistogram;
using bloom::render::kScopeTableVectorscope;
using bloom::render::kScopeTableWaveform;
using bloom::runtime::PreviewFrameScopesStatus;
using bloom::runtime::PreviewScopeRequest;
using bloom::runtime::ScopeFrameStatus;
using bloom::runtime::ScopeMeasureSettings;
using bloom::runtime::ScopeSignal;
using bloom::runtime::ScopeSignalColor;
using bloom::runtime::TaskPriority;
using bloom::runtime::scope_test::tablesWithinOneCell;
using bloom::test_support::awaitCondition;

constexpr std::uint32_t kSize = 64;
constexpr std::uint32_t kNoChromaticity =
    kScopeTableHistogram | kScopeTableWaveform | kScopeTableVectorscope;

struct Rig final {
    PackedRetirementScope retirement;
    TaskScheduler scheduler;
    std::unique_ptr<GpuPreviewDisplayService> service;
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan;
    bloom::document::Snapshot snapshot;
    std::uint64_t nextOwner = 100;

    Rig(const TestOptions& options,
        std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> planValue,
        const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
        bloom::document::Snapshot snapshotValue)
        : scheduler(schedulerConfig()), plan(std::move(planValue)),
          snapshot(std::move(snapshotValue)) {
        auto configured = serviceOptions(options.loaderPath);
        configured.product = bloom::runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
        configured.presentation = GpuPreviewDisplayServicePresentationMode::Disabled;
        auto builder = std::make_shared<CpuGpuSceneBuilder>(
            nullptr, bloom::runtime::GpuSceneMediaContext{}, bloom::runtime::GpuSceneOcioContext{});
        service = std::make_unique<GpuPreviewDisplayService>(
            scheduler, packedStageFunction(plan, builder, {}, processor),
            generalCpuStageFunction(plan, processor, std::make_shared<CpuCompositionEvaluator>()),
            generalFallback(processor), configured);
    }

    ~Rig() {
        if (service) {
            service->beginShutdown();
            service.reset();
        }
    }

    [[nodiscard]] bool ready() const {
        return awaitCondition("the service reaches a terminal startup state",
                              [&] {
                                  return service->status().state !=
                                         GpuPreviewDisplayServiceState::Initializing;
                              }) &&
               service->status().state == GpuPreviewDisplayServiceState::Ready;
    }

    // A frame of `generation` asking for `scopes` (null: none); null when it did not complete on
    // the GPU route.
    [[nodiscard]] std::shared_ptr<const bloom::runtime::PreparedPreviewFrame>
    frame(const std::uint64_t generation, std::shared_ptr<const PreviewScopeRequest> scopes) {
        auto identity = makeIdentity(*plan, generation, {}, {}, {});
        identity.scopes = std::move(scopes);
        const bloom::runtime::TaskOwner owner{
            .kind = bloom::runtime::TaskOwnerKind::Composition,
            .id = bloom::runtime::TaskOwnerId::fromRaw(nextOwner++)};
        auto submission = service->submit(TaskRequest("scope frame", owner, TaskPriority::Visible),
                                          snapshot, identity, kBudget / 4, {});
        if (!submission.accepted()) {
            std::cerr << "frame " << generation << ": the request was not admitted\n";
            return {};
        }
        std::optional<TaskResult<PreviewPreparationResultHandle>> result;
        if (!awaitCondition("frame " + std::to_string(generation), [&] {
                result = submission.handle.tryTakeResult();
                return result.has_value();
            }))
            return {};
        auto frame = packedResult(result);
        if (!frame)
            for (const auto& diagnostic : result->diagnostics())
                std::cerr << diagnostic.code << ": " << diagnostic.summary << '\n';
        return frame;
    }
};

[[nodiscard]] bool isPacked(const bloom::runtime::PreparedPreviewFrame& frame) {
    return frame.provenance().provider ==
               bloom::runtime::PreviewDisplayProvider::GpuPreparedPacked &&
           frame.processIdentity().provider == bloom::runtime::EvaluationProvider::GpuResident;
}

struct Reference final {
    std::shared_ptr<const ScopeSignalColor> working;
    std::shared_ptr<const ScopeSignalColor> display;
    std::shared_ptr<const bloom::runtime::GpuSceneOcioContext> ocio;
    std::shared_ptr<const bloom::runtime::ProcessFrame> cpuFrame;

    [[nodiscard]] bloom::runtime::ScopeFrameMeasure
    measure(const ScopeMeasureSettings& settings,
            const bloom::render::ScopeTableReduction reduction = {}) const {
        return bloom::runtime::measureScopeFrameCpu(
            *cpuFrame, settings, settings.signal == ScopeSignal::Working ? *working : *display, {},
            reduction);
    }
    [[nodiscard]] std::shared_ptr<const PreviewScopeRequest>
    request(const ScopeMeasureSettings& settings,
            const bloom::render::ScopeTableReduction reduction = {}) const {
        return std::make_shared<const PreviewScopeRequest>(
            PreviewScopeRequest{.settings = settings,
                                .reduction = reduction,
                                .colors = std::make_shared<bloom::runtime::ScopeSignalColorCache>(),
                                .ocio = [context = ocio] { return context; }});
    }
};

[[nodiscard]] ScopeMeasureSettings settingsOf(const ScopeSignal signal,
                                              const std::uint32_t scopes) {
    ScopeMeasureSettings settings;
    settings.signal = signal;
    settings.scopes = scopes;
    return settings;
}

bool run(Expectations& checks, const TestOptions& options,
         const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
         const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
         const bloom::document::Snapshot& snapshot, const Reference& reference) {
    Rig rig(options, plan, processor, snapshot);
    if (!rig.ready()) {
        std::cout << "Unsupported: genuine offscreen GPU preparation unavailable: "
                  << rig.service->status().residentDetail << '\n';
        checks.expect(!options.requireDevice, "required genuine offscreen GPU device available");
        return false;
    }
    const auto counters = [&] { return rig.service->status().counters; };
    std::uint64_t generation = 1;

    // (1) - (3), at the full size and at a reduced one.
    for (const auto reduction :
         {bloom::render::ScopeTableReduction{}, bloom::render::ScopeTableReduction{1, 2, 2, 2}}) {
        const auto name =
            std::string(reduction == bloom::render::ScopeTableReduction{} ? "full" : "reduced");
        for (const auto scopes : {kNoChromaticity, kScopeTableAll}) {
            const auto settings = settingsOf(ScopeSignal::Working, scopes);
            const auto before = counters();
            const auto frame = rig.frame(generation++, reference.request(settings, reduction));
            checks.expect(frame != nullptr && isPacked(*frame),
                          name + ": the frame is a GPU-prepared packed frame");
            if (frame == nullptr)
                continue;
            const auto& carried = frame->scopes();
            // The frame keeps the asked reduction; the counted one is resolved for its width.
            const auto counted = bloom::render::scopeReductionForWidth(reduction, kSize);
            checks.expect(carried != nullptr &&
                              carried->status == PreviewFrameScopesStatus::Measured &&
                              carried->settings == settings && carried->reduction == reduction &&
                              carried->tables.layout().reduction == counted &&
                              carried->tables.route() == bloom::runtime::ScopeRoute::Gpu,
                          name + ": the frame carries tables its device job counted" +
                              (carried != nullptr ? ": " + carried->message : std::string()));
            if (carried == nullptr || carried->status != PreviewFrameScopesStatus::Measured)
                continue;
            const auto counts = carried->tables.unpack();
            const auto expected = reference.measure(settings, reduction);
            checks.expect(expected.status == ScopeFrameStatus::Measured,
                          name + ": the CPU reference measures");
            if (scopes == kNoChromaticity)
                checks.expect(counts == expected.counts && carried->area == expected.area,
                              name + ": the tables equal the CPU reference's cell for cell");
            else
                checks.expect(tablesWithinOneCell(counts, expected.counts),
                              name + ": every count is within one cell of the CPU reference's");
            checks.expect(counts.counted() == kSize * kSize, name + ": every pixel is counted");
            const auto after = counters();
            const auto bytes =
                bloom::render::scopeTableLayout(scopes, kSize, counted).byteCount();
            std::cout << name << (scopes == kNoChromaticity ? ", no CIE table" : ", all tables")
                      << ": table readback " << bytes << " bytes\n";
            checks.expect(after.scopeFramesMeasured == before.scopeFramesMeasured + 1 &&
                              after.scopeTableReadbacks == before.scopeTableReadbacks + 1 &&
                              after.scopeTableReadbackBytes == bytes &&
                              carried->tableReadbackBytes == bytes,
                          name + ": one table readback of the layout's bytes for the frame");
        }
    }

    // (4) a region, inside and outside the picture.
    {
        auto settings = settingsOf(ScopeSignal::Working, kNoChromaticity);
        settings.region = bloom::runtime::ScopeRegion{.x = 10, .y = 6, .width = 30, .height = 20};
        const auto frame = rig.frame(generation++, reference.request(settings));
        const auto expected = reference.measure(settings);
        checks.expect(frame != nullptr && frame->scopes() != nullptr &&
                          frame->scopes()->status == PreviewFrameScopesStatus::Measured &&
                          frame->scopes()->tables.unpack() == expected.counts &&
                          frame->scopes()->area == expected.area &&
                          expected.counts.counted() == 30U * 20U,
                      "a region: the frame's tables equal the CPU reference's");
        settings.region = bloom::runtime::ScopeRegion{.x = 200, .y = 6, .width = 20, .height = 20};
        const auto before = counters().scopeTableReadbacks;
        const auto outside = rig.frame(generation++, reference.request(settings));
        checks.expect(outside != nullptr && outside->scopes() != nullptr &&
                          outside->scopes()->status == PreviewFrameScopesStatus::EmptyArea &&
                          counters().scopeTableReadbacks == before,
                      "a region outside the picture settles EmptyArea with no readback");
    }

    // (5) no tables asked for.
    {
        const auto before = counters();
        const auto frame = rig.frame(generation++, nullptr);
        checks.expect(frame != nullptr && isPacked(*frame) && frame->scopes() == nullptr,
                      "a request without a scope request gets a frame without tables");
        const auto after = counters();
        checks.expect(after.scopeFramesMeasured == before.scopeFramesMeasured &&
                          after.scopeTableReadbacks == before.scopeTableReadbacks &&
                          after.scopeColorDispatches == before.scopeColorDispatches,
                      "and nothing is counted");
    }

    // (6) the Display signal is not counted with a frame.
    {
        const auto before = counters();
        const auto settings = settingsOf(ScopeSignal::Display, kScopeTableAll);
        const auto frame = rig.frame(generation++, reference.request(settings));
        const auto carried = frame != nullptr ? frame->scopes() : nullptr;
        checks.expect(frame != nullptr && isPacked(*frame) && carried != nullptr &&
                          carried->status == PreviewFrameScopesStatus::Failed &&
                          carried->settings == settings && !carried->message.empty() &&
                          carried->tables.empty(),
                      "a request for the Display signal gets a frame with a Failed record");
        const auto after = counters();
        checks.expect(after.scopeFramesMeasured == before.scopeFramesMeasured &&
                          after.scopeTableReadbacks == before.scopeTableReadbacks &&
                          after.scopeColorDispatches == before.scopeColorDispatches,
                      "and the frame's job counts nothing for it");
    }
    return true;
}

int runAll(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options.valid)
        return 2;
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(resolution).takeResolved();
    if (!neutral)
        return 1;
    auto processor = makeNeutralProcessor(*neutral);
    if (!processor)
        return 1;

    GpuOcioContextRequest ocioRequest;
    ocioRequest.applicationExecutable =
        std::filesystem::path{BLOOM_PREVIEW_FRAME_SCOPES_TEST_EXECUTABLE};
    ocioRequest.toolPackage.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    ocioRequest.toolPackage.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    ocioRequest.toolPackage.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    ocioRequest.toolPackage.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
    ocioRequest.toolPackage.relocated = static_cast<bool>(BLOOM_GPU_TOOLS_RELOCATED);
#ifdef BLOOM_GPU_TOOLS_BUNDLE_RELATIVE
    ocioRequest.toolPackage.bundleRelative = true;
#endif
#ifdef BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256
    ocioRequest.toolPackage.glslangStagedDigest = bloom::core::Sha256Digest::fromLowercaseHex(
        std::string_view{BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256}.substr(7));
#endif
#ifdef BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256
    ocioRequest.toolPackage.spirvValStagedDigest = bloom::core::Sha256Digest::fromLowercaseHex(
        std::string_view{BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256}.substr(7));
#endif
    GpuOcioContextResolver resolver(std::move(ocioRequest));
    const auto resolved = resolver.resolve();
    if (!resolved.hasValue()) {
        std::cerr << "the packaged tools do not resolve to a shared OCIO context: "
                  << resolved.diagnostic << '\n';
        return 1;
    }

    const auto plan = makePlan(kSize, kSize);
    bloom::document::Project project(kProjectId, "preview-frame-scopes");
    const auto seed = bloom::document::makeNewProject("Fixture", "Main",
                                                      RationalTime::fromInteger(4), plan->format());
    if (!project.addComposition(bloom::document::Composition(
            plan->compositionId(), "Main", RationalTime::fromInteger(4),
            seed.project.findComposition(seed.initialCompositionId)->graph(), plan->format())))
        return 1;
    bloom::document::Document document(std::move(project));

    auto config =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kBloomNeutralV1ConfigUri,
                                         bloom::color::ocioBuiltInContentRevision(
                                             bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                             bloom::color::kBloomNeutralV1ConfigUri)
                                             .value_or(bloom::color::kBloomNeutralV1ConfigDigest),
                                         "lin_rec709_scene")
            .takeResolved();
    if (!config)
        return 1;
    const auto sharedConfig =
        std::make_shared<const bloom::color::ResolvedBloomNeutralConfig>(std::move(*config));
    Reference reference;
    for (const auto signal : {ScopeSignal::Working, ScopeSignal::Display}) {
        auto prepared = ScopeSignalColor::prepare(
            {.config = sharedConfig, .signal = signal, .display = {}, .view = {}});
        if (!prepared) {
            std::cerr << "the scope signal's color objects did not prepare: " << prepared.message
                      << '\n';
            return 1;
        }
        (signal == ScopeSignal::Working ? reference.working : reference.display) = prepared.color;
    }
    reference.ocio = resolved.context;
    {
        CpuCompositionEvaluator evaluator;
        const auto identity = makeIdentity(*plan, 1, {}, {}, {});
        auto evaluated = evaluator.evaluate(plan,
                                            {.time = identity.time,
                                             .output = plan->output(),
                                             .resolution = identity.resolution,
                                             .quality = identity.quality,
                                             .colorIntent = identity.colorIntent,
                                             .pixelStorageByteLimit = kBudget,
                                             .roi = identity.roi,
                                             .bypassLookNodes = !identity.showLook},
                                            {});
        if (evaluated.frame() == nullptr) {
            std::cerr << "the CPU reference did not evaluate the plan\n";
            return 1;
        }
        reference.cpuFrame = evaluated.frame();
    }

    Expectations checks;
    if (!run(checks, options, plan, processor, document.snapshot(), reference))
        return options.requireDevice ? 1 : kSkipExit;
    if (checks.failures() + bloom::test_support::awaitFailureCount() != 0)
        return 1;
    std::cout << "PASS: preview frames carry the scope tables their device job counted\n";
    return 0;
}
#else
int runAll(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    std::cout << "Unsupported: genuine GPU preparation tools unavailable\n";
    return options.requireDevice ? 1 : kSkipExit;
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        return runAll(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
