#include "native.hpp"
#include <bloom/scripting/command_diagnostics.hpp>

#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <bloom/document/animation.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/scripting/merge_entry_view.hpp>

#include <string>

#include <type_traits>

namespace bloom::scripting::python {
namespace {

nb::object parameterValue(const document::ParameterValue& value) {
    return std::visit(
        [](const auto& item) -> nb::object {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, document::Vec2d>) {
                return nb::make_tuple(item.x, item.y);
            } else if constexpr (std::is_same_v<T, document::Vec3d>) {
                return nb::make_tuple(item.x, item.y, item.z);
            } else if constexpr (std::is_same_v<T, core::Color4d>) {
                return nb::make_tuple(item.red, item.green, item.blue, item.alpha);
            } else if constexpr (std::is_same_v<T, core::RationalTime>) {
                return nb::make_tuple(item.numerator(), item.denominator());
            } else if constexpr (std::is_same_v<T, document::PathValue>) {
                const auto contourOf = [](const document::PathContour& contour) {
                    nb::dict path;
                    path["closed"] = contour.closed;
                    nb::list anchors;
                    for (const auto& anchor : contour.anchors) {
                        nb::dict row;
                        row["point"] = nb::make_tuple(anchor.point.x, anchor.point.y);
                        row["in_handle"] =
                            anchor.inHandle ? nb::make_tuple(anchor.inHandle->x, anchor.inHandle->y)
                                            : nb::object(nb::none());
                        row["out_handle"] = anchor.outHandle ? nb::make_tuple(anchor.outHandle->x,
                                                                              anchor.outHandle->y)
                                                             : nb::object(nb::none());
                        row["mode"] = std::string(document::pathAnchorModeName(anchor.mode));
                        anchors.append(row);
                    }
                    path["anchors"] = anchors;
                    return path;
                };
                if (document::pathValueHasFlatForm(item))
                    return contourOf(item.firstContour());
                // #589: {"contours": [...], "fill_rule": ..., "groups": [...]}, as a path argument
                // accepts. Each of the last two appears only when it says something.
                nb::dict path;
                nb::list contours;
                for (const auto& contour : item.contours)
                    contours.append(contourOf(contour));
                path["contours"] = contours;
                if (item.fillRule != document::PathFillRule::Inherit)
                    path["fill_rule"] = std::string(document::pathFillRuleName(item.fillRule));
                if (!item.groups.empty()) {
                    nb::list groups;
                    for (const auto& group : item.groups) {
                        nb::dict row;
                        row["key"] = group.key;
                        row["first_contour"] = group.firstContour;
                        row["contour_count"] = group.contourCount;
                        if (group.color)
                            row["color"] = nb::make_tuple(group.color->red, group.color->green,
                                                          group.color->blue, group.color->alpha);
                        groups.append(row);
                    }
                    path["groups"] = groups;
                }
                return path;
            } else if constexpr (std::is_same_v<T, document::GradientValue>) {
                // #541: {"stops": [{"position": p, "color": (r, g, b, a)}]}.
                nb::list stops;
                for (const auto& stop : item.stops) {
                    nb::dict row;
                    row["position"] = stop.position;
                    row["color"] = nb::make_tuple(stop.color.red, stop.color.green,
                                                  stop.color.blue, stop.color.alpha);
                    stops.append(row);
                }
                nb::dict gradient;
                gradient["stops"] = stops;
                // #546: how it blends between them.
                gradient["interpolation"] =
                    std::string(document::gradientInterpolationName(item.modes.interpolation));
                gradient["color_mode"] =
                    std::string(document::gradientColorModeName(item.modes.colorMode));
                gradient["hue_direction"] =
                    std::string(document::gradientHueDirectionName(item.modes.hueDirection));
                return gradient;
            } else {
                return nb::cast(item);
            }
        },
        value);
}

// Document 1.30: the interpolation's stable token next to its enum id, and the easing-equation
// parameters every key carries.
template <typename Keyframe> void easing(nb::dict& row, const Keyframe& key) {
    row["interpolation_name"] =
        std::string(document::keyframeInterpolationToken(key.outgoingInterpolation));
    row["ease"] = std::string(document::keyframeEasingToken(key.easing.easing));
    row["back"] = key.easing.back;
    row["amplitude"] = key.easing.amplitude;
    row["period"] = key.easing.period;
    row["handle_mode"] = std::string(document::keyframeHandleModeToken(key.handleMode));
    row["outgoing_handle"] = nb::make_tuple(key.outgoingHandle.time, key.outgoingHandle.value);
    row["incoming_handle"] = nb::make_tuple(key.incomingHandle.time, key.incomingHandle.value);
}

nb::list keyframes(const std::vector<document::ScalarKeyframe>& keys) {
    nb::list result;
    for (const auto& key : keys) {
        nb::dict row;
        row["id"] = key.id.value();
        row["time"] = nb::make_tuple(key.time.numerator(), key.time.denominator());
        row["value"] = key.value;
        row["interpolation"] = static_cast<int>(key.outgoingInterpolation);
        easing(row, key);
        result.append(row);
    }
    return result;
}

// ADR 0028: a Path key's value projects as the whole path (closed flag, anchors and handles), the
// same mapping a path argument accepts, so a key read here can be passed straight back. #541: a
// Gradient key's value projects as the whole gradient the same way.
template <typename Key>
nb::list wholeValueKeyframes(const std::vector<Key>& keys) {
    nb::list result;
    for (const auto& key : keys) {
        nb::dict row;
        row["id"] = key.id.value();
        row["time"] = nb::make_tuple(key.time.numerator(), key.time.denominator());
        row["value"] = parameterValue(key.value);
        row["interpolation"] = static_cast<int>(key.outgoingInterpolation);
        easing(row, key);
        result.append(row);
    }
    return result;
}

nb::list keyframes(const std::vector<document::PathKeyframe>& keys) {
    return wholeValueKeyframes(keys);
}

nb::list keyframes(const std::vector<document::GradientKeyframe>& keys) {
    return wholeValueKeyframes(keys);
}

// #502: a layer's controls by role -- parameter ids and constant values (None for an animated or
// driven one), or the defaults an entry without a layer renders with.
void roleFields(nb::dict& record, const std::vector<LayerRoleValue>& roles) {
    nb::dict ids;
    nb::dict values;
    for (const auto& role : roles) {
        const nb::str key(role.role.data(), role.role.size());
        if (role.parameter)
            ids[key] = role.parameter->value();
        values[key] = role.value ? parameterValue(*role.value) : nb::object(nb::none());
    }
    record["parameters"] = ids;
    record["values"] = values;
}

nb::object optionalId(const std::optional<document::NodeId>& id) {
    return id ? nb::cast(id->value()) : nb::object(nb::none());
}

nb::dict mergeEntryRecord(const MergeEntryView& view) {
    nb::dict record;
    const auto* layer = view.layer;
    record["merge"] = view.merge.value();
    record["slot"] = view.slot.value();
    record["index"] = view.index;
    record["output_merge"] = view.outputMerge;
    record["layer"] = layer ? nb::cast(layer->layerId.value()) : nb::object(nb::none());
    record["layer_kind"] = std::string(entryLayerKindName(view.kind));
    record["entry_layer"] = view.kind == EntryLayerKind::Entry;
    record["layer_node"] = optionalId(view.layerNode);
    record["source_node"] = optionalId(view.sourceNode);
    record["source_type"] = view.sourceNode ? nb::cast(view.sourceType) : nb::object(nb::none());
    record["name"] = layer ? nb::cast(layer->name) : nb::object(nb::none());
    record["enabled"] = layer == nullptr || layer->enabled;
    record["solo"] = layer != nullptr && layer->solo;
    record["locked"] = layer != nullptr && layer->locked;
    record["shy"] = layer != nullptr && layer->shy;
    record["pivot"] = std::string(layerPivotName(view.pivot));
    record["adjustment"] = optionalId(view.adjustment);
    record["shared"] = view.shared;
    record["nested_merge"] = optionalId(view.nestedMerge);
    roleFields(record, view.roles);
    return record;
}

} // namespace

nb::dict projectSnapshot(const document::Snapshot& snapshot) {
    nb::dict result;
    result["revision"] = snapshot.revision().value();
    nb::dict project;
    project["id"] = snapshot.project().id().value();
    project["name"] = snapshot.project().name();
    result["project"] = project;
    nb::list compositions;
    for (const auto& composition : snapshot.project().compositions()) {
        nb::dict row;
        row["id"] = composition.id().value();
        row["name"] = composition.name();
        row["width"] = composition.format().width();
        row["height"] = composition.format().height();
        row["frame_rate"] = nb::make_tuple(composition.format().frameRate().numerator(),
                                           composition.format().frameRate().denominator());
        row["duration"] = nb::make_tuple(composition.duration().numerator(),
                                         composition.duration().denominator());
        nb::list nodes;
        for (const auto& node : composition.graph().nodes()) {
            nb::dict record;
            record["id"] = node.id.value();
            record["type_id"] = node.typeId;
            nb::dict bindings;
            for (const auto& binding : node.parameters) {
                bindings[nb::str(binding.role.c_str())] = binding.parameterId.value();
            }
            record["parameters"] = bindings;
            nodes.append(record);
        }
        row["nodes"] = nodes;
        nb::list layers;
        for (const auto& boundary : composition.graph().layerOutputs()) {
            nb::dict record;
            record["id"] = boundary.layerId.value();
            record["name"] = boundary.name;
            record["node_id"] = boundary.nodeId.value();
            record["enabled"] = boundary.enabled;
            record["solo"] = boundary.solo;
            record["locked"] = boundary.locked;
            record["shy"] = boundary.shy;
            record["name_follows_source"] = boundary.nameFollowsSource;
            record["parent"] =
                boundary.parent.has_value() ? nb::cast(boundary.parent->value()) : nb::none();
            record["in"] =
                nb::make_tuple(boundary.inPoint.numerator(), boundary.inPoint.denominator());
            record["out"] =
                nb::make_tuple(boundary.outPoint.numerator(), boundary.outPoint.denominator());
            // #502: an entry layer belongs to a Merge entry (node_id is then the Merge); a Layer
            // node's layer lists every entry it is the layer of (more than one when shared).
            const auto& graph = composition.graph();
            record["entry_layer"] = boundary.entryOwned();
            record["layer_kind"] = std::string(boundary.entryOwned() ? "entry" : "node");
            record["entry_slot"] = boundary.entrySlot ? nb::cast(boundary.entrySlot->value())
                                                      : nb::object(nb::none());
            record["pivot"] = std::string(layerPivotName(boundary.pivot));
            nb::list entries;
            std::optional<document::NodeId> adjustment;
            const auto owners = document::mergeEntriesOfLayer(graph, boundary.layerId);
            for (const auto& entry : owners) {
                nb::dict owner;
                owner["merge"] = entry.merge.value();
                owner["slot"] = entry.slot.value();
                entries.append(owner);
                if (!adjustment)
                    adjustment = document::mergeEntryAdjustment(graph, entry.merge, entry.slot);
            }
            record["entries"] = entries;
            record["shared"] = owners.size() > 1;
            record["adjustment"] = optionalId(adjustment);
            roleFields(record, layerRoleValues(composition, &boundary));
            // ADR 0028: masks in order with their parameter ids, and the optional track matte.
            nb::list masks;
            for (const auto& mask : boundary.masks) {
                nb::dict maskRecord;
                maskRecord["id"] = mask.id.value();
                maskRecord["name"] = mask.name;
                maskRecord["mode"] = static_cast<std::int64_t>(mask.mode);
                maskRecord["inverted"] = mask.inverted;
                maskRecord["path"] = mask.path.value();
                maskRecord["feather"] = mask.feather.value();
                maskRecord["expansion"] = mask.expansion.value();
                maskRecord["opacity"] = mask.opacity.value();
                // #383: a matte mask's Mask output, as (node id, port), or None.
                if (mask.matte)
                    maskRecord["matte"] = nb::make_tuple(mask.matte->nodeId.value(), mask.matte->port);
                else
                    maskRecord["matte"] = nb::none();
                masks.append(maskRecord);
            }
            record["masks"] = masks;
            if (boundary.trackMatte) {
                nb::dict matte;
                matte["source"] = boundary.trackMatte->source.value();
                matte["channel"] = static_cast<std::int64_t>(boundary.trackMatte->channel);
                matte["inverted"] = boundary.trackMatte->inverted;
                // #383: a node matte's Mask output, as (node id, port), or None.
                if (boundary.trackMatte->node)
                    matte["node"] = nb::make_tuple(boundary.trackMatte->node->nodeId.value(),
                                                   boundary.trackMatte->node->port);
                else
                    matte["node"] = nb::none();
                record["track_matte"] = matte;
            } else {
                record["track_matte"] = nb::none();
            }
            layers.append(record);
        }
        row["layers"] = layers;
        nb::list mergeEntries;
        for (const auto& view : mergeEntryViews(composition))
            mergeEntries.append(mergeEntryRecord(view));
        row["merge_entries"] = mergeEntries;
        nb::list parameters;
        for (const auto& parameter : composition.parameters().records()) {
            nb::dict record;
            record["id"] = parameter.id.value();
            record["schema_key"] = parameter.schemaKey;
            std::visit(
                [&record](const auto& source) {
                    using T = std::decay_t<decltype(source)>;
                    if constexpr (std::is_same_v<T, document::ConstantValueSource>) {
                        record["source"] = "constant";
                        record["value"] = parameterValue(source.value);
                    } else if constexpr (std::is_same_v<T, document::AnimationCurveSource>) {
                        record["source"] = "curve";
                        record["curve_id"] = source.curveId.value();
                    } else {
                        record["source"] = "driver";
                        record["node_id"] = source.sourceNodeId.value();
                        record["port"] = source.outputPort;
                    }
                },
                parameter.source);
            parameters.append(record);
        }
        row["parameters"] = parameters;
        nb::list curves;
        for (const auto& curve : composition.animationCurves().records()) {
            nb::dict record;
            record["id"] = document::animationCurveId(curve).value();
            nb::list components;
            std::visit(
                [&components](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (document::kWholeValueAnimationCurve<T>) {
                        components.append(keyframes(item.keyframes));
                    } else {
                        for (const auto& component : item.components) {
                            components.append(keyframes(component.keyframes));
                        }
                    }
                },
                curve);
            record["components"] = components;
            curves.append(record);
        }
        row["curves"] = curves;
        compositions.append(row);
    }
    result["compositions"] = compositions;
    nb::list assets;
    for (const auto& asset : snapshot.project().assets()) {
        nb::dict record;
        record["id"] = asset.id.value();
        record["name"] = asset.name;
        record["kind"] = static_cast<int>(asset.kind);
        record["path"] = asset.locator.path;
        record["width"] = asset.width;
        record["height"] = asset.height;
        const auto digest = asset.contentDigest.toLowercaseHex();
        record["digest"] = "sha256:" + std::string(digest.data(), digest.size());
        assets.append(record);
    }
    result["assets"] = assets;
    return result;
}

nb::dict commandResult(const commands::CommandResult& result) {
    nb::dict row;
    row["succeeded"] = result.succeeded();
    row["revision"] = result.afterRevision.value();
    row["status"] = static_cast<int>(result.status);
    nb::list diagnostics;
    for (const auto& item : commandDiagnostics(result)) {
        nb::dict diagnostic;
        diagnostic["code"] = item.code;
        if (item.severity != "error")
            diagnostic["severity"] = item.severity;
        diagnostic["operation_id"] = item.operation;
        diagnostic["operation_index"] =
            item.operationIndex ? nb::cast(*item.operationIndex) : nb::none();
        diagnostic["argument"] = "";
        if (!item.path.empty())
            diagnostic["path"] = item.path;
        diagnostic["message"] = item.message;
        diagnostics.append(diagnostic);
    }
    row["diagnostics"] = diagnostics;
    nb::list outputs;
    for (const auto& output : result.outputs) {
        nb::dict value;
        value["operation_index"] = output.operationIndex;
        value["name"] = output.output.name;
        value["id"] = std::visit([](const auto& id) { return id.value(); }, output.output.id);
        outputs.append(value);
    }
    row["outputs"] = outputs;
    return row;
}

} // namespace bloom::scripting::python
