// #534: opening a project binds every optional Glow parameter a node saved before #506 lacks, at
// its definition default, so every Glow shows the same Properties rows and node card sockets. The
// defaults are exactly what the compiler assumes for an unbound parameter, so the plan (and every
// pixel) is unchanged. See docs/architecture/project-format.md, "Glow Controls".

#include <bloom/project/document_decode.hpp>

#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <algorithm>
#include <optional>
#include <string>

namespace bloom::project {

void migrateLegacyGlowNodes(DecodedDocumentEnvelope& envelope) {
    const auto* definition = document::builtInNodeDefinitions().find(document::kGlowNodeType, 1);
    if (definition == nullptr)
        return;
    auto ids = document::IdAllocator::fromHighWater(envelope.highWater);
    for (auto& composition : envelope.compositions)
        for (auto& node : composition.graph.nodes) {
            if (node.typeId != document::kGlowNodeType || node.schemaVersion != 1)
                continue;
            bool added = false;
            for (const auto& parameter : definition->parameters) {
                if (parameter.required ||
                    std::ranges::find(node.parameters, parameter.role,
                                      &document::ParameterBinding::role) != node.parameters.end())
                    continue;
                // An exhausted allocator leaves the node as saved: it still renders as Classic.
                const auto id = ids.allocateParameter();
                if (!id)
                    break;
                composition.parameters.push_back(
                    {*id, parameter.schemaKey,
                     document::ConstantValueSource{parameter.defaultValue}});
                node.parameters.push_back({parameter.role, *id});
                added = true;
            }
            // Bindings are held in role order, as a decoded node's are.
            if (added)
                std::ranges::sort(node.parameters, {}, &document::ParameterBinding::role);
        }
    envelope.highWater.parameter = ids.highWater().parameter;
}

} // namespace bloom::project
