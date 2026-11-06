#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bloom::document {

// The groups an artist finds a node under: the Add menu, the Add search's headings, the node card's
// eyebrow and the scripting/MCP `category` all read this one tree (#554).
//
// A new node joins by adding its type id to its group's list in node_groups.cpp, which also lists
// the nodes each group is waiting for. A registered node that is in no list fails
// node_group_tests. The group is presentation only: a document stores the node's type id, so moving
// a node between groups needs no migration and can never change a project. NodeCategory, which
// decides a node's effect mask and how that mask lowers, is a separate thing a node also declares.
//
// Declared in menu order, which reads from what makes an image to what consumes one. A group with
// no registered node is not offered anywhere.
enum class NodeGroup : std::uint8_t {
    Sources,
    Compositing,
    Channels,
    KeyingMattes,
    Transform,
    Distort,
    BlurSharpen,
    LightShadow,
    Stylize,
    ColorCorrection,
    ColorManagement,
    Time,
    TextPaths,
    InstancesParticles,
    TrackingAnalysis,
    Fields,
    // The eight value groups, which the Add menu nests under one Values submenu. The Add search
    // still lists them as sections of their own.
    Values,
    Math,
    Convert,
    String,
    Logic,
    TimeValues,
    ColorValues,
    Vector,
    Utilities,
    Output,
};

inline constexpr std::array kNodeGroups{NodeGroup::Sources,
                                        NodeGroup::Compositing,
                                        NodeGroup::Channels,
                                        NodeGroup::KeyingMattes,
                                        NodeGroup::Transform,
                                        NodeGroup::Distort,
                                        NodeGroup::BlurSharpen,
                                        NodeGroup::LightShadow,
                                        NodeGroup::Stylize,
                                        NodeGroup::ColorCorrection,
                                        NodeGroup::ColorManagement,
                                        NodeGroup::Time,
                                        NodeGroup::TextPaths,
                                        NodeGroup::InstancesParticles,
                                        NodeGroup::TrackingAnalysis,
                                        NodeGroup::Fields,
                                        NodeGroup::Values,
                                        NodeGroup::Math,
                                        NodeGroup::Convert,
                                        NodeGroup::String,
                                        NodeGroup::Logic,
                                        NodeGroup::TimeValues,
                                        NodeGroup::ColorValues,
                                        NodeGroup::Vector,
                                        NodeGroup::Utilities,
                                        NodeGroup::Output};
static_assert(
    [] {
        for (std::size_t index = 0; index < kNodeGroups.size(); ++index)
            if (static_cast<std::size_t>(kNodeGroups[index]) != index)
                return false;
        return true;
    }(),
    "kNodeGroups lists every NodeGroup in declaration order");

// The artist-facing name ("Blur & Sharpen"). Plain text: a menu has to escape the ampersand.
[[nodiscard]] std::string_view nodeGroupName(NodeGroup group) noexcept;
// The scripting/MCP spelling ("blur-sharpen"), the `category` of a node read back.
[[nodiscard]] std::string_view nodeGroupWireName(NodeGroup group) noexcept;
// Whether the Add menu nests this group under its Values submenu.
[[nodiscard]] constexpr bool isValueSubgroup(const NodeGroup group) noexcept {
    return group >= NodeGroup::Values && group <= NodeGroup::Vector;
}
// The type ids listed in this group, whether or not each is registered.
[[nodiscard]] std::span<const std::string_view> nodeGroupMembers(NodeGroup group) noexcept;
// A node type's group; empty for a type that is in no group, which node_group_tests refuses for
// every registered node.
[[nodiscard]] std::optional<NodeGroup> nodeGroupOf(std::string_view typeId) noexcept;

} // namespace bloom::document
