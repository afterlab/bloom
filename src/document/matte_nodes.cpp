#include <bloom/document/matte_nodes.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace bloom::document {

bool takesEffectMask(const NodeDefinition& definition) noexcept {
    const bool category = definition.category == NodeCategory::Grade ||
                          definition.category == NodeCategory::Color ||
                          definition.category == NodeCategory::Filters;
    return definition.lowering == NodeLoweringKind::ImageEffect && category &&
           !definition.inputs.empty() && definition.inputs.front().name == "input" &&
           definition.inputs.front().valueKind == SocketValueKind::Image &&
           definition.outputs ==
               std::vector<OutputPortDefinition>{{"image", SocketValueKind::Image}} &&
           std::ranges::none_of(definition.inputs, [](const auto& input) {
               return input.name == kEffectMaskInputPort;
           });
}

namespace {
[[nodiscard]] InputPortDefinition effectMaskInput() {
    InputPortDefinition input{std::string(kEffectMaskInputPort), SocketValueKind::Mask, false};
    input.acceptsImageMatte = true;
    return input;
}
[[nodiscard]] ParameterDefinition effectMaskChannel() {
    return {std::string(kEffectMaskChannelRole), std::string(kEffectMaskChannelSchemaKey),
            ParameterValueKind::Integer, false, false, std::int64_t{0}};
}
} // namespace

bool hasEffectMask(const NodeDefinition& definition) noexcept {
    return !definition.inputs.empty() && definition.inputs.back() == effectMaskInput() &&
           !definition.parameters.empty() && definition.parameters.back() == effectMaskChannel();
}

void appendEffectMask(NodeDefinition& definition) {
    definition.inputs.push_back(effectMaskInput());
    definition.parameters.push_back(effectMaskChannel());
}

NodeDefinition withoutEffectMask(NodeDefinition definition) {
    if (hasEffectMask(definition)) {
        definition.inputs.pop_back();
        definition.parameters.pop_back();
    }
    return definition;
}

NodeDefinition maskCombineDefinition() {
    return {{std::string(kMaskCombineNodeType), kMaskCombineNodeSchemaVersion},
            NodeLoweringKind::Matte,
            {{std::string(kMaskCombineFirstInputPort), SocketValueKind::Mask, false},
             {std::string(kMaskCombineSecondInputPort), SocketValueKind::Mask, false}},
            {{std::string(kMaskCombineOutputPort), SocketValueKind::Mask}},
            {{std::string(kMaskCombineModeRole), std::string(kMaskCombineModeSchemaKey),
              ParameterValueKind::Integer, true, false,
              static_cast<std::int64_t>(MaskCombineMode::Add)}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Compositing};
}

NodeDefinition imageToMaskDefinition() {
    return {{std::string(kImageToMaskNodeType), kImageToMaskNodeSchemaVersion},
            NodeLoweringKind::Matte,
            {{std::string(kImageToMaskInputPort), SocketValueKind::Image}},
            {{std::string(kImageToMaskOutputPort), SocketValueKind::Mask}},
            {{std::string(kImageToMaskChannelRole), std::string(kImageToMaskChannelSchemaKey),
              ParameterValueKind::Integer, true, false, std::int64_t{0}}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Compositing};
}

} // namespace bloom::document
