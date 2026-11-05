#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
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
#include <cstddef>

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

namespace {

static_assert(runtime::kGpuPreviewCpuFallbackReasonCount ==
              runtime::kProfilerCpuFallbackReasonCount);
static_assert(static_cast<std::size_t>(runtime::PreviewDisplayProvider::GpuPreparedPacked) + 1 ==
              runtime::kProfilerDisplayProviderCount);

runtime::ProfilerFrameFreshness profilerFreshness(const FrameFreshness freshness) {
    switch (freshness) {
    case FrameFreshness::Current:
        return runtime::ProfilerFrameFreshness::Current;
    case FrameFreshness::Stale:
        return runtime::ProfilerFrameFreshness::Stale;
    case FrameFreshness::None:
        break;
    }
    return runtime::ProfilerFrameFreshness::None;
}

// The nearest frame index of `time` on the composition's frame grid.
runtime::ProfilerCount frameIndex(const document::Composition* composition,
                                  const core::RationalTime time) {
    if (composition == nullptr)
        return {};
    const auto rate = composition->format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping.hasValue())
        return {};
    return runtime::ProfilerCount::of(mapping.value()->nearestFrameIndex(time));
}

} // namespace

void RuntimeProfilerFeed::publishUiInputs() {
    const auto now = std::chrono::steady_clock::now();
    if (sources_.ledger != nullptr) {
        runtime::readProfilerLedger(*sources_.ledger, inputs_.ledger, ledgerStateDirty_);
        ledgerStateDirty_ = false;
    }
    if (const auto* admission = sources_.admission) {
        const auto capacity = admission->usableCapacity();
        inputs_.deviceAdmissionClaimBytes = runtime::ProfilerBytes::of(capacity.deviceClaimBytes);
        inputs_.deviceUsableBytes = capacity.deviceReady
                                        ? runtime::ProfilerBytes::of(capacity.deviceUsableBytes)
                                        : runtime::ProfilerBytes{};
        inputs_.ledger.admissionClaimBytes =
            runtime::ProfilerBytes::of(capacity.hostClaimBytes > capacity.hostRetentionBytes
                                           ? capacity.hostClaimBytes - capacity.hostRetentionBytes
                                           : 0);
    }

    bool busy = false;
    if (const auto* preview = sources_.preview) {
        busy = busy || preview->playbackActive();
        inputs_.droppedFrames = preview->isCountingDroppedFrames()
                                    ? runtime::ProfilerCount::of(preview->droppedFrameCount())
                                    : runtime::ProfilerCount{};
        inputs_.playbackFps =
            rate(playbackRate_, preview->playbackActive() && sources_.session != nullptr,
                 playbackFrames_, now);
        const auto& state = preview->state();
        inputs_.viewer.freshness = runtime::ProfilerMetric<runtime::ProfilerFrameFreshness>::of(
            profilerFreshness(state.freshness));
        inputs_.viewer.provider =
            state.frame != nullptr ? runtime::ProfilerMetric<runtime::ProfilerDisplayProvider>::of(
                                         static_cast<runtime::ProfilerDisplayProvider>(
                                             state.frame->provenance().provider))
                                   : runtime::ProfilerMetric<runtime::ProfilerDisplayProvider>{};
        const auto* composition =
            sources_.session != nullptr ? sources_.session->composition() : nullptr;
        inputs_.viewer.frame = state.frame != nullptr
                                   ? frameIndex(composition, state.frame->desiredIdentity().time)
                                   : runtime::ProfilerCount{};
        inputs_.viewer.playheadFrame =
            sources_.session != nullptr ? frameIndex(composition, sources_.session->currentTime())
                                        : runtime::ProfilerCount{};
    }

    if (const auto* service = sources_.previewService) {
        const auto counters = service->status().counters;
        auto& viewer = inputs_.viewer;
        viewer.cpuFallbacks = runtime::ProfilerCount::of(counters.cpuFallbacks);
        for (std::size_t reason = 0; reason < viewer.cpuFallbacksByReason.size(); ++reason)
            viewer.cpuFallbacksByReason[reason] =
                runtime::ProfilerCount::of(counters.cpuFallbacksByReason[reason]);
        viewer.gpuAdmissionRefusals = runtime::ProfilerCount::of(counters.gpuAdmissionRefusals);
        viewer.gpuCompletions = runtime::ProfilerCount::of(counters.residentCompletions);
        viewer.capacityWaits = runtime::ProfilerCount::of(counters.capacityWaits);
    }

    if (const auto* ramPreview = sources_.ramPreview) {
        busy = busy || ramPreview->isCaching();
        const auto cached = ramPreview->cachedFrameCount();
        inputs_.ramPreviewCachedFrames = runtime::ProfilerCount::of(cached);
        inputs_.ramPreviewFillFps = rate(fillRate_, ramPreview->isCaching(), cached, now);
        inputs_.ramPreviewCaching = runtime::ProfilerMetric<bool>::of(ramPreview->isCaching());
        inputs_.ramPreviewTotalFrames = runtime::ProfilerCount::of(ramPreview->totalFrameCount());
        const auto run = ramPreview->longestRetainedRun();
        inputs_.ramPreviewSpanFirstFrame = runtime::ProfilerCount::of(run ? run->first : 0);
        inputs_.ramPreviewSpanFrames =
            runtime::ProfilerCount::of(run ? run->second - run->first : 0);
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
        busy = busy || completed.has_value();
        inputs_.exportFps = rate(exportRate_, completed.has_value(), completed.value_or(0), now);
    }
    inputs_.hostBusy = busy;

    sampler_.publishHostInputs(inputs_);
}

} // namespace bloom::ui
