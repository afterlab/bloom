#include "preview_frame_scopes_private.hpp"

#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

#include <algorithm>
#include <utility>

namespace bloom::runtime {
namespace {

// The color objects of a signal are kept for the last few signals a session measured.
constexpr std::size_t kColorCapacity = 8;

[[nodiscard]] std::string colorKey(const PreviewRequestIdentity& frame, const ScopeSignal signal) {
    const auto revision = frame.colorIntent.ocioConfigRevision.toLowercaseHex();
    std::string key(frame.colorIntent.ocioConfigUri);
    key += '|';
    key.append(revision.data(), revision.size());
    key += '|';
    key += frame.colorIntent.workingColorSpaceId;
    if (signal == ScopeSignal::Display) {
        key += "|display|";
        key += frame.displayName;
        key += '|';
        key += frame.viewName;
    } else {
        key += "|working";
    }
    return key;
}

[[nodiscard]] std::shared_ptr<const PreviewFrameScopes>
failedScopes(const ScopeMeasureSettings& settings, const render::ScopeTableReduction reduction,
             std::string message, std::shared_ptr<const ScopeSignalColor> color = {}) {
    auto scopes = std::make_shared<PreviewFrameScopes>();
    scopes->settings = settings;
    scopes->reduction = reduction;
    scopes->status = PreviewFrameScopesStatus::Failed;
    scopes->message = std::move(message);
    scopes->color = std::move(color);
    return scopes;
}

// #733: why a frame's preparation never counts the Display signal.
constexpr const char* kDisplayAtDisplayTime =
    "The Display signal is counted from the shown frame at display time, not with a frame's "
    "preparation.";

// What the device job's color program factories borrow.
struct ColorPrograms final {
    std::shared_ptr<const ScopeSignalColor> color;
    std::shared_ptr<const GpuSceneOcioContext> ocio;
};

} // namespace

std::shared_ptr<const ScopeSignalColor>
ScopeSignalColorCache::colorFor(const PreviewRequestIdentity& frame, const ScopeSignal signal,
                                std::string& message) {
    auto key = colorKey(frame, signal);
    {
        const std::lock_guard lock(mutex_);
        const auto found = std::ranges::find(entries_, key, &Entry::key);
        if (found != entries_.end()) {
            std::rotate(found, found + 1, entries_.end());
            return entries_.back().color;
        }
    }
    auto config = detail::resolveInputColorConfig(frame.colorIntent);
    if (!config) {
        message = "The project's color config is not available.";
        return nullptr;
    }
    auto prepared = ScopeSignalColor::prepare(
        {.config = std::make_shared<const color::ResolvedBloomNeutralConfig>(std::move(*config)),
         .signal = signal,
         .display = frame.displayName,
         .view = frame.viewName});
    if (!prepared) {
        message = std::move(prepared.message);
        return nullptr;
    }
    const std::lock_guard lock(mutex_);
    if (entries_.size() >= kColorCapacity)
        entries_.erase(entries_.begin());
    entries_.push_back({std::move(key), prepared.color});
    return prepared.color;
}

std::shared_ptr<const PreviewFrameScopes>
makePreviewFrameScopes(const ScopeMeasureSettings& settings, ScopeFrameMeasure measure,
                       std::shared_ptr<const ScopeSignalColor> color,
                       const render::ScopeTableReduction reduction) {
    switch (measure.status) {
    case ScopeFrameStatus::Measured: {
        auto scopes = std::make_shared<PreviewFrameScopes>();
        scopes->settings = settings;
        scopes->reduction = reduction;
        scopes->status = PreviewFrameScopesStatus::Measured;
        scopes->tables = ScopeTables::pack(measure.counts, measure.route);
        scopes->area = measure.area;
        scopes->color = std::move(color);
        scopes->tableReadbackBytes = measure.tableReadbackBytes;
        return scopes;
    }
    case ScopeFrameStatus::EmptyArea: {
        auto scopes = std::make_shared<PreviewFrameScopes>();
        scopes->settings = settings;
        scopes->reduction = reduction;
        scopes->status = PreviewFrameScopesStatus::EmptyArea;
        scopes->color = std::move(color);
        return scopes;
    }
    case ScopeFrameStatus::Cancelled:
        return failedScopes(settings, reduction, "The scope measure was cancelled.",
                            std::move(color));
    case ScopeFrameStatus::Unavailable:
    case ScopeFrameStatus::Failed:
        break;
    }
    return failedScopes(settings, reduction,
                        measure.message.empty() ? std::string("The scopes could not be counted.")
                                                : std::move(measure.message),
                        std::move(color));
}

bool previewFrameScopesAnswer(const PreviewFrameScopes* const scopes,
                              const PreviewScopeRequest& request) noexcept {
    return scopes != nullptr && scopes->settings == request.settings &&
           scopes->reduction == request.reduction;
}

std::shared_ptr<const PreviewFrameScopes>
measurePreviewFrameScopesCpu(const PreviewScopeRequest& request,
                             const PreviewRequestIdentity& identity, const ProcessFrame& frame,
                             const CancellationToken& cancellation) {
    if (request.settings.signal == ScopeSignal::Display)
        return failedScopes(request.settings, request.reduction, kDisplayAtDisplayTime);
    std::string message;
    auto color = request.colors != nullptr
                     ? request.colors->colorFor(identity, request.settings.signal, message)
                     : nullptr;
    if (color == nullptr)
        return failedScopes(request.settings, request.reduction,
                            message.empty() ? std::string("No scope color objects.")
                                            : std::move(message));
    auto measure =
        measureScopeFrameCpu(frame, request.settings, *color, cancellation, request.reduction);
    if (measure.status == ScopeFrameStatus::Cancelled || cancellation.isCancellationRequested())
        return nullptr;
    return makePreviewFrameScopes(request.settings, std::move(measure), std::move(color),
                                  request.reduction);
}

FrameScopeMeasurePreparation prepareFrameScopeMeasure(const PreviewScopeRequest& request,
                                                      const PreviewRequestIdentity& identity,
                                                      const PreparedGpuScene& scene,
                                                      const CompiledCompositionPlan& plan,
                                                      const EvaluationRequest& evaluation,
                                                      const CancellationToken& cancellation) {
    FrameScopeMeasurePreparation result;
    if (request.settings.signal == ScopeSignal::Display) {
        result.settled = failedScopes(request.settings, request.reduction, kDisplayAtDisplayTime);
        return result;
    }
    std::string message;
    auto color = request.colors != nullptr
                     ? request.colors->colorFor(identity, request.settings.signal, message)
                     : nullptr;
    if (color == nullptr) {
        // Nothing the CPU route could count either: the frame keeps the reason.
        result.settled = failedScopes(request.settings, request.reduction,
                                      message.empty() ? std::string("No scope color objects.")
                                                      : std::move(message));
        return result;
    }
    auto ocio = request.ocio ? request.ocio() : nullptr;
    if (ocio == nullptr) {
        result.message = "The shared color context the device's scope color steps need is not "
                         "available.";
        return result;
    }
    auto programs = std::make_shared<const ColorPrograms>(ColorPrograms{color, ocio});
    auto prepared = std::make_shared<PreparedFrameScopeMeasure>();
    prepared->settings = request.settings;
    prepared->reduction = request.reduction;
    prepared->color = color;
    prepared->work.job =
        makeScopeMeasureJob(request.settings, *programs->color, *programs->ocio, programs);
    prepared->work.job.reduction = request.reduction;
    const auto fault =
        detail::prepareScopeMeasure(prepared->work, scene, plan, evaluation, cancellation);
    if (cancellation.isCancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    if (fault) {
        result.message = fault->message.empty()
                             ? std::string("The device cannot count this frame's scopes.")
                             : fault->message;
        return result;
    }
    if (prepared->work.emptyArea) {
        auto scopes = std::make_shared<PreviewFrameScopes>();
        scopes->settings = request.settings;
        scopes->reduction = request.reduction;
        scopes->status = PreviewFrameScopesStatus::EmptyArea;
        scopes->color = std::move(color);
        result.settled = std::move(scopes);
        return result;
    }
    result.prepared = std::move(prepared);
    return result;
}

} // namespace bloom::runtime
