#include <bloom/runtime/compiled_plan_cache.hpp>
#include <bloom/runtime/font_resolver.hpp>
#include <bloom/runtime/function_body.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include "channel_static.hpp"
#include "compositing_recipes.hpp"
#include "depth_effect_recipes.hpp"
#include "motion_blur_recipes.hpp"
#include "codec_degrade_recipes.hpp"
#include "auto_exposure_recipes.hpp"
#include "histogram_recipes.hpp"
#include "statistics_recipes.hpp"
#include <bloom/runtime/audio_analysis_source.hpp>
#include "motion_shutter.hpp"
#include "plan_time_scopes.hpp"
#include "snapshot_compiler_support.hpp"
#include <bloom/runtime/layer_time_projection.hpp>

#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/sharpen_nodes.hpp>
#include <bloom/document/codec_degrade_nodes.hpp>
#include <bloom/document/auto_exposure_nodes.hpp>
#include <bloom/document/audio_effect_nodes.hpp>
#include <bloom/document/lens_distortion_nodes.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/document/rolling_shutter_nodes.hpp>
#include <bloom/document/chromatic_aberration_nodes.hpp>
#include <bloom/document/test_card_nodes.hpp>
#include <bloom/document/light_rays_nodes.hpp>
#include <bloom/document/field_nodes.hpp>
#include <bloom/core/field_program.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/evaluation_inputs.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/image_channel_nodes.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/runtime/curve_compilation.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <new>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace bloom;
using runtime::detail::destinationNode;
using runtime::detail::findParameterBinding;
using runtime::detail::hasValueKind;

using FixedInputKey = std::pair<document::NodeId, std::string>;
// Where one value node's named output landed in the plan's flat value-output table. Keyed exactly
// like FixedInputKey above, and a std::map for the same reason: a NodeId orders, so the table is
// deterministic without a hash of a pair.
using ValueOutputKey = std::pair<document::NodeId, std::string>;

// #590: the value program one function body is lowered into, beside the plan's main one.
struct FunctionBodyBuild final {
    std::vector<runtime::CompiledValueOperation> operations;
    std::map<ValueOutputKey, runtime::ValueOutputIndex> outputs;
    std::size_t outputCount = 0;
    std::vector<std::uint8_t> postImage;
};
using LayerSlotInputKey = std::tuple<document::NodeId, document::LayerSlotId, std::string>;
using DiagnosticKey = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                 std::uint64_t, std::uint64_t, std::string, int>;

struct CompiledCurveTables final {
    std::vector<runtime::CompiledScalarCurve> scalar;
    std::vector<runtime::CompiledVec2Curve> vec2;
    std::vector<runtime::CompiledVec3Curve> vec3;
    std::vector<runtime::CompiledColor4Curve> color4;
};

[[nodiscard]] DiagnosticKey diagnosticKey(const runtime::CompileDiagnostic& diagnostic) {
    const auto value = [](const auto& id) {
        return id.has_value() ? id->value() : std::uint64_t{0};
    };
    return {diagnostic.subject.compositionId.value(),
            value(diagnostic.subject.nodeId),
            value(diagnostic.subject.edgeId),
            value(diagnostic.subject.parameterId),
            value(diagnostic.subject.layerId),
            value(diagnostic.subject.layerSlotId),
            diagnostic.subject.field,
            static_cast<int>(diagnostic.code)};
}

class CompilePass final {
  public:
    CompilePass(const runtime::NodeDefinitionRegistry& registry,
                const runtime::SnapshotCompileRequest& request,
                const runtime::CancellationToken& cancellation,
                runtime::detail::CompileCheckpointObserver* checkpointObserver,
                runtime::CompiledPlanCache* planCache = nullptr)
        : registry_(registry), request_(request), cancellation_(cancellation),
          checkpointObserver_(checkpointObserver), planCache_(planCache) {}

    [[nodiscard]] runtime::SnapshotCompileResult run() {
        if (cancelled()) {
            return result(runtime::SnapshotCompileStatus::Cancelled);
        }
        if (!registry_.isFrozen()) {
            addFailure(runtime::CompileDiagnosticCode::RegistryNotFrozen, {},
                       "Node definition registry is not frozen",
                       "Compilation requires an immutable startup registry.");
            return finishWithoutPlan();
        }

        if (!request_.snapshot.project().validateCompositionNesting().ok()) {
            addFailure(runtime::CompileDiagnosticCode::CompositionNestingCycle, {},
                       "Composition nesting cycle",
                       "Composition sources and Node Groups must form an acyclic graph.");
            return finishWithoutPlan();
        }
        for (const auto& composition : request_.snapshot.project().compositions()) {
            if (cancelled()) {
                return result(runtime::SnapshotCompileStatus::Cancelled);
            }
            if (composition.id() == request_.compositionId) {
                composition_ = &composition;
                break;
            }
        }
        if (composition_ == nullptr) {
            addFailure(runtime::CompileDiagnosticCode::CompositionNotFound, {},
                       "Composition was not found",
                       "The requested composition does not exist in this document snapshot.");
            return finishWithoutPlan();
        }
        // #342: every Node Group is replaced by the nodes of the group it names, so the rest of
        // the pass (and every route that reads the plan) sees ordinary nodes.
        expansion_ = document::expandNodeGroups(request_.snapshot.project(), *composition_);
        if (expansion_.composition.has_value())
            composition_ = &*expansion_.composition;
        for (const auto& [consumer, body] : expansion_.functionBodies)
            for (const auto id : body.nodes)
                bodyOwner_.emplace(id, consumer);
        if (!indexSourceGraph() || !collectReachableGraph() || !indexReachableGraph() ||
            cancelled()) {
            return cancelled() ? result(runtime::SnapshotCompileStatus::Cancelled)
                               : finishWithoutPlan();
        }

        resolveDefinitions();
        validateCompositionOutput();
        validateEdges();
        validateInputs();
        validateParameters();
        validateParameterOverride();
        if (cancelled()) {
            return result(runtime::SnapshotCompileStatus::Cancelled);
        }
        if (hasFailure_ || hasUnsupported_) {
            return finishWithoutPlan();
        }

        const auto topologicalOrder = makeTopologicalOrder();
        if (!topologicalOrder.has_value()) {
            return finishWithoutPlan();
        }
        if (!validateBoundsReadoutRule(*topologicalOrder)) {
            return finishWithoutPlan();
        }
        auto plan = lower(*topologicalOrder);
        if (cancelled()) {
            return result(runtime::SnapshotCompileStatus::Cancelled);
        }
        if (!plan) {
            return finishWithoutPlan();
        }

        auto diagnostics = takeDiagnostics();
        if (cancelled()) {
            return {
                .status = runtime::SnapshotCompileStatus::Cancelled, .plan = {}, .diagnostics = {}};
        }
        return {.status = runtime::SnapshotCompileStatus::Compiled,
                .plan = std::move(plan),
                .diagnostics = std::move(diagnostics)};
    }

  private:
    [[nodiscard]] bool indexSourceGraph() {
        for (const auto& node : composition_->graph().nodes()) {
            if (cancelled()) {
                return false;
            }
            nodes_.emplace(node.id, &node);
        }
        for (const auto& parameter : composition_->parameters().records()) {
            if (cancelled()) {
                return false;
            }
            parameters_.emplace(parameter.id, &parameter);
        }
        for (const auto& boundary : composition_->graph().layerOutputs()) {
            if (cancelled()) {
                return false;
            }
            anySolo_ = anySolo_ || boundary.solo;
            if (boundary.trackMatte && !boundary.trackMatte->node)
                matteSources_.insert(boundary.trackMatte->source);
            // #502: a Merge entry's own layer has no node of its own; its nodeId is the Merge's.
            if (!boundary.entryOwned())
                layerOutputs_.emplace(boundary.nodeId, &boundary);
            if (boundary.parent) {
                if (const auto* parent = composition_->graph().findLayer(*boundary.parent))
                    parent->entryOwned()
                        ? static_cast<void>(entryLayerParents_.insert(parent->layerId))
                        : static_cast<void>(transformParents_.insert(parent->nodeId));
            }
        }
        for (const auto& node : composition_->graph().nodes())
            if (node.typeId == document::kAdjustmentNodeType)
                hasAdjustments_ = true;
        return true;
    }

    [[nodiscard]] const document::NodeRecord*
    findNode(const document::NodeId nodeId) const noexcept {
        const auto node = nodes_.find(nodeId);
        return node == nodes_.end() ? nullptr : node->second;
    }

    [[nodiscard]] const document::ParameterRecord*
    findParameter(const document::ParameterId parameterId) const noexcept {
        if (const auto override = overrides_.find(parameterId); override != overrides_.end())
            return &override->second;
        const auto parameter = parameters_.find(parameterId);
        return parameter == parameters_.end() ? nullptr : parameter->second;
    }

    [[nodiscard]] bool cancelled(const runtime::detail::CompileCheckpointPhase phase =
                                     runtime::detail::CompileCheckpointPhase::General) const {
        if (checkpointObserver_ != nullptr) {
            checkpointObserver_->checkpoint(phase);
        }
        return cancellation_.isCancellationRequested();
    }

    [[nodiscard]] runtime::CompileSubject subject(const document::NodeId nodeId = {},
                                                  std::string field = {}) const {
        return {.compositionId = request_.compositionId,
                .nodeId = nodeId.isValid() ? std::optional(nodeId) : std::nullopt,
                .edgeId = std::nullopt,
                .parameterId = std::nullopt,
                .layerId = std::nullopt,
                .layerSlotId = std::nullopt,
                .field = std::move(field)};
    }

    void addFailure(const runtime::CompileDiagnosticCode code,
                    runtime::CompileSubject diagnosticSubject, std::string summary,
                    std::string detail) {
        diagnosticSubject.compositionId = request_.compositionId;
        runtime::CompileDiagnostic diagnostic{code, runtime::DiagnosticSeverity::Error,
                                              std::move(diagnosticSubject), std::move(summary),
                                              std::move(detail)};
        diagnostics_.emplace(diagnosticKey(diagnostic), std::move(diagnostic));
        hasFailure_ = true;
    }

    void addUnsupported(const runtime::CompileDiagnosticCode code,
                        runtime::CompileSubject diagnosticSubject, std::string summary,
                        std::string detail) {
        diagnosticSubject.compositionId = request_.compositionId;
        runtime::CompileDiagnostic diagnostic{code, runtime::DiagnosticSeverity::Error,
                                              std::move(diagnosticSubject), std::move(summary),
                                              std::move(detail)};
        diagnostics_.emplace(diagnosticKey(diagnostic), std::move(diagnostic));
        hasUnsupported_ = true;
    }

    void addWarning(const runtime::CompileDiagnosticCode code,
                    runtime::CompileSubject diagnosticSubject, std::string summary,
                    std::string detail) {
        diagnosticSubject.compositionId = request_.compositionId;
        runtime::CompileDiagnostic diagnostic{code, runtime::DiagnosticSeverity::Warning,
                                              std::move(diagnosticSubject), std::move(summary),
                                              std::move(detail)};
        // A warning is reported once per subject: a layer lowered more than once (its own slot, as
        // a parent, as a matte source, for its audio) must not repeat what it already said.
        const auto key = diagnosticKey(diagnostic);
        const auto [first, last] = diagnostics_.equal_range(key);
        if (std::any_of(first, last, [&](const auto& existing) {
                return existing.second.summary == diagnostic.summary;
            }))
            return;
        diagnostics_.emplace(key, std::move(diagnostic));
    }

#include "snapshot_compiler_mute.ipp"

    [[nodiscard]] bool collectReachableGraph() {
        const auto& graph = composition_->graph();
        if (!graph.compositionOutput().has_value()) {
            addFailure(runtime::CompileDiagnosticCode::InvalidCompositionOutput, {},
                       "Composition output is missing",
                       "A published composition must provide one explicit output endpoint.");
            return false;
        }

        const auto rootId = graph.compositionOutput()->nodeId;
        if (findNode(rootId) == nullptr) {
            addFailure(runtime::CompileDiagnosticCode::TopologyInvariant,
                       subject(rootId, "compositionOutput.nodeId"),
                       "Composition output references a missing node",
                       "The published graph violates its document topology invariant.");
            return false;
        }

        std::unordered_map<document::NodeId, std::vector<const document::EdgeRecord*>> incoming;
        for (const auto& edge : graph.edges()) {
            if (cancelled(runtime::detail::CompileCheckpointPhase::IncomingEdgeIndex)) {
                return false;
            }
            incoming[destinationNode(edge.destination)].push_back(&edge);
        }

        // A Layer Bounds node is a value-graph readback root: it can be selected in Properties
        // without driving an image parameter, so composition-output reachability alone would
        // otherwise prune the very node the artist asked to inspect. Keep the reverse driver map
        // beside the ordinary incoming-edge index so value nodes downstream of that readout are
        // retained as well. Image consumers are intentionally retained too; the later read-rule
        // pass names and refuses that feedback instead of silently pruning it.
        std::unordered_map<document::NodeId, std::vector<document::NodeId>> driverConsumers;
        std::vector<document::NodeId> readoutRoots;
        for (const auto& node : graph.nodes()) {
            if (const auto* definition = registry_.find(node.typeId, node.schemaVersion);
                definition != nullptr &&
                definition->lowering == runtime::NodeLoweringKind::ValueBoundsReadout) {
                readoutRoots.push_back(node.id);
            }
            for (const auto& reference : driverReferences(node)) {
                driverConsumers[reference.source.nodeId].push_back(node.id);
            }
        }

        // ADR 0026: a Viewer inspection plan reaches only the inspected ChannelSet; the
        // composition's own graph is not part of that viewer-only plan.
        std::vector<document::NodeId> pending{
            request_.channelInspection ? request_.channelInspection->node : rootId};
        if (!request_.channelInspection)
            pending.insert(pending.end(), readoutRoots.begin(), readoutRoots.end());
        std::unordered_set<document::NodeId> visited;
        std::map<document::NodeId, const document::NodeRecord*> reachableNodes;
        std::map<document::EdgeId, const document::EdgeRecord*> reachableEdges;
        while (!pending.empty()) {
            if (cancelled()) {
                return false;
            }
            const auto nodeId = pending.back();
            pending.pop_back();
            if (!visited.insert(nodeId).second) {
                continue;
            }
            const auto* node = findNode(nodeId);
            if (node == nullptr) {
                addFailure(runtime::CompileDiagnosticCode::TopologyInvariant,
                           subject(nodeId, "edges"), "Reachable edge references a missing node",
                           "The published graph violates its document topology invariant.");
                continue;
            }
            reachableNodes.emplace(nodeId, node);
            // #590: a function consumer reads its group's body, so the body is reached with it.
            if (const auto body = expansion_.functionBodies.find(nodeId);
                body != expansion_.functionBodies.end())
                pending.push_back(body->second.output);
            if (const auto boundary = layerOutputs_.find(nodeId); boundary != layerOutputs_.end())
                reachLayerRelations(*boundary->second, pending, reachableEdges);
            // #502: the Adjustment's output is the stack below its entry, which its target Merge
            // lowers; a reached Adjustment reaches that Merge.
            if (node->typeId == document::kAdjustmentNodeType)
                if (const auto target = singleAdjustmentTarget(nodeId))
                    pending.push_back(target->merge);
            if (const auto iterator = incoming.find(nodeId); iterator != incoming.end()) {
                for (const auto* edge : iterator->second) {
                    if (cancelled()) {
                        return false;
                    }
                    if (!consumesEdge(*node, *edge))
                        continue;
                    // #502: a consumed entry's own layer is reached with its slot.
                    if (const auto* slot =
                            std::get_if<document::LayerStackInputRef>(&edge->destination);
                        slot != nullptr && slot->role == document::kLayerStackContentInputRole)
                        if (const auto* layer = entryLayerOf(slot->stackNodeId, slot->slotId);
                            layer != nullptr && layer->entryOwned())
                            reachEntryLayer(*layer, pending, reachableEdges);
                    // A Viewer inspection plan needs only the inspected node's channel sets.
                    // The Cryptomatte preview also shows the node's input image, a pixel output
                    // inspection (#490 Image Preview) needs every input of its node, and an
                    // Image to Channel Set's channels are its input image's (#480).
                    if (request_.channelInspection && nodeId == request_.channelInspection->node &&
                        request_.channelInspection->port !=
                            document::kCryptomattePreviewInspectionPort &&
                        !inspectsPixelOutput(*node) && !isChannelSetInput(*node, *edge) &&
                        node->typeId != document::kImageToChannelSetNodeType)
                        continue;
                    reachableEdges.emplace(edge->id, edge);
                    pending.push_back(edge->source.nodeId);
                }
            }
            // A driven parameter reaches its value node the same way an input port reaches its
            // source: task S7's drivers are part of what the composition output depends on, so the
            // reachable set has to follow them or the value graph they name would never be
            // compiled.
            for (const auto& reference : driverReferences(*node)) {
                if (cancelled()) {
                    return false;
                }
                pending.push_back(reference.source.nodeId);
            }
            if (const auto consumers = driverConsumers.find(nodeId);
                consumers != driverConsumers.end()) {
                pending.insert(pending.end(), consumers->second.begin(), consumers->second.end());
            }
        }

        reachableNodes_.reserve(reachableNodes.size());
        for (const auto& [nodeId, node] : reachableNodes) {
            static_cast<void>(nodeId);
            if (cancelled()) {
                return false;
            }
            reachableNodes_.push_back(node);
        }
        reachableEdges_.reserve(reachableEdges.size());
        for (const auto& [edgeId, edge] : reachableEdges) {
            static_cast<void>(edgeId);
            if (cancelled()) {
                return false;
            }
            reachableEdges_.push_back(edge);
        }
        return !hasFailure_;
    }

    // ADR 0028, #502: a reached layer reaches its transform parent and its track matte source,
    // whatever their slots' enabled and solo state, as a Layer node always has.
    void reachLayerRelations(const document::LayerOutputBoundary& layer,
                             std::vector<document::NodeId>& pending,
                             std::map<document::EdgeId, const document::EdgeRecord*>& edges) {
        const auto& graph = composition_->graph();
        for (const auto related :
             {layer.parent, layer.trackMatte ? std::optional(layer.trackMatte->source)
                                             : std::optional<document::LayerId>{}}) {
            const auto* other = related ? graph.findLayer(*related) : nullptr;
            if (other == nullptr)
                continue;
            if (other->entryOwned())
                reachEntryLayer(*other, pending, edges);
            else
                pending.push_back(other->nodeId);
        }
        // #383: a node track matte and every matte mask's node are read by the layer.
        const auto reachMatte = [&](const document::OutputPortRef& matte) {
            pending.push_back(matte.nodeId);
            reachedMatteOutputs_.emplace(matte.nodeId, matte.port);
        };
        if (layer.trackMatte && layer.trackMatte->node)
            reachMatte(*layer.trackMatte->node);
        for (const auto& mask : layer.masks)
            if (mask.matte)
                reachMatte(*mask.matte);
    }

    // #502: an entry layer is reached with its Merge's slot, or as a reached layer's parent or
    // matte source; it then reaches its slot's image content (even a hidden or disabled slot's, as
    // a parent Layer node's content is), its Merge, its parent and its matte source.
    void reachEntryLayer(const document::LayerOutputBoundary& layer,
                         std::vector<document::NodeId>& pending,
                         std::map<document::EdgeId, const document::EdgeRecord*>& edges) {
        if (!reachedEntryLayerIds_.insert(layer.layerId).second)
            return;
        reachedEntryLayers_.push_back(&layer);
        pending.push_back(layer.nodeId);
        if (const auto* stack = composition_->graph().merge(layer.nodeId);
            stack != nullptr && stack->enabled())
            if (const auto* edge = document::mergeSlotImageEdge(composition_->graph(), layer.nodeId,
                                                                *layer.entrySlot)) {
                edges.emplace(edge->id, edge);
                pending.push_back(edge->source.nodeId);
            }
        reachLayerRelations(layer, pending, edges);
    }

    [[nodiscard]] bool indexReachableGraph() {
        for (const auto* edge : reachableEdges_) {
            if (cancelled()) {
                return false;
            }
            std::visit(
                [&](const auto& input) {
                    using Input = std::decay_t<decltype(input)>;
                    if constexpr (std::is_same_v<Input, document::NodeInputRef>) {
                        fixedInputEdges_.emplace(FixedInputKey{input.nodeId, input.port}, edge);
                    } else {
                        layerSlotInputEdges_.emplace(
                            LayerSlotInputKey{input.stackNodeId, input.slotId, input.role}, edge);
                    }
                },
                edge->destination);
        }
        return true;
    }

    // #490: an inspection with no channels of an Image or Mask output of a node other than a
    // Cryptomatte (whose Mask inspection reads only its channel set) shows that output's own
    // pixels, so the node is evaluated as the composition would evaluate it.
    [[nodiscard]] bool inspectsPixelOutput(const document::NodeRecord& node) const {
        const auto& inspection = request_.channelInspection;
        if (!inspection || !inspection->channels.empty() || inspection->node != node.id)
            return false;
        const auto* definition = registry_.find(node.typeId, node.schemaVersion);
        if (definition == nullptr || definition->lowering == runtime::NodeLoweringKind::Cryptomatte)
            return false;
        return std::ranges::any_of(definition->outputs, [&](const auto& output) {
            return output.name == inspection->port &&
                   (output.valueKind == document::SocketValueKind::Image ||
                    output.valueKind == document::SocketValueKind::Mask);
        });
    }

    [[nodiscard]] bool isChannelSetInput(const document::NodeRecord& node,
                                         const document::EdgeRecord& edge) const {
        const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
        const auto* definition = registry_.find(node.typeId, node.schemaVersion);
        if (input == nullptr || definition == nullptr)
            return false;
        return std::ranges::any_of(definition->inputs, [&](const auto& declared) {
            return declared.name == input->port &&
                   declared.valueKind == document::SocketValueKind::ChannelSet;
        });
    }

    [[nodiscard]] const document::EdgeRecord* fixedInputEdge(const document::NodeId nodeId,
                                                             const std::string_view port) const {
        const auto edge = fixedInputEdges_.find(FixedInputKey{nodeId, std::string(port)});
        return edge == fixedInputEdges_.end() ? nullptr : edge->second;
    }

    [[nodiscard]] const document::EdgeRecord*
    layerSlotInputEdge(const document::NodeId nodeId, const document::LayerSlotId slotId,
                       const std::string_view role) const {
        const auto edge =
            layerSlotInputEdges_.find(LayerSlotInputKey{nodeId, slotId, std::string(role)});
        return edge == layerSlotInputEdges_.end() ? nullptr : edge->second;
    }

    [[nodiscard]] const runtime::InputPortDefinition*
    findInput(const runtime::NodeDefinition& definition, const std::string_view name) const {
        for (const auto& input : definition.inputs) {
            if (cancelled()) {
                return nullptr;
            }
            if (input.name == name) {
                return &input;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const runtime::OutputPortDefinition*
    findOutput(const runtime::NodeDefinition& definition, const std::string_view name) const {
        for (const auto& output : definition.outputs) {
            if (cancelled()) {
                return nullptr;
            }
            if (output.name == name) {
                return &output;
            }
        }
        return nullptr;
    }

    void resolveDefinitions() {
        for (const auto* node : reachableNodes_) {
            if (cancelled(runtime::detail::CompileCheckpointPhase::DefinitionResolution)) {
                return;
            }
            const auto* definition = registry_.find(node->typeId, node->schemaVersion);
            if (cancelled(runtime::detail::CompileCheckpointPhase::DefinitionResolution)) {
                return;
            }
            if (definition == nullptr) {
                if (registry_.containsType(node->typeId)) {
                    addUnsupported(runtime::CompileDiagnosticCode::UnsupportedNodeVersion,
                                   subject(node->id, "schemaVersion"),
                                   "Node schema version is unsupported",
                                   "No registered evaluator definition matches this node version.");
                } else {
                    addUnsupported(runtime::CompileDiagnosticCode::UnknownNodeType,
                                   subject(node->id, "typeId"), "Node type is unavailable",
                                   "No registered node definition matches this node type.");
                }
                continue;
            }
            definitions_.emplace(node->id, definition);
            if (definition->lowering == runtime::NodeLoweringKind::NodeGroup ||
                (definition->lowering == runtime::NodeLoweringKind::ValueFunction &&
                 !expansion_.functionBodies.contains(node->id))) {
                const auto reason = expansion_.unresolved.find(node->id);
                const bool function =
                    definition->lowering == runtime::NodeLoweringKind::ValueFunction;
                addFailure(runtime::CompileDiagnosticCode::TopologyInvariant,
                           subject(node->id, "parameter.group"),
                           function ? "Function cannot read its group" : "Node Group cannot be expanded",
                           reason == expansion_.unresolved.end()
                               ? "The group this node names could not be expanded."
                               : reason->second + ".");
            }
            if (definition->lowering == runtime::NodeLoweringKind::Unsupported &&
                !(isMuted(node->id) && hasImageOutput(*definition))) {
                addUnsupported(
                    runtime::CompileDiagnosticCode::UnsupportedNode, subject(node->id, "typeId"),
                    "Node cannot be evaluated yet",
                    "This registered node type has no compiled operation in this build.");
            }
        }
    }

    void validateCompositionOutput() {
        const auto& compositionOutput = composition_->graph().compositionOutput();
        if (!compositionOutput.has_value()) {
            addFailure(runtime::CompileDiagnosticCode::InvalidCompositionOutput,
                       subject({}, "compositionOutput"), "Composition output endpoint is missing",
                       "The graph must name one Composition Output image port.");
            return;
        }
        const auto& endpoint = *compositionOutput;
        const auto* endpointNode = findNode(endpoint.nodeId);
        if (endpointNode == nullptr ||
            endpointNode->typeId != document::kCompositionOutputNodeType ||
            endpoint.port != document::kCompositionOutputOutputPort) {
            addFailure(runtime::CompileDiagnosticCode::InvalidCompositionOutput,
                       subject(endpoint.nodeId, "compositionOutput"),
                       "Composition output endpoint is invalid",
                       "The endpoint must name a Composition Output image port.");
            return;
        }
        // The endpoint names the node that ENDS the composition, and the port string is how the
        // document has always spelled that endpoint -- not a socket. Task FIX1, item H made the
        // Output node a sink with no declared output port, so the lowering is what is checked here;
        // the port spelling is pinned by the equality test above.
        const auto definition = definitions_.find(endpoint.nodeId);
        if (definition != definitions_.end() &&
            definition->second->lowering != runtime::NodeLoweringKind::CompositionOutput) {
            addFailure(runtime::CompileDiagnosticCode::InvalidCompositionOutput,
                       subject(endpoint.nodeId, "compositionOutput"),
                       "Composition output endpoint is invalid",
                       "The endpoint must name a registered Composition Output image port.");
        }
    }

    void validateEdges() {
        for (const auto* edge : reachableEdges_) {
            if (cancelled()) {
                return;
            }
            const auto sourceDefinition = definitions_.find(edge->source.nodeId);
            const auto destinationDefinition =
                definitions_.find(destinationNode(edge->destination));
            if (sourceDefinition == definitions_.end() ||
                destinationDefinition == definitions_.end()) {
                continue;
            }

            const auto* output = findOutput(*sourceDefinition->second, edge->source.port);
            if (output == nullptr) {
                auto diagnosticSubject = subject(edge->source.nodeId, "source.port");
                diagnosticSubject.edgeId = edge->id;
                addFailure(runtime::CompileDiagnosticCode::UnknownPort,
                           std::move(diagnosticSubject), "Edge source port is unknown",
                           "The source port is not declared by its registered node definition.");
                continue;
            }

            const runtime::SocketValueKind* inputKind = nullptr;
            const runtime::InputPortDefinition* fixedInput = nullptr;
            std::visit(
                [&](const auto& input) {
                    using Input = std::decay_t<decltype(input)>;
                    if constexpr (std::is_same_v<Input, document::NodeInputRef>) {
                        if (const auto* definition =
                                findInput(*destinationDefinition->second, input.port)) {
                            inputKind = &definition->valueKind;
                            fixedInput = definition;
                        }
                    } else if (destinationDefinition->second->layerSlotInput.has_value() &&
                               destinationDefinition->second->layerSlotInput->role == input.role) {
                        inputKind = &destinationDefinition->second->layerSlotInput->valueKind;
                    } else if (destinationDefinition->second->audioLayerSlotInput.has_value() &&
                               destinationDefinition->second->audioLayerSlotInput->role ==
                                   input.role) {
                        inputKind = &destinationDefinition->second->audioLayerSlotInput->valueKind;
                    }
                },
                edge->destination);

            if (inputKind == nullptr) {
                auto diagnosticSubject =
                    subject(destinationNode(edge->destination), "destination.port");
                diagnosticSubject.edgeId = edge->id;
                addFailure(runtime::CompileDiagnosticCode::UnknownPort,
                           std::move(diagnosticSubject), "Edge destination port is unknown",
                           "The destination is not declared by its registered node definition.");
            } else if (fixedInput != nullptr
                           ? !document::isAcceptedInputConnection(output->valueKind, *fixedInput)
                           : !document::isAcceptedSocketConnection(output->valueKind, *inputKind)) {
                auto diagnosticSubject =
                    subject(destinationNode(edge->destination), "destination.port");
                diagnosticSubject.edgeId = edge->id;
                addFailure(runtime::CompileDiagnosticCode::PortTypeMismatch,
                           std::move(diagnosticSubject), "Connected port types do not match",
                           "The edge connects incompatible typed sockets.");
            }
        }
    }

    void validateInputs() {
        const auto& graph = composition_->graph();
        for (const auto* node : reachableNodes_) {
            if (cancelled()) {
                return;
            }
            if (isMuted(node->id) && !transformParents_.contains(node->id))
                continue;
            const auto definition = definitions_.find(node->id);
            if (definition == definitions_.end()) {
                continue;
            }
            for (const auto& input : definition->second->inputs) {
                if (cancelled()) {
                    return;
                }
                if (!input.required) {
                    continue;
                }
                if (fixedInputEdge(node->id, input.name) == nullptr) {
                    addFailure(runtime::CompileDiagnosticCode::MissingInput,
                               subject(node->id, "input." + input.name),
                               "Required node input is not connected",
                               "The registered node definition requires this input.");
                }
            }

            if (const auto* merge = graph.merge(node->id); merge && !merge->enabled())
                continue;
            const auto& layerSlotInput = definition->second->layerSlotInput;
            if (!layerSlotInput.has_value()) {
                continue;
            }
            const auto& slotInputDefinition = *layerSlotInput;
            if (!slotInputDefinition.requiredPerSlot) {
                continue;
            }
            for (const auto& entry : graph.merge(node->id)->entries()) {
                if (entryExcluded(node->id, entry))
                    continue;
                if (cancelled()) {
                    return;
                }
                const auto* const imageEdge =
                    layerSlotInputEdge(node->id, entry.slotId, slotInputDefinition.role);
                const auto* const audioEdge =
                    definition->second->audioLayerSlotInput.has_value()
                        ? layerSlotInputEdge(node->id, entry.slotId,
                                             definition->second->audioLayerSlotInput->role)
                        : nullptr;
                if (imageEdge == nullptr && audioEdge == nullptr) {
                    auto diagnosticSubject = subject(node->id, "layerSlotInput");
                    diagnosticSubject.layerId = entry.layerId;
                    diagnosticSubject.layerSlotId = entry.slotId;
                    addFailure(runtime::CompileDiagnosticCode::MissingInput,
                               std::move(diagnosticSubject),
                               "Layer Stack slot has no content input",
                               "Every visible stack slot requires one typed content connection.");
                }
            }
        }
    }

    void validateParameters() {
        for (const auto* node : reachableNodes_) {
            if (cancelled()) {
                return;
            }
            if (isMuted(node->id) && !transformParents_.contains(node->id))
                continue;
            const auto definition = definitions_.find(node->id);
            if (definition == definitions_.end()) {
                continue;
            }
            std::unordered_set<std::string_view> declaredRoles;
            for (const auto& parameterDefinition : definition->second->parameters) {
                if (cancelled()) {
                    return;
                }
                declaredRoles.insert(parameterDefinition.role);
            }
            std::unordered_map<std::string_view, const document::ParameterBinding*> bindings;
            for (const auto& binding : node->parameters) {
                if (cancelled()) {
                    return;
                }
                bindings.emplace(binding.role, &binding);
                // #543: a gradient stop's link is bound under the stop's socket.
                if (!declaredRoles.contains(binding.role) &&
                    document::gradientParameterOfStopPort(*definition->second, binding.role) ==
                        nullptr) {
                    auto diagnosticSubject = subject(node->id, "parameter." + binding.role);
                    diagnosticSubject.parameterId = binding.parameterId;
                    addFailure(runtime::CompileDiagnosticCode::UnexpectedParameter,
                               std::move(diagnosticSubject),
                               "Node has an unexpected parameter binding",
                               "The binding role is not declared by the registered definition.");
                }
            }
            for (const auto& parameterDefinition : definition->second->parameters) {
                if (cancelled()) {
                    return;
                }
                const auto binding = bindings.find(parameterDefinition.role);
                validateParameter(*node, parameterDefinition,
                                  binding == bindings.end() ? nullptr : binding->second);
            }
        }
    }

    void validateParameterOverride() {
        if (request_.parameterOverrides.size() > 8) {
            addFailure(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                       subject({}, "parameterOverrides"), "Too many parameter overrides",
                       "A preview request accepts at most eight distinct overrides.");
            return;
        }
        std::unordered_set<document::ParameterId> seen;
        for (const auto& override : request_.parameterOverrides) {
            if (!seen.insert(override.parameterId).second) {
                addFailure(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                           subject({}, "parameterOverrides"), "Duplicate parameter override",
                           "A parameter may appear only once in a preview request.");
                return;
            }
            validateOneOverride(override);
        }
    }

    void validateOneOverride(const runtime::SnapshotParameterOverride& parameterOverride) {
        auto diagnosticSubject = subject({}, "parameterOverride");
        diagnosticSubject.parameterId = parameterOverride.parameterId;
        const auto reject = [&](const runtime::CompileDiagnosticCode code, std::string summary,
                                std::string detail) {
            auto overrideSubject = diagnosticSubject;
            addFailure(code, std::move(overrideSubject), std::move(summary), std::move(detail));
        };

        if (parameterOverride.sourceRevision != request_.snapshot.revision()) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override revision does not match the snapshot",
                   "Recreate the interaction from the current immutable snapshot.");
            return;
        }
        const auto* parameter = findParameter(parameterOverride.parameterId);
        if (parameter == nullptr) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override target does not exist",
                   "The target parameter is absent from the requested composition.");
            return;
        }

        // ADR 0028: a mask's path, feather, expansion and opacity belong to its reachable layer,
        // not to a node binding. The Viewer previews a mask drag through this same override.
        std::optional<runtime::ParameterValueKind> maskKind;
        const auto considerMasks = [&](const document::LayerOutputBoundary& boundary) {
            for (const auto& mask : boundary.masks) {
                if (mask.path == parameterOverride.parameterId)
                    maskKind = runtime::ParameterValueKind::Path;
                else if (mask.feather == parameterOverride.parameterId ||
                         mask.expansion == parameterOverride.parameterId ||
                         mask.opacity == parameterOverride.parameterId)
                    maskKind = runtime::ParameterValueKind::Float64;
            }
        };
        for (const auto* node : reachableNodes_) {
            const auto boundary = layerOutputs_.find(node->id);
            if (boundary != layerOutputs_.end())
                considerMasks(*boundary->second);
        }
        // #502: an entry layer's masks and parameters belong to it, not to a node binding.
        for (const auto* layer : reachedEntryLayers_)
            considerMasks(*layer);
        if (maskKind) {
            document::ParameterValue value =
                std::visit([](const auto& entry) -> document::ParameterValue { return entry; },
                           parameterOverride.value);
            if (!hasValueKind(value, *maskKind)) {
                reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                       "Parameter override type does not match its target",
                       "Overrides require a compatible reachable parameter.");
                return;
            }
            document::ParameterRecord replacement{parameter->id, parameter->schemaKey,
                                                  document::ConstantValueSource{std::move(value)}};
            document::ParameterStore validation;
            if (!validation.insert(replacement) || !validation.validate().ok()) {
                reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                       "Parameter override value is outside its schema domain",
                       "Preview constants obey the same value domains as authored constants.");
                return;
            }
            if (std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
                auto overrideSubject = diagnosticSubject;
                addUnsupported(runtime::CompileDiagnosticCode::UnsupportedParameterOverride,
                               std::move(overrideSubject),
                               "Driven parameters cannot be overridden interactively",
                               "Disconnect or explicitly transition the driver before editing.");
                return;
            }
            overrides_.emplace(parameter->id, std::move(replacement));
            return;
        }

        const document::NodeRecord* ownerNode = nullptr;
        const runtime::NodeDefinition* ownerDefinition = nullptr;
        const runtime::ParameterDefinition* parameterDefinition = nullptr;
        for (const auto* node : reachableNodes_) {
            if (cancelled()) {
                return;
            }
            const auto binding = std::ranges::find(node->parameters, parameterOverride.parameterId,
                                                   &document::ParameterBinding::parameterId);
            if (binding == node->parameters.end()) {
                continue;
            }
            if (ownerNode != nullptr) {
                reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                       "Parameter override target has multiple reachable owners",
                       "Version-one parameters must have one canonical node binding.");
                return;
            }
            const auto definition = definitions_.find(node->id);
            if (definition == definitions_.end()) {
                return;
            }
            const auto declaredParameter = std::ranges::find(
                definition->second->parameters, binding->role, &runtime::ParameterDefinition::role);
            if (declaredParameter == definition->second->parameters.end()) {
                return;
            }
            ownerNode = node;
            ownerDefinition = definition->second;
            parameterDefinition = &*declaredParameter;
            diagnosticSubject.nodeId = node->id;
        }
        for (const auto* layer : reachedEntryLayers_) {
            const auto binding = std::ranges::find(layer->parameters, parameterOverride.parameterId,
                                                   &document::ParameterBinding::parameterId);
            if (binding == layer->parameters.end())
                continue;
            if (ownerNode != nullptr || ownerDefinition != nullptr) {
                reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                       "Parameter override target has multiple reachable owners",
                       "Version-one parameters must have one canonical node binding.");
                return;
            }
            // An entry layer binds the Layer node's roles, so the Layer definition types them.
            ownerDefinition = registry_.find(document::kLayerOutputNodeType,
                                             document::kLayerOutputNodeSchemaVersion);
            if (ownerDefinition == nullptr)
                return;
            const auto declaredParameter = std::ranges::find(
                ownerDefinition->parameters, binding->role, &runtime::ParameterDefinition::role);
            if (declaredParameter == ownerDefinition->parameters.end())
                return;
            parameterDefinition = &*declaredParameter;
            ownerNode = findNode(layer->nodeId);
            diagnosticSubject.nodeId = layer->nodeId;
            diagnosticSubject.layerId = layer->layerId;
        }
        if (ownerNode == nullptr || ownerDefinition == nullptr || parameterDefinition == nullptr) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override target is not reachable",
                   "Overrides may affect only parameters on the requested output path.");
            return;
        }
        const auto kind = parameterDefinition->valueKind;
        document::ParameterValue value =
            std::visit([](const auto& entry) -> document::ParameterValue { return entry; },
                       parameterOverride.value);
        if (kind == runtime::ParameterValueKind::Boolean) {
            const auto* integer = std::get_if<std::int64_t>(&parameterOverride.value);
            if (integer && (*integer == 0 || *integer == 1))
                value = *integer != 0;
        }
        if (parameter->schemaKey != parameterDefinition->schemaKey || !hasValueKind(value, kind)) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override type does not match its target",
                   "Overrides require a compatible reachable parameter.");
            return;
        }
        document::ParameterRecord replacement{parameter->id, parameter->schemaKey,
                                              document::ConstantValueSource{std::move(value)}};
        document::ParameterStore validation;
        if (!validation.insert(replacement) || !validation.validate().ok()) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override value is outside its schema domain",
                   "Preview constants obey the same value domains as authored constants.");
            return;
        }
        if (std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
            auto overrideSubject = diagnosticSubject;
            addUnsupported(runtime::CompileDiagnosticCode::UnsupportedParameterOverride,
                           std::move(overrideSubject),
                           "Driven parameters cannot be overridden interactively",
                           "Disconnect or explicitly transition the driver before editing.");
            return;
        }
        overrides_.emplace(parameter->id, std::move(replacement));
    }

    void validateParameter(const document::NodeRecord& node,
                           const runtime::ParameterDefinition& definition,
                           const document::ParameterBinding* binding) {
        if (binding == nullptr) {
            if (definition.required) {
                addFailure(runtime::CompileDiagnosticCode::MissingParameter,
                           subject(node.id, "parameter." + definition.role),
                           "Required parameter binding is missing",
                           "The registered node definition requires this parameter role.");
            }
            return;
        }
        const auto* parameter = findParameter(binding->parameterId);
        if (parameter == nullptr) {
            auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
            diagnosticSubject.parameterId = binding->parameterId;
            addFailure(runtime::CompileDiagnosticCode::TopologyInvariant,
                       std::move(diagnosticSubject), "Parameter record is missing",
                       "The published graph violates its document topology invariant.");
            return;
        }
        if (parameter->schemaKey != definition.schemaKey) {
            auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
            diagnosticSubject.parameterId = parameter->id;
            addFailure(runtime::CompileDiagnosticCode::ParameterSchemaMismatch,
                       std::move(diagnosticSubject), "Parameter schema does not match",
                       "The parameter record uses a different schema than the node role.");
            return;
        }
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
            if (!hasValueKind(constant->value, definition.valueKind)) {
                auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
                diagnosticSubject.parameterId = parameter->id;
                addFailure(runtime::CompileDiagnosticCode::ParameterValueKindMismatch,
                           std::move(diagnosticSubject), "Parameter value type does not match",
                           "The constant value has a different type than the registered schema.");
            }
            return;
        }

        const auto* animation = std::get_if<document::AnimationCurveSource>(&parameter->source);
        if (animation != nullptr && definition.supportsAnimation) {
            const auto* curve = composition_->animationCurves().find(animation->curveId);
            const bool kindMatches =
                curve != nullptr &&
                ((definition.valueKind == runtime::ParameterValueKind::Float64 &&
                  std::holds_alternative<document::ScalarAnimationCurve>(*curve)) ||
                 (definition.valueKind == runtime::ParameterValueKind::Vec2d &&
                  std::holds_alternative<document::Vec2AnimationCurve>(*curve)) ||
                 (definition.valueKind == runtime::ParameterValueKind::Vec3d &&
                  std::holds_alternative<document::Vec3AnimationCurve>(*curve)) ||
                 // Task S5, item 1: the third curve kind. A Color4d-valued parameter demands a
                 // colour curve, exactly as a Float64 one demands a scalar.
                 (definition.valueKind == runtime::ParameterValueKind::Color4d &&
                  std::holds_alternative<document::Color4AnimationCurve>(*curve)) ||
                 // #498: a keyed Path parameter (a Curves curve) is a whole-value Path curve,
                 // compiled inline where it is sampled (compiledPathParameter()).
                 (definition.valueKind == runtime::ParameterValueKind::Path &&
                  std::holds_alternative<document::PathAnimationCurve>(*curve)) ||
                 // #541: a keyed Gradient (a Colorize Gradient Map), compiled inline into the
                 // Colorize kernel (compiledGradientParameter()).
                 (definition.valueKind == runtime::ParameterValueKind::Gradient &&
                  std::holds_alternative<document::GradientAnimationCurve>(*curve)));
            if (!kindMatches) {
                auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
                diagnosticSubject.parameterId = parameter->id;
                addFailure(runtime::CompileDiagnosticCode::ParameterValueKindMismatch,
                           std::move(diagnosticSubject), "Animation curve type does not match",
                           "The curve value kind differs from the registered parameter schema.");
            }
            return;
        }

        // Task DRIVE-1: a driver binding is evaluable for EVERY parameter kind. Task S7 could only
        // accept Scalar, Vector2 and Color here, because those were the only three whose compiled
        // parameter had a value-graph arm; String, Integer, Boolean and Vec3d were linkable in the
        // editor and durable in the document but carried nothing into a compiled operation, so they
        // were reported unsupported rather than pretending to evaluate. Each of the four now has a
        // lowering that reads the value graph -- a Text source's content and a Layer Output's blend
        // mode through CompiledText::drivenContent and CompiledLayerOutput::drivenBlendMode, and
        // every value-node operand of any kind through valueOperand() -- so there is no kind left
        // for which accepting the link would be a promise this build cannot keep. The reference
        // itself was already validated by the document layer; what is checked here is only whether
        // this build can lower it, and it can.
        if (std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
            return;
        }

        // One case is left: a curve over a parameter whose schema declares no animation. The three
        // source alternatives a document can store are a constant (returned above), an animation
        // curve, and a driver binding (returned immediately above, for every kind), so this arm is
        // reached only by the curve the definition refuses -- which is why it no longer has to
        // guess between two reasons.
        {
            auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
            diagnosticSubject.parameterId = parameter->id;
            addUnsupported(runtime::CompileDiagnosticCode::UnsupportedParameterSource,
                           std::move(diagnosticSubject), "Parameter source cannot be evaluated yet",
                           "This registered parameter does not support animation.");
        }
    }

#include "snapshot_compiler_layer_time.ipp"
#include "snapshot_compiler_lowering.ipp"
#include "snapshot_compiler_fields.ipp"
#include "snapshot_compiler_warp.ipp"
#include "snapshot_compiler_test_card.ipp"
#include "snapshot_compiler_recipes.ipp"
#include "snapshot_compiler_depth.ipp"
#include "snapshot_compiler_motion.ipp"
#include "snapshot_compiler_value_graph.ipp"
#include "snapshot_compiler_mattes.ipp"

    void addTopologyFailure(const document::NodeId nodeId, std::string detail) {
        addFailure(runtime::CompileDiagnosticCode::TopologyInvariant, subject(nodeId, "graph"),
                   "Validated graph could not be lowered", std::move(detail));
    }

    [[nodiscard]] std::vector<runtime::CompileDiagnostic> takeDiagnostics() {
        std::vector<runtime::CompileDiagnostic> result;
        result.reserve(diagnostics_.size());
        for (auto& [key, diagnostic] : diagnostics_) {
            static_cast<void>(key);
            if (cancelled()) {
                return {};
            }
            result.push_back(std::move(diagnostic));
        }
        return result;
    }

    [[nodiscard]] runtime::SnapshotCompileResult
    result(const runtime::SnapshotCompileStatus status) {
        if (status == runtime::SnapshotCompileStatus::Cancelled) {
            return {.status = status, .plan = {}, .diagnostics = {}};
        }
        auto diagnostics = takeDiagnostics();
        if (cancelled()) {
            return {
                .status = runtime::SnapshotCompileStatus::Cancelled, .plan = {}, .diagnostics = {}};
        }
        return {.status = status, .plan = {}, .diagnostics = std::move(diagnostics)};
    }

    [[nodiscard]] runtime::SnapshotCompileResult finishWithoutPlan() {
        return result(hasFailure_ ? runtime::SnapshotCompileStatus::Failed
                                  : runtime::SnapshotCompileStatus::Unsupported);
    }

    const runtime::NodeDefinitionRegistry& registry_;
    const runtime::SnapshotCompileRequest& request_;
    const runtime::CancellationToken& cancellation_;
    runtime::detail::CompileCheckpointObserver* checkpointObserver_ = nullptr;
    const document::Composition* composition_ = nullptr;
    document::NodeGroupExpansion expansion_;
    // #590: the node ids of each function body, by the consumer that reads it, and the value program
    // each body is lowered into while the value graph is compiled.
    std::unordered_map<document::NodeId, document::NodeId> bodyOwner_;
    std::map<document::NodeId, FunctionBodyBuild> functionBuilds_;
    std::unordered_map<document::NodeId, const document::NodeRecord*> nodes_;
    std::unordered_map<document::ParameterId, const document::ParameterRecord*> parameters_;
    // Request-local constant records make every lowering kind use the same override channel.
    std::map<document::ParameterId, document::ParameterRecord> overrides_;
    std::vector<const document::NodeRecord*> reachableNodes_;
    std::vector<const document::EdgeRecord*> reachableEdges_;
    std::map<FixedInputKey, const document::EdgeRecord*> fixedInputEdges_;
    std::map<LayerSlotInputKey, const document::EdgeRecord*> layerSlotInputEdges_;
    std::map<document::NodeId, const document::LayerOutputBoundary*> layerOutputs_;
    std::unordered_map<document::NodeId, const runtime::NodeDefinition*> definitions_;
    std::unordered_map<document::AnimationCurveId, runtime::ScalarCurveIndex> scalarCurveIndices_;
    std::unordered_map<document::AnimationCurveId, runtime::Vec2CurveIndex> vec2CurveIndices_;
    std::unordered_map<document::AnimationCurveId, runtime::Vec3CurveIndex> vec3CurveIndices_;
    std::unordered_map<document::AnimationCurveId, runtime::Color4CurveIndex> color4CurveIndices_;
    std::vector<runtime::CompiledValueOperation> valueOperations_;
    std::map<ValueOutputKey, runtime::ValueOutputIndex> valueOutputs_;
    std::unordered_map<document::NodeId, document::NodeId> pendingBoundsReadouts_;
    std::vector<std::uint8_t> postImageValueOutputs_;
    std::size_t valueOutputCount_ = 0;
    std::multimap<DiagnosticKey, runtime::CompileDiagnostic> diagnostics_;
    std::unordered_set<document::NodeId> emptyImages_;
    // #436: the Layer Outputs whose audio the mix lowers (collectAudibleLayers()).
    std::unordered_set<document::NodeId> audibleLayers_;
    // Set Alpha / Multiply Alpha nodes whose optional matte is unconnected, mapped to the empty
    // transparent operation that stands in for it.
    std::unordered_map<document::NodeId, runtime::OperationIndex> unconnectedMattes_;
    // #364: operations of a node with several pixel outputs, by output port (a Cryptomatte's Mask
    // and Image). A port listed here wins over the node's entry in the lowering index.
    std::map<std::pair<document::NodeId, std::string>, runtime::OperationIndex> portOperations_;
    // #383: the Mask outputs a reached layer's node track matte or matte masks read, which are
    // used as an edge from them would be.
    std::set<std::pair<document::NodeId, std::string>> reachedMatteOutputs_;
    std::unordered_set<document::NodeId> transformParents_;
    // #502 every Merge entry is a layer. Entry layers (LayerOutputBoundary::entryOwned) that are
    // transform parents, the entry layers the reachable set reached (a consumed entry's, or a
    // reached layer's parent or matte source), each entry's layer (mergeEntryLayer, memoized), the
    // lowered operation of each entry layer, and the Adjustment nodes with their single target.
    std::unordered_set<document::LayerId> entryLayerParents_;
    std::unordered_set<document::LayerId> reachedEntryLayerIds_;
    std::vector<const document::LayerOutputBoundary*> reachedEntryLayers_;
    mutable std::map<std::pair<document::NodeId, document::LayerSlotId>,
                     const document::LayerOutputBoundary*>
        entryLayerCache_;
    std::unordered_map<document::LayerId, runtime::OperationIndex> entryLayerOps_;
    std::unordered_set<document::LayerId> entryLayersInProgress_;
    mutable std::map<document::NodeId, std::vector<document::MergeEntryRef>> adjustmentTargetCache_;
    // #349: the plan of each Image Statistics node, compiled once however many outputs read it.
    std::map<document::NodeId, std::shared_ptr<const runtime::CompiledCompositionPlan>>
        statisticsPlans_;
    // The stack below an entry (an Adjustment's image, memoized per Merge and slot) and each
    // adjustment entry's composite.
    std::map<std::pair<document::NodeId, document::LayerSlotId>, runtime::OperationIndex>
        stacksBelow_;
    std::map<std::pair<document::NodeId, document::LayerSlotId>, runtime::OperationIndex>
        adjustmentComposites_;
    std::unordered_set<document::LayerId> matteSources_;
    bool anySolo_ = false;
    bool hasAdjustments_ = false;
    runtime::CompiledPlanCache* planCache_ = nullptr;
    std::vector<std::shared_ptr<const runtime::CompiledCompositionPlan>> nestedPlans_;
    std::unordered_map<document::NodeId, runtime::CompiledCompositionSource> compositionSources_;
    // #480 Composition Source `channels`: the operations list lower() is filling (a channel
    // expression may add the nested composite or a nested channel layer image), the composite
    // image added for a source whose `image` output feeds nothing, the image added per nested
    // channel layer plan, and the nestedPlans_ entries that are channel layer plans (never a
    // Composition Source's own nested plan).
    std::vector<runtime::CompiledOperation>* loweringOperations_ = nullptr;
    std::unordered_map<document::NodeId, runtime::OperationIndex> channelComposites_;
    std::map<std::pair<document::NodeId, const runtime::CompiledCompositionPlan*>,
             runtime::OperationIndex>
        channelLayerImages_;
    std::unordered_set<std::size_t> channelLayerPlans_;
    bool hasFailure_ = false;
    bool hasUnsupported_ = false;
};

} // namespace

namespace bloom::runtime {

SnapshotCompileResult detail::compileSnapshot(
    const NodeDefinitionRegistry& registry, const SnapshotCompileRequest& request,
    const CancellationToken& cancellation, detail::CompileCheckpointObserver* checkpointObserver) {
    return CompilePass(registry, request, cancellation, checkpointObserver).run();
}

SnapshotCompileResult SnapshotCompiler::compile(const SnapshotCompileRequest& request,
                                                const CancellationToken& cancellation,
                                                CompiledPlanCache* planCache) const {
    if (!planCache) {
        CompiledPlanCache cache;
        return cache.compile(*this, request, cancellation);
    }
    return CompilePass(registry_, request, cancellation, nullptr, planCache).run();
}

} // namespace bloom::runtime
