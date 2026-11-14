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
// The work is bounded (docs/architecture/scopes.md, "Real-time cost"): one measure is in flight per
// controller, for the shown frame and the RAM preview fill together, and a newer request only
// replaces the one waiting. The shown frame is measured at the live detail first and, when that
// frame is still the one shown, once more at full detail.
//
// During a RAM preview fill, with the panel open, every frame the cache retains is measured too, in
// the background and only while nothing else is wanted, so a warm range plays its scopes without
// measuring.

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

namespace bloom::ui {

class TaskUiBridge;

// One measure of one frame, run on a task worker.
struct ScopesMeasured final {
    runtime::ScopeFrameMeasure measure;
    // The signal's color objects (the graticules' primaries and white, the luma coefficients);
    // null when they could not be prepared.
    std::shared_ptr<const runtime::ScopeSignalColor> color;
};
using ScopesMeasure = std::function<ScopesMeasured(const PreviewScopeSource& source,
                                                   const PreviewScopeIdentity& identity,
                                                   const runtime::CancellationToken& cancellation)>;

// The production measure: the frame's own plan and request through runtime::measureScopeFrame, on
// the device when the frame was prepared on the device. `assetBaseDirectory` is read on the
// worker for every measure. Thread-safe; keeps the color objects of the last few signals.
[[nodiscard]] ScopesMeasure
makeScopesMeasure(runtime::ScopeFrameRoutes routes,
                  std::function<std::optional<std::filesystem::path>()> assetBaseDirectory,
                  std::size_t pixelStorageByteLimit);

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
    ScopesController(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                     PreviewFrameCacheHandle cache, ScopesMeasure measure,
                     QObject* parent = nullptr);
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

    // A RAM preview fill is running: measure the frames the cache retains.
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
        PreviewScopeIdentity identity;
    };
    struct Pending final {
        runtime::TaskHandle<ResultHandle> handle;
        Request request;
        // True for the shown frame's measure, false for a retained frame of the fill.
        bool shown = true;
        // Cancelled: it keeps the slot until its task has finished, and its result is dropped.
        bool cancelled = false;
    };

    [[nodiscard]] PreviewScopeIdentity identityFor(runtime::ViewAdjust viewAdjust,
                                                   std::uint32_t detail = 0) const;
    void refresh();
    void cancelInFlight();
    void poll();
    // Starts the waiting request, else a retained frame of the fill, when nothing is in flight.
    void startNext();
    void takeShown(const Result& result, const Pending& pending);
    [[nodiscard]] bool startShown();
    [[nodiscard]] bool startFill();
    [[nodiscard]] std::optional<Pending> submit(const Request& request, bool shown);
    [[nodiscard]] PreviewScopeSidecarHandle sidecarOf(const Result& result) const;
    void publish(ScopesReading reading);

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    PreviewFrameCacheHandle cache_;
    ScopesMeasure measure_;
    runtime::ScopeMeasureSettings settings_;
    std::optional<PreviewScopeSource> shown_;
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
