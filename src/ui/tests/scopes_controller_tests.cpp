// #724 Scopes: the controller behind the Scopes panel.
//
// Genuine CPU reference frames (the real CpuCompositionEvaluator and display preparer) are shown
// to a ScopesController whose measure is the production one over the CPU route. The measure never
// evaluates a scene: every scene run below is a frame the test built, counted, and none is added
// by a measure of a shown, cached or fill frame. Asserted:
//   * nothing is measured without a frame or without a scope asked for;
//   * a shown frame is measured on a task worker, never on the interface thread, and its tables
//     are attached to the frame's cache entry and charged to the cache's byte budget;
//   * a frame with a sidecar measured under the same settings is not measured again;
//   * a changed scope setting measures again without touching the frame's cache entry;
//   * while a new frame is measured, the earlier frame's tables are on show and labeled behind;
//   * a superseded measure is cancelled and its result is neither shown nor cached;
//   * during a RAM preview fill the frames the cache retains are measured in the background;
//   * a sidecar goes with its frame, and one that does not fit the budget is not kept;
//   * source order: the CPU process picture (Rendered), else the retained picture (Retained, once,
//     at full detail; the Working signal says it is not available for a cached frame); a device
//     tap that is refused, busy or failed falls back to the retained picture and, when the frame
//     stays, is replaced by the Rendered tables without a flicker of source (the tap's outcomes are
//     scripted here; the real tap is exercised by bloom.ui.scopes-device-route);
//   * the sidecar lookup prefers Rendered full, Rendered live, Retained full, Retained live, and a
//     Retained sidecar never replaces a Rendered one;
//   * every flagged RAM preview fill frame is answered (measured or dropped) on every path: panel
//     hidden or every scope collapsed with taps waiting, settings changed in flight, a frame the
//     cache does not retain, a cancelled fill, a destroyed controller, a second panel, shutdown.
// No device is used.

#include "await_qt.hpp"
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scope_fill_taps.hpp>
#include <bloom/ui/scopes_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace document = bloom::document;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace ui = bloom::ui;
using bloom::core::Color4d;
using bloom::runtime::PreparedPreviewFrame;
using bloom::test_support::awaitFailureCount;
using bloom::test_support::awaitQtCondition;
using bloom::ui::PreviewFrameCache;
using bloom::ui::PreviewFrameCacheKey;
using bloom::ui::ScopesReading;
using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (!condition) {
            ++failures_;
            std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = document::NodeId::fromRaw(10);
constexpr auto kLayerNode = document::NodeId::fromRaw(11);
constexpr auto kStackNode = document::NodeId::fromRaw(12);
constexpr auto kOutputNode = document::NodeId::fromRaw(13);
constexpr auto kLayer = document::LayerId::fromRaw(20);
constexpr auto kSlot = document::LayerSlotId::fromRaw(30);

[[nodiscard]] document::CompositionFormat format() {
    const auto value = document::CompositionFormat::create(64, 36);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
oneSolidPlan(const document::ProjectId projectId = kProjectId,
             const document::Revision revision = document::Revision::fromRaw(7)) {
    const auto compositionFormat = format();
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNode,
                               {document::ParameterId::fromRaw(40), Color4d{1.0, 0.0, 0.0, 1.0}},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
                                static_cast<double>(compositionFormat.width())},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
                                static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNode, kLayer, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(41),
                                       document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(43),
                                       document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(44), document::kDefaultScale},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(45),
                                         document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(42), 1.0},
        document::ParameterId::fromRaw(46), bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode,
        {runtime::CompiledMergeInput{kSlot, kLayer, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{revision, projectId, kCompositionId,
                                                   compositionFormat, std::move(operations),
                                                   runtime::OperationIndex::fromRaw(3)});
}

// Scene runs by this test's own evaluator: the frames it builds. The scopes add none.
std::atomic<int> gSceneRuns{0};

[[nodiscard]] std::shared_ptr<const PreparedPreviewFrame>
cpuFrame(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const std::uint64_t generation, const std::int64_t time = 0) {
    const runtime::CpuCompositionEvaluator evaluator;
    ++gSceneRuns;
    const auto evaluated = evaluator.evaluate(
        plan,
        runtime::EvaluationRequest{.time = bloom::core::RationalTime::fromInteger(time),
                                   .output = plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = std::size_t{1} << 22},
        {});
    if (evaluated.frame() == nullptr) {
        return nullptr;
    }
    const runtime::CpuReferenceDisplayPreparer preparer;
    const auto display =
        preparer.prepare(evaluated.frame(),
                         runtime::ReferenceDisplayPreparationRequest{
                             .intent = runtime::ReferenceDisplayIntent::LinearRec709SceneToSrgb,
                             .aggregatePixelStorageByteLimit = std::size_t{1} << 22,
                             .viewAdjust = {},
                             .displayName = {},
                             .viewName = {},
                             .showLook = true},
                         {});
    if (display.frame() == nullptr) {
        return nullptr;
    }
    const auto prepared = PreparedPreviewFrame::create(generation, display.frame());
    if (!prepared.has_value()) {
        return nullptr;
    }
    return std::make_shared<const PreparedPreviewFrame>(*prepared);
}

} // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication application(argc, argv);
        Expectations expectations;
        runtime::TaskScheduler scheduler{runtime::TaskSchedulerConfig{.cpuWorkerCount = 2,
                                                                      .blockingIoWorkerCount = 1,
                                                                      .cpuQueueCapacity = 32,
                                                                      .blockingIoQueueCapacity = 4,
                                                                      .terminalHistoryCapacity = 64,
                                                                      .diagnosticsPerTask = 8,
                                                                      .groupRegistryCapacity = 8}};
        ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
        auto cache = std::make_shared<PreviewFrameCache>(std::size_t{1} << 28);

        const auto production = ui::makeScopesMeasure({});
        // The production measure behind a gate the test can hold shut, recording its thread, the
        // time and detail of every call and how many ran at once.
        struct Gate final {
            std::mutex mutex;
            std::condition_variable condition;
            bool holdAll = false;
            bool holdFull = false; // hold the calls at detail 0
            std::atomic<int> entered{0};
            std::atomic<int> running{0};
            std::atomic<int> maxRunning{0};
            std::atomic<bool> onInterfaceThread{false};
            struct Call final {
                double seconds = 0.0;
                std::uint32_t detail = 0;
            };
            std::vector<Call> calls; // guarded by `mutex`

            void hold(const bool all, const bool full) {
                const std::lock_guard lock(mutex);
                holdAll = all;
                holdFull = full;
                condition.notify_all();
            }
            void enter(const double seconds, const std::uint32_t detail) {
                std::unique_lock lock(mutex);
                calls.push_back({seconds, detail});
                ++entered;
                const int now = ++running;
                int seen = maxRunning.load();
                while (now > seen && !maxRunning.compare_exchange_weak(seen, now)) {
                }
                condition.wait(lock, [&] { return !(holdAll || (holdFull && detail == 0)); });
            }
            [[nodiscard]] std::vector<Call> callsSince(const std::size_t first) {
                const std::lock_guard lock(mutex);
                return {calls.begin() + static_cast<std::ptrdiff_t>(first), calls.end()};
            }
        };
        const auto gate = std::make_shared<Gate>();
        const auto interfaceThread = std::this_thread::get_id();
        const ui::ScopesMeasure measure =
            [production, gate, interfaceThread](const ui::ScopesMeasureInput& input,
                                                const ui::PreviewScopeIdentity& identity,
                                                const runtime::CancellationToken& cancellation) {
                if (std::this_thread::get_id() == interfaceThread)
                    gate->onInterfaceThread = true;
                gate->enter(input.source.identity.time.toSeconds(), identity.settings.detail);
                const auto result = production(input, identity, cancellation);
                --gate->running;
                return result;
            };

        const auto plan = oneSolidPlan();
        const auto frame0 = cpuFrame(plan, 1, 0);
        const auto frame1 = cpuFrame(plan, 2, 1);
        const auto frame2 = cpuFrame(plan, 3, 2);
        const auto frame3 = cpuFrame(plan, 4, 3);
        expectations.expect(frame0 && frame1 && frame2 && frame3, "the CPU frames are built");
        if (!frame0 || !frame1 || !frame2 || !frame3)
            return 1;
        const auto keyOf = [](const std::shared_ptr<const PreparedPreviewFrame>& frame) {
            return PreviewFrameCacheKey::forIdentity(frame->desiredIdentity());
        };
        expectations.expect(cache->insert(frame0).retained() && cache->insert(frame1).retained(),
                            "the frames are cached");
        const auto frameBytes = cache->hostBytes();

        {
            ui::ScopesController controller(scheduler, bridge, cache, measure);
            int changes = 0;
            QObject::connect(&controller, &ui::ScopesController::readingChanged,
                             [&changes] { ++changes; });
            runtime::ScopeMeasureSettings settings;
            settings.scopes = render::kScopeTableHistogram | render::kScopeTableWaveform;
            settings.signal = runtime::ScopeSignal::Display;
            // The frame is measured live, then in full: settled is the end of both.
            const auto settled = [&] {
                return controller.reading().state == ScopesReading::State::Ready &&
                       !controller.busy();
            };
            const auto identityOf =
                [&](const runtime::ScopeMeasureSettings& value,
                    const runtime::ScopeSource source = runtime::ScopeSource::Rendered) {
                    return ui::PreviewScopeIdentity{
                        .settings = value, .viewAdjust = {}, .source = source};
                };

            // Nothing to measure.
            controller.showFrame(frame0, {});
            expectations.expect(controller.reading().state == ScopesReading::State::Idle &&
                                    controller.measuresStarted() == 0,
                                "a frame with no scope asked for is not measured");
            controller.showFrame(nullptr, {});
            controller.setSettings(settings);
            expectations.expect(controller.reading().state == ScopesReading::State::Idle &&
                                    controller.measuresStarted() == 0 &&
                                    !controller.reading().shown,
                                "scopes with no frame are not measured");

            // A shown frame is measured on a worker and its tables attached to its cache entry.
            controller.showFrame(frame0, {});
            expectations.expect(controller.reading().state == ScopesReading::State::Measuring &&
                                    controller.reading().scopes == nullptr &&
                                    !controller.reading().behind,
                                "the first frame is being measured with nothing on show");
            expectations.expect(awaitQtCondition("the frame's scopes arrive: ", settled),
                                "the frame's scopes arrive: " + controller.reading().message);
            const auto first = controller.reading().scopes;
            expectations.expect(first != nullptr && !controller.reading().behind &&
                                    first->tables.route() == runtime::ScopeRoute::Cpu &&
                                    first->tables.counted() == 64U * 36U &&
                                    first->area == runtime::ScopeMeasuredArea{0, 0, 64, 36} &&
                                    first->color != nullptr &&
                                    controller.reading().color == first->color,
                                "the CPU reference counted every pixel of the frame");
            expectations.expect(!gate->onInterfaceThread.load() && gate->entered.load() == 2,
                                "the measures ran on a task worker: live, then full");
            expectations.expect(first != nullptr &&
                                    cache->scopesFor(keyOf(frame0), identityOf(settings)) == first,
                                "the tables are the frame's sidecar in the cache");
            expectations.expect(first != nullptr &&
                                    cache->scopeSidecarBytes() == first->byteSize() &&
                                    cache->hostBytes() == frameBytes + first->byteSize(),
                                "the sidecar is charged to the cache's byte budget");
            if (first != nullptr) {
                // A red solid centered on (2, 1): a 34 x 19 corner of it is in the picture, over
                // transparent black, through the display.
                const auto unpacked = first->tables.unpack();
                std::uint32_t redBins = 0;
                for (std::uint32_t code = 0; code < 1024; ++code)
                    redBins += unpacked.histogram(0, code) != 0 ? 1U : 0U;
                expectations.expect(redBins == 2 && unpacked.histogram(0, 1023) == 34U * 19U &&
                                        unpacked.histogram(0, 0) == 64U * 36U - 34U * 19U &&
                                        unpacked.histogram(1, 0) == 64U * 36U,
                                    "the red solid is counted at full red and no green");
            }

            // The same frame again is a cache hit.
            controller.showFrame(nullptr, {});
            expectations.expect(controller.reading().state == ScopesReading::State::Idle &&
                                    controller.reading().scopes == nullptr,
                                "with no frame nothing is on show");
            controller.showFrame(frame0, {});
            expectations.expect(controller.reading().state == ScopesReading::State::Ready &&
                                    controller.reading().scopes == first &&
                                    controller.measuresStarted() == 2 &&
                                    controller.cacheHits() == 1,
                                "a frame with a matching sidecar is not measured again");

            // A scope setting measures again and leaves the frame's entry alone.
            auto ignoring = settings;
            ignoring.ignoreTransparent = true;
            controller.setSettings(ignoring);
            expectations.expect(controller.reading().state == ScopesReading::State::Measuring &&
                                    controller.reading().behind &&
                                    controller.reading().scopes == first,
                                "a changed setting shows the old tables as behind");
            expectations.expect(
                awaitQtCondition("the frame is measured under the new setting", settled),
                "the frame is measured under the new setting");
            expectations.expect(controller.measuresStarted() == 4 &&
                                    cache->contains(keyOf(frame0)) && cache->size() == 2 &&
                                    cache->scopesFor(keyOf(frame0), identityOf(settings)) ==
                                        nullptr &&
                                    cache->scopesFor(keyOf(frame0), identityOf(ignoring)) ==
                                        controller.reading().scopes,
                                "the sidecar is replaced and the frame's entry is untouched");
            controller.setSettings(settings);
            expectations.expect(awaitQtCondition("the first setting is measured again", settled),
                                "the first setting is measured again");
            const auto current = controller.reading().scopes;
            expectations.expect(first != nullptr && current != nullptr && current != first &&
                                    current->tables == first->tables &&
                                    current->area == first->area,
                                "a cached frame measured again has the tables of its first "
                                "measure");

            // Another frame: the earlier tables are on show, labeled, until its own arrive.
            controller.showFrame(frame1, {});
            expectations.expect(
                controller.reading().state == ScopesReading::State::Measuring &&
                    controller.reading().behind && controller.reading().scopes == current &&
                    controller.reading().measured && controller.reading().shown &&
                    keyOf(frame0) == PreviewFrameCacheKey::forIdentity(
                                         controller.reading().measured->identity) &&
                    keyOf(frame1) ==
                        PreviewFrameCacheKey::forIdentity(controller.reading().shown->identity),
                "the earlier frame's tables are labeled behind and name their frame");
            expectations.expect(awaitQtCondition("the new frame's scopes arrive", settled),
                                "the new frame's scopes arrive");
            expectations.expect(
                !controller.reading().behind && controller.reading().scopes != current &&
                    controller.reading().measured &&
                    keyOf(frame1) ==
                        PreviewFrameCacheKey::forIdentity(controller.reading().measured->identity),
                "the reading is the shown frame's");

            // The Viewer's exposure is part of the Display measure, and not of the Working one.
            const auto beforeAdjust = controller.measuresStarted();
            controller.showFrame(frame1, runtime::ViewAdjust{.exposure = -1.0, .gamma = 1.0});
            expectations.expect(awaitQtCondition("the exposed frame is measured", settled) &&
                                    controller.measuresStarted() == beforeAdjust + 2,
                                "an exposure measures the Display signal again");
            expectations.expect(
                controller.reading().scopes != nullptr &&
                    controller.reading().scopes->tables.unpack().histogram(0, 1023) == 0,
                "the exposure darkens the measured signal");
            controller.showFrame(frame1, {});
            expectations.expect(awaitQtCondition("the neutral frame is measured again", settled),
                                "the neutral frame is measured again");

            // A superseded measure is cancelled; its result is neither shown nor cached.
            expectations.expect(cache->insert(frame2).retained() &&
                                    cache->insert(frame3).retained(),
                                "two more frames are cached");
            gate->hold(true, false);
            const auto enteredBefore = gate->entered.load();
            controller.showFrame(frame2, {});
            expectations.expect(
                awaitQtCondition("the held measure started",
                                 [&] { return gate->entered.load() > enteredBefore; }),
                "the held measure started");
            controller.showFrame(frame3, {});
            gate->hold(false, false);
            expectations.expect(awaitQtCondition("the last shown frame's scopes arrive",
                                                 [&] {
                                                     return controller.reading().state ==
                                                                ScopesReading::State::Ready &&
                                                            !controller.busy();
                                                 }),
                                "the last shown frame's scopes arrive");
            expectations.expect(
                controller.reading().measured &&
                    keyOf(frame3) == PreviewFrameCacheKey::forIdentity(
                                         controller.reading().measured->identity) &&
                    cache->scopesFor(keyOf(frame2), identityOf(settings)) == nullptr &&
                    cache->scopesFor(keyOf(frame3), identityOf(settings)) != nullptr,
                "the superseded frame's result was not shown and not cached");

            // Bounded work: one measure in flight, the latest request wins, and the frames in
            // between are never measured. The first measure is held; five more frames are shown.
            {
                std::vector<std::shared_ptr<const PreparedPreviewFrame>> frames;
                for (std::int64_t index = 0; index < 6; ++index) {
                    frames.push_back(
                        cpuFrame(plan, 100 + static_cast<std::uint64_t>(index), 100 + index));
                    expectations.expect(frames.back() && cache->insert(frames.back()).retained(),
                                        "a frame of the bounded run is cached");
                }
                if (std::ranges::all_of(frames,
                                        [](const auto& frame) { return frame != nullptr; })) {
                    const auto liveIdentity = [&] {
                        auto live = settings;
                        live.detail = ui::kLiveDetail;
                        return identityOf(live);
                    }();
                    const auto started = controller.measuresStarted();
                    const auto skipped = controller.measuresSkipped();
                    const auto base = gate->entered.load();
                    gate->hold(true, false);
                    controller.showFrame(frames[0], {});
                    expectations.expect(
                        awaitQtCondition("the first measure of the run started and is held",
                                         [&] { return gate->entered.load() > base; }),
                        "the first measure of the run started and is held");
                    for (std::size_t index = 1; index < frames.size(); ++index)
                        controller.showFrame(frames[index], {});
                    expectations.expect(controller.measuresStarted() == started + 1 &&
                                            gate->entered.load() == base + 1 &&
                                            controller.measuresInFlight() == 1 &&
                                            controller.maxMeasuresInFlight() == 1 &&
                                            controller.busy(),
                                        "no second measure starts while the first is held");
                    expectations.expect(
                        controller.measuresSkipped() == skipped + 4,
                        "four of the five frames were replaced before they started");
                    // The live measure of the newest frame runs; its full measure is held.
                    gate->hold(false, true);
                    expectations.expect(
                        awaitQtCondition("the newest frame's live measure ran and its full one "
                                         "started",
                                         [&] { return gate->entered.load() >= base + 3; }),
                        "the newest frame's live measure ran and its full one "
                        "started");
                    const auto calls = gate->callsSince(static_cast<std::size_t>(base));
                    expectations.expect(calls.size() == 3 && calls[0].seconds == 100.0 &&
                                            calls[0].detail == ui::kLiveDetail &&
                                            calls[1].seconds == 105.0 &&
                                            calls[1].detail == ui::kLiveDetail &&
                                            calls[2].seconds == 105.0 && calls[2].detail == 0U,
                                        "only the held frame and the newest were measured");
                    expectations.expect(controller.reading().state == ScopesReading::State::Ready &&
                                            controller.reading().detail == ui::kLiveDetail &&
                                            controller.reading().scopes != nullptr &&
                                            cache->scopesFor(keyOf(frames[5]), liveIdentity) ==
                                                controller.reading().scopes &&
                                            controller.reading().measured &&
                                            keyOf(frames[5]) ==
                                                PreviewFrameCacheKey::forIdentity(
                                                    controller.reading().measured->identity),
                                        "the newest frame's live tables are shown and cached");
                    expectations.expect(
                        cache->scopesFor(keyOf(frames[0]), liveIdentity) == nullptr &&
                            cache->scopesFor(keyOf(frames[0]), identityOf(settings)) == nullptr,
                        "the held frame's result was neither shown nor cached");
                    expectations.expect(controller.measuresInFlight() == 1 &&
                                            controller.maxMeasuresInFlight() == 1,
                                        "still one measure in flight");
                    gate->hold(false, false);
                    expectations.expect(
                        awaitQtCondition("the newest frame ends at full detail", settled),
                        "the newest frame ends at full detail");
                    expectations.expect(
                        controller.reading().detail == 0U &&
                            cache->scopesFor(keyOf(frames[5]), identityOf(settings)) ==
                                controller.reading().scopes &&
                            controller.measuresStarted() == started + 3 &&
                            controller.measuresInFlight() == 0 &&
                            gate->entered.load() == base + 3 &&
                            controller.lastTableReadbackBytes() == 0,
                        "one full measure of the newest frame, its sidecar at detail 0");
                }
            }

            // Live, then full. The vectorscope is measured too, for the pooled comparison.
            {
                auto withVectorscope = settings;
                withVectorscope.scopes =
                    render::kScopeTableHistogram | render::kScopeTableVectorscope;
                controller.setSettings(withVectorscope);
                expectations.expect(
                    awaitQtCondition("the shown frame settles under the settings", settled),
                    "the shown frame settles under the settings");
                auto liveSettings = withVectorscope;
                liveSettings.detail = ui::kLiveDetail;
                const auto fullIdentity = identityOf(withVectorscope);
                const auto liveIdentity = identityOf(liveSettings);

                const auto stay = cpuFrame(plan, 120, 120);
                const auto replaced = cpuFrame(plan, 121, 121);
                const auto replacement = cpuFrame(plan, 122, 122);
                const auto cached = cpuFrame(plan, 123, 123);
                expectations.expect(
                    stay && replaced && replacement && cached && cache->insert(stay).retained() &&
                        cache->insert(replaced).retained() &&
                        cache->insert(replacement).retained() && cache->insert(cached).retained(),
                    "the frames of the live and full run are cached");
                if (stay && replaced && replacement && cached) {
                    const auto sourceOf = [](const auto& frame) {
                        return ui::ScopesMeasureInput{
                            ui::PreviewScopeSource{frame->desiredIdentity(),
                                                   frame->processIdentity(),
                                                   frame->provenance().provider},
                            frame, nullptr, false, {}};
                    };
                    // A frame shown and left alone: the live measure, then the full one.
                    auto base = gate->entered.load();
                    auto started = controller.measuresStarted();
                    gate->hold(false, true);
                    controller.showFrame(stay, {});
                    expectations.expect(
                        awaitQtCondition("the full measure follows the live one",
                                         [&] { return gate->entered.load() >= base + 2; }),
                        "the full measure follows the live one");
                    const auto live = controller.reading().scopes;
                    expectations.expect(
                        live != nullptr &&
                            controller.reading().state == ScopesReading::State::Ready &&
                            controller.reading().detail == ui::kLiveDetail &&
                            live->identity == liveIdentity &&
                            cache->scopesFor(keyOf(stay), liveIdentity) == live &&
                            cache->scopesFor(keyOf(stay), fullIdentity) == nullptr,
                        "the live tables are shown while the full ones are measured");
                    gate->hold(false, false);
                    expectations.expect(awaitQtCondition("the frame ends at full detail", settled),
                                        "the frame ends at full detail");
                    const auto full = controller.reading().scopes;
                    const auto calls = gate->callsSince(static_cast<std::size_t>(base));
                    expectations.expect(
                        calls.size() == 2 && calls[0].detail == ui::kLiveDetail &&
                            calls[1].detail == 0U && controller.measuresStarted() == started + 2 &&
                            controller.reading().detail == 0U && full != nullptr && full != live &&
                            cache->scopesFor(keyOf(stay), fullIdentity) == full &&
                            cache->scopesFor(keyOf(stay), liveIdentity) == nullptr,
                        "two measures, live then full; the full tables replace the "
                        "live ones in the reading and the sidecar");
                    if (live != nullptr && full != nullptr) {
                        const auto direct = production(sourceOf(stay), fullIdentity, {});
                        expectations.expect(
                            direct.measure.status == runtime::ScopeFrameStatus::Measured &&
                                full->tables == runtime::ScopeTables::pack(direct.measure.counts,
                                                                           direct.measure.route),
                            "the full tables equal a direct detail 0 measure");
                        const auto coarse = live->tables.unpack();
                        const auto fine = full->tables.unpack();
                        const auto cells = live->tables.layout().cells();
                        bool pooled = cells * 2U == full->tables.layout().cells();
                        for (std::uint32_t cb = 0; pooled && cb < cells; ++cb)
                            for (std::uint32_t cr = 0; cr < cells; ++cr)
                                pooled = pooled && coarse.vectorscope(cb, cr) ==
                                                       fine.vectorscope(2 * cb, 2 * cr) +
                                                           fine.vectorscope(2 * cb + 1, 2 * cr) +
                                                           fine.vectorscope(2 * cb, 2 * cr + 1) +
                                                           fine.vectorscope(2 * cb + 1, 2 * cr + 1);
                        bool histogramEqual = true;
                        for (std::uint32_t channel = 0; channel < render::kScopeChannels;
                             ++channel) {
                            for (std::uint32_t code = 0; code < 1024; ++code)
                                histogramEqual =
                                    histogramEqual && coarse.histogram(channel, code) ==
                                                          fine.histogram(channel, code);
                            histogramEqual = histogramEqual &&
                                             coarse.histogram(channel, render::kScopeCodeUnder) ==
                                                 fine.histogram(channel, render::kScopeCodeUnder) &&
                                             coarse.histogram(channel, render::kScopeCodeOver) ==
                                                 fine.histogram(channel, render::kScopeCodeOver);
                        }
                        expectations.expect(pooled && histogramEqual && coarse.counted() != 0U,
                                            "the live tables are the full ones pooled: Vectorscope "
                                            "2x2 sums, Histogram equal");
                    }

                    // A frame replaced while its live measure is held never gets a full measure.
                    base = gate->entered.load();
                    gate->hold(true, false);
                    controller.showFrame(replaced, {});
                    expectations.expect(
                        awaitQtCondition("the live measure of the frame to be replaced is held",
                                         [&] { return gate->entered.load() > base; }),
                        "the live measure of the frame to be replaced is held");
                    controller.showFrame(replacement, {});
                    gate->hold(false, false);
                    expectations.expect(awaitQtCondition("the replacement settles", settled),
                                        "the replacement settles");
                    const auto replacedCalls = gate->callsSince(static_cast<std::size_t>(base));
                    std::size_t replacedFull = 0;
                    for (const auto& call : replacedCalls)
                        replacedFull += call.seconds == 121.0 && call.detail == 0U ? 1U : 0U;
                    expectations.expect(
                        replacedCalls.size() == 3 && replacedFull == 0 &&
                            replacedCalls[0].seconds == 121.0 &&
                            replacedCalls[1].seconds == 122.0 &&
                            replacedCalls[2].seconds == 122.0 && replacedCalls[2].detail == 0U &&
                            cache->scopesFor(keyOf(replaced), liveIdentity) == nullptr &&
                            cache->scopesFor(keyOf(replaced), fullIdentity) == nullptr,
                        "a frame replaced before its live tables arrive is never "
                        "measured at full detail");

                    // A sidecar at detail 0 is shown at once; one at detail 1 is shown at once and
                    // the full measure follows.
                    controller.showFrame(stay, {});
                    started = controller.measuresStarted();
                    expectations.expect(controller.reading().state == ScopesReading::State::Ready &&
                                            controller.reading().scopes == full &&
                                            controller.reading().detail == 0U &&
                                            !controller.busy() &&
                                            controller.measuresStarted() == started,
                                        "a detail 0 sidecar is shown with no measure");
                    const auto liveMeasured = production(sourceOf(cached), liveIdentity, {});
                    auto sidecar = std::make_shared<ui::PreviewScopeSidecar>();
                    sidecar->identity = liveIdentity;
                    sidecar->tables = runtime::ScopeTables::pack(liveMeasured.measure.counts,
                                                                 liveMeasured.measure.route);
                    sidecar->area = liveMeasured.measure.area;
                    sidecar->color = liveMeasured.color;
                    expectations.expect(cache->attachScopes(keyOf(cached), sidecar),
                                        "a live sidecar is attached to the frame");
                    base = gate->entered.load();
                    gate->hold(false, true);
                    controller.showFrame(cached, {});
                    expectations.expect(controller.reading().state == ScopesReading::State::Ready &&
                                            controller.reading().scopes == sidecar &&
                                            controller.reading().detail == ui::kLiveDetail &&
                                            !controller.reading().behind,
                                        "a detail 1 sidecar is shown at once");
                    expectations.expect(
                        awaitQtCondition("the full measure of the live sidecar's frame started",
                                         [&] { return gate->entered.load() > base; }),
                        "the full measure of the live sidecar's frame started");
                    gate->hold(false, false);
                    expectations.expect(awaitQtCondition("the live sidecar's frame ends at full "
                                                         "detail",
                                                         settled),
                                        "the live sidecar's frame ends at full "
                                        "detail");
                    const auto cachedCalls = gate->callsSince(static_cast<std::size_t>(base));
                    expectations.expect(cachedCalls.size() == 1 && cachedCalls[0].detail == 0U &&
                                            controller.measuresStarted() == started + 1 &&
                                            controller.reading().detail == 0U,
                                        "one full measure follows a live sidecar");
                }
                controller.setSettings(settings);
                expectations.expect(
                    awaitQtCondition("the first settings are measured again", settled),
                    "the first settings are measured again");
            }

            // A RAM preview fill measures the frames the cache retains, from their retained
            // pictures (Retained, full detail, CPU).
            const auto retainedOf = [&](const runtime::ScopeMeasureSettings& value) {
                return identityOf(value, runtime::ScopeSource::Retained);
            };
            const auto frame4 = cpuFrame(plan, 5, 4);
            const auto frame5 = cpuFrame(plan, 6, 5);
            expectations.expect(frame4 && frame5, "the fill's frames are built");
            if (frame4 && frame5) {
                controller.setRangeFillActive(true);
                expectations.expect(cache->insert(frame4).retained() &&
                                        cache->insert(frame5).retained(),
                                    "the fill's frames are cached");
                expectations.expect(
                    awaitQtCondition("the retained frames are measured in the background",
                                     [&] {
                                         return cache->scopesFor(keyOf(frame4),
                                                                 retainedOf(settings)) != nullptr &&
                                                cache->scopesFor(keyOf(frame5),
                                                                 retainedOf(settings)) != nullptr &&
                                                !controller.busy();
                                     }),
                    "the retained frames are measured in the background");
                controller.setRangeFillActive(false);
                const auto started = controller.measuresStarted();
                controller.showFrame(frame4, {});
                expectations.expect(controller.reading().state == ScopesReading::State::Ready &&
                                        controller.measuresStarted() == started &&
                                        controller.reading().scopes ==
                                            cache->scopesFor(keyOf(frame4), retainedOf(settings)) &&
                                        controller.reading().scopes->identity.source ==
                                            runtime::ScopeSource::Retained,
                                    "a warm frame shows its scopes without measuring");
            }
            // An edit of the document: the frame of the new revision is another cache entry, with
            // no sidecar, and is measured; the earlier revision's sidecar stays with its own frame.
            {
                const auto edited = oneSolidPlan(kProjectId, document::Revision::fromRaw(8));
                const auto editedFrame = cpuFrame(edited, 7, 0);
                expectations.expect(editedFrame != nullptr, "the edited frame is built");
                if (editedFrame != nullptr) {
                    expectations.expect(
                        !(keyOf(editedFrame) == keyOf(frame0)) &&
                            cache->insert(editedFrame).retained() &&
                            cache->scopesFor(keyOf(editedFrame), identityOf(settings)) == nullptr,
                        "an edited document's frame has no sidecar");
                    const auto started = controller.measuresStarted();
                    controller.showFrame(editedFrame, {});
                    expectations.expect(
                        controller.reading().state == ScopesReading::State::Measuring &&
                            controller.reading().behind,
                        "the earlier revision's tables are behind the edited frame");
                    expectations.expect(awaitQtCondition("the edited frame is measured", settled),
                                        "the edited frame is measured");
                    expectations.expect(
                        controller.measuresStarted() == started + 2 &&
                            !controller.reading().behind &&
                            controller.reading().scopes ==
                                cache->scopesFor(keyOf(editedFrame), identityOf(settings)) &&
                            controller.reading().scopes !=
                                cache->scopesFor(keyOf(frame0), identityOf(settings)),
                        "the edited frame has its own sidecar");
                }
            }
            expectations.expect(changes > 0, "the panel is told when the reading changes");
            expectations.expect(!gate->onInterfaceThread.load(),
                                "no measure ran on the interface thread");
            expectations.expect(gate->maxRunning.load() == 1 &&
                                    controller.maxMeasuresInFlight() == 1 &&
                                    controller.measuresInFlight() == 0,
                                "never more than one measure ran at once, the fill included");
        }

        // ---- Source order: the process picture, else the retained picture -------------------------
        {
            auto cache2 = std::make_shared<PreviewFrameCache>(std::size_t{1} << 28);
            const auto plan2 = oneSolidPlan(kProjectId, document::Revision::fromRaw(20));
            const auto live = cpuFrame(plan2, 30, 0);
            expectations.expect(live != nullptr && cache2->insert(live).retained(),
                                "the frame of the source order cases is built and cached");
            if (live == nullptr)
                return 1;
            // The frame as the cache serves it: display-only, no process picture.
            const auto cachedFrame = cache2->take(live->desiredIdentity());
            expectations.expect(cachedFrame != nullptr && !cachedFrame->hasProcessFrame() &&
                                    live->hasProcessFrame(),
                                "the cache serves a display-only frame; the foreground frame keeps "
                                "its process picture");
            const auto key = keyOf(live);
            const auto scenesBefore = gSceneRuns.load();
            ui::ScopesController controller(scheduler, bridge, cache2, production);
            runtime::ScopeMeasureSettings settings;
            settings.scopes = render::kScopeTableHistogram | render::kScopeTableWaveform;
            settings.signal = runtime::ScopeSignal::Display;
            controller.setSettings(settings);
            const auto settled = [&] {
                return controller.reading().state != ScopesReading::State::Measuring &&
                       !controller.busy();
            };
            const auto identityIn = [&](const runtime::ScopeMeasureSettings& value,
                                        const runtime::ScopeSource source,
                                        const std::uint32_t detail) {
                auto copy = value;
                copy.detail = detail;
                return ui::PreviewScopeIdentity{.settings = copy, .viewAdjust = {}, .source = source};
            };

            // (c) A cached frame has no process picture: its retained picture is counted once, in
            // full, and the sidecar names the source.
            controller.showFrame(cachedFrame, {});
            expectations.expect(awaitQtCondition("the cached frame is measured", settled),
                                "the cached frame is measured");
            const auto retained = controller.reading().scopes;
            expectations.expect(
                retained != nullptr && controller.reading().state == ScopesReading::State::Ready &&
                    controller.measuresStarted() == 1 && controller.reading().detail == 0U &&
                    retained->identity == identityIn(settings, runtime::ScopeSource::Retained, 0) &&
                    retained->tables.route() == runtime::ScopeRoute::Cpu &&
                    retained->tables.counted() == 64U * 36U && !controller.reading().behind &&
                    cache2->scopesFor(key, identityIn(settings, runtime::ScopeSource::Retained,
                                                      0)) == retained &&
                    cache2->scopesFor(key, identityIn(settings, runtime::ScopeSource::Rendered,
                                                      0)) == nullptr,
                "a retained picture is counted once, at full detail, and marked Retained");
            if (retained != nullptr) {
                const auto unpacked = retained->tables.unpack();
                expectations.expect(unpacked.histogram(0, 1023) == 34U * 19U &&
                                        unpacked.histogram(1, 0) == 64U * 36U,
                                    "the retained picture counts the red solid at full red");
                expectations.expect(retained->note.find("cached picture") != std::string::npos,
                                    "the sidecar says it was counted from the cached picture");
            }
            // Cached again: no new measure (Retained full is found).
            controller.showFrame(nullptr, {});
            controller.showFrame(cachedFrame, {});
            expectations.expect(controller.measuresStarted() == 1 &&
                                    controller.reading().scopes == retained,
                                "a Retained full sidecar is shown with no measure");

            // The Working signal is not available for a retained picture: the panel says so, with
            // the earlier tables marked behind, and nothing is cached.
            auto working = settings;
            working.signal = runtime::ScopeSignal::Working;
            controller.setSettings(working);
            expectations.expect(awaitQtCondition("the Working signal of the cached frame ends", settled),
                                "the Working signal of the cached frame ends");
            expectations.expect(
                controller.reading().state == ScopesReading::State::Failed &&
                    controller.reading().message.find("not available for a cached frame") !=
                        std::string::npos &&
                    controller.reading().behind && controller.reading().scopes == retained &&
                    cache2->scopesFor(key, identityIn(working, runtime::ScopeSource::Retained,
                                                      0)) == nullptr,
                "Working on a retained-only frame reports the measure's message, never empty "
                "tables, and the earlier tables stay marked behind");
            // (b) The same frame with its process picture is counted Rendered: live, then full.
            const auto started = controller.measuresStarted();
            controller.showFrame(live, {});
            expectations.expect(awaitQtCondition("the process picture is counted", settled),
                                "the process picture is counted");
            expectations.expect(
                controller.reading().scopes != nullptr &&
                    controller.reading().state == ScopesReading::State::Ready &&
                    controller.reading().scopes->identity ==
                        identityIn(working, runtime::ScopeSource::Rendered, 0) &&
                    controller.measuresStarted() == started + 2,
                "a frame with its process picture is counted Rendered, live then full, with the "
                "Working signal");
            // A Retained sidecar never replaces a Rendered one.
            {
                auto sidecar = std::make_shared<ui::PreviewScopeSidecar>();
                sidecar->identity = identityIn(working, runtime::ScopeSource::Retained, 0);
                sidecar->tables = retained->tables;
                const auto held = controller.reading().scopes;
                expectations.expect(!ui::attachScopeSidecar(*cache2, key, sidecar) &&
                                        cache2->scopesFor(key, identityIn(working,
                                                                          runtime::ScopeSource::Rendered,
                                                                          0)) == held,
                                    "a Retained sidecar does not replace the Rendered one");
                auto liveRendered = std::make_shared<ui::PreviewScopeSidecar>();
                liveRendered->identity = identityIn(working, runtime::ScopeSource::Rendered, 1);
                liveRendered->tables = retained->tables;
                expectations.expect(!ui::attachScopeSidecar(*cache2, key, liveRendered),
                                    "a Rendered live sidecar does not replace the Rendered full one");
            }
            // A Retained live sidecar is the lowest preference: it is shown and the full measure
            // (counted from the retained picture) follows.
            {
                controller.setSettings(settings);
                expectations.expect(awaitQtCondition("settings settle", settled), "settings settle");
                auto sidecar = std::make_shared<ui::PreviewScopeSidecar>();
                sidecar->identity = identityIn(settings, runtime::ScopeSource::Retained,
                                               ui::kLiveDetail);
                sidecar->tables = retained->tables;
                sidecar->color = retained->color;
                expectations.expect(cache2->attachScopes(key, sidecar), "a Retained live sidecar is attached");
                controller.showFrame(nullptr, {});
                const auto before = controller.measuresStarted();
                controller.showFrame(cachedFrame, {});
                expectations.expect(controller.reading().state == ScopesReading::State::Ready &&
                                        controller.reading().scopes == sidecar &&
                                        controller.reading().detail == ui::kLiveDetail,
                                    "a Retained live sidecar is shown at once");
                expectations.expect(awaitQtCondition("the full retained measure follows", settled) &&
                                        controller.measuresStarted() == before + 1 &&
                                        controller.reading().detail == 0U &&
                                        controller.reading().scopes->identity ==
                                            identityIn(settings, runtime::ScopeSource::Retained, 0),
                                    "one full retained measure follows a Retained live sidecar");
            }
            expectations.expect(gSceneRuns.load() == scenesBefore,
                                "no scene was evaluated by any measure of a shown or cached frame");
        }

        // ---- A device tap that does not measure falls back to the retained picture ----------------
        {
            auto cache3 = std::make_shared<PreviewFrameCache>(std::size_t{1} << 28);
            const auto plan3 = oneSolidPlan(kProjectId, document::Revision::fromRaw(21));
            const auto live = cpuFrame(plan3, 40, 0);
            expectations.expect(live != nullptr, "the tap cases' frame is built");
            if (live == nullptr)
                return 1;
            const auto key = keyOf(live);
            // The tap's outcomes are scripted: what the production measure does with them (the
            // fallback to the retained picture, the note, the retry) is production's.
            enum class Tap : std::uint8_t { Measured, Refused, Gone };
            struct Script final {
                std::atomic<Tap> mode{Tap::Measured};
                std::atomic<int> calls{0};
                std::atomic<int> renderedOnlyCalls{0};
                // While true, a Rendered-only attempt waits (so the test decides its outcome).
                std::atomic<bool> holdRetry{false};
                std::mutex mutex;
                // The display-only frame the next refused measure counts (the cache's own copy of
                // the shown frame).
                std::shared_ptr<const PreparedPreviewFrame> retained;
            };
            const auto script = std::make_shared<Script>();
            const ui::ScopesMeasure scripted =
                [production, script](const ui::ScopesMeasureInput& input,
                                     const ui::PreviewScopeIdentity& identity,
                                     const runtime::CancellationToken& cancellation) {
                    ++script->calls;
                    if (input.renderedOnly) {
                        ++script->renderedOnlyCalls;
                        script->holdRetry.wait(true);
                    }
                    const auto mode = script->mode.load();
                    ui::ScopesMeasured result;
                    if (mode == Tap::Measured) {
                        // The device tap measured: Rendered, at the detail asked for, route GPU.
                        // The picture is counted from the process picture here, labeled as the
                        // device counted it.
                        result = production(input, identity, cancellation);
                        result.measure.route = runtime::ScopeRoute::Gpu;
                    } else if (input.renderedOnly) {
                        // A Rendered-only attempt the device refused: nothing is measured.
                        auto only = input;
                        only.frame = nullptr;
                        result = production(only, identity, cancellation);
                        result.tapNote = "the device is busy";
                        result.tapRetry = true;
                    } else {
                        // The tap was refused: the retained picture is counted.
                        std::shared_ptr<const PreparedPreviewFrame> retainedFrame;
                        {
                            const std::lock_guard lock(script->mutex);
                            retainedFrame = script->retained;
                        }
                        auto fallback = input;
                        fallback.frame = std::move(retainedFrame);
                        result = production(fallback, identity, cancellation);
                        result.tapNote = mode == Tap::Gone ? "the Viewer's device holds no picture "
                                                             "of this frame"
                                                           : "the device has no room for the measure";
                        result.tapRetry = mode != Tap::Gone;
                    }
                    return result;
                };
            const auto retainedOf = [script](const std::shared_ptr<PreviewFrameCache>& frameCache,
                                             const std::shared_ptr<const PreparedPreviewFrame>& frame) {
                if (!frameCache->insert(frame).retained())
                    std::abort();
                const std::lock_guard lock(script->mutex);
                script->retained = frameCache->take(frame->desiredIdentity());
            };
            retainedOf(cache3, live);
            ui::ScopesController controller(scheduler, bridge, cache3, scripted);
            runtime::ScopeMeasureSettings settings;
            settings.scopes = render::kScopeTableHistogram;
            settings.signal = runtime::ScopeSignal::Display;
            controller.setSettings(settings);
            std::vector<runtime::ScopeSource> seen; // the source on show at each published reading
            QObject::connect(&controller, &ui::ScopesController::readingChanged, [&] {
                const auto& reading = controller.reading();
                if (reading.scopes != nullptr &&
                    (seen.empty() || seen.back() != reading.scopes->identity.source))
                    seen.push_back(reading.scopes->identity.source);
            });
            const auto settled = [&] {
                return controller.reading().state != ScopesReading::State::Measuring &&
                       !controller.busy();
            };
            const auto identityIn = [&](const runtime::ScopeSource source,
                                        const std::uint32_t detail) {
                auto copy = settings;
                copy.detail = detail;
                return ui::PreviewScopeIdentity{.settings = copy, .viewAdjust = {}, .source = source};
            };

            // The tap is refused: the frame is counted from its retained picture at once (full
            // detail, one measure), and, the frame staying, the tap is tried once more at full
            // detail; it measures now, and the Rendered tables replace the Retained ones.
            script->mode = Tap::Refused;
            script->holdRetry = true;
            controller.showFrame(live, {});
            expectations.expect(awaitQtCondition("the first Retained result arrives",
                                                 [&] {
                                                     return controller.reading().scopes != nullptr &&
                                                            controller.reading().scopes->identity.source ==
                                                                runtime::ScopeSource::Retained;
                                                 }),
                                "the first Retained result arrives");
            script->mode = Tap::Measured;
            script->holdRetry = false;
            script->holdRetry.notify_all();
            expectations.expect(awaitQtCondition("the Rendered tables replace the Retained ones", [&] {
                                    return settled() && controller.reading().scopes != nullptr &&
                                           controller.reading().scopes->identity.source ==
                                               runtime::ScopeSource::Rendered;
                                }),
                                "the Rendered tables replace the Retained ones");
            expectations.expect(
                script->calls.load() == 2 && script->renderedOnlyCalls.load() == 1 &&
                    controller.measuresStarted() == 2 &&
                    controller.reading().scopes->identity == identityIn(runtime::ScopeSource::Rendered, 0) &&
                    cache3->scopesFor(key, identityIn(runtime::ScopeSource::Rendered, 0)) ==
                        controller.reading().scopes && !controller.reading().behind &&
                    seen == std::vector<runtime::ScopeSource>{runtime::ScopeSource::Retained,
                                                              runtime::ScopeSource::Rendered},
                "Retained first, then Rendered once: two measures, and the source never flickers "
                "back");

            // A refusal that stays: the Retained tables stay, no further measure is made, the
            // reading stays Ready (nothing was lost).
            {
                auto fresh = std::make_shared<PreviewFrameCache>(std::size_t{1} << 28);
                ui::ScopesController second(scheduler, bridge, fresh, scripted);
                second.setSettings(settings);
                const auto plan4 = oneSolidPlan(kProjectId, document::Revision::fromRaw(22));
                const auto live4 = cpuFrame(plan4, 50, 0);
                retainedOf(fresh, live4);
                script->calls = 0;
                script->renderedOnlyCalls = 0;
                script->mode = Tap::Refused;
                second.showFrame(live4, {});
                expectations.expect(
                    awaitQtCondition("the refused frame settles",
                                     [&] {
                                         return second.reading().state ==
                                                    ScopesReading::State::Ready &&
                                                !second.busy();
                                     }),
                    "the refused frame settles");
                expectations.expect(
                    script->calls.load() == 2 && script->renderedOnlyCalls.load() == 1 &&
                        second.reading().scopes != nullptr &&
                        second.reading().scopes->identity.source == runtime::ScopeSource::Retained &&
                        second.reading().detail == 0U && !second.reading().behind,
                    "a tap refused twice leaves the Retained tables, Ready, after one retry");
                // A frame the Viewer's device holds no picture of is not retried.
                script->calls = 0;
                script->mode = Tap::Gone;
                const auto live5 = cpuFrame(plan4, 51, 1);
                retainedOf(fresh, live5);
                second.showFrame(live5, {});
                expectations.expect(
                    awaitQtCondition("the frame with no tap settles",
                                     [&] {
                                         return second.reading().state ==
                                                    ScopesReading::State::Ready &&
                                                !second.busy() && second.reading().measured &&
                                                second.reading().measured->identity.time ==
                                                    live5->desiredIdentity().time;
                                     }),
                    "the frame with no tap settles");
                expectations.expect(script->calls.load() == 1 &&
                                        second.reading().scopes->identity.source ==
                                            runtime::ScopeSource::Retained,
                                    "a frame with no tap is counted from its retained picture, once");
            }

            // Rendered live tables are never replaced by Retained ones: the full measure that
            // follows is a Rendered-only attempt, and when it cannot be made the live tables stay.
            {
                auto fresh = std::make_shared<PreviewFrameCache>(std::size_t{1} << 28);
                ui::ScopesController third(scheduler, bridge, fresh, scripted);
                third.setSettings(settings);
                const auto plan5 = oneSolidPlan(kProjectId, document::Revision::fromRaw(23));
                const auto live6 = cpuFrame(plan5, 60, 0);
                retainedOf(fresh, live6);
                script->calls = 0;
                script->renderedOnlyCalls = 0;
                script->mode = Tap::Measured;
                // The first call (live) measures; the follow-up finds the device busy.
                script->holdRetry = true;
                third.showFrame(live6, {});
                expectations.expect(
                    awaitQtCondition("the live tables are shown",
                                     [&] {
                                         return third.reading().state == ScopesReading::State::Ready &&
                                                third.reading().scopes != nullptr;
                                     }),
                    "the live tables are shown");
                script->mode = Tap::Refused;
                script->holdRetry = false;
                script->holdRetry.notify_all();
                expectations.expect(awaitQtCondition("the live frame settles",
                                                     [&] {
                                                         return !third.busy() &&
                                                                script->calls.load() == 2;
                                                     }),
                                    "the live frame settles");
                expectations.expect(
                    third.reading().scopes != nullptr &&
                        third.reading().scopes->identity.source == runtime::ScopeSource::Rendered &&
                        third.reading().detail == ui::kLiveDetail &&
                        script->renderedOnlyCalls.load() == 1,
                    "Rendered live tables stay when the Rendered full attempt cannot be made");
            }
        }

        // ---- Every flagged RAM preview fill frame is answered -------------------------------------
        {
            auto cache4 = std::make_shared<PreviewFrameCache>(std::size_t{1} << 28);
            const auto plan6 = oneSolidPlan(kProjectId, document::Revision::fromRaw(30));
            std::vector<std::shared_ptr<const PreparedPreviewFrame>> frames;
            for (std::int64_t index = 0; index < 16; ++index)
                frames.push_back(cpuFrame(plan6, 70 + static_cast<std::uint64_t>(index), index));
            expectations.expect(std::ranges::all_of(frames, [](const auto& f) { return f != nullptr; }),
                                "the fill cases' frames are built");
            if (!std::ranges::all_of(frames, [](const auto& f) { return f != nullptr; }))
                return 1;
            const auto scenesBefore = gSceneRuns.load();
            ui::ScopeFillTaps taps(nullptr, cache4, bridge);
            runtime::ScopeMeasureSettings settings;
            settings.scopes = render::kScopeTableHistogram;
            settings.signal = runtime::ScopeSignal::Display;
            const auto fillIdentity = ui::PreviewScopeIdentity{.settings = settings, .viewAdjust = {}};
            // A flagged submission: a real task the fill reads (or not), recorded by the owner.
            std::mutex releaseMutex;
            std::vector<std::shared_ptr<std::promise<void>>> releases;
            const auto flag = [&](const std::size_t index, const bool blocked = false) {
                auto release = std::make_shared<std::promise<void>>();
                auto future = std::make_shared<std::shared_future<void>>(release->get_future().share());
                runtime::TaskRequest request(
                    "Cache RAM preview frame",
                    {.kind = runtime::TaskOwnerKind::Composition,
                     .id = runtime::TaskOwnerId::fromRaw(kCompositionId.value())},
                    runtime::TaskPriority::Foreground);
                auto submission = scheduler.submit<runtime::PreviewPreparationResultHandle>(
                    std::move(request), [blocked, future](runtime::TaskContext&) {
                        if (blocked)
                            future->wait();
                        return runtime::TaskResult<runtime::PreviewPreparationResultHandle>::cancelled();
                    });
                if (!submission.accepted())
                    std::abort();
                taps.adoptSubmission(frames[index]->desiredIdentity(), submission.handle);
                {
                    const std::lock_guard lock(releaseMutex);
                    releases.push_back(release);
                }
                return release;
            };
            const auto answered = [&] { return taps.answeredByMeasure() + taps.answeredByDrop(); };
            const auto everyTapAnswered = [&] {
                return awaitQtCondition("every flagged frame is answered",
                                        [&] {
                                            return answered() == taps.flaggedSubmissions() &&
                                                   taps.pendingSubmissions() == 0 &&
                                                   taps.waitingTaps() == 0;
                                        });
            };
            const auto measureOf = [&](const std::size_t index) {
                return cache4->scopesFor(keyOf(frames[index]), fillIdentity);
            };
            ui::ScopesController controller(scheduler, bridge, cache4, measure, &taps);
            controller.setSettings(settings);
            expectations.expect(taps.wanted(), "a panel with a scope open wants the fill's taps");

            // Measured: a received tap is counted from its own process picture at full detail;
            // its tables go to the frame's entry, now (retained) or when it is retained.
            expectations.expect(cache4->insert(frames[0]).retained(), "the first fill frame is retained");
            static_cast<void>(flag(0));
            taps.received(frames[0]->desiredIdentity(), frames[0]);
            static_cast<void>(flag(1));
            taps.received(frames[1]->desiredIdentity(), frames[1]);
            expectations.expect(
                awaitQtCondition("the fill's first frame has its tables",
                                 [&] { return measureOf(0) != nullptr && !controller.busy(); }),
                "the fill's first frame has its tables");
            expectations.expect(measureOf(0) != nullptr && measureOf(0)->tables.counted() == 64U * 36U &&
                                    measureOf(0)->identity.source == runtime::ScopeSource::Rendered &&
                                    measureOf(0)->identity.settings.detail == 0U,
                                "a fill frame is counted Rendered at full detail");
            expectations.expect(measureOf(1) == nullptr && taps.keptTables() == 1,
                                "the tables of a frame not retained yet wait for it");
            expectations.expect(cache4->insert(frames[1]).retained() && measureOf(1) != nullptr &&
                                    taps.keptTables() == 0,
                                "they are attached when the frame is retained");
            expectations.expect(everyTapAnswered(), "the measured frames are answered");

            // The panel is hidden (or every scope collapsed, which is the same request: no scope
            // bit) while taps wait: they are dropped.
            {
                std::this_thread::yield();
                gate->hold(true, false);
                controller.showFrame(frames[15], {});
                expectations.expect(awaitQtCondition("a shown measure is held",
                                                     [&] { return controller.measuresInFlight() == 1; }),
                                    "a shown measure is held");
                static_cast<void>(flag(2));
                taps.received(frames[2]->desiredIdentity(), frames[2]);
                static_cast<void>(flag(3));
                taps.received(frames[3]->desiredIdentity(), frames[3]);
                expectations.expect(taps.waitingTaps() == 2,
                                    "taps wait behind the shown frame's measure");
                const auto dropped = taps.answeredByDrop();
                auto hidden = settings;
                hidden.scopes = 0;
                controller.setSettings(hidden);
                expectations.expect(!taps.wanted() && taps.waitingTaps() == 0 &&
                                        taps.answeredByDrop() == dropped + 2,
                                    "a hidden panel, or every scope collapsed, drops the waiting taps");
                gate->hold(false, false);
                controller.setSettings(settings);
                expectations.expect(awaitQtCondition("the shown frame settles", [&] { return !controller.busy(); }),
                                    "the shown frame settles");
                expectations.expect(everyTapAnswered(), "the hidden-panel taps are answered");
            }

            // Settings change while a tap's measure is in flight: it is cancelled and answered.
            {
                gate->hold(true, false);
                static_cast<void>(flag(4));
                taps.received(frames[4]->desiredIdentity(), frames[4]);
                expectations.expect(awaitQtCondition("the tap's measure is held",
                                                     [&] { return controller.measuresInFlight() == 1; }),
                                    "the tap's measure is held");
                auto changed = settings;
                changed.ignoreTransparent = true;
                controller.setSettings(changed);
                gate->hold(false, false);
                expectations.expect(everyTapAnswered() && measureOf(4) == nullptr &&
                                        cache4->scopesFor(keyOf(frames[4]),
                                                          ui::PreviewScopeIdentity{.settings = changed,
                                                                                   .viewAdjust = {}}) ==
                                            nullptr,
                                    "a tap measured under old settings is answered and its tables "
                                    "are neither shown nor cached");
                controller.setSettings(settings);
                expectations.expect(awaitQtCondition("the controller is idle again", [&] { return !controller.busy(); }),
                                    "the controller is idle again");
            }

            // A frame the cache does not retain: measured, answered, its tables forgotten when the
            // fill ends.
            {
                static_cast<void>(flag(5));
                taps.received(frames[5]->desiredIdentity(), frames[5]);
                expectations.expect(awaitQtCondition("the unretained frame is measured",
                                                     [&] { return taps.keptTables() == 1; }),
                                    "the unretained frame is measured");
                taps.fillEnded();
                expectations.expect(taps.keptTables() == 0 && !cache4->contains(keyOf(frames[5])) &&
                                        everyTapAnswered(),
                                    "its tables are forgotten when the fill ends and its tap is answered");
            }

            // A cancelled fill: the fill stops reading the task; the owner answers the tap when the
            // task has finished.
            {
                const auto release = flag(6, true);
                taps.orphan(frames[6]->desiredIdentity());
                expectations.expect(taps.pendingSubmissions() == 1,
                                    "an orphaned submission waits for its task to finish");
                release->set_value();
                expectations.expect(everyTapAnswered(), "an orphaned submission is answered");
            }

            // A frame that was not delivered holds nothing and is answered the same way.
            {
                static_cast<void>(flag(7));
                taps.received(frames[7]->desiredIdentity(), nullptr);
                expectations.expect(everyTapAnswered(), "a frame that was not delivered is answered");
            }

            // A second panel: the first one registered serves the fill's taps, the second does not.
            // When the serving panel goes away (hidden here), the second serves; a panel destroyed
            // while it holds a tap, in flight or waiting, lets it go.
            {
                auto doomed = std::make_unique<ui::ScopesController>(scheduler, bridge, cache4,
                                                                      measure, &taps);
                doomed->setSettings(settings);
                expectations.expect(cache4->insert(frames[8]).retained(), "the frame is retained");
                static_cast<void>(flag(8));
                taps.received(frames[8]->desiredIdentity(), frames[8]);
                expectations.expect(awaitQtCondition("the first panel serves the tap",
                                                     [&] { return measureOf(8) != nullptr; }),
                                    "the first panel serves the fill's taps");
                expectations.expect(doomed->measuresStarted() == 0,
                                    "a second panel does not count the fill's taps");
                auto hidden = settings;
                hidden.scopes = 0;
                controller.setSettings(hidden);
                gate->hold(true, false);
                static_cast<void>(flag(9));
                taps.received(frames[9]->desiredIdentity(), frames[9]);
                expectations.expect(awaitQtCondition("the second panel serves the tap once the "
                                                     "first is hidden",
                                                     [&] { return doomed->measuresInFlight() == 1; }),
                                    "the second panel serves the tap once the first is hidden");
                static_cast<void>(flag(10));
                taps.received(frames[10]->desiredIdentity(), frames[10]);
                expectations.expect(taps.waitingTaps() == 1, "a second tap waits behind it");
                doomed.reset();
                gate->hold(false, false);
                expectations.expect(everyTapAnswered(),
                                    "taps are answered when a panel is destroyed, in flight or waiting");
                controller.setSettings(settings);
                expectations.expect(awaitQtCondition("the first panel is idle again",
                                                     [&] { return !controller.busy(); }),
                                    "the first panel is idle again");
            }

            // Application shutdown: the taps the owner holds, received or still in flight, are
            // answered, and it detaches.
            {
                gate->hold(true, false);
                static_cast<void>(flag(11));
                taps.received(frames[11]->desiredIdentity(), frames[11]);
                expectations.expect(awaitQtCondition("a tap is held in flight",
                                                     [&] { return controller.measuresInFlight() == 1; }),
                                    "a tap is held in flight");
                static_cast<void>(flag(12));
                taps.received(frames[12]->desiredIdentity(), frames[12]);
                static_cast<void>(flag(13, true));
                expectations.expect(taps.waitingTaps() == 1 && taps.pendingSubmissions() == 1,
                                    "one tap waits and one submission is not read yet");
                gate->hold(false, false);
                taps.shutdown();
                expectations.expect(awaitQtCondition("the taps are answered at shutdown",
                                                     [&] {
                                                         return answered() == taps.flaggedSubmissions();
                                                     }) &&
                                        !taps.wanted() && taps.waitingTaps() == 0 &&
                                        taps.pendingSubmissions() == 0,
                                    "every tap is answered at shutdown");
                {
                    const std::lock_guard lock(releaseMutex);
                    for (const auto& release : releases)
                        try {
                            release->set_value();
                        } catch (const std::future_error&) {
                        }
                }
                const auto flagged = taps.flaggedSubmissions();
                taps.received(frames[14]->desiredIdentity(), frames[14]);
                expectations.expect(taps.flaggedSubmissions() == flagged && taps.waitingTaps() == 0,
                                    "nothing is taken after shutdown");
            }
            expectations.expect(answered() == taps.flaggedSubmissions() && taps.flaggedSubmissions() >= 13,
                                "every flagged frame was answered exactly once");
            expectations.expect(gSceneRuns.load() == scenesBefore,
                                "no fill measure evaluated a scene");
            (void)awaitQtCondition("the last measures end", [&] { return !controller.busy(); });
        }

        // A sidecar goes with its frame.
        expectations.expect(cache->scopeSidecarBytes() != 0, "sidecars are held");
        cache->clear();
        expectations.expect(cache->scopeSidecarBytes() == 0 && cache->hostBytes() == 0,
                            "clearing the cache drops the sidecars with the frames");

        // A sidecar that does not fit the budget is not kept; the frame stays.
        {
            PreviewFrameCache small(PreviewFrameCache::frameByteCost(*frame0) + 64);
            expectations.expect(small.insert(frame0).retained(), "the small cache holds a frame");
            auto sidecar = std::make_shared<ui::PreviewScopeSidecar>();
            sidecar->note = std::string(4096, 'x');
            expectations.expect(!small.attachScopes(keyOf(frame0), sidecar) &&
                                    small.contains(keyOf(frame0)) && small.scopeSidecarBytes() == 0,
                                "a sidecar with no room is not kept and evicts nothing of its own");
            expectations.expect(!small.attachScopes(keyOf(frame1), sidecar),
                                "a sidecar needs its frame's entry");
        }
        {
            // An older frame gives way to a sidecar as it does to a frame.
            const auto cost = PreviewFrameCache::frameByteCost(*frame0);
            PreviewFrameCache two(2 * cost + 64);
            expectations.expect(two.insert(frame0).retained() && two.insert(frame1).retained(),
                                "two frames fit");
            auto sidecar = std::make_shared<ui::PreviewScopeSidecar>();
            sidecar->note = std::string(4096, 'x');
            expectations.expect(two.attachScopes(keyOf(frame1), sidecar) &&
                                    two.contains(keyOf(frame1)) && !two.contains(keyOf(frame0)) &&
                                    two.scopeSidecarBytes() == sidecar->byteSize(),
                                "the least recently used frame is evicted for a sidecar");
        }

        bridge.beginShutdown();
        (void)awaitQtCondition("the scheduler drains after shutdown",
                               [&] { return scheduler.isQuiescent(); });
        if (expectations.failures() + awaitFailureCount() != 0)
            return 1;
        std::cout << "PASS: scopes controller\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
