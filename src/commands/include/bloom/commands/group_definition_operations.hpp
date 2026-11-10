#pragma once

// #342 Reusable groups. A group definition is a composition that holds a Group Output (see
// document/node_group_nodes.hpp); these operations make one and point a Node Group at it. Editing
// what is inside a group is the ordinary node operations on the definition's composition, so one
// edit is one undo step like any other.

#include <bloom/commands/operation.hpp>
#include <bloom/document/ids.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace bloom::commands {

inline constexpr std::string_view kCreateGroupDefinitionOutput = "group";
inline constexpr std::string_view kCreateGroupDefinitionInputNodeOutput = "groupInput";
inline constexpr std::string_view kCreateGroupDefinitionOutputNodeOutput = "groupOutput";

// Adds a group definition holding a Group Input and a Group Output, sized like the composition it is
// made from. When `instance` names a Node Group in that composition, the new group becomes the one
// it reads, in the same step: in its `group` parameter, or in `role` when that names another group
// reference of the node (Warp Path (Function)'s `groupY`).
class CreateGroupDefinition final : public Operation {
  public:
    CreateGroupDefinition(document::CompositionId from, std::string name,
                          std::optional<document::NodeId> instance = std::nullopt,
                          std::string role = "group")
        : from_(from), name_(std::move(name)), instance_(instance), role_(std::move(role)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId from_;
    std::string name_;
    std::optional<document::NodeId> instance_;
    std::string role_;
};

} // namespace bloom::commands
