#pragma once

// Four-Color Gradient: the node's definition. The parameter vocabulary is
// four_color_gradient_nodes.hpp.

#include <bloom/document/four_color_gradient_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string_view>

namespace bloom::document {

// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isFourColorGradientParameterValueValid(std::string_view schemaKey,
                                                          const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition fourColorGradientDefinition();

} // namespace bloom::document
