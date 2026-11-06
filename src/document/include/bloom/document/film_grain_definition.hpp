#pragma once

// #486 Film Grain: the node's definition. The parameter vocabulary is film_grain_nodes.hpp.

#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string filmGrainSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isFilmGrainParameterValueValid(std::string_view schemaKey,
                                                  const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition filmGrainDefinition();

} // namespace bloom::document
