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
// Threads: the tap slots, the one request and the native state are written on the owner thread;
// any thread may read a tap's geometry, drop a tap (flags it; the owner frees the picture) and
// queue one request, on which only that caller blocks.

#include "gpu_process_frame_scope_private.hpp"

#include <bloom/runtime/gpu_preview_display_service.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
};

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

// Owner thread. Gives every tap picture and the measure's native state back. The hook for memory
// pressure: a tap is the first thing the service gives back for a frame that needs the room.
void releaseScopeTaps(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

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
