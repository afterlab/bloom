#include "gpu_process_frame_scope_private.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <limits>
#include <thread>
#include <utility>

namespace bloom::runtime::detail {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] ScopeMeasureFault fault(const GpuProcessFrameStatus status,
                                      const GpuProcessFrameDiagnosticCode code,
                                      std::string message) {
    return {status, code, std::move(message)};
}

[[nodiscard]] ScopeMeasureFault makeFault(const GpuProcessFrameStatus status,
                                          const GpuProcessFrameDiagnosticCode code,
                                          std::string message) {
    return {status, code, std::move(message)};
}

[[nodiscard]] ScopeMeasureFault colorFault(const GpuOcioExecutorDiagnostic& diagnostic) {
    using Code = GpuOcioExecutorDiagnosticCode;
    switch (diagnostic.code) {
    case Code::Cancelled:
        return fault(GpuProcessFrameStatus::Cancelled, GpuProcessFrameDiagnosticCode::Cancelled,
                     diagnostic.message);
    case Code::OverBudget:
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::OverBudget,
                     diagnostic.message);
    case Code::DeviceLost:
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::DeviceLost,
                     diagnostic.message);
    case Code::NativeTimeout:
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::DeadlineExceeded,
                     diagnostic.message);
    case Code::Unsupported:
    case Code::ProgramRefused:
        return fault(GpuProcessFrameStatus::UnsupportedGpuSubset,
                     GpuProcessFrameDiagnosticCode::SceneUnsupported, diagnostic.message);
    case Code::WrongThread:
    case Code::DeviceUnavailable:
    case Code::Busy:
    case Code::OwnerDrainRequired:
        return fault(GpuProcessFrameStatus::Failed,
                     GpuProcessFrameDiagnosticCode::DeviceUnavailable, diagnostic.message);
    case Code::InvalidArgument:
    case Code::IdentityMismatch:
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InvalidRequest,
                     diagnostic.message);
    case Code::None:
    case Code::DispatchRefused:
    case Code::InternalInvariant:
        break;
    }
    return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InternalInvariant,
                 diagnostic.message.empty() ? std::string("the scope color step failed")
                                            : diagnostic.message);
}

[[nodiscard]] ScopeMeasureFault tablesFault(const render::GpuScopeTablesDiagnostic& diagnostic) {
    using Code = render::GpuScopeTablesCode;
    switch (diagnostic.code) {
    case Code::Cancelled:
        return fault(GpuProcessFrameStatus::Cancelled, GpuProcessFrameDiagnosticCode::Cancelled,
                     diagnostic.message);
    case Code::OverBudget:
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::OverBudget,
                     diagnostic.message);
    case Code::DeviceLost:
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::DeviceLost,
                     diagnostic.message);
    case Code::ShaderRejected:
        return fault(GpuProcessFrameStatus::UnsupportedGpuSubset,
                     GpuProcessFrameDiagnosticCode::SceneUnsupported, diagnostic.message);
    case Code::WrongThread:
    case Code::DeviceUnavailable:
    case Code::Busy:
        return fault(GpuProcessFrameStatus::Failed,
                     GpuProcessFrameDiagnosticCode::DeviceUnavailable, diagnostic.message);
    case Code::InvalidArgument:
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InvalidRequest,
                     diagnostic.message);
    case Code::MeasureFailed:
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::ReadbackFailed,
                     diagnostic.message);
    case Code::None:
        break;
    }
    return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InternalInvariant,
                 "the scope count kernel failed");
}

} // namespace

std::uint64_t scopeMeasureTableBytes(const GpuScopeMeasureJob& job) noexcept {
    // The widest Waveform of the detail: `512 >> detail` columns.
    const std::uint32_t detail =
        job.detail <= render::kScopeMaxDetail ? job.detail : render::kScopeMaxDetail;
    const auto layout =
        render::scopeTableLayout(job.scopes, render::kScopeWaveformMaxColumns >> detail, detail);
    return 3ULL * layout.byteCount();
}

bool scopeMeasureUsesColorProgram(const GpuScopeMeasureJob& job) noexcept {
    return static_cast<bool>(job.displayCommand) ||
           (job.scopes & render::kScopeTableChromaticity) != 0U;
}

std::optional<ScopeMeasureFault> prepareScopeMeasure(ScopeMeasureWork& work,
                                                     const PreparedGpuScene& scene,
                                                     const CompiledCompositionPlan& plan,
                                                     const EvaluationRequest& request,
                                                     const CancellationToken& cancellation) {
    const auto& descriptor = scene.outputDescriptor();
    return prepareScopeMeasureForPicture(work, descriptor.dataWindow(), descriptor.displayWindow(),
                                         request.roi, plan.format().width(),
                                         plan.format().height(), cancellation);
}

std::optional<ScopeMeasureFault> prepareScopeMeasureForPicture(
    ScopeMeasureWork& work, const render::ImageWindow& dataWindow,
    const render::ImageWindow& displayWindow, const std::optional<render::ImageWindow>& roi,
    const std::uint32_t compositionWidth, const std::uint32_t compositionHeight,
    const CancellationToken& cancellation) {
    const auto& job = work.job;
    const std::uint32_t scopes = job.scopes & render::kScopeTableAll;
    if (scopes == 0U || !validScopeAxisMaximum(job.axisMaximum) ||
        !render::validScopeDetail(job.detail)) {
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InvalidRequest,
                     "the scope measure names no scope, an axis maximum that is not valid or a "
                     "detail above 2");
    }
    const bool chromaticity = (scopes & render::kScopeTableChromaticity) != 0U;
    if (chromaticity && !job.xyzCommand) {
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InvalidRequest,
                     "the Chromaticity table needs the signal's XYZ program");
    }
    const auto area = scopeMeasuredArea(dataWindow, displayWindow, roi, job.region,
                                        compositionWidth, compositionHeight);
    if (!area) {
        work.emptyArea = true;
        return std::nullopt;
    }
    work.area = *area;
    const auto extent = dataWindow.extent();
    constexpr std::uint64_t limit = std::numeric_limits<std::uint32_t>::max();
    if (extent.width() == 0 || extent.height() == 0 || extent.width() > limit ||
        extent.height() > limit || static_cast<std::uint64_t>(area->width) * area->height > limit) {
        return fault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::InvalidRequest,
                     "the picture is outside the extent a scope measure counts");
    }
    const GpuOcioCommandGeometry geometry{.width = static_cast<std::uint32_t>(extent.width()),
                                          .height = static_cast<std::uint32_t>(extent.height())};
    const auto prepare = [&](const GpuScopeCommandFactory& factory,
                             std::shared_ptr<const PreparedGpuOcioCommand>& command,
                             const char* const step) -> std::optional<ScopeMeasureFault> {
        command = factory(geometry, cancellation);
        if (cancellation.isCancellationRequested()) {
            return fault(GpuProcessFrameStatus::Cancelled, GpuProcessFrameDiagnosticCode::Cancelled,
                         "cancelled while the scope color programs were prepared");
        }
        if (command == nullptr) {
            return fault(GpuProcessFrameStatus::UnsupportedGpuSubset,
                         GpuProcessFrameDiagnosticCode::SceneUnsupported,
                         std::string("the device has no program for the scope's ") + step);
        }
        if (command->encoding() != GpuOcioOutputEncoding::FinalRgba32f ||
            command->geometry() != geometry) {
            return fault(GpuProcessFrameStatus::Failed,
                         GpuProcessFrameDiagnosticCode::InvalidRequest,
                         std::string("the program of the scope's ") + step +
                             " is not a float program of the picture's extent");
        }
        return std::nullopt;
    };
    if (job.displayCommand) {
        if (auto failed = prepare(job.displayCommand, work.display, "display transform"))
            return failed;
    }
    if (chromaticity) {
        if (auto failed = prepare(job.xyzCommand, work.xyz, "XYZ conversion"))
            return failed;
    }
    return std::nullopt;
}

ScopeMeasureStage::ScopeMeasureStage(std::unique_ptr<GpuOcioProgramExecutor> color) noexcept
    : color_(std::move(color)) {}

std::unique_ptr<ScopeMeasureStage> ScopeMeasureStage::create(render::GpuDevice& device,
                                                             std::string& message) {
    auto created = GpuOcioProgramExecutor::create(device);
    if (!created) {
        message = created.diagnostic.message;
        return nullptr;
    }
    return std::unique_ptr<ScopeMeasureStage>(new ScopeMeasureStage(std::move(created.executor)));
}

bool ScopeMeasureStage::hasUnretiredSubmission() noexcept {
    return color_->hasUnretiredSubmission() || tables_.hasUnretiredSubmission();
}

void ScopeMeasureStage::poll() noexcept {
    try {
        if (color_->hasUnretiredSubmission())
            static_cast<void>(color_->poll());
    } catch (...) {
        // A poll that could not report is retried by the next turn.
    }
    static_cast<void>(tables_.poll());
}

std::uint64_t ScopeMeasureStage::reclaimPrograms(const std::uint64_t bytes) noexcept {
    return color_->reclaim(bytes);
}

ScopeMeasureFault runScopeMeasure(ScopeMeasureStage& stage, ScopeMeasureWork& work,
                                  std::shared_ptr<const render::GpuImage> picture,
                                  const std::uint64_t byteBudget,
                                  const std::chrono::milliseconds nativeDeadline,
                                  const CancellationToken& cancellation,
                                  const std::atomic_bool& stopRequested,
                                  bool& nativeRetirementProven) {
    nativeRetirementProven = true;
    const auto cancelled = [&] {
        return cancellation.isCancellationRequested() || stopRequested.load();
    };
    auto& color = stage.color();
    const auto dispatchesBefore = color.counters().dispatches;

    // One color step: the resident `input` through `command` into a resident float picture.
    const auto runColor =
        [&](const std::shared_ptr<const PreparedGpuOcioCommand>& command,
            const std::shared_ptr<const render::GpuImage>& input,
            std::shared_ptr<const render::GpuImage>& output) -> std::optional<ScopeMeasureFault> {
        nativeRetirementProven = false; // begin may upload a LUT
        const auto accepted = color.begin(command, input, {}, byteBudget);
        if (accepted.code != GpuOcioExecutorDiagnosticCode::None) {
            nativeRetirementProven = !color.hasUnretiredSubmission();
            return colorFault(accepted);
        }
        const auto deadline = std::chrono::steady_clock::now() + nativeDeadline;
        auto polled = GpuOcioExecutorPollResult::Pending;
        bool expired = false;
        for (;;) {
            if (cancelled())
                color.cancel();
            polled = color.poll();
            if (polled != GpuOcioExecutorPollResult::Pending)
                break;
            if (std::chrono::steady_clock::now() >= deadline) {
                expired = true;
                color.cancel();
                const auto drain = std::chrono::steady_clock::now() + nativeDeadline;
                while (std::chrono::steady_clock::now() < drain) {
                    polled = color.poll();
                    if (polled != GpuOcioExecutorPollResult::Pending)
                        break;
                    std::this_thread::sleep_for(50us);
                }
                break;
            }
            std::this_thread::sleep_for(50us);
        }
        if (polled != GpuOcioExecutorPollResult::Ready) {
            nativeRetirementProven =
                polled != GpuOcioExecutorPollResult::Pending && !color.hasUnretiredSubmission();
            if (cancelled())
                return fault(GpuProcessFrameStatus::Cancelled,
                             GpuProcessFrameDiagnosticCode::Cancelled,
                             "cancelled during the scope color step");
            if (expired)
                return fault(GpuProcessFrameStatus::Failed,
                             GpuProcessFrameDiagnosticCode::DeadlineExceeded,
                             "the scope color step did not retire within the request deadline");
            return colorFault(color.diagnostic());
        }
        nativeRetirementProven = true;
        output = color.takeEffectOutput();
        if (output == nullptr)
            return fault(GpuProcessFrameStatus::Failed,
                         GpuProcessFrameDiagnosticCode::InternalInvariant,
                         "the scope color step reported Ready without a picture");
        return std::nullopt;
    };

    if (cancelled())
        return fault(GpuProcessFrameStatus::Cancelled, GpuProcessFrameDiagnosticCode::Cancelled,
                     "cancelled before the scope measure");
    std::shared_ptr<const render::GpuImage> signal = std::move(picture);
    if (work.display != nullptr) {
        std::shared_ptr<const render::GpuImage> displayed;
        if (auto failed = runColor(work.display, signal, displayed))
            return *failed;
        signal = std::move(displayed);
    }
    std::shared_ptr<const render::GpuImage> xyz;
    if (work.xyz != nullptr) {
        if (auto failed = runColor(work.xyz, signal, xyz))
            return *failed;
    }
    work.colorDispatches = color.counters().dispatches - dispatchesBefore;

    const auto& job = work.job;
    const render::GpuScopeTablesRequest request{
        .signal = signal,
        .xyz = xyz,
        .scopes = job.scopes & render::kScopeTableAll,
        .areaX = work.area.x,
        .areaY = work.area.y,
        .areaWidth = work.area.width,
        .areaHeight = work.area.height,
        .inverseAxisMaximum = 1.0F / static_cast<float>(job.axisMaximum),
        .lumaRed = job.luma.red,
        .lumaGreen = job.luma.green,
        .lumaBlue = job.luma.blue,
        .cbDenominator = job.luma.cbDenominator,
        .crDenominator = job.luma.crDenominator,
        .ignoreTransparent = job.ignoreTransparent,
        .detail = job.detail,
    };
    auto& tables = stage.tables();
    nativeRetirementProven = false;
    if (!tables.begin(request, byteBudget)) {
        nativeRetirementProven = !tables.hasUnretiredSubmission();
        return tablesFault(tables.diagnostic());
    }
    const auto deadline = std::chrono::steady_clock::now() + nativeDeadline;
    auto state = render::GpuScopeTablesState::Pending;
    bool expired = false;
    for (;;) {
        if (cancelled())
            tables.cancel();
        state = tables.poll();
        if (state != render::GpuScopeTablesState::Pending)
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            expired = true;
            tables.cancel();
            const auto drain = std::chrono::steady_clock::now() + nativeDeadline;
            while (std::chrono::steady_clock::now() < drain) {
                state = tables.poll();
                if (state != render::GpuScopeTablesState::Pending)
                    break;
                std::this_thread::sleep_for(50us);
            }
            break;
        }
        std::this_thread::sleep_for(50us);
    }
    if (state != render::GpuScopeTablesState::Ready) {
        nativeRetirementProven =
            state != render::GpuScopeTablesState::Pending && !tables.hasUnretiredSubmission();
        if (cancelled())
            return fault(GpuProcessFrameStatus::Cancelled, GpuProcessFrameDiagnosticCode::Cancelled,
                         "cancelled during the scope measure; no table was published");
        if (expired)
            return fault(GpuProcessFrameStatus::Failed,
                         GpuProcessFrameDiagnosticCode::DeadlineExceeded,
                         "the scope measure did not retire within the request deadline");
        return tablesFault(tables.diagnostic());
    }
    nativeRetirementProven = true;
    auto result = tables.take();
    if (result.words.size() != result.layout.wordCount || result.words.empty())
        return fault(GpuProcessFrameStatus::Failed,
                     GpuProcessFrameDiagnosticCode::InternalInvariant,
                     "the scope measure reported Ready without its tables");
    work.counts = ScopeCounts{.layout = result.layout, .words = std::move(result.words)};
    work.tableReadbacks = 1;
    work.tableReadbackBytes = work.counts->layout.byteCount();
    return fault(GpuProcessFrameStatus::Evaluated, GpuProcessFrameDiagnosticCode::None, {});
}


ScopeMeasureStepper::ScopeMeasureStepper(ScopeMeasureStage& stage, ScopeMeasureWork& work,
                                         std::shared_ptr<const render::GpuImage> picture,
                                         const std::uint64_t byteBudget) noexcept
    : stage_(stage), work_(work), signal_(std::move(picture)), byteBudget_(byteBudget),
      dispatchesBefore_(stage.color().counters().dispatches) {}

void ScopeMeasureStepper::settle(ScopeMeasureFault fault) noexcept {
    fault_ = std::move(fault);
    phase_ = Phase::Settled;
    retirementProven_ = !stage_.hasUnretiredSubmission();
}

// A failure with a submission that may not be retired: keep polling it until its fence is proven.
void ScopeMeasureStepper::failAndDrain(ScopeMeasureFault fault) noexcept {
    fault_ = std::move(fault);
    if (stage_.hasUnretiredSubmission()) {
        phase_ = Phase::Draining;
        return;
    }
    phase_ = Phase::Settled;
    retirementProven_ = true;
}

ScopeMeasureFault ScopeMeasureStepper::stopFault(const bool cancelRequested) const {
    if (cancelRequested)
        return makeFault(GpuProcessFrameStatus::Cancelled, GpuProcessFrameDiagnosticCode::Cancelled,
                     "cancelled during the scope measure; no table was published");
    return makeFault(GpuProcessFrameStatus::Failed, GpuProcessFrameDiagnosticCode::DeadlineExceeded,
                 "the scope measure did not retire within the request deadline");
}

ScopeMeasureStepper::State ScopeMeasureStepper::step(const bool cancelRequested,
                                                     const bool deadlineExpired,
                                                     const Hooks& hooks) noexcept {
    if (phase_ == Phase::Settled)
        return State::Settled;
    try {
        return advance(cancelRequested || deadlineExpired, cancelRequested, hooks);
    } catch (...) {
        failAndDrain(makeFault(GpuProcessFrameStatus::Failed,
                           GpuProcessFrameDiagnosticCode::InternalInvariant,
                           "the scope measure raised"));
        return phase_ == Phase::Settled ? State::Settled : State::Running;
    }
}

ScopeMeasureStepper::State ScopeMeasureStepper::advance(const bool stop,
                                                        const bool cancelRequested,
                                                        const Hooks& hooks) {
    auto& color = stage_.color();
    auto& tables = stage_.tables();
    if (stop && !stopIssued_) {
        stopIssued_ = true;
        if (phase_ == Phase::ColorPoll)
            color.cancel();
        else if (phase_ == Phase::TablesPoll)
            tables.cancel();
    }
    switch (phase_) {
    case Phase::Next: {
        if (stop) {
            // Nothing of this measure is in flight between submissions.
            settle(stopFault(cancelRequested));
            return State::Settled;
        }
        const bool colorStep =
            (work_.display != nullptr && !displayDone_) || (work_.xyz != nullptr && !xyzDone_);
        if (hooks.mayBegin && !hooks.mayBegin())
            return State::Running;
        if (colorStep) {
            const bool display = work_.display != nullptr && !displayDone_;
            began_ = true;
            const auto accepted =
                color.begin(display ? work_.display : work_.xyz, signal_, {}, byteBudget_);
            if (accepted.code != GpuOcioExecutorDiagnosticCode::None) {
                failAndDrain(colorFault(accepted));
                return phase_ == Phase::Settled ? State::Settled : State::Running;
            }
            phase_ = Phase::ColorPoll;
            return State::Running;
        }
        if (!colorDispatchCounted_) {
            colorDispatchCounted_ = true;
            work_.colorDispatches = color.counters().dispatches - dispatchesBefore_;
        }
        const auto& job = work_.job;
        const render::GpuScopeTablesRequest request{
            .signal = signal_,
            .xyz = xyz_,
            .scopes = job.scopes & render::kScopeTableAll,
            .areaX = work_.area.x,
            .areaY = work_.area.y,
            .areaWidth = work_.area.width,
            .areaHeight = work_.area.height,
            .inverseAxisMaximum = 1.0F / static_cast<float>(job.axisMaximum),
            .lumaRed = job.luma.red,
            .lumaGreen = job.luma.green,
            .lumaBlue = job.luma.blue,
            .cbDenominator = job.luma.cbDenominator,
            .crDenominator = job.luma.crDenominator,
            .ignoreTransparent = job.ignoreTransparent,
            .detail = job.detail,
        };
        began_ = true;
        if (!tables.begin(request, byteBudget_)) {
            failAndDrain(tablesFault(tables.diagnostic()));
            return phase_ == Phase::Settled ? State::Settled : State::Running;
        }
        phase_ = Phase::TablesPoll;
        return State::Running;
    }
    case Phase::ColorPoll: {
        const auto polled = color.poll();
        if (polled == GpuOcioExecutorPollResult::Pending)
            return State::Running;
        if (polled != GpuOcioExecutorPollResult::Ready) {
            failAndDrain(stop ? stopFault(cancelRequested) : colorFault(color.diagnostic()));
            return phase_ == Phase::Settled ? State::Settled : State::Running;
        }
        auto output = color.takeEffectOutput();
        if (output == nullptr) {
            failAndDrain(makeFault(GpuProcessFrameStatus::Failed,
                               GpuProcessFrameDiagnosticCode::InternalInvariant,
                               "the scope color step reported Ready without a picture"));
            return phase_ == Phase::Settled ? State::Settled : State::Running;
        }
        if (work_.display != nullptr && !displayDone_) {
            displayDone_ = true;
            signal_ = std::move(output);
        } else {
            xyzDone_ = true;
            xyz_ = std::move(output);
        }
        phase_ = Phase::Next;
        return State::Running;
    }
    case Phase::TablesPoll: {
        const auto state = hooks.pollTables ? hooks.pollTables(tables) : tables.poll();
        if (state == render::GpuScopeTablesState::Pending)
            return State::Running;
        if (state != render::GpuScopeTablesState::Ready) {
            failAndDrain(stop ? stopFault(cancelRequested) : tablesFault(tables.diagnostic()));
            return phase_ == Phase::Settled ? State::Settled : State::Running;
        }
        auto result = tables.take();
        if (result.words.size() != result.layout.wordCount || result.words.empty()) {
            failAndDrain(makeFault(GpuProcessFrameStatus::Failed,
                               GpuProcessFrameDiagnosticCode::InternalInvariant,
                               "the scope measure reported Ready without its tables"));
            return phase_ == Phase::Settled ? State::Settled : State::Running;
        }
        work_.counts = ScopeCounts{.layout = result.layout, .words = std::move(result.words)};
        work_.tableReadbacks = 1;
        work_.tableReadbackBytes = work_.counts->layout.byteCount();
        settle(makeFault(GpuProcessFrameStatus::Evaluated, GpuProcessFrameDiagnosticCode::None, {}));
        return State::Settled;
    }
    case Phase::Draining: {
        stage_.poll();
        if (!stage_.hasUnretiredSubmission()) {
            phase_ = Phase::Settled;
            retirementProven_ = true;
            return State::Settled;
        }
        return State::Running;
    }
    case Phase::Settled:
        return State::Settled;
    }
    return State::Settled;
}

} // namespace bloom::runtime::detail

namespace bloom::runtime {

GpuScopeMeasureOutcome GpuProcessFrameEvaluator::measureScopes(
    std::shared_ptr<const CompiledCompositionPlan> plan, const EvaluationRequest& request,
    const GpuScopeMeasureJob& job, const CancellationToken& cancellation,
    const std::optional<std::filesystem::path>& assetBaseDirectory) {
    auto work = std::make_shared<detail::ScopeMeasureWork>();
    work->job = job;
    auto frame =
        evaluateWith(std::move(plan), request, cancellation, {}, nullptr, assetBaseDirectory, work);
    GpuScopeMeasureOutcome outcome;
    outcome.diagnostic = std::move(frame.diagnostic);
    outcome.area = work->area;
    outcome.colorDispatches = work->colorDispatches;
    outcome.tableReadbacks = work->tableReadbacks;
    outcome.tableReadbackBytes = work->tableReadbackBytes;
    outcome.pictureReadbacks = frame.counters.readbacks;
    using Code = GpuProcessFrameDiagnosticCode;
    if (work->emptyArea) {
        outcome.status = GpuScopeMeasureStatus::EmptyArea;
        outcome.diagnostic = {};
    } else if (frame.status == GpuProcessFrameStatus::Evaluated && work->counts) {
        outcome.status = GpuScopeMeasureStatus::Measured;
        outcome.counts = std::move(*work->counts);
    } else if (frame.status == GpuProcessFrameStatus::Cancelled) {
        outcome.status = GpuScopeMeasureStatus::Cancelled;
    } else if (frame.status == GpuProcessFrameStatus::Disabled ||
               frame.status == GpuProcessFrameStatus::DeviceUnavailable ||
               frame.status == GpuProcessFrameStatus::UnsupportedGpuSubset ||
               outcome.diagnostic.code == Code::AwaitingCapacity ||
               outcome.diagnostic.code == Code::BudgetRefused ||
               outcome.diagnostic.code == Code::OverBudget ||
               outcome.diagnostic.code == Code::DeviceUnavailable ||
               outcome.diagnostic.code == Code::DeviceLost) {
        outcome.status = GpuScopeMeasureStatus::Unavailable;
    } else {
        outcome.status = GpuScopeMeasureStatus::Failed;
    }
    return outcome;
}

} // namespace bloom::runtime
