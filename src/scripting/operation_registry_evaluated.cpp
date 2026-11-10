// MCP parity (#389): the three project edits that need a heavy read when they are created -- Reload
// Footage (a media probe), the Properties anchor grid (the layer's evaluated local bounds) and the
// Viewer's Cryptomatte eyedropper (a pixel ID read-back). Like align and distribute (ADR 0031),
// the factory reads the project source the transaction is created against on the client thread
// (the script, MCP or CLI thread; never the UI thread), so the command it builds stays read-free
// and applies on the owner thread like any panel edit. Each read runs as one task on a private
// scheduler and stops when the caller stops waiting (OperationContext::stop).

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/path_write_operations.hpp>
#include <bloom/core/file_uri.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/cryptomatte_inspection.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/node_path_query.hpp>
#include <bloom/scripting/readings.hpp>
#include <bloom/scripting/render.hpp>

#include "operation_registry_parity.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::scripting::detail::parity {
namespace {

using document::AssetId;
using document::CompositionId;
using document::LayerId;
using document::NodeId;

const std::string kReload =
    R"(Reads an asset's own file again, as Reload Footage in the Assets panel does: the media is )"
    R"(probed again the way the asset was imported (a still stays a still, a sequence keeps its )"
    R"(frame range) and the asset takes the file's current size, duration and digest. The probe )"
    R"(runs when the operation is created, off the UI thread, and stops when the request is )"
    R"(cancelled. Reload several assets by giving one operation per asset. )"
    R"(Outputs: none. )"
    R"(Errors: "Asset N does not exist"; "The asset has no file to read" - a data asset or an empty or )"
    R"(unusable locator; the probe's own reason when the file is missing or unreadable )"
    R"((bloom.asset.relink then points the asset at another file); bloom.scripting.cancelled. )"
    R"(Undo: one step restores the asset as it was. )"
    R"(MCP example: {"op":"bloom.asset.reload","args":{"asset":3}}.)";

const std::string kAnchorPreset =
    R"(Moves a layer's anchor point to one of the nine points of its local bounds, as the anchor )"
    R"(grid in Properties > Transform does: the bounds are the layer's evaluated content at )"
    R"(`time` (measured on the CPU reference path when the operation is created, off the UI )"
    R"(thread), and the anchor becomes half the bounds' width and height times the point's )"
    R"(column and row offset (-1, 0 or 1). A constant anchor is replaced; an animated anchor gets )"
    R"(a key at the layer's own key time for `time`. Position is not changed, so the layer moves )"
    R"(on screen exactly as it does when the artist clicks the grid. )"
    R"(Outputs: none. )"
    R"(Errors: "anchor expects ..." - not one of the nine names; "Layer N does not exist"; "The )"
    R"(layer is locked"; "The layer has no anchor point"; "The anchor is driven by a node" - )"
    R"(disconnect it first; "The layer has no visible bounds at this time" - it is outside its )"
    R"(range, disabled or empty; bloom.scripting.layer-bounds-unavailable when the composition )"
    R"(cannot be evaluated; bloom.scripting.cancelled. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.layer.set-anchor-preset","args":{"layer":4,"anchor":"center"}}.)";

const std::string kCryptomattePick =
    R"(Adds the object under a composition point to a Cryptomatte node's matte list, or removes )"
    R"(it, as clicking (or Alt-clicking) with the Viewer's Cryptomatte eyedropper does. `point` )"
    R"(is in composition pixels at composition resolution (what capture shows): it is mapped into )"
    R"(the Cryptomatte's channel set through the evaluated transform of the layer that shows the )"
    R"(node, and the object with the largest coverage at that pixel of the node's selected )"
    R"(Cryptomatte layer is read at `time` (through that layer's own time). The pixel ID read-back )"
    R"(runs when the operation is created, off the UI thread, and stops when the request is )"
    R"(cancelled. The matte is stored by its manifest name, or as its raw ID when the manifest )"
    R"(does not name it; mode "add" leaves a matte already listed alone and "remove" leaves an )"
    R"(unlisted one alone. To choose mattes by name instead, set the node's "mattes" text with )"
    R"(bloom.node.set-parameter-value. )"
    R"(Outputs: none; query the node's "mattes" parameter for the list. )"
    R"(Errors: "Node N is not a Cryptomatte node"; "The Cryptomatte is not shown by a layer" - )"
    R"(connect it to a layer first; "No object under the point" - the point is outside the )"
    R"(layer's content or no object covers it; the read's own reason (a missing channel set, no )"
    R"(Cryptomatte layer); bloom.scripting.cancelled. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.cryptomatte.pick","args":{"node":7,"point":[960,540]}}.)";

const std::string kWriteToPath =
    R"(Writes what a Path-producing value node (Resample Path, Trim Path, Transform Path, Offset Path, )"
    R"(Path Stroke, a Path node, and the others that answer a Path) gives at `time` into a Path node )"
    R"((bloom.value-path) as stored points, so the points can be moved and keyed by hand. The node is )"
    R"(evaluated when the operation is created, off the UI thread, and stops when the request is )"
    R"(cancelled. With no `target` a new Path node is added beside the source and wired to nothing; )"
    R"(the source's links never change. With a `target` Path node, a constant value is replaced and )"
    R"(an animated one gets a whole-path key at `time`. `port` names the Path output; the default is )"
    R"(the node's first. Keys stay whole-path: all points of a path share their key times. )"
    R"(Outputs: node (the Path node written). )"
    R"(Errors: "Node N does not exist"; "Node N has no Path output"; the node's own diagnostic when it )"
    R"(fails at that time (a count past the Path caps, a bad operand); "Node N is not a Path node"; )"
    R"("The Path node's value is driven by another node; disconnect it first"; bloom.scripting.cancelled. )"
    R"(Undo: one step removes the new node, or restores the target's value. )"
    R"(MCP example: {"op":"bloom.node.write-to-path","args":{"node":12,"time":0}}.)";

[[nodiscard]] OperationCreateResult refuse(const std::string& op, std::string code,
                                           std::string argument, std::string message) {
    return OperationCreateResult(nullptr, OperationDiagnostic{.code = std::move(code),
                                                              .operationId = op,
                                                              .argument = std::move(argument),
                                                              .message = std::move(message)});
}

[[nodiscard]] OperationCreateResult invalid(const std::string& op, std::string argument,
                                            std::string message) {
    return refuse(op, "bloom.scripting.invalid-argument", std::move(argument), std::move(message));
}

[[nodiscard]] OperationCreateResult cancelledRead(const std::string& op) {
    return refuse(op, "bloom.scripting.cancelled", {}, "The request was cancelled; nothing changed");
}

[[nodiscard]] OperationCreateResult noSource(const std::string& op) {
    return refuse(op, "bloom.scripting.source-unavailable", {},
                  "This client cannot read the project's media or evaluate it; run the operation "
                  "through a session (MCP, the Script editor or bloom-cli)");
}

// What a cancellable read returned: the value, or that it was cancelled or could not start.
template <typename Value> struct ReadOutcome final {
    std::shared_ptr<Value> value;
    bool cancelled = false;
};

// Runs `work` as one task on a private one-worker scheduler, so the read gets a real runtime
// cancellation token, and waits for it on the calling client thread. `stop` cancels the task. The
// scheduler is retired only once quiescent, so no worker outlives what it borrowed.
template <typename Value, typename Work>
ReadOutcome<Value> runCancellable(const std::stop_token& stop, std::string name,
                                  const runtime::TaskExecutor executor, Work work) {
    if (stop.stop_requested())
        return {.value = nullptr, .cancelled = true};
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    auto submission = scheduler.submit<std::shared_ptr<Value>>(
        runtime::TaskRequest(std::move(name),
                             {.kind = runtime::TaskOwnerKind::Application,
                              .id = runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Foreground, executor),
        [&work](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<Value>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto value = std::make_shared<Value>(work(context.cancellation()));
            if (context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(std::move(value));
        });
    ReadOutcome<Value> outcome;
    if (submission.accepted()) {
        const auto handle = submission.handle;
        const std::stop_callback onStop(stop, [handle] { handle.cancel(); });
        for (;;) {
            if (auto result = handle.tryTakeResult()) {
                if (result->state() == runtime::TaskState::Succeeded && result->value())
                    outcome.value = *result->value();
                outcome.cancelled = result->state() == runtime::TaskState::Cancelled;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (stop.stop_requested())
        outcome.cancelled = true;
    return outcome;
}

// The asset's own file, as Reload Footage and Reveal in File Manager find it: an external locator
// stores its absolute file URI; a project-relative one is lexical below the asset base.
[[nodiscard]] std::filesystem::path assetSourcePath(const document::AssetRecord& asset,
                                                    const std::filesystem::path& base) {
    const auto& path = asset.locator.path;
    if (path.empty())
        return {};
    if (path.find(':') != std::string::npos)
        return core::absolutePathFromFileUri(path).value_or(std::filesystem::path{});
    if (path.front() == '/' || path.find('\\') != std::string::npos)
        return {};
    return base /
           std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()),
                                               path.size()));
}

OperationCreateResult reload(const std::string& op, const Arguments& args,
                             const OperationContext& context) {
    Reader read(op, args);
    const auto assetId = read.id<AssetId>("asset");
    if (read.failed())
        return read.error();
    if (!context.source)
        return noSource(op);
    const auto source = context.source();
    if (!source)
        return noSource(op);
    const auto* asset = source->snapshot.project().findAsset(assetId);
    if (asset == nullptr)
        return invalid(op, "asset", "Asset " + std::to_string(assetId.value()) + " does not exist");
    const auto path = assetSourcePath(*asset, source->assetBaseDirectory);
    if (path.empty())
        return invalid(op, "asset", "The asset has no file to read");
    const auto options = commands::relinkOptions(*asset);
    auto outcome = runCancellable<std::unique_ptr<commands::Operation>>(
        context.stop, "Reload Footage", runtime::TaskExecutor::BlockingIo,
        [&](const runtime::CancellationToken& cancellation) -> std::unique_ptr<commands::Operation> {
            return std::make_unique<commands::RelinkAsset>(
                assetId, path, source->assetBaseDirectory,
                [&cancellation] { return cancellation.isCancellationRequested(); }, options);
        });
    if (outcome.cancelled)
        return cancelledRead(op);
    if (!outcome.value || !*outcome.value)
        return refuse(op, "bloom.scripting.read-failed", "asset", "The file could not be read");
    return made(std::move(*outcome.value));
}

// The nine anchor grid points in reading order: index % 3 is the column, index / 3 the row.
constexpr std::array<std::string_view, 9> kAnchorPoints{
    "top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right"};

[[nodiscard]] std::string anchorChoices() {
    std::string names;
    for (const auto name : kAnchorPoints)
        names += (names.empty() ? "\"" : ", \"") + std::string(name) + "\"";
    return names;
}

// The layer's Layer Output bounds at `time` on the CPU reference path.
ReadOutcome<runtime::LayerBoundsQueryResult>
measureLayer(const OperationContext& context, const SessionRenderSourceSnapshot& source,
             const CompositionId composition, const core::RationalTime time, const LayerId layer,
             const runtime::EvaluationColorIntent& colorIntent) {
    return runCancellable<runtime::LayerBoundsQueryResult>(
        context.stop, "Measure Layer Bounds", runtime::TaskExecutor::Cpu,
        [&](const runtime::CancellationToken& cancellation) {
            const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
            const runtime::CpuCompositionEvaluator evaluator;
            evaluator.setAssetBaseDirectory(source.assetBaseDirectory);
            return runtime::queryLayerBounds(compiler, evaluator,
                                             {.snapshot = source.snapshot,
                                              .composition = composition,
                                              .time = time,
                                              .layers = {layer},
                                              .colorIntent = colorIntent,
                                              .pixelStorageByteLimit =
                                                  runtime::kDefaultLayerBoundsPixelBudget},
                                             cancellation);
        });
}

[[nodiscard]] const document::ParameterBinding* roleOf(const document::NodeRecord& node,
                                                      const std::string_view role) {
    const auto found = std::ranges::find(node.parameters, role, &document::ParameterBinding::role);
    return found == node.parameters.end() ? nullptr : &*found;
}

OperationCreateResult anchorPreset(const std::string& op, const Arguments& args,
                                   const OperationContext& context) {
    Reader read(op, args);
    const auto compositionId = read.id<CompositionId>("composition");
    const auto layerId = read.id<LayerId>("layer");
    const auto time = read.time("time");
    const auto anchor = read.text("anchor");
    if (read.failed())
        return read.error();
    const auto point = anchor ? std::ranges::find(kAnchorPoints, *anchor) : kAnchorPoints.end();
    if (point == kAnchorPoints.end())
        return invalid(op, "anchor", "expects one of " + anchorChoices());
    if (!context.source)
        return noSource(op);
    const auto source = context.source();
    if (!source)
        return noSource(op);
    const auto* composition = source->snapshot.project().findComposition(compositionId);
    if (composition == nullptr)
        return invalid(op, "composition",
                       "Composition " + std::to_string(compositionId.value()) + " does not exist");
    const auto* layer = composition->graph().findLayer(layerId);
    if (layer == nullptr)
        return invalid(op, "layer", "Layer " + std::to_string(layerId.value()) + " does not exist");
    if (layer->locked)
        return invalid(op, "layer", "The layer is locked");
    const auto* node = composition->graph().findNode(layer->nodeId);
    const auto* binding = node ? roleOf(*node, document::kAnchorParameterRole) : nullptr;
    const auto* parameter = binding ? composition->parameters().find(binding->parameterId) : nullptr;
    if (parameter == nullptr)
        return invalid(op, "layer", "The layer has no anchor point");
    const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
    const auto* animated = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (constant == nullptr && animated == nullptr)
        return invalid(op, "layer", "The anchor is driven by a node; disconnect it first");
    if (constant != nullptr && !std::holds_alternative<document::Vec2d>(constant->value))
        return invalid(op, "layer", "The layer has no anchor point");
    const auto colorIntent = captureRenderColorIntent(*source, *composition);
    if (!colorIntent)
        return refuse(op, "bloom.scripting.layer-bounds-unavailable", {},
                      "could not measure the layer: the project color settings are unavailable");

    const auto measured =
        measureLayer(context, *source, compositionId, *time, layerId, *colorIntent);
    if (measured.cancelled)
        return cancelledRead(op);
    if (!measured.value || !measured.value->succeeded())
        return refuse(op, "bloom.scripting.layer-bounds-unavailable", {},
                      "could not measure the layer: " +
                          (measured.value ? measured.value->diagnostic : std::string("no result")));
    const auto found = std::ranges::find(measured.value->bounds, layerId,
                                         &runtime::EvaluatedOperationBounds::layerId);
    if (found == measured.value->bounds.end() || found->local.empty())
        return invalid(op, "time", "The layer has no visible bounds at this time");

    // Exactly the anchor grid's arithmetic (PropertiesAnchorGrid::choosePoint).
    const auto index = static_cast<int>(std::distance(kAnchorPoints.begin(), point));
    const auto& local = found->local;
    const document::Vec2d value{(index % 3 - 1) * (local.right - local.left) / 2.0,
                                static_cast<double>(index / 3 - 1) * (local.bottom - local.top) /
                                    2.0};
    if (constant != nullptr)
        return made(std::make_unique<commands::SetParameterSource>(
            compositionId, parameter->id, document::ConstantValueSource{value}));
    const auto keyTime = commands::layerKeyTimeOfCompositionTime(layer->timing, *time);
    if (!keyTime)
        return invalid(op, "time", "The time does not fit the layer's timing");
    return made(std::make_unique<commands::SetKeyframeAtTime>(compositionId, animated->curveId,
                                                              *keyTime, value));
}

// The one layer whose Layer Output the node feeds (CompositionSession::layerForNode): nullopt when
// none does, or when it reaches two different layers.
[[nodiscard]] std::optional<LayerId> layerShowing(const document::Composition& composition,
                                                  const NodeId node) {
    std::vector<NodeId> pending{node};
    std::vector<NodeId> visited;
    std::optional<LayerId> resolved;
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (std::ranges::find(visited, current) != visited.end())
            continue;
        visited.push_back(current);
        for (const auto& boundary : composition.graph().layerOutputs()) {
            if (boundary.entryOwned() || boundary.nodeId != current)
                continue;
            if (resolved && *resolved != boundary.layerId)
                return std::nullopt;
            resolved = boundary.layerId;
        }
        for (const auto& edge : composition.graph().edges())
            if (edge.source.nodeId == current)
                if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination))
                    pending.push_back(input->nodeId);
    }
    return resolved;
}

// A composition point in the layer's content (local) frame, through the inverse of the evaluated
// content-to-composition affine the polygon carries (the Viewer's masks::contentToComposition).
[[nodiscard]] std::optional<document::Vec2d>
compositionToContent(const runtime::EvaluatedOperationBounds& bounds, const document::Vec2d point) {
    const auto& local = bounds.local;
    if (local.empty())
        return std::nullopt;
    const auto& polygon = bounds.polygon;
    const double ax = (polygon[1].x - polygon[0].x) / (local.right - local.left);
    const double ay = (polygon[1].y - polygon[0].y) / (local.right - local.left);
    const double bx = (polygon[3].x - polygon[0].x) / (local.bottom - local.top);
    const double by = (polygon[3].y - polygon[0].y) / (local.bottom - local.top);
    const double ox = polygon[0].x - ax * local.left - bx * local.top;
    const double oy = polygon[0].y - ay * local.left - by * local.top;
    const double determinant = ax * by - bx * ay;
    if (!std::isfinite(determinant) || determinant == 0.0 || !std::isfinite(ox) ||
        !std::isfinite(oy))
        return std::nullopt;
    const double dx = point.x - ox;
    const double dy = point.y - oy;
    const document::Vec2d result{(by * dx - bx * dy) / determinant,
                                 (-ay * dx + ax * dy) / determinant};
    if (!std::isfinite(result.x) || !std::isfinite(result.y))
        return std::nullopt;
    return result;
}

OperationCreateResult cryptomattePick(const std::string& op, const Arguments& args,
                                      const OperationContext& context) {
    Reader read(op, args);
    const auto compositionId = read.id<CompositionId>("composition");
    const auto nodeId = read.id<NodeId>("node");
    const auto time = read.time("time");
    const auto point = read.vec2("point");
    const auto mode = read.text("mode");
    if (read.failed())
        return read.error();
    if (!point)
        return invalid(op, "point", "is required");
    if (mode && *mode != "add" && *mode != "remove")
        return invalid(op, "mode", R"(expects "add" or "remove")");
    const bool remove = mode && *mode == "remove";
    if (!context.source)
        return noSource(op);
    const auto source = context.source();
    if (!source)
        return noSource(op);
    const auto* composition = source->snapshot.project().findComposition(compositionId);
    if (composition == nullptr)
        return invalid(op, "composition",
                       "Composition " + std::to_string(compositionId.value()) + " does not exist");
    const auto* node = composition->graph().findNode(nodeId);
    if (node == nullptr || node->typeId != document::kCryptomatteNodeType)
        return invalid(op, "node",
                       "Node " + std::to_string(nodeId.value()) + " is not a Cryptomatte node");
    const auto layer = layerShowing(*composition, nodeId);
    if (!layer)
        return invalid(op, "node", "The Cryptomatte is not shown by a layer; connect it to one");
    const auto colorIntent = captureRenderColorIntent(*source, *composition);
    if (!colorIntent)
        return refuse(op, "bloom.scripting.read-failed", {},
                      "The project color settings are unavailable");

    const auto measured = measureLayer(context, *source, compositionId, *time, *layer, *colorIntent);
    if (measured.cancelled)
        return cancelledRead(op);
    if (!measured.value || !measured.value->succeeded())
        return refuse(op, "bloom.scripting.layer-bounds-unavailable", {},
                      "could not measure the layer: " +
                          (measured.value ? measured.value->diagnostic : std::string("no result")));
    const auto found = std::ranges::find(measured.value->bounds, *layer,
                                         &runtime::EvaluatedOperationBounds::layerId);
    const auto content = found != measured.value->bounds.end()
                             ? compositionToContent(*found, *point)
                             : std::nullopt;
    if (!content || !(content->x >= found->local.left && content->x < found->local.right &&
                      content->y >= found->local.top && content->y < found->local.bottom))
        return invalid(op, "point", "No object under the point");
    const auto x = static_cast<std::int64_t>(std::floor(content->x));
    const auto y = static_cast<std::int64_t>(std::floor(content->y));

    const auto picked = runCancellable<runtime::CryptomattePick>(
        context.stop, "Pick Cryptomatte", runtime::TaskExecutor::Cpu,
        [&](const runtime::CancellationToken& cancellation) {
            const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
            return runtime::pickCryptomatte(compiler, source->snapshot, compositionId, nodeId,
                                            source->assetBaseDirectory, *time, *colorIntent,
                                            std::nullopt, x, y, cancellation);
        });
    if (picked.cancelled)
        return cancelledRead(op);
    if (!picked.value)
        return refuse(op, "bloom.scripting.read-failed", {}, "The Cryptomatte could not be read");
    if (!picked.value->diagnostic.empty())
        return refuse(op, "bloom.scripting.read-failed", {}, picked.value->diagnostic);
    if (!picked.value->matte)
        return invalid(op, "point", "No object under the point");

    // The node's matte list as the transaction is created against it.
    std::vector<std::string> mattes;
    if (const auto* binding = roleOf(*node, document::kCryptomatteMattesParameterRole))
        if (const auto* parameter = composition->parameters().find(binding->parameterId))
            if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source))
                if (const auto* text = std::get_if<std::string>(&constant->value))
                    mattes = runtime::splitCryptomatteMatteList(*text);
    const auto listed = std::ranges::find(mattes, *picked.value->matte);
    if (!remove && listed == mattes.end())
        mattes.push_back(*picked.value->matte);
    else if (remove && listed != mattes.end())
        mattes.erase(listed);
    return made(std::make_unique<commands::SetNodeParameterValue>(
        compositionId, nodeId, std::string(document::kCryptomatteMattesParameterRole),
        document::ParameterValue{runtime::joinCryptomatteMatteList(mattes)}));
}

OperationCreateResult writeToPath(const std::string& op, const Arguments& args,
                                  const OperationContext& context) {
    Reader read(op, args);
    const auto compositionId = read.id<CompositionId>("composition");
    const auto nodeId = read.id<NodeId>("node");
    const auto target = read.id<NodeId>("target", false);
    const auto port = read.text("port");
    const auto time = read.time("time");
    if (read.failed())
        return read.error();
    if (!context.source)
        return noSource(op);
    const auto source = context.source();
    if (!source)
        return noSource(op);
    const auto* composition = source->snapshot.project().findComposition(compositionId);
    if (composition == nullptr)
        return invalid(op, "composition",
                       "Composition " + std::to_string(compositionId.value()) + " does not exist");
    const auto* node = composition->graph().findNode(nodeId);
    if (node == nullptr)
        return invalid(op, "node", "Node " + std::to_string(nodeId.value()) + " does not exist");
    if (target.isValid()) {
        const auto* existing = composition->graph().findNode(target);
        if (existing == nullptr || existing->typeId != document::kPathValueNodeType)
            return invalid(op, "target",
                           "Node " + std::to_string(target.value()) + " is not a Path node");
    }
    const auto queried = runCancellable<NodePathQuery>(
        context.stop, "Write To Path", runtime::TaskExecutor::Cpu,
        [&](const runtime::CancellationToken& cancellation) {
            return queryNodePath(source->snapshot, compositionId, nodeId, port.value_or(""), *time,
                                 cancellation);
        });
    if (queried.cancelled || (queried.value && queried.value->cancelled))
        return cancelledRead(op);
    if (!queried.value)
        return refuse(op, "bloom.scripting.read-failed", {}, "The node could not be read");
    if (!queried.value->path)
        return invalid(op, "node", queried.value->diagnostic);
    // Beside the source, clear of it; AddNode moves the card further when something is in the way.
    document::Vec2d position{0.0, 0.0};
    if (const auto found = composition->nodeLayout().find(nodeId);
        found != composition->nodeLayout().end())
        position = {found->second.position.x + found->second.width + 64.0, found->second.position.y};
    return made(std::make_unique<commands::WriteValueToPath>(
        compositionId, target.isValid() ? std::optional<NodeId>(target) : std::nullopt,
        std::move(*queried.value->path), position, *time));
}

void addContextual(std::vector<OperationDescriptor>& descriptors, std::string id,
                   std::string description, std::vector<ArgumentSchema> arguments,
                   OperationCreateResult (*create)(const std::string&, const Arguments&,
                                                   const OperationContext&)) {
    descriptors.push_back(
        {.schema = {.typeId = std::move(id),
                    .arguments = std::move(arguments),
                    .description = std::move(description)},
         .factory = [create](const std::string& op,
                             const Arguments& args) { return create(op, args, {}); },
         .contextualFactory = create});
}

[[nodiscard]] ArgumentSchema timeFromContext(std::string summary) {
    return fromContext("time", ValueKind::Time, ContextSource::Time, std::int64_t{0},
                       std::move(summary));
}

} // namespace

void addEvaluatedOperations(std::vector<OperationDescriptor>& descriptors) {
    addContextual(descriptors, "bloom.asset.reload", kReload,
                  {need("asset", ValueKind::Id, StableId{1}, "Id. The asset to read again.")},
                  reload);
    addContextual(
        descriptors, "bloom.layer.set-anchor-preset", kAnchorPreset,
        {composition(),
         fromContext("layer", ValueKind::Id, ContextSource::Layer, StableId{1},
                     "Id. The layer whose anchor moves. Defaults to the selected layer."),
         need("anchor", ValueKind::String, "center",
              R"(The point of the local bounds: "top-left", "top", "top-right", "left", )"
              R"("center", "right", "bottom-left", "bottom" or "bottom-right".)"),
         timeFromContext("The time whose evaluated bounds are used, and where an animated anchor "
                         "is keyed. Defaults to the current playhead time.")},
        anchorPreset);
    addContextual(
        descriptors, "bloom.cryptomatte.pick", kCryptomattePick,
        {composition(), need("node", ValueKind::Id, StableId{1}, "Id. The Cryptomatte node."),
         need("point", ValueKind::Vec2, document::Vec2d{960.0, 540.0},
              "[x, y] in composition pixels, as capture shows the frame."),
         maybe("mode", ValueKind::String, "add",
               R"("add" (the default) adds the object to the matte list; "remove" removes it, )"
               R"(as Alt-click does.)"),
         timeFromContext("The time whose frame is read. Defaults to the current playhead time.")},
        cryptomattePick);
    addContextual(
        descriptors, "bloom.node.write-to-path", kWriteToPath,
        {composition(),
         need("node", ValueKind::Id, StableId{1},
              "Id. The node whose Path result is written (Resample Path and the other Path "
              "producers)."),
         maybe("port", ValueKind::String, "",
               "The Path output to write. Defaults to the node's first Path output."),
         maybe("target", ValueKind::Id, StableId{1},
               "Id. An existing Path node to write into. Defaults to a new Path node beside the "
               "source."),
         timeFromContext("The time whose result is written (and where an animated target is "
                         "keyed). Defaults to the current playhead time.")},
        writeToPath);
}

[[nodiscard]] static LayerBoundsReading measureLayerBoundsReading(const OperationContext& context,
                                             const CompositionId compositionId,
                                             const core::RationalTime time, const LayerId layerId) {
    LayerBoundsReading reading;
    const auto unavailable = [&reading](std::string why) {
        reading.status = LayerBoundsStatus::Unavailable;
        reading.diagnostic = std::move(why);
        return reading;
    };
    if (!context.source)
        return unavailable("This client cannot read the project to evaluate it");
    const auto source = context.source();
    if (!source)
        return unavailable("The project is closed or being replaced");
    const auto* composition = source->snapshot.project().findComposition(compositionId);
    if (composition == nullptr)
        return unavailable("Composition " + std::to_string(compositionId.value()) +
                           " does not exist; query kind=compositions lists them");
    if (composition->graph().findLayer(layerId) == nullptr)
        return unavailable("Layer " + std::to_string(layerId.value()) +
                           " does not exist in composition " +
                           std::to_string(compositionId.value()) +
                           "; query kind=layers lists its layers");
    const auto colorIntent = captureRenderColorIntent(*source, *composition);
    if (!colorIntent)
        return unavailable("The project color settings are unavailable");
    const auto measured =
        measureLayer(context, *source, compositionId, time, layerId, *colorIntent);
    if (measured.cancelled) {
        reading.status = LayerBoundsStatus::Cancelled;
        reading.diagnostic = "The request was cancelled";
        return reading;
    }
    if (!measured.value || !measured.value->succeeded())
        return unavailable(measured.value ? measured.value->diagnostic : "No result");
    const auto found = std::ranges::find(measured.value->bounds, layerId,
                                         &runtime::EvaluatedOperationBounds::layerId);
    if (found == measured.value->bounds.end() || found->local.empty()) {
        reading.status = LayerBoundsStatus::NotVisible;
        reading.diagnostic = "The layer has no visible bounds at this time: it is outside its "
                             "range, disabled or empty";
        return reading;
    }
    reading.status = LayerBoundsStatus::Measured;
    reading.localLeft = found->local.left;
    reading.localTop = found->local.top;
    reading.localRight = found->local.right;
    reading.localBottom = found->local.bottom;
    for (std::size_t index = 0; index < 4; ++index)
        reading.corners[index] = {found->polygon[index].x, found->polygon[index].y};
    reading.boundsLeft = found->output.left;
    reading.boundsTop = found->output.top;
    reading.boundsRight = found->output.right;
    reading.boundsBottom = found->output.bottom;
    reading.anchorX = found->anchor.x;
    reading.anchorY = found->anchor.y;
    return reading;
}

} // namespace bloom::scripting::detail::parity

namespace bloom::scripting {

LayerBoundsReading measureLayerBounds(const OperationContext& context,
                                      const document::CompositionId composition,
                                      const core::RationalTime time, const document::LayerId layer) {
    return detail::parity::measureLayerBoundsReading(context, composition, time, layer);
}

} // namespace bloom::scripting
