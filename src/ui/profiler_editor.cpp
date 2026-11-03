#include <bloom/ui/profiler_editor.hpp>

#include "profiler_rows.hpp"
#include "profiler_table_model.hpp"
#include "profiler_text.hpp"

#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/chart.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/readout.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/profiler_history.hpp>

#include <QGridLayout>
#include <QHeaderView>
#include <QScrollArea>
#include <QTableView>
#include <QVBoxLayout>

#include <array>

namespace bloom::ui {
namespace {

using namespace profiler;
using Series = ProfilerHistory::Series;

kit::KLabel* addHeading(QVBoxLayout* const layout, QWidget* const parent, const QString& text,
                        const QString& name) {
    auto* label = new kit::KLabel(text, parent, kit::TypeRole::UiSmallStrong);
    label->setObjectName(name);
    label->setTone(kit::Color::Muted);
    layout->addWidget(label);
    return label;
}

kit::KTimeSeriesChart* addChart(QVBoxLayout* const layout, QWidget* const parent,
                                const QString& name, const QString& accessibleName,
                                const kit::MetricUnit unit) {
    auto* chart = new kit::KTimeSeriesChart(parent);
    chart->setObjectName(name);
    chart->setAccessibleName(accessibleName);
    chart->setUnit(unit);
    chart->setSpan(ProfilerHistory::kCapacity);
    layout->addWidget(chart);
    return chart;
}

kit::KReadoutGrid* addReadouts(QVBoxLayout* const layout, QWidget* const parent,
                               const QString& name) {
    auto* grid = new kit::KReadoutGrid(parent);
    grid->setObjectName(name);
    layout->addWidget(grid);
    return grid;
}

} // namespace

ProfilerEditor::ProfilerEditor(ProfilerFeed* const feed, QWidget* const parent)
    : QWidget(parent), feed_(feed), history_(std::make_unique<ProfilerHistory>()),
      current_(std::make_unique<runtime::ProfilerSnapshot>()),
      stageBase_(std::make_unique<std::array<runtime::ProfilerMetric<runtime::ProfilerHistogram>,
                                             runtime::kProfilerStageCount>>()) {
    setObjectName(QStringLiteral("profilerEditor"));
    setAccessibleName(tr("Profiler"));
    setAccessibleDescription(
        tr("Bloom's memory, GPU, CPU, workload, pressure and helper processes over time"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("profilerScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    scroll_ = scroll;
    auto* surface = new kit::KSurface(nullptr, kit::Color::Background);
    surface->setObjectName(QStringLiteral("profilerContent"));
    auto* content = new QVBoxLayout(surface);
    content->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::Gutter),
                                kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS));
    content->setSpacing(kit::px(kit::Spacing::Gutter));
    scroll->setWidget(surface);

    if (feed_ == nullptr) {
        noSource_ = new kit::KLabel(
            tr("No profiler source is connected, so every metric reads Unavailable."), surface,
            kit::TypeRole::Ui);
        noSource_->setObjectName(QStringLiteral("profilerNoSource"));
        noSource_->setTone(kit::Color::Muted);
        noSource_->setWordWrap(true);
        content->addWidget(noSource_);
    }
    buildMemory(content, surface);
    buildGpu(content, surface);
    buildCpu(content, surface);
    buildWorkload(content, surface);
    buildPressure(content, surface);
    buildProcesses(content, surface);
    content->addStretch(1);
    refreshAll();
}

ProfilerEditor::~ProfilerEditor() {
    unsubscribe();
    // The views read current_ and history_ through their models; they go before those members.
    delete scroll_;
    scroll_ = nullptr;
}

void ProfilerEditor::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    subscribe();
}

void ProfilerEditor::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    unsubscribe();
}

void ProfilerEditor::subscribe() {
    if (subscribed_ || feed_ == nullptr)
        return;
    subscribed_ = true;
    feed_->subscribe(*this);
}

void ProfilerEditor::unsubscribe() {
    if (!subscribed_)
        return;
    subscribed_ = false;
    if (feed_ != nullptr)
        feed_->unsubscribe(*this);
}

kit::KSection* ProfilerEditor::addSection(QVBoxLayout* const layout, QWidget* const parent,
                                          const QString& id, const QString& title) {
    auto* section = new kit::KSection(title, parent);
    section->setObjectName(QStringLiteral("profilerSection_") + id);
    section->setPersistenceKey(QStringLiteral("profiler/sections/%1/collapsed").arg(id));
    section->setResetEnabled(false);
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    section->bodyLayout()->setSpacing(kit::px(kit::Spacing::XS));
    layout->addWidget(section);
    // A collapsed section skips its refresh; opening it again shows the current sample at once.
    connect(section, &kit::KSection::collapsedChanged, this, [this](const bool collapsed) {
        if (!collapsed)
            refreshAll();
    });
    return section;
}

void ProfilerEditor::buildMemory(QVBoxLayout* const layout, QWidget* const parent) {
    auto& view = memory_;
    view.section = addSection(layout, parent, QStringLiteral("memory"), tr("Memory"));
    auto* body = view.section->bodyLayout();
    auto* host = view.section->body();
    view.chart = addChart(body, host, QStringLiteral("profilerMemoryChart"), tr("Memory over time"),
                          kit::MetricUnit::Bytes);
    const auto& history = *history_;
    // Unattributed sits at the bottom of the stack so its top reads straight against the idle
    // baseline marker; the stack's top is anonymous memory.
    view.chart->addBand(&history.series(Series::Unattributed), kit::Color::Warn);
    view.chart->addBand(&history.series(Series::Staging), kit::Color::GraphCurve1);
    view.chart->addBand(&history.series(Series::Claims), kit::Color::ComponentW);
    view.firstPoolBand = view.chart->bandCount();
    for (int slot = 0; slot < ProfilerHistory::kPoolSeries; ++slot)
        view.chart->addBand(&history.pool(slot), kit::chartSeriesColor(slot));
    view.chart->addLine(&history.series(Series::Anon), kit::Color::Muted, true);
    view.chart->addLine(&history.series(Series::Rss), kit::Color::Foreground);
    view.baselineMarker = view.chart->addMarker(tr("Idle baseline"), kit::Color::Muted);

    view.readouts = addReadouts(body, host, QStringLiteral("profilerMemoryReadouts"));
    auto* grid = view.readouts;
    (void)grid->addRow(tr("Resident (RSS)"), kit::Color::Foreground);
    (void)grid->addRow(tr("Anonymous"), kit::Color::Muted);
    (void)grid->addRow(tr("File-backed"));
    (void)grid->addRow(tr("Shared memory"));
    (void)grid->addRow(tr("RSS growth"));
    (void)grid->addRow(tr("Unattributed"), kit::Color::Warn);
    (void)grid->addRow(tr("Idle baseline"));
    (void)grid->addRow(tr("Staging"), kit::Color::GraphCurve1);
    (void)grid->addRow(tr("Admission claims"), kit::Color::ComponentW);
    (void)grid->addRow(tr("Pools"));
    (void)grid->addRow(tr("Ledger configured"));
    (void)grid->addRow(tr("Ledger effective"));
    (void)grid->addRow(tr("Ledger retained"));
    (void)grid->addRow(tr("Headroom"));
    (void)grid->addRow(tr("Usable"));
    (void)grid->addRow(tr("Physical memory"));
    (void)grid->addRow(tr("Available"));
    (void)grid->addRow(tr("Swap used"));
    (void)grid->addRow(tr("Bloom in swap"));
    (void)grid->addRow(tr("Memory pressure"));
    const int last = grid->addRow(tr("Swap pressure"));
    Q_ASSERT(last == SwapPressureRow);
    (void)last;

    addHeading(body, host, tr("Pools"), QStringLiteral("profilerPoolsHeading"));
    view.pools = new ProfilerTableModel(
        {tr("Pool"), tr("Kind"), tr("Host"), tr("Pending"), tr("Device"), tr("Entries"),
         tr("Ceiling"), tr("Hits"), tr("Misses"), tr("Stores"), tr("Evictions"), tr("Evicted"),
         tr("Refusals")},
        0x1FFCU,
        [this] {
            return static_cast<int>(
                std::min<std::size_t>(current_->memory.poolCount, runtime::kProfilerMaximumPools));
        },
        [this](const int row, const int column, const int role) {
            return poolCell(row, column, role);
        },
        this);
    view.poolTable = makeProfilerTable(*view.pools, QStringLiteral("profilerPoolTable"),
                                       tr("Ledger pools"), host);
    body->addWidget(view.poolTable);
}

void ProfilerEditor::buildGpu(QVBoxLayout* const layout, QWidget* const parent) {
    auto& view = gpu_;
    view.section = addSection(layout, parent, QStringLiteral("gpu"), tr("GPU"));
    auto* body = view.section->bodyLayout();
    auto* host = view.section->body();
    const auto& history = *history_;
    view.memory = addChart(body, host, QStringLiteral("profilerGpuMemoryChart"),
                           tr("GPU memory over time"), kit::MetricUnit::Bytes);
    view.memory->addBand(&history.series(Series::GpuLive), kit::chartSeriesColor(0));
    view.memory->addBand(&history.series(Series::GpuRetiring), kit::chartSeriesColor(1));
    view.memory->addLine(&history.series(Series::GpuHeapUsage), kit::Color::Foreground);
    view.budgetMarker = view.memory->addMarker(tr("Heap budget"), kit::Color::Muted);
    view.transfer = addChart(body, host, QStringLiteral("profilerGpuTransferChart"),
                             tr("GPU transfers over time"), kit::MetricUnit::BytesPerSecond);
    view.transfer->addLine(&history.series(Series::GpuUploadRate), kit::chartSeriesColor(2));
    view.transfer->addLine(&history.series(Series::GpuReadbackRate), kit::chartSeriesColor(3));

    view.readouts = addReadouts(body, host, QStringLiteral("profilerGpuReadouts"));
    auto* grid = view.readouts;
    (void)grid->addRow(tr("Bloom live"), kit::chartSeriesColor(0));
    (void)grid->addRow(tr("Bloom retiring"), kit::chartSeriesColor(1));
    (void)grid->addRow(tr("Driver heap use"), kit::Color::Foreground);
    (void)grid->addRow(tr("Driver heap budget"));
    (void)grid->addRow(tr("Bloom share of heap use"));
    (void)grid->addRow(tr("Utilization"));
    (void)grid->addRow(tr("Upload"), kit::chartSeriesColor(2));
    (void)grid->addRow(tr("Readback"), kit::chartSeriesColor(3));
    (void)grid->addRow(tr("Queue submits"));
    (void)grid->addRow(tr("Admission refusals"));
    const int last = grid->addRow(tr("Reclaims"));
    Q_ASSERT(last == GpuReclaimsRow);
    (void)last;
}

void ProfilerEditor::buildCpu(QVBoxLayout* const layout, QWidget* const parent) {
    auto& view = cpu_;
    view.section = addSection(layout, parent, QStringLiteral("cpu"), tr("CPU"));
    auto* body = view.section->bodyLayout();
    auto* host = view.section->body();
    const auto& history = *history_;
    view.process = addChart(body, host, QStringLiteral("profilerCpuChart"),
                            tr("Process CPU over time"), kit::MetricUnit::Cores);
    view.process->addLine(&history.series(Series::ProcessCores), kit::Color::Foreground);
    view.executors = addChart(body, host, QStringLiteral("profilerExecutorChart"),
                              tr("Executor pool utilization over time"), kit::MetricUnit::Ratio);
    view.executors->setScaleMaximum(1.0);
    for (int slot = 0; slot < ProfilerHistory::kExecutorSeries; ++slot)
        view.executors->addLine(&history.executor(slot), kit::chartSeriesColor(slot));

    view.readouts = addReadouts(body, host, QStringLiteral("profilerCpuReadouts"));
    (void)view.readouts->addRow(tr("Process"), kit::Color::Foreground);
    for (int slot = 0; slot < ProfilerHistory::kExecutorSeries; ++slot) {
        const int row = view.readouts->addRow(QString{}, kit::chartSeriesColor(slot));
        view.readouts->setRowVisible(row, false);
    }
}

void ProfilerEditor::buildWorkload(QVBoxLayout* const layout, QWidget* const parent) {
    auto& view = workload_;
    view.section = addSection(layout, parent, QStringLiteral("workload"), tr("Workload"));
    auto* body = view.section->bodyLayout();
    auto* host = view.section->body();

    view.tasks = addReadouts(body, host, QStringLiteral("profilerTaskCounts"));
    (void)view.tasks->addRow(tr("Queued"));
    (void)view.tasks->addRow(tr("Running"));
    (void)view.tasks->addRow(tr("Succeeded"));
    (void)view.tasks->addRow(tr("Cancelled"));
    (void)view.tasks->addRow(tr("Failed"));
    (void)view.tasks->addRow(tr("By owner"));
    const int last = view.tasks->addRow(tr("By executor"));
    Q_ASSERT(last == ExecutorsRow);
    (void)last;

    addHeading(body, host, tr("Tasks"), QStringLiteral("profilerTasksHeading"));
    view.taskModel = new ProfilerTableModel(
        {tr("Task"), tr("Owner"), tr("Executor"), tr("State"), tr("Progress"), tr("Age")}, 0x30U,
        [this] {
            return static_cast<int>(std::min<std::size_t>(current_->workload.taskCount,
                                                          runtime::kProfilerMaximumTasks));
        },
        [this](const int row, const int column, const int role) {
            return taskCell(row, column, role);
        },
        this);
    view.taskTable = makeProfilerTable(*view.taskModel, QStringLiteral("profilerTaskTable"),
                                       tr("Active and queued tasks"), host);
    body->addWidget(view.taskTable);

    addHeading(body, host, tr("Stage timings"), QStringLiteral("profilerStagesHeading"));
    auto* stages = new QGridLayout;
    stages->setContentsMargins(0, 0, 0, 0);
    stages->setSpacing(kit::px(kit::Spacing::XS));
    std::array<double, runtime::kProfilerHistogramEdgeCount> edges{};
    for (std::size_t edge = 0; edge < edges.size(); ++edge)
        edges[edge] = static_cast<double>(runtime::profilerHistogramEdgeNs(edge));
    for (std::size_t stage = 0; stage < runtime::kProfilerStageCount; ++stage) {
        auto* chart =
            new kit::KHistogramChart(stageText(static_cast<runtime::ProfilerStage>(stage)), host);
        chart->setObjectName(QStringLiteral("profilerStage_%1").arg(stage));
        chart->setUnit(kit::MetricUnit::Duration);
        chart->setUpperEdges(edges);
        view.stages[stage] = chart;
        stages->addWidget(chart, static_cast<int>(stage / 2), static_cast<int>(stage % 2));
    }
    body->addLayout(stages);

    addHeading(body, host, tr("Throughput"), QStringLiteral("profilerThroughputHeading"));
    const auto& history = *history_;
    view.rates = addChart(body, host, QStringLiteral("profilerThroughputChart"),
                          tr("Playback, RAM Preview and export rates over time"),
                          kit::MetricUnit::FramesPerSecond);
    view.rates->addLine(&history.series(Series::PlaybackFps), kit::chartSeriesColor(0));
    view.rates->addLine(&history.series(Series::RamPreviewFillFps), kit::chartSeriesColor(1));
    view.rates->addLine(&history.series(Series::ExportFps), kit::chartSeriesColor(2));
    view.readouts = addReadouts(body, host, QStringLiteral("profilerThroughputReadouts"));
    (void)view.readouts->addRow(tr("Playback"), kit::chartSeriesColor(0));
    (void)view.readouts->addRow(tr("Dropped frames"));
    (void)view.readouts->addRow(tr("RAM Preview cached"));
    (void)view.readouts->addRow(tr("RAM Preview fill"), kit::chartSeriesColor(1));
    const int lastRate = view.readouts->addRow(tr("Export"), kit::chartSeriesColor(2));
    Q_ASSERT(lastRate == ExportRow);
    (void)lastRate;
}

void ProfilerEditor::buildPressure(QVBoxLayout* const layout, QWidget* const parent) {
    auto& view = pressure_;
    view.section = addSection(layout, parent, QStringLiteral("pressure"), tr("Pressure"));
    auto* body = view.section->bodyLayout();
    auto* host = view.section->body();
    const auto& history = *history_;
    view.chart =
        addChart(body, host, QStringLiteral("profilerPressureChart"),
                 tr("Admission waits and refusals over time"), kit::MetricUnit::CountPerSecond);
    view.chart->addLine(&history.series(Series::AdmissionWaitRate), kit::chartSeriesColor(0));
    view.chart->addLine(&history.series(Series::AdmissionRefusalRate), kit::chartSeriesColor(1));

    view.readouts = addReadouts(body, host, QStringLiteral("profilerPressureReadouts"));
    auto* grid = view.readouts;
    (void)grid->addRow(tr("Memory pressure"));
    (void)grid->addRow(tr("Swap pressure"));
    (void)grid->addRow(tr("Swap used"));
    (void)grid->addRow(tr("Bloom in swap"));
    (void)grid->addRow(tr("Admission waits"), kit::chartSeriesColor(0));
    (void)grid->addRow(tr("Admission refusals"), kit::chartSeriesColor(1));
    for (std::size_t cause = 0; cause < runtime::kProfilerEvictionCauseCount; ++cause)
        (void)grid->addRow(evictionRowText(static_cast<runtime::ProfilerEvictionCause>(cause)));
    (void)grid->addRow(tr("Eviction rate"));

    addHeading(body, host, tr("Events"), QStringLiteral("profilerEventsHeading"));
    view.events = new ProfilerTableModel(
        {tr("When"), tr("Event")}, 0U, [this] { return history_->eventCount(); },
        [this](const int row, const int column, const int role) {
            return eventCell(row, column, role);
        },
        this);
    view.eventTable = makeProfilerTable(*view.events, QStringLiteral("profilerEventTable"),
                                        tr("Pressure events, newest first"), host);
    view.eventTable->horizontalHeader()->setSectionResizeMode(EventWhenColumn,
                                                              QHeaderView::ResizeToContents);
    view.eventTable->horizontalHeader()->setSectionResizeMode(EventTextColumn,
                                                              QHeaderView::Stretch);
    body->addWidget(view.eventTable);
}

void ProfilerEditor::buildProcesses(QVBoxLayout* const layout, QWidget* const parent) {
    auto& view = processes_;
    view.section = addSection(layout, parent, QStringLiteral("processes"), tr("Processes"));
    auto* body = view.section->bodyLayout();
    auto* host = view.section->body();
    view.helpers = new ProfilerTableModel(
        {tr("Process"), tr("PID"), tr("Resident (RSS)"), tr("CPU time"), tr("State")}, 0x0EU,
        [this] {
            return static_cast<int>(std::min<std::size_t>(current_->processes.helperCount,
                                                          runtime::kProfilerMaximumHelpers));
        },
        [this](const int row, const int column, const int role) {
            return helperCell(row, column, role);
        },
        this);
    view.table = makeProfilerTable(*view.helpers, QStringLiteral("profilerHelperTable"),
                                   tr("Helper processes"), host);
    body->addWidget(view.table);
}

bool registerProfilerEditor(EditorRegistry& registry, ProfilerFeed* const feed) {
    QPointer<ProfilerFeed> guarded(feed);
    return registry.registerEditor({.id = "bloom.profiler",
                                    .displayName = ProfilerEditor::tr("Profiler"),
                                    .create = [guarded](QWidget* parent) {
                                        return new ProfilerEditor(guarded.data(), parent);
                                    }});
}

} // namespace bloom::ui
