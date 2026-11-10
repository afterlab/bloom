#include <bloom/commands/node_layout_placement.hpp>

#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/four_color_gradient_nodes.hpp>
#include <bloom/document/field_parameters.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/document/lens_flare_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/lens_distortion_nodes.hpp>
#include <bloom/document/chromatic_aberration_nodes.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/document/light_rays_nodes.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/sharpen_nodes.hpp>
#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/resample_path_nodes.hpp>
#include <bloom/document/shape_morph_nodes.hpp>
#include <bloom/document/warp_path_nodes.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace bloom::commands {
namespace {
using document::NodeCardSize;
using document::NodeCardSizes;
using document::NodeId;
using document::Vec2d;

// The editor's card metrics in document units, frozen here the way kDefaultNodeGroupPadding is:
// Size::NodeTitleBand, Size::PropertyRow, Spacing::S and Size::NodeCardWidth, plus the Merge
// pill's per-slot pitch (Spacing::M). The estimate only has to be an upper bound of what the
// editor draws; the node canvas tests hold every built-in card to it.
constexpr double kTitleBand = 32.0;
constexpr double kRow = 28.0;
constexpr double kPadding = 8.0;
// Size::NodeCardWidth (240) plus the label column an ordinary built-in card can add to it, on the
// grid's half step. A card with a text field, a video's stream readout or a vector operation's
// operator selector runs wider still.
constexpr double kCardWidth = 264.0;
constexpr double kWideCardWidth = 288.0;
// #498: the Curves card widget's graph height over its width (curve_graph.cpp), and the card's
// width: its six curve tabs beside the curve's keyframe diamond.
constexpr double kCurveGraphAspect = 0.75;
constexpr double kCurvesCardWidth = 320.0;
// #494, #501: the Grade and Levels card widgets' metrics (grade_wheels.cpp, levels_graph.cpp):
// Size::Control, Spacing::XS and Spacing::S, Size::IconSmall, the share of a wheel's cell its disc
// takes and the Levels plot's aspect; and their widths: the Grade card's two wheels side by side,
// each with its three colour values, and the Levels card's three values beside their caption.
constexpr double kControl = 26.0;
constexpr double kSmall = 4.0;
constexpr double kLevelsHandle = 12.0;
constexpr double kLevelsPlotAspect = 0.45;
constexpr double kGradeDiscShare = 0.8;
constexpr double kGradeCardWidth = 432.0;
constexpr double kLevelsCardWidth = 360.0;
// A card whose parameters draw as dropdowns (enum choices, OCIO and input colour spaces) carries
// the Properties panel's field control, which is wider still.
constexpr double kDropdownCardWidth = 384.0;
constexpr double kSlotPitch = 12.0;
// #541: the node-controls card widget (node_controls_widget.cpp). An angle's row grows to its dial
// (Size::Control + Size::ToggleCell), a pair of bounds adds its range track (Size::Control) under
// its two rows, and a point pad is at most Size::DropdownWidthExpanded less Size::ControlRoomy tall;
// each section is followed by Spacing::XS and the widget by the card's padding. A pad card is as
// wide as a label beside two position cells.
constexpr double kDialSide = 50.0;
constexpr double kPadMaxHeight = 208.0;
// #486: FilmGrainResponseGraph's height (film_grain_widget.cpp).
constexpr double kFilmGrainGraphHeight = 120.0;
// #527: DitherPreview's height (dither_widget.cpp) and its readout, at most two lines of text.
constexpr double kDitherPreviewHeight = 48.0;
constexpr double kDitherReadout = 40.0;
// #486: SharpenProfileGraph's height (sharpen_widget.cpp).
constexpr double kSharpenGraphHeight = 120.0;
// A gradient row (gradient_parameter_row.cpp) is the gradient editor (Size::Control bar,
// Size::IconLarge handle band, Spacing::XXS), then its Stop row, (#546) its Color mode and
// Interpolation rows and its Values button, each Spacing::XS and Size::Control.
constexpr double kGradientRow = 168.0;
constexpr double kPadCardWidth = 336.0;
// #525: the Noise card's long fractal-noise row labels ("Offset (Offset Turbulence)", "Sub Scaling
// (lacunarity)") beside the dropdown field control.
constexpr double kFieldNoiseCardWidth = 408.0;
// #564: the motion cards (Frame Rate Hold has no dropdown) are never narrower than this.
constexpr double kMotionCardWidth = 344.0;

struct NodeControlsEstimate final {
    int angles = 0;
    int ranges = 0;
    bool pad = false;
    int gradients = 0;
    // #525: a curve graph (curve_graph.cpp) with its caption above it.
    int curves = 0;
    // #637, #598, #597: a value row with its slider under its name and number (GradingValueRow:
    // two Size::Control lines) in place of the role's plain row.
    int sliders = 0;
};

[[nodiscard]] NodeControlsEstimate nodeControlsEstimate(const std::string_view typeId) {
    // #525: the Fields cards' controls (node_controls_widget.cpp).
    if (typeId == document::kFieldNoiseNodeType)
        return {2, 0, true};
    if (typeId == document::kFieldPatternNodeType ||
        typeId == document::kFieldShapeDistanceNodeType ||
        typeId == document::kFieldTransformNodeType)
        return {1, 0, true};
    if (typeId == document::kFieldGradientNodeType || typeId == document::kFieldSampleNodeType)
        return {0, 0, true};
    if (typeId == document::kFieldForceNodeType)
        return {1, 0, true, 0, 1};
    if (typeId == document::kFieldMathNodeType)
        return {1, 0, false};
    if (typeId == document::kFieldRemapNodeType)
        return {0, 2, false, 0, 1};
    if (typeId == document::kFieldRasterizeNodeType)
        return {0, 1, true, 1};
    if (typeId == "bloom.transform-2d")
        return {1, 0, true};
    // #598: the path matrix's position and anchor pad, its rotation and skew-axis dials, and the
    // skew's slider; the warps add their amount's slider. #637 Resample Path's percentage and start
    // offset and #597 Shape Morph's amount are sliders.
    if (typeId == document::kTransformPathNodeType)
        return {.angles = 2, .pad = true, .sliders = 1};
    if (typeId == document::kWarpPathNodeType || typeId == document::kWarpPathFunctionNodeType)
        return {.angles = 2, .pad = true, .sliders = 2};
    if (typeId == document::kResamplePathNodeType)
        return {.sliders = 2};
    if (typeId == document::kShapeMorphNodeType)
        return {.sliders = 1};
    // #511: the pad for the mode's point and the angle dial of Mirror or Kaleidoscope (one at a time).
    if (typeId == document::kTileMirrorNodeType)
        return {1, 0, true};
    if (typeId == "bloom.linear-gradient" || typeId == "bloom.radial-gradient")
        return {0, 0, true, 1};
    if (typeId == "bloom.four-color-gradient")
        return {0, 0, true};
    if (typeId == "bloom.glow")
        return {1, 0, false, 1};
    if (typeId == "bloom.drop-shadow" || typeId == "bloom.inner-shadow" || typeId == "bloom.layer-output" ||
        typeId == "bloom.hue-shift" || typeId == "bloom.rotate-2d" ||
        typeId == "bloom.polar-to-cartesian" || typeId == "bloom.degrees-to-radians" ||
        typeId == "bloom.radians-to-degrees")
        return {1, 0, false};
    if (typeId == "bloom.map-range")
        return {0, 2, false};
    if (typeId == document::kToneMapNodeType || typeId == document::kQuantizeNodeType)
        return {0, 1, false};
    if (typeId == "bloom.luma-key" || typeId == "bloom.clamp" || typeId == "bloom.in-range" ||
        typeId == "bloom.random" || typeId == "bloom.wrap" || typeId == "bloom.smoothstep")
        return {0, 1, false};
    return {};
}

struct Rect final {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
    [[nodiscard]] double right() const noexcept { return left + width; }
    [[nodiscard]] double bottom() const noexcept { return top + height; }
};

[[nodiscard]] bool touches(const Rect& candidate, const Rect& existing, const double gap) noexcept {
    return candidate.left < existing.right() + gap && candidate.right() + gap > existing.left &&
           candidate.top < existing.bottom() + gap && candidate.bottom() + gap > existing.top;
}

[[nodiscard]] double snapDown(const double value) noexcept {
    return std::floor(value / kNodeLayoutGrid) * kNodeLayoutGrid;
}
[[nodiscard]] double snapUp(const double value) noexcept {
    return std::ceil(value / kNodeLayoutGrid) * kNodeLayoutGrid;
}

[[nodiscard]] bool sane(const NodeCardSize size) noexcept {
    return std::isfinite(size.width) && size.width > 0.0 && std::isfinite(size.height) &&
           size.height > 0.0;
}

[[nodiscard]] std::vector<Rect> obstacles(const document::Composition& composition,
                                          const NodeCardSizes& sizes,
                                          const std::set<NodeId>& ignored) {
    std::vector<Rect> result;
    result.reserve(composition.graph().nodes().size());
    for (const auto& node : composition.graph().nodes()) {
        if (ignored.contains(node.id))
            continue;
        const auto position = effectiveNodePosition(composition, node.id);
        const auto found = sizes.find(node.id);
        const auto size = found != sizes.end() && sane(found->second)
                              ? found->second
                              : estimatedNodeCardSize(composition, node.id);
        result.push_back({position.x, position.y, size.width, size.height});
    }
    return result;
}

[[nodiscard]] bool clear(const std::vector<Rect>& rects, const Rect& candidate, const double gap) {
    return std::ranges::none_of(rects,
                                [&](const Rect& rect) { return touches(candidate, rect, gap); });
}

// The upstream closure of `node` (its inputs, their inputs, ...), including `node` itself: the
// cards that make up one layer's row.
[[nodiscard]] std::set<NodeId> upstreamOf(const document::Composition& composition,
                                          const NodeId node) {
    std::set<NodeId> visited{node};
    std::vector<NodeId> pending{node};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        for (const auto& edge : composition.graph().edges()) {
            const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            if (input != nullptr && input->nodeId == id && visited.insert(edge.source.nodeId).second)
                pending.push_back(edge.source.nodeId);
        }
    }
    return visited;
}
} // namespace

NodeCardSize estimatedNodeCardSize(const document::Composition& composition, const NodeId id) {
    const auto* node = composition.graph().findNode(id);
    const auto layout = composition.nodeLayout().find(id);
    const double width =
        std::max(kCardWidth, layout != composition.nodeLayout().end() &&
                                     std::isfinite(layout->second.width)
                                 ? layout->second.width
                                 : 0.0);
    if (node == nullptr)
        return document::kConservativeNodeCardSize;
    if (document::isRerouteNodeType(node->typeId))
        return {kCardWidth, kTitleBand};
    if (layout != composition.nodeLayout().end() && layout->second.collapsed)
        return {width, kTitleBand * 2.0};
    const auto* definition =
        document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
    if (definition == nullptr)
        return document::kConservativeNodeCardSize;
    const bool wide =
        node->typeId == "bloom.video-source" || node->typeId == document::kVector2MathNodeType ||
        node->typeId == document::kVector3MathNodeType ||
        std::ranges::any_of(definition->parameters,
                            [](const auto& parameter) {
                                return parameter.valueKind == document::ParameterValueKind::String;
                            }) ||
        std::ranges::any_of(definition->inputs,
                            [](const auto& port) {
                                return port.valueKind == document::SocketValueKind::String;
                            }) ||
        std::ranges::any_of(definition->outputs, [](const auto& port) {
            return port.valueKind == document::SocketValueKind::String;
        });
    const bool dropdowns = std::ranges::any_of(
        definition->parameters, [](const auto& parameter) {
            // #383: the effect mask's channel is a compact dropdown that never widens a card.
            if (parameter.schemaKey == document::kEffectMaskChannelSchemaKey)
                return false;
            return !document::parameterEnumOptions(parameter.schemaKey).empty() ||
                   parameter.schemaKey.starts_with("bloom.ocio-") ||
                   parameter.schemaKey == document::kImageInputColorSpaceIdParameterSchemaKey ||
                   parameter.schemaKey == document::kVideoInputColorSpaceIdParameterSchemaKey ||
                   // #487: a data reader's block dropdown and a channel checklist.
                   parameter.schemaKey == document::kDataBlockParameterSchemaKey ||
                   document::isPathSourceParameterSchemaKey(parameter.schemaKey) ||
                   parameter.schemaKey == "bloom.channel-copy.select" ||
                   parameter.schemaKey == "bloom.channel-remove.select" ||
                   parameter.schemaKey == document::kCompositionOutputLayersParameterSchemaKey;
        });
    // A vector parameter takes one row per component (they share the first one's socket).
    double rows = 0.0;
    // #494: a Grade or Levels card shows its widget instead of its value rows (Levels keeps its
    // clamp row), until a Values group is opened.
    const bool cardWidget = node->typeId == document::kGradeNodeType ||
                            node->typeId == document::kLevelsNodeType ||
                            node->typeId == document::kCurvesNodeType;
    for (const auto& binding : node->parameters) {
        if (cardWidget && binding.role != "clamp")
            continue;
        const auto parameter = std::ranges::find(definition->parameters, binding.role,
                                                 &document::ParameterDefinition::role);
        const auto kind = parameter == definition->parameters.end()
                              ? document::ParameterValueKind::Float64
                              : parameter->valueKind;
        // A colour is its chip plus one row per channel.
        rows += kind == document::ParameterValueKind::Vec2d     ? 2.0
                : kind == document::ParameterValueKind::Vec3d   ? 3.0
                : kind == document::ParameterValueKind::Color4d ? 4.0
                                                                : 1.0;
    }
    double sockets = static_cast<double>(definition->outputs.size());
    for (const auto& port : definition->inputs)
        if (std::ranges::none_of(node->parameters,
                                 [&](const auto& binding) { return binding.role == port.name; }))
            sockets += 1.0;
    double pills = 0.0;
    if (const auto* stack = composition.graph().merge(id)) {
        const double slots = static_cast<double>(stack->entries().size());
        const double pill = std::max(kRow, slots * kSlotPitch);
        pills += definition->layerSlotInput ? pill : 0.0;
        pills += definition->audioLayerSlotInput ? pill : 0.0;
    }
    // Sources carry a dimensions/range readout row beside their parameters and generators a
    // second closing row; a bounds readout shows each value it reads out on its own row.
    rows += 2.0;
    // #383: the effect mask's socket row and channel dropdown sit below the effect's own rows,
    // with a little more room than a plain row.
    if (document::hasEffectMask(*definition))
        rows += 0.5;
    if (definition->lowering == document::NodeLoweringKind::ValueBoundsReadout)
        rows += static_cast<double>(definition->outputs.size());
    const double cardWidth = dropdowns ? std::max(width, kDropdownCardWidth)
                             : node->typeId == document::kCurvesNodeType
                                 ? std::max(width, kCurvesCardWidth)
                             : node->typeId == document::kGradeNodeType
                                 ? std::max(width, kGradeCardWidth)
                             : node->typeId == document::kLevelsNodeType
                                 ? std::max(width, kLevelsCardWidth)
                             : node->typeId == document::kChromaticAberrationNodeType
                                 ? std::max(width, kPadCardWidth)
                             : node->typeId == document::kFrameRateHoldNodeType
                                 ? std::max(width, kMotionCardWidth)
                             : wide ? std::max(width, kWideCardWidth)
                                    : width;
    // #490: an Image Preview draws its thumbnail below its rows at the composition's aspect, at
    // most twice as tall as it is wide.
    double thumbnail = 0.0;
    if (definition->lowering == document::NodeLoweringKind::ImagePreview) {
        const auto& format = composition.format();
        const double inner = std::max(0.0, cardWidth - 2.0 * kPadding);
        const double aspect = format.width() > 0 ? static_cast<double>(format.height()) /
                                                       static_cast<double>(format.width())
                                                 : 1.0;
        thumbnail = std::min(inner * aspect, inner * 2.0) + kPadding;
    }
    // #498: the Curves card widget: its tab row and its curve row, the graph three quarters as tall
    // as wide, and the selected point's row.
    if (node->typeId == document::kCurvesNodeType)
        thumbnail = 3.0 * kControl + 3.0 * kSmall +
                    std::round(std::max(0.0, cardWidth - 2.0 * kPadding) * kCurveGraphAspect) +
                    kPadding;
    // #494, #501: the wheels two by two (each its title, its disc, its level row over its jog
    // strip and its colour values), then the primaries' title, five rows over their sliders, the
    // Pivot row and its contrast curve; the Levels tabs, the histogram switch, the plot with its
    // handles and output bar, and the input and output values.
    const double inner = std::max(0.0, cardWidth - 2.0 * kPadding);
    if (node->typeId == document::kGradeNodeType) {
        const double cell = std::max(0.0, std::floor((inner - kPadding) / 2.0));
        const double wheel = 4.0 * kControl + 3.0 * kSmall + std::round(cell * kGradeDiscShare);
        thumbnail = 2.0 * wheel + 2.0 * kPadding + (kControl + kSmall) +
                    5.0 * (2.0 * kControl + kSmall) + (kControl + kSmall) + 2.0 * kControl +
                    kPadding;
    }
    if (node->typeId == document::kLevelsNodeType)
        thumbnail = 2.0 * (kControl + kSmall) + std::round(inner * kLevelsPlotAspect) +
                    2.0 * kLevelsHandle + kSmall + kPadding + kSmall + 2.0 * kControl + kSmall +
                    kPadding;
    // #505: the Vignette card's point pad and its five slider rows, each a value over a slider.
    if (node->typeId == document::kVignetteNodeType)
        thumbnail = kPadMaxHeight + kSmall + 5.0 * (kControl + kSmall) + kPadding;
    // #507: the Lens Flare card's preset button, pad, dial and element picker, its three always
    // shown slider rows and the first part's six.
    if (node->typeId == document::kLensFlareNodeType)
        thumbnail = kPadMaxHeight + kDialSide + 2.0 * kControl + 9.0 * (kControl + kSmall) +
                    4.0 * kSmall + kPadding;
    // #486: the Film Grain card's response graph and its eight slider rows, each a value over a
    // slider. #565: Sensor Noise shows nine (Shadow weighting, Chroma blotches, Blotch size and
    // Fixed pattern in place of the three response rows).
    if (node->typeId == document::kFilmGrainNodeType) {
        double grainRows = 8.0;
        for (const auto& binding : node->parameters)
            if (binding.role == "mode")
                if (const auto* record = composition.parameters().find(binding.parameterId))
                    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source))
                        if (const auto* mode = std::get_if<std::int64_t>(&constant->value);
                            mode != nullptr && *mode == document::kFilmGrainSensorNoise)
                            grainRows = 9.0;
        thumbnail = kFilmGrainGraphHeight + kSmall + grainRows * (kControl + kSmall) + kPadding;
    }
    // #527: the Dither card's preview, its Amplitude row (a value over a slider) and its readout.
    if (node->typeId == document::kDitherNodeType)
        thumbnail = kDitherPreviewHeight + kSmall + (kControl + kSmall) + kSmall + kDitherReadout +
                    kPadding;
    // #484: the Spin and Zoom Blur cards' centre pad.
    if (node->typeId == document::kSpinBlurNodeType || node->typeId == document::kZoomBlurNodeType)
        thumbnail = kPadMaxHeight + kSmall + kPadding;
    // #486: the Sharpen card's edge graph and its three slider rows, each a value over a slider.
    if (node->typeId == document::kSharpenNodeType)
        thumbnail = kSharpenGraphHeight + kSmall + 3.0 * (kControl + kSmall) + kPadding;
    // #486: the Lens Distortion card's centre pad and its four slider rows, each a value over a
    // slider.
    if (node->typeId == document::kLensDistortionNodeType)
        thumbnail = kPadMaxHeight + kSmall + 4.0 * (kControl + kSmall) + kPadding;
    // #486: the Chromatic Aberration card's centre pad and its one slider row.
    if (node->typeId == document::kChromaticAberrationNodeType)
        thumbnail = kPadMaxHeight + kSmall + (kControl + kSmall) + kPadding;
    // #508: the Light Rays card's centre pad, its three slider rows and its colour row.
    if (node->typeId == document::kLightRaysNodeType)
        thumbnail = kPadMaxHeight + kSmall + 4.0 * (kControl + kSmall) + kPadding;
    // #348: the Corner Pin card's pad.
    if (node->typeId == document::kCornerPinNodeType)
        thumbnail = kPadMaxHeight + kPadding;
    // #513: the Stabilize card's pad, its Track button and a status line.
    if (node->typeId == document::kStabilizeNodeType)
        thumbnail = kPadMaxHeight + kPadding + kControl + 3.0 * kSmall;
    // #524: the Hue/Saturation card's Vibrance row, a value over a slider, beside the rows the
    // generic count already makes.
    if (node->typeId == document::kHueSaturationNodeType)
        thumbnail = 2.0 * (kControl + kSmall);
    // #524: the Four-Color Gradient card's pad is drawn with the colors' disclosure rows beneath.
    if (node->typeId == document::kFourColorGradientNodeType)
        thumbnail = 3.0 * kPadding;
    // #563: the Codec Degrade card's two slider rows, each a value over a slider.
    if (node->typeId == document::kCodecDegradeNodeType)
        thumbnail = 2.0 * (kControl + kSmall) + kPadding;
    const auto controls = nodeControlsEstimate(node->typeId);
    double controlsHeight = static_cast<double>(controls.angles) * (kDialSide - kRow + kSmall) +
                            static_cast<double>(controls.ranges) * (kControl + 2.0 * kSmall) +
                            static_cast<double>(controls.gradients) * (kGradientRow - kRow + kSmall) +
                            (controls.pad ? kPadMaxHeight + kSmall : 0.0) +
                            static_cast<double>(controls.sliders) * (2.0 * kControl - kRow + kSmall);
    const double controlsWidth =
        node->typeId == document::kFieldNoiseNodeType ? std::max(cardWidth, kFieldNoiseCardWidth)
        : controls.pad                                 ? std::max(cardWidth, kPadCardWidth)
                                                       : cardWidth;
    controlsHeight += static_cast<double>(controls.curves) *
                      (kControl + std::round(controlsWidth * kCurveGraphAspect) + 2.0 * kSmall);
    if (controlsHeight > 0.0)
        controlsHeight += kPadding;
    return {controlsWidth, kTitleBand + sockets * kRow + pills + rows * kRow + kPadding + thumbnail +
                               controlsHeight};
}

NodeCardSizes nodeCardSizes(const document::Composition& composition,
                            const NodeCardSizes& measured) {
    NodeCardSizes sizes;
    for (const auto& node : composition.graph().nodes()) {
        const auto found = measured.find(node.id);
        sizes.emplace(node.id, found != measured.end() && sane(found->second)
                                   ? found->second
                                   : estimatedNodeCardSize(composition, node.id));
    }
    return sizes;
}

Vec2d effectiveNodePosition(const document::Composition& composition, const NodeId node) {
    if (const auto found = composition.nodeLayout().find(node);
        found != composition.nodeLayout().end())
        return found->second.position;
    const auto defaults = document::defaultNodeLayout(composition.graph().nodes());
    const auto found = defaults.find(node);
    return found == defaults.end() ? Vec2d{} : found->second.position;
}

NodeCardSizes nodeCardFloorSizes(const document::Composition& composition,
                                 const NodeCardSizes& measured) {
    auto sizes = nodeCardSizes(composition, measured);
    for (auto& [id, size] : sizes) {
        if (measured.contains(id))
            continue;
        const auto layout = composition.nodeLayout().find(id);
        size.width = std::max(240.0, layout != composition.nodeLayout().end() &&
                                             std::isfinite(layout->second.width)
                                         ? layout->second.width
                                         : 0.0);
    }
    return sizes;
}

Vec2d placeNodeCard(const document::Composition& composition, NodeCardSize size, Vec2d preferred,
                    const NodeCardSizes& sizes, const std::set<NodeId>& ignored,
                    const double acceptGap, const NodeCardSizes* acceptSizes,
                    const std::optional<NodeCardSize> acceptSize) {
    if (!sane(size))
        size = document::kConservativeNodeCardSize;
    if (!std::isfinite(preferred.x) || !std::isfinite(preferred.y))
        preferred = {};
    const auto rects = obstacles(composition, sizes, ignored);
    const auto at = [&](const double x, const double y) {
        return Rect{x, y, size.width, size.height};
    };
    const auto accepting = acceptSize && sane(*acceptSize) ? *acceptSize : size;
    if (clear(acceptSizes != nullptr ? obstacles(composition, *acceptSizes, ignored) : rects,
              Rect{preferred.x, preferred.y, accepting.width, accepting.height},
              std::max(0.0, acceptGap)))
        return preferred;

    // Candidates are the positions just past each neighbour on either axis, on the grid and a full
    // gap away, plus the preferred coordinate itself. Single-axis moves come first: the nearest
    // clear one wins, and the card below every other card in its column is always clear, so this
    // search always ends.
    std::vector<double> xs{preferred.x};
    std::vector<double> ys{preferred.y};
    for (const auto& rect : rects) {
        xs.push_back(snapUp(rect.right() + kNodeLayoutGap));
        xs.push_back(snapDown(rect.left - kNodeLayoutGap - size.width));
        ys.push_back(snapUp(rect.bottom() + kNodeLayoutGap));
        ys.push_back(snapDown(rect.top - kNodeLayoutGap - size.height));
    }
    std::optional<std::tuple<double, double, double>> best; // distance, y, x
    const auto consider = [&](const double x, const double y) {
        const double dx = x - preferred.x;
        const double dy = y - preferred.y;
        const std::tuple candidate{dx * dx + dy * dy, y, x};
        if (best && !(candidate < *best))
            return;
        if (clear(rects, at(x, y), kNodeLayoutGap))
            best = candidate;
    };
    for (const auto y : ys)
        consider(preferred.x, y);
    for (const auto x : xs)
        consider(x, preferred.y);
    if (best)
        return {std::get<2>(*best), std::get<1>(*best)};
    return document::findNearestFreeNodePosition(composition.nodeLayout(), sizes, size, preferred,
                                                 kNodeLayoutGap);
}

void placeLayerCards(document::Composition& composition, const NodeId layerNode,
                     const NodeId sourceNode, const NodeCardSizes& measured) {
    const auto& graph = composition.graph();
    const auto sizes = nodeCardSizes(composition, measured);
    const auto sizeOf = [&](const NodeId id) {
        const auto found = sizes.find(id);
        return found == sizes.end() ? document::kConservativeNodeCardSize : found->second;
    };
    // The stack this layer feeds, and its place in that stack's order (entry zero is the top).
    std::optional<document::LayerStackInputRef> slot;
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<document::LayerStackInputRef>(&edge.destination);
            input != nullptr && edge.source.nodeId == layerNode)
            slot = *input;
    std::optional<NodeId> above;
    std::optional<NodeId> below;
    std::optional<NodeId> stackNode;
    if (slot) {
        stackNode = slot->stackNodeId;
        if (const auto* stack = graph.merge(slot->stackNodeId)) {
            const auto entries = stack->entries();
            const auto boundaryNode = [&](const document::LayerId layer) -> std::optional<NodeId> {
                for (const auto& boundary : graph.layerOutputs())
                    if (boundary.layerId == layer && !boundary.entryOwned() &&
                        boundary.nodeId != layerNode)
                        return boundary.nodeId;
                return std::nullopt;
            };
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (entries[index].slotId != slot->slotId)
                    continue;
                for (std::size_t up = index; up-- > 0 && !above;)
                    above = boundaryNode(entries[up].layerId);
                for (std::size_t down = index + 1; down < entries.size() && !below; ++down)
                    below = boundaryNode(entries[down].layerId);
            }
        }
    }
    const std::set<NodeId> placed{layerNode, sourceNode};
    const auto layerSize = sizeOf(layerNode);
    const auto sourceSize = sizeOf(sourceNode);
    const double rowHeight = std::max(layerSize.height, sourceSize.height);
    // One neighbouring layer's row: the band its Layer Output and everything upstream of it fill.
    const auto rowBand = [&](const NodeId neighbour) {
        double top = std::numeric_limits<double>::infinity();
        double bottom = -std::numeric_limits<double>::infinity();
        for (const auto id : upstreamOf(composition, neighbour)) {
            if (placed.contains(id))
                continue;
            const auto position = effectiveNodePosition(composition, id);
            top = std::min(top, position.y);
            bottom = std::max(bottom, position.y + sizeOf(id).height);
        }
        return std::pair{top, bottom};
    };
    Vec2d layerPreferred;
    if (above || below) {
        const auto neighbour = above ? *above : *below;
        layerPreferred.x = effectiveNodePosition(composition, neighbour).x;
        if (above)
            layerPreferred.y = snapUp(rowBand(*above).second + kNodeLayoutGap);
        else
            layerPreferred.y = snapDown(rowBand(*below).first - kNodeLayoutGap - rowHeight);
    } else if (stackNode) {
        const auto merge = effectiveNodePosition(composition, *stackNode);
        layerPreferred = {snapDown(merge.x - kNodeLayoutGap - layerSize.width), merge.y};
    }
    const auto layerPosition =
        placeNodeCard(composition, layerSize, layerPreferred, sizes, placed, kNodeLayoutGap);
    composition.nodeLayout()[layerNode] = {layerPosition, 128.0, false, false};
    const Vec2d sourcePreferred{snapDown(layerPosition.x - kNodeLayoutGap - sourceSize.width),
                                layerPosition.y};
    const auto sourcePosition =
        placeNodeCard(composition, sourceSize, sourcePreferred, sizes, {sourceNode}, kNodeLayoutGap);
    composition.nodeLayout()[sourceNode] = {sourcePosition, 128.0, false, false};
}

} // namespace bloom::commands
