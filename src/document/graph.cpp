#include <bloom/document/graph.hpp>

#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

using bloom::document::InputPortRef;
using bloom::document::LayerStackInputRef;
using bloom::document::NodeId;
using bloom::document::NodeInputRef;

struct ExpectedParameterBinding final {
    std::string_view role;
    std::string_view schemaKey;
    bool required = true;
};

constexpr std::array kCompositionSourceBindings{
    ExpectedParameterBinding{"composition", "bloom.composition-source.composition"},
    ExpectedParameterBinding{"timeOffset", "bloom.composition-source.time-offset"},
    ExpectedParameterBinding{"timeScale", "bloom.composition-source.time-scale"},
    ExpectedParameterBinding{"loopMode", "bloom.composition-source.loop-mode"},
};
constexpr std::array kSolidSourceBindings{
    ExpectedParameterBinding{bloom::document::kSolidColorParameterRole,
                             bloom::document::kSolidColorParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kSolidWidthParameterRole,
                             bloom::document::kSolidWidthParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kSolidHeightParameterRole,
                             bloom::document::kSolidHeightParameterSchemaKey},
};
constexpr std::array kGradientBindings{
    ExpectedParameterBinding{"gradient", bloom::document::kGradientStopsParameterSchemaKey},
    ExpectedParameterBinding{"start", bloom::document::kGradientStartParameterSchemaKey},
    ExpectedParameterBinding{"end", bloom::document::kGradientEndParameterSchemaKey},
    ExpectedParameterBinding{"width", bloom::document::kSolidWidthParameterSchemaKey},
    ExpectedParameterBinding{"height", bloom::document::kSolidHeightParameterSchemaKey},
    ExpectedParameterBinding{"repeat", bloom::document::kGradientRepeatParameterSchemaKey},
};
constexpr std::array kTextSourceBindings{
    ExpectedParameterBinding{bloom::document::kTextParameterRole,
                             bloom::document::kTextParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTextSizeParameterRole,
                             bloom::document::kTextSizeParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTextColorParameterRole,
                             bloom::document::kTextColorParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTextAlignmentParameterRole,
                             bloom::document::kTextAlignmentParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTextLineHeightParameterRole,
                             bloom::document::kTextLineHeightParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTextLetterSpacingParameterRole,
                             bloom::document::kTextLetterSpacingParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTextFontParameterRole,
                             bloom::document::kTextFontParameterSchemaKey, false},
    ExpectedParameterBinding{bloom::document::kTextBoxParameterRole,
                             bloom::document::kTextBoxParameterSchemaKey, false},
    ExpectedParameterBinding{bloom::document::kTextWrapParameterRole,
                             bloom::document::kTextWrapParameterSchemaKey, false},
    ExpectedParameterBinding{bloom::document::kTextVerticalAlignmentParameterRole,
                             bloom::document::kTextVerticalAlignmentParameterSchemaKey, false},
    ExpectedParameterBinding{bloom::document::kTextAnchorModeParameterRole,
                             bloom::document::kTextAnchorModeParameterSchemaKey, false},
    ExpectedParameterBinding{bloom::document::kTextOverflowParameterRole,
                             bloom::document::kTextOverflowParameterSchemaKey, false},
};
constexpr std::array kLayerOutputBindings{
    ExpectedParameterBinding{bloom::document::kPositionParameterRole,
                             bloom::document::kPositionParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kAnchorParameterRole,
                             bloom::document::kAnchorParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kScaleParameterRole,
                             bloom::document::kScaleParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kRotationParameterRole,
                             bloom::document::kRotationParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kOpacityParameterRole,
                             bloom::document::kOpacityParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kBlendModeParameterRole,
                             bloom::document::kBlendModeParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTimeRemapParameterRole,
                             bloom::document::kTimeRemapParameterSchemaKey, false},
};

[[nodiscard]] std::span<const ExpectedParameterBinding>
expectedBindings(const bloom::document::NodeRecord& node) noexcept {
    using namespace bloom::document;
    if (node.typeId == kCompositionSourceNodeType)
        return kCompositionSourceBindings;
    if (node.typeId == kSolidSourceNodeType &&
        (node.schemaVersion == kSolidSourceNodeSchemaVersion || node.schemaVersion == 1)) {
        return std::span(kSolidSourceBindings).first(node.schemaVersion == 1 ? 1 : 3);
    }
    if ((node.typeId == kLinearGradientNodeType || node.typeId == kRadialGradientNodeType) &&
        node.schemaVersion == kGradientNodeSchemaVersion)
        return kGradientBindings;
    if (node.typeId == kTextSourceNodeType &&
        (node.schemaVersion == kTextSourceNodeSchemaVersion || node.schemaVersion == 1)) {
        return node.schemaVersion == 1 ? std::span(kTextSourceBindings).first(3)
                                       : std::span(kTextSourceBindings);
    }
    if (node.typeId == kLayerOutputNodeType &&
        (node.schemaVersion == kLayerOutputNodeSchemaVersion || node.schemaVersion == 4 ||
         node.schemaVersion == 3)) {
        // Schemas 3 and 4 never declared Time Remap; the document migration rewrites them to 5.
        if (node.schemaVersion == kLayerOutputNodeSchemaVersion)
            return kLayerOutputBindings;
        return std::span(kLayerOutputBindings).first(6);
    }
    return {};
}

void validateExpectedBindings(const bloom::document::NodeRecord& node,
                              const bloom::document::ParameterStore& parameters,
                              const bloom::document::NodeDefinitionRegistry& registry,
                              const std::string& path, bloom::document::ValidationResult& result) {
    using namespace bloom::document;
    const auto validate = [&](const ExpectedParameterBinding& expected) {
        const auto binding =
            std::find_if(node.parameters.begin(), node.parameters.end(),
                         [&](const auto& item) { return item.role == expected.role; });
        const auto bindingPath = path + ".parameters[" + std::string(expected.role) + "]";
        if (binding == node.parameters.end()) {
            if (!expected.required)
                return;
            result.add(ValidationCode::MissingReference, bindingPath,
                       "Known node schema requires this parameter binding");
            return;
        }
        const auto* parameter = parameters.find(binding->parameterId);
        if (parameter != nullptr && parameter->schemaKey != expected.schemaKey) {
            result.add(ValidationCode::InvalidValue, bindingPath,
                       "Parameter binding uses the wrong schema for this node role");
        }
    };
    for (const auto& expected : expectedBindings(node))
        validate(expected);
    if (node.schemaVersion == 1 &&
        (node.typeId == "bloom.flip" || node.typeId == "bloom.rotate-90" ||
         node.typeId == "bloom.resize" || node.typeId == "bloom.transform-2d")) {
        if (const auto* definition = registry.find(node.typeId, node.schemaVersion))
            for (const auto& parameter : definition->parameters)
                validate({parameter.role, parameter.schemaKey, parameter.required});
    }
}

// Whether this input port is backed by a parameter -- an operand socket rather than image
// transport. An operand's link is recorded as its PARAMETER's driver binding, never as an edge: one
// authored value has one durable record of where it comes from, so an edge and a binding can never
// disagree.
[[nodiscard]] bool isParameterSocket(const bloom::document::NodeRecord& node,
                                     const InputPortRef& destination) {
    const auto* fixed = std::get_if<NodeInputRef>(&destination);
    if (fixed == nullptr) {
        return false;
    }
    // #543: a gradient stop's socket is linked by a driver record too, bound or not yet.
    return bloom::document::parseGradientStopPort(fixed->port).has_value() ||
           std::ranges::any_of(node.parameters,
                               [&](const auto& binding) { return binding.role == fixed->port; });
}

[[nodiscard]] NodeId destinationNode(const InputPortRef& destination) {
    return std::visit(
        [](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, NodeInputRef>) {
                return input.nodeId;
            } else {
                return input.stackNodeId;
            }
        },
        destination);
}

[[nodiscard]] std::string destinationKey(const InputPortRef& destination) {
    return std::visit(
        [](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, NodeInputRef>) {
                return "node:" + std::to_string(input.nodeId.value()) + ":" + input.port;
            } else {
                return "stack:" + std::to_string(input.stackNodeId.value()) + ":" +
                       std::to_string(input.slotId.value()) + ":" + input.role;
            }
        },
        destination);
}

[[nodiscard]] bool validDestination(const InputPortRef& destination) {
    return std::visit(
        [](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, NodeInputRef>) {
                return input.nodeId.isValid() && bloom::document::isValidStructuralText(input.port);
            } else {
                return input.stackNodeId.isValid() && input.slotId.isValid() &&
                       bloom::document::isValidStructuralText(input.role);
            }
        },
        destination);
}

} // namespace

namespace bloom::document {

const LayerStack* CanonicalGraph::merge(const NodeId id) const noexcept {
    const auto found = std::ranges::find(layerStacks_, id, &LayerStack::nodeId);
    return found == layerStacks_.end() ? nullptr : &*found;
}
LayerStack* CanonicalGraph::merge(const NodeId id) noexcept {
    return const_cast<LayerStack*>(std::as_const(*this).merge(id));
}
std::optional<NodeId> CanonicalGraph::outputMergeId() const noexcept {
    if (!compositionOutput_)
        return std::nullopt;
    for (const auto& edge : edges_) {
        const auto* input = std::get_if<NodeInputRef>(&edge.destination);
        if (input && input->nodeId == compositionOutput_->nodeId &&
            input->port == kCompositionOutputInputPort && merge(edge.source.nodeId))
            return edge.source.nodeId;
    }
    return std::nullopt;
}
const LayerStack& CanonicalGraph::layerStack() const noexcept {
    if (const auto id = outputMergeId())
        return *merge(*id);
    return layerStacks_.empty() ? emptyStack_ : layerStacks_.front();
}
LayerStack& CanonicalGraph::layerStack() noexcept {
    return const_cast<LayerStack&>(std::as_const(*this).layerStack());
}

const NodeRecord* CanonicalGraph::findNode(const NodeId id) const noexcept {
    const auto iterator = std::find_if(nodes_.begin(), nodes_.end(),
                                       [id](const auto& node) { return node.id == id; });
    return iterator == nodes_.end() ? nullptr : &*iterator;
}

NodeRecord* CanonicalGraph::findNode(const NodeId id) noexcept {
    return const_cast<NodeRecord*>(std::as_const(*this).findNode(id));
}

bool CanonicalGraph::addNode(NodeRecord node) {
    if (!node.id.isValid() || !isValidNamespacedIdentifier(node.typeId) ||
        node.schemaVersion == 0 || findNode(node.id) != nullptr) {
        return false;
    }

    std::unordered_set<std::string> roles;
    for (const auto& binding : node.parameters) {
        if (!isValidStructuralText(binding.role) || !binding.parameterId.isValid() ||
            !roles.insert(binding.role).second) {
            return false;
        }
    }

    if (node.typeId == kLayerStackNodeType && !merge(node.id))
        layerStacks_.emplace_back(node.id);
    nodes_.push_back(std::move(node));
    return true;
}

bool CanonicalGraph::addEdge(EdgeRecord edge, const NodeDefinitionRegistry& registry) {
    const auto* destination = findNode(destinationNode(edge.destination));
    if (destination != nullptr && isParameterSocket(*destination, edge.destination)) {
        return false;
    }
    const auto sourceKind = outputKind(edge.source, registry);
    const auto targetKind = inputKind(edge.destination, registry);
    // Nothing connects FROM a SINK. Task FIX1, item H made the composition Output one -- it
    // declares no output port at all -- and this is the rule that says so for every connect path at
    // once. It asks whether the registered definition has ANY output, not whether this particular
    // port is declared: an unknown port on a node that does have outputs is still the compiler's
    // UnknownPort diagnostic to report, which is a different mistake with a different message.
    if (const auto* source = findNode(edge.source.nodeId); source != nullptr) {
        const auto* definition = registry.find(source->typeId, source->schemaVersion);
        if (definition != nullptr && definition->outputs.empty()) {
            return false;
        }
    }
    // Task S7: equal kinds, or one of the whitelisted promotions. The ONE predicate every connect
    // path asks (node_definition_registry.hpp), so this, validate() below, ConnectPorts and the
    // compiler's edge check cannot disagree about which links exist.
    if (sourceKind && targetKind && !acceptsConnection(*sourceKind, edge.destination, registry)) {
        return false;
    }
    if (!edge.id.isValid() || !edge.source.nodeId.isValid() ||
        !isValidStructuralText(edge.source.port) || !validDestination(edge.destination)) {
        return false;
    }

    const auto duplicateId = std::find_if(edges_.begin(), edges_.end(),
                                          [&edge](const auto& item) { return item.id == edge.id; });
    const auto key = destinationKey(edge.destination);
    const auto duplicateInput =
        std::find_if(edges_.begin(), edges_.end(),
                     [&key](const auto& item) { return destinationKey(item.destination) == key; });
    if (duplicateId != edges_.end() || duplicateInput != edges_.end()) {
        return false;
    }

    edges_.push_back(std::move(edge));
    return true;
}

const LayerOutputBoundary* CanonicalGraph::findLayer(const LayerId id) const noexcept {
    const auto found = std::ranges::find(layerOutputs_, id, &LayerOutputBoundary::layerId);
    return found == layerOutputs_.end() ? nullptr : &*found;
}
LayerOutputBoundary* CanonicalGraph::findLayer(const LayerId id) noexcept {
    return const_cast<LayerOutputBoundary*>(std::as_const(*this).findLayer(id));
}

bool CanonicalGraph::addLayerOutput(LayerOutputBoundary boundary) {
    if (!boundary.nodeId.isValid() || !boundary.layerId.isValid() ||
        !isValidHumanFacingName(boundary.name) || !isValidStructuralText(boundary.outputPort)) {
        return false;
    }

    // A Layer node has one boundary; a Merge node may own one entry layer per slot (#502).
    const auto duplicate =
        std::find_if(layerOutputs_.begin(), layerOutputs_.end(), [&boundary](const auto& item) {
            return item.layerId == boundary.layerId ||
                   (item.nodeId == boundary.nodeId &&
                    (!item.entryOwned() || !boundary.entryOwned() ||
                     item.entrySlot == boundary.entrySlot));
        });
    if (duplicate != layerOutputs_.end()) {
        return false;
    }

    layerOutputs_.push_back(std::move(boundary));
    return true;
}

bool CanonicalGraph::eraseLayerOutput(const LayerId id) {
    return std::erase_if(layerOutputs_,
                         [id](const auto& boundary) { return boundary.layerId == id; }) != 0;
}

bool CanonicalGraph::eraseEdge(const EdgeId id) {
    return std::erase_if(edges_, [id](const auto& edge) { return edge.id == id; }) != 0;
}

bool CanonicalGraph::eraseNode(const NodeId id) {
    if (findNode(id) == nullptr)
        return false;
    for (const auto& edge : edges_) {
        if (edge.source.nodeId == id) {
            if (const auto* slot = std::get_if<LayerStackInputRef>(&edge.destination))
                if (auto* stack = merge(slot->stackNodeId))
                    (void)stack->erase(slot->slotId);
        }
    }
    std::erase_if(edges_, [id](const auto& edge) {
        return edge.source.nodeId == id || destinationNode(edge.destination) == id;
    });
    std::erase_if(layerStacks_, [id](const auto& stack) { return stack.nodeId() == id; });
    for (auto& child : layerOutputs_) {
        if (child.parent) {
            const auto* parent = findLayer(*child.parent);
            if (parent && parent->nodeId == id)
                child.parent.reset();
        }
    }
    // ADR 0028: a matte whose source layer disappears is cleared, as a parent is.
    for (auto& target : layerOutputs_) {
        if (target.trackMatte) {
            const auto* source = findLayer(target.trackMatte->source);
            if ((source && source->nodeId == id) ||
                (target.trackMatte->node && target.trackMatte->node->nodeId == id))
                target.trackMatte.reset();
        }
        // #383: a matte mask whose node disappears goes with it, as a matte does. Its parameters
        // are left to the caller's cleanup, as a removed node's are.
        std::erase_if(target.masks, [id](const auto& mask) {
            return mask.matte && mask.matte->nodeId == id;
        });
    }
    std::erase_if(layerOutputs_, [id](const auto& boundary) { return boundary.nodeId == id; });
    // #502: an entry layer leaves with its slot. Its parameters stay in the store for the
    // command's own cleanup, as a removed node's do.
    std::erase_if(layerOutputs_, [this](const auto& boundary) {
        if (!boundary.entryOwned())
            return false;
        const auto* stack = merge(boundary.nodeId);
        return stack == nullptr || stack->find(*boundary.entrySlot) == nullptr;
    });
    std::erase_if(nodes_, [id](const auto& node) { return node.id == id; });
    return true;
}

void CanonicalGraph::clearTrackMattesOutsideMerge(const LayerId layer) {
    // The same first-Merge membership validate() uses for the same-Merge rule: the Merge whose
    // entry's layer (#502, mergeEntryLayer) is `id`.
    const auto mergeOf = [this](const LayerId id) -> std::optional<NodeId> {
        for (const auto& stack : layerStacks_)
            if (std::ranges::any_of(stack.entries(), [&](const auto& entry) {
                    const auto* layer = mergeEntryLayer(*this, stack.nodeId(), entry.slotId);
                    return layer != nullptr && layer->layerId == id;
                }))
                return stack.nodeId();
        return std::nullopt;
    };
    for (auto& target : layerOutputs_) {
        if (!target.trackMatte || target.trackMatte->node ||
            (target.layerId != layer && target.trackMatte->source != layer))
            continue;
        const auto targetMerge = mergeOf(target.layerId);
        if (!targetMerge || targetMerge != mergeOf(target.trackMatte->source))
            target.trackMatte.reset();
    }
}

bool CanonicalGraph::renameLayer(const LayerId id, std::string name) {
    if (!isValidHumanFacingName(name))
        return false;
    for (auto& boundary : layerOutputs_) {
        if (boundary.layerId == id) {
            boundary.name = std::move(name);
            return true;
        }
    }
    return false;
}

ValidationResult CanonicalGraph::validate(const ParameterStore& parameters,
                                          const NodeDefinitionRegistry& registry,
                                          const CompositionId owner) const {
    ValidationResult result;
    std::unordered_set<LayerSlotId> slots;
    for (const auto& stack : layerStacks_) {
        result.append("merges", stack.validate());
        for (const auto& entry : stack.entries())
            if (!slots.insert(entry.slotId).second)
                result.add(ValidationCode::DuplicateId, "merges.entries.slotId",
                           "Slot IDs must be unique across Merges");
    }

    std::unordered_set<NodeId> nodeIds;
    for (const auto& node : nodes_) {
        const auto path = "nodes[" + std::to_string(node.id.value()) + "]";
        if (!node.id.isValid()) {
            result.add(ValidationCode::InvalidId, path + ".id", "Node ID must not be zero");
        } else if (!nodeIds.insert(node.id).second) {
            result.add(ValidationCode::DuplicateId, path + ".id", "Node ID is duplicated");
        }
        validateNamespacedIdentifier(node.typeId, path + ".typeId", "Node type ID", result);
        if (node.schemaVersion == 0) {
            result.add(ValidationCode::InvalidValue, path + ".schemaVersion",
                       "Node schema version must not be zero");
        }

        std::unordered_set<std::string> roles;
        for (std::size_t bindingIndex = 0; bindingIndex < node.parameters.size(); ++bindingIndex) {
            const auto& binding = node.parameters[bindingIndex];
            const bool roleIsValid = isValidStructuralText(binding.role);
            const bool roleCanIdentifyPath = roleIsValid || binding.role.empty();
            const auto bindingIdentity =
                roleCanIdentifyPath ? binding.role : std::to_string(bindingIndex);
            auto bindingPath = path;
            bindingPath.append(".parameters[").append(bindingIdentity).push_back(']');
            validateStructuralText(binding.role, bindingPath, "Parameter binding role", result);
            if (roleIsValid && !roles.insert(binding.role).second) {
                result.add(ValidationCode::DuplicateId, bindingPath,
                           "Parameter binding role is duplicated on the node");
            }
            if (!binding.parameterId.isValid()) {
                result.add(ValidationCode::InvalidId, bindingPath,
                           "Parameter binding ID must not be zero");
            } else if (parameters.find(binding.parameterId) == nullptr) {
                result.add(ValidationCode::MissingReference, bindingPath,
                           "Parameter binding references a missing parameter");
            }
        }
        validateExpectedBindings(node, parameters, registry, path, result);
        // #342: which group an instance reads is a choice, never a driven value, and a group
        // cannot contain an instance of itself.
        if (node.typeId == kNodeGroupNodeType) {
            for (const auto& binding : node.parameters) {
                if (binding.role != kNodeGroupReferenceRole)
                    continue;
                const auto* parameter = parameters.find(binding.parameterId);
                if (parameter != nullptr &&
                    !std::holds_alternative<ConstantValueSource>(parameter->source))
                    result.add(ValidationCode::InvalidValue, path + ".group",
                               "The group a Node Group names must be a constant");
            }
            if (const auto group = nodeGroupReference(node, parameters);
                group && owner.isValid() && group->value() == owner.value())
                result.add(ValidationCode::CompositionNestingCycle, path + ".group",
                           "A group cannot contain itself");
        }
        if (node.typeId == kCompositionSourceNodeType) {
            for (const auto& binding : node.parameters) {
                if (binding.role != "composition" && binding.role != "loopMode")
                    continue;
                const auto* parameter = parameters.find(binding.parameterId);
                const auto* constant =
                    parameter ? std::get_if<ConstantValueSource>(&parameter->source) : nullptr;
                if (parameter && !constant)
                    result.add(ValidationCode::InvalidValue, path + "." + binding.role,
                               "Composition identity and loop mode must be constant");
                const auto* id = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
                if (binding.role == "composition" && id && *id > 0 && owner.isValid() &&
                    static_cast<std::uint64_t>(*id) == owner.value())
                    result.add(ValidationCode::CompositionNestingCycle, path + ".composition",
                               "A composition cannot contain itself");
            }
        }
    }

    for (const auto& stack : layerStacks_) {
        const auto* node = findNode(stack.nodeId());
        if (!node || node->typeId != kLayerStackNodeType)
            result.add(ValidationCode::InvalidLayerStack, "merges",
                       "Merge slots require a Merge node");
    }
    std::unordered_set<NodeId> boundaryNodeIds;
    std::unordered_map<LayerId, const LayerOutputBoundary*> boundariesByLayer;
    for (const auto& boundary : layerOutputs_) {
        const auto path = "layerOutputs[" + std::to_string(boundary.layerId.value()) + "]";
        if (!boundary.nodeId.isValid() || !boundary.layerId.isValid()) {
            result.add(ValidationCode::InvalidId, path,
                       "Layer Output node and layer IDs must not be zero");
        }
        validateStructuralText(boundary.outputPort, path + ".outputPort", "Layer Output port",
                               result);
        validateHumanFacingName(boundary.name, path + ".name", "Layer name", result);
        if ((!boundary.entryOwned() && !boundaryNodeIds.insert(boundary.nodeId).second) ||
            !boundariesByLayer.emplace(boundary.layerId, &boundary).second) {
            result.add(ValidationCode::DuplicateId, path,
                       "Layer Output node and layer IDs must be unique");
        }

        const auto* node = findNode(boundary.nodeId);
        if (node == nullptr) {
            result.add(ValidationCode::MissingReference, path + ".nodeId",
                       "Layer Output references a missing node");
        } else if (node->typeId !=
                   (boundary.entryOwned() ? kLayerStackNodeType : kLayerOutputNodeType)) {
            result.add(ValidationCode::InvalidLayerBoundary, path + ".nodeId",
                       "Layer Output boundary node has the wrong node type");
        }
    }

    // Parent identities are local to this composition, independently of Merge membership.
    std::unordered_set<LayerId> checkedParentChains;
    for (const auto& boundary : layerOutputs_) {
        if (checkedParentChains.contains(boundary.layerId))
            continue;
        const auto path = "layerOutputs[" + std::to_string(boundary.layerId.value()) + "].parent";
        std::unordered_set<LayerId> ancestors{boundary.layerId};
        auto parent = boundary.parent;
        while (parent) {
            if (checkedParentChains.contains(*parent))
                break;
            const auto found = boundariesByLayer.find(*parent);
            if (found == boundariesByLayer.end()) {
                result.add(ValidationCode::MissingReference, path,
                           "Parent must be a layer in the same composition");
                break;
            }
            if (!ancestors.insert(*parent).second) {
                result.add(ValidationCode::InvalidLayerBoundary, path,
                           "Layer parenting must not contain self-parenting or cycles");
                break;
            }
            parent = found->second->parent;
        }
        checkedParentChains.insert(ancestors.begin(), ancestors.end());
    }

    // ADR 0028 track mattes: the source is another layer of the same Merge, the channel is a
    // closed value, and the relation is acyclic (checked with the evaluation order below).
    std::unordered_map<LayerId, NodeId> mergeOfLayer;
    for (const auto& stack : layerStacks_)
        for (const auto& entry : stack.entries())
            // #502: the entry's layer, which may be a Layer node further up its chain.
            if (const auto* layer =
                    mergeEntryLayer(*this, stack.nodeId(), entry.slotId, registry))
                mergeOfLayer.emplace(layer->layerId, stack.nodeId());
    for (const auto& boundary : layerOutputs_) {
        if (!boundary.trackMatte)
            continue;
        const auto path =
            "layerOutputs[" + std::to_string(boundary.layerId.value()) + "].trackMatte";
        const auto& matte = *boundary.trackMatte;
        if (matte.channel != TrackMatteChannel::Alpha && matte.channel != TrackMatteChannel::Luma)
            result.add(ValidationCode::InvalidValue, path + ".channel",
                       "Track matte channel must be Alpha or Luma");
        // #383: a node matte names a Mask output of this composition, and no source layer.
        if (matte.node) {
            if (matte.source.isValid())
                result.add(ValidationCode::InvalidLayerBoundary, path + ".source",
                           "A node track matte has no source layer");
            if (outputKind(*matte.node, registry) != SocketValueKind::Mask)
                result.add(ValidationCode::MissingReference, path + ".node",
                           "Track matte node must be a Mask output in the same composition");
            continue;
        }
        if (matte.source == boundary.layerId) {
            result.add(ValidationCode::InvalidLayerBoundary, path + ".source",
                       "A layer cannot be its own track matte");
            continue;
        }
        if (!boundariesByLayer.contains(matte.source)) {
            result.add(ValidationCode::MissingReference, path + ".source",
                       "Track matte source must be a layer in the same composition");
            continue;
        }
        const auto targetMerge = mergeOfLayer.find(boundary.layerId);
        const auto sourceMerge = mergeOfLayer.find(matte.source);
        if (targetMerge == mergeOfLayer.end() || sourceMerge == mergeOfLayer.end() ||
            targetMerge->second != sourceMerge->second)
            result.add(ValidationCode::InvalidLayerBoundary, path + ".source",
                       "Track matte source must be a layer in the same Merge");
    }
    // #383: a matte mask names a Mask output of this composition.
    for (const auto& boundary : layerOutputs_)
        for (std::size_t index = 0; index < boundary.masks.size(); ++index)
            if (const auto& matte = boundary.masks[index].matte;
                matte && outputKind(*matte, registry) != SocketValueKind::Mask)
                result.add(ValidationCode::MissingReference,
                           "layerOutputs[" + std::to_string(boundary.layerId.value()) +
                               "].masks[" + std::to_string(index) + "].matte",
                           "A matte mask must name a Mask output in the same composition");
    for (const auto& boundary : layerOutputs_) {
        std::unordered_set<LayerId> chain{boundary.layerId};
        auto matte = boundary.trackMatte;
        while (matte && !matte->node) {
            const auto found = boundariesByLayer.find(matte->source);
            if (found == boundariesByLayer.end())
                break;
            if (!chain.insert(matte->source).second) {
                result.add(ValidationCode::InvalidLayerBoundary,
                           "layerOutputs[" + std::to_string(boundary.layerId.value()) +
                               "].trackMatte",
                           "Track mattes must not form a cycle");
                break;
            }
            matte = found->second->trackMatte;
        }
    }

    std::unordered_set<EdgeId> edgeIds;
    std::unordered_set<std::string> destinations;
    std::unordered_map<NodeId, std::vector<NodeId>> adjacency;
    std::unordered_map<NodeId, std::size_t> indegree;
    for (const auto& node : nodes_) {
        indegree.try_emplace(node.id, 0);
    }

    // Transform dependencies share the image/driver evaluation order: a parent's transform needs
    // the parent's content, so a mixed cycle through content is no more evaluable than a
    // parent-only cycle. A track matte is applied after both layers and adds no order of its own,
    // so a matte source may be a child of the layer it mattes.
    for (const auto& boundary : layerOutputs_) {
        const auto parent =
            boundary.parent ? boundariesByLayer.find(*boundary.parent) : boundariesByLayer.end();
        // #502: entry layers of one Merge evaluate in parent order inside that Merge.
        if (parent != boundariesByLayer.end() && indegree.contains(boundary.nodeId) &&
            indegree.contains(parent->second->nodeId) &&
            parent->second->nodeId != boundary.nodeId) {
            adjacency[parent->second->nodeId].push_back(boundary.nodeId);
            ++indegree[boundary.nodeId];
        }
        // #383: a node matte, or a matte mask's node, evaluates before the layer it mattes, so it
        // must not depend on that layer (or, for a Merge entry's own layer, on its Merge).
        const auto orderMatteNode = [&](const std::optional<OutputPortRef>& matte) {
            if (matte && indegree.contains(boundary.nodeId) && indegree.contains(matte->nodeId) &&
                matte->nodeId != boundary.nodeId) {
                adjacency[matte->nodeId].push_back(boundary.nodeId);
                ++indegree[boundary.nodeId];
            }
        };
        if (boundary.trackMatte)
            orderMatteNode(boundary.trackMatte->node);
        for (const auto& mask : boundary.masks)
            orderMatteNode(mask.matte);
    }

    for (const auto& edge : edges_) {
        const auto path = "edges[" + std::to_string(edge.id.value()) + "]";
        if (!edge.id.isValid()) {
            result.add(ValidationCode::InvalidId, path + ".id", "Edge ID must not be zero");
        } else if (!edgeIds.insert(edge.id).second) {
            result.add(ValidationCode::DuplicateId, path + ".id", "Edge ID is duplicated");
        }
        if (!edge.source.nodeId.isValid() || !isValidStructuralText(edge.source.port)) {
            result.add(ValidationCode::InvalidValue, path + ".source",
                       "Edge source must have a node and port");
        } else if (findNode(edge.source.nodeId) == nullptr) {
            result.add(ValidationCode::MissingReference, path + ".source.nodeId",
                       "Edge source references a missing node");
        }

        if (!validDestination(edge.destination)) {
            result.add(ValidationCode::InvalidValue, path + ".destination",
                       "Edge destination is invalid");
            continue;
        }

        const auto sourceKind = outputKind(edge.source, registry);
        const auto targetKind = inputKind(edge.destination, registry);
        const auto* sourceDefinition = [&]() -> const NodeDefinition* {
            const auto* sourceNode = findNode(edge.source.nodeId);
            return sourceNode == nullptr
                       ? nullptr
                       : registry.find(sourceNode->typeId, sourceNode->schemaVersion);
        }();
        if (sourceDefinition != nullptr && sourceDefinition->outputs.empty()) {
            result.add(ValidationCode::InvalidValue, path + ".source",
                       "This node is a sink and declares no output to connect from");
        } else if (sourceKind && targetKind &&
                   !acceptsConnection(*sourceKind, edge.destination, registry)) {
            result.add(ValidationCode::SocketKindMismatch, path,
                       "Connected socket kinds do not match");
        }
        const auto inputKey = destinationKey(edge.destination);
        if (!destinations.insert(inputKey).second) {
            result.add(ValidationCode::DuplicateInput, path + ".destination",
                       "More than one edge targets the same input");
        }

        const auto targetNodeId = destinationNode(edge.destination);
        const auto* targetNode = findNode(targetNodeId);
        if (targetNode == nullptr) {
            result.add(ValidationCode::MissingReference, path + ".destination",
                       "Edge destination references a missing node");
        } else if (isParameterSocket(*targetNode, edge.destination)) {
            result.add(
                ValidationCode::InvalidValue, path + ".destination",
                "An operand socket is linked by its parameter's driver binding, not an edge");
        }

        if (const auto* nodeInput = std::get_if<NodeInputRef>(&edge.destination)) {
            if (merge(nodeInput->nodeId) != nullptr) {
                result.add(ValidationCode::InvalidLayerStack, path + ".destination",
                           "Layer Stack inputs must address a stable slot and role");
            }
        } else {
            const auto& stackInput = std::get<LayerStackInputRef>(edge.destination);
            const auto* stack = merge(stackInput.stackNodeId);
            if (!stack || !stack->find(stackInput.slotId)) {
                result.add(ValidationCode::MissingReference, path + ".destination.slotId",
                           "Merge edge targets a missing stable slot");
            }
        }

        if (findNode(edge.source.nodeId) != nullptr && findNode(targetNodeId) != nullptr) {
            adjacency[edge.source.nodeId].push_back(targetNodeId);
            ++indegree[targetNodeId];
        }
    }

    // Driver bindings (task S7). A driven parameter is a reference from a value-graph node's output
    // into parameter-address space, so it is validated on exactly an edge's terms -- the target
    // must exist, the port must be declared, and the kinds must be connectable -- and it feeds the
    // SAME adjacency the cycle check below walks. A driver that closes a loop is therefore refused
    // by the one existing same-time cycle rule rather than by a second rule that could disagree
    // with it.
    for (const auto& node : nodes_) {
        const auto* definition = registry.find(node.typeId, node.schemaVersion);
        for (const auto& binding : node.parameters) {
            const auto* parameter = parameters.find(binding.parameterId);
            // #543: a stop's socket holds a stop link and nothing else.
            if (parameter != nullptr && parseGradientStopPort(binding.role).has_value() !=
                                            isGradientStopSchemaKey(parameter->schemaKey))
                result.add(
                    ValidationCode::InvalidValue,
                    "nodes[" + std::to_string(node.id.value()) + "].parameters[" + binding.role +
                        "]",
                    "A gradient stop link binds a stop socket, and a stop socket only a stop link");
            const auto* driver = parameter == nullptr
                                     ? nullptr
                                     : std::get_if<DriverBindingSource>(&parameter->source);
            if (driver == nullptr) {
                continue;
            }
            const auto path = "nodes[" + std::to_string(node.id.value()) + "].parameters[" +
                              binding.role + "].driver";
            if (findNode(driver->sourceNodeId) == nullptr) {
                result.add(ValidationCode::MissingReference, path + ".sourceNodeId",
                           "Driver binding references a missing node");
                continue;
            }
            const auto sourceKind =
                outputKind(OutputPortRef{driver->sourceNodeId, driver->outputPort}, registry);
            // The command path refuses a driver from a sink or from an undeclared output port;
            // a loaded project is held to the same rule.
            const auto* sourceNode = findNode(driver->sourceNodeId);
            const auto* sourceDefinition =
                sourceNode == nullptr
                    ? nullptr
                    : registry.find(sourceNode->typeId, sourceNode->schemaVersion);
            if (sourceDefinition != nullptr && sourceDefinition->outputs.empty()) {
                result.add(ValidationCode::InvalidValue, path + ".sourceNodeId",
                           "This node is a sink and declares no output to drive from");
            } else if (sourceDefinition != nullptr && !sourceKind.has_value() &&
                       !isRerouteNodeType(sourceNode->typeId)) {
                result.add(ValidationCode::MissingReference, path + ".outputPort",
                           "Driver binding references an undeclared output port");
            }
            std::optional<InputPortDefinition> socket;
            if (definition != nullptr)
                socket = findInputPort(*definition, binding.role);
            // #543: a stop link names its component in its schema key and needs its gradient bound.
            if (const auto stop = parseGradientStopPort(binding.role);
                stop && socket &&
                (parameter->schemaKey != gradientStopSchemaKey(stop->component) ||
                 std::ranges::none_of(node.parameters, [&](const ParameterBinding& other) {
                     return other.role == stop->role;
                 })))
                result.add(ValidationCode::InvalidValue, path,
                           "A gradient stop link needs its gradient bound and its stop's schema");
            if (definition != nullptr && !socket) {
                // An inline selector decides which kernel the plan compiles, so it cannot be
                // delivered per frame; it declares no socket, and a driver on it is not a link the
                // editor could have made.
                result.add(ValidationCode::InvalidValue, path,
                           "This parameter role has no linkable socket to drive");
            } else if (sourceKind.has_value() && socket &&
                       !isAcceptedSocketConnection(*sourceKind, socket->valueKind)) {
                result.add(ValidationCode::SocketKindMismatch, path,
                           "Driver binding socket kinds do not match");
            }
            adjacency[driver->sourceNodeId].push_back(node.id);
            ++indegree[node.id];
        }
    }

    for (const auto& stack : layerStacks_) {
        for (const auto& entry : stack.entries()) {
            const auto matching = std::ranges::find_if(edges_, [&](const auto& edge) {
                const auto* input = std::get_if<LayerStackInputRef>(&edge.destination);
                return input && input->stackNodeId == stack.nodeId() &&
                       input->slotId == entry.slotId &&
                       (input->role == kLayerStackContentInputRole ||
                        input->role == kLayerStackAudioInputRole);
            });
            if (matching == edges_.end()) {
                result.add(ValidationCode::InvalidLayerStack, "merges.entries",
                           "Merge slot requires an image or audio edge");
                continue;
            }
            const auto boundary = boundariesByLayer.find(entry.layerId);
            const auto sourceBoundary =
                std::ranges::find_if(layerOutputs_, [&](const LayerOutputBoundary& candidate) {
                    return !candidate.entryOwned() &&
                           candidate.nodeId == matching->source.nodeId;
                });
            // #502: a slot not fed by a Layer node may name its own entry layer.
            const bool ownLayer = boundary != boundariesByLayer.end() &&
                                  boundary->second->entryOwned() &&
                                  boundary->second->nodeId == stack.nodeId() &&
                                  boundary->second->entrySlot == entry.slotId &&
                                  sourceBoundary == layerOutputs_.end();
            if ((entry.layerId.isValid() && !ownLayer &&
                 (boundary == boundariesByLayer.end() ||
                  boundary->second->nodeId != matching->source.nodeId ||
                  boundary->second->outputPort != matching->source.port)) ||
                (!entry.layerId.isValid() && sourceBoundary != layerOutputs_.end()))
                result.add(ValidationCode::InvalidLayerStack, "merges.entries",
                           "Merge slot layer identity must match its image source");
        }
    }

    if (!compositionOutput_.has_value()) {
        result.add(ValidationCode::MissingCompositionOutput, "compositionOutput",
                   "Composition graph requires one explicit output endpoint");
    } else if (!compositionOutput_->nodeId.isValid() ||
               !isValidStructuralText(compositionOutput_->port)) {
        result.add(ValidationCode::InvalidValue, "compositionOutput",
                   "Composition output endpoint is invalid");
    } else if (findNode(compositionOutput_->nodeId) == nullptr) {
        result.add(ValidationCode::MissingReference, "compositionOutput.nodeId",
                   "Composition output references a missing node");
    }

    std::deque<NodeId> ready;
    for (const auto& [nodeId, degree] : indegree) {
        if (degree == 0) {
            ready.push_back(nodeId);
        }
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto nodeId = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto target : adjacency[nodeId]) {
            auto& degree = indegree[target];
            --degree;
            if (degree == 0) {
                ready.push_back(target);
            }
        }
    }
    if (visited != indegree.size()) {
        result.add(ValidationCode::GraphCycle, "edges",
                   "Same-time processing graph must not contain a cycle");
    }

    return result;
}

// A reroute takes the kind of whatever feeds it, following a chain of reroutes to whatever feeds
// the first one (task FIX1, item I). An UNCONNECTED reroute has no kind: std::nullopt is what every
// kind check reads as "nothing to disagree with", so the first link into a fresh reroute is
// accepted whatever it carries and every link after it is checked against what the reroute now
// holds.
//
// `seen` stops a chain that closes on itself. Such a graph is refused by the acyclic rule anyway,
// but this function is also asked about DRAFT graphs mid-edit, where the cycle exists for one call.
std::optional<SocketValueKind>
CanonicalGraph::rerouteKind(const NodeId id, const NodeDefinitionRegistry& registry,
                            std::unordered_set<std::uint64_t>& seen) const {
    if (!seen.insert(id.value()).second) {
        return std::nullopt;
    }
    const auto incoming = std::ranges::find_if(edges_, [id](const auto& edge) {
        const auto* fixed = std::get_if<NodeInputRef>(&edge.destination);
        return fixed != nullptr && fixed->nodeId == id && fixed->port == kValuePortName;
    });
    if (incoming == edges_.end()) {
        return std::nullopt;
    }
    const auto* source = findNode(incoming->source.nodeId);
    if (source == nullptr) {
        return std::nullopt;
    }
    if (isRerouteNodeType(source->typeId)) {
        return rerouteKind(source->id, registry, seen);
    }
    const auto* definition = registry.find(source->typeId, source->schemaVersion);
    if (definition == nullptr) {
        return std::nullopt;
    }
    for (const auto& port : definition->outputs) {
        if (port.name == incoming->source.port) {
            return port.valueKind;
        }
    }
    return std::nullopt;
}

std::optional<SocketValueKind>
CanonicalGraph::rerouteKind(const NodeId id, const NodeDefinitionRegistry& registry) const {
    std::unordered_set<std::uint64_t> seen;
    return rerouteKind(id, registry, seen);
}

std::optional<SocketValueKind>
CanonicalGraph::outputKind(const OutputPortRef& output,
                           const NodeDefinitionRegistry& registry) const {
    const auto* node = findNode(output.nodeId);
    if (node != nullptr && isRerouteNodeType(node->typeId)) {
        return rerouteKind(output.nodeId, registry);
    }
    const auto* definition = node ? registry.find(node->typeId, node->schemaVersion) : nullptr;
    if (definition) {
        for (const auto& port : definition->outputs) {
            if (port.name == output.port)
                return port.valueKind;
        }
    }
    return std::nullopt;
}

std::optional<SocketValueKind>
CanonicalGraph::inputKind(const InputPortRef& input, const NodeDefinitionRegistry& registry) const {
    const auto* node = findNode(destinationNode(input));
    if (node != nullptr && isRerouteNodeType(node->typeId)) {
        return rerouteKind(node->id, registry);
    }
    const auto* definition = node ? registry.find(node->typeId, node->schemaVersion) : nullptr;
    if (definition == nullptr)
        return std::nullopt;
    if (const auto* fixed = std::get_if<NodeInputRef>(&input)) {
        if (const auto port = findInputPort(*definition, fixed->port))
            return port->valueKind;
    } else {
        const auto& layerInput = std::get<LayerStackInputRef>(input);
        if (definition->layerSlotInput && definition->layerSlotInput->role == layerInput.role)
            return definition->layerSlotInput->valueKind;
        if (definition->audioLayerSlotInput &&
            definition->audioLayerSlotInput->role == layerInput.role)
            return definition->audioLayerSlotInput->valueKind;
    }
    return std::nullopt;
}

bool CanonicalGraph::acceptsConnection(const SocketValueKind source, const InputPortRef& input,
                                       const NodeDefinitionRegistry& registry) const {
    const auto target = inputKind(input, registry);
    if (!target || isAcceptedSocketConnection(source, *target))
        return true;
    const auto* fixed = std::get_if<NodeInputRef>(&input);
    const auto* node = fixed ? findNode(fixed->nodeId) : nullptr;
    const auto* definition = node ? registry.find(node->typeId, node->schemaVersion) : nullptr;
    if (definition == nullptr)
        return false;
    const auto port = findInputPort(*definition, fixed->port);
    return port && isAcceptedInputConnection(source, *port);
}

bool dependsOnNode(const CanonicalGraph& graph, const NodeId node, const NodeId upstream) {
    std::vector<NodeId> pending{node};
    std::unordered_set<NodeId> seen;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (id == upstream)
            return true;
        if (!seen.insert(id).second)
            continue;
        for (const auto& edge : graph.edges())
            if (destinationNode(edge.destination) == id)
                pending.push_back(edge.source.nodeId);
    }
    return false;
}

} // namespace bloom::document
