#pragma once

// #724 Scopes: the controller behind the Scopes panel (docs/architecture/scopes.md).
//
// It is told which frame the followed Viewer shows and which measure the panel wants, and gives
// the panel that frame's scope tables. The tables come from the frame's sidecar in the preview
// cache when it has one measured under the same settings; otherwise the frame is measured on a
// task worker (never on the interface thread) and the result is attached to the frame's cache
// entry. A request that is superseded is cancelled and its result is neither shown nor cached.
// While another frame's tables are still the ones on show, the reading says it is behind and names
// the frame that was measured.
//
// A measure never evaluates a scene: it counts a picture the Viewer already produced. For a frame
// it picks, in this order, (a) the scene picture the Viewer's own device kept for the frame (the
// scope tap of the preview display service; source Rendered, route GPU), (b) the CPU route's
// process picture the frame still holds (Rendered, CPU), (c) the retained 8-bit display picture
// (source Retained, route CPU; Display and CIE only, the Working signal says it is not available
// for a cached frame). When (a) is tried and does not measure (skipped, busy, failed) the frame is
// counted from (c) so the scopes keep updating, and a frame that stays shown tries (a) once more at
// full detail and replaces the Retained tables by the Rendered ones when that works.
//
// The work is bounded (docs/architecture/scopes.md, "Real-time cost"): one measure is in flight per
// controller, for the shown frame and the RAM preview fill together, and a newer request only
// replaces the one waiting. A Rendered shown frame is measured at the live detail first and, when
// that frame is still the one shown, once more at full detail. A Retained measure counts the same
// pixels at either detail on the CPU, so it is made once, at full detail, and its frame is not
// measured again (except for the one retry of (a) above); a Rendered measure that follows a live
// one never replaces its tables by Retained ones, so the source never flickers.
//
// During a RAM preview fill, with the panel open, the fill's frames are measured from their own
// scene pictures at full detail (ScopeFillTaps), and every other frame the cache retains from its
// retained picture, in the background and only while nothing else is wanted, so a warm range plays
// its scopes without measuring.

#include <bloom/runtime/gpu_scope_measure.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QObject>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime {
class GpuPreviewDisplayService;
struct GpuSceneOcioContext;
} // namespace bloom::runtime

namespace bloom::ui {

class ScopeFillTap;
class ScopeFillTaps;
class TaskUiBridge;

// What one measure of a frame may count. The measure never evaluates a scene.
struct ScopesMeasureInput final {
    // The frame's request, plan and route.
    PreviewScopeSource source;
    // The frame as the Viewer shows it, or as the cache retains it: its process picture (CPU
    // route) and its retained display picture. Null for a fill frame the cache does not hold yet.
    PreparedPreviewFrameHandle frame;
    // A RAM preview fill frame's tap, counted instead of the Viewer's tap. The measure answers it.
    std::shared_ptr<ScopeFillTap> fillTap;
    // Only the Rendered sources are tried: the device tap and the CPU process picture. Used to
    // replace tables that are already on show; a result that is not Measured leaves them as they
    // are.
    bool renderedOnly = false;
    // Why a RAM preview fill's tap of this frame did not measure, when it did not (for the status
    // of a cached frame); empty otherwise.
    std::string priorTapNote;
};

// One measure of one frame, run on a task worker.
struct ScopesMeasured final {
    runtime::ScopeFrameMeasure measure;
    // The signal's color objects (the graticules' primaries and white, the luma coefficients);
    // null when they could not be prepared.
    std::shared_ptr<const runtime::ScopeSignalColor> color;
    // The detail the tables were counted at (a Retained measure is always at full detail).
    std::uint32_t detail = 0;
    // Why the device tap was not used when it was tried or expected; empty when it was used or the
    // frame has none.
    std::string tapNote;
    // The tap was tried and refused for a reason that may pass (busy, no room, superseded, failed),
    // not because the frame has no tap.
    bool tapRetry = false;
};
using ScopesMeasure = std::function<ScopesMeasured(const ScopesMeasureInput& input,
                                                   const PreviewScopeIdentity& identity,
                                                   const runtime::CancellationToken& cancellation)>;

// Where the production measure finds pictures. Neither source evaluates a scene.
struct ScopesMeasureSources final {
    // The preview display service of the Viewer's device (null: no tap).
    runtime::GpuPreviewDisplayService* previewService = nullptr;
    // The shared OCIO context the device color steps are prepared from, resolved on the worker.
    std::function<std::shared_ptr<const runtime::GpuSceneOcioContext>()> ocio;
};

// The production measure: the device tap, else the frame's own process picture, else its retained
// display picture. Thread-safe; keeps the color objects of the last few signals.
[[nodiscard]] ScopesMeasure makeScopesMeasure(ScopesMeasureSources sources);

struct ScopesReading final {
    enum class State : std::uint8_t {
        // Nothing to measure: no frame, or no scope asked for.
        Idle,
        // The shown frame is being measured; `scopes`, when set, is an earlier frame's.
        Measuring,
        // `scopes` is the shown frame's.
        Ready,
        // The shown frame could not be measured; `message` says why.
        Failed,
    };
    State state = State::Idle;
    std::string message;
    // The tables on show and the frame they were measured from.
    PreviewScopeSidecarHandle scopes;
    std::optional<PreviewScopeSource> measured;
    std::shared_ptr<const runtime::ScopeSignalColor> color;
    // The frame the Viewer shows.
    std::optional<PreviewScopeSource> shown;
    // True when `scopes` is not the shown frame's under the current settings.
    bool behind = false;
    // The detail of the tables on show: 0 is full detail, kLiveDetail the live measure.
    std::uint32_t detail = 0;
};

// The detail the shown frame is first measured at (render::scopeTableLayout's detail 1).
inline constexpr std::uint32_t kLiveDetail = 1U;

class ScopesController final : public QObject {
    Q_OBJECT

  public:
    // `fillTaps` (optional) hands the controller the taps of the RAM preview fill's frames while a
    // scope is open.
    ScopesController(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                     PreviewFrameCacheHandle cache, ScopesMeasure measure,
                     ScopeFillTaps* fillTaps = nullptr, QObject* parent = nullptr);
    ~ScopesController() override;

    // The measure the panel wants. No scope bit (a hidden panel, every section collapsed) measures
    // nothing and cancels what is running. The detail is the controller's choice: the settings'
    // detail is taken as 0 (full) and the live measure is the same settings at kLiveDetail.
    void setSettings(const runtime::ScopeMeasureSettings& settings);
    [[nodiscard]] const runtime::ScopeMeasureSettings& settings() const noexcept {
        return settings_;
    }

    // The frame the followed Viewer shows now (null: none) and its exposure and gamma. `frame` is
    // the Viewer's neutral frame; the adjustment is applied by the measure.
    void showFrame(const PreparedPreviewFrameHandle& frame, runtime::ViewAdjust viewAdjust);

    // A RAM preview fill is running: measure the frames the cache retains from their retained
    // pictures (the fill's own taps are served whenever a scope is open).
    void setRangeFillActive(bool active);

    [[nodiscard]] const ScopesReading& reading() const noexcept { return reading_; }
    // Counters, for tests.
    [[nodiscard]] std::uint64_t measuresStarted() const noexcept { return measuresStarted_; }
    [[nodiscard]] std::uint64_t cacheHits() const noexcept { return cacheHits_; }
    // Measures in flight now (0 or 1) and the greatest number ever in flight at once.
    [[nodiscard]] std::uint32_t measuresInFlight() const noexcept {
        return inFlight_.has_value() ? 1U : 0U;
    }
    [[nodiscard]] std::uint32_t maxMeasuresInFlight() const noexcept { return maxInFlight_; }
    // Waiting requests that were replaced or dropped before they started.
    [[nodiscard]] std::uint64_t measuresSkipped() const noexcept { return measuresSkipped_; }
    // The bytes the table readback of the last measure that finished moved (0 on the CPU route).
    [[nodiscard]] std::uint64_t lastTableReadbackBytes() const noexcept {
        return lastTableReadbackBytes_;
    }
    // A measure is in flight or a shown-frame request waits.
    [[nodiscard]] bool busy() const noexcept {
        return inFlight_.has_value() || (want_.has_value() && !wantInFlight_);
    }

  signals:
    void readingChanged();

  private:
    struct Result final {
        ScopesMeasured measured;
        PreviewScopeSource source;
        PreviewScopeIdentity identity;
    };
    using ResultHandle = std::shared_ptr<const Result>;
    // One measure asked for: a frame under an identity (the detail is part of it).
    struct Request final {
        PreviewScopeSource source;
        PreviewFrameCacheKey key;
        // What is asked for (source Rendered); the sidecar names what was counted.
        PreviewScopeIdentity identity;
        PreparedPreviewFrameHandle frame;
        std::shared_ptr<ScopeFillTap> tap;
        bool renderedOnly = false;
        std::string priorTapNote;
    };
    struct Pending final {
        runtime::TaskHandle<ResultHandle> handle;
        Request request;
        // True for the shown frame's measure, false for a fill frame (a tap or a retained frame).
        bool shown = true;
        // Cancelled: it keeps the slot until its task has finished, and its result is dropped.
        bool cancelled = false;
    };

    [[nodiscard]] PreviewScopeIdentity identityFor(runtime::ViewAdjust viewAdjust,
                                                   std::uint32_t detail = 0,
                                                   runtime::ScopeSource source =
                                                       runtime::ScopeSource::Rendered) const;
    [[nodiscard]] std::string priorNote(const PreviewFrameCacheKey& key) const;
    void refresh();
    void cancelInFlight();
    void poll();
    // Starts the waiting request, else a retained frame of the fill, when nothing is in flight.
    void startNext();
    void takeShown(const Result& result, const Pending& pending);
    [[nodiscard]] bool startShown();
    [[nodiscard]] bool startTap();
    [[nodiscard]] bool startFill();
    void takeFill(const Result* result, const Pending& pending);
    [[nodiscard]] std::optional<Pending> submit(const Request& request, bool shown);
    [[nodiscard]] PreviewScopeSidecarHandle sidecarOf(const Result& result,
                                                      const PreviewScopeIdentity& asked) const;
    void publish(ScopesReading reading);

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    PreviewFrameCacheHandle cache_;
    ScopesMeasure measure_;
    ScopeFillTaps* fillTaps_ = nullptr;
    runtime::ScopeMeasureSettings settings_;
    std::optional<PreviewScopeSource> shown_;
    PreparedPreviewFrameHandle shownFrame_;
    runtime::ViewAdjust viewAdjust_{};
    ScopesReading reading_;
    // The one measure in flight: the shown frame's or a retained frame's.
    std::optional<Pending> inFlight_;
    // The shown frame's measure wanted now; in flight when `wantInFlight_`, else the one waiting
    // request.
    std::optional<Request> want_;
    bool wantInFlight_ = false;
    std::deque<PreviewFrameCacheKey> fillQueue_;
    bool rangeFillActive_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t measuresStarted_ = 0;
    std::uint64_t cacheHits_ = 0;
    std::uint32_t maxInFlight_ = 0;
    std::uint64_t measuresSkipped_ = 0;
    std::uint64_t lastTableReadbackBytes_ = 0;
};

} // namespace bloom::ui
