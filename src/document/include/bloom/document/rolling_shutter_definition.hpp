#pragma once

// #568 Rolling Shutter: the node's definition. The parameter vocabulary is rolling_shutter_nodes.hpp.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/rolling_shutter_nodes.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string rollingShutterSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey` (the camera offset included).
[[nodiscard]] bool isRollingShutterParameterValueValid(std::string_view schemaKey,
                                                       const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition rollingShutterDefinition();

} // namespace bloom::document
