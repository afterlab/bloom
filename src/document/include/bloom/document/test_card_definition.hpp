#pragma once

// #587 Test Card: the node's definition. The parameter vocabulary is test_card_nodes.hpp.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/test_card_nodes.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string testCardSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isTestCardParameterValueValid(std::string_view schemaKey,
                                                 const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition testCardDefinition();

} // namespace bloom::document
