#pragma once

// Private helper of GpuProcessFrameEvaluator for the scope measure job (#724): the work a
// measure request carries, its preparation on the calling worker and the owner-thread stage that
// runs the color steps and the count kernel on the resident picture. It never touches the request
// queue, bootstrap or lifecycle.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_scope_tables.hpp>
#include <bloom/runtime/gpu_ocio_program_executor.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_scope_measure.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime::detail {

// One measure request. The caller fills `job`; preparation fills the area and the programs; the
// owner fills the result. The caller reads it only after the request has completed.
struct ScopeMeasureWork final {
    GpuScopeMeasureJob job;

    // Calling worker, after the scene is prepared.
    bool emptyArea = false;
    ScopeMeasuredArea area;
    std::shared_ptr<const PreparedGpuOcioCommand> display;
    std::shared_ptr<const PreparedGpuOcioCommand> xyz;

    // Owner.
    std::optional<ScopeCounts> counts;
    std::uint64_t colorDispatches = 0;
    std::uint64_t tableReadbacks = 0;
    std::uint64_t tableReadbackBytes = 0;
};

// The bytes the measure's tables can take on the device and the host: the table buffer, its
// staging copy and the host copy, for the widest Waveform of the job's detail.
[[nodiscard]] std::uint64_t scopeMeasureTableBytes(const GpuScopeMeasureJob& job) noexcept;

// True when the job runs a color program on the device, so the phase reserves room for one.
[[nodiscard]] bool scopeMeasureUsesColorProgram(const GpuScopeMeasureJob& job) noexcept;

struct ScopeMeasureFault final {
    GpuProcessFrameStatus status = GpuProcessFrameStatus::Failed;
    GpuProcessFrameDiagnosticCode code = GpuProcessFrameDiagnosticCode::None;
    std::string message;
};

// Calling worker: the measured area of the prepared scene's picture and the color programs for its
// extent. Nothing when the work is ready to queue (or its area is empty, `work.emptyArea`).
[[nodiscard]] std::optional<ScopeMeasureFault>
prepareScopeMeasure(ScopeMeasureWork& work, const PreparedGpuScene& scene,
                    const CompiledCompositionPlan& plan, const EvaluationRequest& request,
                    const CancellationToken& cancellation);

// The same preparation for a picture whose geometry is known without its scene: the preview
// service's scope tap keeps the picture, its windows, the request's region of interest and the
// composition's size.
[[nodiscard]] std::optional<ScopeMeasureFault> prepareScopeMeasureForPicture(
    ScopeMeasureWork& work, const render::ImageWindow& dataWindow,
    const render::ImageWindow& displayWindow, const std::optional<render::ImageWindow>& roi,
    std::uint32_t compositionWidth, std::uint32_t compositionHeight,
    const CancellationToken& cancellation);

// Owner-thread native state of the measure, created on first use and destroyed on the owner.
class ScopeMeasureStage final {
  public:
    // Null with `message` set when the device cannot host the color step executor.
    [[nodiscard]] static std::unique_ptr<ScopeMeasureStage> create(render::GpuDevice& device,
                                                                   std::string& message);

    [[nodiscard]] bool hasUnretiredSubmission() noexcept;
    // One non-blocking turn of whatever is in flight.
    void poll() noexcept;
    std::uint64_t reclaimPrograms(std::uint64_t bytes) noexcept;

    [[nodiscard]] GpuOcioProgramExecutor& color() noexcept { return *color_; }
    [[nodiscard]] render::GpuScopeTables& tables() noexcept { return tables_; }

  private:
    explicit ScopeMeasureStage(std::unique_ptr<GpuOcioProgramExecutor> color) noexcept;

    std::unique_ptr<GpuOcioProgramExecutor> color_;
    render::GpuScopeTables tables_;
};

// Owner thread. Runs the color steps and the count kernel on `picture` (the scene's resident
// output) and reads back the tables into `work.counts`. `nativeRetirementProven` is false on
// return only while a submission of this measure is not proven retired. Evaluated on success.
[[nodiscard]] ScopeMeasureFault
runScopeMeasure(ScopeMeasureStage& stage, ScopeMeasureWork& work,
                std::shared_ptr<const render::GpuImage> picture, std::uint64_t byteBudget,
                std::chrono::milliseconds nativeDeadline, const CancellationToken& cancellation,
                const std::atomic_bool& stopRequested, bool& nativeRetirementProven);


// The non-blocking counterpart of runScopeMeasure for the preview service's owner thread (#724).
// It runs the display color step, the XYZ color step (as the job asks), then the count kernel and
// its one table readback, one native submission at a time, and never waits on a fence: each call
// of step() begins at most one submission or polls the one in flight once. The picture is only
// read; nothing is read back but the tables.
//
// Lifetime follows runScopeMeasure: a cancel or a deadline requests cancellation of the submission
// in flight and the stepper then keeps polling until its fence is proven retired; only then does
// step() report Settled with retirementProven() true. A submission that was never begun leaves
// nothing in flight. The stage and the work must outlive the stepper.
class ScopeMeasureStepper final {
  public:
    struct Hooks final {
        // Whether the next native submission of this measure may begin now. Empty: always.
        std::function<bool()> mayBegin;
        // The table poll. Empty: GpuScopeTables::poll. The service's test seam holds the
        // readback through it.
        std::function<render::GpuScopeTablesState(render::GpuScopeTables&)> pollTables;
    };
    enum class State : std::uint8_t { Running, Settled };

    ScopeMeasureStepper(ScopeMeasureStage& stage, ScopeMeasureWork& work,
                        std::shared_ptr<const render::GpuImage> picture,
                        std::uint64_t byteBudget) noexcept;

    // One non-blocking turn. `cancelRequested` and `deadlineExpired` cancel what is in flight (a
    // deadline reports DeadlineExceeded). Settled once the measure ended (its fault() says how)
    // and nothing it submitted is still unretired.
    [[nodiscard]] State step(bool cancelRequested, bool deadlineExpired, const Hooks& hooks) noexcept;

    // The result once Settled: Evaluated with `work.counts` set, or the failure.
    [[nodiscard]] const ScopeMeasureFault& fault() const noexcept { return fault_; }
    // True while a native submission of this measure is in flight (the deadline runs over it).
    [[nodiscard]] bool submissionInFlight() const noexcept {
        return phase_ == Phase::ColorPoll || phase_ == Phase::TablesPoll || phase_ == Phase::Draining;
    }
    // True once cancellation was issued to the submission in flight.
    [[nodiscard]] bool stopIssued() const noexcept { return stopIssued_; }
    // True once any native submission was begun.
    [[nodiscard]] bool began() const noexcept { return began_; }
    // True when the stepper settled with every submission proven retired.
    [[nodiscard]] bool retirementProven() const noexcept { return retirementProven_; }

  private:
    enum class Phase : std::uint8_t { Next, ColorPoll, TablesPoll, Draining, Settled };

    void settle(ScopeMeasureFault fault) noexcept;
    void failAndDrain(ScopeMeasureFault fault) noexcept;
    [[nodiscard]] State advance(bool stop, bool cancelRequested, const Hooks& hooks);
    [[nodiscard]] ScopeMeasureFault stopFault(bool cancelRequested) const;

    ScopeMeasureStage& stage_;
    ScopeMeasureWork& work_;
    std::shared_ptr<const render::GpuImage> signal_;
    std::shared_ptr<const render::GpuImage> xyz_;
    std::uint64_t byteBudget_ = 0;
    std::uint64_t dispatchesBefore_ = 0;
    Phase phase_ = Phase::Next;
    bool displayDone_ = false;
    bool xyzDone_ = false;
    bool colorDispatchCounted_ = false;
    bool stopIssued_ = false;
    bool began_ = false;
    bool retirementProven_ = false;
    ScopeMeasureFault fault_;
};

} // namespace bloom::runtime::detail
