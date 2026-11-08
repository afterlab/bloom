#pragma once

// #569 audio effect nodes: the nodes' definitions. The parameter vocabulary is
// audio_effect_nodes.hpp.

#include <bloom/document/audio_effect_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string audioEffectSchemaKey(std::string_view typeId, std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isAudioEffectParameterValueValid(std::string_view schemaKey,
                                                    const ParameterValue& value) noexcept;
// The definition of the effect node `typeId`; the nodes' shared shape with that node's parameters.
[[nodiscard]] NodeDefinition audioEffectDefinition(std::string_view typeId);

} // namespace bloom::document
