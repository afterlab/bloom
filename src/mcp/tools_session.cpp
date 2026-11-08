// The tools that act on the application window rather than the project (live_ui_port.hpp): project
// lifecycle, editor.reveal, selection, active composition, transport, Viewer settings and
// export.reveal. They run through McpExportAccess::run on the owner thread and hold no session
// lease, so project.open works while the old project is being replaced. Session-state tools never
// dirty the project.
#include "core_internal.hpp"

#include <bloom/document/project.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace bloom::mcp {
namespace {

yyjson_mut_val* refusal(Json& out, const McpUiRefusal code, const std::string& message) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(false));
    auto* diagnostic = out.object();
    out.set(diagnostic, "code", out.text(uiRefusalCode(code)));
    out.set(diagnostic, "message", out.text(message));
    out.set(value, "diagnostic", diagnostic);
    return value;
}

yyjson_mut_val* uiResult(Json& out, const McpUiResult& result) {
    if (!result.ok())
        return refusal(out, result.refusal,
                       result.message.empty() ? "Bloom refused the request." : result.message);
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(true));
    if (!result.message.empty())
        out.set(value, "message", out.text(result.message));
    return value;
}

// Runs `work` on the owner thread; false when the request stopped or the server is stopping.
template <typename Work>
bool onOwner(McpServerCore::State& core, const McpRequestControl& control, Work work) {
    return core.dependencies.exports.run(
        [&work](host::ExportQueueService*, std::uint64_t) { work(); }, control.stop);
}

[[noreturn]] void cancelled() {
    throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
}

std::uint64_t compositionArgument(yyjson_val* arguments) {
    const auto id = integer(member(arguments, "composition"));
    if (id == 0)
        throw InvalidInput("composition must be a composition id (query kind=compositions)");
    return id;
}

// The current snapshot and context composition, read through a short lease on the live host or
// from the headless session.
struct Current final {
    document::Snapshot snapshot;
    std::optional<std::uint64_t> composition;
    // The selected nodes and layers (live only).
    std::vector<std::uint64_t> nodes;
    std::vector<std::uint64_t> layers;
};

Current current(McpServerCore::State& core, const McpRequestControl& control) {
    if (auto* live = core.liveHost()) {
        auto lease =
            live->acquire({.stop = control.stop,
                           .deadline = std::chrono::steady_clock::now() + kMcpCommitDeadline});
        if (!lease)
            throw ToolFailure(McpToolErrorCode::ProjectUnavailable, "A project is opening.");
        const auto context = lease.operationContext();
        return {lease.session().snapshot(), context.composition, context.selection,
                context.layers};
    }
    auto& session =
        std::get<std::reference_wrapper<scripting::Session>>(core.dependencies.session).get();
    return {session.snapshot(), session.operationContext().composition, {}, {}};
}

} // namespace

// The time contract every client shares: an integer is a whole frame of `composition` at its frame
// rate, and [numerator, denominator] is an exact time in seconds.
core::RationalTime frameOrSecondsArgument(yyjson_val* value, const document::Snapshot& snapshot,
                                          const std::uint64_t compositionId) {
    if (yyjson_is_arr(value) && yyjson_arr_size(value) == 2 &&
        yyjson_is_int(yyjson_arr_get(value, 0)) && yyjson_is_int(yyjson_arr_get(value, 1))) {
        const auto time = core::RationalTime::create(yyjson_get_sint(yyjson_arr_get(value, 0)),
                                                     yyjson_get_sint(yyjson_arr_get(value, 1)));
        if (!time)
            throw InvalidInput("time [numerator, denominator] needs a non-zero denominator");
        return *time;
    }
    if (!yyjson_is_int(value))
        throw InvalidInput("time is a whole frame (integer) or [numerator, denominator] seconds");
    const auto* composition =
        snapshot.project().findComposition(document::CompositionId::fromRaw(compositionId));
    if (composition == nullptr)
        throw InvalidInput("Composition " + std::to_string(compositionId) +
                           " does not exist; query kind=compositions lists them");
    const auto rate = composition->format().frameRate();
    const auto frames = yyjson_get_sint(value);
    constexpr auto limit = std::numeric_limits<std::int64_t>::max() / 4294967296LL;
    const auto time =
        frames > limit || frames < -limit
            ? std::nullopt
            : core::RationalTime::create(frames * static_cast<std::int64_t>(rate.denominator()),
                                         static_cast<std::int64_t>(rate.numerator()));
    if (!time)
        throw InvalidInput("time is too large for the composition's frame rate");
    return *time;
}

namespace {

// The nodes linked to `seeds` upstream or downstream, seeds included, through the same links the
// Nodes canvas draws: graph edges and driver bindings (NodeGraphEditor::linkedNodes).
std::vector<std::uint64_t> linkedNodes(const document::Composition& composition,
                                       const std::vector<std::uint64_t>& seeds,
                                       const bool upstream) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> links;
    for (const auto& edge : composition.graph().edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination))
            links.emplace_back(edge.source.nodeId.value(), input->nodeId.value());
    for (const auto& node : composition.graph().nodes())
        for (const auto& binding : node.parameters)
            if (const auto* parameter = composition.parameters().find(binding.parameterId))
                if (const auto* driver =
                        std::get_if<document::DriverBindingSource>(&parameter->source))
                    links.emplace_back(driver->sourceNodeId.value(), node.id.value());
    std::vector<std::uint64_t> visited = seeds;
    std::vector<std::uint64_t> frontier = seeds;
    while (!frontier.empty()) {
        std::vector<std::uint64_t> next;
        for (const auto id : frontier)
            for (const auto& [from, to] : links) {
                const auto neighbor = upstream ? (to == id ? from : 0) : (from == id ? to : 0);
                if (neighbor != 0 && std::ranges::find(visited, neighbor) == visited.end()) {
                    visited.push_back(neighbor);
                    next.push_back(neighbor);
                }
            }
        frontier = std::move(next);
    }
    return visited;
}

yyjson_mut_val* projectTool(McpServerCore::State& core, Json& out, const McpTool tool,
                            yyjson_val* arguments, const McpRequestControl& control,
                            std::string& summary) {
    McpProjectRequest request;
    switch (tool) {
    case McpTool::ProjectSave:
        members(arguments, {});
        request.kind = McpProjectRequestKind::Save;
        break;
    case McpTool::ProjectSaveAs:
        members(arguments, {"path", "replace"}, {"path"});
        request.kind = McpProjectRequestKind::SaveAs;
        request.path = string(member(arguments, "path"));
        request.replace = member(arguments, "replace") && boolean(member(arguments, "replace"));
        break;
    case McpTool::ProjectOpen:
        members(arguments, {"path", "discardChanges", "stopJobs"}, {"path"});
        request.kind = McpProjectRequestKind::Open;
        request.path = string(member(arguments, "path"));
        break;
    case McpTool::ProjectNew:
        members(arguments, {"discardChanges", "stopJobs"});
        request.kind = McpProjectRequestKind::New;
        break;
    case McpTool::ProjectSaveCopy:
        members(arguments, {"path", "replace"}, {"path"});
        request.kind = McpProjectRequestKind::SaveCopy;
        request.path = string(member(arguments, "path"));
        request.replace = member(arguments, "replace") && boolean(member(arguments, "replace"));
        break;
    default:
        members(arguments, {"discardChanges", "stopJobs"});
        request.kind = McpProjectRequestKind::Close;
        break;
    }
    if (tool == McpTool::ProjectOpen || tool == McpTool::ProjectNew ||
        tool == McpTool::ProjectClose) {
        request.discardChanges =
            member(arguments, "discardChanges") && boolean(member(arguments, "discardChanges"));
        request.stopJobs = member(arguments, "stopJobs") && boolean(member(arguments, "stopJobs"));
    }
    if ((request.kind == McpProjectRequestKind::SaveAs ||
         request.kind == McpProjectRequestKind::SaveCopy ||
         request.kind == McpProjectRequestKind::Open) &&
        (request.path.empty() || !request.path.is_absolute()))
        return refusal(out, McpUiRefusal::PathInvalid,
                       "The path must be an absolute path to a .bloom file.");
    const auto& lifecycle = core.dependencies.ui.projectLifecycle;
    if (!lifecycle)
        return refusal(out, McpUiRefusal::Unbound,
                       "This Bloom window cannot open or save projects for agents yet.");

    struct Pending {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::optional<McpProjectResult> result;
    };
    const auto pending = std::make_shared<Pending>();
    const auto ran = onOwner(core, control, [&] {
        lifecycle(request, [pending](McpProjectResult result) {
            {
                const std::lock_guard lock(pending->mutex);
                if (!pending->result)
                    pending->result = std::move(result);
            }
            pending->changed.notify_all();
        });
    });
    if (!ran)
        cancelled();
    std::unique_lock lock(pending->mutex);
    pending->changed.wait(lock, control.stop, [&] { return pending->result.has_value(); });
    if (!pending->result)
        throw ToolFailure(McpToolErrorCode::Cancelled,
                          "The request was cancelled; the project operation may still finish. "
                          "Query the project to see where it ended.");
    const auto result = *pending->result;
    lock.unlock();
    // The activity log reads the project's name after the tool name ("project.save SC020").
    summary = result.ok() && !result.projectName.empty() ? result.projectName
                                                         : std::string(toolName(tool));
    if (!result.ok())
        return refusal(out, result.refusal,
                       result.message.empty() ? "Bloom refused the request." : result.message);
    constexpr std::array<std::string_view, 6> kinds{"project.saved",   "project.saved",
                                                    "project.opened",  "project.created",
                                                    "project.closed",  "project.copy-saved"};
    McpNotice notice{.kind = std::string(kinds.at(static_cast<std::size_t>(request.kind))),
                     .message = result.message,
                     .revision = 0,
                     .composition = std::nullopt,
                     .node = std::nullopt,
                     .parameter = std::nullopt,
                     .path = result.path.generic_string(),
                     .projectName = result.projectName};
    try {
        notice.revision = current(core, control).snapshot.revision().value();
    } catch (const std::exception&) {
        // The new project is still binding; the notice keeps revision 0.
    }
    core.notice(notice);
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(true));
    out.set(value, "event", out.text(notice.kind));
    out.set(value, "path", result.path.empty() ? out.null() : out.text(notice.path));
    out.set(value, "projectName", out.text(result.projectName));
    if (!result.message.empty())
        out.set(value, "message", out.text(result.message));
    return value;
}

std::vector<std::uint64_t> idList(yyjson_val* value, const char* name) {
    std::vector<std::uint64_t> ids;
    if (value == nullptr)
        return ids;
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) > 4096)
        throw InvalidInput(std::string(name) + " is a list of at most 4096 ids");
    yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
    while (auto* item = yyjson_arr_iter_next(&iterator)) {
        const auto id = integer(item);
        if (id == 0)
            throw InvalidInput(std::string(name) + " ids are positive");
        ids.push_back(id);
    }
    return ids;
}

// A live-only tool whose whole work is one port call on the owner thread answering McpUiResult.
template <typename Member, typename Call>
yyjson_mut_val* simpleCall(McpServerCore::State& core, Json& out, const McpRequestControl& control,
                           std::string& summary, const McpTool tool, const bool live,
                           const Member& member, Call call) {
    if (!live)
        return refusal(out, McpUiRefusal::NotLive,
                       "This server has no application window; " + std::string(toolName(tool)) +
                           " works only with the in-app MCP server.");
    if (!member)
        return refusal(out, McpUiRefusal::Unbound,
                       "This Bloom window does not offer " + std::string(toolName(tool)) +
                           " to agents yet.");
    McpUiResult result;
    if (!onOwner(core, control, [&] { result = call(); }))
        cancelled();
    summary = std::string(toolName(tool));
    return uiResult(out, result);
}

} // namespace

yyjson_mut_val* sessionTool(McpServerCore::State& core, Json& out, const McpTool tool,
                            yyjson_val* arguments, const McpRequestControl& control,
                            std::string& summary) {
    if ((tool >= McpTool::ProjectSave && tool <= McpTool::ProjectClose) ||
        tool == McpTool::ProjectSaveCopy)
        return projectTool(core, out, tool, arguments, control, summary);
    const bool live = core.liveHost() != nullptr;
    const auto& ui = core.dependencies.ui;
    const auto notLive = [&] {
        return refusal(out, McpUiRefusal::NotLive,
                       "This server has no application window; " + std::string(toolName(tool)) +
                           " works only with the in-app MCP server.");
    };
    const auto unbound = [&] {
        return refusal(out, McpUiRefusal::Unbound,
                       "This Bloom window does not offer " + std::string(toolName(tool)) +
                           " to agents yet.");
    };
    switch (tool) {
    case McpTool::EditorReveal: {
        members(arguments, {"composition", "time"}, {"composition"});
        const auto composition = compositionArgument(arguments);
        if (!live)
            return notLive();
        if (!ui.revealComposition)
            return unbound();
        std::optional<core::RationalTime> time;
        if (auto* value = member(arguments, "time"))
            time = frameOrSecondsArgument(value, current(core, control).snapshot, composition);
        bool shown = false;
        if (!onOwner(core, control, [&] {
                shown = ui.revealComposition(document::CompositionId::fromRaw(composition), time);
            }))
            cancelled();
        summary = "editor.reveal";
        if (!shown)
            return refusal(out, McpUiRefusal::UnknownTarget,
                           "Composition " + std::to_string(composition) +
                               " could not be shown; query kind=compositions lists the "
                               "compositions this project has.");
        return uiResult(out, {});
    }
    case McpTool::SelectionSet: {
        members(arguments, {"composition", "layers", "nodes", "mode"}, {"composition"});
        McpSelectionRequest request;
        request.composition = document::CompositionId::fromRaw(compositionArgument(arguments));
        for (const auto id : idList(member(arguments, "layers"), "layers"))
            request.layers.push_back(document::LayerId::fromRaw(id));
        for (const auto id : idList(member(arguments, "nodes"), "nodes"))
            request.nodes.push_back(document::NodeId::fromRaw(id));
        // The Select menus' whole-set commands resolve here to a plain set (Select All, Invert,
        // Linked Upstream and Downstream); the window then selects exactly that.
        std::string derived;
        if (auto* mode = member(arguments, "mode")) {
            const auto name = string(mode, 24);
            constexpr std::array<std::string_view, 4> modes{"set", "add", "remove", "clear"};
            constexpr std::array<std::string_view, 6> derivedModes{
                "all-layers",    "all-nodes",       "invert-layers",
                "invert-nodes",  "linked-upstream", "linked-downstream"};
            const auto found = std::ranges::find(modes, name);
            if (found != modes.end())
                request.mode = static_cast<McpSelectionMode>(std::distance(modes.begin(), found));
            else if (std::ranges::find(derivedModes, name) != derivedModes.end())
                derived = name;
            else
                throw InvalidInput("mode is set, add, remove, clear, all-layers, all-nodes, "
                                   "invert-layers, invert-nodes, linked-upstream or "
                                   "linked-downstream");
        }
        if (!live)
            return notLive();
        if (!ui.setSelection)
            return unbound();
        if (!derived.empty()) {
            const auto now = current(core, control);
            const auto* composition =
                now.snapshot.project().findComposition(request.composition);
            if (composition == nullptr)
                return refusal(out, McpUiRefusal::UnknownTarget,
                               "Composition " + std::to_string(request.composition.value()) +
                                   " does not exist; query kind=compositions lists them.");
            // Invert and the linked modes read the selection of the active composition.
            const bool active = now.composition == request.composition.value();
            std::vector<std::uint64_t> layers;
            std::vector<std::uint64_t> nodes;
            if (derived == "all-layers" || derived == "invert-layers") {
                for (const auto& layer : composition->graph().layerOutputs())
                    if (derived == "all-layers" || !active ||
                        std::ranges::find(now.layers, layer.layerId.value()) == now.layers.end())
                        layers.push_back(layer.layerId.value());
            } else if (derived == "all-nodes" || derived == "invert-nodes") {
                for (const auto& node : composition->graph().nodes())
                    if (derived == "all-nodes" || !active ||
                        std::ranges::find(now.nodes, node.id.value()) == now.nodes.end())
                        nodes.push_back(node.id.value());
            } else {
                std::vector<std::uint64_t> seeds;
                for (const auto node : request.nodes)
                    seeds.push_back(node.value());
                if (seeds.empty() && active)
                    seeds = now.nodes;
                if (seeds.empty())
                    return refusal(out, McpUiRefusal::InvalidArgument,
                                   "Select a node first, or pass nodes to extend from.");
                for (const auto seed : seeds)
                    if (composition->graph().findNode(document::NodeId::fromRaw(seed)) == nullptr)
                        return refusal(out, McpUiRefusal::UnknownTarget,
                                       "Node " + std::to_string(seed) +
                                           " is not in the composition.");
                nodes = linkedNodes(*composition, seeds, derived == "linked-upstream");
            }
            request.layers.clear();
            request.nodes.clear();
            for (const auto id : layers)
                request.layers.push_back(document::LayerId::fromRaw(id));
            for (const auto id : nodes)
                request.nodes.push_back(document::NodeId::fromRaw(id));
            request.mode = request.layers.empty() && request.nodes.empty()
                               ? McpSelectionMode::Clear
                               : McpSelectionMode::Set;
        }
        McpUiResult result;
        if (!onOwner(core, control, [&] { result = ui.setSelection(request); }))
            cancelled();
        summary = "selection.set";
        return uiResult(out, result);
    }
    case McpTool::CompositionActivate: {
        members(arguments, {"composition"}, {"composition"});
        const auto composition = compositionArgument(arguments);
        if (!live)
            return notLive();
        if (!ui.activateComposition)
            return unbound();
        McpUiResult result;
        if (!onOwner(core, control, [&] {
                result = ui.activateComposition(document::CompositionId::fromRaw(composition));
            }))
            cancelled();
        summary = "composition.activate";
        return uiResult(out, result);
    }
    case McpTool::Transport: {
        members(arguments, {"action", "composition", "time"}, {"action"});
        const auto action = string(member(arguments, "action"), 16);
        constexpr std::array<std::string_view, 8> actions{
            "seek",        "play",          "stop",        "step-forward",
            "step-backward", "go-to-start", "go-to-end",  "ram-preview"};
        const auto found = std::ranges::find(actions, action);
        if (found == actions.end())
            throw InvalidInput("action is seek, play, stop, step-forward, step-backward, "
                               "go-to-start, go-to-end or ram-preview");
        McpTransportRequest request;
        request.action = static_cast<McpTransportAction>(std::distance(actions.begin(), found));
        if (request.action == McpTransportAction::Seek && !member(arguments, "time"))
            throw InvalidInput("seek needs time: a whole frame, or [numerator, denominator] "
                               "seconds");
        if (!live)
            return notLive();
        if (!ui.transport)
            return unbound();
        if (auto* value = member(arguments, "time")) {
            const auto now = current(core, control);
            const auto composition = member(arguments, "composition")
                                         ? compositionArgument(arguments)
                                         : now.composition.value_or(0);
            if (composition == 0 && yyjson_is_int(value))
                throw InvalidInput("a time in frames needs a composition: pass composition, or "
                                   "give the time as [numerator, denominator] seconds");
            request.time = frameOrSecondsArgument(value, now.snapshot, composition);
        }
        McpUiResult result;
        if (!onOwner(core, control, [&] { result = ui.transport(request); }))
            cancelled();
        summary = "transport " + action;
        return uiResult(out, result);
    }
    case McpTool::ViewerSet: {
        members(arguments, {"display", "view", "exposure", "channel", "zoom", "gamma",
                            "resolution", "background", "transparencyGrid", "look",
                            "regionOfInterest"});
        McpViewerRequest request;
        if (auto* display = member(arguments, "display"))
            request.display = string(display, 256);
        if (auto* view = member(arguments, "view"))
            request.view = string(view, 256);
        if (auto* exposure = member(arguments, "exposure")) {
            if (!yyjson_is_num(exposure) || !std::isfinite(yyjson_get_num(exposure)) ||
                std::abs(yyjson_get_num(exposure)) > 64.0)
                throw InvalidInput("exposure is a number of stops in -64..64");
            request.exposure = yyjson_get_num(exposure);
        }
        if (auto* channel = member(arguments, "channel")) {
            const auto name = string(channel, 16);
            constexpr std::array<std::string_view, 6> channels{"rgba",  "rgb",  "red",
                                                               "green", "blue", "alpha"};
            const auto found = std::ranges::find(channels, name);
            if (found == channels.end())
                throw InvalidInput("channel is rgba, rgb, red, green, blue or alpha");
            request.channel = static_cast<McpViewerChannel>(std::distance(channels.begin(), found));
        }
        if (auto* zoom = member(arguments, "zoom")) {
            if (yyjson_is_str(zoom)) {
                const auto name = string(zoom, 16);
                if (name == "fit")
                    request.zoom = McpViewerZoom::Fit;
                else if (name == "actual-size")
                    request.zoom = McpViewerZoom::ActualSize;
                else
                    throw InvalidInput(
                        "zoom is \"fit\", \"actual-size\" or a factor (1.0 is 100%)");
            } else if (yyjson_is_num(zoom) && std::isfinite(yyjson_get_num(zoom)) &&
                       yyjson_get_num(zoom) > 0.0 && yyjson_get_num(zoom) <= 64.0) {
                request.zoom = McpViewerZoom::Factor;
                request.zoomFactor = yyjson_get_num(zoom);
            } else {
                throw InvalidInput("zoom is \"fit\", \"actual-size\" or a factor in (0, 64]");
            }
        }
        if (auto* gamma = member(arguments, "gamma")) {
            if (!yyjson_is_num(gamma) || !std::isfinite(yyjson_get_num(gamma)) ||
                yyjson_get_num(gamma) <= 0.0 || yyjson_get_num(gamma) > 10.0)
                throw InvalidInput("gamma is a number in (0, 10]; 1.0 is neutral");
            request.gamma = yyjson_get_num(gamma);
        }
        if (auto* resolution = member(arguments, "resolution")) {
            const auto name = string(resolution, 16);
            constexpr std::array<std::string_view, 4> names{"auto", "full", "half", "quarter"};
            const auto found = std::ranges::find(names, name);
            if (found == names.end())
                throw InvalidInput("resolution is auto, full, half or quarter");
            request.resolution =
                static_cast<McpViewerResolution>(std::distance(names.begin(), found));
        }
        if (auto* background = member(arguments, "background")) {
            const auto name = string(background, 16);
            constexpr std::array<std::string_view, 4> names{"solid", "gray", "black", "white"};
            const auto found = std::ranges::find(names, name);
            if (found == names.end())
                throw InvalidInput("background is solid, gray, black or white");
            request.background =
                static_cast<McpViewerBackground>(std::distance(names.begin(), found));
        }
        if (auto* grid = member(arguments, "transparencyGrid"))
            request.transparencyGrid = boolean(grid);
        if (auto* look = member(arguments, "look"))
            request.look = boolean(look);
        if (auto* region = member(arguments, "regionOfInterest")) {
            if (yyjson_is_null(region)) {
                request.regionOfInterest = std::optional<McpViewerRegion>{};
            } else {
                const auto number = [&](const char* name) {
                    auto* value = yyjson_is_obj(region) ? member(region, name) : nullptr;
                    if (value == nullptr || !yyjson_is_num(value) ||
                        !std::isfinite(yyjson_get_num(value)))
                        throw InvalidInput("regionOfInterest is {x, y, width, height} in "
                                           "composition pixels, or null to clear it");
                    return yyjson_get_num(value);
                };
                members(region, {"x", "y", "width", "height"}, {"x", "y", "width", "height"});
                const McpViewerRegion value{.x = number("x"),
                                            .y = number("y"),
                                            .width = number("width"),
                                            .height = number("height")};
                if (value.width < 1.0 || value.height < 1.0)
                    throw InvalidInput("regionOfInterest is at least 1 x 1 composition pixels");
                request.regionOfInterest = value;
            }
        }
        if (!live)
            return notLive();
        if (!ui.setViewer)
            return unbound();
        McpUiResult result;
        if (!onOwner(core, control, [&] { result = ui.setViewer(request); }))
            cancelled();
        summary = "viewer.set";
        return uiResult(out, result);
    }
    case McpTool::ExportReveal: {
        members(arguments, {"job"});
        std::optional<document::ExportJobId> job;
        if (auto* value = member(arguments, "job")) {
            const auto id = identifier(value);
            if (id == 0)
                throw InvalidInput("job must be an export job id (export.status lists them)");
            job = document::ExportJobId::fromRaw(id);
        }
        if (!live)
            return notLive();
        if (!ui.revealExport)
            return unbound();
        McpUiResult result;
        if (!onOwner(core, control, [&] { result = ui.revealExport(job); }))
            cancelled();
        summary = "export.reveal";
        return uiResult(out, result);
    }
    case McpTool::ProjectCancelOpen:
        members(arguments, {});
        return simpleCall(core, out, control, summary, tool, live, ui.cancelOpen,
                          [&] { return ui.cancelOpen(); });
    case McpTool::CachePurge: {
        members(arguments, {"cache"}, {"cache"});
        const auto cache = string(member(arguments, "cache"), 16);
        if (cache != "preview" && cache != "media")
            throw InvalidInput("cache is preview or media");
        return simpleCall(core, out, control, summary, tool, live, ui.purgeCache,
                          [&] { return ui.purgeCache(cache == "media"); });
    }
    case McpTool::JobsCancel: {
        members(arguments, {"job"}, {"job"});
        const auto job = integer(member(arguments, "job"));
        if (job == 0)
            throw InvalidInput("job must be a job id (jobs.list lists them)");
        return simpleCall(core, out, control, summary, tool, live, ui.cancelJob,
                          [&] { return ui.cancelJob(job); });
    }
    case McpTool::AssetReveal: {
        members(arguments, {"asset"}, {"asset"});
        const auto asset = integer(member(arguments, "asset"));
        if (asset == 0)
            throw InvalidInput("asset must be an asset id (query kind=assets lists them)");
        return simpleCall(core, out, control, summary, tool, live, ui.revealAsset, [&] {
            return ui.revealAsset(document::AssetId::fromRaw(asset));
        });
    }
    case McpTool::CompositionSavePreset: {
        members(arguments, {"composition", "name"}, {"composition", "name"});
        const auto composition = compositionArgument(arguments);
        const auto name = string(member(arguments, "name"), 256);
        return simpleCall(core, out, control, summary, tool, live, ui.saveCompositionPreset, [&] {
            return ui.saveCompositionPreset(document::CompositionId::fromRaw(composition), name);
        });
    }
    case McpTool::CompositionRenamePreset: {
        members(arguments, {"name", "newName"}, {"name", "newName"});
        const auto name = string(member(arguments, "name"), 256);
        const auto newName = string(member(arguments, "newName"), 256);
        return simpleCall(core, out, control, summary, tool, live, ui.renameCompositionPreset,
                          [&] { return ui.renameCompositionPreset(name, newName); });
    }
    case McpTool::CompositionDeletePreset: {
        members(arguments, {"name"}, {"name"});
        const auto name = string(member(arguments, "name"), 256);
        return simpleCall(core, out, control, summary, tool, live, ui.deleteCompositionPreset,
                          [&] { return ui.deleteCompositionPreset(name); });
    }
    case McpTool::PreferencesSet: {
        members(arguments, {"preferences"}, {"preferences"});
        auto* entries = member(arguments, "preferences");
        if (!yyjson_is_obj(entries) || yyjson_obj_size(entries) == 0 ||
            yyjson_obj_size(entries) > 64)
            throw InvalidInput("preferences is an object of 1 to 64 {key: value} entries; "
                               "preferences.get lists the keys");
        std::vector<McpPreference> request;
        yyjson_obj_iter iterator = yyjson_obj_iter_with(entries);
        while (auto* key = yyjson_obj_iter_next(&iterator)) {
            auto* value = yyjson_obj_iter_get_val(key);
            McpPreference entry{.key = string(key, 64), .value = false};
            if (yyjson_is_bool(value))
                entry.value = yyjson_get_bool(value);
            else if (yyjson_is_num(value) && std::isfinite(yyjson_get_num(value)))
                entry.value = yyjson_get_num(value);
            else if (yyjson_is_str(value))
                entry.value = string(value, 4096);
            else
                throw InvalidInput("preference " + entry.key +
                                   " takes true/false, a number or a string");
            request.push_back(std::move(entry));
        }
        return simpleCall(core, out, control, summary, tool, live, ui.setPreferences,
                          [&] { return ui.setPreferences(request); });
    }
    case McpTool::PreferencesGet: {
        members(arguments, {});
        if (!live)
            return notLive();
        if (!ui.preferences)
            return unbound();
        std::vector<McpPreference> values;
        if (!onOwner(core, control, [&] { values = ui.preferences(); }))
            cancelled();
        summary = std::string(toolName(tool));
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(true));
        auto* preferences = out.object();
        for (const auto& entry : values) {
            if (const auto* flag = std::get_if<bool>(&entry.value))
                out.set(preferences, entry.key, out.boolean(*flag));
            else if (const auto* number = std::get_if<double>(&entry.value))
                out.set(preferences, entry.key, out.real(*number));
            else
                out.set(preferences, entry.key, out.text(std::get<std::string>(entry.value)));
        }
        out.set(value, "preferences", preferences);
        return value;
    }
    case McpTool::JobsList: {
        members(arguments, {});
        if (!live)
            return notLive();
        if (!ui.jobs)
            return unbound();
        std::vector<McpJob> jobs;
        if (!onOwner(core, control, [&] { jobs = ui.jobs(); }))
            cancelled();
        summary = std::string(toolName(tool));
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(true));
        auto* list = out.array();
        for (const auto& job : jobs) {
            auto* item = out.object();
            out.set(item, "id", out.number(job.id));
            out.set(item, "name", out.text(job.name));
            out.set(item, "phase", out.text(job.phase));
            out.set(item, "state", out.text(job.state));
            out.set(item, "completed", job.completed ? out.number(*job.completed) : out.null());
            out.set(item, "total", job.total ? out.number(*job.total) : out.null());
            out.set(item, "canCancel", out.boolean(job.canCancel));
            out.set(item, "cancellationRequested", out.boolean(job.cancellationRequested));
            out.set(item, "exportJob",
                    job.exportJob ? out.text(std::to_string(job.exportJob->value())) : out.null());
            out.append(list, item);
        }
        out.set(value, "jobs", list);
        return value;
    }
    case McpTool::CompositionPresets: {
        members(arguments, {});
        if (!live)
            return notLive();
        if (!ui.compositionPresets)
            return unbound();
        std::vector<McpCompositionPreset> presets;
        if (!onOwner(core, control, [&] { presets = ui.compositionPresets(); }))
            cancelled();
        summary = std::string(toolName(tool));
        const auto time = [&](const core::RationalTime value) {
            auto* pair = out.array();
            out.append(pair, out.signedNumber(value.numerator()));
            out.append(pair, out.signedNumber(value.denominator()));
            return pair;
        };
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(true));
        auto* list = out.array();
        for (const auto& preset : presets) {
            auto* item = out.object();
            out.set(item, "name", out.text(preset.name));
            out.set(item, "builtIn", out.boolean(preset.builtIn));
            out.set(item, "width", out.number(preset.width));
            out.set(item, "height", out.number(preset.height));
            auto* aspect = out.array();
            out.append(aspect, out.number(preset.pixelAspectNumerator));
            out.append(aspect, out.number(preset.pixelAspectDenominator));
            out.set(item, "pixelAspect", aspect);
            if (preset.frameRate) {
                auto* rate = out.array();
                out.append(rate, out.number(preset.frameRate->first));
                out.append(rate, out.number(preset.frameRate->second));
                out.set(item, "frameRate", rate);
            }
            if (preset.duration)
                out.set(item, "duration", time(*preset.duration));
            if (preset.startTimecode)
                out.set(item, "startTimecode", time(*preset.startTimecode));
            if (preset.dropFrame)
                out.set(item, "dropFrame", out.boolean(*preset.dropFrame));
            if (preset.background) {
                auto* colour = out.array();
                for (const auto channel : *preset.background)
                    out.append(colour, out.real(channel));
                out.set(item, "background", colour);
            }
            out.append(list, item);
        }
        out.set(value, "presets", list);
        return value;
    }
    default:
        throw InvalidInput("Unknown tool");
    }
}

} // namespace bloom::mcp
