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

} // namespace bloom::runtime::detail
