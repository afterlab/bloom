#pragma once

// #566 Auto Exposure: the node's definition. The parameter vocabulary is auto_exposure_nodes.hpp.

#include <bloom/document/auto_exposure_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <string>
#include <string_view>

namespace bloom::document {

[[nodiscard]] std::string autoExposureSchemaKey(std::string_view role);
// Whether `value` lies in the document domain of the parameter `schemaKey`.
[[nodiscard]] bool isAutoExposureParameterValueValid(std::string_view schemaKey,
                                                     const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition autoExposureDefinition();

} // namespace bloom::document
