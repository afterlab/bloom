#pragma once

// #348 Corner Pin, ST Map and Displace: the nodes' definitions. The vocabulary is warp_nodes.hpp.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/warp_nodes.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string warpSchemaKey(std::string_view nodeType, std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey` (a corner included).
[[nodiscard]] bool isWarpParameterValueValid(std::string_view schemaKey,
                                             const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition cornerPinDefinition();
[[nodiscard]] NodeDefinition stMapDefinition();
[[nodiscard]] NodeDefinition displaceDefinition();

} // namespace bloom::document
