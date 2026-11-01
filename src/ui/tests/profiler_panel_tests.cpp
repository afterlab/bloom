// The Profiler panel (#423): registration and switcher glyph, feed subscription on show/hide/close
// with a refcount across panels, fixed history rings that wrap at 600 without allocating, absent
// versus zero, stale ages, the unattributed math, and the snapshot's trivial copyability.
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/profiler_sampler.hpp>
#include <bloom/runtime/profiler_snapshot.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/chart.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/panel_switcher.hpp>
#include <bloom/ui/kit/readout.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/profiler_editor.hpp>
#include <bloom/ui/profiler_fake_feed.hpp>
#include <bloom/ui/profiler_history.hpp>
#include <bloom/ui/profiler_runtime_feed.hpp>

#include <QAbstractItemModel>
#include <QApplication>
#include <QImage>
#include <QTableView>
#include <QTest>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <source_location>
#include <string>
#include <type_traits>

// ---- Allocation counting: the global operator new, counted only while `gCounting` is set. -----
namespace {
std::atomic<bool> gCounting{false};
std::atomic<long> gAllocations{0};
} // namespace

void* operator new(std::size_t size) {
    if (gCounting.load(std::memory_order_relaxed))
        gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(size == 0 ? 1 : size))
        return block;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }

namespace {

namespace runtime = bloom::runtime;
namespace ui = bloom::ui;
namespace kit = bloom::ui::kit;
using namespace bloom::ui;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class CountingSubscriber final : public ProfilerFeedSubscriber {
  public:
    void profilerSnapshotArrived(const runtime::ProfilerSnapshot& snapshot) override {
        ++deliveries;
        lastSerial = snapshot.serial;
    }
    long deliveries = 0;
    std::uint64_t lastSerial = 0;
};

static_assert(std::is_trivially_copyable_v<runtime::ProfilerSnapshot>);
static_assert(std::is_trivially_copyable_v<runtime::ProfilerPool>);
static_assert(std::is_trivially_copyable_v<runtime::ProfilerMetric<runtime::ProfilerHistogram>>);

template <typename Widget> Widget* child(QWidget& parent, const char* name) {
    return parent.findChild<Widget*>(QString::fromLatin1(name));
}

void testRegistrationAndSwitcherIcon(Expectations& expectations) {
    FakeProfilerFeed feed;
    EditorRegistry registry;
    expectations.expect(registerProfilerEditor(registry, &feed), "the Profiler registers");
    expectations.expect(!registerProfilerEditor(registry, &feed),
                        "a second bloom.profiler is refused as a duplicate");
    const auto& editors = registry.editors();
    expectations.expect(editors.size() == 1 && editors.front().id == "bloom.profiler" &&
                            editors.front().displayName == QStringLiteral("Profiler"),
                        "the editor is bloom.profiler, named Profiler");

    EditorArea area(registry, "bloom.profiler", QString{});
    auto* picker = child<kit::KPanelSwitcher>(area, "editorTypePicker");
    expectations.expect(picker != nullptr, "the area has a panel switcher");
    if (picker == nullptr)
        return;
    const int index = picker->findData(QStringLiteral("bloom.profiler"));
    expectations.expect(index >= 0, "the switcher offers the Profiler");
    const auto box = kit::px(kit::iconSize(kit::IconRole::Chrome));
    const QIcon actual = picker->itemIcon(index);
    const QIcon expected = kit::icon(kit::IconId::Profiler, kit::IconRole::Chrome);
    expectations.expect(!actual.isNull(), "the Profiler entry carries an icon");
    expectations.expect(actual.pixmap(QSize(box, box)).toImage() ==
                            expected.pixmap(QSize(box, box)).toImage(),
                        "the Profiler entry shows the gauge glyph");
    expectations.expect(kit::iconResourcePath(kit::IconId::Profiler, kit::IconWeight::Bold)
                            .endsWith(QStringLiteral("/gauge-bold.svg")),
                        "IconId::Profiler is Phosphor gauge");
    expectations.expect(child<ProfilerEditor>(area, "profilerEditor") != nullptr,
                        "the area hosts a ProfilerEditor");
}

void testSubscriptionFollowsVisibilityWithARefcount(Expectations& expectations) {
    FakeProfilerFeed feed;
    expectations.expect(!feed.active() && feed.subscriberCount() == 0, "a new feed is idle");
    auto first = std::make_unique<ProfilerEditor>(&feed);
    auto second = std::make_unique<ProfilerEditor>(&feed);
    expectations.expect(!feed.active() && !first->isSubscribed(),
                        "an editor that was never shown does not subscribe");
    expectations.expect(!feed.pump(), "an idle feed delivers nothing");

    first->show();
    expectations.expect(feed.active() && feed.subscriberCount() == 1 && first->isSubscribed(),
                        "showing a Profiler subscribes and starts the feed");
    second->show();
    expectations.expect(feed.subscriberCount() == 2 && feed.activationCount() == 1,
                        "a second Profiler shares the running feed");
    expectations.expect(feed.pump() && first->history().size() == 1 &&
                            second->history().size() == 1,
                        "one pump delivers the same sample to both");

    first->hide();
    expectations.expect(feed.active() && feed.subscriberCount() == 1 && !first->isSubscribed(),
                        "hiding one leaves the feed running for the other");
    (void)feed.pump();
    expectations.expect(first->history().size() == 1 && second->history().size() == 2,
                        "a hidden Profiler receives nothing");

    second.reset();
    expectations.expect(!feed.active() && feed.subscriberCount() == 0,
                        "destroying the last visible Profiler stops the feed");
    first->show();
    expectations.expect(feed.active() && feed.activationCount() == 2,
                        "showing again restarts the feed");
    first->close();
    expectations.expect(!feed.active() && !first->isSubscribed(),
                        "closing the Profiler unsubscribes it");
    first->show();
    first.reset();
    expectations.expect(!feed.active() && feed.subscriberCount() == 0,
                        "destroying a visible Profiler unsubscribes it");
}

void testRingsWrapAt600WithoutAllocating(Expectations& expectations) {
    ProfilerHistory history;
    const auto& rss = history.series(ProfilerHistory::Series::Rss);
    const float* const storage = rss.storage();
    expectations.expect(rss.capacity() == ProfilerHistory::kCapacity && rss.capacity() == 600,
                        "every ring holds 600 samples");
    runtime::ProfilerSnapshot snapshot;
    fillFakeProfilerSnapshot(1, std::chrono::milliseconds(500), snapshot);
    history.push(snapshot);

    gAllocations = 0;
    gCounting = true;
    for (std::uint64_t serial = 2; serial <= 10'000; ++serial) {
        snapshot.serial = serial;
        snapshot.steadyTimestampNs = static_cast<std::int64_t>(serial) * 500'000'000;
        snapshot.memory.rssBytes = runtime::ProfilerBytes::of(serial);
        history.push(snapshot);
    }
    gCounting = false;
    expectations.expect(gAllocations == 0, "10,000 pushes allocate nothing (counted " +
                                               std::to_string(gAllocations.load()) + ")");
    expectations.expect(history.size() == 600 && rss.size() == 600 && rss.capacity() == 600,
                        "the rings stay at 600 after 10,000 samples");
    expectations.expect(rss.storage() == storage, "the ring storage never moved");
    expectations.expect(rss.at(0) == 9'401.0F && rss.latest() == 10'000.0F,
                        "the oldest held sample is 9,401 and the newest 10,000");
    expectations.expect(ProfilerHistory::storageBytes() ==
                            sizeof(float) * 600 * ProfilerHistory::kRingCount,
                        "the history is one fixed block");

    // The feed's delivery path: reading the fake source and fanning out allocates nothing either.
    FakeProfilerFeed feed;
    CountingSubscriber subscriber;
    feed.subscribe(subscriber);
    gAllocations = 0;
    gCounting = true;
    for (int sample = 0; sample < 10'000; ++sample)
        (void)feed.pump();
    gCounting = false;
    expectations.expect(gAllocations == 0, "10,000 feed deliveries allocate nothing (counted " +
                                               std::to_string(gAllocations.load()) + ")");
    expectations.expect(subscriber.deliveries == 10'000 && subscriber.lastSerial == 10'000,
                        "every pump delivered a new sample");
    feed.unsubscribe(subscriber);

    // A panel's history keeps its capacity across 10,000 deliveries too.
    ProfilerEditor editor(nullptr);
    for (std::uint64_t serial = 1; serial <= 10'000; ++serial) {
        fillFakeProfilerSnapshot(serial, std::chrono::milliseconds(500), snapshot);
        editor.deliver(snapshot);
    }
    expectations.expect(editor.history().size() == 600 &&
                            editor.history().series(ProfilerHistory::Series::Anon).capacity() ==
                                600,
                        "the panel's rings wrap at 600");
}

void testAbsentReadsAbsentAndZeroReadsZero(Expectations& expectations) {
    ProfilerEditor editor(nullptr);
    editor.resize(900, 1200);
    expectations.expect(child<kit::KLabel>(editor, "profilerNoSource") != nullptr,
                        "a Profiler without a source says so");
    auto* memory = child<kit::KReadoutGrid>(editor, "profilerMemoryReadouts");
    auto* gpu = child<kit::KReadoutGrid>(editor, "profilerGpuReadouts");
    expectations.expect(memory != nullptr && gpu != nullptr, "the readout grids exist");
    if (memory == nullptr || gpu == nullptr)
        return;
    expectations.expect(memory->isAbsent(0) && memory->valueText(0) == kit::absentMetricText(),
                        "before any sample RSS reads Unavailable, not 0");
    expectations.expect(memory->isAbsent(9), "before any sample the pool total is absent too");
    auto* taskCounts = child<kit::KReadoutGrid>(editor, "profilerTaskCounts");
    expectations.expect(taskCounts != nullptr && taskCounts->isAbsent(0),
                        "before any sample the task counts are absent");

    runtime::ProfilerSnapshot snapshot;
    snapshot.serial = 1;
    snapshot.intervalNs = 500'000'000;
    snapshot.memory.rssBytes = runtime::ProfilerBytes::of(0);
    snapshot.gpu.liveBytes = runtime::ProfilerBytes::of(0);
    editor.deliver(snapshot);
    expectations.expect(!memory->isAbsent(0) && memory->valueText(0) == QStringLiteral("0 B"),
                        "a measured zero reads 0 B");
    expectations.expect(!memory->isAbsent(9) && memory->valueText(9) == QStringLiteral("0 B") &&
                            taskCounts != nullptr &&
                            taskCounts->valueText(0) == QStringLiteral("0"),
                        "once sampled, no pools and no tasks read as measured zeros");
    expectations.expect(memory->isAbsent(1), "anonymous memory that is absent stays absent");
    expectations.expect(gpu->isAbsent(5), "an absent GPU utilization reads Unavailable");
    expectations.expect(!gpu->isAbsent(0) && gpu->valueText(0) == QStringLiteral("0 B"),
                        "Bloom's zero GPU bytes read 0 B");
    expectations.expect(
        std::isnan(editor.history().series(ProfilerHistory::Series::Anon).latest()) &&
            editor.history().series(ProfilerHistory::Series::Rss).latest() == 0.0F,
        "the history stores a gap for absent and 0 for zero");

    // The fake feed's absent fields: utilization, the encode stage before an export, a helper's
    // CPU time and an exited helper's RSS.
    fillFakeProfilerSnapshot(2, std::chrono::milliseconds(500), snapshot);
    editor.deliver(snapshot);
    expectations.expect(gpu->isAbsent(5), "the fake feed leaves GPU utilization absent");
    auto* encode = child<kit::KHistogramChart>(editor, "profilerStage_5");
    auto* decode = child<kit::KHistogramChart>(editor, "profilerStage_0");
    expectations.expect(encode != nullptr && encode->absent() &&
                            encode->summary() == kit::absentMetricText(),
                        "an untimed stage reads Unavailable");
    expectations.expect(decode != nullptr && !decode->absent(), "a timed stage is present");
    auto* helpers = child<QTableView>(editor, "profilerHelperTable");
    expectations.expect(helpers != nullptr && helpers->model()->rowCount() == 3,
                        "the helper table lists three processes");
    if (helpers != nullptr && helpers->model()->rowCount() == 3) {
        const auto* model = helpers->model();
        expectations.expect(model->index(1, 3).data().toString() == kit::absentMetricText(),
                            "an absent helper CPU time reads Unavailable");
        expectations.expect(model->index(2, 2).data().toString() == kit::absentMetricText(),
                            "an absent helper RSS reads Unavailable");
        expectations.expect(model->index(0, 2).data().toString() != kit::absentMetricText(),
                            "a measured helper RSS reads its value");
    }
}

void testStaleAgesShowTheirAge(Expectations& expectations) {
    ProfilerEditor editor(nullptr);
    auto* memory = child<kit::KReadoutGrid>(editor, "profilerMemoryReadouts");
    if (memory == nullptr) {
        expectations.expect(false, "the memory readouts exist");
        return;
    }
    runtime::ProfilerSnapshot snapshot;
    snapshot.serial = 1;
    snapshot.memory.anonBytes = runtime::ProfilerBytes::of(512ULL << 20);
    snapshot.ledgerSampleAgeNs = runtime::ProfilerNanoseconds::of(3'000'000'000);
    editor.deliver(snapshot);
    expectations.expect(memory->noteText(1) == QStringLiteral("3.0 s old") &&
                            memory->noteTone(1) == kit::Color::Muted,
                        "a fresh ledger value shows its age in Muted");
    snapshot.serial = 2;
    snapshot.ledgerSampleAgeNs = runtime::ProfilerNanoseconds::of(12'500'000'000);
    editor.deliver(snapshot);
    expectations.expect(memory->noteText(1) == QStringLiteral("12.5 s old") &&
                            memory->noteTone(1) == kit::Color::Warn,
                        "a stale ledger value shows its age in Warn");
    snapshot.serial = 3;
    snapshot.memory.anonBytes = {};
    editor.deliver(snapshot);
    expectations.expect(memory->isAbsent(1) && memory->noteText(1).isEmpty(),
                        "an absent value shows no age");

    // The fake feed stalls its ledger sample once a cycle: the age grows past the stale line.
    fillFakeProfilerSnapshot(175, std::chrono::milliseconds(500), snapshot);
    expectations.expect(snapshot.ledgerSampleAgeNs.present &&
                            snapshot.ledgerSampleAgeNs.value > 10'000'000'000,
                        "the fake feed produces stale ledger ages");
    editor.deliver(snapshot);
    expectations.expect(memory->noteTone(1) == kit::Color::Warn,
                        "the fake feed's stalled ledger sample shows as stale");
}

void testUnattributedMath(Expectations& expectations) {
    runtime::ProfilerMemory memory;
    expectations.expect(!runtime::profilerUnattributedBytes(memory).present,
                        "absent anonymous memory leaves unattributed absent");
    memory.anonBytes = runtime::ProfilerBytes::of(1000);
    memory.poolCount = 2;
    memory.pools[0].hostBytes = 300;
    memory.pools[0].pendingBytes = 50;
    memory.pools[1].hostBytes = 100;
    // A slot past poolCount never counts.
    memory.pools[2].hostBytes = 999;
    expectations.expect(runtime::profilerPoolHostBytes(memory) == 450, "pools sum host + pending");
    expectations.expect(runtime::profilerUnattributedBytes(memory).value == 550,
                        "absent staging and claims contribute nothing");
    memory.stagingLiveBytes = runtime::ProfilerBytes::of(100);
    memory.admissionClaimBytes = runtime::ProfilerBytes::of(50);
    const auto unattributed = runtime::profilerUnattributedBytes(memory);
    expectations.expect(unattributed.present && unattributed.value == 400,
                        "unattributed = anon - (pools + pending + staging + claims)");
    expectations.expect(!runtime::profilerUnattributedOverBaseline(memory).present,
                        "no baseline, no over-baseline figure");
    memory.idleBaselineBytes = runtime::ProfilerBytes::of(250);
    expectations.expect(runtime::profilerUnattributedOverBaseline(memory).value == 150,
                        "over baseline subtracts the idle baseline");
    memory.admissionClaimBytes = runtime::ProfilerBytes::of(700);
    expectations.expect(runtime::profilerUnattributedBytes(memory).value == -250,
                        "overcounted attribution goes negative rather than wrapping");

    runtime::ProfilerSnapshot snapshot;
    fillFakeProfilerSnapshot(77, std::chrono::milliseconds(500), snapshot);
    const auto fake = runtime::profilerUnattributedBytes(snapshot.memory);
    expectations.expect(fake.present && snapshot.memory.unattributedBytes.present &&
                            fake.value == snapshot.memory.unattributedBytes.value &&
                            fake.value >
                                static_cast<std::int64_t>(snapshot.memory.idleBaselineBytes.value),
                        "the fake feed's unattributed gap sits above its idle baseline");
}

void testSnapshotIsTriviallyCopyable(Expectations& expectations) {
    expectations.expect(std::is_trivially_copyable_v<runtime::ProfilerSnapshot>,
                        "ProfilerSnapshot is trivially copyable");
    runtime::ProfilerSnapshot source;
    fillFakeProfilerSnapshot(321, std::chrono::milliseconds(500), source);
    runtime::ProfilerSnapshot copy;
    std::memcpy(&copy, &source, sizeof(source));
    expectations.expect(copy.serial == 321 && copy.memory.poolCount == source.memory.poolCount &&
                            runtime::profilerNameView(copy.memory.pools[0].name) ==
                                runtime::profilerNameView(source.memory.pools[0].name),
                        "a memcpy reproduces the snapshot");
    expectations.expect(copy.schemaVersion == runtime::kProfilerSnapshotSchemaVersion,
                        "the snapshot carries its schema version");
    runtime::ProfilerName name;
    runtime::setProfilerName(name, std::string(80, 'x'));
    expectations.expect(runtime::profilerNameView(name).size() ==
                            runtime::kProfilerNameCapacity - 1,
                        "a long name truncates inside its fixed capacity");
    expectations.expect(runtime::profilerHistogramBucket(1'000) == 0 &&
                            runtime::profilerHistogramBucket(16'000) == 1 &&
                            runtime::profilerHistogramBucket(10'000'000'000) ==
                                runtime::kProfilerHistogramBucketCount - 1,
                        "histogram buckets run from under 16 us to over 4 s");
}

void testSampleRingAndFormatting(Expectations& expectations) {
    std::array<float, 4> storage{};
    kit::SampleRing ring(storage.data(), static_cast<int>(storage.size()));
    for (int value = 1; value <= 6; ++value)
        ring.push(static_cast<float>(value));
    expectations.expect(ring.size() == 4 && ring.at(0) == 3.0F && ring.latest() == 6.0F,
                        "a ring keeps the newest samples, oldest first");
    expectations.expect(std::isnan(ring.at(4)), "reading past the end is absent");
    expectations.expect(kit::formatMetric(1536.0 * 1024.0 * 1024.0, kit::MetricUnit::Bytes) ==
                            QStringLiteral("1.50 GiB"),
                        "bytes print in binary units");
    expectations.expect(kit::formatMetric(-2.0 * 1024 * 1024, kit::MetricUnit::Bytes) ==
                            QStringLiteral("−2.00 MiB"),
                        "negative bytes keep their sign");
    expectations.expect(kit::formatMetric(std::nan(""), kit::MetricUnit::Count) ==
                            kit::absentMetricText(),
                        "NaN prints as absent");
    expectations.expect(kit::formatMetric(12345.0, kit::MetricUnit::Count) ==
                            QStringLiteral("12,345"),
                        "counts group digits in American English");
}

} // namespace

// The real source: the panel's visibility acquires and releases the runtime sampler, so no
// sampler thread runs while no Profiler is shown, and the UI-side hook's ledger values arrive.
void testRuntimeFeedDrivesTheSampler(Expectations& expectations) {
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    runtime::MemoryBudgetLedger ledger(std::size_t{32} * 1024U * 1024U * 1024U);
    const int cacheOwner = 0;
    ledger.registerCache(
        &cacheOwner, std::size_t{1} << 30U,
        [] {
            return runtime::MemoryPoolHolding{.hostBytes = std::size_t{256} << 20U,
                                              .hostPendingRetirementBytes = 0,
                                              .deviceBytes = 0};
        },
        [](std::size_t) {});
    {
        runtime::ProfilerSampler sampler(
            {.scheduler = &scheduler, .readGpu = &readProfilerGpuImageStats});
        ProfilerUiSources sources;
        sources.ledger = &ledger;
        RuntimeProfilerFeed feed(sampler, sources);
        expectations.expect(!sampler.running(), "no sampler thread before a Profiler is shown");
        {
            ProfilerEditor panel(&feed);
            panel.resize(640, 480);
            panel.show();
            expectations.expect(sampler.running() && sampler.subscribers() == 1,
                                "showing the Profiler starts the sampler");
            const bool delivered =
                QTest::qWaitFor([&] { return panel.current().serial >= 2; }, 5000);
            expectations.expect(delivered, "the panel receives live samples");
            const auto& snapshot = panel.current();
            expectations.expect(snapshot.memory.rssBytes.present, "RSS is live");
            expectations.expect(snapshot.memory.physicalBytes.present &&
                                    snapshot.memory.poolCount == 1 &&
                                    snapshot.memory.pools[0].hostBytes == (std::size_t{256} << 20U),
                                "the hook's ledger reading reaches the panel");
            expectations.expect(snapshot.cpu.executorPoolCount >= 3 &&
                                    snapshot.cpu.processCpuNs.present,
                                "CPU figures are live");
            expectations.expect(!snapshot.workload.playbackFps.present &&
                                    !snapshot.workload.exportFps.present,
                                "controllers that are not connected stay absent");
            panel.hide();
            expectations.expect(!sampler.running(), "hiding the Profiler stops the sampler");
            panel.show();
            expectations.expect(sampler.running(), "showing it again restarts the sampler");
        }
        expectations.expect(!sampler.running() && sampler.subscribers() == 0,
                            "destroying the Profiler releases the sampler");
    }
    ledger.unregisterCache(&cacheOwner);
    scheduler.beginShutdown();
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("BloomProfilerPanelTests"));
    application.setApplicationName(QStringLiteral("ProfilerPanel"));
    kit::installKinetikTheme(application);
    Expectations expectations;
    testRegistrationAndSwitcherIcon(expectations);
    testSubscriptionFollowsVisibilityWithARefcount(expectations);
    testRingsWrapAt600WithoutAllocating(expectations);
    testAbsentReadsAbsentAndZeroReadsZero(expectations);
    testStaleAgesShowTheirAge(expectations);
    testUnattributedMath(expectations);
    testSnapshotIsTriviallyCopyable(expectations);
    testSampleRingAndFormatting(expectations);
    testRuntimeFeedDrivesTheSampler(expectations);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " Profiler expectation(s) failed\n";
        return 1;
    }
    std::cout << "Profiler panel tests passed\n";
    return 0;
}
