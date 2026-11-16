// #733 Scopes: the Display signal tables of a shown frame, counted at display time on the preview
// display service's device.
//
// The real GpuPreviewDisplayService runs with the GPU-prepared packed product, presentation
// Disabled, a declared card (no window system) and a real processing phase policy, as the packed
// service test does. A ShownFrameScopeMeasurer posts its measures to that service's device.
//
// What is asserted:
//   (1) a stored display frame of each layout (generated cells) is counted on the device: the
//       tables without the Chromaticity table equal the CPU reference's cell for cell, at the full
//       size and at a reduced size; with the Chromaticity table every count is within one cell
//       (OCIO's CPU and GPU processors may round a pixel into the neighbouring cell);
//   (2) the native work of a measure: one upload, one table readback moving exactly the layout's
//       bytes for the asked size (printed), the XYZ color step only with the Chromaticity table,
//       no scene job, no frame readback; its processing phase is released afterwards;
//   (3) a frame the service prepared (no tables asked) is counted on the device from its own
//       stored cells and equals the CPU reference of those cells; no scene runs again;
//   (4) frames come first: with the measure's upload held, a frame submitted meanwhile is
//       prepared and published while the measure is still pending, and the measure then
//       completes on the device with the right tables;
//   (5) a region equals the CPU reference's region; a region outside the picture settles
//       EmptyArea with no native work;
//   (6) a service that stops while a measure is in flight cancels it, retires what it submitted
//       and returns; the frame is then counted by the CPU reference on a worker, which says why.
//
// Requires the Vulkan backend and the pinned shader tools; skips (77) without a device and fails
// under --require-device.

#include "gpu_preview_display_service_packed_support.hpp"
#include "scope_tables_compare_support.hpp"

#include "await.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/shown_frame_scopes.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <span>

namespace {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

using bloom::render::DisplayPixelLayout;
using bloom::render::kScopeTableAll;
using bloom::render::kScopeTableHistogram;
using bloom::render::kScopeTableVectorscope;
using bloom::render::kScopeTableWaveform;
using bloom::render::ScopeTableReduction;
using bloom::runtime::PreviewFrameScopesStatus;
using bloom::runtime::ScopeFrameStatus;
using bloom::runtime::ScopeMeasureSettings;
using bloom::runtime::ScopeRoute;
using bloom::runtime::ScopeSignal;
using bloom::runtime::ScopeSignalColor;
using bloom::runtime::ScopeStoredFrame;
using bloom::runtime::ShownFrameScopeMeasurer;
using bloom::runtime::ShownFrameScopeRequest;
using bloom::runtime::ShownFrameScopes;
using bloom::runtime::TaskPriority;
using bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess;
using bloom::runtime::scope_test::tablesWithinOneCell;
using bloom::test_support::awaitCondition;

constexpr std::uint32_t kSize = 64;
constexpr std::uint32_t kStoredWidth = 96;
constexpr std::uint32_t kStoredHeight = 54;
constexpr std::uint32_t kNoChromaticity =
    kScopeTableHistogram | kScopeTableWaveform | kScopeTableVectorscope;

using Cells = std::shared_ptr<const std::vector<bloom::render::Rgba8>>;

[[nodiscard]] Cells makeCells(const DisplayPixelLayout layout, const std::uint32_t seed) {
    std::vector<bloom::render::Rgba8> cells;
    cells.reserve(std::size_t{kStoredWidth} * kStoredHeight);
    for (std::uint32_t y = 0; y < kStoredHeight; ++y) {
        for (std::uint32_t x = 0; x < kStoredWidth; ++x) {
            const std::uint32_t i = y * kStoredWidth + x + seed * 97U;
            if (layout == DisplayPixelLayout::Rgb10A2) {
                const bool transparent = i % 13U == 5U;
                cells.push_back(transparent ? bloom::render::displayCell(0U)
                                            : bloom::render::rgb10A2Cell(
                                                  {(i * 3U) % 1024U, (i * 7U + 5U) % 1024U,
                                                   1023U - (i * 5U) % 1024U,
                                                   bloom::render::kDisplayAlpha2Opaque}));
            } else {
                const auto alpha = static_cast<std::uint8_t>(
                    i % 5U == 0U ? 255U : (i % 5U == 1U ? 0U : (i * 3U) % 256U));
                cells.push_back(
                    bloom::render::Rgba8{static_cast<std::uint8_t>(i % 256U),
                                         static_cast<std::uint8_t>((i * 5U + 1U) % 256U),
                                         static_cast<std::uint8_t>(255U - i % 256U), alpha});
            }
        }
    }
    return std::make_shared<const std::vector<bloom::render::Rgba8>>(std::move(cells));
}

[[nodiscard]] ScopeStoredFrame storedOf(const Cells& cells, const DisplayPixelLayout layout) {
    const auto window = bloom::render::ImageWindow::create(0, 0, kStoredWidth, kStoredHeight);
    if (!window)
        throw std::logic_error("fixture window");
    return ScopeStoredFrame{.cells = std::span<const bloom::render::Rgba8>(*cells),
                            .layout = layout,
                            .dataWindow = *window.value(),
                            .displayWindow = *window.value(),
                            .pixelAspect = bloom::core::PixelAspectRatio::square(),
                            .roi = std::nullopt,
                            .compositionWidth = kStoredWidth,
                            .compositionHeight = kStoredHeight};
}

// The stored cells of a frame the service prepared, as the measure reads them.
[[nodiscard]] std::optional<ScopeStoredFrame>
storedOf(const bloom::runtime::PreparedPreviewFrame& frame) {
    const auto view = frame.displayBufferView();
    if (!view)
        return std::nullopt;
    return ScopeStoredFrame{.cells = view->pixels,
                            .layout = view->pixelLayout,
                            .dataWindow = view->dataWindow.value_or(view->displayWindow),
                            .displayWindow = view->displayWindow,
                            .pixelAspect = view->pixelAspect,
                            .roi = frame.desiredIdentity().roi,
                            .compositionWidth = kSize,
                            .compositionHeight = kSize};
}

[[nodiscard]] ScopeMeasureSettings displaySettings(const std::uint32_t scopes) {
    ScopeMeasureSettings settings;
    settings.signal = ScopeSignal::Display;
    settings.scopes = scopes;
    return settings;
}

struct Rig final {
    PackedRetirementScope retirement;
    TaskScheduler scheduler;
    std::shared_ptr<bloom::runtime::GpuProcessingPhasePolicy> policy;
    std::unique_ptr<GpuPreviewDisplayService> service;
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan;
    bloom::document::Snapshot snapshot;
    std::shared_ptr<const ScopeSignalColor> color;
    std::shared_ptr<std::atomic<int>> ready = std::make_shared<std::atomic<int>>(0);
    std::unique_ptr<ShownFrameScopeMeasurer> measurer;
    std::uint64_t nextOwner = 100;
    std::uint64_t nextTag = 1;

    Rig(const TestOptions& options,
        std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> planValue,
        const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
        bloom::document::Snapshot snapshotValue, std::shared_ptr<const ScopeSignalColor> colorValue,
        std::shared_ptr<const bloom::runtime::GpuSceneOcioContext> ocio)
        : scheduler(schedulerConfig()),
          policy(std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(32, retirement.owner)),
          plan(std::move(planValue)), snapshot(std::move(snapshotValue)),
          color(std::move(colorValue)) {
        auto sample = bloom::test_support::fixedMemoryBudgetSample();
        bloom::runtime::MemoryBudgetLedger ledger(sample.physicalBytes,
                                                  sample.machine.availableBytes.value_or(0));
        ledger.beginProcessSampling();
        const auto deliveredAt = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
        const auto sampled = ledger.applySample(std::move(sample), deliveredAt);
        policy->applyHostSample(sampled, ledger.physicalByteBudget(), deliveredAt);

        auto configured = serviceOptions(options.loaderPath);
        configured.product = bloom::runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
        configured.presentation = GpuPreviewDisplayServicePresentationMode::Disabled;
        configured.processingPhasePolicy = policy;
        const bloom::runtime::GpuScenePreparationPhase phase{
            .policy = policy,
            .envelope = {.metadataBytes = 1024ULL * 1024,
                         .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                         .requestedExecutorBytes = kBudget,
                         .deviceWorkingBytes = 832ULL * 1024 * 1024}};
        auto builder = std::make_shared<CpuGpuSceneBuilder>(
            nullptr, bloom::runtime::GpuSceneMediaContext{}, bloom::runtime::GpuSceneOcioContext{},
            phase);
        service = std::make_unique<GpuPreviewDisplayService>(
            scheduler, packedStageFunction(plan, builder, {}, processor),
            generalCpuStageFunction(plan, processor, std::make_shared<CpuCompositionEvaluator>()),
            generalFallback(processor), configured);

        ShownFrameScopeMeasurer::Options measurerOptions;
        measurerOptions.scheduler = &scheduler;
        measurerOptions.owner = {.kind = bloom::runtime::TaskOwnerKind::PanelRequest,
                                 .id = bloom::runtime::TaskOwnerId::fromRaw(1)};
        measurerOptions.colors = std::make_shared<bloom::runtime::ScopeSignalColorCache>();
        measurerOptions.ocio = [context = std::move(ocio)] { return context; };
        measurerOptions.device = service->shownFrameScopePort();
        measurerOptions.resultReady = [count = ready] { count->fetch_add(1); };
        measurer = std::make_unique<ShownFrameScopeMeasurer>(std::move(measurerOptions));
    }

    ~Rig() {
        measurer.reset();
        if (service) {
            service->beginShutdown();
            service.reset();
        }
    }

    [[nodiscard]] bool serviceReady() const {
        return awaitCondition("the service reaches a terminal startup state",
                              [&] {
                                  return service->status().state !=
                                         GpuPreviewDisplayServiceState::Initializing;
                              }) &&
               service->status().state == GpuPreviewDisplayServiceState::Ready;
    }

    [[nodiscard]] bloom::runtime::GpuPreviewDisplayServiceCounters counters() const {
        return service->status().counters;
    }

    [[nodiscard]] auto submitFrame(const std::uint64_t generation) {
        const auto identity = makeIdentity(*plan, generation, {}, {}, {});
        const bloom::runtime::TaskOwner owner{
            .kind = bloom::runtime::TaskOwnerKind::Composition,
            .id = bloom::runtime::TaskOwnerId::fromRaw(nextOwner++)};
        return service->submit(TaskRequest("shown frame", owner, TaskPriority::Visible), snapshot,
                               identity, kBudget / 4, {});
    }

    // A frame of `generation` that asks for no tables; null when it did not complete on the GPU
    // route.
    [[nodiscard]] std::shared_ptr<const bloom::runtime::PreparedPreviewFrame>
    frame(const std::uint64_t generation) {
        auto submission = submitFrame(generation);
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
        auto prepared = packedResult(result);
        if (!prepared)
            for (const auto& diagnostic : result->diagnostics())
                std::cerr << diagnostic.code << ": " << diagnostic.summary << '\n';
        return prepared;
    }

    [[nodiscard]] ShownFrameScopeRequest stored(const Cells& cells, const DisplayPixelLayout layout,
                                                const ScopeMeasureSettings& settings,
                                                const ScopeTableReduction reduction) {
        ShownFrameScopeRequest request;
        request.stored = storedOf(cells, layout);
        request.storedOwner = cells;
        request.storedColor = color;
        request.settings = settings;
        request.reduction = reduction;
        request.tag = nextTag++;
        return request;
    }

    // Shows `request` and returns its result; nothing when it did not arrive.
    [[nodiscard]] std::optional<ShownFrameScopes> measure(ShownFrameScopeRequest request,
                                                          const std::string& what) {
        const auto tag = request.tag;
        measurer->show(std::move(request));
        if (!awaitCondition(what, [this] { return !measurer->busy(); }))
            return std::nullopt;
        auto result = measurer->take();
        if (!result || result->tag != tag || result->scopes == nullptr)
            return std::nullopt;
        return result;
    }
};

[[nodiscard]] bool measuredOnDevice(const ShownFrameScopes& result) {
    return result.scopes->status == PreviewFrameScopesStatus::Measured &&
           result.scopes->tables.route() == ScopeRoute::Gpu;
}

void testStoredFrames(Expectations& checks, Rig& rig) {
    for (const auto layout : {DisplayPixelLayout::Straight8888, DisplayPixelLayout::Rgb10A2}) {
        const std::string layoutName =
            layout == DisplayPixelLayout::Rgb10A2 ? "10:10:10:2" : "8:8:8:8";
        const auto cells = makeCells(layout, 1);
        for (const auto reduction : {ScopeTableReduction{}, ScopeTableReduction{1, 2, 2, 2}}) {
            for (const auto scopes : {kNoChromaticity, kScopeTableAll}) {
                const std::string name =
                    layoutName + (reduction == ScopeTableReduction{} ? ", full" : ", reduced") +
                    (scopes == kScopeTableAll ? ", all tables" : ", no CIE table");
                auto settings = displaySettings(scopes);
                settings.ignoreTransparent = reduction != ScopeTableReduction{};
                const auto expected = bloom::runtime::measureScopeStoredFrameCpu(
                    storedOf(cells, layout), settings, reduction, *rig.color);
                checks.expect(expected.status == ScopeFrameStatus::Measured,
                              name + ": the CPU reference measures");
                const auto before = rig.counters();
                const auto result =
                    rig.measure(rig.stored(cells, layout, settings, reduction), name);
                checks.expect(result.has_value(), name + ": a result");
                if (!result)
                    continue;
                checks.expect(measuredOnDevice(*result),
                              name + ": counted on the device: " + result->scopes->message + " " +
                                  result->deviceMessage);
                if (!measuredOnDevice(*result))
                    continue;
                const auto counts = result->scopes->tables.unpack();
                if (scopes == kNoChromaticity)
                    checks.expect(counts == expected.counts &&
                                      result->scopes->area == expected.area,
                                  name + ": the tables equal the CPU reference's cell for cell");
                else
                    checks.expect(tablesWithinOneCell(counts, expected.counts) &&
                                      counts.counted() == expected.counts.counted(),
                                  name + ": every count is within one cell of the CPU "
                                         "reference's");
                // The asked reduction names Waveform columns against the widest table; the
                // counted one is resolved for the measured width.
                const auto counted =
                    bloom::render::scopeReductionForWidth(reduction, kStoredWidth);
                const auto bytes =
                    bloom::render::scopeTableLayout(scopes, kStoredWidth, counted).byteCount();
                const auto after = rig.counters();
                std::cout << name << ": table readback " << result->scopes->tableReadbackBytes
                          << " bytes\n";
                checks.expect(result->uploads == 1 && result->tableReadbacks == 1 &&
                                  result->scopes->tableReadbackBytes == bytes &&
                                  counts.layout.reduction == counted &&
                                  result->reduction == reduction,
                              name + ": one upload and one table readback of the layout's bytes");
                checks.expect(result->colorDispatches == (scopes == kScopeTableAll ? 1U : 0U),
                              name + ": the XYZ color step runs only with the CIE table");
                checks.expect(
                    after.shownScopeMeasures == before.shownScopeMeasures + 1 &&
                        after.shownScopeUploads == before.shownScopeUploads + 1 &&
                        after.shownScopeTableReadbacks == before.shownScopeTableReadbacks + 1 &&
                        after.shownScopeTableReadbackBytes == bytes &&
                        after.residentGraphJobs == before.residentGraphJobs &&
                        after.nativeDispatches == before.nativeDispatches &&
                        after.packedTransfers == before.packedTransfers &&
                        after.fullFrameReadbacks == before.fullFrameReadbacks &&
                        after.scopeFramesMeasured == before.scopeFramesMeasured,
                    name + ": no scene job, no frame transfer and no frame readback");
            }
        }
    }
    checks.expect(awaitCondition("the measures' phases are released",
                                 [&] {
                                     return rig.policy->snapshot().reservations.admittedPhases ==
                                            0;
                                 }),
                  "a measure's processing phase is released when it ends");
}

void testPreparedFrame(Expectations& checks, Rig& rig, std::uint64_t& generation) {
    const auto frame = rig.frame(generation++);
    checks.expect(frame != nullptr && frame->scopes() == nullptr,
                  "a prepared frame that asked for no tables carries none");
    if (frame == nullptr)
        return;
    const auto stored = storedOf(*frame);
    checks.expect(stored.has_value(), "the prepared frame has stored cells");
    if (!stored)
        return;
    for (const auto scopes : {kNoChromaticity, kScopeTableAll}) {
        const std::string name =
            scopes == kScopeTableAll ? "prepared frame, all tables" : "prepared frame, no CIE table";
        const auto settings = displaySettings(scopes);
        const auto expected =
            bloom::runtime::measureScopeStoredFrameCpu(*stored, settings, {}, *rig.color);
        ShownFrameScopeRequest request;
        request.frame = frame;
        request.compositionWidth = kSize;
        request.compositionHeight = kSize;
        request.settings = settings;
        request.tag = rig.nextTag++;
        const auto before = rig.counters();
        const auto result = rig.measure(std::move(request), name);
        checks.expect(result.has_value() && result->frame == frame, name + ": a result");
        if (!result)
            continue;
        checks.expect(measuredOnDevice(*result), name + ": counted on the device: " +
                                                     result->scopes->message + " " +
                                                     result->deviceMessage);
        if (!measuredOnDevice(*result))
            continue;
        const auto counts = result->scopes->tables.unpack();
        if (scopes == kNoChromaticity)
            checks.expect(expected.status == ScopeFrameStatus::Measured &&
                              counts == expected.counts && result->scopes->area == expected.area,
                          name + ": the tables equal the CPU reference of the frame's cells");
        else
            checks.expect(expected.status == ScopeFrameStatus::Measured &&
                              tablesWithinOneCell(counts, expected.counts),
                          name + ": every count is within one cell of the CPU reference's");
        checks.expect(counts.counted() == kSize * kSize, name + ": every pixel is counted");
        const auto after = rig.counters();
        checks.expect(after.residentGraphJobs == before.residentGraphJobs &&
                          after.nativeDispatches == before.nativeDispatches &&
                          after.fullFrameReadbacks == before.fullFrameReadbacks &&
                          after.shownScopeMeasures == before.shownScopeMeasures + 1,
                      name + ": no scene runs again and no frame is read back");
    }
}

void testFramesComeFirst(Expectations& checks, Rig& rig, std::uint64_t& generation) {
    const auto core = GpuPreviewDisplayServiceTestAccess::coreOf(*rig.service);
    const auto cells = makeCells(DisplayPixelLayout::Rgb10A2, 5);
    const auto settings = displaySettings(kNoChromaticity);
    const auto expected = bloom::runtime::measureScopeStoredFrameCpu(
        storedOf(cells, DisplayPixelLayout::Rgb10A2), settings, {}, *rig.color);
    const auto held = std::make_shared<std::atomic_bool>(true);
    GpuPreviewDisplayServiceTestAccess::setShownScopeUploadPollOverride(
        *core, [held](bloom::render::GpuImageUpload& upload) {
            return held->load() ? bloom::render::GpuImageUploadPollResult::Pending : upload.poll();
        });
    const auto before = rig.counters();
    auto request = rig.stored(cells, DisplayPixelLayout::Rgb10A2, settings, {});
    const auto tag = request.tag;
    rig.measurer->show(std::move(request));
    if (!awaitCondition("the held measure is taken up by the device", [&] {
            return rig.counters().shownScopeUploads == before.shownScopeUploads + 1;
        })) {
        held->store(false);
        return;
    }
    // A frame submitted while the measure is pending is prepared and published at once.
    const auto frame = rig.frame(generation++);
    const auto during = rig.counters();
    checks.expect(frame != nullptr, "a frame submitted during a measure is prepared");
    checks.expect(rig.measurer->busy() && during.shownScopeMeasures == before.shownScopeMeasures,
                  "the measure is still pending when the frame is published");
    checks.expect(during.shownScopeFramesStartedDuringMeasure ==
                          before.shownScopeFramesStartedDuringMeasure + 1 &&
                      during.residentGraphJobs == before.residentGraphJobs + 1,
                  "the frame's job took the device while the measure was in flight");
    held->store(false);
    const bool ended =
        awaitCondition("the released measure ends", [&] { return !rig.measurer->busy(); });
    GpuPreviewDisplayServiceTestAccess::setShownScopeUploadPollOverride(*core, {});
    if (!ended)
        return;
    const auto result = rig.measurer->take();
    checks.expect(result.has_value() && result->tag == tag && result->scopes != nullptr &&
                      measuredOnDevice(*result) &&
                      result->scopes->tables.unpack() == expected.counts,
                  "the measure then completes on the device with the right tables");
    checks.expect(awaitCondition("the phases are released",
                                 [&] {
                                     return rig.policy->snapshot().reservations.admittedPhases ==
                                            0;
                                 }),
                  "the frame's and the measure's processing phases are released");
}

void testRegions(Expectations& checks, Rig& rig) {
    const auto cells = makeCells(DisplayPixelLayout::Rgb10A2, 9);
    auto settings = displaySettings(kNoChromaticity);
    settings.region = bloom::runtime::ScopeRegion{.x = 10, .y = 6, .width = 30, .height = 20};
    const auto expected = bloom::runtime::measureScopeStoredFrameCpu(
        storedOf(cells, DisplayPixelLayout::Rgb10A2), settings, {}, *rig.color);
    auto result =
        rig.measure(rig.stored(cells, DisplayPixelLayout::Rgb10A2, settings, {}), "a region");
    checks.expect(result.has_value() && measuredOnDevice(*result) &&
                      result->scopes->tables.unpack() == expected.counts &&
                      result->scopes->area == expected.area &&
                      expected.counts.counted() == 30U * 20U,
                  "a region: the device's tables equal the CPU reference's");
    settings.region = bloom::runtime::ScopeRegion{.x = 400, .y = 6, .width = 20, .height = 20};
    const auto before = rig.counters();
    result = rig.measure(rig.stored(cells, DisplayPixelLayout::Rgb10A2, settings, {}),
                         "a region outside the picture");
    const auto after = rig.counters();
    checks.expect(result.has_value() &&
                      result->scopes->status == PreviewFrameScopesStatus::EmptyArea &&
                      after.shownScopeUploads == before.shownScopeUploads &&
                      after.shownScopeTableReadbacks == before.shownScopeTableReadbacks,
                  "a region outside the picture settles EmptyArea with no native work");
}

void testShutdownDuringMeasure(Expectations& checks, Rig& rig) {
    const auto core = GpuPreviewDisplayServiceTestAccess::coreOf(*rig.service);
    const auto cells = makeCells(DisplayPixelLayout::Straight8888, 11);
    const auto settings = displaySettings(kNoChromaticity);
    const auto expected = bloom::runtime::measureScopeStoredFrameCpu(
        storedOf(cells, DisplayPixelLayout::Straight8888), settings, {}, *rig.color);
    GpuPreviewDisplayServiceTestAccess::setShownScopeUploadPollOverride(
        *core, [](bloom::render::GpuImageUpload&) {
            return bloom::render::GpuImageUploadPollResult::Pending;
        });
    const auto before = rig.counters();
    const auto dropsBefore = rig.measurer->counters().deviceDrops;
    auto request = rig.stored(cells, DisplayPixelLayout::Straight8888, settings, {});
    const auto tag = request.tag;
    rig.measurer->show(std::move(request));
    if (!awaitCondition("the held measure is taken up by the device", [&] {
            return rig.counters().shownScopeUploads == before.shownScopeUploads + 1;
        }))
        return;
    // The owner cancels the measure, proves its submission retired and stops.
    rig.service->beginShutdown();
    rig.service.reset();
    if (!awaitCondition("the cancelled measure's frame is counted on a worker",
                        [&] { return !rig.measurer->busy(); }))
        return;
    const auto result = rig.measurer->take();
    checks.expect(result.has_value() && result->tag == tag && result->scopes != nullptr &&
                      result->scopes->status == PreviewFrameScopesStatus::Measured &&
                      result->scopes->tables.route() == ScopeRoute::Cpu &&
                      result->scopes->tables.unpack() == expected.counts &&
                      !result->deviceMessage.empty(),
                  "a measure the stopping service cancelled is counted by the CPU reference");
    checks.expect(rig.measurer->counters().deviceDrops == dropsBefore + 1,
                  "and is counted as a measure the device did not finish");
    checks.expect(rig.policy->snapshot().reservations.admittedPhases == 0,
                  "its processing phase is released");
}

bool run(Expectations& checks, const TestOptions& options,
         const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
         const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
         const bloom::document::Snapshot& snapshot,
         const std::shared_ptr<const ScopeSignalColor>& color,
         const std::shared_ptr<const bloom::runtime::GpuSceneOcioContext>& ocio) {
    Rig rig(options, plan, processor, snapshot, color, ocio);
    if (!rig.serviceReady()) {
        std::cout << "Unsupported: genuine offscreen GPU preparation unavailable: "
                  << rig.service->status().residentDetail << '\n';
        checks.expect(!options.requireDevice, "required genuine offscreen GPU device available");
        return false;
    }
    std::uint64_t generation = 1;
    testStoredFrames(checks, rig);
    testPreparedFrame(checks, rig, generation);
    testFramesComeFirst(checks, rig, generation);
    testRegions(checks, rig);
    const auto counters = rig.measurer->counters();
    checks.expect(counters.cpuMeasures == 0 && counters.deviceDrops == 0,
                  "no measure of this run was dropped to the worker");
    testShutdownDuringMeasure(checks, rig);
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
        std::filesystem::path{BLOOM_SHOWN_FRAME_SCOPES_TEST_EXECUTABLE};
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
    bloom::document::Project project(kProjectId, "shown-frame-scopes");
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
    auto prepared = ScopeSignalColor::prepare(
        {.config =
             std::make_shared<const bloom::color::ResolvedBloomNeutralConfig>(std::move(*config)),
         .signal = ScopeSignal::Display,
         .display = {},
         .view = {}});
    if (!prepared) {
        std::cerr << "the Display signal's color objects did not prepare: " << prepared.message
                  << '\n';
        return 1;
    }

    Expectations checks;
    if (!run(checks, options, plan, processor, document.snapshot(), prepared.color,
             resolved.context))
        return options.requireDevice ? 1 : kSkipExit;
    if (checks.failures() + bloom::test_support::awaitFailureCount() != 0)
        return 1;
    std::cout << "PASS: a shown frame's Display tables are counted on the device at display "
                 "time\n";
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
