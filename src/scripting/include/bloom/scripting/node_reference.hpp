#pragma once

// The node and parameter vocabulary every agent surface reads back: node types with their ports
// and parameters, each parameter's unit, range, default and closed values, and the wire names of
// the value kinds. Qt-free; the JSON returned here is what MCP's query node-types and parameter
// metadata publish, and nodeReferenceMarkdown() renders the same data.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::scripting {

// One member of a closed parameter vocabulary: the stored integer and the label the Properties
// panel shows ("Screen" is blend mode 3).
struct ParameterChoice final {
    std::int64_t value = 0;
    std::string label;
};

// The closed values of a schema key in the order every editing surface offers them; empty for a
// parameter that is not an enumeration.
[[nodiscard]] std::vector<ParameterChoice> parameterChoices(std::string_view schemaKey);

// "bloom.gaussian-blur" -> "Gaussian Blur".
[[nodiscard]] std::string nodeTypeDisplayName(std::string_view typeId);
[[nodiscard]] std::string_view nodeCategoryName(document::NodeCategory category) noexcept;
[[nodiscard]] std::string_view socketKindName(document::SocketValueKind kind) noexcept;
[[nodiscard]] std::string_view parameterKindName(document::ParameterValueKind kind) noexcept;
[[nodiscard]] std::string_view assetKindName(std::uint8_t kind) noexcept;

// A parameter value as JSON: numbers, [x, y], [x, y, z], [r, g, b, a], text, booleans, or a path
// object {closed, anchors}.
[[nodiscard]] std::string parameterValueJson(const document::ParameterValue& value);

// A time counted in frames of a rate: exact when the time is on the frame grid.
struct FrameCount final {
    bool exact = false;
    std::int64_t frames = 0;
    double approximate = 0.0;
};
[[nodiscard]] FrameCount framesAt(core::RationalTime time, const document::FrameRate& rate);

// {frames, seconds:[n, d]} for a time at a frame rate (frames is an integer when the time is on
// the frame grid, otherwise a decimal).
[[nodiscard]] std::string timeJson(core::RationalTime time, const document::FrameRate& rate);

// What an editor needs to write a parameter: {valueKind, animatable, default, unit, displayScale,
// minimum, maximum, softMinimum, softMaximum, choices}. `definition` may be null (mask and other
// non-node parameters); the schema key alone then answers.
[[nodiscard]] std::string parameterMetadataJson(std::string_view schemaKey,
                                                document::ParameterValueKind kind,
                                                const document::ParameterDefinition* definition,
                                                std::string_view role = {});

// One node type. `full` adds each parameter's metadata.
[[nodiscard]] std::string nodeTypeJson(const document::NodeDefinition& definition, bool full);

// The definition that owns a parameter role, or null.
[[nodiscard]] const document::ParameterDefinition*
findParameterDefinition(const document::NodeDefinition& definition, std::string_view role);

} // namespace bloom::scripting
