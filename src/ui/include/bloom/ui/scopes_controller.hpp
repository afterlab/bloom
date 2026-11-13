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
// During a RAM preview fill, with the panel open, every frame the cache retains is measured too, in
// the background, so a warm range plays its scopes without measuring.

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
};

class ScopesController final : public QObject {
    Q_OBJECT

  public:
    ScopesController(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                     PreviewFrameCacheHandle cache, ScopesMeasure measure,
                     QObject* parent = nullptr);
    ~ScopesController() override;

    // The measure the panel wants. No scope bit (a hidden panel, every section collapsed) measures
    // nothing and cancels what is running.
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
    // Measures started and finished, for tests.
    [[nodiscard]] std::uint64_t measuresStarted() const noexcept { return measuresStarted_; }
    [[nodiscard]] std::uint64_t cacheHits() const noexcept { return cacheHits_; }
    [[nodiscard]] bool busy() const noexcept { return active_.has_value() || fill_.has_value(); }

  signals:
    void readingChanged();

  private:
    struct Result final {
        ScopesMeasured measured;
        PreviewScopeSource source;
        PreviewScopeIdentity identity;
    };
    using ResultHandle = std::shared_ptr<const Result>;
    struct Pending final {
        runtime::TaskHandle<ResultHandle> handle;
        PreviewFrameCacheKey key;
        PreviewScopeIdentity identity;
    };

    [[nodiscard]] PreviewScopeIdentity identityFor(runtime::ViewAdjust viewAdjust) const;
    void refresh();
    void poll();
    void startFill();
    [[nodiscard]] std::optional<Pending> submit(const PreviewScopeSource& source,
                                                const PreviewScopeIdentity& identity,
                                                runtime::TaskPriority priority);
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
    // The shown frame's measure and one background measure of a retained frame.
    std::optional<Pending> active_;
    std::optional<Pending> fill_;
    std::deque<PreviewFrameCacheKey> fillQueue_;
    bool rangeFillActive_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t measuresStarted_ = 0;
    std::uint64_t cacheHits_ = 0;
};

} // namespace bloom::ui
