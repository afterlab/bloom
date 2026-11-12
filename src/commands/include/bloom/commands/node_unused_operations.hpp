#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <bloom/commands/operation.hpp>

#include <optional>
#include <set>
#include <string_view>

namespace bloom::document {
class Composition;
}

namespace bloom::commands {

// #701: which nodes of a composition do nothing for its output. A node is USED when a path leads
// from it to Composition Output (or to a Group Output / Group Input, which a group definition
// exposes) through graph links, Merge slot links, driver bindings (a value node driving a used
// node's parameter is used, transitively), a layer's parent and a layer's track or mask matte
// source. Muted, collapsed and disabled nodes are still used: this is structure, not rendering.
// An encapsulated Node Group node is a node like any other here; what it contains lives in its
// own composition and is never read or changed.
struct UnusedNodes final {
    // Unused nodes Delete accepts. This is what Delete Unused Nodes removes.
    std::set<document::NodeId> removable;
    // Unused nodes Delete refuses (a locked layer's nodes); they stay.
    std::set<document::NodeId> staying;
};
[[nodiscard]] UnusedNodes findUnusedNodes(const document::Composition& composition);

// Delete Unused Nodes, as one RemoveNodes: links break, parameters a removed node drove return to
// their registered defaults, and layout frames lose the members and an emptied frame goes, exactly
// as Delete does. Nothing unused is a no-change.
class DeleteUnusedNodes final : public Operation {
  public:
    explicit DeleteUnusedNodes(
        document::CompositionId compositionId,
        const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions())
        : compositionId_(compositionId), registry_(registry) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.delete-unused";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return compositionId_;
    }

  private:
    document::CompositionId compositionId_;
    const document::NodeDefinitionRegistry& registry_;
};

} // namespace bloom::commands
