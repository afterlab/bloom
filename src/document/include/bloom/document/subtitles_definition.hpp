#pragma once

// #752 Subtitles: the node's definition. The parameter vocabulary is subtitles_nodes.hpp.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/subtitles_nodes.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string subtitlesSchemaKey(std::string_view role);
// The default of a Color parameter of the table.
[[nodiscard]] core::Color4d subtitlesDefaultColor(std::string_view role) noexcept;
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isSubtitlesParameterValueValid(std::string_view schemaKey,
                                                  const ParameterValue& value);
[[nodiscard]] NodeDefinition subtitlesDefinition();

} // namespace bloom::document
