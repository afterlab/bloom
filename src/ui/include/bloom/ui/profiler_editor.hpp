#pragma once

// The Profiler panel (#423): editor `bloom.profiler`, "Profiler". An ordinary replaceable editor
// panel that shows Bloom's memory, GPU, CPU, workload, pressure and helper processes from the one
// runtime telemetry contract (runtime::ProfilerSnapshot) delivered by a ProfilerFeed.
//
// It costs nothing while it is not visible: it subscribes to the feed when shown and unsubscribes
// when hidden or destroyed, so the feed (and the runtime sampler behind it) runs only while some
// Profiler is on screen. Its history is a fixed set of 600-sample rings allocated once with the
// panel and freed with it. On a sample it copies the snapshot, pushes one float per series and asks
// the expanded sections to repaint; numbers are formatted only where they paint. It reads; it never
// changes scheduling, budgets or caches.

#include <bloom/runtime/profiler_snapshot.hpp>
#include <bloom/ui/profiler_feed.hpp>

#include <QPointer>
#include <QWidget>

#include <array>
#include <memory>

class QTableView;
class QVBoxLayout;

namespace bloom::ui::kit {
class KHistogramChart;
class KLabel;
class KReadoutGrid;
class KSection;
class KTimeSeriesChart;
} // namespace bloom::ui::kit

namespace bloom::ui {

class EditorRegistry;
class ProfilerHistory;
class ProfilerTableModel;

class ProfilerEditor final : public QWidget, private ProfilerFeedSubscriber {
    Q_OBJECT

  public:
    // A null feed is a Profiler with no source: every metric reads absent.
    explicit ProfilerEditor(ProfilerFeed* feed, QWidget* parent = nullptr);
    ~ProfilerEditor() override;

    [[nodiscard]] bool isSubscribed() const noexcept { return subscribed_; }
    [[nodiscard]] const ProfilerHistory& history() const noexcept { return *history_; }
    [[nodiscard]] const runtime::ProfilerSnapshot& current() const noexcept { return *current_; }

    // Takes a snapshot exactly as a feed delivery does (tests).
    void deliver(const runtime::ProfilerSnapshot& snapshot) { profilerSnapshotArrived(snapshot); }

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void profilerSnapshotArrived(const runtime::ProfilerSnapshot& snapshot) override;
    void subscribe();
    void unsubscribe();

    kit::KSection* addSection(QVBoxLayout* layout, QWidget* parent, const QString& id,
                              const QString& title);
    void buildMemory(QVBoxLayout* layout, QWidget* parent);
    void buildGpu(QVBoxLayout* layout, QWidget* parent);
    void buildCpu(QVBoxLayout* layout, QWidget* parent);
    void buildWorkload(QVBoxLayout* layout, QWidget* parent);
    void buildPressure(QVBoxLayout* layout, QWidget* parent);
    void buildProcesses(QVBoxLayout* layout, QWidget* parent);

    void refreshAll();
    void refreshMemory();
    void refreshGpu();
    void refreshCpu();
    void refreshWorkload();
    void refreshPressure();
    void refreshProcesses();

    [[nodiscard]] QVariant poolCell(int row, int column, int role) const;
    [[nodiscard]] QVariant taskCell(int row, int column, int role) const;
    [[nodiscard]] QVariant helperCell(int row, int column, int role) const;
    [[nodiscard]] QVariant eventCell(int row, int column, int role) const;

    QPointer<ProfilerFeed> feed_;
    std::unique_ptr<ProfilerHistory> history_;
    std::unique_ptr<runtime::ProfilerSnapshot> current_;
    // The stage histograms show samples since this panel's first delivery: cumulative counts at
    // that point are subtracted.
    std::unique_ptr<std::array<runtime::ProfilerMetric<runtime::ProfilerHistogram>,
                               runtime::kProfilerStageCount>>
        stageBase_;
    bool subscribed_ = false;

    struct Memory {
        kit::KSection* section = nullptr;
        kit::KTimeSeriesChart* chart = nullptr;
        kit::KReadoutGrid* readouts = nullptr;
        ProfilerTableModel* pools = nullptr;
        QTableView* poolTable = nullptr;
        int baselineMarker = -1;
        int firstPoolBand = 0;
    } memory_;
    struct Gpu {
        kit::KSection* section = nullptr;
        kit::KTimeSeriesChart* memory = nullptr;
        kit::KTimeSeriesChart* transfer = nullptr;
        kit::KReadoutGrid* readouts = nullptr;
        int budgetMarker = -1;
    } gpu_;
    struct Cpu {
        kit::KSection* section = nullptr;
        kit::KTimeSeriesChart* process = nullptr;
        kit::KTimeSeriesChart* executors = nullptr;
        kit::KReadoutGrid* readouts = nullptr;
    } cpu_;
    struct Workload {
        kit::KSection* section = nullptr;
        kit::KReadoutGrid* tasks = nullptr;
        ProfilerTableModel* taskModel = nullptr;
        QTableView* taskTable = nullptr;
        std::array<kit::KHistogramChart*, runtime::kProfilerStageCount> stages{};
        kit::KTimeSeriesChart* rates = nullptr;
        kit::KReadoutGrid* readouts = nullptr;
    } workload_;
    struct Pressure {
        kit::KSection* section = nullptr;
        kit::KTimeSeriesChart* chart = nullptr;
        kit::KReadoutGrid* readouts = nullptr;
        ProfilerTableModel* events = nullptr;
        QTableView* eventTable = nullptr;
    } pressure_;
    struct Processes {
        kit::KSection* section = nullptr;
        ProfilerTableModel* helpers = nullptr;
        QTableView* table = nullptr;
    } processes_;
    kit::KLabel* noSource_ = nullptr;
    // Owns every view; destroyed first, while the snapshot the tables read is still alive.
    QWidget* scroll_ = nullptr;
};

// Registers `bloom.profiler`. The feed must outlive every editor the registry creates; null
// registers the Profiler with no source.
[[nodiscard]] bool registerProfilerEditor(EditorRegistry& registry, ProfilerFeed* feed);

} // namespace bloom::ui
