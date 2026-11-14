#pragma once

// #724 Scopes: the scope measure as a job of the final-render device route
// (GpuProcessFrameEvaluator::measureScopes). docs/architecture/scopes.md is the specification.
//
// The frame's scene runs on the evaluator's device exactly as a final frame does. Instead of the
// final picture readback, the owner converts the resident picture to the measured signal (the
// display transform as a float picture for the Display signal; the picture itself for Working),
// converts that to CIE XYZ when the Chromaticity table is asked for, counts the tables with
// render::GpuScopeTables and reads back only the tables: the named bounded readback
// `prep.scopes.result_readback`. No picture leaves the device.
//
// The color programs are prepared on the calling worker, never on the device owner: the job
// carries two factories that are called there with the picture's extent.

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime {

// Prepares the immutable program of one color step for a picture of `geometry`. Called on the
// calling worker. Null means the step cannot run on the device; the measure is then Unavailable.
using GpuScopeCommandFactory = std::function<std::shared_ptr<const PreparedGpuOcioCommand>(
    GpuOcioCommandGeometry geometry, const CancellationToken& cancellation)>;

struct GpuScopeMeasureJob final {
    std::uint32_t scopes = render::kScopeTableAll; // render::kScopeTable* bits
    std::uint32_t axisMaximum = 1;
    ScopeLuma luma;
    bool ignoreTransparent = false;
    // 0 (full), 1 or 2: the detail of the Vectorscope, CIE and Waveform tables the device counts
    // and reads back (render/scope_table_layout.hpp).
    std::uint32_t detail = 0;
    // In composition pixels; the whole picture when not set.
    std::optional<ScopeRegion> region;
    // The Display signal: a GpuOcioTransformKind::ScopeDisplay program. Empty measures the Working
    // signal, the scene-linear picture as it is.
    GpuScopeCommandFactory displayCommand;
    // The signal to CIE XYZ (D65): a GpuOcioTransformKind::ScopeXyz program from the signal's
    // color space. Required with, and used only for, the Chromaticity table.
    GpuScopeCommandFactory xyzCommand;
};

enum class GpuScopeMeasureStatus : std::uint8_t {
    // `counts` holds the tables of `area`, counted on the device.
    Measured,
    // The region lies outside the picture: there is nothing to count.
    EmptyArea,
    // The device route cannot measure this frame (no device, a scene outside the prepared-GPU
    // subset, a color step with no device program, no room). The caller measures with the CPU
    // reference and labels the result CPU.
    Unavailable,
    Cancelled,
    Failed,
};

struct GpuScopeMeasureOutcome final {
    GpuScopeMeasureStatus status = GpuScopeMeasureStatus::Failed;
    GpuProcessFrameDiagnostic diagnostic;
    ScopeCounts counts;
    ScopeMeasuredArea area;
    // Native work of this measure: color program dispatches, table dispatches and table
    // readbacks. Proof that the device counted the tables and that no picture was read back.
    std::uint64_t colorDispatches = 0;
    std::uint64_t tableReadbacks = 0;
    std::uint64_t tableReadbackBytes = 0; // the bytes those readbacks moved
    std::uint64_t pictureReadbacks = 0; // always zero
};

} // namespace bloom::runtime
