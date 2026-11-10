#include <bloom/commands/path_glow_presets.hpp>

#include <bloom/commands/group_definition_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_operations.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace bloom::commands {
namespace {

using document::NodeId;
using document::PathAnchor;
using document::PathValue;
using document::Vec2d;

// What a preset makes. The path is in the group's own pixels about its centre. Without
// `whiteCore` the stroke's fill reads the instance's colour and the glow takes the stroke's colour;
// with it the fill is white and the glow is tinted by the colour (a white-hot core in a coloured
// glow).
struct PresetSpec final {
    std::string_view name;
    PathValue path;
    double width = 4.0;
    double glowSize = 24.0;
    double intensity = 1.5;
    core::Color4d color{1.0, 1.0, 1.0, 1.0};
    bool whiteCore = false;
    double taperEnd = 0.0;
    double dashLength = 0.0;
    double dashGap = 8.0;
    // A Path Stroke value that runs with time: `rate` per second from 0 (a Value Time times the
    // rate). A group slot is not keyable, so the motion is made inside the group, where opening it
    // retimes it.
    std::string_view movingRole;
    double rate = 0.0;
};

[[nodiscard]] PathAnchor corner(const double x, const double y) {
    return PathAnchor{Vec2d{x, y}, std::nullopt, std::nullopt};
}

[[nodiscard]] PathAnchor smooth(const double x, const double y, const double dx, const double dy) {
    PathAnchor anchor{Vec2d{x, y}, Vec2d{x - dx, y - dy}, Vec2d{x + dx, y + dy}};
    anchor.mode = document::inferPathAnchorMode(anchor);
    return anchor;
}

// A gentle S across the frame, the curve Write-on and Stroke draw.
[[nodiscard]] PathValue sCurve() {
    return PathValue::fromContour(
        {corner(-720.0, 240.0), smooth(0.0, 0.0, 320.0, -240.0), corner(720.0, -240.0)}, false);
}

// Each preset is laid out for a 1920 x 1080 frame about its centre; `presetSpec` scales its
// geometry, widths, glow size, dashes and dash speed by the host's height over 1080, so it frames
// the same in any composition.
[[nodiscard]] PresetSpec designedSpec(const PathGlowPreset preset) {
    PresetSpec spec;
    spec.name = pathGlowPresetName(preset);
    switch (preset) {
    case PathGlowPreset::Saber:
        // A blade: a white core tapering to its tip in a wide blue glow.
        spec.path = PathValue::fromContour({corner(0.0, -380.0), corner(0.0, 380.0)}, false);
        spec.width = 16.0;
        spec.glowSize = 64.0;
        spec.intensity = 2.0;
        spec.color = core::Color4d{0.25, 0.55, 1.0, 1.0};
        spec.whiteCore = true;
        spec.taperEnd = 15.0;
        break;
    case PathGlowPreset::Beam:
        // A thin straight beam of coloured light.
        spec.path = PathValue::fromContour({corner(-800.0, 0.0), corner(800.0, 0.0)}, false);
        spec.width = 4.0;
        spec.glowSize = 32.0;
        spec.intensity = 1.5;
        spec.color = core::Color4d{1.0, 0.3, 0.15, 1.0};
        break;
    case PathGlowPreset::Vegas:
        // Dashes chasing round a closed outline.
        spec.path = PathValue::fromContour({corner(-600.0, -340.0), corner(600.0, -340.0),
                                            corner(600.0, 340.0), corner(-600.0, 340.0)},
                                           true);
        spec.width = 6.0;
        spec.glowSize = 32.0;
        spec.intensity = 1.5;
        spec.color = core::Color4d{1.0, 0.75, 0.3, 1.0};
        spec.dashLength = 40.0;
        spec.dashGap = 28.0;
        spec.movingRole = "dashOffset";
        spec.rate = 240.0; // pixels a second
        break;
    case PathGlowPreset::WriteOn:
        // The stroke drawing itself on along its path.
        spec.path = sCurve();
        spec.width = 8.0;
        spec.glowSize = 40.0;
        spec.intensity = 1.5;
        spec.color = core::Color4d{0.4, 1.0, 0.6, 1.0};
        spec.movingRole = "end";
        spec.rate = 50.0; // percent a second: written on in two seconds
        break;
    case PathGlowPreset::Stroke:
        // A plain stroke with a soft glow.
        spec.path = sCurve();
        spec.width = 6.0;
        spec.glowSize = 24.0;
        spec.intensity = 1.0;
        spec.color = core::Color4d{0.9, 0.95, 1.0, 1.0};
        break;
    }
    return spec;
}

[[nodiscard]] PresetSpec presetSpec(const PathGlowPreset preset, const double height) {
    auto spec = designedSpec(preset);
    const double scale = height / 1080.0;
    const auto scaled = [scale](const Vec2d point) {
        return Vec2d{point.x * scale, point.y * scale};
    };
    for (auto& contour : spec.path.contours)
        for (auto& anchor : contour.anchors) {
            anchor.point = scaled(anchor.point);
            if (anchor.inHandle)
                anchor.inHandle = scaled(*anchor.inHandle);
            if (anchor.outHandle)
                anchor.outHandle = scaled(*anchor.outHandle);
        }
    spec.width *= scale;
    spec.glowSize = std::min(spec.glowSize * scale, document::kMaximumRecipeBlurRadius);
    spec.dashLength *= scale;
    spec.dashGap *= scale;
    if (spec.movingRole == "dashOffset")
        spec.rate *= scale;
    return spec;
}

// An applied sub-operation's output id, or the reason it was not applied.
template <class Id>
[[nodiscard]] std::optional<Id> outputOf(const OperationResult& result,
                                         const std::string_view name) {
    if (result.status != OperationStatus::Applied)
        return std::nullopt;
    for (const auto& output : result.outputs)
        if (output.name == name)
            if (const auto* id = std::get_if<Id>(&output.id))
                return *id;
    return std::nullopt;
}

[[nodiscard]] bool applied(const OperationResult& result) {
    return result.status == OperationStatus::Applied || result.status == OperationStatus::NoChange;
}

} // namespace

std::string_view pathGlowPresetName(const PathGlowPreset preset) noexcept {
    switch (preset) {
    case PathGlowPreset::Saber:
        return "Saber";
    case PathGlowPreset::Beam:
        return "Beam";
    case PathGlowPreset::Vegas:
        return "Vegas";
    case PathGlowPreset::WriteOn:
        return "Write-on";
    case PathGlowPreset::Stroke:
        return "Stroke";
    }
    return "Stroke";
}

std::string_view CreatePathGlowPreset::typeId() const noexcept {
    return "bloom.path-glow-preset.create";
}

OperationResult CreatePathGlowPreset::apply(document::Draft& draft) const {
    using namespace document;
    const auto* host = draft.project().findComposition(host_);
    if (host == nullptr)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "The composition to add the preset to does not exist");
    const auto spec = presetSpec(preset_, static_cast<double>(host->format().height()));
    // Every step below is an ordinary operation on the same draft, so its own checks hold and a
    // refusal anywhere leaves the transaction unapplied.
    const auto made = CreateGroupDefinition(host_, std::string(spec.name)).apply(draft);
    const auto group = outputOf<CompositionId>(made, kCreateGroupDefinitionOutput);
    const auto groupInput = outputOf<NodeId>(made, kCreateGroupDefinitionInputNodeOutput);
    const auto groupOutput = outputOf<NodeId>(made, kCreateGroupDefinitionOutputNodeOutput);
    if (!group || !groupInput || !groupOutput)
        return made.status == OperationStatus::Rejected
                   ? made
                   : OperationResult::rejected(OperationIssueCode::InvalidValue,
                                               "The preset's group could not be made");

    std::optional<OperationResult> refusal;
    const auto check = [&](OperationResult result) {
        if (!refusal && !applied(result))
            refusal = std::move(result);
        return refusal.has_value();
    };
    const auto add = [&](const CompositionId composition, const std::string_view type,
                         const Vec2d position) -> NodeId {
        auto result = AddNode(composition, std::string(type), position).apply(draft);
        const auto id = outputOf<NodeId>(result, kAddNodeOutput);
        check(std::move(result));
        return id.value_or(NodeId{});
    };
    const auto set = [&](const CompositionId composition, const NodeId node,
                         const std::string_view role, ParameterValue value) {
        if (!refusal)
            check(SetNodeParameterValue(composition, node, std::string(role), std::move(value))
                      .apply(draft));
    };
    const auto connect = [&](const NodeId from, const std::string_view output, const NodeId to,
                             const std::string_view input) {
        if (!refusal)
            check(ConnectPorts(*group, OutputPortRef{from, std::string(output)},
                               NodeInputRef{to, std::string(input)})
                      .apply(draft));
    };

    // Inside the group: Group Input -> Path Stroke -> Shape Source -> Glow -> Group Output.
    const auto stroke = add(*group, kPathStrokeNodeType, Vec2d{200.0, -80.0});
    const auto shape = add(*group, kShapeSourceNodeType, Vec2d{420.0, 0.0});
    const auto glow = add(*group, kGlowNodeType, Vec2d{640.0, 0.0});
    if (refusal)
        return *refusal;
    if (auto* definition = draft.project().findComposition(*group))
        definition->nodeLayout()[*groupOutput].position = Vec2d{860.0, 0.0};
    set(*group, stroke, "path", spec.path);
    set(*group, stroke, "taperEnd", spec.taperEnd);
    set(*group, stroke, "dashLength", spec.dashLength);
    set(*group, stroke, "dashGap", spec.dashGap);
    set(*group, shape, "kind", static_cast<std::int64_t>(ShapeKind::Path));
    set(*group, shape, "fillColor", core::Color4d{1.0, 1.0, 1.0, 1.0});
    // Every pixel of the stroke glows: no luma threshold.
    set(*group, glow, "threshold", 0.0);
    set(*group, glow, "knee", 0.0);
    connect(stroke, "path", shape, "path");
    connect(shape, "image", glow, "input");
    connect(glow, "image", *groupOutput, "image1");
    // The instance's values: Width, the glow's Size and Intensity, and the colour.
    connect(*groupInput, "scalar1", stroke, "width");
    connect(*groupInput, "scalar2", glow, "size");
    connect(*groupInput, "scalar3", glow, "intensity");
    connect(*groupInput, "color1", spec.whiteCore ? glow : shape,
            spec.whiteCore ? "tint" : "fillColor");
    if (!spec.movingRole.empty()) {
        const auto time = add(*group, kTimeValueNodeType, Vec2d{-20.0, 200.0});
        const auto rate = add(*group, kScalarMathNodeType, Vec2d{180.0, 200.0});
        set(*group, rate, kOperationParameterRole,
            scalarOperationStoredValue(core::primitives::ScalarPrimitive::Multiply));
        set(*group, rate, "b", spec.rate);
        connect(time, kTimeSecondsPortName, rate, "a");
        connect(rate, kResultPortName, stroke, spec.movingRole);
    }
    if (refusal)
        return *refusal;

    // The instance in the host composition, pointed at the group and holding the preset's values.
    const auto instance = add(host_, kNodeGroupNodeType, position_);
    set(host_, instance, kNodeGroupReferenceRole, static_cast<std::int64_t>(group->value()));
    set(host_, instance, "scalar1", spec.width);
    set(host_, instance, "scalar2", spec.glowSize);
    set(host_, instance, "scalar3", spec.intensity);
    set(host_, instance, "color1", spec.color);
    if (refusal)
        return *refusal;
    return OperationResult::applied({{std::string(kPathGlowPresetNodeOutput), instance},
                                     {std::string(kPathGlowPresetGroupOutput), *group}});
}

} // namespace bloom::commands
