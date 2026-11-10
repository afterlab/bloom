#include <bloom/commands/group_definition_operations.hpp>

#include "node_operation_support.hpp"

#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/persisted_text.hpp>

namespace bloom::commands {

std::string_view CreateGroupDefinition::typeId() const noexcept {
    return "bloom.group-definition.create";
}

OperationResult CreateGroupDefinition::apply(document::Draft& draft) const {
    using namespace document;
    auto* host = draft.project().findComposition(from_);
    if (host == nullptr)
        return detail::missingComposition(from_);
    if (!isValidHumanFacingName(name_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Group name must be valid nonempty UTF-8");
    ParameterId instanceParameter;
    if (instance_.has_value()) {
        const auto* node = host->graph().findNode(*instance_);
        if (node == nullptr || !usesGroupReference(node->typeId))
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "The node does not name a group");
        const auto roles = groupReferenceRoles(node->typeId);
        if (std::ranges::find(roles, std::string_view(role_)) == roles.end())
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "The node does not name a group in " + role_);
        const auto binding = std::ranges::find(node->parameters, role_, &ParameterBinding::role);
        if (binding == node->parameters.end())
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "The Node Group has no group parameter");
        instanceParameter = binding->parameterId;
    }

    const auto compositionId = draft.ids().allocateComposition();
    const auto stackNode = draft.ids().allocateNode();
    const auto outputNode = draft.ids().allocateNode();
    const auto outputEdge = draft.ids().allocateEdge();
    const auto inputNode = draft.ids().allocateNode();
    const auto groupOutputNode = draft.ids().allocateNode();
    if (!compositionId || !stackNode || !outputNode || !outputEdge || !inputNode ||
        !groupOutputNode)
        return detail::exhaustedIds("composition, node and edge");

    CanonicalGraph graph(*stackNode);
    if (!graph.addNode({*stackNode, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}) ||
        !graph.addNode({*outputNode, std::string(kCompositionOutputNodeType), {},
                        kCompositionOutputNodeSchemaVersion}) ||
        !graph.addEdge({*outputEdge,
                        {*stackNode, std::string(kLayerStackOutputPort)},
                        NodeInputRef{*outputNode, std::string(kCompositionOutputInputPort)}}))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Group topology could not be created");
    graph.setCompositionOutput({*outputNode, std::string(kCompositionOutputOutputPort)});

    Composition group(*compositionId, name_, host->duration(), std::move(graph), host->format());
    for (const auto& [typeId, nodeId, x] :
         {std::tuple{kGroupInputNodeType, *inputNode, 0.0},
          std::tuple{kGroupOutputNodeType, *groupOutputNode, 480.0}}) {
        const auto* definition = builtInNodeDefinitions().find(typeId, kNodeGroupNodeSchemaVersion);
        if (definition == nullptr)
            return OperationResult::rejected(OperationIssueCode::Unsupported,
                                             "Group nodes are not registered");
        NodeRecord node{nodeId, std::string(typeId), {}, kNodeGroupNodeSchemaVersion};
        for (const auto& parameter : definition->parameters) {
            const auto parameterId = draft.ids().allocateParameter();
            if (!parameterId)
                return detail::exhaustedIds("parameter");
            if (!group.parameters().insert({*parameterId, parameter.schemaKey,
                                            ConstantValueSource{parameter.defaultValue}}))
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "Group node default violates its parameter schema");
            node.parameters.push_back({parameter.role, *parameterId});
        }
        if (!group.graph().addNode(std::move(node)))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Group node could not be inserted");
        group.nodeLayout()[nodeId] = {Vec2d{x, 0.0}, 128.0, false, false};
    }
    if (!draft.project().addComposition(std::move(group)))
        return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                         "Group could not be added");
    if (instance_.has_value()) {
        // The host is looked up again: adding a composition may have moved the storage.
        auto* current = draft.project().findComposition(from_);
        if (current == nullptr ||
            !current->parameters().setSource(
                instanceParameter,
                ConstantValueSource{static_cast<std::int64_t>(compositionId->value())}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "The Node Group could not be pointed at the group");
    }
    return OperationResult::applied({{std::string(kCreateGroupDefinitionOutput), *compositionId},
                                     {std::string(kCreateGroupDefinitionInputNodeOutput), *inputNode},
                                     {std::string(kCreateGroupDefinitionOutputNodeOutput),
                                      *groupOutputNode}});
}

} // namespace bloom::commands
