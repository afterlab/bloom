#include "properties_registry_row.hpp"
#include "channel_checklist_field.hpp"
#include "node_editor_items.hpp"
#include "numeric_field_projection.hpp"
#include "properties_sections.hpp"
#include "properties_text_edit.hpp"
#include "properties_value_edits.hpp"
#include "row_inline_controls.hpp"
#include "font_picker.hpp"
#include "value_presentation.hpp"
#include <bloom/document/denoise_nodes.hpp>
#include <bloom/document/parameter_activity.hpp>
#include <bloom/document/four_color_gradient_nodes.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/audio_effect_nodes.hpp>
#include <bloom/document/codec_degrade_nodes.hpp>
#include <bloom/document/lens_distortion_nodes.hpp>
#include <bloom/document/test_card_nodes.hpp>
#include <bloom/document/chromatic_aberration_nodes.hpp>
#include <bloom/document/rolling_shutter_nodes.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/document/color_cleanup_nodes.hpp>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <optional>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/channel_set_controller.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <filesystem>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

namespace bloom::ui {
namespace {

[[nodiscard]] bool isFontSchema(const std::string_view schema) {
    return schema == document::kTextFontParameterSchemaKey;
}

} // namespace

document::ShapeKind nodeShapeKind(const document::Composition& composition,
                                  const document::NodeRecord& node) {
    for (const auto& binding : node.parameters)
        if (binding.role == "kind") {
            const auto* p = composition.parameters().find(binding.parameterId);
            const auto* c = p ? std::get_if<document::ConstantValueSource>(&p->source) : nullptr;
            if (const auto* kind = c ? std::get_if<std::int64_t>(&c->value) : nullptr)
                return static_cast<document::ShapeKind>(*kind);
        }
    return document::ShapeKind::Rectangle;
}

bool propertiesRowInactive(const document::Composition& composition,
                           const document::NodeRecord& node, const std::string_view schemaKey) {
    return document::isParameterInactive(composition, node, schemaKey);
}

// #525: the Fields parameters whose role alone does not say what they do. The noise rows carry the
// fractal-noise names an artist knows from a layer-based compositor (Complexity, Sub Influence,
// Sub Scaling, Sub Rotation, Sub Offset, Offset Turbulence, Evolution) beside the standard ones.
[[nodiscard]] std::optional<QString> fieldParameterLabel(const std::string_view schemaKey) {
    struct Label final {
        std::string_view key;
        const char* text;
    };
    static constexpr std::array kLabels{
        Label{"bloom.field-noise.type", "Noise type"},
        Label{"bloom.field-noise.fractal", "Fractal type"},
        Label{"bloom.field-noise.seed", "Random seed"},
        Label{"bloom.field-noise.size", "Scale"},
        Label{"bloom.field-noise.offset", "Offset (Offset Turbulence)"},
        Label{"bloom.field-noise.octaves", "Complexity (octaves)"},
        Label{"bloom.field-noise.lacunarity", "Sub Scaling (lacunarity)"},
        Label{"bloom.field-noise.gain", "Sub Influence (gain)"},
        Label{"bloom.field-noise.octaveRotation", "Sub Rotation"},
        Label{"bloom.field-noise.octaveOffset", "Sub Offset"},
        Label{"bloom.field-noise.ridgeOffset", "Ridge offset"},
        Label{"bloom.field-noise.ridgeGain", "Ridge sharpness"},
        Label{"bloom.field-noise.evolution", "Evolution (cycles)"},
        Label{"bloom.field-noise.evolutionMode", "Evolution motion"},
        Label{"bloom.field-noise.loop", "Cycle evolution"},
        Label{"bloom.field-noise.loopLength", "Cycle length (0 = composition)"},
        Label{"bloom.field-noise.overflow", "Overflow"},
        Label{"bloom.field-noise.range", "Output range"},
        Label{"bloom.field-voronoi.mode", "Distance output"},
        Label{"bloom.field-voronoi.metric", "Distance metric"},
        Label{"bloom.field-voronoi.seed", "Random seed"},
        Label{"bloom.field-voronoi.size", "Cell size"},
        Label{"bloom.field-voronoi.jitter", "Randomness"},
        Label{"bloom.field-voronoi.evolution", "Evolution (cycles)"},
        Label{"bloom.field-voronoi.octaves", "Layers (octaves)"},
        Label{"bloom.field-random.seed", "Random seed"},
        Label{"bloom.field-random.cellSize", "Cell size"},
        Label{"bloom.field-random.smooth", "Smooth between cells"},
        Label{"bloom.field-random.evolution", "Evolution (cycles)"},
        Label{"bloom.field-pattern.size", "Period"},
        Label{"bloom.field-pattern.width", "Line width"},
        Label{"bloom.field-pattern.phase", "Phase (cycles)"},
        Label{"bloom.field-pattern.waveform", "Profile"},
        Label{"bloom.field-pattern.speed", "Speed (cycles a second)"},
        Label{"bloom.field-pattern.softness", "Edge softness"},
        Label{"bloom.denoise.mode", "Mode"},
        Label{"bloom.denoise.radius", "Radius (pixels)"},
        Label{"bloom.denoise.strength", "Strength (noise tolerance)"},
        Label{"bloom.denoise.frames", "Frames"},
        Label{"bloom.denoise.auto-strength", "Auto strength"},
        Label{"bloom.denoise.sample-from", "Sample corner 1"},
        Label{"bloom.denoise.sample-to", "Sample corner 2"},
        Label{"bloom.four-color-gradient.point1", "Point 1"},
        Label{"bloom.four-color-gradient.color1", "Color 1"},
        Label{"bloom.four-color-gradient.point2", "Point 2"},
        Label{"bloom.four-color-gradient.color2", "Color 2"},
        Label{"bloom.four-color-gradient.point3", "Point 3"},
        Label{"bloom.four-color-gradient.color3", "Color 3"},
        Label{"bloom.four-color-gradient.point4", "Point 4"},
        Label{"bloom.four-color-gradient.color4", "Color 4"},
        Label{"bloom.four-color-gradient.blend", "Blend (sharpness)"},
        Label{"bloom.field-gradient.repeat", "Outside the ramp"},
        Label{"bloom.field-gradient.cycle", "Phase (cycles)"},
        Label{"bloom.field-shape-distance.source", "Shape from"},
        Label{"bloom.field-shape-distance.falloff", "Falloff distance"},
        Label{"bloom.field-shape-distance.falloffCurve", "Falloff curve"},
        Label{"bloom.field-shape-distance.sides", "Sides or points"},
        Label{"bloom.field-from-image.channel", "Scalar from"},
        Label{"bloom.field-from-image.pairFirst", "Vector X from"},
        Label{"bloom.field-from-image.pairSecond", "Vector Y from"},
        Label{"bloom.field-from-image.filter", "Sampling"},
        Label{"bloom.field-from-image.edge", "Outside the image"},
        Label{"bloom.field-force.mode", "Force type"},
        Label{"bloom.field-force.direction", "Spin"},
        Label{"bloom.field-force.radius", "Radius"},
        Label{"bloom.field-force.angle", "Direction"},
        Label{"bloom.field-math.valueA", "A when unconnected"},
        Label{"bloom.field-math.valueB", "B when unconnected"},
        Label{"bloom.field-math.valueT", "T when unconnected"},
        Label{"bloom.field-remap.inLow", "Input low"},
        Label{"bloom.field-remap.inHigh", "Input high"},
        Label{"bloom.field-remap.outLow", "Output low"},
        Label{"bloom.field-remap.outHigh", "Output high"},
        Label{"bloom.field-remap.clamp", "Clamp output"},
        Label{"bloom.field-remap.steps", "Steps (0 = smooth)"},
        Label{"bloom.field-remap.gain", "Gain (contrast)"},
        Label{"bloom.field-transform.mode", "Coordinate mode"},
        Label{"bloom.field-transform.tile", "Tile size"},
        Label{"bloom.field-transform.segments", "Kaleidoscope segments"},
        Label{"bloom.field-transform.warpAmount", "Warp amount"},
        Label{"bloom.field-derivative.step", "Step"},
        Label{"bloom.field-combine.valueX", "X when unconnected"},
        Label{"bloom.field-combine.valueY", "Y when unconnected"},
        Label{"bloom.field-combine.valueZ", "Z when unconnected"},
        Label{"bloom.field-combine.valueW", "W when unconnected"},
        Label{"bloom.field-rasterize.explicitSize", "Explicit size"},
        Label{"bloom.field-rasterize.supersample", "Sub-samples"},
        Label{"bloom.field-rasterize.inspect", "Inspect"},
        Label{"bloom.field-rasterize.arrowSpacing", "Arrow spacing"},
        Label{"bloom.field-rasterize.arrowScale", "Arrow length per unit"},
    };
    const auto* found = std::ranges::find(kLabels, schemaKey, &Label::key);
    if (found == kLabels.end())
        return std::nullopt;
    return QCoreApplication::translate("PropertiesRow", found->text);
}

QString parameterRowLabel(const std::string_view schemaKey, const std::string_view role) {
    if (auto field = fieldParameterLabel(schemaKey))
        return *field;
    // #483 Z Defocus and Depth Fog: the depth table's own labels.
    if (const auto* depth = document::effectParameterSpec(schemaKey); depth != nullptr)
        return QCoreApplication::translate("PropertiesRow",
                                           std::string(depth->label).c_str());
    // #507 Lens Flare: its table's own labels.
    if (const auto* flare = document::lensFlareParameterSpec(schemaKey); flare != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(flare->label).c_str());
    // #486 Film Grain: its table's own labels.
    if (const auto* grain = document::filmGrainParameterSpec(schemaKey); grain != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(grain->label).c_str());
    // #563 Codec Degrade: its table's own labels.
    if (const auto* codec = document::codecDegradeParameterSpec(schemaKey); codec != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(codec->label).c_str());
    // #566 Auto Exposure: its table's own labels.
    if (const auto* autoExposure = document::autoExposureParameterSpec(schemaKey);
        autoExposure != nullptr)
        return QCoreApplication::translate("PropertiesRow",
                                           std::string(autoExposure->label).c_str());
    // #569 audio effects: their tables' own labels.
    if (const auto* audioEffect = document::audioEffectParameterSpec(schemaKey);
        audioEffect != nullptr)
        return QCoreApplication::translate("PropertiesRow",
                                           std::string(audioEffect->label).c_str());
    // #564 Frame Rate Hold: its table's own labels.
    if (schemaKey.starts_with(document::kFrameRateHoldNodeType))
        if (const auto* hold = document::motionParameterSpec(schemaKey); hold != nullptr)
            return QCoreApplication::translate("PropertiesRow", std::string(hold->label).c_str());
    // #486 Chromatic Aberration.
    if (schemaKey == document::kChromaticAberrationAmountSchemaKey)
        return QCoreApplication::translate("PropertiesRow", "Amount");
    // #568 Rolling Shutter: its table's own labels; the two camera sockets read Camera Offset and
    // Camera Roll.
    if (const auto* shutter = document::rollingShutterParameterSpec(schemaKey); shutter != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(shutter->label).c_str());
    if (schemaKey == document::kRollingShutterCameraOffsetSchemaKey)
        return QCoreApplication::translate("PropertiesRow", "Camera Offset");
    if (schemaKey == document::kRollingShutterCameraRollSchemaKey)
        return QCoreApplication::translate("PropertiesRow", "Camera Roll");
    // #348 Corner Pin, ST Map and Displace: their tables' own labels; the corners by name.
    if (const auto* warp = document::warpParameterSpec(schemaKey); warp != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(warp->label).c_str());
    if (document::isWarpPointSchemaKey(schemaKey)) {
        const bool stabilize = schemaKey.starts_with("bloom.stabilize.");
        const auto roles =
            stabilize ? document::stabilizePointRoles() : document::cornerPinCornerRoles();
        const auto labels =
            stabilize ? document::stabilizePointLabels() : document::cornerPinCornerLabels();
        for (std::size_t index = 0; index < roles.size(); ++index)
            if (schemaKey.ends_with(roles[index]))
                return QCoreApplication::translate("PropertiesRow",
                                                   std::string(labels[index]).c_str());
    }
    // #486 Lens Distortion: its table's own labels.
    if (const auto* lens = document::lensDistortionParameterSpec(schemaKey); lens != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(lens->label).c_str());
    // #587 Test Card: its table's own labels.
    if (const auto* card = document::testCardParameterSpec(schemaKey); card != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(card->label).c_str());
    // #486 Sharpen: its table's own labels.
    if (const auto* sharpen = document::sharpenParameterSpec(schemaKey); sharpen != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(sharpen->label).c_str());
    // #518 Tone Map, #517 Color Matrix and #523 HSL Qualifier: their table's own labels.
    if (const auto* cleanup = document::colorCleanupParameterSpec(schemaKey); cleanup != nullptr)
        return QCoreApplication::translate("PropertiesRow", std::string(cleanup->label).c_str());
    return
        isEffectColorSpaceSchema(schemaKey)
            ? node_editor::displayTypeName(role)
        : isInputColorSpaceSchema(schemaKey)             ? QObject::tr("Input color space")
        : schemaKey == document::kTextParameterSchemaKey ? QObject::tr("Text")
        : schemaKey == "bloom.exposure.stops"            ? QObject::tr("Exposure")
        : schemaKey == "bloom.offset.amount"             ? QObject::tr("Offset")
        : schemaKey == "bloom.multiply.factor"           ? QObject::tr("Factor")
        // #501: what the switch clamps.
        : schemaKey == document::kLevelsClampSchemaKey ||
                schemaKey == document::kCurvesClampSchemaKey
            ? QObject::tr("Clamp to 0\u20131")
        : schemaKey == "bloom.image-mix.factor"          ? QObject::tr("Mix factor")
        : schemaKey == "bloom.multiply-alpha.factor"     ? QObject::tr("Alpha factor")
        : schemaKey == "bloom.multiply-alpha.channel" ||
                schemaKey == "bloom.set-alpha.channel" ||
                schemaKey == document::kImageToMaskChannelSchemaKey
            ? QObject::tr("Matte channel")
        // #383: the effect mask's selector and Mask Combine's mode.
        : schemaKey == document::kEffectMaskChannelSchemaKey ? QObject::tr("Mask channel")
        : schemaKey == document::kMaskCombineModeSchemaKey   ? QObject::tr("Combine")
        // #503: Unmult's backing and Remove Color Matting's colour.
        : schemaKey == document::kAlphaAssociationWhiteSchemaKey
            ? QObject::tr("White background")
        : schemaKey == document::kAlphaAssociationBackgroundSchemaKey
            ? QObject::tr("Matte color")
        // #504 Colorize: what each colour and choice is for.
        : schemaKey == document::kColorizeColorSchemaKey ? QObject::tr("Fill color")
        : schemaKey == document::kColorizeSourceSchemaKey ? QObject::tr("Map from")
        : schemaKey == document::kColorizeCycleSchemaKey  ? QObject::tr("Outside the ramp")
        : schemaKey == document::kColorizePhaseSchemaKey  ? QObject::tr("Phase (cycles)")
        : schemaKey == "bloom.rotate-90.quarter-turns" ? QObject::tr("Quarter turns (clockwise)")
        : schemaKey == "bloom.blur.radius"             ? QObject::tr("Radius (pixels)")
        : schemaKey == "bloom.blur.edge"               ? QObject::tr("Edges")
        : schemaKey == "bloom.blur.separate"           ? QObject::tr("Separate axes")
        : schemaKey == "bloom.blur.radius-y"           ? QObject::tr("Vertical radius (pixels)")
        : schemaKey == "bloom.blur.per-channel"        ? QObject::tr("Per-channel radius")
        : schemaKey == "bloom.blur.red-scale"          ? QObject::tr("Red radius")
        : schemaKey == "bloom.blur.green-scale"        ? QObject::tr("Green radius")
        : schemaKey == "bloom.blur.blue-scale"         ? QObject::tr("Blue radius")
        : schemaKey == "bloom.blur.alpha-scale"        ? QObject::tr("Alpha radius")
        : schemaKey == "bloom.transform-2d.skew"       ? QObject::tr("Skew")
        : schemaKey == "bloom.transform-2d.skew-axis"  ? QObject::tr("Skew axis")
        : schemaKey == "bloom.transform-2d.opacity"    ? QObject::tr("Opacity")
        : schemaKey == document::kHueSaturationVibranceSchemaKey ? QObject::tr("Vibrance")
        : schemaKey == document::kHueSaturationColorizeSchemaKey ? QObject::tr("Colorize")
        : schemaKey == "bloom.luma-key.low"            ? QObject::tr("Low luma")
        : schemaKey == "bloom.luma-key.high"           ? QObject::tr("High luma")
        : schemaKey == "bloom.luma-key.invert"         ? QObject::tr("Invert key")
        : schemaKey == document::kTextAlignmentParameterSchemaKey ? QObject::tr("Text Alignment")
        : schemaKey == document::kTextLineHeightParameterSchemaKey ? QObject::tr("Line Height")
        : schemaKey == document::kTextLetterSpacingParameterSchemaKey
            ? QObject::tr("Letter Spacing")
        : schemaKey == document::kTextSizeParameterSchemaKey
            ? QObject::tr("Font Size")
            : node_editor::displayTypeName(role);
}

PropertiesRowVisibility
propertiesRowVisibility(const std::string_view role, const std::string_view schemaKey,
                        const std::optional<document::ShapeKind> shape) noexcept {
    if (shape && !document::shapeRoleVisible(*shape, role))
        return PropertiesRowVisibility::Hidden;
    // These are presentation metadata, not authored controls. Keep this classification in the UI
    // registry mapping: the document vocabulary remains the durable source of truth, while the
    // Properties surface decides which declared values are meaningful to an artist.
    const bool technicalRole = role == "alpha-association" || role == "color-encoding" ||
                               role == "color-space" || role == "encoding";
    // ADR 0026: part, layer and the output layer list have purpose-built pickers, as does the
    // Video node's audio stream (document 1.25).
    const bool technicalSchema =
        schemaKey == "bloom.color.alpha-association" || schemaKey == "bloom.color.encoding" ||
        schemaKey == "bloom.solid.alpha-association" || schemaKey == "bloom.solid.encoding" ||
        schemaKey == "bloom.image.part" || schemaKey == "bloom.image.layer" ||
        schemaKey == "bloom.composition-output.layers" || schemaKey == "bloom.cryptomatte.layer" ||
        schemaKey == "bloom.cryptomatte.mattes" ||
        schemaKey == document::kVideoAudioStreamParameterSchemaKey ||
        // #487: the legacy source Color Space lives in the one Input color space control.
        schemaKey == "bloom.image.color-space" || schemaKey == "bloom.video.color-space";
    return technicalRole || technicalSchema ? PropertiesRowVisibility::Hidden
                                            : PropertiesRowVisibility::Visible;
}

PropertiesRegistryRow::PropertiesRegistryRow(CompositionSession& session, document::NodeId node,
                                             document::ParameterId parameter,
                                             document::ParameterDefinition definition,
                                             QWidget* parent)
    : QWidget(parent), session_(session), node_(node), parameter_(parameter),
      definition_(std::move(definition)) {
    setObjectName("propertiesRegistryRow");
    setProperty("parameterId", QVariant::fromValue(static_cast<qulonglong>(parameter.value())));
    setProperty("role", QString::fromStdString(definition_.role));
    const auto label = parameterRowLabel(definition_.schemaKey, definition_.role);
    setProperty("rowLabel", label);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(kit::px(kit::Spacing::Gutter));
    auto* controls = new QWidget(this);
    auto* layout = new QHBoxLayout(controls);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    diamond_ = new KeyframeDiamond(session_, definition_.role, this);
    diamond_->setObjectName("propertiesRegistryDiamond");
    diamond_->setParameterId(parameter_);

    const auto items = propertiesSelectorItems(definition_.schemaKey);
    if (definition_.valueKind == document::ParameterValueKind::Path) {
        auto* summary = new kit::KLabel(controls);
        summary->setObjectName("propertiesPathSummary");
        summary->setTone(kit::Color::Foreground);
        layout->addWidget(summary);
    } else if (definition_.schemaKey == document::kAudioLevelParameterSchemaKey) {
        slider_ = new kit::KSlider(controls);
        slider_->setObjectName(QStringLiteral("propertiesAudioLevelSlider"));
        slider_->setAccessibleName(label);
        // The level slider is in stored units (0 to 2, unity at 1): its soft range IS the domain.
        const auto level = document::parameterPresentation(definition_);
        slider_->setRange(level.softMinimum, level.softMaximum);
        properties::bindValueEdit(session_, *slider_, [this] { return parameter_; });
        layout->addWidget(slider_);
        connect(slider_, &kit::KSlider::valueChanged, this, [this] { commit(); });
    } else if (propertiesRowControl(definition_.schemaKey) == PropertiesRowControl::SegmentedEnum) {
        segments_ = new kit::KRadioGroup(controls);
        segments_->setObjectName("propertiesRegistryEnum");
        segments_->setAccessibleName(label);
        segments_->setIconsOnly(true);
        const std::array icons{kit::IconId::AlignLeft, kit::IconId::AlignCenter,
                               kit::IconId::AlignRight};
        for (int index = 0; index < items.size(); ++index)
            segments_->addOption(items[index].first, icons[static_cast<std::size_t>(index)]);
        segments_->setFixedSize(segments_->sizeHint());
        layout->addWidget(segments_);
        connect(segments_, &kit::KRadioGroup::currentIndexChanged, this, [this] { commit(); });
    } else if (const auto checklistKind = channelChecklistKind(definition_.schemaKey)) {
        // #487: a channel pattern is chosen from the channels the input carries.
        checklist_ =
            new ChannelChecklistField(session_, node_, definition_.role, *checklistKind, controls);
        checklist_->setObjectName(QStringLiteral("propertiesChannelChecklist"));
        checklist_->setAccessibleName(label);
        layout->addWidget(checklist_, 1);
    } else if (isInputColorSpaceSchema(definition_.schemaKey) || !items.empty() ||
               isShuffleSlotSchema(definition_.schemaKey) ||
               definition_.schemaKey == document::kDataBlockParameterSchemaKey ||
               definition_.schemaKey == "bloom.ocio-file.lut" ||
               definition_.schemaKey == "bloom.image.asset" ||
               definition_.schemaKey == "bloom.video.asset" ||
               definition_.schemaKey == "bloom.audio.asset") {
        selector_ = new kit::KDropdown(controls);
        selector_->setObjectName(
            definition_.schemaKey == "bloom.ocio-file.lut"       ? "propertiesLutAsset"
            : isEffectColorSpaceSchema(definition_.schemaKey)    ? "propertiesEffectColorSpace"
            : definition_.schemaKey == "bloom.image.asset"       ? "propertiesImageAsset"
            : definition_.schemaKey == "bloom.audio.asset"       ? "propertiesAudioAsset"
            : definition_.schemaKey == "bloom.image.loop-mode"   ? "propertiesImageLoopMode"
            : definition_.schemaKey == "bloom.image.color-space" ? "propertiesImageColorSpace"
            : definition_.schemaKey == document::kImageInputColorSpaceIdParameterSchemaKey
                ? "propertiesImageInputColorSpace"
            : definition_.schemaKey == document::kVideoInputColorSpaceIdParameterSchemaKey
                ? "propertiesVideoInputColorSpace"
            : isShuffleSlotSchema(definition_.schemaKey) ? "propertiesShuffleSlot"
            : definition_.schemaKey == document::kDataBlockParameterSchemaKey
                ? "propertiesDataBlock"
                : "propertiesRegistryEnum");
        selector_->setControlSize(kit::KDropdown::ControlSize::Dense);
        if (isInputColorSpaceSchema(definition_.schemaKey)) {
            selector_->setSearchable(true);
            populateInputColorSpaceSelector();
        } else if (isShuffleSlotSchema(definition_.schemaKey)) {
            // A multi-pass render carries dozens of channels: type to find one.
            selector_->setSearchable(true);
        } else if (definition_.schemaKey == document::kDataBlockParameterSchemaKey) {
            // Filled on refresh from the project's blocks of the reader's kind.
        } else {
            for (const auto& [name, stored] : items)
                selector_->addItem(name, QVariant::fromValue(stored));
        }
        // Hugs its widest item where the row has room and elides down to the Properties dropdown
        // width where it does not.
        selector_->setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth));
        layout->addWidget(selector_);
        connect(selector_, &kit::KDropdown::activated, this, [this] { commit(); });
    } else if (isFontSchema(definition_.schemaKey)) {
        // #489: a font is a font ID chosen in the picker; there is no font asset to pick.
        fontField_ = new FontField(controls);
        fontField_->setAccessibleName(label);
        layout->addWidget(fontField_, 1);
        bindFontFieldToParameter(*fontField_, session_, [this] {
            return detached_ ? std::nullopt : std::optional(parameter_);
        });
    } else if (definition_.valueKind == document::ParameterValueKind::Integer) {
        // A whole-number parameter is a value field in integer mode: it scrubs and steps in whole
        // units, parses exactly across the int64 range, and edits live like every number.
        integer_ = new kit::KValueField(controls);
        integer_->setObjectName(
            definition_.schemaKey == "bloom.image.start-frame"   ? "propertiesImageStartFrame"
            : definition_.schemaKey == "bloom.audio.start-frame" ? "propertiesAudioStartFrame"
                                                                 : "propertiesRegistryInteger");
        integer_->setAccessibleName(label);
        integer_->setCompact(true);
        auto presentation = document::parameterPresentation(definition_);
        presentation.kind = document::ParameterPresentationKind::Integer;
        configureValueField(*integer_, presentation);
        layout->addWidget(integer_);
        properties::bindValueEdit(session_, *integer_, [this] { return parameter_; });
        connect(integer_, &kit::KValueField::integerValueChanged, this, [this] { commit(); });
        // #512: the Seed's Randomise button, beside the field.
        if (rowInlineFor(definition_.schemaKey) == RowInline::RandomiseSeed) {
            randomise_ = makeRandomiseSeedButton(
                session_, [this] { return std::optional(parameter_); }, controls);
            layout->addWidget(randomise_);
        }
    } else if (definition_.valueKind == document::ParameterValueKind::Boolean) {
        toggle_ = new kit::KSwitch(controls);
        toggle_->setObjectName(definition_.schemaKey == "bloom.image.premultiply"
                                   ? "propertiesImagePremultiply"
                                   : "propertiesRegistryBool");
        layout->addWidget(toggle_);
        connect(toggle_, &kit::KSwitch::toggled, this, [this] { commit(); });
    } else if (definition_.valueKind == document::ParameterValueKind::String) {
        text_ = new kit::KLineEdit(controls);
        text_->setObjectName("propertiesRegistryString");
        text_->setTypeRole(kit::TypeRole::Value);
        layout->addWidget(text_, 1);
        // The one text-parameter editor: live while typing, one commit when the edit ends.
        properties::bindTextEdit(session_, *text_, [this] { return parameter_; });
        if (definition_.schemaKey == document::kTextParameterSchemaKey) {
            multiline_ = new kit::KTextArea(this);
            multiline_->setObjectName("propertiesRegistryMultiline");
            multiline_->setFixedHeight(kit::px(kit::Size::MultilineHeight));
            properties::bindTextEdit(session_, *multiline_, [this] { return parameter_; });
            multiline_->hide();
            auto* expand = new kit::KButton(controls);
            expand->setObjectName("propertiesRegistryTextExpand");
            expand->setIconId(kit::IconId::CaretRight);
            expand->setVariant(kit::KButton::Variant::Ghost);
            expand->setCheckable(true);
            expand->setFixedSize(kit::px(kit::Size::Control),
                                 kit::px(kit::Size::Control));
            expand->setToolTip(tr("Edit multiple lines"));
            layout->addWidget(expand);
            connect(expand, &kit::KButton::toggled, this, [this, expand](bool on) {
                multiline_->setVisible(on);
                expand->setIconId(on ? kit::IconId::CaretDown : kit::IconId::CaretRight);
            });
        }
    } else {
        int count = 1;
        if (definition_.valueKind == document::ParameterValueKind::Vec2d)
            count = 2;
        if (definition_.valueKind == document::ParameterValueKind::Vec3d)
            count = 3;

        if (definition_.valueKind == document::ParameterValueKind::Color4d) {
            count = 4;
            color_ = new kit::KColorChip(controls);
            color_->setObjectName("propertiesRegistryColor");
            properties::bindValueEdit(session_, *color_, [this] { return parameter_; });
            connect(color_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& color) {
                if (!refreshing_) {
                    (void)session_.setParameterValue(
                        parameter_,
                        core::Color4d{
                            static_cast<double>(color.red), static_cast<double>(color.green),
                            static_cast<double>(color.blue), static_cast<double>(color.alpha)},
                        tr("Set Color"));
                    refresh();
                }
            });
        }
        // #512: an angle's dial before its field, as Transform's Rotation row.
        if (count == 1 && !color_ && rowInlineFor(definition_.schemaKey) == RowInline::AngleDial) {
            dial_ = makeRowAngleDial(
                session_, [this] { return parameter_; }, label,
                std::get_if<double>(&definition_.defaultValue)
                    ? *std::get_if<double>(&definition_.defaultValue)
                    : 0.0,
                controls);
            layout->addWidget(dial_);
        }
        for (int i = 0; i < count; ++i) {
            auto* field = fields_[static_cast<std::size_t>(i)] = new kit::KValueField(controls);
            field->setObjectName("propertiesRegistryValue");
            field->setAccessibleName(label);
            field->setCompact(true);
            // The parameter's own presentation, the same one its Timeline row and node card use.
            configureValueField(*field, document::parameterPresentation(definition_));
            if (definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey) {
                // Properties reads letter spacing as a percentage of the font size (refresh());
                // the stored value stays pixels, which is what the other surfaces show.
                field->setUnit(QStringLiteral("%"));
            }
            if (count == 3)
                field->setMinimumWidth(kit::px(kit::Size::PropertiesComponentMinWidth));
            field->setStepper(propertiesRowControl(definition_.schemaKey) ==
                              PropertiesRowControl::Stepper);
            if (count > 1)
                field->setLabel(QString(color_ ? "RGBA" : "XYZ").mid(i, 1));
            if (count > 1 && !color_) {
                const std::array components{document::AnimationComponent::X,
                                            document::AnimationComponent::Y,
                                            document::AnimationComponent::Z};
                layout->addWidget(properties::makeComponentCell(
                    session_, definition_.role, components[static_cast<std::size_t>(i)], field,
                    controls, parameter_));
            } else
                layout->addWidget(field);
            properties::bindValueEdit(
                session_, *field, [this] { return parameter_; },
                [count, i, this] {
                    if (count == 1)
                        return std::optional<document::AnimationComponent>{};
                    const auto first = color_ ? document::AnimationComponent::Red
                                              : document::AnimationComponent::X;
                    return std::optional(
                        static_cast<document::AnimationComponent>(static_cast<int>(first) + i));
                });
            connect(field, &kit::KValueField::valueChanged, this,
                    [this, i] { commit(static_cast<std::size_t>(i)); });
        }
    }
    if (color_) {
        (void)properties::addColorRow(session_, definition_.role, outer, this, color_, diamond_,
                                      {fields_[0], fields_[1], fields_[2], fields_[3]},
                                      "propertiesRegistryColorExpand",
                                      "propertiesRegistryColorFields", parameter_,
                                      // #347, #524: a node with several colors (audio plots, a Four-Color
                                      // Gradient's corners, Shape Source's fill
                                      // and stroke, a Checker's two cells) names which each is.
                                      label);
        delete controls;
    } else {
        // A Path's summary is filled in after the row is built, so its width is not known here.
        if (!text_ && !multiline_ && !checklist_ &&
            definition_.valueKind != document::ParameterValueKind::Path) {
            const int preferred = controls->sizeHint().width();
            controls->setMaximumWidth(preferred);
        }
        properties::addRow(outer, this, properties::makeRowLabel(label, this), diamond_, controls);
    }
    if (multiline_)
        outer->addWidget(multiline_);
    connect(&session_, &CompositionSession::liveValueChanged, this,
            &PropertiesRegistryRow::refresh);
    if (auto* controller = session_.assetController())
        connect(controller, &AssetController::changed, this, &PropertiesRegistryRow::refresh);
    if (auto* channels = session_.channelSetController();
        channels != nullptr && isShuffleSlotSchema(definition_.schemaKey))
        connect(channels, &ChannelSetController::changed, this, &PropertiesRegistryRow::refresh);
    refresh();
}

void PropertiesRegistryRow::populateInputColorSpaceSelector() {
    if (selector_ == nullptr || !isInputColorSpaceSchema(definition_.schemaKey))
        return;
    populateColorSpaceSelector(*selector_, session_, definition_.schemaKey,
                               session_.constantStringValue(parameter_).value_or(QString{}));
}

void PropertiesRegistryRow::detachFromSession() { detached_ = true; }

void PropertiesRegistryRow::refresh() {
    refreshing_ = true;
    const auto* composition = session_.composition();
    const auto* node = composition ? composition->graph().findNode(node_) : nullptr;
    const bool hidden =
        propertiesRowVisibility(definition_.role, definition_.schemaKey,
                                node && node->typeId == document::kShapeSourceNodeType
                                    ? std::optional(nodeShapeKind(*composition, *node))
                                    : std::nullopt) == PropertiesRowVisibility::Hidden ||
        (node != nullptr && propertiesRowInactive(*composition, *node, definition_.schemaKey));
    setProperty("roleHidden", hidden);
    setVisible(!hidden);
    const auto* parameter = composition ? composition->parameters().find(parameter_) : nullptr;
    const bool editable = parameter && displayScale() != 0.0 && !composition->nodeLocked(node_) &&
                          !std::holds_alternative<document::DriverBindingSource>(parameter->source);
    for (auto* field : fields_)
        if (field) {
            field->setEnabled(editable);
            field->setToolTip(
                displayScale() == 0.0
                    ? tr("Font size must resolve before editing percentage letter spacing")
                    : QString{});
        }
    for (auto* control : {static_cast<QWidget*>(selector_), static_cast<QWidget*>(segments_),
                          static_cast<QWidget*>(toggle_), static_cast<QWidget*>(color_),
                          static_cast<QWidget*>(text_), static_cast<QWidget*>(multiline_),
                          static_cast<QWidget*>(integer_), static_cast<QWidget*>(slider_),
                          static_cast<QWidget*>(checklist_), static_cast<QWidget*>(fontField_)})
        if (control)
            control->setEnabled(editable);
    if (dial_)
        refreshRowAngleDial(session_, *dial_, parameter ? std::optional(parameter_) : std::nullopt,
                            composition == nullptr || composition->nodeLocked(node_));
    if (randomise_)
        randomise_->setEnabled(editable);
    if (parameter) {
        auto value = definition_.defaultValue;
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source))
            value = constant->value;
        if (const auto live = session_.liveValue(parameter_))
            value = *live;
        if (const auto* path = std::get_if<document::PathValue>(&value))
            if (auto* summary = findChild<kit::KLabel*>("propertiesPathSummary"))
                summary->setText(
                    path->contours.size() > 1
                        ? tr("%1 contours · %2 anchors")
                              .arg(path->contours.size())
                              .arg(path->anchorCount())
                        : tr("%1 anchors · %2")
                              .arg(path->anchorCount())
                              .arg(path->firstContour().closed ? tr("Closed") : tr("Open")));
        if (const auto scalar = session_.effectiveScalarValue(parameter_))
            value = *scalar;
        if (const auto vector = session_.effectiveVec2Value(parameter_))
            value = *vector;
        if (const auto color = session_.effectiveColorValue(parameter_))
            value = *color;
        if (auto* scalar = std::get_if<double>(&value); scalar && fields_[0])
            if (displayScale() != 0.0)
                projectNumericField(
                    *fields_[0], *scalar, displayScale(),
                    definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey
                        ? QStringLiteral("%")
                        : QString::fromStdString(document::parameterPresentation(definition_).unit),
                    definition_.schemaKey == document::kTextLineHeightParameterSchemaKey
                        ? QStringLiteral("×")
                        : definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey
                        ? QStringLiteral("px")
                        : QString::fromStdString(document::parameterPresentation(definition_).unit));
        if (auto* scalar = std::get_if<double>(&value); scalar && slider_)
            slider_->setValue(*scalar);
        if (checklist_)
            checklist_->refresh();
        if (auto* integer = std::get_if<std::int64_t>(&value)) {
            if (selector_ && definition_.schemaKey == "bloom.ocio-file.lut")
                refreshLutSelector(*selector_, session_, *integer);
            if (selector_ && node &&
                definition_.schemaKey == document::kDataBlockParameterSchemaKey)
                refreshDataBlockSelector(*selector_, session_, *node);
            if (integer_)
                integer_->setIntegerValue(*integer);
            if (fields_[0])
                fields_[0]->setValue(static_cast<double>(*integer));
            if (segments_)
                segments_->setCurrentIndex(static_cast<int>(*integer));
            if (selector_) {
                for (int index = 0; index < selector_->count(); ++index)
                    if (selector_->itemData(index).value<std::int64_t>() == *integer)
                        selector_->setCurrentIndex(index);
            }
        }
        if (auto* vector = std::get_if<document::Vec2d>(&value); vector && fields_[1]) {
            fields_[0]->setValue(vector->x);
            fields_[1]->setValue(vector->y);
        }
        if (auto* vector = std::get_if<document::Vec3d>(&value); vector && fields_[2]) {
            fields_[0]->setValue(vector->x);
            fields_[1]->setValue(vector->y);
            fields_[2]->setValue(vector->z);
        }
        if (auto* color = std::get_if<core::Color4d>(&value); color && color_) {
            properties::refreshColor(session_, definition_.schemaKey, *color, color_,
                                     {fields_[0], fields_[1], fields_[2], fields_[3]});
        }
        if (auto* boolean = std::get_if<bool>(&value); boolean && toggle_)
            toggle_->setChecked(*boolean);
        if (auto* text = std::get_if<std::string>(&value)) {
            if (selector_ && definition_.schemaKey == "bloom.video.asset")
                refreshVideoAssetSelector(*selector_, session_, QString::fromStdString(*text));
            if (selector_ && definition_.schemaKey == "bloom.image.asset")
                refreshImageAssetSelector(*selector_, session_, QString::fromStdString(*text));
            if (selector_ && definition_.schemaKey == "bloom.audio.asset")
                refreshAudioAssetSelector(*selector_, session_, QString::fromStdString(*text));
            if (fontField_)
                fontField_->setFontText(QString::fromStdString(*text));
            if (selector_ && node && isShuffleSlotSchema(definition_.schemaKey)) {
                refreshShuffleSlotSelector(*selector_, session_, *node, definition_.schemaKey);
                // The probe enables a slot once it is listed; a locked node stays locked.
                selector_->setEnabled(selector_->isEnabled() && editable);
            }
            if (selector_ && isInputColorSpaceSchema(definition_.schemaKey))
                refreshColorSpaceSelector(*selector_, session_, node, definition_.schemaKey,
                                          QString::fromStdString(*text));
            if (text_)
                text_->setText(QString::fromStdString(*text));
            if (multiline_ && !multiline_->hasFocus())
                multiline_->setPlainText(QString::fromStdString(*text));
        }
    }
    diamond_->refresh();
    refreshing_ = false;
}

void PropertiesRegistryRow::reset() {
    resetPropertiesParameter(session_, parameter_);
    refresh();
}

double PropertiesRegistryRow::displayScale() const {
    if (definition_.schemaKey == document::kTextLineHeightParameterSchemaKey)
        return 100.0;
    if (definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey) {
        const auto* composition = session_.composition();
        const auto* node = composition ? composition->graph().findNode(node_) : nullptr;
        if (node)
            for (const auto& binding : node->parameters)
                if (binding.role == document::kTextSizeParameterRole)
                    if (const auto size = session_.effectiveScalarValue(binding.parameterId);
                        size && *size > 0)
                        return 100.0 / *size;
    }
    return definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey
               ? 0.0
               : document::parameterPresentation(definition_).displayScale;
}

void PropertiesRegistryRow::commit(const std::optional<std::size_t> component) {
    if (refreshing_ || !isEnabled() || displayScale() == 0.0)
        return;
    if (component && (definition_.valueKind == document::ParameterValueKind::Vec2d ||
                      definition_.valueKind == document::ParameterValueKind::Vec3d)) {
        const std::array axes{document::AnimationComponent::X, document::AnimationComponent::Y,
                              document::AnimationComponent::Z};
        if (*component < axes.size() && fields_[*component])
            (void)session_.setParameterComponentValue(parameter_, axes[*component],
                                                      fields_[*component]->value());
        refresh();
        return;
    }
    document::ParameterValue value = definition_.defaultValue;
    if (segments_)
        value = propertiesSelectorItems(definition_.schemaKey)[segments_->currentIndex()].second;
    else if (fontField_) {
        return; // The font field commits itself through bindFontFieldToParameter().
    } else if (selector_ && isSourceInputColorSpaceSchema(definition_.schemaKey)) {
        (void)commitSourceInputColorSpace(
            session_, node_, selector_->itemData(selector_->currentIndex()).toString());
        refresh();
        return;
    } else if (selector_ && (isInputColorSpaceSchema(definition_.schemaKey) ||
                             isShuffleSlotSchema(definition_.schemaKey) ||
                             definition_.schemaKey == "bloom.image.asset" ||
                             definition_.schemaKey == "bloom.video.asset" ||
                             definition_.schemaKey == "bloom.audio.asset"))
        value = selector_->itemData(selector_->currentIndex()).toString().toStdString();
    else if (slider_)
        value = slider_->value();
    else if (selector_)
        value = selector_->itemData(selector_->currentIndex()).value<std::int64_t>();
    else if (integer_)
        value = integer_->integerValue();
    else if (toggle_)
        value = toggle_->isChecked();
    else if (text_)
        value = text_->text().toStdString();
    else if (multiline_)
        value = multiline_->toPlainText().toStdString();
    else if (color_) {
        const auto converted = properties::colorFromFields(
            session_, definition_.schemaKey, {fields_[0], fields_[1], fields_[2], fields_[3]});
        if (!converted)
            return;
        value = *converted;
    } else if (fields_[2])
        value = document::Vec3d{fields_[0]->value(), fields_[1]->value(), fields_[2]->value()};
    else if (fields_[1])
        value = document::Vec2d{fields_[0]->value(), fields_[1]->value()};
    else if (definition_.valueKind == document::ParameterValueKind::Integer)
        value = static_cast<std::int64_t>(fields_[0]->value());
    else
        value = numericFieldValue(*fields_[0]);
    (void)session_.setParameterValue(parameter_, value, tr("Set Parameter"));
    refresh();
}
} // namespace bloom::ui
