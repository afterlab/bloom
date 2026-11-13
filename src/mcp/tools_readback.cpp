// Read-back query kinds (layers, keyframes, node-types) and the richer records of compositions,
// assets, nodes and parameters. Values come back in the units they are written in: times as
// {frames, seconds:[n, d]}, colours and positions as the operations take them, enumerations as
// names or stored integers with labels (scripting/node_reference.hpp owns the vocabulary).
#include "core_internal.hpp"

#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/scripting/merge_entry_view.hpp>
#include <bloom/scripting/node_reference.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace bloom::mcp {
namespace {

constexpr std::size_t kPageSize = 64;

yyjson_mut_val* timeValue(Json& out, const core::RationalTime time,
                          const document::FrameRate& rate) {
    auto* value = out.object();
    const auto count = scripting::framesAt(time, rate);
    out.set(value, "frames",
            count.exact ? out.signedNumber(count.frames) : out.real(count.approximate));
    auto* seconds = out.array();
    out.append(seconds, out.signedNumber(time.numerator()));
    out.append(seconds, out.signedNumber(time.denominator()));
    out.set(value, "seconds", seconds);
    return value;
}

yyjson_mut_val* pair(Json& out, const std::int64_t first, const std::int64_t second) {
    auto* value = out.array();
    out.append(value, out.signedNumber(first));
    out.append(value, out.signedNumber(second));
    return value;
}

yyjson_mut_val* roles(Json& out, const document::NodeRecord* node) {
    auto* value = out.object();
    if (node != nullptr)
        for (const auto& binding : node->parameters)
            out.set(value, binding.role, out.number(binding.parameterId.value()));
    return value;
}

yyjson_mut_val* parsedJson(Json& out, const std::string& text) {
    Parsed parsed(text);
    return out.copy(parsed.root());
}

// #502: a layer's parameters by role (its Layer node's, or an entry layer's own).
yyjson_mut_val* layerRoles(Json& out, const document::CanonicalGraph& graph,
                           const document::LayerOutputBoundary& layer) {
    auto* value = out.object();
    for (const auto& binding : document::layerParameterBindings(graph, layer))
        out.set(value, binding.role, out.number(binding.parameterId.value()));
    return value;
}

// The layer controls' constant values by role (null for an animated or driven one), or the
// defaults an entry without a layer renders with.
yyjson_mut_val* roleValues(Json& out, const std::vector<scripting::LayerRoleValue>& roles) {
    auto* value = out.object();
    for (const auto& role : roles)
        out.set(value, role.role,
                role.value ? parsedJson(out, scripting::parameterValueJson(*role.value))
                           : out.null());
    return value;
}

yyjson_mut_val* roleIds(Json& out, const std::vector<scripting::LayerRoleValue>& roles) {
    auto* value = out.object();
    for (const auto& role : roles)
        if (role.parameter)
            out.set(value, role.role, out.number(role.parameter->value()));
    return value;
}

yyjson_mut_val* optionalId(Json& out, const std::optional<document::NodeId>& id) {
    return id ? out.number(id->value()) : out.null();
}

// #502: the fields every Merge entry reports, in the `layers` query's `entries` and the `nodes`
// query's Merge `stack`. An entry without a layer reports the defaults it renders with.
void appendMergeEntryFields(Json& out, yyjson_mut_val* row, const scripting::MergeEntryView& view) {
    const auto* layer = view.layer;
    out.set(row, "resolvedLayer", layer ? out.number(layer->layerId.value()) : out.null());
    out.set(row, "layerKind", out.text(scripting::entryLayerKindName(view.kind)));
    out.set(row, "entryLayer", out.boolean(view.kind == scripting::EntryLayerKind::Entry));
    out.set(row, "layerNode", optionalId(out, view.layerNode));
    out.set(row, "sourceNode", optionalId(out, view.sourceNode));
    out.set(row, "sourceType", view.sourceNode ? out.text(view.sourceType) : out.null());
    out.set(row, "name", layer ? out.text(layer->name) : out.null());
    out.set(row, "enabled", out.boolean(layer == nullptr || layer->enabled));
    out.set(row, "solo", out.boolean(layer != nullptr && layer->solo));
    out.set(row, "locked", out.boolean(layer != nullptr && layer->locked));
    out.set(row, "shy", out.boolean(layer != nullptr && layer->shy));
    out.set(row, "pivot", out.text(scripting::layerPivotName(view.pivot)));
    out.set(row, "adjustment", optionalId(out, view.adjustment));
    out.set(row, "shared", out.boolean(view.shared));
    out.set(row, "nestedMerge", optionalId(out, view.nestedMerge));
    out.set(row, "parameters", roleIds(out, view.roles));
    out.set(row, "values", roleValues(out, view.roles));
}

std::string_view timeModeName(const document::LayerTimeMode mode) {
    constexpr std::array<std::string_view, 4> names{"none", "hold", "loop", "ping-pong"};
    const auto index = static_cast<std::size_t>(mode);
    return index < names.size() ? names[index] : "none";
}

const document::Composition& compositionArgument(const document::Snapshot& snapshot,
                                                 yyjson_val* arguments) {
    if (!member(arguments, "composition"))
        throw InvalidInput("This query kind needs composition (query {kind:\"compositions\"} "
                           "lists the ids)");
    const auto id = integer(member(arguments, "composition"));
    const auto* composition =
        snapshot.project().findComposition(document::CompositionId::fromRaw(id));
    if (!composition)
        throw InvalidInput("Stale composition ID " + std::to_string(id) + " at revision " +
                           std::to_string(snapshot.revision().value()) +
                           "; query {kind:\"compositions\"} lists the current ids");
    return *composition;
}

std::string pageCursor(const std::uint64_t session, const std::uint64_t project,
                       const std::uint64_t composition, const std::uint64_t revision,
                       const std::uint64_t curve, const std::size_t offset) {
    return "k1:" + std::to_string(session) + ':' + std::to_string(project) + ':' +
           std::to_string(composition) + ':' + std::to_string(revision) + ':' +
           std::to_string(curve) + ':' + std::to_string(offset);
}

std::size_t parsePageCursor(std::string_view cursor, const std::array<std::uint64_t, 5>& expected,
                            const std::size_t count) {
    if (!cursor.starts_with("k1:"))
        throw InvalidInput("Malformed keyframe cursor; pass the nextCursor of the previous page");
    cursor.remove_prefix(3);
    std::array<std::uint64_t, 6> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto separator = cursor.find(':');
        const auto part = cursor.substr(0, separator);
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), values[index]);
        if (part.empty() || parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
            (index + 1 == values.size()) != (separator == std::string_view::npos))
            throw InvalidInput("Malformed keyframe cursor; pass the nextCursor of the previous "
                               "page");
        if (separator != std::string_view::npos)
            cursor.remove_prefix(separator + 1);
    }
    if (!std::equal(expected.begin(), expected.end(), values.begin()))
        throw InvalidInput("Stale keyframe cursor: the project changed; start again without a "
                           "cursor");
    if (values[5] >= count)
        throw InvalidInput("Keyframe cursor is out of range");
    return static_cast<std::size_t>(values[5]);
}

// ---- layers

yyjson_mut_val* layersQuery(Json& out, yyjson_mut_val* result, const document::Project& project,
                            const document::Composition& comp,
                            const std::optional<document::LayerId> only) {
    const auto& graph = comp.graph();
    const auto rate = comp.format().frameRate();
    out.set(result, "composition", out.number(comp.id().value()));
    struct Placement final {
        const document::LayerOutputBoundary* layer;
        std::optional<document::NodeId> merge;
        std::optional<document::LayerSlotId> slot;
        std::size_t index = 0;
        std::size_t mergeOrder = 0;
    };
    std::vector<Placement> placements;
    const auto output = graph.outputMergeId();
    for (const auto& boundary : graph.layerOutputs()) {
        if (only && boundary.layerId != *only)
            continue;
        Placement placement{.layer = &boundary,
                            .merge = {},
                            .slot = {},
                            .index = 0,
                            .mergeOrder = std::numeric_limits<std::size_t>::max()};
        std::size_t order = 1;
        for (const auto& merge : graph.merges()) {
            const auto entries = merge.entries();
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (entries[index].layerId != boundary.layerId)
                    continue;
                placement.merge = merge.nodeId();
                placement.slot = entries[index].slotId;
                placement.index = index;
                placement.mergeOrder = output == merge.nodeId() ? 0 : order;
            }
            ++order;
        }
        // #502: a Layer node further up an entry's chain (Layer -> filter -> Merge) is still
        // that entry's layer, though no slot names it.
        if (!placement.merge)
            if (const auto entries = document::mergeEntriesOfLayer(graph, boundary.layerId);
                !entries.empty())
                if (const auto* stack = graph.merge(entries.front().merge)) {
                    const auto stackEntries = stack->entries();
                    for (std::size_t index = 0; index < stackEntries.size(); ++index)
                        if (stackEntries[index].slotId == entries.front().slot) {
                            placement.merge = entries.front().merge;
                            placement.slot = entries.front().slot;
                            placement.index = index;
                            std::size_t rank = 1;
                            for (const auto& merge : graph.merges()) {
                                if (merge.nodeId() == entries.front().merge)
                                    placement.mergeOrder = output == merge.nodeId() ? 0 : rank;
                                ++rank;
                            }
                        }
                }
        placements.push_back(placement);
    }
    std::ranges::stable_sort(placements, {}, [](const Placement& placement) {
        return std::pair(placement.mergeOrder, placement.index);
    });
    auto* records = out.array();
    for (const auto& placement : placements) {
        const auto& layer = *placement.layer;
        auto* row = out.object();
        out.set(row, "id", out.number(layer.layerId.value()));
        out.set(row, "name", out.text(layer.name));
        out.set(row, "index", placement.merge ? out.number(placement.index) : out.null());
        out.set(row, "slot", placement.slot ? out.number(placement.slot->value()) : out.null());
        out.set(row, "merge", placement.merge ? out.number(placement.merge->value()) : out.null());
        // #502: an entry layer has no Layer Output node; its `merge` and `slot` own it.
        out.set(row, "layerOutputNode",
                layer.entryOwned() ? out.null() : out.number(layer.nodeId.value()));
        const document::NodeRecord* source = nullptr;
        if (layer.entryOwned()) {
            if (const auto* edge =
                    document::mergeSlotImageEdge(graph, layer.nodeId, *layer.entrySlot))
                source = graph.findNode(edge->source.nodeId);
        }
        for (const std::string_view port :
             {document::kLayerOutputContentInputPort, document::kLayerOutputAudioInputPort}) {
            for (const auto& edge : graph.edges()) {
                const auto* target = std::get_if<document::NodeInputRef>(&edge.destination);
                if (!layer.entryOwned() && source == nullptr && target != nullptr &&
                    target->nodeId == layer.nodeId && target->port == port)
                    source = graph.findNode(edge.source.nodeId);
            }
        }
        // #646: named warnings (an image layer made from an SVG asset draws nothing).
        auto* layerWarnings = out.array();
        if (const auto warning = document::layerSvgFootageWarning(project, comp, layer.layerId))
            out.append(layerWarnings, out.text(*warning));
        out.set(row, "warnings", layerWarnings);
        out.set(row, "sourceNode", source ? out.number(source->id.value()) : out.null());
        out.set(row, "sourceType", source ? out.text(source->typeId) : out.null());
        out.set(row, "enabled", out.boolean(layer.enabled));
        out.set(row, "solo", out.boolean(layer.solo));
        out.set(row, "locked", out.boolean(layer.locked));
        out.set(row, "shy", out.boolean(layer.shy));
        out.set(row, "nameFollowsSource", out.boolean(layer.nameFollowsSource));
        if (const auto name = document::layerSourceName(project, comp, layer))
            out.set(row, "sourceName", out.text(*name));
        if (layer.labelColor) {
            auto* color = out.array();
            for (const auto channel : *layer.labelColor)
                out.append(color, out.number(channel));
            out.set(row, "labelColor", color);
        } else {
            out.set(row, "labelColor", out.null());
        }
        out.set(row, "parent", layer.parent ? out.number(layer.parent->value()) : out.null());
        out.set(row, "start", timeValue(out, layer.inPoint, rate));
        out.set(row, "end", timeValue(out, layer.endPoint(comp.duration()), rate));
        auto* timing = out.object();
        out.set(timing, "startTime", timeValue(out, layer.timing.startTime, rate));
        out.set(timing, "speed",
                pair(out, layer.timing.speed.numerator(), layer.timing.speed.denominator()));
        out.set(timing, "before", out.text(timeModeName(layer.timing.beforeMode)));
        out.set(timing, "after", out.text(timeModeName(layer.timing.afterMode)));
        if (const auto* fixed =
                std::get_if<document::FixedLayerExtent>(&layer.timing.contentExtent)) {
            auto* extent = out.object();
            out.set(extent, "duration", timeValue(out, fixed->duration, rate));
            out.set(timing, "extent", extent);
        } else if (const auto* fromSource =
                       std::get_if<document::SourceLayerExtent>(&layer.timing.contentExtent)) {
            auto* extent = out.object();
            out.set(extent, "sourceNode", out.number(fromSource->sourceNodeId.value()));
            out.set(timing, "extent", extent);
        } else {
            out.set(timing, "extent", out.text("unbounded"));
        }
        out.set(row, "timing", timing);
        auto* masks = out.array();
        constexpr std::array<std::string_view, 5> modes{"none", "add", "subtract", "intersect",
                                                        "difference"};
        for (const auto& mask : layer.masks) {
            auto* item = out.object();
            out.set(item, "id", out.number(mask.id.value()));
            out.set(item, "name", out.text(mask.name));
            const auto mode = static_cast<std::size_t>(mask.mode);
            out.set(item, "mode", out.text(mode < modes.size() ? modes[mode] : "add"));
            out.set(item, "modeValue", out.number(mode));
            out.set(item, "inverted", out.boolean(mask.inverted));
            out.set(item, "path", out.number(mask.path.value()));
            out.set(item, "feather", out.number(mask.feather.value()));
            out.set(item, "expansion", out.number(mask.expansion.value()));
            out.set(item, "opacity", out.number(mask.opacity.value()));
            // #383: a matte mask's Mask output.
            if (mask.matte) {
                auto* matte = out.object();
                out.set(matte, "node", out.number(mask.matte->nodeId.value()));
                out.set(matte, "port", out.text(mask.matte->port));
                out.set(item, "matte", matte);
            } else {
                out.set(item, "matte", out.null());
            }
            out.append(masks, item);
        }
        out.set(row, "masks", masks);
        if (layer.trackMatte) {
            auto* matte = out.object();
            out.set(matte, "source", out.number(layer.trackMatte->source.value()));
            // #383: a node matte's Mask output (its source is then 0).
            if (layer.trackMatte->node) {
                auto* node = out.object();
                out.set(node, "node", out.number(layer.trackMatte->node->nodeId.value()));
                out.set(node, "port", out.text(layer.trackMatte->node->port));
                out.set(matte, "node", node);
            } else {
                out.set(matte, "node", out.null());
            }
            out.set(matte, "channel",
                    out.text(layer.trackMatte->channel == document::TrackMatteChannel::Luma
                                 ? "luma"
                                 : "alpha"));
            out.set(matte, "inverted", out.boolean(layer.trackMatte->inverted));
            out.set(row, "trackMatte", matte);
        } else {
            out.set(row, "trackMatte", out.null());
        }
        out.set(row, "parameters", layerRoles(out, graph, layer));
        out.set(row, "sourceParameters", roles(out, source));
        // #502: what kind of layer this is, the Merge entries it is the layer of (more than one
        // for a shared Layer node), whether one is an adjustment entry, and its control values.
        out.set(row, "layerKind", out.text(layer.entryOwned() ? "entry" : "node"));
        out.set(row, "entryLayer", out.boolean(layer.entryOwned()));
        out.set(row, "pivot", out.text(scripting::layerPivotName(layer.pivot)));
        const auto entries = document::mergeEntriesOfLayer(graph, layer.layerId);
        auto* entryRows = out.array();
        std::optional<document::NodeId> adjustment;
        for (const auto& entry : entries) {
            auto* item = out.object();
            out.set(item, "merge", out.number(entry.merge.value()));
            out.set(item, "slot", out.number(entry.slot.value()));
            out.append(entryRows, item);
            if (!adjustment)
                adjustment = document::mergeEntryAdjustment(graph, entry.merge, entry.slot);
        }
        out.set(row, "entries", entryRows);
        out.set(row, "shared", out.boolean(entries.size() > 1));
        out.set(row, "adjustment", optionalId(out, adjustment));
        out.set(row, "values", roleValues(out, scripting::layerRoleValues(comp, &layer)));
        out.append(records, row);
    }
    out.set(result, "records", records);
    // #502: every Merge entry, whatever its layer is -- including entries that have none yet and
    // render as an entry layer at its defaults.
    auto* entries = out.array();
    for (const auto& view : scripting::mergeEntryViews(comp)) {
        if (only && (view.layer == nullptr || view.layer->layerId != *only))
            continue;
        auto* row = out.object();
        out.set(row, "merge", out.number(view.merge.value()));
        out.set(row, "slot", out.number(view.slot.value()));
        out.set(row, "index", out.number(view.index));
        out.set(row, "outputMerge", out.boolean(view.outputMerge));
        appendMergeEntryFields(out, row, view);
        out.append(entries, row);
    }
    out.set(result, "entries", entries);
    return result;
}

// ---- keyframes

struct KeyRow final {
    std::uint64_t keyframe = 0;
    std::optional<std::string_view> component;
    core::RationalTime time;
    std::string value;
    document::KeyframeInterpolation interpolation = document::KeyframeInterpolation::Linear;
    document::KeyframeHandle outgoing;
    document::KeyframeHandle incoming;
    document::KeyframeEasingParameters easing;
    document::KeyframeHandleMode handleMode = document::KeyframeHandleMode::Free;
};

std::string number(const double value) {
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), converted.ptr);
}

void scalarRows(std::vector<KeyRow>& rows, const std::vector<document::ScalarKeyframe>& keys,
                const std::optional<std::string_view> component) {
    for (const auto& key : keys)
        rows.push_back({.keyframe = key.id.value(),
                        .component = component,
                        .time = key.time,
                        .value = number(key.value),
                        .interpolation = key.outgoingInterpolation,
                        .outgoing = key.outgoingHandle,
                        .incoming = key.incomingHandle,
                        .easing = key.easing,
                        .handleMode = key.handleMode});
}

template <std::size_t Count>
void componentRows(std::vector<KeyRow>& rows,
                   const std::array<document::ComponentAnimationCurve, Count>& components,
                   const std::array<std::string_view, Count>& names) {
    for (std::size_t index = 0; index < Count; ++index)
        scalarRows(rows, components[index].keyframes, names[index]);
}

yyjson_mut_val* keyframesQuery(Json& out, yyjson_mut_val* result,
                               const document::Snapshot& snapshot,
                               const document::Composition& comp, yyjson_val* arguments,
                               const std::uint64_t sessionToken) {
    const bool byParameter = member(arguments, "parameter") != nullptr;
    if (byParameter == (member(arguments, "curve") != nullptr))
        throw InvalidInput("Keyframes need exactly one of parameter or curve");
    std::optional<document::ParameterId> parameterId;
    document::AnimationCurveId curveId;
    if (byParameter) {
        parameterId = document::ParameterId::fromRaw(integer(member(arguments, "parameter")));
        const auto* parameter = comp.parameters().find(*parameterId);
        if (!parameter)
            throw InvalidInput("Stale parameter ID " + std::to_string(parameterId->value()) +
                               "; query {kind:\"layers\"} or {kind:\"nodes\"} lists them");
        const auto* curve = std::get_if<document::AnimationCurveSource>(&parameter->source);
        if (!curve)
            throw InvalidInput("Parameter " + std::to_string(parameterId->value()) +
                               " is not animated; query {kind:\"parameter\"} reads its value");
        curveId = curve->curveId;
    } else {
        curveId = document::AnimationCurveId::fromRaw(integer(member(arguments, "curve")));
    }
    const auto* record = comp.animationCurves().find(curveId);
    if (!record)
        throw InvalidInput("Stale curve ID " + std::to_string(curveId.value()));
    std::vector<KeyRow> rows;
    std::string_view valueKind;
    std::visit(
        [&](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                valueKind = "scalar";
                scalarRows(rows, curve.keyframes, std::nullopt);
            } else if constexpr (std::is_same_v<Curve, document::Vec2AnimationCurve>) {
                valueKind = "vector2";
                componentRows<2>(rows, curve.components, {"x", "y"});
            } else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>) {
                valueKind = "vector3";
                componentRows<3>(rows, curve.components, {"x", "y", "z"});
            } else if constexpr (std::is_same_v<Curve, document::Color4AnimationCurve>) {
                valueKind = "color";
                componentRows<4>(rows, curve.components, {"red", "green", "blue", "alpha"});
            } else {
                valueKind = std::is_same_v<Curve, document::GradientAnimationCurve> ? "gradient"
                                                                                      : "path";
                for (const auto& key : curve.keyframes)
                    rows.push_back({.keyframe = key.id.value(),
                                    .component = std::nullopt,
                                    .time = key.time,
                                    .value = scripting::parameterValueJson(key.value),
                                    .interpolation = key.outgoingInterpolation,
                                    .outgoing = key.outgoingHandle,
                                    .incoming = key.incomingHandle,
                                    .easing = key.easing,
                                    .handleMode = key.handleMode});
            }
        },
        *record);
    std::ranges::stable_sort(rows, {}, &KeyRow::time);
    const auto pageSize =
        member(arguments, "pageSize") ? integer(member(arguments, "pageSize")) : kPageSize;
    if (pageSize == 0 || pageSize > kPageSize)
        throw InvalidInput("Keyframe pageSize must be in 1..64");
    const std::array<std::uint64_t, 5> binding{sessionToken, snapshot.project().id().value(),
                                               comp.id().value(), snapshot.revision().value(),
                                               curveId.value()};
    const auto offset =
        member(arguments, "cursor")
            ? parsePageCursor(string(member(arguments, "cursor"), 160), binding, rows.size())
            : 0;
    const auto end = offset + std::min<std::size_t>(pageSize, rows.size() - offset);
    const auto rate = comp.format().frameRate();
    std::string text = "[";
    for (auto index = offset; index < end; ++index) {
        const auto& row = rows[index];
        const auto handle = [](const document::KeyframeHandle& value) {
            return "{\"time\":" + number(value.time) + ",\"value\":" + number(value.value) + "}";
        };
        text +=
            (index == offset ? "" : ",") + std::string("{\"keyframe\":") +
            std::to_string(row.keyframe) + ",\"component\":" +
            (row.component ? "\"" + std::string(*row.component) + "\"" : std::string("null")) +
            ",\"time\":" + scripting::timeJson(row.time, rate) + ",\"value\":" + row.value +
            ",\"interpolation\":\"" +
            std::string(document::keyframeInterpolationToken(row.interpolation)) +
            "\",\"interpolationId\":" +
            std::to_string(static_cast<std::uint64_t>(row.interpolation)) +
            ",\"outgoingHandle\":" + handle(row.outgoing) +
            ",\"incomingHandle\":" + handle(row.incoming) + ",\"ease\":\"" +
            std::string(document::keyframeEasingToken(row.easing.easing)) +
            "\",\"back\":" + number(row.easing.back) +
            ",\"amplitude\":" + number(row.easing.amplitude) +
            ",\"period\":" + number(row.easing.period) + ",\"handleMode\":\"" +
            std::string(document::keyframeHandleModeToken(row.handleMode)) +
            "\",\"handleModeId\":" + std::to_string(static_cast<std::uint64_t>(row.handleMode)) +
            "}";
    }
    out.set(result, "composition", out.number(comp.id().value()));
    out.set(result, "parameter", parameterId ? out.number(parameterId->value()) : out.null());
    out.set(result, "curve", out.number(curveId.value()));
    out.set(result, "valueKind", out.text(valueKind));
    out.set(result, "frameRate", pair(out, rate.numerator(), rate.denominator()));
    out.set(result, "records", parsedJson(out, text + "]"));
    out.set(result, "totalRecords", out.number(rows.size()));
    out.set(result, "nextCursor",
            end < rows.size() ? out.text(pageCursor(binding[0], binding[1], binding[2], binding[3],
                                                    binding[4], end))
                              : out.null());
    return result;
}

// ---- node types

yyjson_mut_val* nodeTypesQuery(Json& out, yyjson_mut_val* result, yyjson_val* arguments) {
    const auto& registry = document::builtInNodeDefinitions();
    if (auto* requested = member(arguments, "nodeType")) {
        const auto typeId = string(requested, 128);
        const auto definitions = registry.definitions();
        const auto found = std::ranges::find_if(
            definitions, [&](const auto& definition) { return definition.key.typeId == typeId; });
        if (found == definitions.end())
            throw InvalidInput("Unknown node type " + typeId +
                               "; query {kind:\"node-types\"} lists every type");
        out.set(result, "record", parsedJson(out, scripting::nodeTypeJson(*found, true)));
        return result;
    }
    std::string text = "[";
    for (const auto& definition : registry.definitions())
        text += (text.size() > 1 ? "," : "") + scripting::nodeTypeJson(definition, false);
    out.set(result, "records", parsedJson(out, text + "]"));
    return result;
}

} // namespace

bool isReadbackQuery(const std::string_view kind) noexcept {
    return kind == "layers" || kind == "keyframes" || kind == "node-types";
}

yyjson_mut_val* readbackQuery(Json& out, McpCall& call, yyjson_val* arguments,
                              const std::string_view kind, const std::uint64_t sessionToken) {
    if (kind == "layers")
        members(arguments, {"kind", "composition", "layer", "fields"}, {"kind", "composition"});
    else if (kind == "keyframes")
        members(arguments,
                {"kind", "composition", "parameter", "curve", "pageSize", "cursor", "fields"},
                {"kind", "composition"});
    else
        members(arguments, {"kind", "nodeType", "fields"}, {"kind"});
    const auto snapshot = call.session.snapshot();
    auto* result = out.object();
    out.set(result, "revision", out.number(snapshot.revision().value()));
    if (kind == "node-types") {
        nodeTypesQuery(out, result, arguments);
        projectRecords(out, result, arguments);
        return result;
    }
    const auto& composition = compositionArgument(snapshot, arguments);
    if (kind == "layers") {
        layersQuery(out, result, snapshot.project(), composition,
                    layerFilter(composition, arguments));
    } else {
        if (member(arguments, "layer"))
            throw InvalidInput("layer applies to the layers and nodes queries");
        keyframesQuery(out, result, snapshot, composition, arguments, sessionToken);
    }
    projectRecords(out, result, arguments);
    return result;
}

yyjson_mut_val* compositionRecord(Json& out, const document::Composition& composition) {
    const auto format = composition.format();
    const auto rate = format.frameRate();
    auto* record = out.object();
    out.set(record, "id", out.number(composition.id().value()));
    out.set(record, "name", out.text(composition.name()));
    out.set(record, "width", out.number(format.width()));
    out.set(record, "height", out.number(format.height()));
    out.set(record, "frameRateNumerator", out.number(rate.numerator()));
    out.set(record, "frameRateDenominator", out.number(rate.denominator()));
    out.set(record, "frameRate", pair(out, rate.numerator(), rate.denominator()));
    out.set(
        record, "fps",
        out.real(static_cast<double>(rate.numerator()) / static_cast<double>(rate.denominator())));
    out.set(record, "pixelAspect",
            pair(out, static_cast<std::int64_t>(format.pixelAspect().numerator()),
                 static_cast<std::int64_t>(format.pixelAspect().denominator())));
    out.set(record, "duration", timeValue(out, composition.duration(), rate));
    const auto timecode = composition.timecode();
    out.set(record, "start", timeValue(out, timecode.start, rate));
    out.set(record, "dropFrame", out.boolean(timecode.dropFrame));
    if (const auto& area = composition.workArea()) {
        auto* work = out.object();
        out.set(work, "start", timeValue(out, area->start, rate));
        out.set(work, "end", timeValue(out, area->end, rate));
        out.set(record, "workArea", work);
    } else {
        out.set(record, "workArea", out.null());
    }
    const auto background = composition.backgroundColor();
    auto* color = out.array();
    for (const auto channel : {background.red, background.green, background.blue, background.alpha})
        out.append(color, out.real(channel));
    out.set(record, "backgroundColor", color);
    out.set(record, "workingColorSpace",
            composition.workingColorSpaceId() ? out.text(*composition.workingColorSpaceId())
                                              : out.null());
    auto* safe = out.object();
    out.set(safe, "action", out.real(composition.safeAreas().action));
    out.set(safe, "title", out.real(composition.safeAreas().title));
    out.set(record, "safeAreas", safe);
    out.set(record, "layerCount", out.number(composition.graph().layerOutputs().size()));
    out.set(record, "folder",
            composition.folder() ? out.number(composition.folder()->value()) : out.null());
    return record;
}

yyjson_mut_val* assetRecord(Json& out, const document::AssetRecord& asset) {
    auto* record = out.object();
    out.set(record, "id", out.number(asset.id.value()));
    out.set(record, "name", out.text(asset.name));
    out.set(record, "kind",
            out.text(scripting::assetKindName(static_cast<std::uint8_t>(asset.kind))));
    out.set(record, "path", out.text(asset.locator.path));
    out.set(record, "width", out.number(asset.width));
    out.set(record, "height", out.number(asset.height));
    out.set(record, "frames", out.number(asset.frames));
    const auto digest = asset.contentDigest.toLowercaseHex();
    out.set(record, "digest", out.text("sha256:" + std::string(digest.data(), digest.size())));
    out.set(record, "folder", asset.folder ? out.number(asset.folder->value()) : out.null());
    auto* tags = out.array();
    for (const auto& tag : asset.tags)
        out.append(tags, out.text(tag));
    out.set(record, "tags", tags);
    return record;
}

yyjson_mut_val* assetFolders(Json& out, const document::Project& project) {
    auto* folders = out.array();
    for (const auto& folder : project.assetFolders()) {
        auto* row = out.object();
        out.set(row, "id", out.number(folder.id.value()));
        out.set(row, "name", out.text(folder.name));
        out.set(row, "parent", folder.parent ? out.number(folder.parent->value()) : out.null());
        out.append(folders, row);
    }
    return folders;
}

std::optional<document::LayerId> layerFilter(const document::Composition& composition,
                                             yyjson_val* arguments) {
    if (!member(arguments, "layer"))
        return std::nullopt;
    const auto id = document::LayerId::fromRaw(identifier(member(arguments, "layer")));
    if (composition.graph().findLayer(id) == nullptr)
        throw InvalidInput("Layer " + std::to_string(id.value()) + " does not exist in composition " +
                           std::to_string(composition.id().value()) +
                           "; query {kind:\"layers\",\"composition\":" +
                           std::to_string(composition.id().value()) + "} lists its layers");
    return id;
}

void projectRecords(Json& out, yyjson_mut_val* result, yyjson_val* arguments) {
    auto* requested = member(arguments, "fields");
    if (requested == nullptr)
        return;
    if (!yyjson_is_arr(requested) || yyjson_arr_size(requested) == 0 ||
        yyjson_arr_size(requested) > 64)
        throw InvalidInput("fields is a list of 1 to 64 member names, such as [\"id\", \"name\"]");
    std::vector<std::string> names;
    yyjson_arr_iter names_iterator = yyjson_arr_iter_with(requested);
    while (auto* item = yyjson_arr_iter_next(&names_iterator))
        names.push_back(string(item, 128));
    auto* records = yyjson_mut_obj_get(result, "records");
    if (records == nullptr || !yyjson_mut_is_arr(records))
        throw InvalidInput("fields applies to the queries that return records");
    // Every requested name must exist on the first record, so a misspelling is an error, not an
    // empty answer.
    if (auto* first = yyjson_mut_arr_get_first(records); first != nullptr) {
        std::string available;
        std::vector<std::string> present;
        yyjson_mut_obj_iter probe = yyjson_mut_obj_iter_with(first);
        while (auto* key = yyjson_mut_obj_iter_next(&probe)) {
            present.emplace_back(yyjson_mut_get_str(key), yyjson_mut_get_len(key));
            available += (available.empty() ? "" : ", ") + present.back();
        }
        for (const auto& name : names)
            if (std::ranges::find(present, name) == present.end())
                throw InvalidInput("Unknown field " + name + "; the records carry: " + available);
    }
    yyjson_mut_arr_iter iterator = yyjson_mut_arr_iter_with(records);
    while (auto* record = yyjson_mut_arr_iter_next(&iterator)) {
        if (!yyjson_mut_is_obj(record))
            continue;
        std::vector<std::string> drop;
        yyjson_mut_obj_iter members_iterator = yyjson_mut_obj_iter_with(record);
        while (auto* key = yyjson_mut_obj_iter_next(&members_iterator)) {
            const std::string name(yyjson_mut_get_str(key), yyjson_mut_get_len(key));
            if (std::ranges::find(names, name) == names.end())
                drop.push_back(name);
        }
        for (const auto& name : drop)
            yyjson_mut_obj_remove_strn(record, name.data(), name.size());
    }
    static_cast<void>(out);
}

void appendNodeGraph(Json& out, yyjson_mut_val* result, yyjson_mut_val* records,
                     const document::Composition& composition,
                     const std::optional<document::LayerId> layer, const document::Project* project) {
    const auto& graph = composition.graph();
    // With a layer filter: that layer's node (or its Merge entry's source) and everything upstream.
    std::set<document::NodeId> included;
    if (layer) {
        std::vector<document::NodeId> pending;
        if (const auto* boundary = graph.findLayer(*layer)) {
            if (boundary->entryOwned()) {
                if (const auto* edge =
                        document::mergeSlotImageEdge(graph, boundary->nodeId, *boundary->entrySlot))
                    pending.push_back(edge->source.nodeId);
            } else {
                pending.push_back(boundary->nodeId);
            }
        }
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (!included.insert(current).second)
                continue;
            for (const auto& edge : graph.edges()) {
                const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                if (input != nullptr && input->nodeId == current)
                    pending.push_back(edge.source.nodeId);
            }
        }
        out.set(result, "layer", out.number(layer->value()));
    }
    const auto keep = [&](const document::NodeId node) {
        return !layer || included.contains(node);
    };
    const auto& registry = document::builtInNodeDefinitions();
    const auto& layout = composition.nodeLayout();
    const auto target = [&out](const document::InputPortRef& input) {
        auto* value = out.object();
        if (const auto* port = std::get_if<document::NodeInputRef>(&input)) {
            out.set(value, "node", out.number(port->nodeId.value()));
            out.set(value, "port", out.text(port->port));
        } else {
            const auto& stack = std::get<document::LayerStackInputRef>(input);
            out.set(value, "node", out.number(stack.stackNodeId.value()));
            out.set(value, "role", out.text(stack.role));
            out.set(value, "slot", out.number(stack.slotId.value()));
        }
        return value;
    };
    out.set(result, "composition", out.number(composition.id().value()));
    for (const auto& node : graph.nodes()) {
        if (!keep(node.id))
            continue;
        const auto* definition = registry.find(node.typeId, node.schemaVersion);
        auto* record = out.object();
        out.set(record, "id", out.number(node.id.value()));
        out.set(record, "typeId", out.text(node.typeId));
        out.set(record, "name", out.text(scripting::nodeTypeDisplayName(node.typeId)));
        out.set(record, "category",
                definition ? out.text(scripting::nodeCategoryName(*definition)) : out.null());
        out.set(record, "parameters", roles(out, &node));
        // #646: named warnings (an image source on an SVG record draws nothing).
        auto* nodeWarnings = out.array();
        if (project != nullptr)
            if (const auto warning = document::svgFootageWarning(*project, composition, node))
                out.append(nodeWarnings, out.text(*warning));
        out.set(record, "warnings", nodeWarnings);
        const auto placed = layout.find(node.id);
        if (placed != layout.end()) {
            auto* position = out.array();
            out.append(position, out.real(placed->second.position.x));
            out.append(position, out.real(placed->second.position.y));
            out.set(record, "position", position);
            out.set(record, "width", out.real(placed->second.width));
            out.set(record, "collapsed", out.boolean(placed->second.collapsed));
            out.set(record, "muted", out.boolean(placed->second.muted));
        } else {
            out.set(record, "position", out.null());
        }
        const auto* group = document::findNodeGroupOf(composition.nodeGroups(), node.id);
        out.set(record, "group", group ? out.number(group->id.value()) : out.null());
        auto* inputs = out.array();
        if (definition != nullptr) {
            for (const auto& port : definition->inputs) {
                auto* row = out.object();
                out.set(row, "port", out.text(port.name));
                out.set(row, "kind", out.text(scripting::socketKindName(port.valueKind)));
                out.set(row, "required", out.boolean(port.required));
                // #490: an inspection input (the Image Preview's) also takes these kinds.
                if (port.acceptsPixelKinds) {
                    auto* also = out.array();
                    out.append(
                        also, out.text(scripting::socketKindName(document::SocketValueKind::Mask)));
                    out.append(also, out.text(scripting::socketKindName(
                                         document::SocketValueKind::ChannelSet)));
                    out.set(row, "alsoAccepts", also);
                }
                yyjson_mut_val* source = out.null();
                for (const auto& edge : graph.edges()) {
                    const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                    if (input != nullptr && input->nodeId == node.id && input->port == port.name) {
                        source = out.object();
                        out.set(source, "node", out.number(edge.source.nodeId.value()));
                        out.set(source, "port", out.text(edge.source.port));
                    }
                }
                out.set(row, "source", source);
                out.append(inputs, row);
            }
            // #543: each stop of a bound gradient takes a colour and a position link; a linked
            // one names the output that drives it.
            for (const auto& parameter : definition->parameters) {
                if (parameter.valueKind != document::ParameterValueKind::Gradient)
                    continue;
                const auto gradient = std::ranges::find(node.parameters, parameter.role,
                                                        &document::ParameterBinding::role);
                const auto* held = gradient == node.parameters.end()
                                       ? nullptr
                                       : composition.parameters().find(gradient->parameterId);
                if (held == nullptr)
                    continue;
                std::size_t count = 0;
                if (const auto* constant =
                        std::get_if<document::ConstantValueSource>(&held->source))
                    if (const auto* value = std::get_if<document::GradientValue>(&constant->value))
                        count = value->stops.size();
                if (const auto* animated =
                        std::get_if<document::AnimationCurveSource>(&held->source))
                    if (const auto* curve =
                            composition.animationCurves().findGradient(animated->curveId))
                        for (const auto& key : curve->keyframes)
                            count = std::max(count, key.value.stops.size());
                for (const auto& binding : node.parameters)
                    if (const auto stop = document::parseGradientStopPort(binding.role);
                        stop && stop->role == parameter.role)
                        count = std::max<std::size_t>(count, stop->stop + 1U);
                for (std::uint32_t stop = 0; stop < count; ++stop)
                    for (const auto component : {document::GradientStopComponent::Color,
                                                 document::GradientStopComponent::Position}) {
                        const auto port =
                            document::gradientStopPortName(parameter.role, stop, component);
                        auto* row = out.object();
                        out.set(row, "port", out.text(port));
                        out.set(row, "kind",
                                out.text(scripting::socketKindName(
                                    component == document::GradientStopComponent::Color
                                        ? document::SocketValueKind::Color
                                        : document::SocketValueKind::Scalar)));
                        out.set(row, "required", out.boolean(false));
                        yyjson_mut_val* source = out.null();
                        const auto link = std::ranges::find(node.parameters, port,
                                                            &document::ParameterBinding::role);
                        const auto* linked = link == node.parameters.end()
                                                 ? nullptr
                                                 : composition.parameters().find(link->parameterId);
                        if (const auto* driver =
                                linked ? std::get_if<document::DriverBindingSource>(&linked->source)
                                       : nullptr) {
                            source = out.object();
                            out.set(source, "node", out.number(driver->sourceNodeId.value()));
                            out.set(source, "port", out.text(driver->outputPort));
                        }
                        out.set(row, "source", source);
                        out.append(inputs, row);
                    }
            }
        }
        out.set(record, "inputs", inputs);
        auto* outputs = out.array();
        if (definition != nullptr) {
            for (const auto& port : definition->outputs) {
                auto* row = out.object();
                out.set(row, "port", out.text(port.name));
                out.set(row, "kind", out.text(scripting::socketKindName(port.valueKind)));
                auto* targets = out.array();
                for (const auto& edge : graph.edges())
                    if (edge.source.nodeId == node.id && edge.source.port == port.name)
                        out.append(targets, target(edge.destination));
                out.set(row, "targets", targets);
                out.append(outputs, row);
            }
        }
        out.set(record, "outputs", outputs);
        if (const auto* merge = graph.merge(node.id)) {
            auto* stack = out.array();
            const auto entries = merge->entries();
            for (std::size_t index = 0; index < entries.size(); ++index) {
                auto* row = out.object();
                out.set(row, "slot", out.number(entries[index].slotId.value()));
                out.set(row, "layer", out.number(entries[index].layerId.value()));
                out.set(row, "index", out.number(index));
                if (const auto view =
                        scripting::mergeEntryView(composition, node.id, entries[index].slotId))
                    appendMergeEntryFields(out, row, *view);
                out.append(stack, row);
            }
            out.set(record, "stack", stack);
            out.set(record, "enabled", out.boolean(merge->enabled()));
        }
        out.append(records, record);
    }
    auto* links = out.array();
    for (const auto& edge : graph.edges()) {
        if (layer) {
            const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            if (!keep(edge.source.nodeId) || input == nullptr || !keep(input->nodeId))
                continue;
        }
        auto* row = out.object();
        out.set(row, "id", out.number(edge.id.value()));
        auto* source = out.object();
        out.set(source, "node", out.number(edge.source.nodeId.value()));
        out.set(source, "port", out.text(edge.source.port));
        out.set(row, "source", source);
        out.set(row, "target", target(edge.destination));
        out.append(links, row);
    }
    out.set(result, "links", links);
    auto* groups = out.array();
    for (const auto& [id, group] : composition.nodeGroups()) {
        if (layer && std::ranges::none_of(group.members, keep))
            continue;
        auto* row = out.object();
        out.set(row, "id", out.number(id.value()));
        out.set(row, "name", out.text(group.name));
        auto* members = out.array();
        for (const auto node : group.members)
            if (keep(node))
                out.append(members, out.number(node.value()));
        out.set(row, "members", members);
        out.append(groups, row);
    }
    out.set(result, "groups", groups);
}

yyjson_mut_val* parameterMetadata(Json& out, const document::Composition& composition,
                                  const document::ParameterRecord& parameter) {
    const auto& registry = document::builtInNodeDefinitions();
    const document::ParameterDefinition* definition = nullptr;
    std::string owner = "null";
    for (const auto& node : composition.graph().nodes()) {
        for (const auto& binding : node.parameters) {
            if (binding.parameterId != parameter.id)
                continue;
            if (const auto* type = registry.find(node.typeId, node.schemaVersion))
                definition = scripting::findParameterDefinition(*type, binding.role);
            owner = "{\"node\":" + std::to_string(node.id.value()) + ",\"typeId\":\"" +
                    node.typeId + "\",\"role\":\"" + binding.role + "\"}";
        }
    }
    for (const auto& layer : composition.graph().layerOutputs()) {
        // #502: an entry layer's own transform, opacity, blend mode and Time Remap take the Layer
        // node's parameter definitions.
        for (const auto& binding : layer.parameters) {
            if (binding.parameterId != parameter.id)
                continue;
            if (const auto* type = registry.find(document::kLayerOutputNodeType,
                                                 document::kLayerOutputNodeSchemaVersion))
                definition = scripting::findParameterDefinition(*type, binding.role);
            owner = "{\"layer\":" + std::to_string(layer.layerId.value()) +
                    ",\"merge\":" + std::to_string(layer.nodeId.value()) + ",\"slot\":" +
                    std::to_string(layer.entrySlot ? layer.entrySlot->value() : 0) +
                    ",\"role\":\"" + binding.role + "\"}";
        }
        for (const auto& mask : layer.masks) {
            const std::array<std::pair<document::ParameterId, std::string_view>, 4> roles{
                {{mask.path, "path"},
                 {mask.feather, "feather"},
                 {mask.expansion, "expansion"},
                 {mask.opacity, "opacity"}}};
            for (const auto& [id, role] : roles)
                if (id == parameter.id)
                    owner = "{\"layer\":" + std::to_string(layer.layerId.value()) +
                            ",\"mask\":" + std::to_string(mask.id.value()) + ",\"role\":\"" +
                            std::string(role) + "\"}";
        }
    }
    // The value kind of the stored value, or of the definition when the value is a curve.
    auto kind = definition ? definition->valueKind : document::ParameterValueKind::Float64;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter.source)) {
        kind = std::visit(
            [kind](const auto& held) {
                using T = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<T, bool>)
                    return document::ParameterValueKind::Boolean;
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    return document::ParameterValueKind::Integer;
                else if constexpr (std::is_same_v<T, double>)
                    return document::ParameterValueKind::Float64;
                else if constexpr (std::is_same_v<T, std::string>)
                    return document::ParameterValueKind::String;
                else if constexpr (std::is_same_v<T, document::Vec2d>)
                    return document::ParameterValueKind::Vec2d;
                else if constexpr (std::is_same_v<T, document::Vec3d>)
                    return document::ParameterValueKind::Vec3d;
                else if constexpr (std::is_same_v<T, core::Color4d>)
                    return document::ParameterValueKind::Color4d;
                else if constexpr (std::is_same_v<T, document::PathValue>)
                    return document::ParameterValueKind::Path;
                else if constexpr (std::is_same_v<T, document::GradientValue>)
                    return document::ParameterValueKind::Gradient;
                else if constexpr (std::is_same_v<T, document::NumberListValue>)
                    return document::ParameterValueKind::NumberList;
                else
                    return kind;
            },
            constant->value);
    }
    auto text = scripting::parameterMetadataJson(parameter.schemaKey, kind, definition);
    text.pop_back();
    text += ",\"owner\":" + owner + "}";
    return parsedJson(out, text);
}

} // namespace bloom::mcp
