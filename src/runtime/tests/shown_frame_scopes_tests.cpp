// #733 Scopes: the Display signal tables of a shown frame, counted at display time on a worker
// (the route without a device).
//
// The frames are stored display frames built here (generated cells of both layouts), so nothing is
// rendered and no device is used. What is asserted:
//   (1) a shown frame's tables are the CPU reference's, exactly, at the full size and at a reduced
//       size, for both layouts; no native work is reported and the route is named Cpu;
//   (2) reduced rate: with the worker's count held by the injected step, six shown frames return
//       at once to their caller, give two measures (the one in flight and the newest), and the
//       result is the last frame's, equal to the CPU reference;
//   (3) clear(): the held measure and the waiting request give no result, and a later frame is
//       counted;
//   (4) a region outside the picture settles EmptyArea; the Working signal is refused;
//   (5) a measurer destroyed while its measure is held is never called back.

#include "await.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/shown_frame_scopes.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using bloom::test_support::awaitCondition;
using render::DisplayPixelLayout;
using runtime::PreviewFrameScopesStatus;
using runtime::ScopeFrameStatus;
using runtime::ScopeMeasureSettings;
using runtime::ScopeRoute;
using runtime::ScopeSignal;
using runtime::ScopeSignalColor;
using runtime::ScopeStoredFrame;
using runtime::ShownFrameScopeMeasurer;
using runtime::ShownFrameScopeRequest;

int failures = 0;

void check(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr std::uint32_t kWidth = 48;
constexpr std::uint32_t kHeight = 30;

using Cells = std::shared_ptr<const std::vector<render::Rgba8>>;

// Generated cells; `seed` makes frames differ.
[[nodiscard]] Cells makeCells(const DisplayPixelLayout layout, const std::uint32_t seed) {
    std::vector<render::Rgba8> cells;
    cells.reserve(std::size_t{kWidth} * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const std::uint32_t i = y * kWidth + x + seed * 97U;
            if (layout == DisplayPixelLayout::Rgb10A2) {
                const bool transparent = i % 13U == 5U;
                cells.push_back(transparent
                                    ? render::displayCell(0U)
                                    : render::rgb10A2Cell({(i * 3U) % 1024U, (i * 7U + 5U) % 1024U,
                                                           1023U - (i * 5U) % 1024U,
                                                           render::kDisplayAlpha2Opaque}));
            } else {
                const auto alpha = static_cast<std::uint8_t>(
                    i % 5U == 0U ? 255U : (i % 5U == 1U ? 0U : (i * 3U) % 256U));
                cells.push_back(render::Rgba8{static_cast<std::uint8_t>(i % 256U),
                                              static_cast<std::uint8_t>((i * 5U + 1U) % 256U),
                                              static_cast<std::uint8_t>(255U - i % 256U), alpha});
            }
        }
    }
    return std::make_shared<const std::vector<render::Rgba8>>(std::move(cells));
}

[[nodiscard]] ScopeStoredFrame storedOf(const Cells& cells, const DisplayPixelLayout layout) {
    const auto window = render::ImageWindow::create(0, 0, kWidth, kHeight);
    if (!window)
        throw std::logic_error("fixture window");
    return ScopeStoredFrame{.cells = std::span<const render::Rgba8>(*cells),
                            .layout = layout,
                            .dataWindow = *window.value(),
                            .displayWindow = *window.value(),
                            .pixelAspect = core::PixelAspectRatio::square(),
                            .roi = std::nullopt,
                            .compositionWidth = kWidth,
                            .compositionHeight = kHeight};
}

[[nodiscard]] runtime::TaskSchedulerConfig schedulerConfig() {
    runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 2;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    config.blockingIoWorkerCount = 1;
    config.cpuQueueCapacity = 64;
    config.blockingIoQueueCapacity = 8;
    config.terminalHistoryCapacity = 64;
    config.diagnosticsPerTask = 16;
    config.groupRegistryCapacity = 16;
    return config;
}

// The injected step of the worker's count: closed, it holds the worker until opened.
class Gate final {
  public:
    void close() {
        const std::lock_guard lock(mutex_);
        open_ = false;
        entered_ = 0;
    }
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }
    void pass() {
        std::unique_lock lock(mutex_);
        ++entered_;
        condition_.wait(lock, [this] { return open_; });
    }
    [[nodiscard]] int entered() const {
        const std::lock_guard lock(mutex_);
        return entered_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ = true;
    int entered_ = 0;
};

struct Rig final {
    runtime::TaskScheduler scheduler{schedulerConfig()};
    std::shared_ptr<const ScopeSignalColor> color;
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::shared_ptr<std::atomic<int>> ready = std::make_shared<std::atomic<int>>(0);
    std::unique_ptr<ShownFrameScopeMeasurer> measurer;

    explicit Rig(std::shared_ptr<const ScopeSignalColor> colorValue)
        : color(std::move(colorValue)) {
        ShownFrameScopeMeasurer::Options options;
        options.scheduler = &scheduler;
        options.owner = {.kind = runtime::TaskOwnerKind::PanelRequest,
                         .id = runtime::TaskOwnerId::fromRaw(1)};
        options.colors = std::make_shared<runtime::ScopeSignalColorCache>();
        options.resultReady = [count = ready] { count->fetch_add(1); };
        options.beforeCpuCount = [held = gate] { held->pass(); };
        measurer = std::make_unique<ShownFrameScopeMeasurer>(std::move(options));
    }

    [[nodiscard]] ShownFrameScopeRequest request(const Cells& cells,
                                                 const DisplayPixelLayout layout,
                                                 const ScopeMeasureSettings& settings,
                                                 const render::ScopeTableReduction reduction,
                                                 const std::uint64_t tag) const {
        ShownFrameScopeRequest request;
        request.stored = storedOf(cells, layout);
        request.storedOwner = cells;
        request.storedColor = color;
        request.settings = settings;
        request.reduction = reduction;
        request.tag = tag;
        return request;
    }

    [[nodiscard]] bool idle(const std::string& what) const {
        return awaitCondition(what, [this] { return !measurer->busy(); });
    }
};

[[nodiscard]] ScopeMeasureSettings displaySettings(const std::uint32_t scopes) {
    ScopeMeasureSettings settings;
    settings.signal = ScopeSignal::Display;
    settings.scopes = scopes;
    return settings;
}

void testPausedFrameIsExact(const std::shared_ptr<const ScopeSignalColor>& color) {
    Rig rig(color);
    std::uint64_t tag = 1;
    for (const auto layout : {DisplayPixelLayout::Straight8888, DisplayPixelLayout::Rgb10A2}) {
        const std::string name =
            layout == DisplayPixelLayout::Rgb10A2 ? "10:10:10:2" : "8:8:8:8";
        const auto cells = makeCells(layout, 1);
        for (const auto reduction :
             {render::ScopeTableReduction{}, render::ScopeTableReduction{2, 1, 1, 1}}) {
            for (const bool ignoreTransparent : {false, true}) {
                auto settings = displaySettings(render::kScopeTableAll);
                settings.ignoreTransparent = ignoreTransparent;
                const auto expected = runtime::measureScopeStoredFrameCpu(
                    storedOf(cells, layout), settings, reduction, *color);
                check(expected.status == ScopeFrameStatus::Measured,
                      name + ": the CPU reference measures the stored frame");
                rig.measurer->show(rig.request(cells, layout, settings, reduction, tag));
                if (!rig.idle(name + ": the shown frame is counted"))
                    return;
                const auto result = rig.measurer->take();
                check(result.has_value() && result->tag == tag && result->scopes != nullptr &&
                          result->scopes->status == PreviewFrameScopesStatus::Measured,
                      name + ": a result with tables");
                if (!result || result->scopes == nullptr)
                    return;
                const auto& scopes = *result->scopes;
                check(scopes.tables.route() == ScopeRoute::Cpu && result->uploads == 0 &&
                          result->tableReadbacks == 0 && scopes.tableReadbackBytes == 0,
                      name + ": counted on the worker, no native work");
                check(scopes.tables.unpack() == expected.counts && scopes.area == expected.area &&
                          scopes.settings == settings && result->reduction == reduction,
                      name + ": the tables equal the CPU reference's");
                check(scopes.tables.layout().reduction == reduction,
                      name + ": the tables have the asked size");
                check(scopes.color == color, name + ": the result carries the color objects");
                ++tag;
            }
        }
    }
    check(rig.ready->load() == static_cast<int>(tag - 1),
          "one result notification for every counted frame");
}

void testReducedRate(const std::shared_ptr<const ScopeSignalColor>& color) {
    Rig rig(color);
    const auto settings = displaySettings(render::kScopeTableHistogram | render::kScopeTableWaveform);
    constexpr std::uint64_t kFrames = 6;
    std::vector<Cells> frames;
    for (std::uint64_t index = 0; index < kFrames; ++index)
        frames.push_back(makeCells(DisplayPixelLayout::Rgb10A2, static_cast<std::uint32_t>(index)));
    rig.gate->close();
    rig.measurer->show(rig.request(frames[0], DisplayPixelLayout::Rgb10A2, settings, {}, 1));
    if (!awaitCondition("the first frame's count is held",
                        [&] { return rig.gate->entered() == 1; }))
        return;
    // The caller is never blocked: every show() returns while the worker is held.
    for (std::uint64_t index = 1; index < kFrames; ++index)
        rig.measurer->show(
            rig.request(frames[index], DisplayPixelLayout::Rgb10A2, settings, {}, index + 1));
    const auto held = rig.measurer->counters();
    check(held.requested == kFrames && held.replaced == kFrames - 2 && held.cpuMeasures == 0 &&
              rig.measurer->busy() && !rig.measurer->take().has_value(),
          "six frames were shown while the first count was held: four gave way to a newer one");
    rig.gate->open();
    if (!rig.idle("the held and the newest frame are counted"))
        return;
    const auto after = rig.measurer->counters();
    check(after.cpuMeasures == 2 && after.cpuMeasures < kFrames && after.deviceMeasures == 0,
          "two measures for six shown frames");
    const auto result = rig.measurer->take();
    const auto expected = runtime::measureScopeStoredFrameCpu(
        storedOf(frames[kFrames - 1], DisplayPixelLayout::Rgb10A2), settings, {}, *color);
    check(result.has_value() && result->tag == kFrames && result->scopes != nullptr &&
              result->scopes->status == PreviewFrameScopesStatus::Measured &&
              expected.status == ScopeFrameStatus::Measured &&
              result->scopes->tables.unpack() == expected.counts,
          "the result is the last shown frame's, equal to the CPU reference");
    check(!rig.measurer->take().has_value(), "a result is taken once");
}

void testClear(const std::shared_ptr<const ScopeSignalColor>& color) {
    Rig rig(color);
    const auto settings = displaySettings(render::kScopeTableHistogram);
    const auto first = makeCells(DisplayPixelLayout::Straight8888, 3);
    const auto second = makeCells(DisplayPixelLayout::Straight8888, 4);
    rig.gate->close();
    rig.measurer->show(rig.request(first, DisplayPixelLayout::Straight8888, settings, {}, 1));
    if (!awaitCondition("the count is held", [&] { return rig.gate->entered() == 1; }))
        return;
    rig.measurer->show(rig.request(second, DisplayPixelLayout::Straight8888, settings, {}, 2));
    rig.measurer->clear();
    rig.gate->open();
    if (!rig.idle("the cleared measure ends"))
        return;
    check(!rig.measurer->take().has_value() && rig.ready->load() == 0 &&
              rig.measurer->counters().cpuMeasures == 0,
          "clear(): neither the held measure nor the waiting request gives a result");
    rig.measurer->show(rig.request(second, DisplayPixelLayout::Straight8888, settings, {}, 3));
    if (!rig.idle("a frame shown after clear() is counted"))
        return;
    const auto result = rig.measurer->take();
    check(result.has_value() && result->tag == 3 && result->scopes != nullptr &&
              result->scopes->status == PreviewFrameScopesStatus::Measured,
          "a frame shown after clear() is counted");
}

void testAreaAndSignal(const std::shared_ptr<const ScopeSignalColor>& color) {
    Rig rig(color);
    const auto cells = makeCells(DisplayPixelLayout::Rgb10A2, 7);
    auto settings = displaySettings(render::kScopeTableHistogram);
    settings.region = runtime::ScopeRegion{.x = 8, .y = 4, .width = 20, .height = 10};
    rig.measurer->show(rig.request(cells, DisplayPixelLayout::Rgb10A2, settings, {}, 1));
    if (!rig.idle("a region is counted"))
        return;
    auto result = rig.measurer->take();
    const auto expected = runtime::measureScopeStoredFrameCpu(
        storedOf(cells, DisplayPixelLayout::Rgb10A2), settings, {}, *color);
    check(result.has_value() && result->scopes != nullptr &&
              result->scopes->status == PreviewFrameScopesStatus::Measured &&
              result->scopes->tables.unpack() == expected.counts &&
              result->scopes->tables.counted() == 20U * 10U,
          "a region: the tables are the region's");

    settings.region = runtime::ScopeRegion{.x = 400, .y = 4, .width = 20, .height = 10};
    rig.measurer->show(rig.request(cells, DisplayPixelLayout::Rgb10A2, settings, {}, 2));
    if (!rig.idle("a region outside the picture settles"))
        return;
    result = rig.measurer->take();
    check(result.has_value() && result->scopes != nullptr &&
              result->scopes->status == PreviewFrameScopesStatus::EmptyArea,
          "a region outside the picture settles EmptyArea");

    auto working = displaySettings(render::kScopeTableHistogram);
    working.signal = ScopeSignal::Working;
    rig.measurer->show(rig.request(cells, DisplayPixelLayout::Rgb10A2, working, {}, 3));
    if (!rig.idle("the Working signal is refused"))
        return;
    result = rig.measurer->take();
    check(result.has_value() && result->scopes != nullptr &&
              result->scopes->status == PreviewFrameScopesStatus::Failed &&
              !result->scopes->message.empty(),
          "the Working signal is not a shown frame's: Failed with the reason");
}

void testDestroyedWhileHeld(const std::shared_ptr<const ScopeSignalColor>& color) {
    Rig rig(color);
    const auto cells = makeCells(DisplayPixelLayout::Straight8888, 9);
    rig.gate->close();
    rig.measurer->show(rig.request(cells, DisplayPixelLayout::Straight8888,
                                   displaySettings(render::kScopeTableHistogram), {}, 1));
    if (!awaitCondition("the count is held", [&] { return rig.gate->entered() == 1; }))
        return;
    rig.measurer.reset();
    rig.gate->open();
    if (!awaitCondition("the held worker ends", [&] { return rig.scheduler.isQuiescent(); }))
        return;
    check(rig.ready->load() == 0, "a destroyed measurer is never called back");
}

int runAll() {
    auto config =
        color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn,
                                  color::kBloomNeutralV1ConfigUri,
                                  color::ocioBuiltInContentRevision(
                                      color::OcioConfigLocatorKind::BloomBuiltIn,
                                      color::kBloomNeutralV1ConfigUri)
                                      .value_or(color::kBloomNeutralV1ConfigDigest),
                                  "lin_rec709_scene")
            .takeResolved();
    if (!config) {
        std::cerr << "FAIL: the built-in color config did not resolve\n";
        return 1;
    }
    auto prepared = ScopeSignalColor::prepare(
        {.config = std::make_shared<const color::ResolvedBloomNeutralConfig>(std::move(*config)),
         .signal = ScopeSignal::Display,
         .display = {},
         .view = {}});
    if (!prepared) {
        std::cerr << "FAIL: the Display signal's color objects did not prepare: "
                  << prepared.message << '\n';
        return 1;
    }
    testPausedFrameIsExact(prepared.color);
    testReducedRate(prepared.color);
    testClear(prepared.color);
    testAreaAndSignal(prepared.color);
    testDestroyedWhileHeld(prepared.color);
    if (failures + test_support::awaitFailureCount() != 0)
        return 1;
    std::cout << "shown frame scopes tests passed\n";
    return 0;
}

} // namespace

int main() {
    try {
        return runAll();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
