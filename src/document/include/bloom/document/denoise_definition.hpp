#pragma once

// #514 Denoise & Smooth: the node's definition. The parameter vocabulary is denoise_nodes.hpp.

#include <bloom/document/denoise_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>

#include <string_view>

namespace bloom::document {

[[nodiscard]] bool isDenoiseParameterValueValid(std::string_view schemaKey,
                                                const ParameterValue& value) noexcept;
[[nodiscard]] NodeDefinition denoiseDefinition();

} // namespace bloom::document
