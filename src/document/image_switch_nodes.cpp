#include <bloom/document/image_switch_definition.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>

namespace bloom::document {

bool isImageSwitchParameterValueValid(const std::string_view schemaKey,
                                      const ParameterValue& value) noexcept {
    if (schemaKey == kImageSwitchWrapSchemaKey)
        return std::holds_alternative<bool>(value);
    const auto* number = std::get_if<double>(&value);
    return schemaKey == kImageSwitchIndexSchemaKey && number != nullptr &&
           std::isfinite(*number) && isImageSwitchIndexWithinDomain(*number);
}

NodeDefinition imageSwitchDefinition(const std::string_view typeId) {
    const bool channels = typeId == kChannelSetSwitchNodeType;
    const auto kind = channels ? SocketValueKind::ChannelSet : SocketValueKind::Image;
    NodeDefinition definition{{std::string(channels ? kChannelSetSwitchNodeType : kImageSwitchNodeType),
                               kImageSwitchNodeSchemaVersion},
                              NodeLoweringKind::Switch,
                              {},
                              {{std::string(channels ? kChannelSetSwitchOutputPort
                                                     : kImageSwitchOutputPort),
                                kind},
                               // The input shown, counted from 1 (0 when it shows nothing).
                               {std::string(kImageSwitchSelectedPort), SocketValueKind::Integer}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Compositing};
    for (const auto port : imageSwitchInputPorts(definition.key.typeId))
        definition.inputs.push_back({std::string(port), kind, false});
    // The index has a socket of its own, so an Integer or Scalar value node drives it.
    definition.inputs.push_back({std::string(kImageSwitchIndexRole), SocketValueKind::Scalar, false});
    definition.parameters.push_back({std::string(kImageSwitchIndexRole),
                                     std::string(kImageSwitchIndexSchemaKey),
                                     ParameterValueKind::Float64, true, true,
                                     ParameterValue{kDefaultImageSwitchIndex}});
    definition.parameters.push_back({std::string(kImageSwitchWrapRole),
                                     std::string(kImageSwitchWrapSchemaKey),
                                     ParameterValueKind::Boolean, true, false, ParameterValue{false}});
    return definition;
}

std::optional<std::string_view> imageSwitchConstantPort(const Composition& composition,
                                                       const NodeRecord& node) {
    if (!isImageSwitchNodeType(node.typeId))
        return std::nullopt;
    const auto constantOf = [&](const std::string_view role) -> const ParameterValue* {
        for (const auto& binding : node.parameters) {
            if (binding.role != role)
                continue;
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* constant =
                parameter == nullptr ? nullptr
                                     : std::get_if<ConstantValueSource>(&parameter->source);
            return constant == nullptr ? nullptr : &constant->value;
        }
        return nullptr;
    };
    const auto* index = constantOf(kImageSwitchIndexRole);
    const auto* wrap = constantOf(kImageSwitchWrapRole);
    const auto* number = index == nullptr ? nullptr : std::get_if<double>(index);
    const auto* wraps = wrap == nullptr ? nullptr : std::get_if<bool>(wrap);
    if (number == nullptr)
        return std::nullopt;
    const auto& ports = imageSwitchInputPorts(node.typeId);
    std::size_t count = 0;
    for (const auto& edge : composition.graph().edges()) {
        const auto* input = std::get_if<NodeInputRef>(&edge.destination);
        if (input == nullptr || input->nodeId != node.id)
            continue;
        for (std::size_t position = 0; position < ports.size(); ++position)
            if (input->port == ports[position])
                count = std::max(count, position + 1);
    }
    const auto position = imageSwitchPosition(count, *number, wraps != nullptr && *wraps);
    if (!position)
        return std::nullopt;
    return ports[*position];
}

} // namespace bloom::document
