#pragma once

#include <atomic>
#include <bloom/document/document.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <optional>

class QTimer;
namespace bloom::ui {
class CompositionSession;

// Resolves the same immutable local geometry as the viewer, off the UI thread.
class PropertiesAnchorGrid final : public kit::KAnchorGrid {
    Q_OBJECT
    Q_PROPERTY(int selectedPoint READ selectedPoint)
    // For tests (#718): how many bounds evaluations were started, whether one is running, and
    // the wait after the playhead last moved (a test makes it endless and ends it itself, so no
    // clock decides what it sees).
    Q_PROPERTY(int boundsEvaluations READ boundsEvaluations)
    Q_PROPERTY(bool boundsResolving READ boundsResolving)
    Q_PROPERTY(int settleMilliseconds READ settleMilliseconds WRITE setSettleMilliseconds)
  public:
    PropertiesAnchorGrid(CompositionSession& session, QWidget* parent);
    ~PropertiesAnchorGrid() override;
    void refresh();
    [[nodiscard]] int selectedPoint() const override;
    void choosePoint(int index) override;
    [[nodiscard]] int boundsEvaluations() const noexcept { return evaluations_; }
    [[nodiscard]] bool boundsResolving() const noexcept { return active_; }
    [[nodiscard]] int settleMilliseconds() const;
    void setSettleMilliseconds(int milliseconds);
    // Ends the wait after a playhead move now, as its timer would.
    Q_INVOKABLE void settleNow();

  private:
    struct Result {
        std::optional<runtime::ContentBounds> bounds;
        QString diagnostic;
    };
    void start();
    void poll();
    CompositionSession& session_;
    std::optional<document::LayerId> layer_;
    std::optional<runtime::ContentBounds> bounds_;
    QString identity_;
    QString structuralIdentity_;
    // #685: only the playhead moved, so the last resolved bounds still draw while new ones resolve.
    bool refining_ = false;
    int shownPoint_ = -1;
    std::uint64_t generation_ = 0;
    std::uint64_t activeGeneration_ = 0;
    std::shared_ptr<runtime::TaskScheduler> scheduler_;
    std::shared_ptr<std::atomic_bool> retire_;
    runtime::TaskHandle<std::shared_ptr<Result>> task_;
    QTimer* timer_;
    // Restarted by every time-only change; the bounds are resolved when it runs out (#718).
    QTimer* settle_;
    bool active_ = false;
    int evaluations_ = 0;
};
} // namespace bloom::ui
