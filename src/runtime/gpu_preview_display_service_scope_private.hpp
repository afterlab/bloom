#pragma once

// The scope tap and the device scope measure of the preview display service (#724).
//
// The Viewer's own frame job computes the scene-linear RGBA32F picture of the frame on the
// service's device. When a frame publishes, the owner keeps an alias of that picture (the tap): for
// the latest Viewer-class frame always, and for one RAM-fill frame only when its request asks for
// it. A measure counts the tapped picture with the display and XYZ color steps and the count kernel
// on the same device, as a separate submission the owner steps without ever waiting on a fence, and
// reads back only the tables. No scene runs and no picture leaves the device.
//
// Memory (#724). The measure never claims memory the Viewer needs; it yields. Its native memory
// (the color outputs, the table buffers and their staging, the OCIO programs) is admitted only as
// a Background-class processing phase through the policy's non-blocking reserve(), retained on the
// device for the measure's life so every allocation is tracked against it, and retired when it
// ends. See scopeAdmission() in the .cpp for the exact rule. Under device memory pressure the
// service gives the measure back FIRST: the tap aliases, then a measure with nothing in flight,
// before parked storage, the scene cache, the OCIO programs or a request to the UI to evict
// finished frames. A measure whose submission is in flight is only polled to its end (its fence
// must be proven retired before its memory is freed) with cancellation requested, and it never
// makes a Viewer stage wait: a stage that needs the device's one processing-phase slot parks the
// measure's phase (or ends a measure with nothing in flight) and retains its own.
//
// Threads: the tap slots, the one request and the native state are written on the owner thread;
// any thread may read a tap's geometry, drop a tap (flags it; the owner frees the picture) and
// queue one request, on which only that caller blocks.

#include "gpu_process_frame_scope_private.hpp"

#include <bloom/runtime/gpu_preview_display_service.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace bloom::runtime::detail {

struct PreviewDisplayServiceCore;
struct PreviewDisplayStageRecord;

// One kept picture. `picture` is touched only on the owner thread.
struct ScopeTapEntry final {
    GpuScopeTapKey key;
    GpuScopeTapGeometry geometry;
    std::shared_ptr<const render::GpuImage> picture;
    // Increases with every tap of its slot: a request names the serial it found.
    std::uint64_t serial = 0;
    // A caller dropped it: it names nothing any more and the owner frees it.
    bool dropped = false;
};

// The one measure request of a service. The worker fills `key`, `work` (prepared on the worker)
// and `cancellation` before queueing; the owner fills the rest and completes it.
struct ScopeMeasureRequest final {
    GpuScopeTapKey key;
    ScopeMeasureWork work;
    CancellationToken cancellation;
    bool fill = false;
    std::uint64_t tapSerial = 0;

    // Completion: the caller waits on `done`.
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    GpuScopeTapMeasureOutcome outcome;

    // Owner thread.
    std::shared_ptr<const render::GpuImage> picture;
    std::unique_ptr<ScopeMeasureStepper> stepper;
    bool yieldCounted = false;
    bool admissionWaitCounted = false;
    // This request's admission retained the measure's phase on the device.
    bool ownsPhase = false;
    // The service gave the measure's memory back for a frame that needed it: the measure is
    // cancelled (polled to its end when a submission is in flight) and ends Skipped(NoRoom).
    bool givenBack = false;
    bool tapDropped = false;
    bool deadlineAnchored = false;
    bool stopAnchored = false;
    std::chrono::steady_clock::time_point deadlineAnchor{};
    std::chrono::steady_clock::time_point stopAnchor{};
    std::uint64_t tableDispatchesBefore = 0;
};

struct ScopeTapState final {
    // Guards the slots' key, geometry, serial and `dropped` flag, the request pointer and
    // `closed`. The owner is the only thread that adds or removes an entry or touches a picture,
    // and it never destroys one while holding this lock.
    std::mutex mutex;
    std::optional<ScopeTapEntry> viewer;
    std::optional<ScopeTapEntry> fill;
    std::uint64_t viewerSerial = 0;
    std::uint64_t fillSerial = 0;
    std::shared_ptr<ScopeMeasureRequest> request;
    // Set by shutdown: no request queues after it.
    bool closed = false;

    // Owner thread only. Created on the first measure, destroyed before the device.
    std::unique_ptr<ScopeMeasureStage> stage;

    // Owner thread only. The measure's Background-class processing phase: retained on the device
    // from admission, parked while a stage retains its own, retired once the measure ended and
    // nothing of it is unretired. `phaseRetirePending` is set once the measure ended while its
    // phase could not be retired yet (a stage's phase is retained and this one is parked).
    std::shared_ptr<render::GpuProcessingPhaseLease> phase;
    bool phaseParked = false;
    bool phaseRetirePending = false;
    bool phaseRetireProven = true;
    // True while the owner steps the measure: an allocation it makes never asks anything to give
    // device memory back for it.
    bool stepping = false;

    // Under `mutex`. The last keys whose tap was given back or refused (bounded), so a later
    // request for one ends Skipped(NoRoom) rather than TapGone.
    std::deque<GpuScopeTapKey> givenBack;

    // Bumped by every change the owner loop should iterate again for (a tap kept or freed, a
    // measure ended), so a stage held back by the fill slot starts at once.
    std::atomic<std::uint64_t> epoch{0};
    // True from queueing to completion: the owner polls at its short interval meanwhile.
    std::atomic_bool active{false};
    // Owner-written probes for tests: the stepper began a submission; cancellation reached it.
    std::atomic_bool stepperBegan{false};
    std::atomic_bool stopIssued{false};

    std::atomic<std::uint64_t> tapsKept{0};
    std::atomic<std::uint64_t> fillTapsKept{0};
    std::atomic<std::uint64_t> fillStageWaits{0};
    std::atomic<std::uint64_t> measuresRequested{0};
    std::atomic<std::uint64_t> measuresStarted{0};
    std::atomic<std::uint64_t> measuresCompleted{0};
    std::atomic<std::uint64_t> measuresSkipped{0};
    std::atomic<std::uint64_t> measuresCancelled{0};
    std::atomic<std::uint64_t> measuresFailed{0};
    std::atomic<std::uint64_t> measuresBusy{0};
    std::atomic<std::uint64_t> measureYields{0};
    std::atomic<std::uint64_t> colorDispatches{0};
    std::atomic<std::uint64_t> tableDispatches{0};
    std::atomic<std::uint64_t> tableReadbacks{0};
    std::atomic<std::uint64_t> tableReadbackBytes{0};
    std::atomic<std::uint64_t> tableReadbackBytesTotal{0};
    std::atomic<std::uint64_t> measureAdmissions{0};
    std::atomic<std::uint64_t> measureAdmissionWaits{0};
    std::atomic<std::uint64_t> measureNeedBytes{0};
    std::atomic<std::uint64_t> measuresNoRoom{0};
    std::atomic<std::uint64_t> measuresGivenBack{0};
    std::atomic<std::uint64_t> measurePhaseParks{0};
    std::atomic<std::uint64_t> viewerTapYields{0};
    std::atomic<std::uint64_t> fillTapYields{0};
    std::atomic<std::uint64_t> tapIdleReleases{0};
    std::atomic<std::uint64_t> reclaimsSeenAtFirstGiveBack{0};
    // Owner thread only: the first give-back for memory has been noted.
    bool firstGiveBackNoted = false;
    std::atomic<std::uint64_t> fillTapsRefused{0};
};

// The most keys whose given-back or refused tap the service remembers.
inline constexpr std::size_t kScopeGivenBackKeys = 64;

[[nodiscard]] constexpr bool scopeViewerClass(const TaskPriority priority) noexcept {
    return priority == TaskPriority::Interactive || priority == TaskPriority::Visible;
}

// Owner thread. Whether a stage's picture is worth keeping: a Viewer-class frame always, a fill
// frame when its request asked.
[[nodiscard]] bool scopeTapWanted(const PreviewDisplayStageRecord& stage) noexcept;

// Owner thread. True while a flagged fill stage must not start its scene work: the fill tap slot
// holds a picture, or a flagged fill stage that started will put one there. Counts each held
// stage once.
[[nodiscard]] bool scopeFillStageHeldBack(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                          PreviewDisplayStageRecord& stage) noexcept;

// Owner thread, where a stage's frame is published and before its phase is released: moves the
// stage's picture alias (if it kept one) into its tap slot. Frees what it replaces.
void keepScopeTap(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                  PreviewDisplayStageRecord& stage) noexcept;

// Owner thread, once per loop turn before the resident pump: frees the pictures of dropped taps.
void reapScopeTaps(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread, once per loop turn after the resident pump: begins or polls the request once.
// Never waits on a fence.
void stepScopeMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread. Ends the queued or in-flight request (Cancelled), waiting for its submission's
// fence to be proven retired within the dispatch deadline, and closes the queue. Idempotent.
void shutdownScopeMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread. Gives every tap picture back and ends the measure (the shutdown and teardown
// path; nothing is recorded as memory pressure).
void releaseScopeTaps(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// What giveScopeMemoryBack let go of.
struct ScopeGiveBack final {
    // Something was given back: a tap, or a measure ended or told to stop. Room can return even
    // when no byte was freed yet (a measure in flight returns its memory when its fence retires).
    bool any = false;
    // Bytes freed now: the allocation of every tap picture nothing else holds (an alias held by the
    // scene cache or the in-flight measure frees nothing until that holder lets go).
    std::uint64_t freedBytes = 0;
};

// Owner thread. The first give-back of memory pressure: called where a frame is short of device
// memory (the resident capacity resolver when work waits for device memory or the reusable
// reserve is short, an executor allocation refused for room, and a frame's capacity wait), BEFORE
// parked storage, the scene cache, the OCIO programs or a request to the UI to evict finished
// frames are touched. Releases the Viewer tap and the fill tap (each counted, its key remembered),
// ends a measure that has nothing in flight and cancels one with a submission in flight (polled to
// its end, then Skipped(NoRoom)). Does nothing while the measure itself is allocating.
ScopeGiveBack giveScopeMemoryBack(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread. The idle trim: a tap matters only while frames are being shown, so the Viewer tap
// and the fill tap are released with the rest of the idle device retention.
void releaseScopeTapsIdle(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread. Where a stage is about to retain its own processing phase on the device: frees the
// device's retained slot of the measure's phase. A measure with nothing in flight ends (given back);
// one with a submission in flight keeps being polled with its phase parked. Returns false only
// when the slot could not be freed (the stage then stays queued and retries; it never fails).
[[nodiscard]] bool yieldScopeDeviceToStage(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread, where a flagged fill frame publishes: whether its tap fits as Background (the
// picture's bytes beside what the Viewer needs). When it does not, the refusal is counted and the
// key remembered so a later measureScopeTap for it ends Skipped(NoRoom).
[[nodiscard]] bool scopeFillTapFits(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                    const GpuScopeTapKey& key, std::uint64_t pictureBytes) noexcept;

// Owner thread, once per loop turn: retires the measure's phase once it can be.
void settleScopePhase(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Reclaims the scope color programs' device memory (owner thread), `bytes` at most.
[[nodiscard]] std::uint64_t reclaimScopePrograms(
    const std::shared_ptr<PreviewDisplayServiceCore>& core, std::uint64_t bytes) noexcept;

// Owner thread, at teardown before the device goes: ends the request, frees every tap picture and
// destroys the measure's native state.
void retireScopeNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// The public entry points of GpuPreviewDisplayService.
[[nodiscard]] std::optional<GpuScopeTapGeometry>
scopeTapGeometryOf(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                   const GpuScopeTapKey& key);
[[nodiscard]] GpuScopeTapMeasureOutcome
measureScopeTapOn(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                  const GpuScopeTapKey& key, const GpuScopeMeasureJob& job,
                  const CancellationToken& cancellation);
void dropScopeTapOn(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                    const GpuScopeTapKey& key) noexcept;

} // namespace bloom::runtime::detail
