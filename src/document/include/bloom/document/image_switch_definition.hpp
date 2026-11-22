#pragma once

// #531 Image Switch and Channel Set Switch: the nodes' definitions. The parameter vocabulary is
// image_switch_nodes.hpp.

#include <bloom/document/image_switch_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>

#include <optional>
#include <string_view>

namespace bloom::document {

[[nodiscard]] bool isImageSwitchParameterValueValid(std::string_view schemaKey,
                                                    const ParameterValue& value) noexcept;
// The definition of `typeId` (kImageSwitchNodeType or kChannelSetSwitchNodeType).
[[nodiscard]] NodeDefinition imageSwitchDefinition(std::string_view typeId);

class Composition;
struct NodeRecord;
// The input port a switch `node` shows at every frame because its index is a constant, or nullopt
// when its index is animated or driven (the plan chooses per frame) or none of its inputs is
// connected. The inputs counted are the first up to the last connected one. A constant switch reads
// only this input: the compiler and render reachability consume no other input edge, so nothing
// upstream of the other inputs is compiled, validated or evaluated.
[[nodiscard]] std::optional<std::string_view> imageSwitchConstantPort(const Composition& composition,
                                                                      const NodeRecord& node);

} // namespace bloom::document
