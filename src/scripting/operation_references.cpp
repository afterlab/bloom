#include "operation_references.hpp"

#include "operation_values.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting::detail {
namespace {

constexpr std::size_t kListed = 12;

struct Resolver final {
    const OperationSchema& schema;
    const document::Snapshot& snapshot;
    const document::Composition& composition;

    [[nodiscard]] OperationDiagnostic refuse(const std::string& argument,
                                             std::string message) const {
        return {.code = "bloom.scripting.invalid-reference",
                .operationId = schema.typeId,
                .argument = argument,
                .message = std::move(message)};
    }

    [[nodiscard]] std::string where() const {
        return "composition " + std::to_string(composition.id().value());
    }

    [[nodiscard]] static std::string describe(const document::LayerOutputBoundary& layer) {
        return "\"" + layer.name + "\" (id " + std::to_string(layer.layerId.value()) + ")";
    }

    // The layer a name or id denotes, or the refusal listing the candidates.
    [[nodiscard]] std::variant<const document::LayerOutputBoundary*, OperationDiagnostic>
    layer(const std::string& argument, const std::optional<std::string>& name,
          const std::optional<std::uint64_t>& id) const {
        const auto& layers = composition.graph().layerOutputs();
        if (id) {
            const auto* found = composition.graph().findLayer(document::LayerId::fromRaw(*id));
            if (found == nullptr)
                return refuse(argument, "Layer " + std::to_string(*id) + " does not exist in " +
                                            where());
            return found;
        }
        if (!name)
            return refuse(argument, "A layer reference needs a name, as {\"name\": \"Shot 2\"}");
        std::vector<const document::LayerOutputBoundary*> matches;
        for (const auto& candidate : layers)
            if (candidate.name == *name)
                matches.push_back(&candidate);
        if (matches.size() == 1)
            return matches.front();
        std::string listed;
        std::size_t count = 0;
        const auto add = [&](const document::LayerOutputBoundary& candidate) {
            if (count++ < kListed)
                listed += (listed.empty() ? "" : ", ") + describe(candidate);
        };
        if (matches.empty()) {
            for (const auto& candidate : layers)
                add(candidate);
            if (count > kListed)
                listed += ", and " + std::to_string(count - kListed) + " more";
            return refuse(argument, "No layer is named \"" + *name + "\" in " + where() +
                                        (layers.empty() ? "; it has no layers."
                                                        : "; its layers are " + listed + "."));
        }
        for (const auto* match : matches)
            add(*match);
        return refuse(argument, std::to_string(matches.size()) + " layers are named \"" + *name +
                                    "\" in " + where() + ": " + listed +
                                    ". Pass the layer by id.");
    }

    // The node feeding a layer's content (else audio) input: the layer's source.
    [[nodiscard]] const document::NodeRecord*
    sourceNode(const document::LayerOutputBoundary& layer) const {
        const auto& graph = composition.graph();
        if (layer.entryOwned()) {
            if (const auto* edge =
                    document::mergeSlotImageEdge(graph, layer.nodeId, *layer.entrySlot))
                return graph.findNode(edge->source.nodeId);
            return nullptr;
        }
        for (const std::string_view port :
             {document::kLayerOutputContentInputPort, document::kLayerOutputAudioInputPort})
            for (const auto& edge : graph.edges()) {
                const auto* target = std::get_if<document::NodeInputRef>(&edge.destination);
                if (target != nullptr && target->nodeId == layer.nodeId && target->port == port)
                    return graph.findNode(edge.source.nodeId);
            }
        return nullptr;
    }

    [[nodiscard]] std::variant<std::uint64_t, OperationDiagnostic>
    parameter(const std::string& argument, const ObjectReference& reference) const {
        if (!reference.role || (!reference.layerName && !reference.layerId))
            return refuse(argument,
                          "A parameter reference needs the layer and the role, as "
                          "{\"layer\": \"Shot 2\", \"role\": \"opacity\"}");
        const auto found = layer(argument, reference.layerName, reference.layerId);
        if (const auto* failure = std::get_if<OperationDiagnostic>(&found))
            return *failure;
        const auto& owner = *std::get<const document::LayerOutputBoundary*>(found);
        const auto& graph = composition.graph();
        for (const auto& binding : document::layerParameterBindings(graph, owner))
            if (binding.role == *reference.role)
                return binding.parameterId.value();
        const auto* source = sourceNode(owner);
        if (source != nullptr)
            for (const auto& binding : source->parameters)
                if (binding.role == *reference.role)
                    return binding.parameterId.value();
        std::string roles;
        for (const auto& binding : document::layerParameterBindings(graph, owner))
            roles += (roles.empty() ? "" : ", ") + binding.role;
        std::string sourceRoles;
        if (source != nullptr)
            for (const auto& binding : source->parameters)
                sourceRoles += (sourceRoles.empty() ? "" : ", ") + binding.role;
        return refuse(argument, "Layer " + describe(owner) + " has no parameter with the role \"" +
                                    *reference.role + "\". Its parameters are: " + roles +
                                    (sourceRoles.empty()
                                         ? std::string(".")
                                         : "; its source's are: " + sourceRoles + "."));
    }
};

[[nodiscard]] bool takesLayer(const OperationSchema& schema, const ArgumentSchema& argument) {
    if (argument.name == "layer")
        return argument.kind == ValueKind::Id;
    if (argument.name == "parent")
        return argument.kind == ValueKind::Id && schema.typeId.starts_with("bloom.layer.");
    return argument.name == "layers" && argument.kind == ValueKind::Array;
}

} // namespace

std::optional<OperationDiagnostic> resolveObjectReferences(const OperationSchema& schema,
                                                           Arguments& arguments,
                                                           const OperationContext& context) {
    const auto holdsReference = [](const Value& value) {
        return std::holds_alternative<ObjectReference>(value.storage);
    };
    bool any = false;
    for (const auto& [name, value] : arguments) {
        if (holdsReference(value)) {
            any = true;
        } else if (const auto* list = std::get_if<ValueArray>(&value.storage)) {
            any = any || std::ranges::any_of(*list, holdsReference);
        }
    }
    if (!any)
        return std::nullopt;
    const auto refuse = [&](const std::string& argument, std::string message) {
        return OperationDiagnostic{.code = "bloom.scripting.invalid-reference",
                                   .operationId = schema.typeId,
                                   .argument = argument,
                                   .message = std::move(message)};
    };
    // A reference outside the arguments that take one is refused, not passed on as an id.
    for (const auto& [name, value] : arguments) {
        const auto schemaArgument =
            std::ranges::find(schema.arguments, name, &ArgumentSchema::name);
        const bool ok = schemaArgument != schema.arguments.end() &&
                        (takesLayer(schema, *schemaArgument) ||
                         (name == "parameter" && schemaArgument->kind == ValueKind::Id));
        const bool listed = std::holds_alternative<ValueArray>(value.storage);
        const bool has = holdsReference(value) ||
                         (listed && std::ranges::any_of(std::get<ValueArray>(value.storage),
                                                        holdsReference));
        if (has && !ok)
            return refuse(name, "\"" + name + "\" takes ids; only layer, layers, parent (of a "
                                "layer operation) and parameter also accept a name reference");
    }
    if (!context.source)
        return refuse({}, "A name reference needs the project, which this client cannot read; "
                          "pass the id instead");
    const auto source = context.source();
    if (!source)
        return refuse({}, "A name reference needs an open project");
    std::optional<std::uint64_t> scope;
    if (const auto found = arguments.find("composition"); found != arguments.end())
        scope = idOf(found->second);
    for (const auto& [name, value] : arguments)
        if (const auto* reference = std::get_if<ObjectReference>(&value.storage);
            reference != nullptr && reference->composition)
            scope = reference->composition;
    if (!scope)
        scope = context.composition;
    const auto* composition =
        scope ? source->snapshot.project().findComposition(document::CompositionId::fromRaw(*scope))
              : nullptr;
    if (composition == nullptr)
        return refuse({}, scope ? "Composition " + std::to_string(*scope) +
                                      " does not exist, so a name reference has nothing to search"
                                : std::string("A name reference is scoped to a composition; pass "
                                              "composition"));
    const Resolver resolver{schema, source->snapshot, *composition};
    const auto resolveOne = [&](const std::string& argument, const Value& value,
                                const bool parameter) -> std::variant<Value, OperationDiagnostic> {
        const auto* reference = std::get_if<ObjectReference>(&value.storage);
        if (reference == nullptr)
            return value;
        if (parameter) {
            auto resolved = resolver.parameter(argument, *reference);
            if (const auto* failure = std::get_if<OperationDiagnostic>(&resolved))
                return *failure;
            return Value(StableId{std::get<std::uint64_t>(resolved)});
        }
        auto resolved = resolver.layer(argument, reference->name, std::nullopt);
        if (const auto* failure = std::get_if<OperationDiagnostic>(&resolved))
            return *failure;
        return Value(
            StableId{std::get<const document::LayerOutputBoundary*>(resolved)->layerId.value()});
    };
    for (auto& [name, value] : arguments) {
        const bool parameter = name == "parameter";
        if (const auto* list = std::get_if<ValueArray>(&value.storage)) {
            ValueArray resolved;
            for (const auto& item : *list) {
                auto one = resolveOne(name, item, false);
                if (auto* failure = std::get_if<OperationDiagnostic>(&one))
                    return *failure;
                resolved.push_back(std::get<Value>(std::move(one)));
            }
            value = Value(std::move(resolved));
        } else {
            auto one = resolveOne(name, value, parameter);
            if (auto* failure = std::get_if<OperationDiagnostic>(&one))
                return *failure;
            value = std::get<Value>(std::move(one));
        }
    }
    return std::nullopt;
}

} // namespace bloom::scripting::detail
