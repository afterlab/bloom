// The read-back queries that answer a fact an agent would otherwise have to render or provoke an
// error to learn (#390): text-metrics, layer-bounds, history, viewer, preview-cache, jobs and
// render-route (pixels lives with the render code, tools_render.cpp). They hold no project truth
// and change nothing. A state the application does not know is reported as null,
// never as a made-up value.
#include "core_internal.hpp"

#include <bloom/runtime/profiler_snapshot.hpp>
#include <bloom/scripting/node_reference.hpp>
#include <bloom/scripting/readings.hpp>

#include <algorithm>
#include <cctype>
#include <memory>

namespace bloom::mcp {
namespace {

yyjson_mut_val* pair(Json& out, const std::int64_t first, const std::int64_t second) {
    auto* value = out.array();
    out.append(value, out.signedNumber(first));
    out.append(value, out.signedNumber(second));
    return value;
}

yyjson_mut_val* timeObject(Json& out, const core::RationalTime time,
                           const document::FrameRate& rate) {
    auto* value = out.object();
    const auto count = scripting::framesAt(time, rate);
    out.set(value, "frames",
            count.exact ? out.signedNumber(count.frames) : out.real(count.approximate));
    out.set(value, "seconds", pair(out, time.numerator(), time.denominator()));
    return value;
}

yyjson_mut_val* notLive(Json& out, const std::string_view kind) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(false));
    auto* diagnostic = out.object();
    out.set(diagnostic, "code", out.text(uiRefusalCode(McpUiRefusal::NotLive)));
    out.set(diagnostic, "message",
            out.text("This server has no application window; query kind=" + std::string(kind) +
                     " works only with the in-app MCP server."));
    out.set(value, "diagnostic", diagnostic);
    return value;
}

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

// The four built-in faces a text layer can name: the stored choice, the label a Text layer's font
// shows, and the family|style prefix of its font reference.
struct Face final {
    std::int64_t value;
    std::string_view label;
    std::string_view reference;
};
constexpr std::array<Face, 4> kFaces{{{0, "DejaVu Sans", "DejaVu Sans|Book"},
                                      {1, "Inter Regular", "Inter|Regular"},
                                      {2, "Inter Medium", "Inter|Medium"},
                                      {3, "Inter SemiBold", "Inter|SemiBold"}}};

const Face& faceArgument(yyjson_val* value) {
    if (value == nullptr)
        return kFaces[0];
    if (yyjson_is_int(value)) {
        const auto number = yyjson_get_sint(value);
        for (const auto& face : kFaces)
            if (face.value == number)
                return face;
    } else if (yyjson_is_str(value)) {
        const auto text = lowered(string(value, 512));
        for (const auto& face : kFaces)
            if (text == lowered(face.label) || text.starts_with(lowered(face.reference)))
                return face;
    }
    throw InvalidInput(
        "font names one of the built-in faces: \"DejaVu Sans\", \"Inter Regular\", \"Inter "
        "Medium\", \"Inter SemiBold\" (or its stored value 0 to 3, or its family|style|digest "
        "reference). A font asset's own metrics are not measured here.");
}

yyjson_mut_val* textMetrics(Json& out, McpCall& call, yyjson_val* arguments) {
    members(arguments, {"kind", "text", "font", "size"}, {"kind", "text", "size"});
    auto* text = member(arguments, "text");
    if (!yyjson_is_str(text))
        throw InvalidInput("text is the string to measure");
    auto* size = member(arguments, "size");
    if (!yyjson_is_num(size))
        throw InvalidInput("size is a number: pixels per em");
    const auto& face = faceArgument(member(arguments, "font"));
    scripting::TextMetricsRequest request;
    request.text = string(text, 4096);
    request.font = face.value;
    request.size = yyjson_get_num(size);
    const auto measured = scripting::measureText(request);
    if (!measured.ok)
        throw InvalidInput(measured.diagnostic);
    auto* result = out.object();
    out.set(result, "revision", out.number(call.session.snapshot().revision().value()));
    auto* font = out.object();
    out.set(font, "value", out.signedNumber(face.value));
    out.set(font, "label", out.text(std::string(face.label)));
    out.set(result, "font", font);
    out.set(result, "size", out.real(request.size));
    out.set(result, "unit", out.text("pixels"));
    out.set(result, "space",
            out.text("Layout pixels at 1:1: x to the right of the text origin (the left edge of "
                     "the first glyph's advance), y down from the top of the first line. Point "
                     "text, left aligned, no wrapping; line height 1.0 and no letter spacing."));
    out.set(result, "advance", out.real(measured.advance));
    out.set(result, "lineHeight", out.real(measured.lineHeight));
    auto* lines = out.array();
    for (const auto& line : measured.lines) {
        auto* row = out.object();
        out.set(row, "index", out.number(line.index));
        out.set(row, "x", out.real(line.x));
        out.set(row, "y", out.real(line.y));
        out.set(row, "advance", out.real(line.advance));
        out.set(row, "height", out.real(line.height));
        out.set(row, "byteBegin", out.number(line.byteBegin));
        out.set(row, "byteEnd", out.number(line.byteEnd));
        out.append(lines, row);
    }
    out.set(result, "lines", lines);
    if (measured.hasInk) {
        auto* ink = out.object();
        out.set(ink, "x", out.signedNumber(measured.inkX));
        out.set(ink, "y", out.signedNumber(measured.inkY));
        out.set(ink, "width", out.number(measured.inkWidth));
        out.set(ink, "height", out.number(measured.inkHeight));
        out.set(result, "ink", ink);
    } else {
        out.set(result, "ink", out.null());
    }
    return result;
}

yyjson_mut_val* layerBounds(Json& out, McpCall& call, yyjson_val* arguments) {
    members(arguments, {"kind", "composition", "layer", "time"}, {"kind", "composition", "layer"});
    const auto snapshot = call.session.snapshot();
    const auto compositionId = integer(member(arguments, "composition"));
    const auto layerId = integer(member(arguments, "layer"));
    if (compositionId == 0 || layerId == 0)
        throw InvalidInput("composition and layer are ids (query kind=compositions and layers)");
    const auto* composition =
        snapshot.project().findComposition(document::CompositionId::fromRaw(compositionId));
    if (composition == nullptr)
        throw InvalidInput("Composition " + std::to_string(compositionId) +
                           " does not exist; query kind=compositions lists them");
    core::RationalTime time = call.context.time.value_or(core::RationalTime::fromInteger(0));
    if (auto* requested = member(arguments, "time"))
        time = frameOrSecondsArgument(requested, snapshot, compositionId);
    const auto reading =
        scripting::measureLayerBounds(call.context, document::CompositionId::fromRaw(compositionId),
                                      time, document::LayerId::fromRaw(layerId));
    if (reading.status == scripting::LayerBoundsStatus::Cancelled)
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    auto* result = out.object();
    out.set(result, "revision", out.number(snapshot.revision().value()));
    out.set(result, "composition", out.number(compositionId));
    out.set(result, "layer", out.number(layerId));
    out.set(result, "time", timeObject(out, time, composition->format().frameRate()));
    out.set(result, "visible",
            out.boolean(reading.status == scripting::LayerBoundsStatus::Measured));
    if (reading.status != scripting::LayerBoundsStatus::Measured) {
        out.set(result, "status",
                out.text(reading.status == scripting::LayerBoundsStatus::NotVisible
                             ? "not-visible"
                             : "unavailable"));
        out.set(result, "reason", out.text(reading.diagnostic));
        return result;
    }
    out.set(result, "space",
            out.text("bounds, corners and anchor are in composition pixels: x to the right, y "
                     "down, origin at the frame's top-left corner, before any clipping to the "
                     "frame. local is the layer's content rectangle before its transform: pixel "
                     "edges, origin at the top-left of the layer's content."));
    auto* local = out.object();
    out.set(local, "left", out.real(reading.localLeft));
    out.set(local, "top", out.real(reading.localTop));
    out.set(local, "right", out.real(reading.localRight));
    out.set(local, "bottom", out.real(reading.localBottom));
    out.set(result, "local", local);
    auto* bounds = out.object();
    out.set(bounds, "left", out.real(reading.boundsLeft));
    out.set(bounds, "top", out.real(reading.boundsTop));
    out.set(bounds, "right", out.real(reading.boundsRight));
    out.set(bounds, "bottom", out.real(reading.boundsBottom));
    out.set(result, "bounds", bounds);
    auto* corners = out.array();
    for (const auto& corner : reading.corners) {
        auto* point = out.array();
        out.append(point, out.real(corner[0]));
        out.append(point, out.real(corner[1]));
        out.append(corners, point);
    }
    out.set(result, "corners", corners);
    auto* anchor = out.array();
    out.append(anchor, out.real(reading.anchorX));
    out.append(anchor, out.real(reading.anchorY));
    out.set(result, "anchor", anchor);
    return result;
}

yyjson_mut_val* historyQuery(McpServerCore::State& core, Json& out, McpCall& call,
                             yyjson_val* arguments) {
    members(arguments, {"kind"}, {"kind"});
    const auto snapshot = call.session.snapshot();
    std::vector<std::string> labels;
    std::size_t position = 0;
    if (core.liveHost() != nullptr) {
        const auto& ui = core.dependencies.ui;
        if (!ui.undoHistory)
            return notLive(out, "history");
        McpUndoHistory history;
        if (!core.dependencies.exports.run(
                [&](host::ExportQueueService*, std::uint64_t) { history = ui.undoHistory(); },
                call.control.stop))
            throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
        labels = std::move(history.labels);
        position = history.position;
    } else if (auto* stack = call.session.commandStack()) {
        labels = stack->historyLabels();
        position = stack->historyPosition();
    }
    position = std::min(position, labels.size());
    auto* result = out.object();
    out.set(result, "revision", out.number(snapshot.revision().value()));
    out.set(result, "canUndo", out.boolean(position > 0));
    out.set(result, "canRedo", out.boolean(position < labels.size()));
    out.set(result, "undoLabel", position > 0 ? out.text(labels[position - 1]) : out.null());
    out.set(result, "redoLabel",
            position < labels.size() ? out.text(labels[position]) : out.null());
    out.set(result, "position", out.number(position));
    auto* entries = out.array();
    for (std::size_t index = 0; index < labels.size(); ++index) {
        auto* row = out.object();
        out.set(row, "index", out.number(index));
        out.set(row, "label", out.text(labels[index]));
        out.set(row, "applied", out.boolean(index < position));
        out.append(entries, row);
    }
    out.set(result, "entries", entries);
    return result;
}

constexpr std::array<std::string_view, 6> kChannels{"rgba", "rgb", "red", "green", "blue", "alpha"};
constexpr std::array<std::string_view, 4> kResolutions{"auto", "full", "half", "quarter"};
constexpr std::array<std::string_view, 4> kBackgrounds{"solid", "gray", "black", "white"};

yyjson_mut_val* viewerQuery(McpServerCore::State& core, Json& out, McpCall& call,
                            yyjson_val* arguments) {
    members(arguments, {"kind"}, {"kind"});
    const auto& ui = core.dependencies.ui;
    if (core.liveHost() == nullptr || !ui.viewerState)
        return notLive(out, "viewer");
    McpViewerState state;
    if (!core.dependencies.exports.run(
            [&](host::ExportQueueService*, std::uint64_t) { state = ui.viewerState(); },
            call.control.stop))
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    auto* result = out.object();
    out.set(result, "revision", out.number(call.session.snapshot().revision().value()));
    out.set(result, "available", out.boolean(state.available));
    if (!state.available)
        return result;
    out.set(result, "display", out.text(state.display));
    out.set(result, "view", out.text(state.view));
    out.set(result, "exposure", out.real(state.exposure));
    out.set(result, "gamma", out.real(state.gamma));
    out.set(result, "channel", out.text(std::string(kChannels.at(static_cast<std::size_t>(state.channel)))));
    out.set(result, "zoom", state.fit ? out.text("fit") : out.real(state.zoomFactor));
    out.set(result, "resolution",
            out.text(std::string(kResolutions.at(static_cast<std::size_t>(state.resolution)))));
    out.set(result, "background",
            out.text(std::string(kBackgrounds.at(static_cast<std::size_t>(state.background)))));
    out.set(result, "transparencyGrid", out.boolean(state.transparencyGrid));
    out.set(result, "look", out.boolean(state.look));
    if (state.regionOfInterest) {
        auto* region = out.object();
        out.set(region, "x", out.real(state.regionOfInterest->x));
        out.set(region, "y", out.real(state.regionOfInterest->y));
        out.set(region, "width", out.real(state.regionOfInterest->width));
        out.set(region, "height", out.real(state.regionOfInterest->height));
        out.set(result, "regionOfInterest", region);
    } else {
        out.set(result, "regionOfInterest", out.null());
    }
    return result;
}

yyjson_mut_val* previewCacheQuery(McpServerCore::State& core, Json& out, McpCall& call,
                                  yyjson_val* arguments) {
    members(arguments, {"kind"}, {"kind"});
    const auto& ui = core.dependencies.ui;
    if (core.liveHost() == nullptr || !ui.previewCache)
        return notLive(out, "preview-cache");
    McpPreviewCacheState state;
    // The fill rate and the cache's budget come from the profiler's last sample; without one they
    // are unknown.
    auto sample = std::make_unique<runtime::ProfilerSnapshot>();
    bool sampled = false;
    if (!core.dependencies.exports.run(
            [&](host::ExportQueueService*, std::uint64_t) {
                state = ui.previewCache();
                if (core.dependencies.profilerSnapshot)
                    sampled = core.dependencies.profilerSnapshot(*sample);
            },
            call.control.stop))
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    auto* result = out.object();
    out.set(result, "revision", out.number(call.session.snapshot().revision().value()));
    out.set(result, "available", out.boolean(state.available));
    if (!state.available)
        return result;
    out.set(result, "state",
            state.filling ? out.text(*state.filling ? "filling" : "idle") : out.null());
    out.set(result, "cachedFrames",
            state.cachedFrames ? out.number(*state.cachedFrames) : out.null());
    out.set(result, "totalFrames",
            state.totalFrames ? out.number(*state.totalFrames) : out.null());
    // The longest run of consecutive cached frames: composition frame indices, end exclusive.
    if (state.longestRun && state.longestRun->second > state.longestRun->first) {
        auto* run = out.object();
        out.set(run, "firstFrame", out.number(state.longestRun->first));
        out.set(run, "endFrame", out.number(state.longestRun->second));
        out.set(run, "frames", out.number(state.longestRun->second - state.longestRun->first));
        out.set(result, "filledRange", run);
    } else {
        out.set(result, "filledRange", out.null());
    }
    // Frames per second; unknown (null) without a profiler sample, and unknown while no fill ran.
    const bool rateKnown = sampled && sample->workload.ramPreviewFillFps.present;
    out.set(result, "fillRate",
            rateKnown ? out.real(static_cast<double>(sample->workload.ramPreviewFillFps.value)) : out.null());
    // The Preview frames pool's ceiling and what it holds, in bytes.
    yyjson_mut_val* budget = out.null();
    yyjson_mut_val* held = out.null();
    if (sampled) {
        const auto count =
            std::min<std::size_t>(sample->memory.poolCount, sample->memory.pools.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& pool = sample->memory.pools[index];
            if (runtime::profilerNameView(pool.name) != "Preview frames")
                continue;
            held = out.number(pool.hostBytes);
            if (pool.ceilingBytes.present)
                budget = out.number(pool.ceilingBytes.value);
        }
    }
    out.set(result, "budgetBytes", budget);
    out.set(result, "heldBytes", held);
    // Why the last fill stopped; null while filling and before any fill ended.
    yyjson_mut_val* stop = out.null();
    if (state.stopReason) {
        constexpr std::array<std::string_view, 4> names{"complete", "budget", "cancelled",
                                                        "failed"};
        stop = out.text(std::string(names.at(static_cast<std::size_t>(*state.stopReason))));
    }
    out.set(result, "stopReason", stop);
    return result;
}

// The Jobs panel's rows that are not export runs: imports, previews, thumbnails, purges and other
// background work, with a stable kind and state and, for a failed job, the reason it recorded.
yyjson_mut_val* jobsQuery(McpServerCore::State& core, Json& out, McpCall& call,
                          yyjson_val* arguments) {
    members(arguments, {"kind"}, {"kind"});
    const auto& ui = core.dependencies.ui;
    if (core.liveHost() == nullptr || !ui.jobs)
        return notLive(out, "jobs");
    std::vector<McpJob> rows;
    if (!core.dependencies.exports.run(
            [&](host::ExportQueueService*, std::uint64_t) { rows = ui.jobs(); },
            call.control.stop))
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    auto* result = out.object();
    out.set(result, "revision", out.number(call.session.snapshot().revision().value()));
    auto* jobs = out.array();
    for (const auto& job : rows) {
        // An export run is read with export.status.
        if (job.exportJob || job.kind == "export")
            continue;
        auto* row = out.object();
        out.set(row, "id", out.number(job.id));
        out.set(row, "name", out.text(job.name));
        out.set(row, "kind", job.kind.empty() ? out.null() : out.text(job.kind));
        out.set(row, "state", job.stateToken.empty() ? out.null() : out.text(job.stateToken));
        auto* progress = out.object();
        out.set(progress, "phase", out.text(job.phase));
        out.set(progress, "completed", job.completed ? out.number(*job.completed) : out.null());
        out.set(progress, "total", job.total ? out.number(*job.total) : out.null());
        out.set(row, "progress", progress);
        out.set(row, "failure", job.failure.empty() ? out.null() : out.text(job.failure));
        out.set(row, "canCancel", out.boolean(job.canCancel));
        out.set(row, "cancellationRequested", out.boolean(job.cancellationRequested));
        out.append(jobs, row);
    }
    out.set(result, "jobs", jobs);
    return result;
}

yyjson_mut_val* routeText(Json& out, const std::optional<McpRouteKind> route) {
    return route ? out.text(*route == McpRouteKind::Gpu ? "gpu" : "cpu") : out.null();
}

// Which route produced the Viewer's displayed frame, exactly as the frame recorded it, and the
// preview service's running count of frames that fell back to the CPU, by reason.
yyjson_mut_val* renderRouteQuery(McpServerCore::State& core, Json& out, McpCall& call,
                                 yyjson_val* arguments) {
    members(arguments, {"kind"}, {"kind"});
    const auto& ui = core.dependencies.ui;
    if (core.liveHost() == nullptr || !ui.viewerRoute)
        return notLive(out, "render-route");
    McpViewerRoute route;
    auto sample = std::make_unique<runtime::ProfilerSnapshot>();
    bool sampled = false;
    if (!core.dependencies.exports.run(
            [&](host::ExportQueueService*, std::uint64_t) {
                route = ui.viewerRoute();
                if (core.dependencies.profilerSnapshot)
                    sampled = core.dependencies.profilerSnapshot(*sample);
            },
            call.control.stop))
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    auto* result = out.object();
    out.set(result, "revision", out.number(call.session.snapshot().revision().value()));
    out.set(result, "available", out.boolean(route.available));
    if (!route.available)
        return result;
    out.set(result, "hasFrame", out.boolean(route.hasFrame));
    out.set(result, "freshness", out.text(route.freshness));
    if (route.hasFrame) {
        yyjson_mut_val* time = out.null();
        if (route.time) {
            time = out.array();
            out.append(time, out.signedNumber(route.time->numerator()));
            out.append(time, out.signedNumber(route.time->denominator()));
        }
        out.set(result, "time", time);
        out.set(result, "scene", routeText(out, route.scene));
        out.set(result, "display", routeText(out, route.display));
        out.set(result, "provider", out.text(route.provider));
        out.set(result, "label", out.text(route.label));
        out.set(result, "why", route.note.empty() ? out.null() : out.text(route.note));
    } else {
        for (const auto* name : {"time", "scene", "display", "provider", "label", "why"})
            out.set(result, name, out.null());
    }
    // Cumulative since the application started, not per frame: the service counts every request
    // that took the CPU path against the one reason it recorded.
    yyjson_mut_val* fallbacks = out.null();
    if (sampled && sample->viewer.cpuFallbacks.present) {
        constexpr std::array<std::string_view, runtime::kProfilerCpuFallbackReasonCount> names{
            "gpu-unavailable",        "non-neutral-request", "not-worthwhile", "ineligible",
            "unsupported-gpu-subset", "native-failure",      "native-timeout"};
        fallbacks = out.object();
        out.set(fallbacks, "total", out.number(sample->viewer.cpuFallbacks.value));
        auto* reasons = out.object();
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto& count = sample->viewer.cpuFallbacksByReason[index];
            out.set(reasons, names[index],
                    count.present ? out.number(count.value) : out.null());
        }
        out.set(fallbacks, "byReason", reasons);
    }
    out.set(result, "cpuFallbacks", fallbacks);
    return result;
}

} // namespace

bool isInspectionQuery(const std::string_view kind) noexcept {
    return kind == "text-metrics" || kind == "layer-bounds" || kind == "history" ||
           kind == "viewer" || kind == "preview-cache" || kind == "jobs" ||
           kind == "render-route" || kind == "pixels";
}

yyjson_mut_val* inspectionQuery(McpServerCore::State& core, Json& out, McpCall& call,
                                yyjson_val* arguments, const std::string_view kind) {
    if (kind == "text-metrics")
        return textMetrics(out, call, arguments);
    if (kind == "layer-bounds")
        return layerBounds(out, call, arguments);
    if (kind == "history")
        return historyQuery(core, out, call, arguments);
    if (kind == "viewer")
        return viewerQuery(core, out, call, arguments);
    if (kind == "jobs")
        return jobsQuery(core, out, call, arguments);
    if (kind == "render-route")
        return renderRouteQuery(core, out, call, arguments);
    if (kind == "pixels")
        return pixelsQuery(core, out, call, arguments);
    return previewCacheQuery(core, out, call, arguments);
}

} // namespace bloom::mcp
