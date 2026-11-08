#pragma once

// #563 Codec Degrade: the node's definition. The parameter vocabulary is codec_degrade_nodes.hpp.

#include <bloom/document/codec_degrade_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string codecDegradeSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isCodecDegradeParameterValueValid(std::string_view schemaKey,
                                                     const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition codecDegradeDefinition();

} // namespace bloom::document
