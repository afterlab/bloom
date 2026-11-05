#pragma once

// #525 Fields: the durable vocabulary of the Fields node family. A field is a lazy, typed function
// of a field position -- composition pixels from the composition's centre, +x right, +y up -- that
// flows over the ScalarField, Vector2Field and ColorField sockets. It is not an Image or Mask: it
// has no data window and never converts implicitly. The one explicit boundary is a raster: the
// Rasterize Field node, or a generator's own `image` and `mask` outputs (the same raster, fused).
// Field Sample reads a field at one position as a value. docs/architecture/fields.md is the
// specification; this table is the one description of every node's sockets and parameters that
// the definitions, validation, presentation, lowering and interface all read.
//
// Schema keys are "<type id>.<role>" (bloom.field-noise.size). Every Scalar and Vector 2 parameter
// marked `animated` is animatable and has a socket of its own (drivable); every Choice, Integer,
// Boolean and Seed is a constant that chooses the arithmetic, with no socket. A Curve is a Curves
// master point list (grade_curves.hpp, at most kMaximumFieldCurvePoints points), animatable as a
// whole value. A Gradient is a Gradient value, animatable as a whole value.

#include <bloom/document/field_parameters.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bloom::document {

// The generators' fused raster outputs, and Rasterize Field's.
inline constexpr std::string_view kFieldImageOutputPort = "image";
inline constexpr std::string_view kFieldMaskOutputPort = "mask";

struct FieldPortSpec final {
    std::string_view name;
    SocketValueKind kind = SocketValueKind::ScalarField;
    // An input that accepts every field kind (it reads the lanes it needs).
    bool anyField = false;
    // An input that accepts an Image or a Mask.
    bool pixels = false;
};

struct FieldNodeSpec final {
    std::string_view typeId;
    NodeLoweringKind lowering = NodeLoweringKind::Field;
    std::span<const FieldPortSpec> inputs;
    std::span<const FieldPortSpec> outputs;
    std::span<const FieldParameterSpec> parameters;
};

// Every Fields node, in menu order.
[[nodiscard]] std::span<const FieldNodeSpec> fieldNodeSpecs() noexcept;
[[nodiscard]] const FieldNodeSpec* fieldNodeSpec(std::string_view typeId) noexcept;
[[nodiscard]] inline bool isFieldNodeType(const std::string_view typeId) noexcept {
    return fieldNodeSpec(typeId) != nullptr;
}
// A node whose outputs are fields and pixel rasters, so it compiles into a field program.
[[nodiscard]] bool isFieldGeneratorNodeType(std::string_view typeId) noexcept;
[[nodiscard]] std::string fieldSchemaKey(std::string_view typeId, std::string_view role);
// Whether `value` lies in the document domain of the Fields parameter `schemaKey`.
[[nodiscard]] bool isFieldParameterValueValid(std::string_view schemaKey,
                                              const ParameterValue& value) noexcept;
// The registered definition of a Fields node.
[[nodiscard]] NodeDefinition fieldNodeDefinition(const FieldNodeSpec& spec);
// The default of a Curve parameter: the identity (0, 0) (1, 1).
[[nodiscard]] PathValue defaultFieldCurve();
// The default of a Gradient parameter: black to white.
[[nodiscard]] GradientValue defaultFieldGradient();

} // namespace bloom::document
