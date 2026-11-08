#pragma once

// #508 Light Rays: the node's definition. The parameter vocabulary is light_rays_nodes.hpp.

#include <bloom/document/light_rays_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string_view>

namespace bloom::document {

// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isLightRaysParameterValueValid(std::string_view schemaKey,
                                                  const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition lightRaysDefinition();

} // namespace bloom::document
