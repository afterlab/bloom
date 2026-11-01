// Benchmark mode of the packed RAM-fill harness: a saved project instead of a synthetic sequence.
// It is wired like the desktop application (apps/bloom/main.cpp): app scheduler and budget view,
// process ledger, MemoryBudgetController with the application's memory wiring
// (connectApplicationMemoryBudget), qualified display bootstrap, settings-derived cache
// budgets, the packed GPU preview service, RAM preview fill width from measured capacity, and
// background read-ahead for playback. It measures; it does not qualify parity (the cache100 and
// route-proof gates do). tools/bench/run_bench.py drives it, one fresh process per measurement.
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/profiler_gpu_reader.hpp>
#include <bloom/runtime/profiler_ledger_adapter.hpp>
#include <bloom/runtime/profiler_sampler.hpp>
#include <bloom/runtime/profiler_snapshot_json.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/audio_playback_session.hpp>
#include <bloom/ui/background_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/live_session_adapter.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/qualified_display_processor_bootstrap.hpp>

#include <QTimer>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#endif

#include <fstream>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <vector>

namespace {
std::string gBenchProject;
bool gBenchPlayback = false;          // playback-only run: no RAM fill, read-ahead from cold
double gBenchPlaySeconds = 6.0;       // transport run length for either playback measurement
bool gBenchWorkspace = false;         // the application's window and editors, not a bare Viewer
bool gBenchWorkspaceAssets = true;    // the workspace also runs the application's AssetController
std::chrono::seconds gBenchTimeout{1800};
// Scrub run (#421): jumps to this many frames spread over the work area, none of them cached, and
// times each until the Viewer shows it; then clears the preview frame cache and visits them again.
std::size_t gBenchScrubFrames = 0;
// Runs the profiler sampler thread over the measured window (an open Profiler) and prints the
// window's last ProfilerSnapshot as a BENCH_PROFILER line (#423).
bool gBenchProfiler = false;

[[nodiscard]] std::string benchJson(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (const char c : text) {
        const auto code = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (code < 0x20)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int{code} << std::dec;
        else
            out << c;
    }
    out << '"';
    return out.str();
}

[[nodiscard]] std::uint64_t benchPeakRssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)))
        return 0;
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#else
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss); // macOS reports bytes
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL; // Linux reports KiB
#endif
#endif
}

// Wall-clock epoch seconds, so the runner can align its GPU/CPU samples to the measured window.
[[nodiscard]] double benchEpochSeconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Where the process's memory is, sampled during the measured window. Every figure the snapshot
// carries comes from the one runtime telemetry contract (#423; runtime::ProfilerSnapshot, the
// same numbers the Profiler and MCP profiler.snapshot show), under the field names earlier results
// used: RSS and anonymous memory from statm, pools from the ledger's per-pool rows, staging,
// admission claims and unattributed memory. Absent values print as null, never 0. Four figures
// the snapshot does not carry are read from their owners as before: the operation cache's shared
// references and the processing policy's host claims, usable host capacity and device claims.
struct BenchMemorySample {
    double seconds = 0;
    std::uint64_t preparedFrames = 0;
    runtime::ProfilerBytes rssBytes{};
    runtime::ProfilerBytes anonBytes{};
    runtime::ProfilerBytes fileBytes{};
    runtime::ProfilerBytes shmemBytes{};
    runtime::ProfilerBytes reusablePoolBytes{};
    runtime::ProfilerBytes previewCacheBytes{};
    runtime::ProfilerBytes operationCacheBytes{};
    runtime::ProfilerBytes decodedMediaBytes{}; // operation-cache entries of decoded source media
    runtime::ProfilerBytes operationPendingBytes{}; // inside operationCacheBytes
    runtime::ProfilerBytes reusablePendingBytes{};  // inside reusablePoolBytes
    runtime::ProfilerBytes reusableDeviceBytes{};   // device-resident pool bytes, outside it
    // reusablePoolBytes outside the preview and operation caches: decoded video, the disk-cache
    // write queue and the asset pools.
    runtime::ProfilerBytes otherPoolBytes{};
    runtime::ProfilerBytes stagingLiveBytes{};
    runtime::ProfilerBytes stagingPeakBytes{};
    runtime::ProfilerBytes admissionClaimBytes{};
    runtime::ProfilerMetric<std::int64_t> unattributedBytes{};
    // Not in the snapshot.
    std::uint64_t operationSharedBytes = 0;
    std::uint64_t hostClaimBytes = 0;
    std::uint64_t hostUsableBytes = 0;
    std::uint64_t deviceClaimBytes = 0;
};

void benchMemoryFromSnapshot(const runtime::ProfilerSnapshot& snapshot, BenchMemorySample& sample) {
    const auto& memory = snapshot.memory;
    sample.rssBytes = memory.rssBytes;
    sample.anonBytes = memory.anonBytes;
    sample.fileBytes = memory.fileBytes;
    sample.shmemBytes = memory.shmemBytes;
    sample.stagingLiveBytes = memory.stagingLiveBytes;
    sample.stagingPeakBytes = memory.stagingPeakBytes;
    sample.admissionClaimBytes = memory.admissionClaimBytes;
    sample.unattributedBytes = memory.unattributedBytes;
    if (memory.poolCount == 0 && memory.droppedPools == 0)
        return; // The ledger reading has not been published yet.
    std::uint64_t all = 0, pending = 0, device = 0, preview = 0, operation = 0, decoded = 0,
                  operationPending = 0;
    for (std::size_t index = 0; index < memory.poolCount; ++index) {
        const auto& pool = memory.pools[index];
        const auto held = pool.hostBytes + pool.pendingBytes;
        const auto name = runtime::profilerNameView(pool.name);
        all += held;
        pending += pool.pendingBytes;
        device += pool.deviceBytes;
        if (name == "Preview frames")
            preview += held;
        if (name.starts_with("Operation cache")) {
            operation += held;
            operationPending += pool.pendingBytes;
        }
        if (name == "Operation cache: decoded media")
            decoded += held;
    }
    sample.reusablePoolBytes = runtime::ProfilerBytes::of(all);
    sample.reusablePendingBytes = runtime::ProfilerBytes::of(pending);
    sample.reusableDeviceBytes = runtime::ProfilerBytes::of(device);
    sample.previewCacheBytes = runtime::ProfilerBytes::of(preview);
    sample.operationCacheBytes = runtime::ProfilerBytes::of(operation);
    sample.decodedMediaBytes = runtime::ProfilerBytes::of(decoded);
    sample.operationPendingBytes = runtime::ProfilerBytes::of(operationPending);
    sample.otherPoolBytes =
        runtime::ProfilerBytes::of(all > preview + operation ? all - preview - operation : 0);
}

template <typename T>
void benchMetricJson(std::ostringstream& out, const char* name,
                     const runtime::ProfilerMetric<T>& value) {
    out << ",\"" << name << "\":";
    if (value.present)
        out << value.value;
    else
        out << "null";
}

[[nodiscard]] std::string benchMemoryJson(const BenchMemorySample& sample) {
    std::ostringstream out;
    out << std::setprecision(6) << "{\"seconds\":" << sample.seconds
        << ",\"preparedFrames\":" << sample.preparedFrames;
    benchMetricJson(out, "rssBytes", sample.rssBytes);
    benchMetricJson(out, "anonBytes", sample.anonBytes);
    benchMetricJson(out, "fileBytes", sample.fileBytes);
    benchMetricJson(out, "shmemBytes", sample.shmemBytes);
    benchMetricJson(out, "reusablePoolBytes", sample.reusablePoolBytes);
    benchMetricJson(out, "previewCacheBytes", sample.previewCacheBytes);
    benchMetricJson(out, "operationCacheBytes", sample.operationCacheBytes);
    benchMetricJson(out, "decodedMediaBytes", sample.decodedMediaBytes);
    benchMetricJson(out, "operationPendingBytes", sample.operationPendingBytes);
    out << ",\"operationSharedBytes\":" << sample.operationSharedBytes;
    benchMetricJson(out, "reusablePendingBytes", sample.reusablePendingBytes);
    benchMetricJson(out, "reusableDeviceBytes", sample.reusableDeviceBytes);
    benchMetricJson(out, "otherPoolBytes", sample.otherPoolBytes);
    benchMetricJson(out, "stagingLiveBytes", sample.stagingLiveBytes);
    benchMetricJson(out, "stagingPeakBytes", sample.stagingPeakBytes);
    benchMetricJson(out, "admissionClaimBytes", sample.admissionClaimBytes);
    benchMetricJson(out, "unattributedBytes", sample.unattributedBytes);
    out << ",\"hostClaimBytes\":" << sample.hostClaimBytes
        << ",\"hostUsableBytes\":" << sample.hostUsableBytes
        << ",\"deviceClaimBytes\":" << sample.deviceClaimBytes << '}';
    return out.str();
}

[[nodiscard]] double benchProcessCpuSeconds() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0.0;
    const auto seconds = [](const FILETIME& value) {
        const auto ticks = (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
                           value.dwLowDateTime;
        return static_cast<double>(ticks) / 1e7; // 100 ns units
    };
    return seconds(kernel) + seconds(user);
#else
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](const timeval& value) {
        return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1e6;
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
#endif
}

struct BenchPlayback {
    double seconds = 0.0;
    std::uint64_t transportFrames = 0; // distinct times the transport asked for
    std::uint64_t paintedFrames = 0;   // distinct times the Viewer actually showed Current
    std::uint64_t paintedChanges = 0;  // times the shown Current frame changed (counts loops)
    double advancedSeconds = 0.0;      // session time advanced, unwrapped across loop restarts
    double uiBusySeconds = 0.0;        // time spent inside event-loop slices on the UI thread
    double maxSliceMs = 0.0;           // the longest single event-loop slice
    std::uint64_t longSlices = 0;      // slices longer than one frame period
    // Start of playback (#404). Seconds are from play().
    double firstMotionSeconds = -1.0;     // the shown Current frame first left the start frame
    std::uint64_t droppedFirstSecond = 0; // the footer's dropped count one second after play()
    std::uint64_t paintedFirstSecond = 0; // distinct Current frames shown in that second
    std::uint64_t droppedTotal = 0;       // the footer's dropped count when the run ends
    std::uint64_t readAheadWidth = 0;     // frames the read-ahead may keep in flight at play()
    // When each of the first frames from the start entered the preview cache (-1: never).
    std::vector<double> frameCachedSeconds;
    // Preroll (#404): how the hold ended, how long it held, the frames it waited for, and the
    // dropped count one second after the clock started.
    int prerollOutcome = 0;
    double prerollHeldSeconds = 0.0;
    std::uint64_t prerollFrames = 0;
    double releasedSeconds = -1.0;
    std::uint64_t droppedFirstSecondAfterRelease = 0;
};

constexpr std::size_t kBenchStartFrames = 24;

struct PreviewFrameCacheKeyProbe {
    core::RationalTime time;
    double cachedSeconds = -1.0;
};

[[nodiscard]] const char* prerollOutcomeName(const int outcome) {
    switch (static_cast<ui::PlaybackPreroll::Outcome>(outcome)) {
    case ui::PlaybackPreroll::Outcome::None:
        return "none";
    case ui::PlaybackPreroll::Outcome::Immediate:
        return "immediate";
    case ui::PlaybackPreroll::Outcome::Ready:
        return "ready";
    case ui::PlaybackPreroll::Outcome::CapReached:
        return "cap";
    case ui::PlaybackPreroll::Outcome::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

// The start-of-playback fields shared by both playback measurements (#404).
void benchStartJson(std::ostringstream& json, const char* prefix, const BenchPlayback& played) {
    json << ",\"" << prefix << "FirstMotionSeconds\":" << played.firstMotionSeconds << ",\""
         << prefix << "DroppedFirstSecond\":" << played.droppedFirstSecond << ",\"" << prefix
         << "PaintedFirstSecond\":" << played.paintedFirstSecond << ",\"" << prefix
         << "DroppedTotal\":" << played.droppedTotal << ",\"" << prefix
         << "ReadAheadWidth\":" << played.readAheadWidth << ",\"" << prefix
         << "PrerollOutcome\":\"" << prerollOutcomeName(played.prerollOutcome) << "\",\""
         << prefix << "PrerollHeldSeconds\":" << played.prerollHeldSeconds << ",\"" << prefix
         << "PrerollFrames\":" << played.prerollFrames << ",\"" << prefix
         << "ReleasedSeconds\":" << played.releasedSeconds << ",\"" << prefix
         << "DroppedFirstSecondAfterRelease\":" << played.droppedFirstSecondAfterRelease
         << ",\"" << prefix
         << "FrameCachedSeconds\":[";
    for (std::size_t index = 0; index < played.frameCachedSeconds.size(); ++index)
        json << (index == 0 ? "" : ",") << played.frameCachedSeconds[index];
    json << ']';
}

// Plays from the start for gBenchPlaySeconds and counts what the Viewer really showed.
BenchPlayback benchPlay(ui::CompositionSession& session, ui::PlaybackController& playback,
                        ui::ViewerEditor& viewer, ui::CompositionPreviewController& controller,
                        const std::function<std::size_t(std::uint64_t)>& widthProvider) {
    double framePeriod = 1.0 / 24.0;
    if (const auto* composition = session.composition(); composition != nullptr) {
        const auto rate = composition->format().frameRate();
        if (rate.numerator() > 0)
            framePeriod = static_cast<double>(rate.denominator()) / rate.numerator();
    }
    BenchPlayback result;
    static_cast<void>(session.setCurrentTime(core::RationalTime::fromInteger(0)));
    std::set<std::int64_t> requested, painted;
    std::optional<std::int64_t> lastPainted;
    const auto area = session.workArea();
    const double loopSeconds = area.end.toSeconds() - area.start.toSeconds();
    double previousTime = session.currentTime().toSeconds();
    const auto toKey = [](const core::RationalTime& time) {
        return static_cast<std::int64_t>(std::llround(time.toSeconds() * 1e6));
    };
    // The first frames from the start, as the cache keys the controller would ask for.
    std::vector<PreviewFrameCacheKeyProbe> startFrames;
    if (const auto* composition = session.composition(); composition != nullptr) {
        const auto rate = composition->format().frameRate();
        if (const auto mapping = core::FrameTimeMapping::create(
                composition->duration(), rate.numerator(), rate.denominator());
            mapping.hasValue()) {
            const auto first = mapping.value()->nearestFrameIndex(area.start);
            for (std::size_t index = 0; index < kBenchStartFrames; ++index)
                if (const auto at = mapping.value()->timeForFrame(first + index); at.hasValue())
                    startFrames.push_back({*at.value(), -1.0});
        }
        if (widthProvider)
            result.readAheadWidth = widthProvider(
                static_cast<std::uint64_t>(composition->format().width()) *
                composition->format().height());
    }
    const auto startKey = toKey(session.currentTime());
    std::set<std::int64_t> paintedFirst;
    bool firstSecondSampled = false;
    bool afterReleaseSampled = false;
    const auto started = std::chrono::steady_clock::now();
    playback.play();
    const auto deadline = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        std::chrono::duration<double>(gBenchPlaySeconds));
    std::cout << "BENCH_PLAY_START\n" << std::flush;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto sliceStarted = std::chrono::steady_clock::now();
        QApplication::processEvents();
        const auto slice =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - sliceStarted).count();
        result.uiBusySeconds += slice;
        result.maxSliceMs = std::max(result.maxSliceMs, slice * 1000.0);
        if (slice > framePeriod)
            ++result.longSlices;
        requested.insert(toKey(session.currentTime()));
        const double now = session.currentTime().toSeconds();
        if (now >= previousTime)
            result.advancedSeconds += now - previousTime;
        else if (loopSeconds > 0) // a loop restart: the run went on to the area start
            result.advancedSeconds += now - previousTime + loopSeconds;
        previousTime = now;
        const double sinceStart =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto shown = viewer.displayedPreviewState();
        if (shown.freshness == ui::FrameFreshness::Current && shown.frame) {
            const auto key = toKey(shown.frame->desiredIdentity().time);
            painted.insert(key);
            if (lastPainted != key)
                ++result.paintedChanges;
            lastPainted = key;
            if (key != startKey && result.firstMotionSeconds < 0)
                result.firstMotionSeconds = sinceStart;
            if (sinceStart < 1.0)
                paintedFirst.insert(key);
        }
        for (auto& probe : startFrames) {
            if (probe.cachedSeconds >= 0)
                continue;
            if (const auto cacheKey = controller.cacheKeyForTime(probe.time);
                cacheKey && controller.frameCache().contains(*cacheKey))
                probe.cachedSeconds = sinceStart;
        }
        if (result.releasedSeconds < 0 && !playback.isPreparing())
            result.releasedSeconds = sinceStart;
        if (!afterReleaseSampled && result.releasedSeconds >= 0 &&
            sinceStart >= result.releasedSeconds + 1.0) {
            afterReleaseSampled = true;
            result.droppedFirstSecondAfterRelease = controller.droppedFrameCount();
        }
        if (!firstSecondSampled && sinceStart >= 1.0) {
            firstSecondSampled = true;
            result.droppedFirstSecond = controller.droppedFrameCount();
            result.paintedFirstSecond = paintedFirst.size();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    result.droppedTotal = controller.droppedFrameCount();
    result.prerollOutcome = static_cast<int>(playback.lastPreroll().outcome);
    result.prerollHeldSeconds =
        std::chrono::duration<double>(playback.lastPreroll().held).count();
    result.prerollFrames = playback.prerollFrames();
    for (const auto& probe : startFrames)
        result.frameCachedSeconds.push_back(probe.cachedSeconds);
    playback.pause();
    std::cout << "BENCH_PLAY_END\n" << std::flush;
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.transportFrames = requested.size();
    result.paintedFrames = painted.size();
    return result;
}

// Scrub latency (#421): each target is a frame no cache holds, visited out of order and away from
// the previous one, so read-ahead cannot have prepared it. Seconds from setCurrentTime() until the
// Viewer shows that frame Current.
struct BenchScrub {
    std::vector<double> uncachedSeconds;
    // The same frames again after the preview frame cache is cleared: what the media caches below
    // it (decoded video, decoded media) save.
    std::vector<double> revisitSeconds;
    // Near scrubs: one frame ahead of a shown target, then back to it, each after the preview
    // frame cache is cleared so the frame renders again from its source.
    std::vector<double> nearAheadSeconds;
    std::vector<double> nearBehindSeconds;
};

[[nodiscard]] std::vector<double> benchScrubPass(ui::CompositionSession& session,
                                                 ui::ViewerEditor& viewer,
                                                 const std::vector<core::RationalTime>& targets) {
    std::vector<double> seconds;
    const auto toKey = [](const core::RationalTime& time) {
        return static_cast<std::int64_t>(std::llround(time.toSeconds() * 1e6));
    };
    for (const auto& target : targets) {
        const auto started = std::chrono::steady_clock::now();
        static_cast<void>(session.setCurrentTime(target));
        const bool shown = waitUntil(
            [&] {
                const auto state = viewer.displayedPreviewState();
                return state.freshness == ui::FrameFreshness::Current && state.frame &&
                       toKey(state.frame->desiredIdentity().time) == toKey(target);
            },
            60s);
        seconds.push_back(
            shown ? std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                        .count()
                  : -1.0);
    }
    return seconds;
}

BenchScrub benchScrub(ui::CompositionSession& session, ui::ViewerEditor& viewer,
                      ui::PreviewFrameCache& frameCache) {
    BenchScrub result;
    const auto* composition = session.composition();
    if (composition == nullptr || gBenchScrubFrames == 0)
        return result;
    const auto rate = composition->format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping.hasValue())
        return result;
    const auto area = session.workArea();
    const auto first = static_cast<std::int64_t>(mapping.value()->nearestFrameIndex(area.start));
    const auto last = static_cast<std::int64_t>(mapping.value()->nearestFrameIndex(area.end));
    if (last <= first + 2)
        return result;
    // Alternate between the two halves of the area, so consecutive targets are far apart.
    const auto span = last - first - 1;
    std::vector<core::RationalTime> targets;
    for (std::size_t index = 0; index < gBenchScrubFrames; ++index) {
        const auto step = static_cast<std::int64_t>(index / 2 + 1) * span /
                          static_cast<std::int64_t>(gBenchScrubFrames / 2 + 2);
        const auto frame = index % 2 == 0 ? last - 1 - step : first + 1 + step;
        if (const auto at = mapping.value()->timeForFrame(static_cast<std::uint64_t>(frame));
            at.hasValue())
            targets.push_back(*at.value());
    }
    std::cout << "BENCH_PLAY_START\n" << std::flush;
    result.uncachedSeconds = benchScrubPass(session, viewer, targets);
    frameCache.clear();
    result.revisitSeconds = benchScrubPass(session, viewer, targets);
    for (const auto& target : targets) {
        const auto index = mapping.value()->nearestFrameIndex(target);
        const auto next = mapping.value()->timeForFrame(index + 1);
        if (!next.hasValue())
            continue;
        static_cast<void>(benchScrubPass(session, viewer, {target}));
        frameCache.clear();
        const auto ahead = benchScrubPass(session, viewer, {*next.value()});
        frameCache.clear();
        const auto behind = benchScrubPass(session, viewer, {target});
        result.nearAheadSeconds.insert(result.nearAheadSeconds.end(), ahead.begin(), ahead.end());
        result.nearBehindSeconds.insert(result.nearBehindSeconds.end(), behind.begin(),
                                        behind.end());
    }
    std::cout << "BENCH_PLAY_END\n" << std::flush;
    return result;
}

void benchSecondsJson(std::ostringstream& json, const char* name, std::vector<double> seconds) {
    json << ",\"" << name << "\":[";
    for (std::size_t index = 0; index < seconds.size(); ++index)
        json << (index == 0 ? "" : ",") << seconds[index];
    json << ']';
    std::ranges::sort(seconds);
    const auto median = seconds.empty() ? 0.0 : seconds[seconds.size() / 2];
    const auto worst = seconds.empty() ? 0.0 : seconds.back();
    json << ",\"" << name << "Median\":" << median << ",\"" << name << "Max\":" << worst;
}

int runPackedBenchProject(const TestOptions& options, const bool half) {
    const char* resolution = gQuarter ? "Quarter" : (half ? "Half" : "Full");
    const std::filesystem::path projectPath = std::filesystem::absolute(gBenchProject);
    QSettings().setValue("viewer/resolution", resolution);
    const auto processStarted = std::chrono::steady_clock::now();

    runtime::processMemoryBudgetLedger().beginProcessSampling();
    const auto taskBudgetView = std::make_shared<runtime::TaskBudgetView>();
    auto taskConfig = runtime::TaskSchedulerConfig::defaults();
    taskConfig.budgetView = taskBudgetView;
    runtime::TaskScheduler scheduler(taskConfig);
    ui::ProjectHost projectHost(scheduler);
    bool replaced = false;
    std::optional<ui::ProjectHostOperationOutcome> openOutcome;
    QString openMessage;
    QObject::connect(&projectHost, &ui::ProjectHost::sessionReplaced, [&] { replaced = true; });
    QObject::connect(&projectHost, &ui::ProjectHost::openFinished,
                     [&](const ui::ProjectHostOperationOutcome outcome, const QString& message) {
                         openOutcome = outcome;
                         openMessage = message;
                     });
    projectHost.setUnsavedChangeDecisionProvider([] { return ui::UnsavedChangeDecision::Discard; });
    projectHost.setOpenPathProvider([&] { return std::optional(projectPath); });
    projectHost.requestOpen();
    if (!waitUntil([&] { return replaced || openOutcome.has_value(); }, 300s) || !replaced) {
        std::cout << "BENCH_ERROR open " << projectPath << " failed: "
                  << openMessage.toStdString() << '\n';
        return 2;
    }
    auto [document, commandStack] = projectHost.liveDocumentAndStack();
    if (document == nullptr || commandStack == nullptr) {
        std::cout << "BENCH_ERROR project opened read-only\n";
        return 2;
    }
    ui::CompositionSession session(*document, *commandStack, projectHost.lowestCompositionId());
    if (const auto* color = projectHost.colorSettings(); color != nullptr)
        session.setColorSettings(*color);

    runtime::NodeDefinitionRegistry definitions;
    if (!runtime::registerBuiltInNodeDefinitions(definitions))
        return 2;
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(projectPath.parent_path());
    const QSettings playbackSettings;
    const auto cacheBudgets = ui::cacheMemoryBudgetsFromSettings(playbackSettings);
    evaluator.setVideoCacheByteBudget(
        runtime::processMemoryBudgetLedger().allocate().operationCacheByteBudget);
    evaluator.operationCache()->setByteBudget(cacheBudgets.operationCacheByteBudget);
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    const auto retirementOwner = render::PackedHostRetirementOwner::create();
    PackedHostTestTeardown retirementTeardown(retirementOwner);
    ui::TaskUiBridge bridge(scheduler);
    const auto policy =
        std::make_shared<runtime::GpuProcessingPhasePolicy>(32, retirementOwner, taskBudgetView);
    ui::MemoryBudgetController memoryController(scheduler, bridge,
                                                runtime::processMemoryBudgetLedger(),
                                                ui::applicationMemorySampler(policy));
    // The application's memory model, not a copy of it: delivered samples, active-work claims,
    // evict-on-demand for host capacity waits, and the RAM/VRAM headroom.
    static_cast<void>(ui::connectApplicationMemoryBudget(
        memoryController, policy, runtime::processMemoryBudgetLedger(), &bridge));
    ui::QualifiedDisplayProcessorBootstrap bootstrap(scheduler, bridge, qualifiedProvider);
    auto frameCache =
        std::make_shared<ui::PreviewFrameCache>(cacheBudgets.previewFrameCacheByteBudget);
    bool cacheCapacityInitialized = false;
    QObject::connect(&memoryController, &ui::MemoryBudgetController::sampleApplied, &bridge, [&] {
        if (cacheCapacityInitialized ||
            runtime::processMemoryBudgetLedger().physicalByteBudget() == 0)
            return;
        cacheCapacityInitialized = true;
        const QSettings current;
        static_cast<void>(ui::applyCacheMemoryBudgetsFromSettings(
            current, *evaluator.operationCache(), *frameCache));
    });
    auto planCache = std::make_shared<ui::CompiledPlanCache>();
    const auto pipeline = ui::makeCompositionPreviewPipeline(
        compiler, evaluator, referenceDisplayPreparer, qualifiedProvider, planCache);
    runtime::GpuPreviewDisplayServiceOptions serviceOptionsApp;
    serviceOptionsApp.enabled = true;
    serviceOptionsApp.loaderPath = options.loader_path;
    serviceOptionsApp.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    serviceOptionsApp.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    const auto residentPlan = ui::gpuResidentBudgetPlanFor(cacheBudgets.previewFrameCacheByteBudget);
    ui::applyGpuResidentBudgetPlan(serviceOptionsApp, residentPlan);
    serviceOptionsApp.previewByteAllowance.reset();
    ui::applyGpuResidentCacheLimits(*frameCache, residentPlan);
    auto coverageCache = std::make_shared<runtime::GpuSceneCoverageCache>();
    evaluator.operationCache()->bindHostRetirement(retirementOwner);
    std::shared_ptr<runtime::GpuOcioContextResolver> resolver =
        host::makePackagedGpuOcioResolver(QCoreApplication::applicationFilePath().toStdString());
    if (resolver == nullptr)
        resolver = std::make_shared<runtime::GpuOcioContextResolver>();
    const auto deviceWorkingBytes = (512ULL << 20) + serviceOptionsApp.packedTransferByteAllowance;
    auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
        compiler, evaluator, qualifiedProvider, coverageCache, planCache, resolver,
        {.policy = policy,
         .envelope = {.metadataBytes = 16ULL << 20,
                      .retainedCoverageByteLimit = 16ULL << 20,
                      .requestedExecutorBytes = std::nullopt,
                      .deviceWorkingBytes = deviceWorkingBytes}});
    serviceOptionsApp.processingPhasePolicy = policy;
    serviceOptionsApp.cpuStoragePlanner =
        ui::makeCompositionPreviewCpuStoragePlanner(compiler, evaluator, planCache);
    auto serviceOwner = std::make_unique<runtime::GpuPreviewDisplayService>(
        scheduler, std::move(gpuStage),
        ui::makeCompositionPreviewCpuStage(compiler, evaluator, qualifiedProvider, planCache),
        ui::makeCompositionPreviewCpuDisplayFallback(referenceDisplayPreparer), serviceOptionsApp);
    auto& service = *serviceOwner;
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return service.submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    ui::CompositionPreviewSettings settings;
    settings.colorIntent = session.colorIntent();
    settings.resolutionPolicy = gQuarter ? runtime::PreviewResolutionPolicy::Quarter
                                : half   ? runtime::PreviewResolutionPolicy::Half
                                         : runtime::PreviewResolutionPolicy::Full;
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                frameCache, nullptr, submitter, nullptr, policy);
    QObject::connect(&memoryController, &ui::MemoryBudgetController::sampleApplied, &controller,
                     &ui::CompositionPreviewController::notifyProcessingCapacityChanged);
    const auto widthProvider = [policy, deviceWorkingBytes](const std::uint64_t pixels) {
        return runtime::previewFramesThatFit(*policy, pixels, deviceWorkingBytes);
    };
    // Read-ahead also fills while idle, so only the playback measurement has it: a fill run
    // measures the RAM preview alone. The workspace mode always has it, as the application does.
    std::optional<ui::BackgroundPreviewController> background;
    if (gBenchPlayback || gBenchWorkspace || gBenchScrubFrames != 0) {
        background.emplace(session, controller, scheduler, bridge, pipeline, nullptr, submitter);
        background->setReadAheadWidthProvider(widthProvider);
    }
    ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                 submitter);
    ram.setFillWidthProvider(widthProvider);
    // The workspace mode drives the controller's own transport, as the application does.
    std::optional<ui::PlaybackController> ownPlayback;
    if (!gBenchWorkspace)
        ownPlayback.emplace(session, controller);
    ui::PlaybackController& playback =
        gBenchWorkspace ? controller.playbackController() : *ownPlayback;
    // The application's preroll (#404): the read-ahead window when read-ahead runs; the same
    // capacity-sized window for the cached run after a fill.
    playback.setPrerollFrameCountProvider([&background, &session, widthProvider]() -> std::size_t {
        if (background)
            return background->prerollFrameCount();
        const auto* composition = session.composition();
        if (composition == nullptr)
            return 1;
        return std::clamp<std::size_t>(
            widthProvider(static_cast<std::uint64_t>(composition->format().width()) *
                          composition->format().height()),
            1, ui::BackgroundPreviewController::kMaximumPrerollFrames);
    });
    const auto firstStarted = std::chrono::steady_clock::now();
    std::optional<ui::ViewerEditor> ownViewer;
    std::optional<ui::AssetController> assets;
    std::optional<ui::AudioPlaybackSession> audio;
    std::optional<scripting::LiveSessionHost> liveHost;
    std::optional<ui::LiveSessionAdapter> liveAdapter;
    std::optional<ui::EditorRegistry> registry;
    std::unique_ptr<ui::MainWindow> window;
    if (gBenchWorkspace) {
        // The application-lifetime controllers that follow session time (apps/bloom/main.cpp).
        if (gBenchWorkspaceAssets)
            assets.emplace(session, projectHost, scheduler, bridge, nullptr, nullptr,
                           ui::AudioProcessingAdmission{.policy = policy});
        audio.emplace(session, scheduler, bridge, compiler, planCache, evaluator);
        if (assets)
            audio->setAudioBufferSource(*assets);
        liveHost.emplace();
        liveAdapter.emplace(*liveHost, session, projectHost);
        // The application's window, default workspace and every foundation editor (Timeline,
        // Properties, Assets, ...), so per-tick work in panels is measured too. Offscreen, so the
        // Viewer presents through its non-surface path.
        registry.emplace();
        if (!ui::registerFoundationEditors(*registry, session, controller, &ram, &projectHost)) {
            std::cout << "BENCH_ERROR editors could not be registered\n";
            return 2;
        }
        window = std::make_unique<ui::MainWindow>(*registry, session, projectHost, &ram,
                                                  &controller, nullptr, &playback);
        window->resize(1920, 1080);
        window->show();
        static_cast<void>(waitUntil([&] { return window->displayedViewer() != nullptr; }, 30s));
        if (window->displayedViewer() == nullptr) {
            std::cout << "BENCH_ERROR the workspace has no Viewer\n";
            return 2;
        }
    } else {
        ownViewer.emplace(session, controller);
        ownViewer->resize(960, 540);
        ownViewer->show();
    }
    ui::ViewerEditor& viewer = gBenchWorkspace ? *window->displayedViewer() : *ownViewer;

    const auto shutdown = [&] {
        if (audio)
            audio->beginShutdown();
        if (assets)
            assets->setCachePurgeGate(true);
        ram.beginShutdown();
        if (background)
            background->beginShutdown();
        controller.beginShutdown();
        memoryController.beginShutdown();
        bridge.beginShutdown();
        service.beginShutdown();
        const bool stopped = waitUntil(
            [&] {
                return scheduler.isQuiescent() &&
                       service.status().state == runtime::GpuPreviewDisplayServiceState::Stopped;
            },
            30s);
        viewer.releasePackedFramesForShutdown();
        if (window) {
            window->completeShutdown();
            window.reset();
        }
        controller.releasePublishedFramesForShutdown();
        frameCache->clear();
        evaluator.operationCache()->close();
        retirementOwner->beginShutdown();
        const bool retired = waitUntil([&] { return retirementOwner->exitAcknowledged(); }, 15s);
        if (!stopped || !retired) {
            std::cout << "BENCH_WARNING shutdown stopped=" << stopped << " retired=" << retired
                      << '\n'
                      << std::flush;
            std::_Exit(0); // the measurement is already reported
        }
    };

    if (!waitUntil(
            [&] {
                return service.status().state !=
                       runtime::GpuPreviewDisplayServiceState::Initializing;
            },
            90s) ||
        service.status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
        std::cout << "Unsupported: " << service.status().residentDetail << '\n';
        shutdown();
        return options.require_device ? 1 : 77;
    }
    static_cast<void>(waitUntil([&] { return policy->snapshot().reservations.hostReady; }, 30s));
    const bool firstPainted = waitUntil(
        [&] { return viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current; },
        300s);
    const auto firstFrameSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - firstStarted).count();
    const auto firstFrame = viewer.displayedFrameForTest();
    const int firstProvider =
        firstFrame ? static_cast<int>(firstFrame->provenance().provider) : -1;
    std::ostringstream json;
    json << std::setprecision(6) << "{\"project\":" << benchJson(projectPath.string())
         << ",\"mode\":\""
         << (gBenchScrubFrames != 0 ? "scrub" : (gBenchPlayback ? "playback" : "fill")) << "\""
         << ",\"resolution\":\"" << resolution << "\""
         << ",\"workspace\":" << (gBenchWorkspace ? "true" : "false")
         << ",\"workspaceAssets\":" << (gBenchWorkspace && gBenchWorkspaceAssets ? "true" : "false")
         << ",\"firstFramePainted\":" << (firstPainted ? "true" : "false")
         << ",\"firstFrameSeconds\":" << firstFrameSeconds
         << ",\"firstFrameProvider\":" << firstProvider
         << ",\"previewCacheBudgetBytes\":" << frameCache->byteBudget();
    const auto memoryWindowStarted = std::chrono::steady_clock::now();
    // Memory over the measured window: the sample with the highest RSS, and a line every ten
    // seconds so a run that stops making progress shows where its memory is.
    BenchMemorySample memoryPeak;
    std::chrono::steady_clock::time_point lastMemoryLine{};
    // The runtime profiler sampler (#423), fed like the application's UI-side hook. Without
    // --bench-profiler no sampler thread runs (a closed Profiler): each memory tick takes one
    // sample on this thread. With it the thread runs for the window, as an open Profiler would.
    runtime::ProfilerSampler profiler(
        {.scheduler = &scheduler, .readGpu = &runtime::readProfilerGpuDevice});
    runtime::ProfilerHostInputs profilerInputs{};
    const auto profilerSnapshot = std::make_unique<runtime::ProfilerSnapshot>();
    bool ledgerStateRead = false;
    if (gBenchProfiler)
        profiler.acquire();
    QTimer memoryTimer;
    memoryTimer.setInterval(250);
    QObject::connect(&memoryTimer, &QTimer::timeout, [&] {
        BenchMemorySample sample;
        sample.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                       memoryWindowStarted)
                             .count();
        sample.preparedFrames = ram.preparedFrameCount();
        const auto capacity = policy->usableCapacity();
        runtime::readProfilerLedger(runtime::processMemoryBudgetLedger(), profilerInputs.ledger,
                                    !std::exchange(ledgerStateRead, true));
        profilerInputs.ledger.admissionClaimBytes =
            runtime::ProfilerBytes::of(capacity.hostClaimBytes > capacity.hostRetentionBytes
                                           ? capacity.hostClaimBytes - capacity.hostRetentionBytes
                                           : 0);
        profiler.publishHostInputs(profilerInputs);
        if (!gBenchProfiler)
            static_cast<void>(profiler.sampleNow());
        if (profiler.readLatest(*profilerSnapshot, 0))
            benchMemoryFromSnapshot(*profilerSnapshot, sample);
        sample.operationSharedBytes = evaluator.operationCache()->memory().sharedReferenceBytes;
        sample.hostClaimBytes = capacity.hostClaimBytes;
        sample.hostUsableBytes = capacity.hostUsableBytes;
        sample.deviceClaimBytes = capacity.deviceClaimBytes;
        if (sample.rssBytes.valueOr(0) >= memoryPeak.rssBytes.valueOr(0))
            memoryPeak = sample;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastMemoryLine >= 10s) {
            lastMemoryLine = now;
            std::cout << "BENCH_MEMORY " << benchMemoryJson(sample) << '\n' << std::flush;
        }
    });
    const auto before = service.status().counters;
    const auto cpuBefore = benchProcessCpuSeconds();
    const auto started = std::chrono::steady_clock::now();
    memoryTimer.start();
    json << std::setprecision(15) << ",\"windowStartEpoch\":" << benchEpochSeconds()
         << std::setprecision(6);
    if (gBenchScrubFrames != 0) {
        const auto scrubbed = benchScrub(session, viewer, *frameCache);
        benchSecondsJson(json, "scrubUncachedSeconds", scrubbed.uncachedSeconds);
        benchSecondsJson(json, "scrubRevisitSeconds", scrubbed.revisitSeconds);
        benchSecondsJson(json, "scrubNearAheadSeconds", scrubbed.nearAheadSeconds);
        benchSecondsJson(json, "scrubNearBehindSeconds", scrubbed.nearBehindSeconds);
        json << std::setprecision(15) << ",\"windowEndEpoch\":" << benchEpochSeconds()
             << std::setprecision(6);
    } else if (gBenchPlayback) {
        const auto played = benchPlay(session, playback, viewer, controller, widthProvider);
        benchStartJson(json, "play", played);
        json << ",\"playSeconds\":" << played.seconds
             << ",\"transportFrames\":" << played.transportFrames
             << ",\"paintedFrames\":" << played.paintedFrames
             << ",\"paintedChanges\":" << played.paintedChanges
             << ",\"realTimeRatio\":"
             << (played.seconds > 0 ? played.advancedSeconds / played.seconds : 0.0)
             << ",\"playUiBusySeconds\":" << played.uiBusySeconds
             << ",\"playMaxSliceMs\":" << played.maxSliceMs
             << ",\"playLongSlices\":" << played.longSlices << std::setprecision(15)
             << ",\"windowEndEpoch\":" << benchEpochSeconds() << std::setprecision(6)
             << ",\"playbackFps\":"
             << (played.seconds > 0 ? static_cast<double>(played.paintedFrames) / played.seconds
                                    : 0.0);
    } else {
        ram.start();
        const bool finished = waitUntil([&] { return !ram.isCaching(); }, gBenchTimeout);
        const auto wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto cpu = benchProcessCpuSeconds() - cpuBefore;
        const auto completion = ram.lastCompletion();
        json << ",\"finished\":" << (finished ? "true" : "false") << ",\"completion\":"
             << (completion ? static_cast<int>(*completion) : -1)
             << ",\"completionComplete\":"
             << (completion == ui::RamPreviewController::Completion::Complete ? "true" : "false")
             << ",\"totalFrames\":" << ram.totalFrameCount()
             << ",\"preparedFrames\":" << ram.preparedFrameCount()
             << ",\"cachedFrames\":" << ram.cachedFrameCount()
             << ",\"fillSeconds\":" << wall << ",\"fillFps\":"
             << (wall > 0 ? static_cast<double>(ram.preparedFrameCount()) / wall : 0.0)
             << ",\"fillCpuSeconds\":" << cpu
             << ",\"fillCoresBusy\":" << (wall > 0 ? cpu / wall : 0.0)
             << ",\"peakInFlight\":" << ram.peakInFlightFrames()
             << ",\"cacheResidentBytes\":" << frameCache->residentBytes()
             << ",\"previewFirstYieldedBytes\":"
             << frameCache->statistics().previewFirstYieldedBytes
             << ",\"previewEvictions\":" << frameCache->statistics().evictions
             << ",\"previewPressureDrops\":" << frameCache->statistics().pressureDrops
             << ",\"previewStaleDrops\":" << frameCache->statistics().staleDrops
             << ",\"previewGpuResidentEvictions\":"
             << frameCache->statistics().gpuResidentEvictions << std::setprecision(15)
             << ",\"windowEndEpoch\":" << benchEpochSeconds() << std::setprecision(6);
        if (finished && ram.canAutoPlay() && gBenchPlaySeconds > 0) {
            // Why a cached run might not be cached: document revisions, session snapshot
            // refreshes and cache hits/misses across the run, and the last frame's diagnostics.
            const auto revisionBefore = session.snapshot().revision().value();
            const auto cacheBefore = frameCache->statistics();
            std::uint64_t snapshotRefreshes = 0;
            const auto refreshCounter =
                QObject::connect(&session, &ui::CompositionSession::snapshotChanged,
                                 [&snapshotRefreshes] { ++snapshotRefreshes; });
            const auto played = benchPlay(session, playback, viewer, controller, widthProvider);
            QObject::disconnect(refreshCounter);
            benchStartJson(json, "cachedPlay", played);
            const auto cacheAfter = frameCache->statistics();
            json << ",\"cachedPlayRevisionChanges\":"
                 << session.snapshot().revision().value() - revisionBefore
                 << ",\"cachedPlaySnapshotRefreshes\":" << snapshotRefreshes
                 << ",\"cachedPlayCacheHits\":" << cacheAfter.hits - cacheBefore.hits
                 << ",\"cachedPlayCacheMisses\":" << cacheAfter.misses - cacheBefore.misses
                 << ",\"cachedPlayStaleDrops\":" << cacheAfter.staleDrops - cacheBefore.staleDrops
                 << ",\"cachedPlayEvictions\":" << cacheAfter.evictions - cacheBefore.evictions;
            json << ",\"cachedPlaySeconds\":" << played.seconds
                 << ",\"cachedTransportFrames\":" << played.transportFrames
                 << ",\"cachedPaintedFrames\":" << played.paintedFrames
                 << ",\"cachedPaintedChanges\":" << played.paintedChanges
                 << ",\"cachedRealTimeRatio\":"
                 << (played.seconds > 0 ? played.advancedSeconds / played.seconds : 0.0)
                 << ",\"cachedPresentedFps\":"
                 << (played.seconds > 0
                         ? static_cast<double>(played.paintedChanges) / played.seconds
                         : 0.0)
                 << ",\"cachedPlayUiBusySeconds\":" << played.uiBusySeconds
                 << ",\"cachedPlayMaxSliceMs\":" << played.maxSliceMs
                 << ",\"cachedPlayLongSlices\":" << played.longSlices << ",\"cachedPlaybackFps\":"
                 << (played.seconds > 0
                         ? static_cast<double>(played.paintedFrames) / played.seconds
                         : 0.0);
        }
    }
    {
        std::string codes;
        for (const auto& item : controller.state().diagnostics)
            codes += (codes.empty() ? "" : "; ") + item.code + ": " + item.summary;
        json << ",\"lastFrameDiagnostics\":" << benchJson(codes);
    }
    memoryTimer.stop();
    if (gBenchProfiler) {
        // The newest sample of the window, whole: the same JSON MCP profiler.snapshot returns.
        if (profiler.readLatest(*profilerSnapshot, 0))
            std::cout << "BENCH_PROFILER " << runtime::profilerSnapshotJson(*profilerSnapshot)
                      << '\n'
                      << std::flush;
        profiler.release();
    }
    const auto after = service.status().counters;
    const auto claims = policy->snapshot().reservations;
    json << ",\"cpuFallbackFrames\":" << after.cpuFallbacks - before.cpuFallbacks
         << ",\"cpuFallbacksTotal\":" << after.cpuFallbacks
         << ",\"nativeDispatches\":" << after.nativeDispatches - before.nativeDispatches
         << ",\"packedTransfers\":" << after.packedTransfers - before.packedTransfers
         << ",\"residentGraphJobs\":" << after.residentGraphJobs - before.residentGraphJobs
         << ",\"uploadSubmitSeconds\":"
         << 1e-9 * static_cast<double>(after.uploadSubmitNanoseconds - before.uploadSubmitNanoseconds)
         << ",\"uploadFenceSeconds\":"
         << 1e-9 * static_cast<double>(after.uploadFenceObservationNanoseconds -
                                       before.uploadFenceObservationNanoseconds)
         << ",\"processingSubmitSeconds\":"
         << 1e-9 * static_cast<double>(after.processingSubmitNanoseconds -
                                       before.processingSubmitNanoseconds)
         << ",\"processingFenceSeconds\":"
         << 1e-9 * static_cast<double>(after.processingFenceObservationNanoseconds -
                                       before.processingFenceObservationNanoseconds)
         << ",\"packedTransferSeconds\":"
         << 1e-9 * static_cast<double>(after.packedTransferNanoseconds -
                                       before.packedTransferNanoseconds)
         << ",\"admissionRefusals\":" << after.gpuAdmissionRefusals - before.gpuAdmissionRefusals
         << ",\"residentDetail\":" << benchJson(service.status().residentDetail)
         << ",\"hostCapacityBytes\":" << claims.hostCapacityBytes.value_or(0)
         << ",\"deviceCapacityBytes\":" << claims.deviceCapacityBytes.value_or(0)
         << ",\"memoryAtPeak\":" << benchMemoryJson(memoryPeak)
         << ",\"peakRssBytes\":" << benchPeakRssBytes() << ",\"processSeconds\":"
         << std::chrono::duration<double>(std::chrono::steady_clock::now() - processStarted)
                .count()
         << '}';
    std::cout << "BENCH_RESULT " << json.str() << '\n' << std::flush;
    shutdown();
    return 0;
}
} // namespace
