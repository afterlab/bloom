#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU affine image transform: AffineBilinearV1. Additive to
// gpu_composite.hpp; it does not change TranslationOpacityBilinearV1 or SourceOverV1.
//
// The operation resamples ONE opaque, device-bound source GpuImage through a complete composed
// affine placement into a new resident RGBA32F GpuImage, preserving the source display window and
// pixel aspect ratio. It covers rotation 0/90/arbitrary, uniform/nonuniform/negative scale, an
// authored anchor plus translation, and any precomposed parent matrix (including shear).
//
// The CPU reference of the composed-matrix form, which is the form every production route uses
// (#545), is affineMatrixResampleRow() below. It defines the sample coordinate as the same portable
// double-float reconstruction the kernel runs, so the production CPU and GPU routes agree bit for
// bit on all four components, alpha included. The LayerTransform form keeps
// render::layerTransformBilinearRow() (src/render/cpu_image_primitives.cpp), which evaluates
// LayerTransform::inverseMap() in binary64, as its oracle; that form is held to the documented
// per-finite-component 2e-6 absolute-or-relative gate (docs/architecture/gpu-backend.md).
//
// SAMPLE METADATA. The general affine inverse map is uploaded as a compact O(1) set of binary64
// coefficients (see GpuAffineMap) and the per-pixel source-local coordinate is derived on the GPU
// with a portable error-free double-float reconstruction; the kernel performs the floor/factor
// reduction and the bilinear gather plus the opacity multiply. No O(width*height) per-pixel host
// metadata loop runs on the successful path. The host never generates or resamples RGBA pixels, and
// the kernel needs neither shaderFloat64 nor a 64-bit integer type. The original per-pixel binary64
// host preparation below (prepareAffineSamples/prepareAffineMatrixSamples) is retained for tests;
// it is not on the GPU path.
//
// This header is intentionally narrow. It does not create a device or a service; it consumes the
// existing GpuDevice and GpuImage. Native work runs on the device owner thread and fails closed
// from any other thread. One job is outstanding at a time.

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::render {

// One host-prepared per-output-pixel sample. `baseX`/`baseY` are the exact CPU floor() of the
// inverse-mapped source-local coordinate and `factorX`/`factorY` the exact CPU Float32 fraction.
// `baseX == INT_MIN` is the transparent sentinel: the CPU's `sample <= -1.0 || >= extent` branch on
// either axis. base+/-1 taps outside the source data window are transparent black.
struct GpuAffineSample final {
    std::int32_t baseX = 0;
    std::int32_t baseY = 0;
    float factorX = 0.0F;
    float factorY = 0.0F;

    friend bool operator==(const GpuAffineSample&, const GpuAffineSample&) noexcept = default;
};

// The transparent sentinel value of GpuAffineSample::baseX.
inline constexpr std::int32_t kGpuAffineTransparentBase = std::numeric_limits<std::int32_t>::min();

// A complete composed forward affine placement of the source: a source-local pixel-centre
// coordinate (0-based within the source data window, the convention LayerTransform::forwardMap()
// uses) maps to an absolute output/composition coordinate as
//     outX = a * localX + b * localY + tx
//     outY = c * localX + d * localY + ty
// It may carry shear, which a rotation/scale decomposition alone cannot express.
struct GpuAffineMatrix final {
    double a = 1.0;
    double b = 0.0;
    double tx = 0.0;
    double c = 0.0;
    double d = 1.0;
    double ty = 0.0;

    friend bool operator==(const GpuAffineMatrix&, const GpuAffineMatrix&) noexcept = default;
};

// Exact host inverse-map preparation for the render-owned LayerTransform oracle. For every output
// pixel centre in `outputWindow` (absolute composition coordinates) it calls
// LayerTransform::inverseMap() -- the same function layerTransformBilinearRow() uses -- and reduces
// the result to the exact CPU base/factor pair. The returned vector is row-major, outputWindow
// width times height. Pure host arithmetic; identical in the Vulkan and stub builds.
[[nodiscard]] std::vector<GpuAffineSample> prepareAffineSamples(const LayerTransform& transform,
                                                                ImageWindow outputWindow);

// Exact host inverse-map preparation for a composed matrix over `sourceWindow`. The composed
// forward matrix is inverted in Float64; a non-finite or singular (zero-determinant) matrix has no
// inverse and yields an all-transparent sample vector, the empty-layer result. Otherwise each
// output pixel centre is inverse-mapped and reduced exactly as the CPU row does.
[[nodiscard]] std::vector<GpuAffineSample> prepareAffineMatrixSamples(const GpuAffineMatrix& matrix,
                                                                      ImageWindow sourceWindow,
                                                                      ImageWindow outputWindow);

// The compact O(1) GPU sample map: the inverse affine placement restricted to the output pixel
// lattice, expressed on output-local integer indices so the kernel needs no output origin and no
// per-pixel host loop:
//   localX(column, row) = localXAtOrigin + stepXPerColumn * column + stepXPerRow * row
//   localY(column, row) = localYAtOrigin + stepYPerColumn * column + stepYPerRow * row
// `column`/`row` are 0-based data-window indices of `outputWindow`; the origin is folded into
// `localXAtOrigin`/`localYAtOrigin`. The six coefficients are derived from the CPU oracle sampled
// at the output origin and at one step along each axis. The kernel reconstructs each pixel in
// error-free two-Float32 (double-float, ~48 significand bits) arithmetic without shaderFloat64. That
// reconstruction is not bit-identical to a per-pixel binary64 inverse map, so for the
// composed-matrix form it IS the definition of the sample coordinate: affineMatrixResampleRow()
// runs it on the CPU (#545). A collapsed (non-finite or zero-determinant) composed matrix yields the
// all-transparent sentinel map (both constants -2.0, all steps 0).
struct GpuAffineMap final {
    double localXAtOrigin = 0.0;
    double localYAtOrigin = 0.0;
    double stepXPerColumn = 1.0;
    double stepXPerRow = 0.0;
    double stepYPerColumn = 0.0;
    double stepYPerRow = 1.0;

    friend bool operator==(const GpuAffineMap&, const GpuAffineMap&) noexcept = default;
};

// Compact O(1) inverse-map preparation for the LayerTransform oracle: probes
// LayerTransform::inverseMap() at the output origin and one local pixel step along each axis. Pure
// host arithmetic; O(1) in the output extent. Identical in the Vulkan and stub builds.
[[nodiscard]] GpuAffineMap prepareAffineMap(const LayerTransform& transform,
                                            ImageWindow outputWindow);

// Compact O(1) inverse-map preparation for the composed-matrix form. Uses the same closed-form
// inverse as prepareAffineMatrixSamples; a non-finite or singular matrix yields the transparent
// sentinel map. O(1) in the output extent.
[[nodiscard]] GpuAffineMap prepareAffineMatrixMap(const GpuAffineMatrix& matrix,
                                                  ImageWindow outputWindow);

// The resample filter of the composed-matrix form (#480). Bilinear interpolates the four taps
// around the sample point; Nearest copies the one tap whose centre is nearest (the upper tap on an
// exact half-way factor), so data and ID images keep their stored values exactly.
enum class GpuAffineFilter : std::uint32_t { Bilinear = 0, Nearest = 1 };

// The six map coefficients as the kernel receives them: each binary64 coefficient split into an
// error-free Float32 (hi, lo) pair, hi = (float)value and lo = (float)(value - hi), in the order
// localX@origin, localY@origin, stepX/column, stepX/row, stepY/column, stepY/row. The device upload
// and the CPU reference both use this split.
[[nodiscard]] std::array<float, 12> splitAffineMap(const GpuAffineMap& map) noexcept;

// The sample one output pixel of the lattice resamples, by the kernel's own arithmetic (#545):
// localX = (x0 + stepXPerColumn * column) + stepXPerRow * row (and likewise localY) in error-free
// double-float with Float32 operations and fma only, then the transparent range test on the pair
// (not strictly inside (-1, extent) on either axis) and the floor/factor reduction of the pair, with
// the factor rounded once to Float32. `column`/`row` are 0-based indices of the window the map was
// prepared for and must be below 2^24. Returns the transparent sentinel outside the range.
[[nodiscard]] GpuAffineSample reconstructAffineSample(const std::array<float, 12>& coefficients,
                                                      std::uint32_t column, std::uint32_t row,
                                                      std::uint32_t sourceWidth,
                                                      std::uint32_t sourceHeight) noexcept;

// The CPU reference of AffineBilinearV1's composed-matrix form, and the production CPU resample of
// every layer, Transform 2D, Resize/Fit and discrete-transform map that is not a pure translation
// (#545). It writes one row of `outputWindow` exactly as the kernel does when it runs over
// `latticeWindow`: the map comes from prepareAffineMatrixMap(matrix, latticeWindow), every sample
// from reconstructAffineSample(), then the same tap selection (Nearest: base + (factor >= 0.5)),
// the same fma lerp with exact 0/1 factors, and the same opacity multiply. `latticeWindow` is the
// window the GPU command covers; `outputWindow` may be a sub-window of it (a region-of-interest
// clip) and then gets the same pixels. `source` is the image the matrix maps from: its data window is
// the matrix's source window. Rejects an output window outside the lattice, a lattice of 2^24 or
// more pixels on an axis, a row outside `outputWindow`, a mismatched output span and a non-finite
// result.
[[nodiscard]] ImageStatus affineMatrixResampleRow(Rgba32fImageView source,
                                                  const GpuAffineMatrix& matrix, float opacity,
                                                  GpuAffineFilter filter, ImageWindow latticeWindow,
                                                  ImageWindow outputWindow, std::int64_t outputY,
                                                  std::span<Rgba32f> output) noexcept;

// Validated inputs for the LayerTransform form. `source` is shared immutable ownership retained for
// the job's lifetime. `outputWindow` is the output DATA window only; the output DISPLAY window and
// pixel aspect are preserved from the source. The opacity is LayerTransform::opacity(), already
// rounded once to Float32 by the CPU primitive.
struct GpuAffineParameters final {
    std::shared_ptr<const GpuImage> source;
    ImageWindow outputWindow;
    std::optional<LayerTransform> transform;
};

// Validated inputs for the composed-matrix form. `opacity` must be finite in [0, 1] and is used as
// the Float32 binding directly (the caller has already rounded it once).
struct GpuAffineMatrixParameters final {
    std::shared_ptr<const GpuImage> source;
    ImageWindow outputWindow;
    GpuAffineMatrix matrix;
    float opacity = 1.0F;
    // Resize/Fit can author a new display extent/PAR; ordinary layer placements inherit.
    std::optional<Rgba32fImageDescriptor> outputDescriptor{};
    GpuAffineFilter filter = GpuAffineFilter::Bilinear;
};

// Per-call ceilings. Defaults are effectively unbounded so the real bounds are the live device
// limits and the per-call byteBudget; no fixed 256 MiB or 4K ceiling is baked in. This matches
// GpuPointResampleBudgets and the full-resolution admission direction. A non-zero injected value
// proves the fail-closed budget path.
struct GpuAffineBudgets final {
    std::uint64_t maxImageBytes = std::numeric_limits<std::uint64_t>::max();
    // The compact inverse-affine metadata is O(1) (six binary64 coefficients); the practical
    // metadata bound is the per-call byteBudget, never a per-pixel O(width*height) array.
    std::uint64_t maxMetadataBytes = std::numeric_limits<std::uint64_t>::max();
};

enum class GpuAffineJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuAffinePollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuAffineDiagnosticCode : std::uint8_t {
    None,
    InvalidArgument,
    Unsupported,
    OverBudget,
    Busy,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    ShaderRejected,
    AllocationFailed,
    Cancelled,
    NativeTimeout,
    // The kernel's status/error flag was nonzero: the frame must not be published and the caller
    // falls back to the CPU reference path.
    StatusFlagRejected,
};

struct GpuAffineDiagnostic final {
    GpuAffineDiagnosticCode code = GpuAffineDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuAffineDiagnostic&, const GpuAffineDiagnostic&) = default;
};

struct GpuAffineCreateResult;

class GpuAffine final {
  public:
    GpuAffine(const GpuAffine&) = delete;
    GpuAffine& operator=(const GpuAffine&) = delete;
    GpuAffine(GpuAffine&& other) noexcept;
    GpuAffine& operator=(GpuAffine&& other) noexcept;
    ~GpuAffine();

    // Validates the device and budgets on the device owner thread and returns a lazy, native-free
    // instance. Wrong thread returns WrongThread. The AffineBilinearV1 pipeline, layout, descriptor
    // set, command pool, and fence are built once on the first begin, under one bounded
    // resident-pool slot acquired before any native allocation; an idle instance allocates nothing.
    [[nodiscard]] static GpuAffineCreateResult create(GpuDevice& device,
                                                      const GpuAffineBudgets& budgets = {});

    [[nodiscard]] GpuAffineJobState state() const noexcept;
    [[nodiscard]] const GpuAffineDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    // #386: blocks the owner thread until this object's submission's fence signals (or the
    // device is lost), at most `timeoutNanoseconds`. True when it did; the next poll() observes
    // it. False with nothing submitted or off the owner thread. Changes no state.
    [[nodiscard]] bool waitForSubmission(std::uint64_t timeoutNanoseconds) noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    // Validates geometry/budgets/device limits and the device generation, prepares the host sample
    // metadata, creates the resident output image, and submits one dispatch. None when accepted.
    [[nodiscard]] GpuAffineDiagnostic beginAffine(const GpuAffineParameters& parameters,
                                                  std::uint64_t byteBudget);
    [[nodiscard]] GpuAffineDiagnostic beginAffineMatrix(const GpuAffineMatrixParameters& parameters,
                                                        std::uint64_t byteBudget);

    // Non-blocking fence query. On Ready the status flag word has been checked; a nonzero flag
    // publishes Failure(StatusFlagRejected) instead of Ready.
    [[nodiscard]] GpuAffinePollResult poll();

    // The resident output image while Ready; nullptr otherwise. Ownership stays with the pipeline.
    [[nodiscard]] const GpuImage* image() const noexcept;

    // Moves the resident output image out and returns the pipeline to Idle. Only valid when Ready.
    [[nodiscard]] GpuImage takeImage() noexcept;

    // Marks an outstanding job's result discarded; resources stay until the fence retires.
    void cancel() noexcept;

    // Recoverable pressure, not a permanent fuse: true while a foreign-released or unproven
    // submission is retained in the bounded process-wide resident pool, and false again once the
    // rightful owner thread drains it.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuAffine(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuAffineCreateResult final {
    std::unique_ptr<GpuAffine> affine;
    GpuAffineDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return affine != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
