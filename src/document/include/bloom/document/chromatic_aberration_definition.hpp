#pragma once

// #486 Chromatic Aberration: the node's definition. The parameter vocabulary is
// chromatic_aberration_nodes.hpp.

#include <bloom/document/chromatic_aberration_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string_view>

namespace bloom::document {

// Whether `value` lies in the document domain of the parameter `schemaKey` (the centre included).
[[nodiscard]] bool isChromaticAberrationParameterValueValid(std::string_view schemaKey,
                                                            const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition chromaticAberrationDefinition();

} // namespace bloom::document
