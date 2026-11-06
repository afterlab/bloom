#pragma once

// #486 Sharpen: the node's definition. The parameter vocabulary is sharpen_nodes.hpp.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/sharpen_nodes.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string sharpenSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isSharpenParameterValueValid(std::string_view schemaKey,
                                                const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition sharpenDefinition();

} // namespace bloom::document
