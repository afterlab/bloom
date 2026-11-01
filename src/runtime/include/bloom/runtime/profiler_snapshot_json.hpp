#pragma once

// The ProfilerSnapshot as JSON (#423; docs/architecture/profiler-feed.md): the one serialization
// the MCP/CLI `profiler.snapshot` tool and the benchmark harness print, so a number has the same
// name and meaning on every surface. Absent metrics are JSON null, never 0. Field names are the
// snapshot's member names; enums are lowercase words. It allocates (it builds a string), so it
// never runs on the sampler thread.

#include <bloom/runtime/profiler_snapshot.hpp>

#include <string>

namespace bloom::runtime {

[[nodiscard]] std::string profilerSnapshotJson(const ProfilerSnapshot& snapshot);

} // namespace bloom::runtime
