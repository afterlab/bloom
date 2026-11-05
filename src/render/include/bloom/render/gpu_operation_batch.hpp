#pragma once

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_blend.hpp>
#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_ocio_program.hpp>
#include <bloom/render/gpu_solid.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bloom::render {
// One command buffer and one fence per run of eligible operations; the run ends at the first
// ineligible kind or at this many operations, whichever comes first.
inline constexpr std::uint32_t kGpuOperationBatchCapacity = 64;
inline constexpr std::uint32_t kGpuBatchExternalImage = UINT32_MAX;
// Exactly one source: an already-retired external image, or an earlier operation's output.
// Pending internal outputs never escape the batch before its single completion fence proves all
// writes retired. References must be topological and bound to this exact device generation.
struct GpuBatchImageRef final {
    std::shared_ptr<const GpuImage> external;
    std::uint32_t operation = kGpuBatchExternalImage;
};
struct GpuBatchSolid final {
    GpuSolidParameters parameters;
};
// One stop of a gradient's table: its position along the gradient, the reciprocal of the span to
// the next stop (0 for the last stop and for a hard edge), and its straight scene-linear RGBA.
struct GpuBatchGradientStop final {
    float position = 0.0F;
    float inverseSpan = 0.0F;
    std::array<float, 4> color{};
};
inline constexpr std::size_t kGpuBatchGradientMaximumStops = 64;
struct GpuBatchGradient final {
    GpuBatchGradient(ImageWindow data, ImageWindow display, core::PixelAspectRatio aspect)
        : dataWindow(data), displayWindow(display), pixelAspect(aspect) {}
    ImageWindow dataWindow;
    ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    // One to kGpuBatchGradientMaximumStops stops sorted by position, each in [0, 1].
    std::vector<GpuBatchGradientStop> stops;
    std::array<float, 4> endpoints{}; // startX, startY, endX, endY in author pixels
    float inverseHorizontalScale = 1.0F;
    float inverseVerticalScale = 1.0F;
    float exactWidth = 0.0F;
    float exactHeight = 0.0F;
    std::uint32_t repeat = 0; // clamp, repeat, mirror
    bool radial = false;
    // #546: how the colour blends between stops (document::GradientModes' numbers): interpolation
    // 0 Linear, 1 Constant, 2 Ease, 3 Cardinal, 4 B-Spline, 5 Exponential; colour mode 0 RGB,
    // 1 HSV, 2 HSL; hue 0 Near, 1 Far, 2 Clockwise, 3 Counter-clockwise. leadIn and leadOut are the
    // reciprocals of the spans from 0 to the first stop and from the last stop to 1 (0 when empty).
    std::uint32_t interpolation = 0;
    std::uint32_t colorMode = 0;
    std::uint32_t hueDirection = 0;
    float leadIn = 0.0F;
    float leadOut = 0.0F;
    // Binary64 normalized coordinate metadata split into portable Float32 hi/lo pairs.
    // Linear: origin, column step, row step; radial: origin x/y, column/row step.
    std::optional<std::array<float, 8>> normalizedCoordinates;
};
struct GpuBatchTranslation final {
    GpuBatchImageRef source;
    ImageWindow outputWindow;
    double translationX;
    double translationY;
    float opacity;
    std::optional<ImageWindow> outputDisplayWindow{};
};
struct GpuBatchAffine final {
    GpuBatchImageRef source;
    ImageWindow outputWindow;
    GpuAffineMatrix matrix;
    float opacity;
    std::optional<Rgba32fImageDescriptor> outputDescriptor{};
    GpuAffineFilter filter = GpuAffineFilter::Bilinear;
};
struct GpuBatchSourceOver final {
    GpuBatchImageRef source;
    GpuBatchImageRef destination;
};
// #386: one two-input blend or image combine, with the same kernels, push values and status word as
// GpuBlend::beginBlend (the general modes use the Float64 companion when the device enabled
// shaderFloat64, exactly like GpuBlend's Auto policy). `mask` is Image Mix's resident Mask
// (GpuImageCombineParameters::Mask::Present) and is empty otherwise.
struct GpuBatchBlend final {
    GpuBatchImageRef source;
    GpuBatchImageRef destination;
    core::BlendMode mode = core::BlendMode::Normal;
    std::optional<GpuImageCombineParameters> imageCombine{};
    std::optional<GpuBatchImageRef> mask{};
};
// #386: one OCIO ProcessEffect dispatch of a retained effect-arm program, recorded into the batch's
// command buffer with the program's own pipeline, descriptor sets, uniform and status buffers. The
// output has the input's geometry, as GpuOcioProgram::beginEffect. The program is borrowed: it must
// be idle (no job of its own, nothing unretired) when the batch begins, it may appear at most once
// in a batch, and it reports an unretired submission until the batch's fence is proven, so its
// owner cannot evict or reuse it while the batch can still read it.
struct GpuBatchOcioEffect final {
    GpuBatchImageRef source;
    GpuOcioProgram* program = nullptr;
    std::vector<std::byte> uniformBytes;
};
using GpuBatchOperation =
    std::variant<GpuBatchSolid, GpuBatchTranslation, GpuBatchAffine, GpuBatchSourceOver,
                 GpuBatchGradient, GpuBatchBlend, GpuBatchOcioEffect>;

enum class GpuOperationBatchCode : std::uint8_t {
    None,
    Unsupported,
    InvalidRequest,
    OverBudget,
    Busy,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    AllocationFailed,
    StatusRejected,
    Cancelled,
    Unproven,
};
struct GpuOperationBatchDiagnostic final {
    GpuOperationBatchCode code = GpuOperationBatchCode::None;
    std::string message;
};
enum class GpuOperationBatchState : std::uint8_t { Idle, Pending, Ready, Failure, WrongThread };
struct GpuOperationBatchStatistics final {
    std::uint64_t submissions = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t imageBytes = 0;
    std::uint64_t peakAllocationBytes = 0;
};
struct GpuOperationBatchBudgets final {
    std::uint64_t maxImageBytes = GpuSolidBudgets{}.maxImageBytes;
    std::uint64_t maxMetadataBytes = 256ULL * 1024 * 1024;
};

// One exclusive bounded native batch owner per device. Other facades report Busy for serial
// fallback. Uses the existing compute kernels and allocation helpers. All actual backing is
// admitted before one queue submission; one fence retires every input/output/metadata reference. No
// output escapes before every kernel status passes after genuine fence proof. Pre-submit refusal
// destroys backing before quiet same-origin prefix restoration, permitting generic native execution
// without a second allowance. Unknown retirement retains the full owner.
class GpuOperationBatch final {
  public:
    [[nodiscard]] static std::unique_ptr<GpuOperationBatch>
    create(GpuDevice&, const GpuOperationBatchBudgets& = {}) noexcept;
    GpuOperationBatch(const GpuOperationBatch&) = delete;
    GpuOperationBatch& operator=(const GpuOperationBatch&) = delete;
    GpuOperationBatch(GpuOperationBatch&&) noexcept;
    GpuOperationBatch& operator=(GpuOperationBatch&&) noexcept;
    ~GpuOperationBatch();

    [[nodiscard]] GpuOperationBatchDiagnostic begin(std::span<const GpuBatchOperation>,
                                                    std::uint64_t byteBudget);
    [[nodiscard]] GpuOperationBatchState poll() noexcept;
    [[nodiscard]] GpuOperationBatchState state() const noexcept;
    [[nodiscard]] GpuOperationBatchDiagnostic diagnostic() const;
    [[nodiscard]] GpuOperationBatchStatistics statistics() const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    // #386: blocks the owner thread until this object's submission's fence signals (or the
    // device is lost), at most `timeoutNanoseconds`. True when it did; the next poll() observes
    // it. False with nothing submitted or off the owner thread. Changes no state.
    [[nodiscard]] bool waitForSubmission(std::uint64_t timeoutNanoseconds) noexcept;
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;
    [[nodiscard]] std::vector<GpuImage> takeImages();
    void cancel() noexcept;

  private:
    struct Impl;
    explicit GpuOperationBatch(std::unique_ptr<Impl>) noexcept;
    void releaseImpl() noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace bloom::render
