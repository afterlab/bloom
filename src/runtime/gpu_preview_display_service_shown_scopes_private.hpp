#pragma once

// #733 Scopes: the Display signal tables of the frame the Viewer shows, counted on the preview
// display service's device at display time (docs/architecture/scopes.md).
//
// A shown frame is a stored display frame: 4-byte cells in host memory. Its measure is a job of
// the service's owner thread that is not a frame job: the cells are copied once into a staging
// buffer and unpacked on the device (render::GpuImageUpload with displayCells), the CIE XYZ color
// step runs only when the Chromaticity table is asked for, the count kernel counts, and only the
// tables are read back. No scene runs and no picture is read back.
//
// Frames always come first. A measure is taken up only while no frame has native work on the
// device and the device holds no processing phase; otherwise it is Dropped at once and its caller
// counts the frame on a worker (runtime/shown_frame_scopes.hpp). A measure that was taken up then
// advances one non-blocking step per owner turn, after the turn's frame steps, and never waits on
// a fence. A frame that needs the device while a measure is in flight is not held back: its phase
// is retained beside the measure's (the measure's phase is parked, GpuDevice::parkProcessingPhase).

#include "gpu_process_frame_scope_private.hpp"

#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/image_types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace bloom::runtime::detail {

struct PreviewDisplayServiceCore;

enum class ShownScopeJobStatus : std::uint8_t {
    // `counts` holds the tables, counted on the device.
    Measured,
    // The device did not take the measure up, or had no room for it: frames were using it, a
    // processing phase was held, the phase was not admitted or an allocation was refused. Nothing
    // was counted; the caller counts the frame with the CPU reference.
    Dropped,
    // The service is stopping.
    Cancelled,
    Failed,
};

struct ShownScopeJobOutcome final {
    ShownScopeJobStatus status = ShownScopeJobStatus::Failed;
    std::string message;
    std::optional<ScopeCounts> counts;
    // The native work of the measure: frame uploads (1 when it was taken up), color program
    // dispatches, table readbacks and the bytes they moved. No picture is read back.
    std::uint64_t uploads = 0;
    std::uint64_t colorDispatches = 0;
    std::uint64_t tableReadbacks = 0;
    std::uint64_t tableReadbackBytes = 0;
};

// One measure, filled on a worker and posted with postShownFrameScopes. `work` is prepared for the
// frame's picture: its area, and its XYZ program when the Chromaticity table is asked for;
// `work.display` stays null (the cells are already the Display signal).
struct ShownFrameScopeJob final {
    // The stored cells, row-major over `descriptor`'s data window; `owner` keeps them alive.
    std::shared_ptr<const void> owner;
    std::span<const render::Rgba8> cells;
    render::DisplayPixelLayout layout = render::DisplayPixelLayout::Straight8888;
    // The picture the cells unpack to.
    std::optional<render::Rgba32fImageDescriptor> descriptor;
    ScopeMeasureWork work;
    // The measure's processing phase, reserved by the caller when the service has a phase policy
    // (shownFrameScopePhase). Null with a policy means the measure was not admitted.
    std::shared_ptr<render::GpuProcessingPhaseLease> phase;
    // Called exactly once, on the owner thread (or on the posting thread when the post is
    // refused). It must not block.
    std::function<void(ShownScopeJobOutcome)> done;
};

// The service's shown-frame measure state.
struct ShownScopeState final {
    // Any thread.
    std::mutex mutex;
    std::deque<std::shared_ptr<ShownFrameScopeJob>> pending;
    std::atomic_bool hasPending{false};

    // Owner thread.
    enum class Phase : std::uint8_t { Upload, Count };
    std::shared_ptr<ShownFrameScopeJob> active;
    Phase phase = Phase::Upload;
    // The measure's own native stage (color step executor and count kernel), so a frame that
    // counts its Working tables in its job never finds its stage busy with a shown frame.
    std::unique_ptr<ScopeMeasureStage> stage;
    std::unique_ptr<render::GpuImageUpload> upload;
    std::unique_ptr<ScopeMeasureStepper> stepper;
    bool phaseArmed = false;
    bool cancelIssued = false;
    // Owner turns spent since the measure was told to stop (the service is stopping or its
    // deadline passed) without its submission proven retired.
    std::size_t stopTurns = 0;
    std::chrono::steady_clock::time_point startedAt{};
    // Bumped whenever the measure changes state, so the owner iterates again at once.
    std::uint64_t progress = 0;

    std::atomic<std::uint64_t> measured{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> uploads{0};
    std::atomic<std::uint64_t> colorDispatches{0};
    std::atomic<std::uint64_t> tableReadbacks{0};
    std::atomic<std::uint64_t> tableReadbackBytes{0};
    // Frame stages that took their processing phase while a measure was in flight.
    std::atomic<std::uint64_t> framesStartedDuringMeasure{0};

    // Hold seam: while it returns Pending the measure's upload reports pending (the real poll is
    // not called). Empty polls the upload. Set it only while no measure is in flight.
    std::function<render::GpuImageUploadPollResult(render::GpuImageUpload&)> uploadPollOverride;
};

// The phase envelope of a measure of `pixels` cells whose tables take `tableBytes`
// (scopeMeasureTableBytes), with or without the XYZ color step.
[[nodiscard]] std::shared_ptr<render::GpuProcessingPhaseLease>
reserveShownFrameScopePhase(const PreviewDisplayServiceCore& core, std::uint64_t pixels,
                            std::uint64_t tableBytes, bool colorStep) noexcept;

// Any thread. False when the service is stopping: `job.done` was then not called and never will
// be.
[[nodiscard]] bool postShownFrameScopes(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                        std::shared_ptr<ShownFrameScopeJob> job) noexcept;

// Owner thread, once per turn after the turn's frame steps: takes a waiting measure up or drops
// it, or advances the one in flight by one non-blocking step.
void processShownFrameScopes(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread: a measure is in flight.
[[nodiscard]] bool shownFrameScopesActive(const PreviewDisplayServiceCore& core) noexcept;

// Owner thread, immediately before a frame stage retains its processing phase: a measure in flight
// gives the device's retained slot up (its phase is parked).
void shownFrameScopesGiveWay(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread, when the service is idle: the measure's native stage and upload object are
// released when no measure is in flight; the next measure creates them again.
void releaseShownFrameScopesIdle(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread, while the service drains: waiting measures end Cancelled and the one in flight is
// cancelled and stepped until nothing of it is unretired (or its bounded budget is spent). True
// while a measure is still in flight.
[[nodiscard]] bool
drainShownFrameScopes(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner thread, at teardown, before the device goes.
void retireShownFrameScopesNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

} // namespace bloom::runtime::detail
