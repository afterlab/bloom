#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/profiler_gpu_reader.hpp>

namespace bloom::runtime {

void readProfilerGpuDevice(ProfilerGpu& gpu) noexcept {
    const auto stats = render::GpuDevice::processImageReclamationStats();
    // The portable stub reports no capacity: no device module, so nothing is measured.
    if (stats.capacityPerFamily == 0)
        return;
    gpu.liveBytes = ProfilerBytes::of(stats.liveAllocationBytes);
    gpu.retiringBytes = ProfilerBytes::of(stats.retiringAllocationBytes);
    gpu.refusals = ProfilerCount::of(stats.admissionRefusals);
    gpu.reclaims = ProfilerCount::of(stats.ownerReclaims);
}

} // namespace bloom::runtime
