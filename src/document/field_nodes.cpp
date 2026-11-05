#include "bloom/document/field_nodes.hpp"

#include "bloom/document/grade_curves.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace bloom::document {

namespace {

using Kind = FieldParameterKind;
using Socket = SocketValueKind;
using namespace field_parameters;

constexpr FieldPortSpec port(const std::string_view name, const Socket kind) {
    return {name, kind, false, false};
}
constexpr FieldPortSpec anyField(const std::string_view name) {
    return {name, Socket::ScalarField, true, false};
}

// --- The nodes ---------------------------------------------------------------------------------

constexpr std::array<FieldPortSpec, 0> kNoPorts{};

constexpr std::array kNoiseOutputs{port("scalar", Socket::ScalarField),
                                   port("vector", Socket::Vector2Field),
                                   port("gradient", Socket::Vector2Field),
                                   port("curl", Socket::Vector2Field),
                                   port("image", Socket::Image), port("mask", Socket::Mask)};

constexpr std::array kVoronoiOutputs{
    port("distance", Socket::ScalarField),    port("cellId", Socket::ScalarField),
    port("cellPosition", Socket::Vector2Field), port("direction", Socket::Vector2Field),
    port("edgeDistance", Socket::ScalarField), port("image", Socket::Image),
    port("mask", Socket::Mask)};

constexpr std::array kRandomOutputs{port("scalar", Socket::ScalarField),
                                    port("vector", Socket::Vector2Field),
                                    port("color", Socket::ColorField),
                                    port("image", Socket::Image), port("mask", Socket::Mask)};

constexpr std::array kPatternOutputs{port("scalar", Socket::ScalarField),
                                     port("image", Socket::Image), port("mask", Socket::Mask)};

constexpr std::array kGradientOutputs{port("t", Socket::ScalarField),
                                      port("image", Socket::Image), port("mask", Socket::Mask)};

constexpr std::array kShapeInputs{FieldPortSpec{"maskInput", Socket::Mask, false, true}};
constexpr std::array kShapeOutputs{port("distance", Socket::ScalarField),
                                   port("falloff", Socket::ScalarField),
                                   port("direction", Socket::Vector2Field),
                                   port("image", Socket::Image), port("mask", Socket::Mask)};

constexpr std::array kImageInputs{FieldPortSpec{"image", Socket::Image, false, true},
                                  port("position", Socket::Vector2Field)};
constexpr std::array kImageOutputs{port("scalar", Socket::ScalarField),
                                   port("vector", Socket::Vector2Field),
                                   port("color", Socket::ColorField)};

constexpr std::array kForceOutputs{port("vector", Socket::Vector2Field),
                                   port("magnitude", Socket::ScalarField)};

constexpr std::array kMathInputs{anyField("a"), anyField("b"), anyField("t")};
constexpr std::array kAnyOutputs{port("scalar", Socket::ScalarField),
                                 port("vector", Socket::Vector2Field),
                                 port("color", Socket::ColorField)};

constexpr std::array kOneFieldInput{anyField("field")};
constexpr std::array kRemapOutputs{port("scalar", Socket::ScalarField),
                                   port("vector", Socket::Vector2Field)};

constexpr std::array kTransformInputs{anyField("field"), port("warp", Socket::Vector2Field)};

constexpr std::array kDerivativeOutputs{port("vector", Socket::Vector2Field),
                                        port("scalar", Socket::ScalarField)};

constexpr std::array kCombineInputs{port("x", Socket::ScalarField), port("y", Socket::ScalarField),
                                    port("z", Socket::ScalarField), port("w", Socket::ScalarField)};
constexpr std::array kCombineOutputs{port("vector", Socket::Vector2Field),
                                     port("color", Socket::ColorField)};

constexpr std::array kSeparateOutputs{
    port("x", Socket::ScalarField), port("y", Socket::ScalarField),
    port("z", Socket::ScalarField), port("w", Socket::ScalarField)};

constexpr std::array kSampleOutputs{port("scalar", Socket::Scalar), port("vector", Socket::Vector2),
                                    port("color", Socket::Color)};

constexpr std::array kRasterInputs{anyField("field"),
                                   FieldPortSpec{"extentImage", Socket::Image, false, true}};
constexpr std::array kRasterOutputs{port("image", Socket::Image), port("mask", Socket::Mask)};

constexpr std::array kFieldNodes{
    FieldNodeSpec{kFieldNoiseNodeType, NodeLoweringKind::Field, kNoPorts, kNoiseOutputs,
                  kNoiseParameters},
    FieldNodeSpec{kFieldVoronoiNodeType, NodeLoweringKind::Field, kNoPorts, kVoronoiOutputs,
                  kVoronoiParameters},
    FieldNodeSpec{kFieldRandomNodeType, NodeLoweringKind::Field, kNoPorts, kRandomOutputs,
                  kRandomParameters},
    FieldNodeSpec{kFieldPatternNodeType, NodeLoweringKind::Field, kNoPorts, kPatternOutputs,
                  kPatternParameters},
    FieldNodeSpec{kFieldGradientNodeType, NodeLoweringKind::Field, kNoPorts, kGradientOutputs,
                  kGradientParameters},
    FieldNodeSpec{kFieldShapeDistanceNodeType, NodeLoweringKind::Field, kShapeInputs,
                  kShapeOutputs, kShapeParameters},
    FieldNodeSpec{kFieldFromImageNodeType, NodeLoweringKind::Field, kImageInputs, kImageOutputs,
                  kImageParameters},
    FieldNodeSpec{kFieldForceNodeType, NodeLoweringKind::Field, kNoPorts, kForceOutputs,
                  kForceParameters},
    FieldNodeSpec{kFieldMathNodeType, NodeLoweringKind::Field, kMathInputs, kAnyOutputs,
                  kMathParameters},
    FieldNodeSpec{kFieldRemapNodeType, NodeLoweringKind::Field, kOneFieldInput, kRemapOutputs,
                  kRemapParameters},
    FieldNodeSpec{kFieldTransformNodeType, NodeLoweringKind::Field, kTransformInputs,
                  kAnyOutputs, kTransformParameters},
    FieldNodeSpec{kFieldDerivativeNodeType, NodeLoweringKind::Field, kOneFieldInput,
                  kDerivativeOutputs, kDerivativeParameters},
    FieldNodeSpec{kFieldCombineNodeType, NodeLoweringKind::Field, kCombineInputs,
                  kCombineOutputs, kCombineParameters},
    FieldNodeSpec{kFieldSeparateNodeType, NodeLoweringKind::Field, kOneFieldInput,
                  kSeparateOutputs, kNoParameters},
    FieldNodeSpec{kFieldSampleNodeType, NodeLoweringKind::ValueFieldSample, kOneFieldInput,
                  kSampleOutputs, kSampleParameters},
    FieldNodeSpec{kFieldRasterizeNodeType, NodeLoweringKind::Field, kRasterInputs,
                  kRasterOutputs, kRasterParameters},
};

ParameterValueKind valueKindOf(const FieldParameterKind kind) noexcept {
    switch (kind) {
    case Kind::Choice:
    case Kind::Integer:
    case Kind::Seed:
        return ParameterValueKind::Integer;
    case Kind::Boolean:
        return ParameterValueKind::Boolean;
    case Kind::Scalar:
        return ParameterValueKind::Float64;
    case Kind::Vector2:
        return ParameterValueKind::Vec2d;
    case Kind::Curve:
        return ParameterValueKind::Path;
    case Kind::Gradient:
        return ParameterValueKind::Gradient;
    }
    return ParameterValueKind::Float64;
}

ParameterValue defaultValueOf(const FieldParameterSpec& spec) {
    switch (spec.kind) {
    case Kind::Choice:
    case Kind::Integer:
    case Kind::Seed:
        return static_cast<std::int64_t>(spec.defaultX);
    case Kind::Boolean:
        return spec.defaultX != 0.0;
    case Kind::Scalar:
        return spec.defaultX;
    case Kind::Vector2:
        return Vec2d{spec.defaultX, spec.defaultY};
    case Kind::Curve:
        // A falloff curve starts at full strength at the centre and fades out at the radius.
        return spec.defaultX != 0.0 ? gradeCurvePath({Vec2d{0.0, 1.0}, Vec2d{1.0, 0.0}})
                                    : defaultFieldCurve();
    case Kind::Gradient:
        return defaultFieldGradient();
    }
    return 0.0;
}

} // namespace

std::span<const FieldNodeSpec> fieldNodeSpecs() noexcept { return kFieldNodes; }

const FieldNodeSpec* fieldNodeSpec(const std::string_view typeId) noexcept {
    const auto found = std::ranges::find(kFieldNodes, typeId, &FieldNodeSpec::typeId);
    return found == kFieldNodes.end() ? nullptr : &*found;
}

bool isFieldGeneratorNodeType(const std::string_view typeId) noexcept {
    const auto* spec = fieldNodeSpec(typeId);
    return spec != nullptr && std::ranges::any_of(spec->outputs, [](const auto& output) {
               return output.name == kFieldImageOutputPort;
           }) && typeId != kFieldRasterizeNodeType;
}

std::string fieldSchemaKey(const std::string_view typeId, const std::string_view role) {
    std::string key(typeId);
    key += '.';
    key += role;
    return key;
}

bool isFieldParameterValueValid(const std::string_view schemaKey,
                                const ParameterValue& value) noexcept {
    const auto* spec = fieldParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    switch (spec->kind) {
    case Kind::Choice:
    case Kind::Integer: {
        const auto* v = std::get_if<std::int64_t>(&value);
        return v != nullptr && within(static_cast<double>(*v));
    }
    case Kind::Seed:
        return std::holds_alternative<std::int64_t>(value);
    case Kind::Boolean:
        return std::holds_alternative<bool>(value);
    case Kind::Scalar: {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    case Kind::Vector2: {
        const auto* v = std::get_if<Vec2d>(&value);
        return v != nullptr && within(v->x) && within(v->y);
    }
    case Kind::Curve: {
        const auto* v = std::get_if<PathValue>(&value);
        return v != nullptr && v->anchors.size() <= kMaximumFieldCurvePoints &&
               isGradeCurveValueValid(GradeCurve::Master, *v);
    }
    case Kind::Gradient: {
        const auto* v = std::get_if<GradientValue>(&value);
        return v != nullptr && v->isValid();
    }
    }
    return false;
}

PathValue defaultFieldCurve() { return gradeCurvePath({Vec2d{0.0, 0.0}, Vec2d{1.0, 1.0}}); }

GradientValue defaultFieldGradient() {
    GradientValue gradient;
    gradient.stops = {GradientStop{0.0, core::Color4d{0.0, 0.0, 0.0, 1.0}},
                      GradientStop{1.0, core::Color4d{1.0, 1.0, 1.0, 1.0}}};
    return gradient;
}

NodeDefinition fieldNodeDefinition(const FieldNodeSpec& spec) {
    NodeDefinition definition{{std::string(spec.typeId), 1},
                              spec.lowering,
                              {},
                              {},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Fields};
    for (const auto& input : spec.inputs) {
        InputPortDefinition port{std::string(input.name), input.kind, false, input.pixels};
        port.acceptsFieldKinds = input.anyField;
        definition.inputs.push_back(std::move(port));
    }
    for (const auto& output : spec.outputs)
        definition.outputs.push_back({std::string(output.name), output.kind});
    for (const auto& parameter : spec.parameters) {
        const bool animated = parameter.animated &&
                              (parameter.kind == Kind::Scalar || parameter.kind == Kind::Vector2 ||
                               parameter.kind == Kind::Curve || parameter.kind == Kind::Gradient);
        definition.parameters.push_back({std::string(parameter.role),
                                         fieldSchemaKey(spec.typeId, parameter.role),
                                         valueKindOf(parameter.kind), true, animated,
                                         defaultValueOf(parameter)});
        if (animated && parameter.kind == Kind::Scalar)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Scalar, false});
        else if (animated && parameter.kind == Kind::Vector2)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Vector2, false});
    }
    return definition;
}

} // namespace bloom::document
