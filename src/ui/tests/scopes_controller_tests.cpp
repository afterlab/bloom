// #724 Scopes: the controller behind the Scopes panel.
//
// Genuine CPU reference frames (the real CpuCompositionEvaluator and display preparer) are shown
// to a ScopesController whose measure is the production one over the CPU route. Asserted:
//   * nothing is measured without a frame or without a scope asked for;
//   * a shown frame is measured on a task worker, never on the interface thread, and its tables
//     are attached to the frame's cache entry and charged to the cache's byte budget;
//   * a frame with a sidecar measured under the same settings is not measured again;
//   * a changed scope setting measures again without touching the frame's cache entry;
//   * while a new frame is measured, the earlier frame's tables are on show and labeled behind;
//   * a superseded measure is cancelled and its result is neither shown nor cached;
//   * during a RAM preview fill the frames the cache retains are measured in the background;
//   * a sidecar goes with its frame, and one that does not fit the budget is not kept.
// No device is used.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>

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

// Runs the event loop until `predicate` holds; the bound only ends a run that cannot succeed.
template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 60000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

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

[[nodiscard]] std::shared_ptr<const PreparedPreviewFrame>
cpuFrame(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const std::uint64_t generation, const std::int64_t time = 0) {
    const runtime::CpuCompositionEvaluator evaluator;
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

        const runtime::CpuCompositionEvaluator cpuEvaluator;
        runtime::ScopeFrameRoutes routes;
        routes.evaluateCpu = [&cpuEvaluator](auto plan, const runtime::EvaluationRequest& request,
                                             const runtime::CancellationToken& cancellation) {
            return cpuEvaluator.evaluate(std::move(plan), request, cancellation);
        };
        const auto production = ui::makeScopesMeasure(routes, {}, std::size_t{1} << 26);
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
            [production, gate, interfaceThread](const ui::PreviewScopeSource& source,
                                                const ui::PreviewScopeIdentity& identity,
                                                const runtime::CancellationToken& cancellation) {
                if (std::this_thread::get_id() == interfaceThread)
                    gate->onInterfaceThread = true;
                gate->enter(source.identity.time.toSeconds(), identity.settings.detail);
                const auto result = production(source, identity, cancellation);
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
            const auto identityOf = [&](const runtime::ScopeMeasureSettings& value) {
                return ui::PreviewScopeIdentity{.settings = value, .viewAdjust = {}};
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
            expectations.expect(waitUntil(settled),
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
            expectations.expect(waitUntil(settled), "the frame is measured under the new setting");
            expectations.expect(controller.measuresStarted() == 4 &&
                                    cache->contains(keyOf(frame0)) && cache->size() == 2 &&
                                    cache->scopesFor(keyOf(frame0), identityOf(settings)) ==
                                        nullptr &&
                                    cache->scopesFor(keyOf(frame0), identityOf(ignoring)) ==
                                        controller.reading().scopes,
                                "the sidecar is replaced and the frame's entry is untouched");
            controller.setSettings(settings);
            expectations.expect(waitUntil(settled), "the first setting is measured again");
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
            expectations.expect(waitUntil(settled), "the new frame's scopes arrive");
            expectations.expect(
                !controller.reading().behind && controller.reading().scopes != current &&
                    controller.reading().measured &&
                    keyOf(frame1) ==
                        PreviewFrameCacheKey::forIdentity(controller.reading().measured->identity),
                "the reading is the shown frame's");

            // The Viewer's exposure is part of the Display measure, and not of the Working one.
            const auto beforeAdjust = controller.measuresStarted();
            controller.showFrame(frame1, runtime::ViewAdjust{.exposure = -1.0, .gamma = 1.0});
            expectations.expect(waitUntil(settled) &&
                                    controller.measuresStarted() == beforeAdjust + 2,
                                "an exposure measures the Display signal again");
            expectations.expect(
                controller.reading().scopes != nullptr &&
                    controller.reading().scopes->tables.unpack().histogram(0, 1023) == 0,
                "the exposure darkens the measured signal");
            controller.showFrame(frame1, {});
            expectations.expect(waitUntil(settled), "the neutral frame is measured again");

            // A superseded measure is cancelled; its result is neither shown nor cached.
            expectations.expect(cache->insert(frame2).retained() &&
                                    cache->insert(frame3).retained(),
                                "two more frames are cached");
            gate->hold(true, false);
            const auto enteredBefore = gate->entered.load();
            controller.showFrame(frame2, {});
            expectations.expect(waitUntil([&] { return gate->entered.load() > enteredBefore; }),
                                "the held measure started");
            controller.showFrame(frame3, {});
            gate->hold(false, false);
            expectations.expect(waitUntil([&] {
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
                    expectations.expect(waitUntil([&] { return gate->entered.load() > base; }),
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
                    expectations.expect(waitUntil([&] { return gate->entered.load() >= base + 3; }),
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
                    expectations.expect(waitUntil(settled), "the newest frame ends at full detail");
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
                expectations.expect(waitUntil(settled),
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
                        return ui::PreviewScopeSource{frame->desiredIdentity(),
                                                      frame->processIdentity(),
                                                      frame->provenance().provider};
                    };
                    // A frame shown and left alone: the live measure, then the full one.
                    auto base = gate->entered.load();
                    auto started = controller.measuresStarted();
                    gate->hold(false, true);
                    controller.showFrame(stay, {});
                    expectations.expect(waitUntil([&] { return gate->entered.load() >= base + 2; }),
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
                    expectations.expect(waitUntil(settled), "the frame ends at full detail");
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
                    expectations.expect(waitUntil([&] { return gate->entered.load() > base; }),
                                        "the live measure of the frame to be replaced is held");
                    controller.showFrame(replacement, {});
                    gate->hold(false, false);
                    expectations.expect(waitUntil(settled), "the replacement settles");
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
                    expectations.expect(waitUntil([&] { return gate->entered.load() > base; }),
                                        "the full measure of the live sidecar's frame started");
                    gate->hold(false, false);
                    expectations.expect(waitUntil(settled), "the live sidecar's frame ends at full "
                                                            "detail");
                    const auto cachedCalls = gate->callsSince(static_cast<std::size_t>(base));
                    expectations.expect(cachedCalls.size() == 1 && cachedCalls[0].detail == 0U &&
                                            controller.measuresStarted() == started + 1 &&
                                            controller.reading().detail == 0U,
                                        "one full measure follows a live sidecar");
                }
                controller.setSettings(settings);
                expectations.expect(waitUntil(settled), "the first settings are measured again");
            }

            // A RAM preview fill measures the frames the cache retains.
            const auto frame4 = cpuFrame(plan, 5, 4);
            const auto frame5 = cpuFrame(plan, 6, 5);
            expectations.expect(frame4 && frame5, "the fill's frames are built");
            if (frame4 && frame5) {
                controller.setRangeFillActive(true);
                expectations.expect(cache->insert(frame4).retained() &&
                                        cache->insert(frame5).retained(),
                                    "the fill's frames are cached");
                expectations.expect(
                    waitUntil([&] {
                        return cache->scopesFor(keyOf(frame4), identityOf(settings)) != nullptr &&
                               cache->scopesFor(keyOf(frame5), identityOf(settings)) != nullptr &&
                               !controller.busy();
                    }),
                    "the retained frames are measured in the background");
                controller.setRangeFillActive(false);
                const auto started = controller.measuresStarted();
                controller.showFrame(frame4, {});
                expectations.expect(controller.reading().state == ScopesReading::State::Ready &&
                                        controller.measuresStarted() == started &&
                                        controller.reading().scopes ==
                                            cache->scopesFor(keyOf(frame4), identityOf(settings)),
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
                    expectations.expect(waitUntil(settled), "the edited frame is measured");
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
        for (int turn = 0; turn < 2000 && !scheduler.isQuiescent(); ++turn)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (expectations.failures() != 0)
            return 1;
        std::cout << "PASS: scopes controller\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
