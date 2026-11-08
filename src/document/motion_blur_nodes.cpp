#include <bloom/document/motion_blur_nodes.hpp>

#include <bloom/document/grade_curves.hpp>

#include <algorithm>
#include <cmath>
#include <variant>

namespace bloom::document {

namespace {

using Kind = DepthParameterKind;

ParameterValueKind valueKindOf(const Kind kind) noexcept {
    switch (kind) {
    case Kind::Choice:
    case Kind::Integer:
        return ParameterValueKind::Integer;
    case Kind::Boolean:
        return ParameterValueKind::Boolean;
    case Kind::Scalar:
        return ParameterValueKind::Float64;
    case Kind::Vector2:
        return ParameterValueKind::Vec2d;
    case Kind::Color:
        return ParameterValueKind::Color4d;
    case Kind::Curve:
        return ParameterValueKind::Path;
    case Kind::Text:
        return ParameterValueKind::String;
    }
    return ParameterValueKind::Float64;
}

ParameterValue defaultValueOf(const DepthParameterSpec& spec) {
    switch (spec.kind) {
    case Kind::Choice:
    case Kind::Integer:
        return static_cast<std::int64_t>(spec.value[0]);
    case Kind::Boolean:
        return spec.value[0] != 0.0;
    case Kind::Scalar:
        return spec.value[0];
    case Kind::Vector2:
        return Vec2d{spec.value[0], spec.value[1]};
    case Kind::Color:
        return core::Color4d{spec.value[0], spec.value[1], spec.value[2], spec.value[3]};
    case Kind::Curve:
        return defaultShutterCurve();
    case Kind::Text:
        return std::string{};
    }
    return 0.0;
}

ParameterDefinition parameterOf(const std::string_view typeId, const DepthParameterSpec& spec,
                                const bool required) {
    return {std::string(spec.role), motionSchemaKey(typeId, spec.role), valueKindOf(spec.kind),
            required, spec.animated, defaultValueOf(spec)};
}

} // namespace

std::string motionSchemaKey(const std::string_view typeId, const std::string_view role) {
    std::string key(typeId);
    key += '.';
    key += role;
    return key;
}

PathValue defaultShutterCurve() { return gradeCurvePath({Vec2d{0.0, 1.0}, Vec2d{1.0, 1.0}}); }

bool isMotionParameterValueValid(const std::string_view schemaKey,
                                 const ParameterValue& value) noexcept {
    const auto* spec = motionParameterSpec(schemaKey);
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
    case Kind::Color:
        return false;
    case Kind::Curve: {
        const auto* v = std::get_if<PathValue>(&value);
        return v != nullptr && v->anchorCount() <= kMaximumDepthCurvePoints &&
               isGradeCurveValueValid(GradeCurve::Master, *v);
    }
    case Kind::Text: {
        const auto* v = std::get_if<std::string>(&value);
        return v != nullptr && v->size() <= kMaximumDepthChannelBytes &&
               std::ranges::none_of(*v, [](const char c) {
                   return static_cast<unsigned char>(c) < 0x20U;
               });
    }
    }
    return false;
}

NodeDefinition motionBlurDefinition(const std::string_view typeId) {
    const bool flow = typeId == kOpticalFlowNodeType;
    NodeDefinition definition{{std::string(typeId), kMotionBlurNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{std::string(kMotionInputPort), SocketValueKind::Image}},
                              {},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Filters};
    if (flow) {
        definition.outputs = {{std::string(kFlowForwardPort), SocketValueKind::Image},
                              {std::string(kFlowBackwardPort), SocketValueKind::Image},
                              {std::string(kFlowConfidencePort), SocketValueKind::Mask},
                              {std::string(kFlowOcclusionPort), SocketValueKind::Mask},
                              {std::string(kFlowCutPort), SocketValueKind::Mask}};
    } else {
        definition.outputs = {{std::string(kMotionOutputPort), SocketValueKind::Image}};
        // The flow it solved, and its diagnostics, as Optical Flow's.
        if (typeId == kOpticalFlowMotionBlurNodeType) {
            definition.outputs.push_back({std::string(kFlowForwardPort), SocketValueKind::Image});
            definition.outputs.push_back({std::string(kFlowBackwardPort), SocketValueKind::Image});
            definition.outputs.push_back({std::string(kFlowConfidencePort), SocketValueKind::Mask});
            definition.outputs.push_back({std::string(kFlowOcclusionPort), SocketValueKind::Mask});
        }
    }
    if (typeId == kVectorMotionBlurNodeType) {
        // The vectors and depth: an Image, a Mask or a channel set (channels picked by name).
        InputPortDefinition vectors{std::string(kMotionVectorsPort), SocketValueKind::Image, false};
        vectors.acceptsPixelKinds = true;
        definition.inputs.push_back(std::move(vectors));
        InputPortDefinition depth{std::string(kMotionDepthPort), SocketValueKind::Image, false};
        depth.acceptsPixelKinds = true;
        definition.inputs.push_back(std::move(depth));
    }
    if (typeId == kVectorMotionBlurNodeType || typeId == kOpticalFlowMotionBlurNodeType ||
        flow) {
        InputPortDefinition matte{std::string(kMotionMattePort), SocketValueKind::Mask, false};
        matte.acceptsImageMatte = true;
        definition.inputs.push_back(std::move(matte));
    }
    if (typeId == kOpticalFlowMotionBlurNodeType || flow)
        definition.inputs.push_back(
            {std::string(kMotionGuidePort), SocketValueKind::Image, false});
    for (const auto& parameter : motionBlurParameters(typeId)) {
        definition.parameters.push_back(parameterOf(typeId, parameter, true));
        if (!parameter.animated)
            continue;
        if (parameter.kind == Kind::Scalar)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Scalar, false});
        else if (parameter.kind == Kind::Vector2)
            definition.inputs.push_back(
                {std::string(parameter.role), SocketValueKind::Vector2, false});
    }
    return definition;
}

NodeDefinition cameraShakeDefinition() {
    auto definition = motionBlurDefinition(kCameraShakeNodeType);
    definition.category = NodeCategory::Transform;
    // The shake itself, for any other parameter: the translation applied in pixels (Transform 2D's
    // translate) and the roll applied in degrees clockwise (its rotation).
    definition.outputs.push_back({std::string(kCameraShakeOffsetPort), SocketValueKind::Vector2});
    definition.outputs.push_back({std::string(kCameraShakeRollPort), SocketValueKind::Scalar});
    return definition;
}

std::vector<ParameterDefinition> transformMotionBlurParameterDefinitions() {
    std::vector<ParameterDefinition> parameters;
    for (const auto& parameter : motionBlurParameters(kTransform2DNodeType))
        parameters.push_back(parameterOf(kTransform2DNodeType, parameter, false));
    return parameters;
}

VectorConvention vectorConvention(const std::int64_t convention) noexcept {
    switch (convention) {
    case kVectorConventionNuke:
        // Forward and backward vectors in pixels, y up, as a scanline compositor's (forward.u/v, backward.u/v).
        return {1.0, 0.0, true, kVectorLayoutForwardBackward, kVectorUnitsPixels, false,
                "forward"};
    case kVectorConventionRsmb:
        // RE:Vision's format: one vector, y up, a float value times MaxDisplace (the node's own
        // parameter, 2048 as RE:Vision recommends), valid where the alpha is full on.
        return {2048.0, 0.0, true, kVectorLayoutSingle, kVectorUnitsPixels, true, "motion"};
    case kVectorConventionBlender:
        // A Vector pass in one image: the motion from the previous frame in RG and to the next in BA,
        // in pixels, y up.
        return {1.0, 0.0, true, kVectorLayoutBlender, kVectorUnitsPixels, false, "Vector"};
    case kVectorConventionArnold:
        // Arnold's motionvector AOV: raw pixels, y up, the forward vector.
        return {1.0, 0.0, true, kVectorLayoutSingle, kVectorUnitsPixels, false, "motionvector"};
    case kVectorConventionVray:
        // V-Ray's VelocityPass: pixels over the frame, y up.
        return {1.0, 0.0, true, kVectorLayoutSingle, kVectorUnitsPixels, false, "VRayVelocity"};
    case kVectorConventionRedshift:
        // Redshift's MotionVectors AOV (raw, not normalised): pixels, y up.
        return {1.0, 0.0, true, kVectorLayoutSingle, kVectorUnitsPixels, false, "MotionVectors"};
    case kVectorConventionKarma:
        // Karma's motionvector AOV: NDC over the frame, y up.
        return {1.0, 0.0, true, kVectorLayoutSingle, kVectorUnitsNdc, false, "motionvector"};
    default:
        // Bloom's own: the forward vector in pixels, y down (optical flow's output).
        return {1.0, 0.0, false, kVectorLayoutSingle, kVectorUnitsPixels, false, ""};
    }
}

} // namespace bloom::document
