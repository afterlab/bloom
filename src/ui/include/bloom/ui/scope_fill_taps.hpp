#pragma once

// #724 Scopes: the one owner of every RAM preview fill frame's scope tap.
//
// While a Scopes panel is visible with a scope open, the RAM preview fill asks the preview display
// service to keep the scene picture of each frame it renders (GpuPreviewDisplayService::submit's
// `tapForScopes`), so the panel can count that picture on the Viewer's device and keep the tables
// beside the retained frame. The service holds ONE such picture at a time and holds the next
// flagged fill frame back until the picture is measured or dropped (measureScopeTap or
// dropScopeTap). An unanswered tap would therefore stall the fill, and this class exists so that
// cannot happen by construction, however many panels, Viewers or fills there are:
//
//   * Every flagged submission is made through `submit`, and is recorded here at that moment.
//   * The fill controller reports each result it receives (`received`) and each frame it stops
//     reading (`orphan`). A received frame becomes a `ScopeFillTap`; an orphaned one is polled here
//     until its task has finished and then becomes one too.
//   * A `ScopeFillTap` answers its tap exactly once: `measure` frees the slot through the
//     service's own measure (and drops it again whenever the measure did not), and its destructor
//     drops the tap when nothing was measured. A tap is held only by this class's queue, by the
//     panel's measure task that was handed it, or by nobody; the last holder letting go is the
//     answer. So a hidden or closed panel, a collapsed scope, changed settings, a frame the cache
//     did not keep, a cancelled fill, a destroyed controller or panel, a second panel and the
//     application's shutdown all end in one of two ways: the tap is measured, or it is dropped.
//   * `shutdown` (and the destructor) answers everything still held and detaches from the service,
//     so a late holder's drop is a no-op rather than a call into a gone service.
//
// The panels never talk to the service about fill taps themselves. Interface-thread object, except
// for `ScopeFillTap`, whose `measure` is called on a task worker.

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QObject>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace bloom::ui {

class ScopeFillTaps;
class TaskUiBridge;

// One RAM preview fill frame the panel measures from its scene picture, to be answered exactly
// once. It carries the frame as the fill received it (its process picture on the CPU route, its
// retained display picture on the device route, for the retained-picture fallback) and, for a
// flagged submission, the right to measure the scene picture the service kept. Thread-safe.
class ScopeFillTap final {
  public:
    ScopeFillTap(const ScopeFillTap&) = delete;
    ScopeFillTap& operator=(const ScopeFillTap&) = delete;
    // Drops the tap when it was not measured.
    ~ScopeFillTap();

    [[nodiscard]] const runtime::PreviewRequestIdentity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const runtime::GpuScopeTapKey& key() const noexcept { return key_; }
    // The frame as the fill received it.
    [[nodiscard]] const PreparedPreviewFrameHandle& frame() const noexcept { return frame_; }
    // True when the service may hold this frame's scene picture (a flagged submission).
    [[nodiscard]] bool tapped() const noexcept { return tapped_; }

    // Counts the tapped picture on the service's device (GpuPreviewDisplayService::measureScopeTap)
    // and answers the tap whatever the result. A task worker only: it blocks until the result.
    [[nodiscard]] runtime::GpuScopeTapMeasureOutcome
    measure(const runtime::GpuScopeMeasureJob& job, const runtime::CancellationToken& cancellation);
    // Answers the tap by dropping it when nothing has answered it yet. Any thread.
    void release() noexcept;

  private:
    friend class ScopeFillTaps;
    // The link to the service, shared with the owner so that closing the owner detaches every tap
    // it handed out. `mutex` is held shared across a service call and exclusively to detach.
    struct Port final {
        std::shared_mutex mutex;
        runtime::GpuPreviewDisplayService* service = nullptr;
        std::atomic<std::uint64_t> answeredByMeasure{0};
        std::atomic<std::uint64_t> answeredByDrop{0};
    };
    ScopeFillTap(std::shared_ptr<Port> port, runtime::PreviewRequestIdentity identity,
                 PreparedPreviewFrameHandle frame, bool tapped);

    std::shared_ptr<Port> port_;
    runtime::PreviewRequestIdentity identity_;
    PreparedPreviewFrameHandle frame_;
    bool tapped_ = false;
    runtime::GpuScopeTapKey key_;
    std::atomic<bool> answered_{false};
};

// Attaches `sidecar` to the cache entry of `key` unless the entry already holds better tables for
// the same measure: a Retained sidecar never replaces a Rendered one, and a Rendered live (detail
// 1) one never replaces the Rendered full one. False when nothing was attached.
bool attachScopeSidecar(PreviewFrameCache& cache, const PreviewFrameCacheKey& key,
                        const PreviewScopeSidecarHandle& sidecar);

class ScopeFillTaps final : public QObject {
    Q_OBJECT

  public:
    // `service` may be null (no device: nothing is ever flagged). It must outlive this object or
    // be detached by `shutdown` first.
    ScopeFillTaps(runtime::GpuPreviewDisplayService* service, PreviewFrameCacheHandle cache,
                  TaskUiBridge& bridge, QObject* parent = nullptr);
    ~ScopeFillTaps() override;

    // Answers every tap still held and detaches from the service. Idempotent.
    void shutdown();

    // ---- The panels' side ---------------------------------------------------------------------
    // A panel (any pointer that names it) is shown with a scope open, or is not. Fill requests are
    // flagged exactly while at least one panel is.
    void setWanted(const void* client, bool wanted);
    [[nodiscard]] bool wanted() const noexcept;
    // Whether fill requests can carry the flag (there is a preview display service). Without one
    // the fill's CPU frames are still measured from their process pictures.
    [[nodiscard]] bool canTap() const noexcept { return !closed_ && service_ != nullptr; }
    // Why the tap of the frame `key` names did not measure (a refused, given back or failed tap),
    // for the frame's status when it is shown; empty when unknown.
    [[nodiscard]] std::string failureFor(const PreviewFrameCacheKey& key) const;
    void rememberFailure(const PreviewFrameCacheKey& key, std::string reason);
    // The next received tap, for the panel that serves the fill's taps (the first one registered);
    // null for any other panel or when none waits. The caller owns the answer from here: it
    // measures the tap, or lets go of it.
    [[nodiscard]] std::shared_ptr<ScopeFillTap> takeTap(const void* client);
    // The tables a tap's measure produced for the frame `key` names: attached to its cache entry
    // now, or kept (a few) until the frame is retained, when the entry does not exist yet.
    void deliver(const PreviewFrameCacheKey& key, PreviewScopeSidecarHandle sidecar);

    // ---- The fill's side ----------------------------------------------------------------------
    // A flagged submission to the service. `origin` names the caller, for the counters only.
    [[nodiscard]] runtime::TaskSubmission<runtime::PreviewPreparationResultHandle>
    submit(runtime::TaskRequest request, const document::Snapshot& snapshot,
           const runtime::PreviewRequestIdentity& identity, std::size_t pixelStorageByteLimit,
           const std::vector<runtime::SnapshotParameterOverride>& overrides);
    // Records a flagged submission the service accepted (what `submit` does after the service
    // accepted its request); public for fills that submit through another path and for tests.
    void adoptSubmission(const runtime::PreviewRequestIdentity& identity,
                         runtime::TaskHandle<runtime::PreviewPreparationResultHandle> handle);
    // The fill has received the result of the submission `identity` names; `frame` is the frame it
    // delivered (null: none). A flagged submission always ends in an answered tap; a CPU frame that
    // still holds its process picture is queued for the panel to count while the picture is held.
    void received(const runtime::PreviewRequestIdentity& identity,
                  const PreparedPreviewFrameHandle& frame);
    // The fill no longer reads the result of a flagged submission (cancelled, detached, or its
    // controller is gone): this object polls the task and answers its tap when it has finished.
    void orphan(const runtime::PreviewRequestIdentity& identity);
    // A fill ended: tables kept for frames that were not retained are dropped.
    void fillEnded();

    // ---- Counters, for tests -------------------------------------------------------------------
    // Flagged submissions accepted by the service.
    [[nodiscard]] std::uint64_t flaggedSubmissions() const noexcept { return flagged_; }
    // Taps answered by their measure and by a drop. After every holder has let go and every
    // orphan has finished, flagged == measured + dropped.
    [[nodiscard]] std::uint64_t answeredByMeasure() const noexcept;
    [[nodiscard]] std::uint64_t answeredByDrop() const noexcept;
    // Taps submitted and not yet turned into a `ScopeFillTap` (in flight or orphaned).
    [[nodiscard]] std::size_t pendingSubmissions() const noexcept { return entries_.size(); }
    // Taps received and waiting for a panel to take them.
    [[nodiscard]] std::size_t waitingTaps() const noexcept { return ready_.size(); }
    // Tables of unretained frames kept for their frame.
    [[nodiscard]] std::size_t keptTables() const noexcept { return kept_.size(); }

  signals:
    // A tap is waiting (or the panel that serves the taps changed).
    void tapsAvailable();

  private:
    struct Entry final {
        runtime::PreviewRequestIdentity identity;
        runtime::TaskHandle<runtime::PreviewPreparationResultHandle> handle;
        bool orphaned = false;
    };
    void onRetained(const PreviewFrameCacheKey& key);
    void pollOrphans();
    [[nodiscard]] std::shared_ptr<ScopeFillTap> makeTap(const runtime::PreviewRequestIdentity&,
                                                        PreparedPreviewFrameHandle frame,
                                                        bool tapped);

    runtime::GpuPreviewDisplayService* service_ = nullptr;
    PreviewFrameCacheHandle cache_;
    std::shared_ptr<ScopeFillTap::Port> port_;
    std::vector<const void*> clients_;
    std::vector<Entry> entries_;
    std::deque<std::shared_ptr<ScopeFillTap>> ready_;
    // Tables measured before their frame was retained, oldest first.
    std::vector<std::pair<PreviewFrameCacheKey, PreviewScopeSidecarHandle>> kept_;
    std::vector<std::pair<PreviewFrameCacheKey, std::string>> failures_;
    bool fillActive_ = false;
    bool closed_ = false;
    std::uint64_t flagged_ = 0;
};

} // namespace bloom::ui
