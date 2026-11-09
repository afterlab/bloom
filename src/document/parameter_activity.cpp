#include <bloom/document/parameter_activity.hpp>

#include <bloom/document/denoise_nodes.hpp>

#include <cstdint>
#include <optional>
#include <variant>

namespace bloom::document {
namespace {

// A constant integer or switch of `node` (a switch reads 1 or 0); none when it is not a constant.
[[nodiscard]] std::optional<std::int64_t> constantOf(const Composition& composition,
                                                     const NodeRecord& node,
                                                     const std::string_view role) {
    for (const auto& binding : node.parameters)
        if (binding.role == role) {
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* constant =
                parameter ? std::get_if<ConstantValueSource>(&parameter->source) : nullptr;
            if (const auto* integer = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr)
                return *integer;
            if (const auto* flag = constant ? std::get_if<bool>(&constant->value) : nullptr)
                return *flag ? 1 : 0;
        }
    return std::nullopt;
}

} // namespace

bool isParameterInactive(const Composition& composition, const NodeRecord& node,
                         const std::string_view schemaKey) {
    if (schemaKey == "bloom.blur.radius-y")
        return constantOf(composition, node, "separate").value_or(0) == 0;
    if (schemaKey == "bloom.blur.red-scale" || schemaKey == "bloom.blur.green-scale" ||
        schemaKey == "bloom.blur.blue-scale" || schemaKey == "bloom.blur.alpha-scale")
        return constantOf(composition, node, "perChannel").value_or(0) == 0;
    if (node.typeId != kDenoiseNodeType)
        return false;
    constexpr auto temporal = static_cast<std::int64_t>(DenoiseNodeMode::Temporal);
    const bool automatic = constantOf(composition, node, "autoStrength").value_or(0) != 0;
    if (schemaKey == kDenoiseFramesSchemaKey)
        return constantOf(composition, node, "mode").value_or(0) != temporal;
    // Temporal compares 3 x 3 patches at the same place in each frame: it has no search radius.
    if (schemaKey == kDenoiseRadiusSchemaKey)
        return constantOf(composition, node, "mode").value_or(0) == temporal;
    if (schemaKey == kDenoiseStrengthSchemaKey)
        return automatic;
    if (schemaKey == kDenoiseSampleFromSchemaKey || schemaKey == kDenoiseSampleToSchemaKey)
        return !automatic;
    return false;
}

} // namespace bloom::document
