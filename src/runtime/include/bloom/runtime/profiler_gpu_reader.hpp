#pragma once

// The profiler sampler's GPU source (#423; docs/architecture/profiler-feed.md): Bloom's own device
// images from the render module's process-wide reclamation counters (live and retiring bytes,
// admission refusals, owner reclaims). A build or process without a GPU device module leaves the
// section absent. Never allocates or blocks; matches ProfilerSamplerSources::readGpu.

#include <bloom/runtime/profiler_snapshot.hpp>

namespace bloom::runtime {

void readProfilerGpuDevice(ProfilerGpu& gpu) noexcept;

} // namespace bloom::runtime
