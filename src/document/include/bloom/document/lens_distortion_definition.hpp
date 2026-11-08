#pragma once

// #486 Lens Distortion: the node's definition. The parameter vocabulary is lens_distortion_nodes.hpp.

#include <bloom/document/lens_distortion_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string lensDistortionSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey` (the centre included).
[[nodiscard]] bool isLensDistortionParameterValueValid(std::string_view schemaKey,
                                                       const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition lensDistortionDefinition();

} // namespace bloom::document
