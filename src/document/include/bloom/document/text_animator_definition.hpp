#pragma once

// #758 Text Animator: the node's definition. The parameter vocabulary is text_animator_nodes.hpp.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/text_animator_nodes.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string textAnimatorSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isTextAnimatorParameterValueValid(std::string_view schemaKey,
                                                     const ParameterValue& value);
// The longest cursor character text, in bytes.
inline constexpr std::size_t kTextAnimatorCursorBytes = 16;
[[nodiscard]] NodeDefinition textAnimatorDefinition();

} // namespace bloom::document
