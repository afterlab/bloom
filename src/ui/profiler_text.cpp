#include "profiler_text.hpp"

#include <bloom/ui/kit/metrics.hpp>

#include <QCoreApplication>

namespace bloom::ui::profiler {
namespace {

QString tr(const char* source) {
    return QCoreApplication::translate("bloom::ui::Profiler", source);
}

} // namespace

QString text(const runtime::ProfilerName& name) {
    const auto view = runtime::profilerNameView(name);
    return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
}

QString poolKindText(const runtime::ProfilerPoolKind kind) {
    switch (kind) {
    case runtime::ProfilerPoolKind::Scratch:
        return tr("Scratch");
    case runtime::ProfilerPoolKind::Cache:
        return tr("Cache");
    case runtime::ProfilerPoolKind::Live:
        return tr("Live");
    }
    return {};
}

QString taskStateText(const runtime::TaskState state) {
    switch (state) {
    case runtime::TaskState::Queued:
        return tr("Queued");
    case runtime::TaskState::Running:
        return tr("Running");
    case runtime::TaskState::Succeeded:
        return tr("Succeeded");
    case runtime::TaskState::Cancelled:
        return tr("Cancelled");
    case runtime::TaskState::Failed:
        return tr("Failed");
    }
    return {};
}

QString ownerKindText(const runtime::TaskOwnerKind kind) {
    switch (kind) {
    case runtime::TaskOwnerKind::Application:
        return tr("Application");
    case runtime::TaskOwnerKind::Project:
        return tr("Project");
    case runtime::TaskOwnerKind::Composition:
        return tr("Composition");
    case runtime::TaskOwnerKind::PanelRequest:
        return tr("Panel request");
    case runtime::TaskOwnerKind::Export:
        return tr("Export");
    }
    return {};
}

QString executorText(const runtime::TaskExecutor executor) {
    switch (executor) {
    case runtime::TaskExecutor::Cpu:
        return tr("CPU");
    case runtime::TaskExecutor::BlockingIo:
        return tr("Blocking I/O");
    case runtime::TaskExecutor::Gpu:
        return tr("GPU service");
    }
    return {};
}

QString stageText(const runtime::ProfilerStage stage) {
    switch (stage) {
    case runtime::ProfilerStage::Decode:
        return tr("Decode");
    case runtime::ProfilerStage::ColourConvert:
        return tr("Color convert");
    case runtime::ProfilerStage::Upload:
        return tr("Upload");
    case runtime::ProfilerStage::Render:
        return tr("Render");
    case runtime::ProfilerStage::Transfer:
        return tr("Transfer");
    case runtime::ProfilerStage::Encode:
        return tr("Encode");
    }
    return {};
}

QString evictionCauseText(const runtime::ProfilerEvictionCause cause) {
    switch (cause) {
    case runtime::ProfilerEvictionCause::Ceiling:
        return tr("over its ceiling");
    case runtime::ProfilerEvictionCause::MemoryPressure:
        return tr("for memory pressure");
    case runtime::ProfilerEvictionCause::Admission:
        return tr("to admit work");
    case runtime::ProfilerEvictionCause::Purge:
        return tr("by a purge");
    }
    return {};
}

QString evictionRowText(const runtime::ProfilerEvictionCause cause) {
    switch (cause) {
    case runtime::ProfilerEvictionCause::Ceiling:
        return tr("Evicted (ceiling)");
    case runtime::ProfilerEvictionCause::MemoryPressure:
        return tr("Evicted (pressure)");
    case runtime::ProfilerEvictionCause::Admission:
        return tr("Evicted (admission)");
    case runtime::ProfilerEvictionCause::Purge:
        return tr("Evicted (purge)");
    }
    return {};
}

QString eventText(const ProfilerHistory::Event& event) {
    using Kind = ProfilerHistory::EventKind;
    switch (event.kind) {
    case Kind::MemoryPressureBegan:
        return tr("Memory pressure began");
    case Kind::MemoryPressureEnded:
        return tr("Memory pressure ended");
    case Kind::SwapPressureBegan:
        return tr("Swap pressure began");
    case Kind::SwapPressureEnded:
        return tr("Swap pressure ended");
    case Kind::AdmissionRefused:
        return (event.value == 1.0 ? tr("Admission refused %1 request")
                                   : tr("Admission refused %1 requests"))
            .arg(kit::formatMetric(event.value, kit::MetricUnit::Count));
    case Kind::GpuRefused:
        return (event.value == 1.0 ? tr("GPU admission refused %1 request")
                                   : tr("GPU admission refused %1 requests"))
            .arg(kit::formatMetric(event.value, kit::MetricUnit::Count));
    case Kind::Evicted:
        return tr("Evicted %1 %2")
            .arg(kit::formatMetric(event.value, kit::MetricUnit::Bytes),
                 evictionCauseText(static_cast<runtime::ProfilerEvictionCause>(event.detail)));
    case Kind::HelperExited:
        return tr("Helper process %1 exited").arg(static_cast<qlonglong>(event.value));
    }
    return {};
}

QString agoText(const std::int64_t ageNs) {
    return tr("%1 s ago").arg(QString::number(static_cast<double>(ageNs) / 1e9, 'f', 1));
}

} // namespace bloom::ui::profiler
