#include "gpu_solid_private.hpp"

#include <bloom/render/painted_coverage.hpp>

#include <cstddef>
#include <cstdint>

// #758: PaintedCoverageV1, the entry point. The job itself is the covered fill's
// (gpu_solid_covered.cpp) with this operation's pipeline, palette and geometry bound in place of
// the coverage palette and mask; this file holds the argument checks the shader relies on, so every
// index it reads is in bounds.

namespace bloom::render {

GpuSolidDiagnostic GpuSolid::beginPainted(const GpuSolidParameters& base,
                                          const PathPaintedCoverage& coverage,
                                          const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                  "the SolidV1 pipeline is not initialized");
    }
    const auto invalid = [](const char* message) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument, message);
    };
    if (coverage.width != base.dataWindow.extent().width() ||
        coverage.height != base.dataWindow.extent().height() ||
        coverage.rows.size() != static_cast<std::size_t>(coverage.height) * 4U) {
        return invalid("the painted coverage does not match the data window");
    }
    const std::size_t paints = coverage.paints();
    if (paints == 0 || coverage.palette.size() != paints * kPaintedCoverageLevels) {
        return invalid("the painted palette must hold 17 pixels for each paint");
    }
    for (const auto& range : coverage.rows) {
        if (static_cast<std::size_t>(range.offset) + range.count > coverage.spans.size()) {
            return invalid("a painted coverage range addresses spans it does not have");
        }
    }
    for (const auto& span : coverage.spans) {
        if (span.paint >= paints) {
            return invalid("a painted coverage span names a paint the palette does not hold");
        }
    }
    return impl_->beginCoveredJob(base, {}, 1.0F, byteBudget, VK_NULL_HANDLE, 0, nullptr, {},
                                  &coverage);
}

} // namespace bloom::render
