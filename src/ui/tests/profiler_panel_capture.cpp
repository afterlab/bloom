// Captures of the Profiler panel (#423) for visual review: over the deterministic fake feed, a
// wide and a narrow panel after five minutes of history; over the real runtime sampler in an
// app-shaped setup (a scheduler with running and queued work, a ledger with a named, counted
// reusable pool, an admission policy, the GPU image counters, memory that grows while it samples),
// a wide live panel; and a panel with no
// source. Writes
// <dir>/profiler-<state>-dpr<scale>.png, where <dir> is BLOOM_PROFILER_CAPTURE_DIR (or the first
// argument) and the scale follows QT_SCALE_FACTOR. Without either it captures into a temporary
// directory and only checks that each capture rendered.
#include "await_qt.hpp"
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/memory_pool_counters.hpp>
#include <bloom/runtime/profiler_sampler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/profiler_editor.hpp>
#include <bloom/ui/profiler_fake_feed.hpp>
#include <bloom/ui/profiler_history.hpp>
#include <bloom/ui/profiler_runtime_feed.hpp>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

namespace ui = bloom::ui;

// The panel's whole content at its natural height for `width`, so one image shows every section.
QImage grabWhole(ui::ProfilerEditor& panel, const int width) {
    auto* content = panel.findChild<QWidget*>(QStringLiteral("profilerContent"));
    panel.resize(width, content != nullptr ? content->sizeHint().height() : width);
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    if (content != nullptr)
        panel.resize(width, content->heightForWidth(width) > 0 ? content->heightForWidth(width)
                                                               : content->sizeHint().height());
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    return panel.grab().toImage();
}

int save(const QImage& image, const QString& directory, const QString& name, const QString& scale) {
    const QString path = directory + QStringLiteral("/profiler-%1-dpr%2.png").arg(name, scale);
    if (image.isNull() || image.width() == 0 || !image.save(path)) {
        std::cerr << "cannot write " << path.toStdString() << '\n';
        return 1;
    }
    std::cout << path.toStdString() << '\n';
    return 0;
}

int captureWithHistory(const QString& directory, const QString& scale, const QString& name,
                       const int width) {
    ui::FakeProfilerFeed feed;
    ui::ProfilerEditor panel(&feed);
    panel.resize(width, width);
    panel.show();
    // Five minutes of history ending on a stalled ledger sample during an export, so the capture
    // shows full rings, the eviction drop, stale ages and the absent fields.
    feed.setNextSerial(1 + 2 * 240 + 175 - ui::ProfilerHistory::kCapacity);
    for (int sample = 0; sample < ui::ProfilerHistory::kCapacity; ++sample)
        (void)feed.pump();
    return save(grabWhole(panel, width), directory, name, scale);
}

// The real sampler at 10 Hz for about six seconds while work runs and memory grows.
int captureLive(const QString& directory, const QString& scale) {
    namespace runtime = bloom::runtime;
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 2;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    std::atomic<bool> released{false};
    const auto work = [&released](runtime::TaskContext& context) {
        for (std::uint64_t step = 0; !released.load() && !context.isCancellationRequested();
             ++step) {
            context.reportProgress(
                {.phase = "Render", .subphase = {}, .completed = step % 100, .total = 100});
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return runtime::TaskResult<void>::succeeded();
    };
    const auto owner = [](const runtime::TaskOwnerKind kind, const std::uint64_t id) {
        return runtime::TaskOwner{.kind = kind, .id = runtime::TaskOwnerId::fromRaw(id)};
    };
    std::vector<runtime::TaskHandle<void>> handles;
    for (const auto& [name, kind, executor] :
         {std::tuple{"Viewer frame 0412", runtime::TaskOwnerKind::Composition,
                     runtime::TaskExecutor::Cpu},
          std::tuple{"RAM Preview fill", runtime::TaskOwnerKind::Composition,
                     runtime::TaskExecutor::Cpu},
          std::tuple{"Decode clip_A012.mov", runtime::TaskOwnerKind::Project,
                     runtime::TaskExecutor::BlockingIo},
          std::tuple{"Export frame 0098", runtime::TaskOwnerKind::Export,
                     runtime::TaskExecutor::Cpu},
          std::tuple{"Thumbnail strip", runtime::TaskOwnerKind::PanelRequest,
                     runtime::TaskExecutor::Cpu}})
        handles.push_back(
            scheduler
                .submit<void>(runtime::TaskRequest(name, owner(kind, 1),
                                                   runtime::TaskPriority::Visible, executor),
                              work)
                .handle);

    runtime::MemoryBudgetLedger ledger(runtime::physicalMemoryBytes());
    ledger.setRamHeadroom(runtime::physicalMemoryBytes() / 10);
    std::atomic<std::size_t> cached{0};
    const int cacheOwner = 0;
    ledger.registerCache(
        &cacheOwner, std::size_t{2} << 30U,
        [&cached] {
            return runtime::MemoryPoolHolding{
                .hostBytes = cached.load(), .hostPendingRetirementBytes = 0, .deviceBytes = 0};
        },
        [](std::size_t) {});
    runtime::MemoryPoolCounters counters;
    ledger.setPoolProfile(&cacheOwner, {.name = "Preview frames",
                                        .kind = runtime::ProfilerPoolKind::Cache,
                                        .counters = &counters,
                                        .rows = {},
                                        .entries = {}});
    runtime::GpuProcessingPhasePolicy admission;

    int failures = 0;
    {
        runtime::ProfilerSampler sampler(
            {.scheduler = &scheduler, .readGpu = &ui::readProfilerGpuImageStats});
        ui::ProfilerUiSources sources;
        sources.ledger = &ledger;
        sources.admission = &admission;
        ui::RuntimeProfilerFeed feed(sampler, sources);
        feed.setInterval(std::chrono::milliseconds(100));
        ui::ProfilerEditor panel(&feed);
        panel.resize(960, 960);
        panel.show();
        // Grow a "cache" of touched pages and some unattributed memory while sampling.
        std::vector<std::unique_ptr<char[]>> blocks;
        constexpr std::size_t kBlock = std::size_t{8} << 20U;
        for (int tick = 0; tick < 60; ++tick) {
            auto block = std::make_unique<char[]>(kBlock);
            std::memset(block.get(), tick + 1, kBlock);
            blocks.push_back(std::move(block));
            if (tick % 3 != 0) {
                cached.fetch_add(kBlock);
                counters.store();
            } else {
                counters.miss();
            }
            if (tick % 20 == 19) {
                cached.fetch_sub(kBlock);
                counters.evicted(kBlock);
            }
            counters.hit();
            QTest::qWait(100);
        }
        if (!sampler.running() || panel.current().serial < 30) {
            std::cerr << "the live sampler did not deliver\n";
            ++failures;
        }
        failures += save(grabWhole(panel, 960), directory, QStringLiteral("live-wide"), scale);
    }
    released.store(true);
    ledger.unregisterCache(&cacheOwner);
    scheduler.beginShutdown();
    if (!bloom::test_support::awaitCondition("scheduler quiescent",
                                             [&] { return scheduler.isQuiescent(); }))
        ++failures;
    return failures;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("BloomProfilerPanelCapture"));
    application.setApplicationName(QStringLiteral("ProfilerPanelCapture"));
    ui::kit::installAfterlabTheme(application);
    QTemporaryDir scratch;
    QString directory = qEnvironmentVariable("BLOOM_PROFILER_CAPTURE_DIR");
    if (directory.isEmpty() && argc > 1)
        directory = QString::fromLocal8Bit(argv[1]);
    if (directory.isEmpty())
        directory = scratch.path();
    QDir().mkpath(directory);
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).remove('.');
    int failures = 0;
    failures += captureWithHistory(directory, scale, QStringLiteral("wide"), 960);
    failures += captureWithHistory(directory, scale, QStringLiteral("narrow"), 380);
    failures += captureLive(directory, scale);
    {
        ui::ProfilerEditor panel(nullptr);
        panel.show();
        failures += save(grabWhole(panel, 760), directory, QStringLiteral("no-source"), scale);
    }
    return failures;
}
