#pragma once

// #703: adding a node INTO an existing link. The Node editor's Add placement and the
// bloom.node.insert-on-link operation (MCP and Python) share this one rule, so where a dropped node
// is wired never depends on which surface added it.

#include <bloom/commands/operation.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/node_layout.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::document {
class Composition;
}

namespace bloom::commands {

// The two sockets of a new node a link runs through: the input its old source now feeds and the
// output that now feeds its old destination.
struct LinkInsertionSockets final {
    // An input port name, or the role of the node's layer-slot input (a Merge's content stack)
    // when `layerSlot` is set.
    std::string input;
    bool layerSlot = false;
    std::string output;

    friend bool operator==(const LinkInsertionSockets&, const LinkInsertionSockets&) = default;
};

// The best matching input and output of a node of `definition` for a link that carries `source`
// into a destination of kind `destination` (which `destinationAccepts` answers for, promotions and
// an inspection input's extra kinds included). An input matches when it takes the source kind and
// an output when the destination takes it. A socket of exactly the link's kind beats one that only
// accepts it by conversion; a transport socket beats a parameter's socket; declaration order
// breaks the remaining ties. Empty when either side has no match: the node is then not inserted.
[[nodiscard]] std::optional<LinkInsertionSockets>
linkInsertionSockets(const document::NodeDefinition& definition, document::SocketValueKind source,
                     std::optional<document::SocketValueKind> destination,
                     const std::function<bool(document::SocketValueKind)>& destinationAccepts);

// The same question for a link that exists in `composition`, named by its destination input:
// empty when nothing feeds that input, the type is unknown, or no socket pair matches.
[[nodiscard]] std::optional<LinkInsertionSockets>
linkInsertionSockets(const document::Composition& composition, std::string_view type,
                     const document::InputPortRef& destination);

// What feeds `destination`: its graph edge, or the driver binding of the parameter it names.
[[nodiscard]] std::optional<document::OutputPortRef>
linkSource(const document::Composition& composition, const document::InputPortRef& destination);

// Adds a node of `type` at `position` and inserts it into the link that ends at `destination`: the
// link's source feeds the node's best matching input and the node's best matching output feeds
// the destination. One operation, so the whole add is one undo step. The card lands at `position`
// unless that spot would touch another card (AddNode's placement rule). Outputs: "node" and
// "editorNode" (the new node).
class InsertNodeOnLink final : public Operation {
  public:
    InsertNodeOnLink(document::CompositionId composition, std::string type,
                     document::InputPortRef destination, document::Vec2d position,
                     document::NodeCardSizes sizes = {})
        : composition_(composition), type_(std::move(type)), destination_(std::move(destination)),
          position_(position), sizes_(std::move(sizes)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.insert-on-link";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    std::string type_;
    document::InputPortRef destination_;
    document::Vec2d position_;
    document::NodeCardSizes sizes_;
};

} // namespace bloom::commands
