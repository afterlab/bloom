#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/profiler_gpu_reader.hpp>
#include <bloom/runtime/profiler_ledger_adapter.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/memory_budget_controller.hpp>
#include <bloom/ui/profiler_runtime_feed.hpp>
#include <bloom/ui/ram_preview_controller.hpp>

#include <algorithm>

namespace bloom::ui {

void readProfilerGpuImageStats(runtime::ProfilerGpu& gpu) noexcept {
    runtime::readProfilerGpuDevice(gpu);
}

RuntimeProfilerFeed::RuntimeProfilerFeed(runtime::ProfilerSampler& sampler,
                                         const ProfilerUiSources sources, QObject* parent)
    : ProfilerFeed(parent), sampler_(sampler), sources_(sources) {}

RuntimeProfilerFeed::~RuntimeProfilerFeed() { shutdown(); }

void RuntimeProfilerFeed::activate() {
    ledgerStateDirty_ = true;
    playbackRate_ = {};
    fillRate_ = {};
    exportRate_ = {};
    if (sources_.memoryBudget != nullptr) {
        const auto markDirty = [this] { ledgerStateDirty_ = true; };
        connections_[0] =
            connect(sources_.memoryBudget, &MemoryBudgetController::sampleApplied, this, markDirty);
        connections_[1] = connect(sources_.memoryBudget, &MemoryBudgetController::pressureApplied,
                                  this, markDirty);
    }
    if (sources_.session != nullptr && sources_.preview != nullptr)
        connections_[2] =
            connect(sources_.session, &CompositionSession::currentTimeChanged, this, [this] {
                if (sources_.preview->playbackActive())
                    ++playbackFrames_;
            });
    // The first sample, taken as the thread starts, already sees the UI-owned values.
    publishUiInputs();
    sampler_.setInterval(interval());
    sampler_.acquire();
}

void RuntimeProfilerFeed::deactivate() {
    sampler_.release();
    for (auto& connection : connections_)
        disconnect(connection);
}

bool RuntimeProfilerFeed::readLatest(runtime::ProfilerSnapshot& out) {
    publishUiInputs();
    return sampler_.readLatest(out, out.serial);
}

runtime::ProfilerRatio RuntimeProfilerFeed::rate(Rate& previous, const bool active,
                                                 const std::uint64_t count,
                                                 const std::chrono::steady_clock::time_point now) {
    if (!active) {
        previous = {};
        return {};
    }
    runtime::ProfilerRatio result;
    if (previous.count && now > previous.at && count >= *previous.count) {
        const auto seconds = std::chrono::duration<double>(now - previous.at).count();
        result = runtime::ProfilerRatio::of(
            static_cast<float>(static_cast<double>(count - *previous.count) / seconds));
    }
    previous = {count, now};
    return result;
}

void RuntimeProfilerFeed::publishUiInputs() {
    const auto now = std::chrono::steady_clock::now();
    if (sources_.ledger != nullptr) {
        runtime::readProfilerLedger(*sources_.ledger, inputs_.ledger, ledgerStateDirty_);
        ledgerStateDirty_ = false;
    }
    if (const auto* admission = sources_.admission) {
        const auto capacity = admission->usableCapacity();
        inputs_.ledger.admissionClaimBytes =
            runtime::ProfilerBytes::of(capacity.hostClaimBytes > capacity.hostRetentionBytes
                                           ? capacity.hostClaimBytes - capacity.hostRetentionBytes
                                           : 0);
    }

    if (const auto* preview = sources_.preview) {
        inputs_.droppedFrames = preview->isCountingDroppedFrames()
                                    ? runtime::ProfilerCount::of(preview->droppedFrameCount())
                                    : runtime::ProfilerCount{};
        inputs_.playbackFps =
            rate(playbackRate_, preview->playbackActive() && sources_.session != nullptr,
                 playbackFrames_, now);
    }

    if (const auto* ramPreview = sources_.ramPreview) {
        const auto cached = ramPreview->cachedFrameCount();
        inputs_.ramPreviewCachedFrames = runtime::ProfilerCount::of(cached);
        inputs_.ramPreviewFillFps = rate(fillRate_, ramPreview->isCaching(), cached, now);
    }

    if (const auto* exportQueue = sources_.exportQueue) {
        std::optional<std::uint64_t> completed;
        // A pointer copy of the adapter's last snapshot; nothing is formatted or rebuilt.
        if (const auto snapshot = exportQueue->snapshot();
            snapshot != nullptr && snapshot->runner == host::ExportRunnerState::Running &&
            snapshot->activeJob) {
            const auto job =
                std::ranges::find(snapshot->jobs, *snapshot->activeJob, &host::ExportJobView::id);
            if (job != snapshot->jobs.end())
                completed = job->progress.framesCompleted;
        }
        inputs_.exportFps = rate(exportRate_, completed.has_value(), completed.value_or(0), now);
    }

    sampler_.publishHostInputs(inputs_);
}

} // namespace bloom::ui
