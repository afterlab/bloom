#pragma once

// #481 Image Math, Image Map Range and Image Clamp: the node definitions and value checks. The
// parameter vocabulary is image_math_parameters.hpp.

#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string imagePixelSchemaKey(std::string_view typeId, std::string_view role);
// Whether `value` lies in the document domain of the image pixel parameter `schemaKey`.
[[nodiscard]] bool isImagePixelParameterValueValid(std::string_view schemaKey,
                                                   const ParameterValue& value) noexcept;
// The registered definition of an image pixel node (before the effect mask is appended).
[[nodiscard]] NodeDefinition imagePixelDefinition(std::string_view typeId);

} // namespace bloom::document
