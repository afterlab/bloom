#include <bloom/commands/operations.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>

#include "node_operation_support.hpp"

#include <bloom/document/merge_entries.hpp>
#include <bloom/commands/node_layout_placement.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bloom::commands {
namespace {

// #589: a path with no closed contour has no area to fill: every contour is an outline.
[[nodiscard]] bool hasNoClosedContour(const document::PathValue& path) noexcept {
    return std::ranges::none_of(
        path.contours, [](const document::PathContour& contour) { return contour.closed; });
}

OperationResult invalidComposition(const document::CompositionId compositionId) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Composition " + std::to_string(compositionId.value()) +
                                         " does not exist");
}

OperationResult exhaustedIds(const std::string_view kinds) {
    return OperationResult::rejected(OperationIssueCode::Unsupported,
                                     "The " + std::string(kinds) + " ID space is exhausted; the project cannot create another one");
}

struct CompositionCloneIds final {
    document::CompositionId composition;
    std::unordered_map<document::NodeId, document::NodeId> nodes;
    std::unordered_map<document::EdgeId, document::EdgeId> edges;
    std::unordered_map<document::LayerId, document::LayerId> layers;
    std::unordered_map<document::LayerSlotId, document::LayerSlotId> slots;
    std::unordered_map<document::ParameterId, document::ParameterId> parameters;
    std::unordered_map<document::AnimationCurveId, document::AnimationCurveId> curves;
    std::unordered_map<document::KeyframeId, document::KeyframeId> keyframes;
    std::unordered_map<document::NodeGroupId, document::NodeGroupId> groups;
};

// Every stored key of one curve: a scalar curve's own, or every component key of a vector or
// colour curve. There is no third place a key can live.
template <typename Curve, typename Callback>
void forEachStoredKeyframe(Curve& curve, Callback&& callback) {
    using CurveType = std::remove_cvref_t<Curve>;
    if constexpr (document::kWholeValueAnimationCurve<CurveType>) {
        for (auto& keyframe : curve.keyframes)
            callback(keyframe);
    } else {
        for (auto& component : curve.components)
            for (auto& keyframe : component.keyframes)
                callback(keyframe);
    }
}

template <typename Curve> std::size_t storedKeyframeCount(const Curve& curve) {
    using CurveType = std::remove_cvref_t<Curve>;
    if constexpr (document::kWholeValueAnimationCurve<CurveType>) {
        return curve.keyframes.size();
    } else {
        std::size_t count = 0;
        for (const auto& component : curve.components)
            count += component.keyframes.size();
        return count;
    }
}

struct CloneFailure final {
    std::string message;
    bool exhausted = false;
};

[[nodiscard]] CloneFailure exhaustedClone(const std::string_view kind) {
    return {"Composition copy failed: the " + std::string(kind) +
                " ID space is exhausted; the project cannot create another one",
            true};
}

[[nodiscard]] std::optional<CompositionCloneIds>
allocateCompositionCloneIds(document::Draft& draft, const document::Composition& source,
                            CloneFailure& failure) {
    CompositionCloneIds ids;
    const auto compositionId = draft.ids().allocateComposition();
    if (!compositionId) {
        failure = exhaustedClone("composition");
        return std::nullopt;
    }
    ids.composition = *compositionId;
    for (const auto& node : source.graph().nodes()) {
        const auto id = draft.ids().allocateNode();
        if (!id) {
            failure = exhaustedClone("node");
            return std::nullopt;
        }
        ids.nodes.emplace(node.id, *id);
    }
    for (const auto& edge : source.graph().edges()) {
        const auto id = draft.ids().allocateEdge();
        if (!id) {
            failure = exhaustedClone("edge");
            return std::nullopt;
        }
        ids.edges.emplace(edge.id, *id);
    }
    for (const auto& boundary : source.graph().layerOutputs()) {
        const auto id = draft.ids().allocateLayer();
        if (!id) {
            failure = exhaustedClone("layer");
            return std::nullopt;
        }
        ids.layers.emplace(boundary.layerId, *id);
    }
    for (const auto& stack : source.graph().merges())
        for (const auto& entry : stack.entries()) {
            const auto id = draft.ids().allocateLayerSlot();
            if (!id) {
                failure = exhaustedClone("layer slot");
                return std::nullopt;
            }
            ids.slots.emplace(entry.slotId, *id);
        }
    for (const auto& parameter : source.parameters().records()) {
        const auto id = draft.ids().allocateParameter();
        if (!id) {
            failure = exhaustedClone("parameter");
            return std::nullopt;
        }
        ids.parameters.emplace(parameter.id, *id);
    }
    std::size_t keyframeCount = 0;
    bool keyframesExhausted = false;
    std::size_t repeatedKeyframes = 0;
    for (const auto& record : source.animationCurves().records())
        keyframeCount +=
            std::visit([](const auto& curve) { return storedKeyframeCount(curve); }, record);
    // keyframeCount spans every curve, so the allocation total is verified once after all of them.
    for (const auto& record : source.animationCurves().records()) {
        const auto curveId = draft.ids().allocateAnimationCurve();
        if (!curveId) {
            failure = exhaustedClone("animation curve");
            return std::nullopt;
        }
        ids.curves.emplace(document::animationCurveId(record), *curveId);
        std::visit(
            [&](auto& curve) {
                forEachStoredKeyframe(curve, [&](const auto& keyframe) {
                    const auto keyframeId = draft.ids().allocateKeyframe();
                    if (!keyframeId) {
                        keyframesExhausted = true;
                        return;
                    }
                    if (!ids.keyframes.emplace(keyframe.id, *keyframeId).second)
                        ++repeatedKeyframes;
                });
            },
            record);
    }
    if (keyframesExhausted) {
        failure = exhaustedClone("keyframe");
        return std::nullopt;
    }
    if (ids.keyframes.size() != keyframeCount) {
        failure = {"Composition copy failed: " + std::to_string(repeatedKeyframes) +
                       " key(s) of the source share a keyframe ID with another key, so the copy "
                       "cannot give every key its own ID",
                   false};
        return std::nullopt;
    }
    for (const auto& [groupId, unused] : source.nodeGroups()) {
        static_cast<void>(unused);
        const auto id = draft.ids().allocateNodeGroup();
        if (!id) {
            failure = exhaustedClone("node group");
            return std::nullopt;
        }
        ids.groups.emplace(groupId, *id);
    }
    return ids;
}

template <typename Id>
[[nodiscard]] Id remap(const std::unordered_map<Id, Id>& mapping, const Id id) {
    return mapping.at(id);
}

// Appends "<kind>.<old id>" -> new id for every object the copy holds, so a client can address the
// copy's objects without reading it back. Masks are keyed by the source mask's id.
template <typename Id>
void appendIdMap(std::vector<OperationOutput>& outputs, const std::string_view kind,
                 const std::unordered_map<Id, Id>& mapping) {
    std::vector<std::pair<Id, Id>> ordered(mapping.begin(), mapping.end());
    std::ranges::sort(ordered, {}, [](const auto& entry) { return entry.first.value(); });
    for (const auto& [from, to] : ordered)
        outputs.push_back({std::string(kind) + "." + std::to_string(from.value()), to});
}

[[nodiscard]] std::optional<document::Composition>
cloneComposition(document::Draft& draft, const document::Composition& source,
                 const std::string& name, CloneFailure& failure,
                 std::vector<OperationOutput>* idMaps = nullptr) {
    const auto ids = allocateCompositionCloneIds(draft, source, failure);
    if (!ids) {
        return std::nullopt;
    }
    std::unordered_map<document::LayerMaskId, document::LayerMaskId> maskIds;

    const auto newLayerStackId = source.graph().merges().empty()
                                     ? document::NodeId{}
                                     : remap(ids->nodes, source.graph().merges().front().nodeId());
    document::CanonicalGraph graph(newLayerStackId);
    for (const auto& sourceNode : source.graph().nodes()) {
        auto node = sourceNode;
        node.id = remap(ids->nodes, sourceNode.id);
        for (auto& binding : node.parameters) {
            binding.parameterId = remap(ids->parameters, binding.parameterId);
        }
        if (!graph.addNode(std::move(node))) {
            failure.message = "Composition copy failed: node " + std::to_string(sourceNode.id.value()) +
                              " could not be added to the copy";
            return std::nullopt;
        }
    }
    for (const auto& sourceBoundary : source.graph().layerOutputs()) {
        auto boundary = sourceBoundary;
        boundary.nodeId = remap(ids->nodes, sourceBoundary.nodeId);
        boundary.layerId = remap(ids->layers, sourceBoundary.layerId);
        // ADR 0028: masks take fresh ids and the cloned parameters; a matte follows its source.
        for (auto& mask : boundary.masks) {
            const auto maskId = draft.ids().allocateLayerMask();
            if (!maskId) {
                failure = exhaustedClone("layer mask");
                return std::nullopt;
            }
            maskIds.emplace(mask.id, *maskId);
            mask.id = *maskId;
            mask.path = remap(ids->parameters, mask.path);
            mask.feather = remap(ids->parameters, mask.feather);
            mask.expansion = remap(ids->parameters, mask.expansion);
            mask.opacity = remap(ids->parameters, mask.opacity);
            if (mask.matte)
                mask.matte->nodeId = remap(ids->nodes, mask.matte->nodeId);
        }
        if (boundary.trackMatte) {
            // A node matte (#383) has no source layer; a layer matte has no node.
            if (boundary.trackMatte->source.isValid())
                boundary.trackMatte->source = remap(ids->layers, boundary.trackMatte->source);
            if (boundary.trackMatte->node)
                boundary.trackMatte->node->nodeId =
                    remap(ids->nodes, boundary.trackMatte->node->nodeId);
        }
        // The Source extent names a node of this composition, resolved at compile time.
        if (auto* extent = std::get_if<document::SourceLayerExtent>(&boundary.timing.contentExtent);
            extent != nullptr && extent->sourceNodeId.isValid())
            extent->sourceNodeId = remap(ids->nodes, extent->sourceNodeId);
        if (sourceBoundary.parent)
            boundary.parent = remap(ids->layers, *sourceBoundary.parent);
        // #502: an entry layer follows its slot and owns cloned parameters.
        if (boundary.entrySlot)
            boundary.entrySlot = remap(ids->slots, *boundary.entrySlot);
        for (auto& binding : boundary.parameters)
            binding.parameterId = remap(ids->parameters, binding.parameterId);
        if (!graph.addLayerOutput(std::move(boundary))) {
            failure.message = "Composition copy failed: layer " +
                              std::to_string(sourceBoundary.layerId.value()) +
                              " could not be added to the copy";
            return std::nullopt;
        }
    }
    for (const auto& stack : source.graph().merges())
        graph.merge(remap(ids->nodes, stack.nodeId()))->setEnabled(stack.enabled());
    for (const auto& stack : source.graph().merges())
        for (const auto& sourceEntry : stack.entries()) {
            if (!graph.merge(remap(ids->nodes, stack.nodeId()))
                     ->append({remap(ids->slots, sourceEntry.slotId),
                               sourceEntry.layerId.isValid()
                                   ? remap(ids->layers, sourceEntry.layerId)
                                   : document::LayerId{}})) {
                failure.message = "Composition copy failed: stack slot " +
                                  std::to_string(sourceEntry.slotId.value()) + " of Merge node " +
                                  std::to_string(stack.nodeId().value()) +
                                  " could not be added to the copy";
                return std::nullopt;
            }
        }
    for (const auto& sourceEdge : source.graph().edges()) {
        auto edge = sourceEdge;
        edge.id = remap(ids->edges, sourceEdge.id);
        edge.source.nodeId = remap(ids->nodes, sourceEdge.source.nodeId);
        std::visit(
            [&](auto& destination) {
                using Destination = std::decay_t<decltype(destination)>;
                if constexpr (std::is_same_v<Destination, document::NodeInputRef>) {
                    destination.nodeId = remap(ids->nodes, destination.nodeId);
                } else {
                    destination.stackNodeId = remap(ids->nodes, destination.stackNodeId);
                    destination.slotId = remap(ids->slots, destination.slotId);
                }
            },
            edge.destination);
        if (!graph.addEdge(std::move(edge))) {
            failure.message = "Composition copy failed: edge " + std::to_string(sourceEdge.id.value()) +
                              " could not be added to the copy";
            return std::nullopt;
        }
    }
    if (source.graph().compositionOutput().has_value()) {
        auto output = *source.graph().compositionOutput();
        output.nodeId = remap(ids->nodes, output.nodeId);
        graph.setCompositionOutput(std::move(output));
    }

    document::Composition composition(ids->composition, name, source.duration(), std::move(graph),
                                      source.format());
    composition.setWorkArea(source.workArea());
    composition.setSafeAreas(source.safeAreas());
    composition.setBackgroundColor(source.backgroundColor());
    composition.setTimecode(source.timecode());
    composition.setWorkingColorSpaceId(source.workingColorSpaceId());
    for (const auto& sourceParameter : source.parameters().records()) {
        auto parameter = sourceParameter;
        parameter.id = remap(ids->parameters, sourceParameter.id);
        std::visit(
            [&](auto& valueSource) {
                using Source = std::decay_t<decltype(valueSource)>;
                if constexpr (std::is_same_v<Source, document::AnimationCurveSource>) {
                    valueSource.curveId = remap(ids->curves, valueSource.curveId);
                } else if constexpr (std::is_same_v<Source, document::DriverBindingSource>) {
                    valueSource.sourceNodeId = remap(ids->nodes, valueSource.sourceNodeId);
                }
            },
            parameter.source);
        if (!composition.parameters().insert(std::move(parameter))) {
            failure.message = "Composition copy failed: parameter " +
                              std::to_string(sourceParameter.id.value()) +
                              " could not be added to the copy";
            return std::nullopt;
        }
    }
    for (const auto& sourceRecord : source.animationCurves().records()) {
        auto record = sourceRecord;
        std::visit(
            [&](auto& curve) {
                curve.id = remap(ids->curves, curve.id);
                forEachStoredKeyframe(curve, [&](auto& keyframe) {
                    keyframe.id = remap(ids->keyframes, keyframe.id);
                });
            },
            record);
        if (!composition.animationCurves().insert(std::move(record))) {
            failure.message = "Composition copy failed: animation curve " +
                              std::to_string(document::animationCurveId(sourceRecord).value()) +
                              " could not be added to the copy";
            return std::nullopt;
        }
    }
    for (const auto& [nodeId, layout] : source.nodeLayout()) {
        composition.nodeLayout().emplace(remap(ids->nodes, nodeId), layout);
    }
    for (const auto& [groupId, sourceGroup] : source.nodeGroups()) {
        auto group = sourceGroup;
        group.id = remap(ids->groups, groupId);
        group.members.clear();
        for (const auto member : sourceGroup.members) {
            group.members.insert(remap(ids->nodes, member));
        }
        composition.nodeGroups().emplace(group.id, std::move(group));
    }
    if (idMaps != nullptr) {
        appendIdMap(*idMaps, "layer", ids->layers);
        appendIdMap(*idMaps, "node", ids->nodes);
        appendIdMap(*idMaps, "edge", ids->edges);
        appendIdMap(*idMaps, "parameter", ids->parameters);
        appendIdMap(*idMaps, "slot", ids->slots);
        appendIdMap(*idMaps, "mask", maskIds);
        appendIdMap(*idMaps, "curve", ids->curves);
        appendIdMap(*idMaps, "keyframe", ids->keyframes);
        appendIdMap(*idMaps, "group", ids->groups);
    }
    return composition;
}

// One parameter a source node owns: its node-local role, its global schema key, its initial value,
// and the command-result output name the caller reads its freshly allocated ID back from. A solid
// source has exactly one (color); a text source has twelve (content, size, color, typography, and
// box layout), in the order its registered definition declares them.
struct StructuredSourceParameter {
    std::string_view role;
    std::string_view schemaKey;
    document::ParameterValue value;
    std::string_view outputName;
};

struct StructuredLayerIds {
    document::NodeId sourceNodeId;
    document::NodeId layerOutputNodeId;
    document::EdgeId sourceToLayerEdgeId;
    document::EdgeId layerToStackEdgeId;
    document::LayerId layerId;
    document::LayerSlotId slotId;
    std::vector<document::ParameterId> sourceParameterIds;
    document::ParameterId positionParameterId;
    document::ParameterId anchorParameterId;
    document::ParameterId scaleParameterId;
    document::ParameterId rotationParameterId;
    document::ParameterId opacityParameterId;
    document::ParameterId blendModeParameterId;
};

struct StructuredLayerDescriptor {
    std::string_view sourceNodeType;
    std::uint32_t sourceNodeSchemaVersion;
    std::string_view sourceOutputPort;
    std::vector<StructuredSourceParameter> sourceParameters;
    bool audio = false;
};

struct StructuredLayerOutputNames {
    std::string_view layer;
    std::string_view slot;
    std::string_view sourceNode;
    std::string_view layerOutputNode;
    std::string_view positionParameter;
    std::string_view anchorParameter;
    std::string_view scaleParameter;
    std::string_view rotationParameter;
    std::string_view opacityParameter;
    std::string_view blendModeParameter;
    std::string_view sourceToLayerEdge;
    std::string_view layerToStackEdge;
};

// Allocation ORDER matches the registered Layer Output parameter order -- position, anchor, scale,
// rotation, opacity, blendMode -- so the ids a layer publishes read in the same sequence the
// properties grid shows. The transform breadth slice (task S4) inserted anchor/scale/rotation
// between position and opacity and the blend-mode slice appends after opacity, both of which shift
// the ids of a newly created layer; nothing persisted depends on a particular id value, and the
// operation still publishes every id by NAME rather than by position.
[[nodiscard]] std::optional<StructuredLayerIds>
allocateStructuredLayerIds(document::IdAllocator& allocator,
                           const std::size_t sourceParameterCount) {
    const auto sourceNodeId = allocator.allocateNode();
    const auto layerOutputNodeId = allocator.allocateNode();
    const auto sourceToLayerEdgeId = allocator.allocateEdge();
    const auto layerToStackEdgeId = allocator.allocateEdge();
    const auto layerId = allocator.allocateLayer();
    const auto slotId = allocator.allocateLayerSlot();
    std::vector<document::ParameterId> sourceParameterIds;
    sourceParameterIds.reserve(sourceParameterCount);
    bool sourceParametersAllocated = true;
    for (std::size_t index = 0; index < sourceParameterCount; ++index) {
        const auto sourceParameterId = allocator.allocateParameter();
        if (!sourceParameterId.has_value()) {
            sourceParametersAllocated = false;
            break;
        }
        sourceParameterIds.push_back(*sourceParameterId);
    }
    const auto positionParameterId = allocator.allocateParameter();
    const auto anchorParameterId = allocator.allocateParameter();
    const auto scaleParameterId = allocator.allocateParameter();
    const auto rotationParameterId = allocator.allocateParameter();
    const auto opacityParameterId = allocator.allocateParameter();
    const auto blendModeParameterId = allocator.allocateParameter();
    if (!sourceNodeId.has_value() || !layerOutputNodeId.has_value() ||
        !sourceToLayerEdgeId.has_value() || !layerToStackEdgeId.has_value() ||
        !layerId.has_value() || !slotId.has_value() || !sourceParametersAllocated ||
        !positionParameterId.has_value() || !anchorParameterId.has_value() ||
        !scaleParameterId.has_value() || !rotationParameterId.has_value() ||
        !opacityParameterId.has_value() || !blendModeParameterId.has_value()) {
        return std::nullopt;
    }
    return StructuredLayerIds{*sourceNodeId,
                              *layerOutputNodeId,
                              *sourceToLayerEdgeId,
                              *layerToStackEdgeId,
                              *layerId,
                              *slotId,
                              std::move(sourceParameterIds),
                              *positionParameterId,
                              *anchorParameterId,
                              *scaleParameterId,
                              *rotationParameterId,
                              *opacityParameterId,
                              *blendModeParameterId};
}

[[nodiscard]] OperationResult
addStructuredLayer(document::Draft& draft, document::Composition& composition,
                   const std::string& name, StructuredLayerDescriptor descriptor,
                   const document::Vec2d position, const double opacity,
                   const StructuredLayerOutputNames& outputNames) {
    const auto ids = allocateStructuredLayerIds(draft.ids(), descriptor.sourceParameters.size());
    if (!ids.has_value()) {
        return exhaustedIds("node, edge, layer, layer slot and parameter");
    }

    auto& parameters = composition.parameters();
    std::vector<document::ParameterBinding> sourceBindings;
    sourceBindings.reserve(descriptor.sourceParameters.size());
    for (std::size_t index = 0; index < descriptor.sourceParameters.size(); ++index) {
        auto& sourceParameter = descriptor.sourceParameters[index];
        const auto parameterId = ids->sourceParameterIds[index];
        if (!parameters.insert({parameterId, std::string(sourceParameter.schemaKey),
                                document::ConstantValueSource{std::move(sourceParameter.value)}})) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer parameters could not be inserted");
        }
        sourceBindings.push_back({std::string(sourceParameter.role), parameterId});
    }
    // The three transform breadth parameters and the blend mode are always created at their schema
    // defaults -- the identity transform and Normal blending -- so "add a layer" means exactly what
    // it meant before task S4 and the caller needs no new arguments. Anchor, scale, rotation, and
    // the blend mode are authored afterwards like any other parameter, through
    // SetParameterConstant.
    if (!parameters.insert({ids->positionParameterId,
                            std::string(document::kPositionParameterSchemaKey),
                            document::ConstantValueSource{position}}) ||
        !parameters.insert({ids->anchorParameterId,
                            std::string(document::kAnchorParameterSchemaKey),
                            document::ConstantValueSource{document::kDefaultAnchor}}) ||
        !parameters.insert({ids->scaleParameterId, std::string(document::kScaleParameterSchemaKey),
                            document::ConstantValueSource{document::kDefaultScale}}) ||
        !parameters.insert({ids->rotationParameterId,
                            std::string(document::kRotationParameterSchemaKey),
                            document::ConstantValueSource{document::kDefaultRotationDegrees}}) ||
        !parameters.insert({ids->opacityParameterId,
                            std::string(document::kOpacityParameterSchemaKey),
                            document::ConstantValueSource{opacity}}) ||
        !parameters.insert({ids->blendModeParameterId,
                            std::string(document::kBlendModeParameterSchemaKey),
                            document::ConstantValueSource{document::kDefaultBlendModeValue}})) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer parameters could not be inserted");
    }

    auto& graph = composition.graph();
    document::NodeRecord sourceNode{
        ids->sourceNodeId,
        std::string(descriptor.sourceNodeType),
        std::move(sourceBindings),
        descriptor.sourceNodeSchemaVersion,
    };
    document::NodeRecord layerOutputNode{
        ids->layerOutputNodeId,
        std::string(document::kLayerOutputNodeType),
        {
            {std::string(document::kPositionParameterRole), ids->positionParameterId},
            {std::string(document::kAnchorParameterRole), ids->anchorParameterId},
            {std::string(document::kScaleParameterRole), ids->scaleParameterId},
            {std::string(document::kRotationParameterRole), ids->rotationParameterId},
            {std::string(document::kOpacityParameterRole), ids->opacityParameterId},
            {std::string(document::kBlendModeParameterRole), ids->blendModeParameterId},
        },
        document::kLayerOutputNodeSchemaVersion,
    };
    document::LayerOutputBoundary layerBoundary{
        ids->layerOutputNodeId,
        ids->layerId,
        name,
        std::string(document::kLayerOutputOutputPort),
    };
    document::EdgeRecord sourceToLayerEdge{
        ids->sourceToLayerEdgeId,
        {ids->sourceNodeId, std::string(descriptor.sourceOutputPort)},
        document::NodeInputRef{ids->layerOutputNodeId,
                               std::string(document::kLayerOutputContentInputPort)},
    };
    document::EdgeRecord layerToStackEdge{
        ids->layerToStackEdgeId,
        {ids->layerOutputNodeId, std::string(document::kLayerOutputOutputPort)},
        document::LayerStackInputRef{graph.layerStack().nodeId(), ids->slotId,
                                     std::string(document::kLayerStackContentInputRole)},
    };
    // Where the new layer LANDS in the stack: on top, which is entry ZERO. The evaluator folds the
    // stack from its last entry to its first, so entry zero is the topmost layer and the timeline
    // lists it first. Appending put every new layer underneath every existing one instead -- so an
    // artist who added a layer and changed its blend mode saw nothing change, because the layer
    // they had just made was beneath an opaque one and had only the transparent backdrop under
    // itself, where every mode folds to Normal. That was the substance of the "blend modes are not
    // working" report, and every comparable tool adds a layer on top.
    const auto existingTop = graph.layerStack().entries().empty()
                                 ? std::nullopt
                                 : std::optional(graph.layerStack().entries().front().slotId);
    if (!graph.addNode(std::move(sourceNode)) || !graph.addNode(std::move(layerOutputNode)) ||
        !graph.addLayerOutput(std::move(layerBoundary)) ||
        !graph.layerStack().append({ids->slotId, ids->layerId}) ||
        !graph.layerStack().moveBefore(ids->slotId, existingTop) ||
        !graph.addEdge(std::move(sourceToLayerEdge)) ||
        !graph.addEdge(std::move(layerToStackEdge))) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer topology could not be inserted");
    }

    if (descriptor.audio) {
        const auto sourceAudio = draft.ids().allocateEdge(),
                   layerAudio = draft.ids().allocateEdge();
        if (!sourceAudio || !layerAudio ||
            !graph.addEdge({*sourceAudio,
                            {ids->sourceNodeId, "audio"},
                            document::NodeInputRef{ids->layerOutputNodeId, "audio"}}) ||
            !graph.addEdge(
                {*layerAudio,
                 {ids->layerOutputNodeId, "audio"},
                 document::LayerStackInputRef{graph.layerStack().nodeId(), ids->slotId, "audio"}}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Video audio edges could not be inserted");
        const auto output = graph.compositionOutput();
        if (output && std::ranges::none_of(graph.edges(), [&](const auto& edge) {
                const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                return input && input->nodeId == output->nodeId && input->port == "audio";
            })) {
            const auto edge = draft.ids().allocateEdge();
            if (!edge || !graph.addEdge({*edge,
                                         {graph.layerStack().nodeId(), "audio"},
                                         document::NodeInputRef{output->nodeId, "audio"}}))
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "Video audio output could not be connected");
        }
    }

    // Structured layer creation owns two new cards: the Layer Output goes in the layer column at
    // its stack position (above the layer under it) and its source on the same row to the left,
    // each moved off any card it would touch. The existing layout is never rewritten.
    placeLayerCards(composition, ids->layerOutputNodeId, ids->sourceNodeId);
    std::vector<OperationOutput> outputs{
        {std::string(outputNames.layer), DurableObjectId{ids->layerId}},
        {std::string(outputNames.slot), DurableObjectId{ids->slotId}},
        {std::string(outputNames.sourceNode), DurableObjectId{ids->sourceNodeId}},
        {std::string(outputNames.layerOutputNode), DurableObjectId{ids->layerOutputNodeId}},
        {std::string(outputNames.positionParameter), DurableObjectId{ids->positionParameterId}},
        {std::string(outputNames.anchorParameter), DurableObjectId{ids->anchorParameterId}},
        {std::string(outputNames.scaleParameter), DurableObjectId{ids->scaleParameterId}},
        {std::string(outputNames.rotationParameter), DurableObjectId{ids->rotationParameterId}},
        {std::string(outputNames.opacityParameter), DurableObjectId{ids->opacityParameterId}},
        {std::string(outputNames.blendModeParameter), DurableObjectId{ids->blendModeParameterId}},
        {std::string(outputNames.sourceToLayerEdge), DurableObjectId{ids->sourceToLayerEdgeId}},
        {std::string(outputNames.layerToStackEdge), DurableObjectId{ids->layerToStackEdgeId}},
    };
    for (std::size_t index = 0; index < descriptor.sourceParameters.size(); ++index) {
        outputs.push_back({std::string(descriptor.sourceParameters[index].outputName),
                           DurableObjectId{ids->sourceParameterIds[index]}});
    }
    return OperationResult::applied(std::move(outputs));
}

} // namespace

std::string_view AddShapeLayer::typeId() const noexcept { return "bloom.layer.add-shape"; }
OperationResult AddShapeLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    if (kind_ < document::ShapeKind::Rectangle || kind_ > document::ShapeKind::Path)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Unknown shape kind");
    if (geometry_.position &&
        (!std::isfinite(geometry_.position->x) || !std::isfinite(geometry_.position->y)))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Shape position must be finite");
    const auto definition = document::shapeDefinition();
    StructuredLayerDescriptor descriptor{document::kShapeSourceNodeType, 1, "image", {}};
    for (const auto& parameter : definition.parameters) {
        auto value = parameter.defaultValue;
        if (parameter.role == "kind")
            value = static_cast<std::int64_t>(kind_);
        // An open outline is visible by default: a white stroke where a closed shape has black.
        if ((kind_ == document::ShapeKind::Line ||
             (kind_ == document::ShapeKind::Path && geometry_.path &&
              hasNoClosedContour(*geometry_.path))) &&
            parameter.role == "strokeColor")
            value = core::Color4d{1, 1, 1, 1};
        // The style applies first, so a Line's or open path's fixed fill and stroke still win.
        if (parameter.role == "fillEnabled" && style_.fillEnabled)
            value = *style_.fillEnabled;
        if (parameter.role == "fillColor" && style_.fillColor)
            value = *style_.fillColor;
        if (parameter.role == "strokeEnabled" && style_.strokeEnabled)
            value = *style_.strokeEnabled;
        if (parameter.role == "strokeColor" && style_.strokeColor)
            value = *style_.strokeColor;
        if (parameter.role == "strokeWidth" && style_.strokeWidth)
            value = *style_.strokeWidth;
        const bool openOutline = kind_ == document::ShapeKind::Line ||
                                 (kind_ == document::ShapeKind::Path && geometry_.path &&
                                  hasNoClosedContour(*geometry_.path));
        if (openOutline && parameter.role == "fillEnabled")
            value = false;
        if (openOutline && parameter.role == "strokeEnabled")
            value = true;
        if (parameter.role == "size" && geometry_.size)
            value = *geometry_.size;
        if (parameter.role == "lineStart" && geometry_.lineStart)
            value = *geometry_.lineStart;
        if (parameter.role == "lineEnd" && geometry_.lineEnd)
            value = *geometry_.lineEnd;
        if (parameter.role == "path" && geometry_.path)
            value = *geometry_.path;
        if (!document::shapeConstantMatchesSchema(parameter.schemaKey, value))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Invalid shape geometry");
        descriptor.sourceParameters.push_back(
            {parameter.role, parameter.schemaKey, std::move(value), parameter.role});
    }
    return addStructuredLayer(
        draft, *composition, std::string(document::shapeKindName(kind_)), descriptor,
        geometry_.position.value_or(
            document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                            static_cast<double>(composition->format().height()) / 2.0}),
        1.0,
        {"layer", "slot", "shapeNode", "layerOutputNode", "positionParameter", "anchorParameter",
         "scaleParameter", "rotationParameter", "opacityParameter", "blendModeParameter",
         "shapeToLayerEdge", "layerToStackEdge"});
}

std::string_view AddSolidLayer::typeId() const noexcept { return "bloom.layer.add-solid"; }

OperationResult AddSolidLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (name_.empty()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Solid layer name must not be empty");
    }
    if (!color_.isValid()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Solid layer color must be finite with alpha between zero "
                                         "and one");
    }
    if (!std::isfinite(position_.x) || !std::isfinite(position_.y)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Solid layer position must be finite");
    }
    if (!std::isfinite(opacity_) || opacity_ < 0.0 || opacity_ > 1.0) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Solid layer opacity must be between zero and one");
    }

    return addStructuredLayer(
        draft, *composition, name_,
        {document::kSolidSourceNodeType,
         document::kSolidSourceNodeSchemaVersion,
         document::kSolidSourceOutputPort,
         {{document::kSolidColorParameterRole, document::kSolidColorParameterSchemaKey, color_,
           kAddSolidLayerColorParameterOutput},
          {document::kSolidWidthParameterRole, document::kSolidWidthParameterSchemaKey,
           static_cast<double>(composition->format().width()), "widthParameter"},
          {document::kSolidHeightParameterRole, document::kSolidHeightParameterSchemaKey,
           static_cast<double>(composition->format().height()), "heightParameter"}}},
        useCompositionCentre_
            ? document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                              static_cast<double>(composition->format().height()) / 2.0}
            : position_,
        opacity_,
        {kAddSolidLayerLayerOutput, kAddSolidLayerSlotOutput, kAddSolidLayerSolidNodeOutput,
         kAddSolidLayerLayerOutputNodeOutput, kAddSolidLayerPositionParameterOutput,
         kAddSolidLayerAnchorParameterOutput, kAddSolidLayerScaleParameterOutput,
         kAddSolidLayerRotationParameterOutput, kAddSolidLayerOpacityParameterOutput,
         kAddSolidLayerBlendModeParameterOutput, kAddSolidLayerSolidToLayerEdgeOutput,
         kAddSolidLayerLayerToStackEdgeOutput});
}

OperationResult AddImageLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    const auto* asset = draft.project().findAsset(asset_);
    if (!composition)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Composition " + std::to_string(composition_.value()) + " does not exist");
    if (!asset)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Asset " + std::to_string(asset_.value()) +
                                             " does not exist in the project");
    // The layer shows its source's name and follows it.
    const std::string name =
        !asset->name.empty() ? asset->name
        : asset->kind == document::AssetKind::Sequence
            ? asset->manifest.pattern
            : asset->locator.path.substr(asset->locator.path.find_last_of('/') + 1);
    // #646: an imported SVG has no picture to decode; its artwork comes from the SVG import.
    if (document::isSvgSourceAsset(*asset))
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "'" + name + "' is an SVG, not footage: add it from Assets (Add to Composition), bloom.layer.add-from-assets "
            "or bloom.layer.add-svg so it is built as artwork");
    const bool video = asset->kind == document::AssetKind::Video;
    if (!video && asset->kind != document::AssetKind::Image &&
        asset->kind != document::AssetKind::Sequence)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Asset is not visual media");
    StructuredLayerDescriptor source{
        video ? "bloom.video-source" : "bloom.image-source",
        video ? 1U : 2U,
        "image",
        {{"asset", video ? "bloom.video.asset" : "bloom.image.asset",
          std::to_string(asset_.value()), "assetParameter"},
         {"startFrame", video ? "bloom.video.start-frame" : "bloom.image.start-frame",
          std::int64_t{0}, "startFrameParameter"},
         {"loopMode", video ? "bloom.video.loop-mode" : "bloom.image.loop-mode", std::int64_t{0},
          "loopModeParameter"},
         {"colorSpace", video ? "bloom.video.color-space" : "bloom.image.color-space",
          std::int64_t{0}, "colorSpaceParameter"}},
        video && asset->channels > 0};
    // `premultiply` means "the source stores straight alpha; premultiply it". A new layer inherits
    // the asset's probed association, so a premultiplied EXR is never multiplied by alpha twice.
    if (!video)
        source.sourceParameters.push_back({"premultiply", "bloom.image.premultiply",
                                           asset->interpretation.alphaAssociation !=
                                               document::AssetAlphaAssociation::Premultiplied,
                                           "premultiplyParameter"});
    // ADR 0026: an image layer starts on the first part's root layer; binding the selection makes
    // it editable in Properties.
    if (!video) {
        source.sourceParameters.push_back({document::kImagePartParameterRole,
                                           document::kImagePartParameterSchemaKey, std::string{},
                                           "partParameter"});
        source.sourceParameters.push_back({document::kImageLayerParameterRole,
                                           document::kImageLayerParameterSchemaKey, std::string{},
                                           "layerParameter"});
    }
    auto added = addStructuredLayer(draft, *composition, name, std::move(source),
                                    {static_cast<double>(composition->format().width()) / 2.0,
                                     static_cast<double>(composition->format().height()) / 2.0},
                                    1.0,
                                    {"layer", "slot", "imageNode", "layerOutputNode",
                                     "positionParameter", "anchorParameter", "scaleParameter",
                                     "rotationParameter", "opacityParameter", "blendModeParameter",
                                     "imageToLayerEdge", "layerToStackEdge"});
    if (added.status != OperationStatus::Applied)
        return added;
    std::optional<document::LayerId> layerId;
    std::optional<document::NodeId> imageNode;
    for (const auto& output : added.outputs) {
        if (output.name == "layer")
            layerId = std::get<document::LayerId>(output.id);
        if (output.name == "imageNode")
            imageNode = std::get<document::NodeId>(output.id);
    }
    auto* layer = layerId ? composition->graph().findLayer(*layerId) : nullptr;
    if (layer != nullptr)
        layer->nameFollowsSource = !asset->name.empty();
    const bool still = asset->kind == document::AssetKind::Image;
    // A still spans the composition unless the Import preference gave it a duration (#381).
    if (still && !asset->interpretation.stillDuration)
        return added;
    // ADR 0024: timed media starts at its natural length (capped by the composition), with the
    // media as its content extent, so after its last frame the layer shows nothing.
    if (layer == nullptr || !imageNode)
        return added;
    if (!still) {
        layer->timing.contentExtent = document::SourceLayerExtent{*imageNode};
        // #559: footage holds its last frame past the source end by default. The project format
        // omits None, so a saved project keeps whatever mode it stored.
        layer->timing.afterMode = document::LayerTimeMode::Hold;
    }
    const auto rate = composition->format().frameRate();
    // #381: the interpreted length (an assumed or conformed rate, and Loop).
    const auto length = document::footageDuration(*asset, rate);
    if (length && *length < composition->duration()) {
        // End on the first composition frame boundary at or after the media end.
        const auto frames = (length->numerator() * rate.numerator() +
                             length->denominator() * rate.denominator() - 1) /
                            (length->denominator() * rate.denominator());
        const auto end = core::RationalTime::create(frames * rate.denominator(), rate.numerator());
        if (end && *end > core::RationalTime{} && *end < composition->duration())
            layer->outPoint = *end;
    }
    return added;
}

std::string_view AddTextLayer::typeId() const noexcept { return "bloom.layer.add-text"; }

OperationResult AddTextLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (name_.empty()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer name must not be empty");
    }
    if (!std::isfinite(position_.x) || !std::isfinite(position_.y)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer position must be finite");
    }
    if (!std::isfinite(opacity_) || opacity_ < 0.0 || opacity_ > 1.0) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer opacity must be between zero and one");
    }

    if (!std::isfinite(size_) || size_ <= 0.0 || size_ > document::kMaximumTextSizePixels) {
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Text layer size must be finite, greater than zero, and no more than " +
                std::to_string(static_cast<std::int64_t>(document::kMaximumTextSizePixels)) +
                " pixels");
    }
    if (!color_.isValid()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer color must be finite with alpha between zero "
                                         "and one");
    }
    // Empty content is accepted deliberately: a text layer the artist has not typed into yet is a
    // real, selectable, editable layer that simply renders nothing, and refusing it would make "add
    // a text layer, then type" impossible.
    if (!core::isValidUtf8(text_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text layer content must be valid UTF-8");
    }

    if (style_.alignment && (*style_.alignment < 0 || *style_.alignment > 2))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Text alignment must be left, center or right");
    if (style_.font && !document::isValidFontId(*style_.font))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The text font must be a valid font ID");
    const auto fontText = document::encodeFontId(style_.font.value_or(document::defaultFontId()));

    return addStructuredLayer(
        draft, *composition, name_,
        {document::kTextSourceNodeType,
         document::kTextSourceNodeSchemaVersion,
         document::kTextSourceOutputPort,
         // Order matches the registered text definition's parameter order exactly.
         {{document::kTextParameterRole, document::kTextParameterSchemaKey, text_,
           kAddTextLayerTextParameterOutput},
          {document::kTextSizeParameterRole, document::kTextSizeParameterSchemaKey, size_,
           kAddTextLayerSizeParameterOutput},
          {document::kTextColorParameterRole, document::kTextColorParameterSchemaKey, color_,
           kAddTextLayerColorParameterOutput},
          {document::kTextAlignmentParameterRole, document::kTextAlignmentParameterSchemaKey,
           style_.alignment.value_or(std::int64_t{0}), "alignmentParameter"},
          {document::kTextLineHeightParameterRole, document::kTextLineHeightParameterSchemaKey, 1.0,
           "lineHeightParameter"},
          {document::kTextLetterSpacingParameterRole,
           document::kTextLetterSpacingParameterSchemaKey, 0.0, "letterSpacingParameter"},
          {document::kTextFontParameterRole, document::kTextFontParameterSchemaKey,
           fontText, kAddTextLayerFontParameterOutput},
          {document::kTextBoxParameterRole, document::kTextBoxParameterSchemaKey, document::Vec2d{},
           kAddTextLayerBoxParameterOutput},
          {document::kTextWrapParameterRole, document::kTextWrapParameterSchemaKey, false,
           kAddTextLayerWrapParameterOutput},
          {document::kTextVerticalAlignmentParameterRole,
           document::kTextVerticalAlignmentParameterSchemaKey, std::int64_t{0},
           kAddTextLayerVerticalAlignmentParameterOutput},
          {document::kTextAnchorModeParameterRole, document::kTextAnchorModeParameterSchemaKey,
           document::kTextAnchorModeFollowsAlignment, kAddTextLayerAnchorModeParameterOutput},
          {document::kTextOverflowParameterRole, document::kTextOverflowParameterSchemaKey,
           std::int64_t{0}, kAddTextLayerOverflowParameterOutput}}},
        useCompositionCentre_
            ? document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                              static_cast<double>(composition->format().height()) / 2.0}
            : position_,
        opacity_,
        {kAddTextLayerLayerOutput, kAddTextLayerSlotOutput, kAddTextLayerTextNodeOutput,
         kAddTextLayerLayerOutputNodeOutput, kAddTextLayerPositionParameterOutput,
         kAddTextLayerAnchorParameterOutput, kAddTextLayerScaleParameterOutput,
         kAddTextLayerRotationParameterOutput, kAddTextLayerOpacityParameterOutput,
         kAddTextLayerBlendModeParameterOutput, kAddTextLayerTextToLayerEdgeOutput,
         kAddTextLayerLayerToStackEdgeOutput});
}

std::string_view SetProjectName::typeId() const noexcept { return "bloom.project.set-name"; }

OperationResult SetProjectName::apply(document::Draft& draft) const {
    if (name_.empty()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Project name must not be empty");
    }
    if (draft.project().name() == name_) {
        return OperationResult::noChange();
    }
    draft.project().setName(name_);
    return OperationResult::applied();
}

std::string_view AddComposition::typeId() const noexcept { return "bloom.composition.add"; }

OperationResult AddComposition::apply(document::Draft& draft) const {
    if (!document::isValidHumanFacingName(name_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition name is invalid");
    }
    if (duration_ <= core::RationalTime{}) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition duration must be greater than zero");
    }
    if (!background_.isValid())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid composition background color");
    const auto format = document::CompositionFormat::create(format_.width(), format_.height(),
                                                            format_.pixelAspect(), frameRate_);
    if (!format) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition format is invalid");
    }

    const auto compositionId = draft.ids().allocateComposition();
    const auto layerStackNodeId = draft.ids().allocateNode();
    const auto outputNodeId = draft.ids().allocateNode();
    const auto outputEdgeId = draft.ids().allocateEdge();
    if (!compositionId || !layerStackNodeId || !outputNodeId || !outputEdgeId) {
        return exhaustedIds("composition, node and edge");
    }

    document::CanonicalGraph graph(*layerStackNodeId);
    if (!graph.addNode({*layerStackNodeId,
                        std::string(document::kLayerStackNodeType),
                        {},
                        document::kLayerStackNodeSchemaVersion}) ||
        !graph.addNode({*outputNodeId,
                        std::string(document::kCompositionOutputNodeType),
                        {},
                        document::kCompositionOutputNodeSchemaVersion}) ||
        !graph.addEdge({*outputEdgeId,
                        {*layerStackNodeId, std::string(document::kLayerStackOutputPort)},
                        document::NodeInputRef{
                            *outputNodeId, std::string(document::kCompositionOutputInputPort)}})) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition topology could not be created");
    }
    graph.setCompositionOutput(
        {*outputNodeId, std::string(document::kCompositionOutputOutputPort)});

    if (!draft.project().addComposition(
            document::Composition(*compositionId, name_, duration_, std::move(graph), *format))) {
        return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                         "Composition could not be added");
    }
    draft.project().findComposition(*compositionId)->setBackgroundColor(background_);
    return OperationResult::applied({{std::string(kAddCompositionOutput), *compositionId}});
}

std::string_view DeleteComposition::typeId() const noexcept { return "bloom.composition.delete"; }

OperationResult DeleteComposition::apply(document::Draft& draft) const {
    if (draft.project().findComposition(compositionId_) == nullptr) {
        return invalidComposition(compositionId_);
    }
    // A project may legitimately own no composition: a blank new project already does, and deleting
    // the final composition must reach that same usable empty state as one undoable command. There
    // is therefore no last-composition guard here.
    if (!draft.project().removeComposition(compositionId_)) {
        return invalidComposition(compositionId_);
    }
    return OperationResult::applied();
}

std::string_view DuplicateComposition::typeId() const noexcept {
    return "bloom.composition.duplicate";
}

OperationResult DuplicateComposition::apply(document::Draft& draft) const {
    const auto* source = draft.project().findComposition(compositionId_);
    if (source == nullptr) {
        return invalidComposition(compositionId_);
    }
    const std::string name = source->name() + " copy";
    if (!document::isValidHumanFacingName(name)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Duplicated composition name is invalid");
    }
    CloneFailure failure;
    std::vector<OperationOutput> outputs;
    auto copy = cloneComposition(draft, *source, name, failure, &outputs);
    if (!copy) {
        return OperationResult::rejected(failure.exhausted ? OperationIssueCode::Unsupported
                                                           : OperationIssueCode::InvalidValue,
                                         failure.message);
    }
    const auto copyId = copy->id();
    if (!draft.project().addComposition(std::move(*copy))) {
        return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                         "Composition " + std::to_string(compositionId_.value()) +
                                             " could not be duplicated: composition ID " +
                                             std::to_string(copyId.value()) +
                                             " or the name \"" + name + "\" is already taken");
    }
    outputs.insert(outputs.begin(), {std::string(kDuplicateCompositionOutput), copyId});
    return OperationResult::applied(std::move(outputs));
}

std::string_view SetCompositionName::typeId() const noexcept {
    return "bloom.composition.set-name";
}

OperationResult SetCompositionName::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (name_.empty()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition name must not be empty");
    }
    if (composition->name() == name_) {
        return OperationResult::noChange();
    }
    composition->setName(name_);
    // Layers nesting this composition that still follow their source show the new name.
    (void)document::refreshLayerNamesFromSources(draft.project());
    return OperationResult::applied();
}

std::string_view SetCompositionDuration::typeId() const noexcept {
    return "bloom.composition.set-duration";
}

OperationResult SetCompositionDuration::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (composition->duration() == duration_) {
        return OperationResult::noChange();
    }
    if (!composition->setDuration(duration_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition duration must be greater than zero");
    }
    return OperationResult::applied();
}

std::string_view SetCompositionFormat::typeId() const noexcept {
    return "bloom.composition.set-format";
}

OperationResult SetCompositionFormat::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (composition->format() == format_) {
        return OperationResult::noChange();
    }
    composition->setFormat(format_);
    return OperationResult::applied();
}

std::string_view SetCompositionSafeAreas::typeId() const noexcept {
    return "bloom.composition.set-safe-areas";
}

OperationResult SetCompositionSafeAreas::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (!settings_.isValid()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Composition safe areas are invalid");
    }
    if (composition->safeAreas() == settings_) {
        return OperationResult::noChange();
    }
    composition->setSafeAreas(settings_);
    return OperationResult::applied();
}

std::string_view MoveLayerBefore::typeId() const noexcept { return "bloom.layer.move-before"; }

OperationResult MoveLayerBefore::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }

    // #502: the slot moves within whichever Merge holds it (a nested Merge's entries too).
    const auto holder = document::mergeEntryOfSlot(std::as_const(*composition).graph(), slotId_);
    if (!holder) {
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Layer slot " + std::to_string(slotId_.value()) +
                                             " does not exist");
    }
    auto& stack = *composition->graph().merge(holder->merge);
    const auto entries = stack.entries();
    const auto moving = std::find_if(entries.begin(), entries.end(),
                                     [this](const auto& entry) { return entry.slotId == slotId_; });
    if (const auto* layer =
            document::mergeEntryLayer(std::as_const(*composition).graph(), holder->merge, slotId_);
        layer && layer->locked)
        return detail::lockedLayer(*layer);
    if (beforeSlotId_ == slotId_) {
        return OperationResult::noChange();
    }

    if (beforeSlotId_.has_value()) {
        const auto before = std::find_if(entries.begin(), entries.end(), [this](const auto& entry) {
            return entry.slotId == *beforeSlotId_;
        });
        if (before == entries.end()) {
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "Destination layer slot " +
                                                 std::to_string(beforeSlotId_->value()) +
                                                 " does not exist");
        }
        if (std::next(moving) == before) {
            return OperationResult::noChange();
        }
    } else if (std::next(moving) == entries.end()) {
        return OperationResult::noChange();
    }

    if (!stack.moveBefore(slotId_, beforeSlotId_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                         "Layer move could not be applied");
    }
    return OperationResult::applied();
}

} // namespace bloom::commands
