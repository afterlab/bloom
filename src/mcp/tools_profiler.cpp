// profiler.snapshot (#423; docs/architecture/profiler-feed.md): the runtime telemetry contract as
// JSON, with the same field names the Profiler panel and the benchmark harness use. The snapshot
// is filled on the owner thread by the host's binding (the application's sampler and UI-side hook,
// or the stdio host's own sampler); the JSON is built here, on the request worker.

#include "core_internal.hpp"

#include <bloom/runtime/profiler_snapshot_json.hpp>

#include <memory>

namespace bloom::mcp {

yyjson_mut_val* profilerSnapshotTool(McpServerCore::State& core, Json& out,
                                     const McpRequestControl& control) {
    const auto& read = core.dependencies.profilerSnapshot;
    if (!read) {
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(false));
        out.set(value, "message", out.text("This host has no profiler."));
        return value;
    }
    auto snapshot = std::make_unique<runtime::ProfilerSnapshot>();
    bool filled = false;
    if (!core.dependencies.exports.run(
            [&](host::ExportQueueService*, std::uint64_t) { filled = read(*snapshot); },
            control.stop))
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    if (!filled) {
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(false));
        out.set(value, "message", out.text("The profiler has not taken a sample yet."));
        return value;
    }
    const Parsed parsed(runtime::profilerSnapshotJson(*snapshot));
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(true));
    out.set(value, "snapshot", out.copy(parsed.root()));
    return value;
}

} // namespace bloom::mcp
