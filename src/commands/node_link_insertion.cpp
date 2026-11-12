#include <bloom/commands/node_link_insertion.hpp>

#include <bloom/commands/node_operations.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <tuple>
#include <utility>
#include <variant>

namespace bloom::commands {
namespace {

// The newest registered schema of `type`, as Add creates it.
[[nodiscard]] const document::NodeDefinition* newestDefinition(const std::string_view type) {
    const document::NodeDefinition* found = nullptr;
    for (const auto& candidate : document::builtInNodeDefinitions().definitions())
        if (candidate.key.typeId == type &&
            (found == nullptr || candidate.key.schemaVersion > found->key.schemaVersion))
            found = &candidate;
    return found;
}

// Lower is better: (conversion needed, parameter socket, declaration order).
using Rank = std::tuple<int, int, std::size_t>;

} // namespace

std::optional<LinkInsertionSockets>
linkInsertionSockets(const document::NodeDefinition& definition,
                     const document::SocketValueKind source,
                     const std::optional<document::SocketValueKind> destination,
                     const std::function<bool(document::SocketValueKind)>& destinationAccepts) {
    std::optional<std::pair<Rank, LinkInsertionSockets>> input;
    const auto offerInput = [&](const Rank rank, std::string name, const bool layerSlot) {
        if (!input || rank < input->first)
            input = std::pair{rank, LinkInsertionSockets{std::move(name), layerSlot, {}}};
    };
    // A Merge's content stack takes an image as a new slot, ahead of every declared port.
    if (definition.layerSlotInput && source == definition.layerSlotInput->valueKind)
        offerInput({0, 0, 0}, definition.layerSlotInput->role, true);
    for (std::size_t index = 0; index < definition.inputs.size(); ++index) {
        const auto& port = definition.inputs[index];
        if (!document::isAcceptedInputConnection(source, port))
            continue;
        const bool parameter =
            std::ranges::any_of(definition.parameters,
                                [&](const auto& candidate) { return candidate.role == port.name; });
        offerInput({port.valueKind == source ? 0 : 1, parameter ? 1 : 0, index + 1}, port.name,
                   false);
    }
    if (!input)
        return std::nullopt;
    std::optional<std::pair<Rank, std::string>> output;
    for (std::size_t index = 0; index < definition.outputs.size(); ++index) {
        const auto& port = definition.outputs[index];
        const bool exact = destination.has_value() && port.valueKind == *destination;
        if (!exact && !(destinationAccepts && destinationAccepts(port.valueKind)))
            continue;
        const Rank rank{exact ? 0 : 1, 0, index};
        if (!output || rank < output->first)
            output = std::pair{rank, port.name};
    }
    if (!output)
        return std::nullopt;
    auto sockets = std::move(input->second);
    sockets.output = std::move(output->second);
    return sockets;
}

std::optional<document::OutputPortRef> linkSource(const document::Composition& composition,
                                                  const document::InputPortRef& destination) {
    for (const auto& edge : composition.graph().edges())
        if (edge.destination == destination)
            return edge.source;
    const auto* fixed = std::get_if<document::NodeInputRef>(&destination);
    const auto* node = fixed != nullptr ? composition.graph().findNode(fixed->nodeId) : nullptr;
    if (node == nullptr)
        return std::nullopt;
    const auto binding =
        std::ranges::find(node->parameters, fixed->port, &document::ParameterBinding::role);
    if (binding == node->parameters.end())
        return std::nullopt;
    const auto* parameter = composition.parameters().find(binding->parameterId);
    const auto* driver =
        parameter != nullptr ? std::get_if<document::DriverBindingSource>(&parameter->source)
                             : nullptr;
    if (driver == nullptr)
        return std::nullopt;
    return document::OutputPortRef{driver->sourceNodeId, driver->outputPort};
}

std::optional<LinkInsertionSockets>
linkInsertionSockets(const document::Composition& composition, const std::string_view type,
                     const document::InputPortRef& destination) {
    const auto* definition = newestDefinition(type);
    const auto source = linkSource(composition, destination);
    if (definition == nullptr || !source)
        return std::nullopt;
    const auto& graph = composition.graph();
    const auto sourceKind = graph.outputKind(*source);
    if (!sourceKind)
        return std::nullopt;
    return linkInsertionSockets(
        *definition, *sourceKind, graph.inputKind(destination),
        [&](const document::SocketValueKind kind) {
            return graph.inputKind(destination).has_value() &&
                   graph.acceptsConnection(kind, destination);
        });
}

OperationResult InsertNodeOnLink::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition " + std::to_string(composition_.value()) +
                                             " does not exist");
    const auto source = linkSource(*composition, destination_);
    if (!source)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "That link no longer exists");
    if (newestDefinition(type_) == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Unknown node type " + type_);
    const auto sockets = linkInsertionSockets(*composition, type_, destination_);
    if (!sockets)
        return OperationResult::rejected(
            OperationIssueCode::Unsupported,
            "The node has no input for what the link carries and no output its destination takes");
    AddNode add(composition_, type_, position_);
    add.setMeasuredSizes(sizes_);
    auto added = add.apply(draft);
    if (added.status == OperationStatus::Rejected)
        return added;
    std::optional<document::NodeId> node;
    for (const auto& item : added.outputs)
        if (const auto* id = std::get_if<document::NodeId>(&item.id);
            id != nullptr && item.name == kAddNodeOutput)
            node = *id;
    if (!node)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Node command returned no node identity");
    const document::InputPortRef input =
        sockets->layerSlot
            ? document::InputPortRef{document::LayerStackInputRef{*node, {}, sockets->input}}
            : document::InputPortRef{document::NodeInputRef{*node, sockets->input}};
    auto upstream = ConnectPorts(composition_, *source, input).apply(draft);
    if (upstream.status == OperationStatus::Rejected)
        return upstream;
    auto downstream =
        ConnectPorts(composition_, document::OutputPortRef{*node, sockets->output}, destination_)
            .apply(draft);
    if (downstream.status == OperationStatus::Rejected)
        return downstream;
    auto result = std::move(added);
    result.outputs.push_back({"editorNode", *node});
    return result;
}

} // namespace bloom::commands
