#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU scope measure: ScopeTablesV1 (#724).
//
// Counts the pixels of a measured area of one resident RGBA32F signal picture into the scope count
// tables of scope_table_layout.hpp, on the device, and reads back only those tables: never the
// picture. The counts are whole numbers added with atomic integer adds, so they do not depend on
// scheduling, and the float arithmetic that places a pixel in a cell is the CPU reference's
// (src/runtime/scope_measure.cpp), operation for operation, so the two agree exactly for the same
// float input.
//
// The readback is the named bounded readback `prep.scopes.result_readback` of the coverage
// contract (docs/architecture/scopes.md).
//
// Lifetime follows GpuProcessReadback: begin() is owner-thread only and non-blocking; poll() never
// blocks; the pictures and every native resource are retained until the submission's fence is
// proven retired, so a cancel, a lost device or a timeout never frees a live resource and never
// publishes a table. One submission may be outstanding process-wide.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/scope_table_layout.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {

struct GpuScopeTablesRequest final {
    // Red, green and blue are the signal taken over black; alpha is the pixel's alpha.
    std::shared_ptr<const GpuImage> signal;
    // The signal converted to CIE XYZ (D65), of the signal's extent. Required with, and read only
    // for, the Chromaticity table.
    std::shared_ptr<const GpuImage> xyz;
    std::uint32_t scopes = 0; // kScopeTable* bits
    // The measured area, in pixels of the signal picture from its first stored pixel. It may reach
    // outside the picture: those pixels are transparent black.
    std::int32_t areaX = 0;
    std::int32_t areaY = 0;
    std::uint32_t areaWidth = 0;
    std::uint32_t areaHeight = 0;
    float inverseAxisMaximum = 1.0F; // 1 / M
    float lumaRed = 0.0F;
    float lumaGreen = 0.0F;
    float lumaBlue = 0.0F;
    float cbDenominator = 1.0F; // 2 (1 - Kb) in binary32
    float crDenominator = 1.0F; // 2 (1 - Kr) in binary32
    bool ignoreTransparent = false;
    // 0 is full detail; 1 and 2 count the Vectorscope, Chromaticity and Waveform tables into 2x
    // and 4x coarser cells (scope_table_layout.hpp). The table buffer, its staging copy and the
    // host copy are sized from the detail's layout; a detail above kScopeMaxDetail is refused as
    // InvalidArgument.
    std::uint32_t detail = 0;
};

enum class GpuScopeTablesState : std::uint8_t { Idle, Pending, Ready, Failure };

enum class GpuScopeTablesCode : std::uint8_t {
    None,
    InvalidArgument,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    OverBudget,
    ShaderRejected,
    Busy,
    MeasureFailed,
    Cancelled,
};

struct GpuScopeTablesDiagnostic final {
    GpuScopeTablesCode code = GpuScopeTablesCode::None;
    std::string message;

    friend bool operator==(const GpuScopeTablesDiagnostic&,
                           const GpuScopeTablesDiagnostic&) = default;
};

struct GpuScopeTablesResult final {
    ScopeTableLayout layout;
    std::vector<std::uint32_t> words;
};

class GpuScopeTables final {
  public:
    GpuScopeTables();
    GpuScopeTables(const GpuScopeTables&) = delete;
    GpuScopeTables& operator=(const GpuScopeTables&) = delete;
    GpuScopeTables(GpuScopeTables&&) = delete;
    GpuScopeTables& operator=(GpuScopeTables&&) = delete;
    ~GpuScopeTables();

    [[nodiscard]] GpuScopeTablesState state() const noexcept;
    [[nodiscard]] const GpuScopeTablesDiagnostic& diagnostic() const noexcept;

    // Owner-thread. Validates the request, the device limits and the byte budget (the table
    // buffer's actual allocation plus the host copy), zeroes the table buffer, records the pixel
    // pass and, with the Waveform, the column pass, and submits. True while Pending. The pipeline
    // is built on the first call for a device generation and kept.
    [[nodiscard]] bool begin(const GpuScopeTablesRequest& request,
                             std::uint64_t byteBudget) noexcept;

    // Owner-thread, non-blocking. On a proven-signalled fence the tables are copied to the host
    // exactly once and the state becomes Ready (Failure for a cancelled measure).
    [[nodiscard]] GpuScopeTablesState poll() noexcept;

    // Owner-thread. Only meaningful in Ready; moves the tables out and resets to Idle.
    [[nodiscard]] GpuScopeTablesResult take() noexcept;

    // Owner-thread. Requests cancellation; nothing is freed until the fence is proven.
    void cancel() noexcept;

    // Owner-thread. True while a submission of this measure, or one it handed to the process slot,
    // is not proven retired; a handed-over submission whose fence has signaled is freed here.
    [[nodiscard]] bool hasUnretiredSubmission() noexcept;

    // Positive proof that native dispatches were submitted to the queue (tests and route proofs).
    [[nodiscard]] static std::uint64_t nativeDispatchCount() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::render
