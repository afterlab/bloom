// The Display signal tables of the frame the Viewer shows (#733). See shown_frame_scopes.hpp.

#include <bloom/runtime/shown_frame_scopes.hpp>

#include "gpu_preview_display_service_private.hpp"

#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <mutex>
#include <utility>

namespace bloom::runtime {

namespace detail {

struct ShownFrameScopeMeasurerState final {
    ShownFrameScopeMeasurer::Options options;

    mutable std::mutex mutex;
    bool closed = false;
    GpuShownFrameScopePort device;
    // The request that waits for the measure in flight to end, with its serial.
    std::optional<ShownFrameScopeRequest> waiting;
    std::uint64_t waitingSerial = 0;
    std::uint64_t nextSerial = 1;
    // Requests up to this serial were cleared: their results are not published.
    std::uint64_t clearedThrough = 0;
    bool inFlight = false;
    std::optional<ShownFrameScopes> finished;
    ShownFrameScopeCounters counters;

    // Held around `options.resultReady`, and by the destructor when it closes the measurer, so no
    // call is running or starts once the destructor returned.
    std::mutex callbackMutex;
};

} // namespace detail

namespace {

using State = detail::ShownFrameScopeMeasurerState;
using StatePtr = std::shared_ptr<State>;

// One request on its way through a measure.
struct Run final {
    ShownFrameScopeRequest request;
    std::uint64_t serial = 0;
    std::shared_ptr<const ScopeSignalColor> color;
    std::optional<ScopeStoredFrame> stored;
    // Keeps `stored->cells` alive.
    std::shared_ptr<const void> owner;
};

// What the device job's XYZ program factory borrows.
struct ColorPrograms final {
    std::shared_ptr<const ScopeSignalColor> color;
    std::shared_ptr<const GpuSceneOcioContext> ocio;
};

[[nodiscard]] bool cleared(const State& state, const std::uint64_t serial) {
    const std::lock_guard lock(state.mutex);
    return state.closed || serial <= state.clearedThrough;
}

[[nodiscard]] std::shared_ptr<const PreviewFrameScopes>
failedScopes(const ScopeMeasureSettings& settings, std::string message,
             std::shared_ptr<const ScopeSignalColor> color = {},
             const render::ScopeTableReduction reduction = {}) {
    ScopeFrameMeasure measure;
    measure.status = ScopeFrameStatus::Failed;
    measure.message = std::move(message);
    return makePreviewFrameScopes(settings, std::move(measure), std::move(color), reduction);
}

void publish(const StatePtr& state, const Run& run, ShownFrameScopes result) {
    {
        const std::lock_guard lock(state->mutex);
        if (state->closed || run.serial <= state->clearedThrough)
            return;
        if (result.scopes != nullptr && result.scopes->status == PreviewFrameScopesStatus::Measured) {
            if (result.scopes->tables.route() == ScopeRoute::Gpu)
                ++state->counters.deviceMeasures;
            else
                ++state->counters.cpuMeasures;
        } else {
            ++state->counters.unmeasured;
        }
        state->finished = std::move(result);
    }
    const std::lock_guard lock(state->callbackMutex);
    bool closed = false;
    {
        const std::lock_guard stateLock(state->mutex);
        closed = state->closed;
    }
    if (!closed && state->options.resultReady)
        state->options.resultReady();
}

[[nodiscard]] ShownFrameScopes resultOf(const Run& run,
                                        std::shared_ptr<const PreviewFrameScopes> scopes) {
    ShownFrameScopes result;
    result.frame = run.request.frame;
    result.tag = run.request.tag;
    result.reduction = run.request.reduction;
    result.scopes = std::move(scopes);
    return result;
}

void countOnWorker(const StatePtr& state, const Run& run, const CancellationToken& cancellation,
                   std::string deviceMessage) {
    if (state->options.beforeCpuCount)
        state->options.beforeCpuCount();
    if (cleared(*state, run.serial))
        return;
    auto measure =
        measureScopeStoredFrameCpu(*run.stored, run.request.settings, run.request.reduction,
                                   *run.color, [&state, &run, &cancellation] {
                                       return cancellation.isCancellationRequested() ||
                                              cleared(*state, run.serial);
                                   });
    if (measure.status == ScopeFrameStatus::Cancelled)
        return;
    auto result =
        resultOf(run, makePreviewFrameScopes(run.request.settings, std::move(measure), run.color,
                                             run.request.reduction));
    result.deviceMessage = std::move(deviceMessage);
    publish(state, run, std::move(result));
}

void launch(const StatePtr& state);
void continueAfterDevice(const StatePtr& state, std::shared_ptr<Run> run,
                         detail::ShownScopeJobOutcome outcome);

// Resolves the request's color objects and its stored frame. False when the request ended here
// (its result, if any, is published).
[[nodiscard]] bool resolve(const StatePtr& state, Run& run) {
    const auto& request = run.request;
    const bool storedGiven =
        request.stored.has_value() && request.storedOwner != nullptr &&
        request.storedColor != nullptr && request.storedColor->signal() == ScopeSignal::Display;
    if ((request.frame == nullptr && !storedGiven) ||
        request.settings.signal != ScopeSignal::Display || request.settings.axisMaximum != 1U) {
        publish(state, run,
                resultOf(run, failedScopes(request.settings,
                                           "The shown-frame measure counts the Display signal of "
                                           "a frame.")));
        return false;
    }
    if (request.frame == nullptr) {
        run.stored = request.stored;
        run.owner = request.storedOwner;
        run.color = request.storedColor;
        return true;
    }
    std::string message;
    run.color = state->options.colors != nullptr
                    ? state->options.colors->colorFor(request.frame->desiredIdentity(),
                                                      ScopeSignal::Display, message)
                    : nullptr;
    if (run.color == nullptr) {
        publish(state, run,
                resultOf(run, failedScopes(request.settings,
                                           message.empty() ? std::string("No scope color objects.")
                                                           : std::move(message))));
        return false;
    }
    const auto view = request.frame->displayBufferView();
    if (!view) {
        publish(state, run,
                resultOf(run, failedScopes(request.settings,
                                           "The shown frame is held on the device only: it has "
                                           "no stored display cells to count.",
                                           run.color)));
        return false;
    }
    run.stored = ScopeStoredFrame{.cells = view->pixels,
                                  .layout = view->pixelLayout,
                                  .dataWindow = view->dataWindow.value_or(view->displayWindow),
                                  .displayWindow = view->displayWindow,
                                  .pixelAspect = view->pixelAspect,
                                  .roi = request.frame->desiredIdentity().roi,
                                  .compositionWidth = request.compositionWidth,
                                  .compositionHeight = request.compositionHeight};
    run.owner = request.frame;
    return true;
}

// Posts the measure to the device. True when it was posted: its continuation ends the run. False
// with `message`: the worker counts the frame.
[[nodiscard]] bool postToDevice(const StatePtr& state, const std::shared_ptr<Run>& run,
                                const CancellationToken& cancellation, std::string& message,
                                bool& ended) {
    GpuShownFrameScopePort port;
    {
        const std::lock_guard lock(state->mutex);
        port = state->device;
    }
    const auto core = detail::ShownFrameScopePortAccess::core(port);
    if (core == nullptr) {
        message = "No device counts shown frames in this session.";
        return false;
    }
    const auto& request = run->request;
    const bool chromaticity = (request.settings.scopes & render::kScopeTableChromaticity) != 0U;
    auto job = std::make_shared<detail::ShownFrameScopeJob>();
    job->owner = run->owner;
    job->cells = run->stored->cells;
    job->layout = run->stored->layout;
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        run->stored->dataWindow, run->stored->displayWindow, run->stored->pixelAspect);
    if (!descriptor) {
        message = "The shown frame's windows do not describe a picture.";
        return false;
    }
    job->descriptor = *descriptor.value();
    auto& work = job->work;
    work.job.scopes = request.settings.scopes;
    work.job.axisMaximum = request.settings.axisMaximum;
    work.job.luma = run->color->luma();
    work.job.ignoreTransparent = request.settings.ignoreTransparent;
    work.job.region = request.settings.region;
    work.job.reduction = request.reduction;
    if (chromaticity) {
        auto ocio = state->options.ocio ? state->options.ocio() : nullptr;
        if (ocio == nullptr) {
            message = "The shared color context the device's CIE XYZ step needs is not "
                      "available.";
            return false;
        }
        auto programs = std::make_shared<const ColorPrograms>(ColorPrograms{run->color, ocio});
        // The cells are the Display signal already: only the XYZ program is taken.
        work.job.xyzCommand = makeScopeMeasureJob(request.settings, *programs->color,
                                                  *programs->ocio, programs)
                                  .xyzCommand;
    }
    const auto fault = detail::prepareScopeMeasureForPicture(
        work, run->stored->dataWindow, run->stored->displayWindow, run->stored->roi,
        run->stored->compositionWidth, run->stored->compositionHeight, cancellation);
    if (cancellation.isCancellationRequested() || cleared(*state, run->serial)) {
        ended = true;
        return false;
    }
    if (fault) {
        message = fault->message.empty() ? std::string("The device cannot count this frame's "
                                                       "scopes.")
                                         : fault->message;
        return false;
    }
    if (work.emptyArea) {
        ScopeFrameMeasure measure;
        measure.status = ScopeFrameStatus::EmptyArea;
        publish(state, *run,
                resultOf(*run, makePreviewFrameScopes(request.settings, std::move(measure),
                                                      run->color, request.reduction)));
        ended = true;
        return false;
    }
    job->phase = detail::reserveShownFrameScopePhase(
        *core, job->cells.size(), detail::scopeMeasureTableBytes(work.job), chromaticity);
    if (core->options.processingPhasePolicy && !job->phase) {
        message = "The device has no room for the scope measure now.";
        return false;
    }
    job->done = [state, run](detail::ShownScopeJobOutcome outcome) {
        // Owner thread: the rest runs on a worker.
        continueAfterDevice(state, run, std::move(outcome));
    };
    if (!detail::postShownFrameScopes(core, job)) {
        job->done = {};
        message = "The preview device is stopping.";
        return false;
    }
    return true;
}

// Takes the waiting request, or ends the flight when none waits.
[[nodiscard]] std::shared_ptr<Run> takeWaiting(State& state) {
    const std::lock_guard lock(state.mutex);
    if (state.closed || !state.waiting) {
        state.inFlight = false;
        state.waiting.reset();
        return nullptr;
    }
    auto run = std::make_shared<Run>();
    run->request = std::move(*state.waiting);
    run->serial = state.waitingSerial;
    state.waiting.reset();
    return run;
}

void endFlight(State& state) {
    const std::lock_guard lock(state.mutex);
    state.inFlight = false;
}

// After a measure ended: a request that waits gets a task of its own, so work the scheduler holds
// for frames is never kept behind a run of measures; with none waiting the flight ends.
void continueFlight(const StatePtr& state) {
    {
        const std::lock_guard lock(state->mutex);
        if (state->closed || !state->waiting) {
            state->inFlight = false;
            state->waiting.reset();
            return;
        }
    }
    launch(state);
}

// Worker: measures the waiting request.
void runOne(const StatePtr& state, const CancellationToken& cancellation) {
    if (cancellation.isCancellationRequested()) {
        endFlight(*state);
        return;
    }
    auto run = takeWaiting(*state);
    if (run == nullptr)
        return;
    try {
        if (resolve(state, *run)) {
            std::string message;
            bool ended = false;
            if (postToDevice(state, run, cancellation, message, ended))
                return; // The flight goes on in the device job's continuation.
            if (!ended) {
                {
                    const std::lock_guard lock(state->mutex);
                    if (detail::ShownFrameScopePortAccess::core(state->device) != nullptr)
                        ++state->counters.deviceDrops;
                }
                countOnWorker(state, *run, cancellation, std::move(message));
            }
        }
    } catch (...) {
        publish(state, *run,
                resultOf(*run, failedScopes(run->request.settings,
                                            "The scope measure could not allocate.")));
    }
    continueFlight(state);
}

template <typename Body> [[nodiscard]] bool submitWorker(const StatePtr& state, Body body) {
    if (state->options.scheduler == nullptr)
        return false;
    TaskRequest task("Count scopes of the shown frame", state->options.owner,
                     TaskPriority::Background);
    auto submission = state->options.scheduler->submit<void>(
        std::move(task), [state, body = std::move(body)](TaskContext& context) mutable {
            body(context.cancellation());
            return TaskResult<void>::succeeded();
        });
    return submission.accepted();
}

void launch(const StatePtr& state) {
    if (!submitWorker(state, [state](const CancellationToken& cancellation) {
            runOne(state, cancellation);
        }))
        endFlight(*state);
}

void continueAfterDevice(const StatePtr& state, std::shared_ptr<Run> run,
                         detail::ShownScopeJobOutcome outcome) {
    auto shared = std::make_shared<detail::ShownScopeJobOutcome>(std::move(outcome));
    const bool submitted =
        submitWorker(state, [state, run, shared](const CancellationToken& cancellation) {
            auto& result = *shared;
            try {
                if (result.status == detail::ShownScopeJobStatus::Measured && result.counts) {
                    ScopeFrameMeasure measure;
                    measure.status = ScopeFrameStatus::Measured;
                    measure.route = ScopeRoute::Gpu;
                    measure.counts = std::move(*result.counts);
                    measure.colorDispatches = result.colorDispatches;
                    measure.tableReadbacks = result.tableReadbacks;
                    measure.tableReadbackBytes = result.tableReadbackBytes;
                    if (const auto area = scopeMeasuredArea(
                            run->stored->dataWindow, run->stored->displayWindow, run->stored->roi,
                            run->request.settings.region, run->stored->compositionWidth,
                            run->stored->compositionHeight))
                        measure.area = *area;
                    auto published = resultOf(
                        *run, makePreviewFrameScopes(run->request.settings, std::move(measure),
                                                     run->color, run->request.reduction));
                    published.uploads = result.uploads;
                    published.colorDispatches = result.colorDispatches;
                    published.tableReadbacks = result.tableReadbacks;
                    publish(state, *run, std::move(published));
                } else {
                    bool newer = false;
                    {
                        const std::lock_guard lock(state->mutex);
                        ++state->counters.deviceDrops;
                        newer = state->waiting.has_value();
                    }
                    // A newer frame waits: it is the one to count. Otherwise this frame is the
                    // one shown, and the CPU reference counts it.
                    if (!newer)
                        countOnWorker(state, *run, cancellation, std::move(result.message));
                }
            } catch (...) {
                publish(state, *run,
                        resultOf(*run, failedScopes(run->request.settings,
                                                    "The scope measure could not allocate.")));
            }
            continueFlight(state);
        });
    if (!submitted)
        endFlight(*state);
}

} // namespace

ShownFrameScopeMeasurer::ShownFrameScopeMeasurer(Options options)
    : state_(std::make_shared<detail::ShownFrameScopeMeasurerState>()) {
    state_->device = options.device;
    state_->options = std::move(options);
}

ShownFrameScopeMeasurer::~ShownFrameScopeMeasurer() {
    const std::lock_guard callbackLock(state_->callbackMutex);
    const std::lock_guard lock(state_->mutex);
    state_->closed = true;
    state_->waiting.reset();
    state_->finished.reset();
}

void ShownFrameScopeMeasurer::show(ShownFrameScopeRequest request) {
    bool start = false;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->closed)
            return;
        ++state_->counters.requested;
        if (state_->waiting)
            ++state_->counters.replaced;
        state_->waiting = std::move(request);
        state_->waitingSerial = state_->nextSerial++;
        if (!state_->inFlight) {
            state_->inFlight = true;
            start = true;
        }
    }
    if (start)
        launch(state_);
}

void ShownFrameScopeMeasurer::clear() {
    const std::lock_guard lock(state_->mutex);
    state_->waiting.reset();
    state_->finished.reset();
    state_->clearedThrough = state_->nextSerial - 1;
}

void ShownFrameScopeMeasurer::setDevice(GpuShownFrameScopePort device) {
    const std::lock_guard lock(state_->mutex);
    state_->device = std::move(device);
}

std::optional<ShownFrameScopes> ShownFrameScopeMeasurer::take() {
    const std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->finished);
    state_->finished.reset();
    return result;
}

bool ShownFrameScopeMeasurer::busy() const {
    const std::lock_guard lock(state_->mutex);
    return state_->inFlight || state_->waiting.has_value();
}

ShownFrameScopeCounters ShownFrameScopeMeasurer::counters() const {
    const std::lock_guard lock(state_->mutex);
    return state_->counters;
}

} // namespace bloom::runtime
