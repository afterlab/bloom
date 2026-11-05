#include "node_editor_items.hpp"
#include "value_presentation.hpp"
#include <QCoreApplication>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QShortcut>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/field_nodes.hpp>
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/document/image_channel_nodes.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <memory>

namespace bloom::ui {
kit::Color socketColorToken(const runtime::SocketValueKind kind) noexcept {
    switch (kind) {
    case runtime::SocketValueKind::Image:
        return kit::Color::SocketImage;
    case runtime::SocketValueKind::Audio:
        return kit::Color::SocketAudio;
    case runtime::SocketValueKind::Color:
        return kit::Color::SocketColor;
    case runtime::SocketValueKind::Scalar:
        return kit::Color::SocketScalar;
    case runtime::SocketValueKind::Vector2:
    // Task S7: both vector widths share one token so they read as a family. The socket's NAME and
    // tooltip are what distinguish them, and a cross-width link is refused by the kind check
    // regardless -- two adjacent violets would have said "these connect" when they do not.
    case runtime::SocketValueKind::Vector3:
        return kit::Color::SocketVector;
    case runtime::SocketValueKind::String:
        return kit::Color::SocketString;
    case runtime::SocketValueKind::Integer:
        return kit::Color::SocketInteger;
    case runtime::SocketValueKind::Boolean:
        return kit::Color::SocketBoolean;
    case runtime::SocketValueKind::Mask:
        return kit::Color::SocketImage; // coverage, drawn as a pixel socket; named "Mask"
    case runtime::SocketValueKind::ChannelSet:
        return kit::Color::SocketChannelSet;
    case runtime::SocketValueKind::ScalarField:
        return kit::Color::SocketScalarField;
    case runtime::SocketValueKind::Vector2Field:
        return kit::Color::SocketVectorField;
    case runtime::SocketValueKind::ColorField:
        return kit::Color::SocketColorField;
    }
    return kit::Color::SocketImage;
}

namespace node_editor {
QString nodeCategoryName(const document::NodeCategory category) {
    switch (category) {
    case document::NodeCategory::Sources:
        return QCoreApplication::translate("node_editor", "Sources");
    case document::NodeCategory::Layers:
        return QCoreApplication::translate("node_editor", "Layers");
    case document::NodeCategory::Color:
        return QCoreApplication::translate("node_editor", "Color Management");
    case document::NodeCategory::Compositing:
        return QCoreApplication::translate("node_editor", "Compositing");
    case document::NodeCategory::Values:
        return QCoreApplication::translate("node_editor", "Values");
    case document::NodeCategory::Math:
        return QCoreApplication::translate("node_editor", "Math");
    case document::NodeCategory::Output:
        return QCoreApplication::translate("node_editor", "Output");
    case document::NodeCategory::Utilities:
        return QCoreApplication::translate("node_editor", "Utilities");
    case document::NodeCategory::Transform:
        return QCoreApplication::translate("node_editor", "Transform");
    case document::NodeCategory::Filters:
        return QCoreApplication::translate("node_editor", "Filters");
    case document::NodeCategory::Grade:
        return QCoreApplication::translate("node_editor", "Grade");
    case document::NodeCategory::Fields:
        return QCoreApplication::translate("node_editor", "Fields");
    }
    return {};
}

QString nodeCategoryName(const QString& category) { return category; }
QString nodeCategoryName(const document::NodeDefinition& definition) {
    const auto& type = definition.key.typeId;
    if (type == document::kTimeValueNodeType || type == document::kFrameNumberNodeType ||
        type == document::kFrameRateNodeType || type == document::kSecondsToFramesNodeType ||
        type == document::kFramesToSecondsNodeType ||
        type == document::kSecondsToTimecodeNodeType ||
        type == document::kTimecodeToSecondsNodeType)
        return QStringLiteral("Time");
    // #479: Separate/Combine Color (Value) join the other Color value nodes, apart from the image
    // Separate/Combine Channels under Compositing (#500).
    if (type == document::kSeparateHsvNodeType || type == document::kCombineHsvNodeType ||
        type == document::kHueShiftNodeType || type == document::kLuminanceNodeType ||
        type == document::kColorMixNodeType || type == document::kSeparateRgbaNodeType ||
        type == document::kCombineRgbaNodeType)
        return QStringLiteral("Color Values");
    if (definition.category == document::NodeCategory::Values)
        return QStringLiteral("Values");
    if (type == document::kBooleanLogicNodeType || type == document::kBooleanNotNodeType ||
        type == document::kInRangeNodeType || type == document::kCompareNodeType ||
        type.starts_with("bloom.switch-"))
        return QStringLiteral("Logic");
    if (type == document::kVector2MathNodeType || type == document::kVector3MathNodeType ||
        type == document::kVector2ReduceNodeType || type == document::kVector3ReduceNodeType ||
        type == document::kRotate2dNodeType || type == document::kPolarToCartesianNodeType ||
        type == document::kCartesianToPolarNodeType)
        return QStringLiteral("Vector");
    if (definition.category == document::NodeCategory::Utilities &&
        type.find("-to-") != std::string::npos)
        return QStringLiteral("Convert");
    if (type.starts_with("bloom.string-"))
        return QStringLiteral("String");
    return nodeCategoryName(definition.category);
}
std::span<const QString> nodeCategoryOrder() {
    static const std::array order{QStringLiteral("Sources"),
                                  QStringLiteral("Layers"),
                                  QStringLiteral("Compositing"),
                                  QStringLiteral("Transform"),
                                  QStringLiteral("Filters"),
                                  QStringLiteral("Grade"),
                                  QStringLiteral("Fields"),
                                  QStringLiteral("Color Management"),
                                  QStringLiteral("Values"),
                                  QStringLiteral("Math"),
                                  QStringLiteral("Convert"),
                                  QStringLiteral("String"),
                                  QStringLiteral("Logic"),
                                  QStringLiteral("Time"),
                                  QStringLiteral("Color Values"),
                                  QStringLiteral("Vector"),
                                  QStringLiteral("Utilities"),
                                  QStringLiteral("Output")};
    return order;
}

QString displayTypeName(const std::string_view typeId) {
    if (typeId == "bloom.crop")
        return QCoreApplication::translate("node_editor", "Crop");
    if (typeId == "bloom.pad")
        return QCoreApplication::translate("node_editor", "Pad");
    if (typeId == "bloom.flip")
        return QCoreApplication::translate("node_editor", "Flip");
    if (typeId == "bloom.rotate-90")
        return QCoreApplication::translate("node_editor", "Rotate 90");
    if (typeId == "bloom.gaussian-blur")
        return QCoreApplication::translate("node_editor", "Gaussian Blur");
    if (typeId == "bloom.box-blur")
        return QCoreApplication::translate("node_editor", "Box Blur");
    if (typeId == "bloom.matte-blur")
        return QCoreApplication::translate("node_editor", "Matte Blur");
    if (typeId == "bloom.matte-erode")
        return QCoreApplication::translate("node_editor", "Matte Erode");
    if (typeId == "bloom.matte-dilate")
        return QCoreApplication::translate("node_editor", "Matte Dilate");
    if (typeId == "bloom.luma-key")
        return QCoreApplication::translate("node_editor", "Luma Key");
    if (typeId == "bloom.hue-saturation")
        return QCoreApplication::translate("node_editor", "Hue/Saturation");
    // #493 Curves hue curves.
    if (typeId == "hueVsHue")
        return QCoreApplication::translate("node_editor", "Hue vs Hue");
    if (typeId == "hueVsSaturation")
        return QCoreApplication::translate("node_editor", "Hue vs Saturation");
    if (typeId == "hueVsLuma")
        return QCoreApplication::translate("node_editor", "Hue vs Luma");
    if (typeId == "lumaVsSaturation")
        return QCoreApplication::translate("node_editor", "Luma vs Saturation");
    if (typeId == "bloom.exposure")
        return QCoreApplication::translate("node_editor", "Exposure");
    if (typeId == "bloom.offset")
        return QCoreApplication::translate("node_editor", "Offset");
    if (typeId == "bloom.multiply")
        return QCoreApplication::translate("node_editor", "Multiply");
    if (typeId == "bloom.invert")
        return QCoreApplication::translate("node_editor", "Invert");
    if (typeId == "bloom.image-mix")
        return QCoreApplication::translate("node_editor", "Image Mix");
    if (typeId == "bloom.multiply-alpha")
        return QCoreApplication::translate("node_editor", "Multiply Alpha");
    if (typeId == "bloom.set-alpha")
        return QCoreApplication::translate("node_editor", "Set Alpha");
    if (typeId == "second")
        return QCoreApplication::translate("node_editor", "Second Image");
    if (typeId == "matte")
        return QCoreApplication::translate("node_editor", "Matte");
    if (typeId == "factor")
        return QCoreApplication::translate("node_editor", "Factor");
    if (typeId == "channel")
        return QCoreApplication::translate("node_editor", "Matte Channel");
    if (typeId == "amount")
        return QCoreApplication::translate("node_editor", "Amount");
    if (typeId == "bloom.resize")
        return QCoreApplication::translate("node_editor", "Resize");
    if (typeId == "bloom.fit")
        return QCoreApplication::translate("node_editor", "Fit");
    if (typeId == "bloom.transform-2d")
        return QCoreApplication::translate("node_editor", "Transform 2D");
    if (typeId == "pixelAspectNumerator")
        return QCoreApplication::translate("node_editor", "Pixel Aspect Numerator");
    if (typeId == "pixelAspectDenominator")
        return QCoreApplication::translate("node_editor", "Pixel Aspect Denominator");
    if (typeId == "bloom.ocio-colour-space-transform")
        return QCoreApplication::translate("node_editor", "OCIO Color Space Transform");
    if (typeId == "bloom.ocio-file-transform")
        return QCoreApplication::translate("node_editor", "OCIO File Transform");
    if (typeId == "processSpace")
        return QCoreApplication::translate("node_editor", "Process Space");
    if (typeId == "lut")
        return QCoreApplication::translate("node_editor", "LUT");
    if (typeId == "timeOffset")
        return QCoreApplication::translate("node_editor", "Time Offset");
    if (typeId == "timeScale")
        return QCoreApplication::translate("node_editor", "Time Scale");
    if (typeId == "startFrame")
        return QCoreApplication::translate("node_editor", "Start Frame");
    if (typeId == "loopMode")
        return QCoreApplication::translate("node_editor", "Loop Mode");
    if (typeId == "colorSpace")
        return QCoreApplication::translate("node_editor", "Color Space");
    if (typeId == "cornerRadius")
        return QCoreApplication::translate("node_editor", "Corner Radius");
    if (typeId == "innerRatio")
        return QCoreApplication::translate("node_editor", "Inner Ratio");
    if (typeId == "lineStart")
        return QCoreApplication::translate("node_editor", "Line Start");
    if (typeId == "lineEnd")
        return QCoreApplication::translate("node_editor", "Line End");
    if (typeId == "fillEnabled")
        return QCoreApplication::translate("node_editor", "Fill");
    if (typeId == "fillColor")
        return QCoreApplication::translate("node_editor", "Fill Color");
    if (typeId == "strokeEnabled")
        return QCoreApplication::translate("node_editor", "Stroke");
    if (typeId == "strokeColor")
        return QCoreApplication::translate("node_editor", "Stroke Color");
    if (typeId == "strokeWidth")
        return QCoreApplication::translate("node_editor", "Stroke Width");
    if (typeId == "strokeAlign")
        return QCoreApplication::translate("node_editor", "Stroke Align");
    if (typeId == "strokeJoin")
        return QCoreApplication::translate("node_editor", "Stroke Join");
    if (typeId == "strokeCap")
        return QCoreApplication::translate("node_editor", "Stroke Cap");
    if (typeId == "fillRule")
        return QCoreApplication::translate("node_editor", "Fill Rule");
    QString name = QString::fromUtf8(typeId.data(), static_cast<qsizetype>(typeId.size()));
    if (name.startsWith(QStringLiteral("bloom."))) {
        name.remove(0, 6);
    }
    name.replace('-', ' ');
    // A camelCase role ("verticalAlignment", "inputColorSpaceId") reads as words.
    for (qsizetype index = name.size() - 1; index > 0; --index)
        if (name[index].isUpper() && name[index - 1].isLower())
            name.insert(index, QLatin1Char(' '));
    bool first = true;
    for (auto& character : name) {
        if (first)
            character = character.toUpper();
        first = character.isSpace();
    }
    return name;
}

// Task S1, item 7. displayTypeName() above spells a name out of an identifier, which is the right
// answer for a parameter role and the wrong one for a node the artist reads on a card: "Solid
// source" names the implementation, "Solid" names the thing. Four built-ins are therefore named
// here. Type ids are untouched -- this is vocabulary, not identity.
QString nodeTypeDisplayName(const std::string_view typeId) {
    if (typeId == document::kShapeSourceNodeType)
        return QCoreApplication::translate("node_editor", "Shape");
    if (typeId == document::kCompositionSourceNodeType)
        return QObject::tr("Composition");
    if (typeId == "bloom.video-source")
        return QCoreApplication::translate("node_editor", "Video");
    if (typeId == "bloom.image-source")
        return QCoreApplication::translate("node_editor", "Image");
    // ADR 0026 channel nodes. The Remove node keeps or drops channels by pattern, so it reads as
    // "Select Channels" (a menu item never claims a remove operation it is not).
    if (typeId == document::kChannelShuffleNodeType)
        return QCoreApplication::translate("node_editor", "Shuffle");
    if (typeId == document::kChannelPassNodeType)
        return QCoreApplication::translate("node_editor", "Pass");
    if (typeId == document::kImageToChannelSetNodeType)
        return QCoreApplication::translate("node_editor", "Image to Channel Set");
    if (typeId == document::kChannelCopyNodeType)
        return QCoreApplication::translate("node_editor", "Channel Copy");
    if (typeId == document::kChannelRemoveNodeType)
        return QCoreApplication::translate("node_editor", "Select Channels");
    if (typeId == document::kCryptomatteNodeType)
        return QCoreApplication::translate("node_editor", "Cryptomatte");
    // #383 matte workflow.
    if (typeId == document::kMaskCombineNodeType)
        return QCoreApplication::translate("node_editor", "Mask Combine");
    if (typeId == document::kImageToMaskNodeType)
        return QCoreApplication::translate("node_editor", "Image to Mask");
    if (typeId == "bloom.audio-source")
        return QCoreApplication::translate("node_editor", "Audio");
    if (typeId == document::kSolidSourceNodeType)
        return QCoreApplication::translate("node_editor", "Solid");
    if (typeId == document::kCheckerSourceNodeType)
        return QCoreApplication::translate("node_editor", "Checker");
    if (typeId == document::kLinearGradientNodeType)
        return QCoreApplication::translate("node_editor", "Linear Gradient");
    if (typeId == document::kRadialGradientNodeType)
        return QCoreApplication::translate("node_editor", "Radial Gradient");
    if (typeId == document::kLayerOutputNodeType)
        return QCoreApplication::translate("node_editor", "Layer");
    if (typeId == document::kAdjustmentNodeType)
        return QCoreApplication::translate("node_editor", "Adjustment");
    if (typeId == document::kLayerStackNodeType)
        return QCoreApplication::translate("node_editor", "Merge");
    if (typeId == document::kCompositionOutputNodeType)
        return QCoreApplication::translate("node_editor", "Output");
    // Task S7's library. Spelled out for the same reason the four above are: displayTypeName()
    // reads a name out of an identifier, which gives "Value scalar" and "Separate xy" -- the
    // implementation's spelling rather than the artist's. Type ids are untouched; this is
    // vocabulary, not identity.
    struct LibraryName final {
        std::string_view typeId;
        const char* name;
    };
    static const std::array kLibraryNames{
        LibraryName{document::kIntegerValueNodeType, "Integer"},
        LibraryName{document::kScalarValueNodeType, "Scalar"},
        LibraryName{document::kVector2ValueNodeType, "Vector 2"},
        LibraryName{document::kVector3ValueNodeType, "Vector 3"},
        LibraryName{document::kStringValueNodeType, "String"},
        LibraryName{document::kColorValueNodeType, "Color"},
        LibraryName{document::kBooleanValueNodeType, "Boolean"},
        LibraryName{document::kTimeValueNodeType, "Time"},
        LibraryName{document::kScalarMathNodeType, "Math"},
        LibraryName{document::kVector2MathNodeType, "Vector 2 Math"},
        LibraryName{document::kVector3MathNodeType, "Vector 3 Math"},
        LibraryName{document::kVector2ReduceNodeType, "Vector 2 Measure"},
        LibraryName{document::kVector3ReduceNodeType, "Vector 3 Measure"},
        LibraryName{document::kMapRangeNodeType, "Map Range"},
        LibraryName{document::kClampNodeType, "Clamp"},
        LibraryName{document::kMixNodeType, "Mix"},
        LibraryName{document::kColorMixNodeType, "Mix Color"},
        LibraryName{document::kCompareNodeType, "Compare"},
        LibraryName{document::kScalarSwitchNodeType, "Switch Scalar"},
        LibraryName{document::kIntegerSwitchNodeType, "Switch Integer"},
        LibraryName{document::kBooleanSwitchNodeType, "Switch Boolean"},
        LibraryName{document::kVector2SwitchNodeType, "Switch Vector 2"},
        LibraryName{document::kVector3SwitchNodeType, "Switch Vector 3"},
        LibraryName{document::kColorSwitchNodeType, "Switch Color"},
        LibraryName{document::kStringSwitchNodeType, "Switch String"},
        LibraryName{document::kSeparateXyNodeType, "Separate XY"},
        LibraryName{document::kCombineXyNodeType, "Combine XY"},
        LibraryName{document::kSeparateXyzNodeType, "Separate XYZ"},
        LibraryName{document::kCombineXyzNodeType, "Combine XYZ"},
        // #479: the Color VALUE nodes say so, so the image Separate/Combine Channels nodes are
        // the ones an image search finds first.
        LibraryName{document::kSeparateRgbaNodeType, "Separate Color (Value)"},
        LibraryName{document::kCombineRgbaNodeType, "Combine Color (Value)"},
        LibraryName{document::kRandomNodeType, "Random"},
        LibraryName{document::kLayerBoundsNodeType, "Layer Bounds"},
        LibraryName{document::kRerouteNodeType, "Reroute"},
        // #525 Fields. "Random Field" and "Gradient Field" say so beside the Random value and the
        // gradient generators.
        LibraryName{document::kFieldNoiseNodeType, "Noise"},
        LibraryName{document::kFieldVoronoiNodeType, "Voronoi"},
        LibraryName{document::kFieldRandomNodeType, "Random Field"},
        LibraryName{document::kFieldPatternNodeType, "Pattern"},
        LibraryName{document::kFieldGradientNodeType, "Gradient Field"},
        LibraryName{document::kFieldShapeDistanceNodeType, "Shape Distance"},
        LibraryName{document::kFieldFromImageNodeType, "Image Field"},
        LibraryName{document::kFieldForceNodeType, "Force Field"},
        LibraryName{document::kFieldMathNodeType, "Field Math"},
        LibraryName{document::kFieldRemapNodeType, "Field Remap"},
        LibraryName{document::kFieldTransformNodeType, "Field Transform"},
        LibraryName{document::kFieldDerivativeNodeType, "Field Derivative"},
        LibraryName{document::kFieldCombineNodeType, "Combine Field"},
        LibraryName{document::kFieldSeparateNodeType, "Separate Field"},
        LibraryName{document::kFieldSampleNodeType, "Field Sample"},
        LibraryName{document::kFieldRasterizeNodeType, "Rasterize Field"},
    };
    const auto* const match = std::ranges::find(kLibraryNames, typeId, &LibraryName::typeId);
    if (match != kLibraryNames.end())
        return QCoreApplication::translate("node_editor", match->name);
    // Task UTIL-1's library carries its own name in its descriptor, beside the shape that name
    // belongs to, rather than in a second table here that could fall out of step with the first.
    if (const auto* descriptor = document::findValueUtilityDescriptor(typeId))
        return QString::fromUtf8(descriptor->displayName.data(),
                                 static_cast<qsizetype>(descriptor->displayName.size()));
    return displayTypeName(typeId);
}

QString nodeSearchKeywords(const std::string_view typeId) {
    if (typeId == document::kSeparateChannelsNodeType ||
        typeId == document::kCombineChannelsNodeType)
        return QStringLiteral("RGB RGBA channel split pass");
    // #503: the names compositors search for.
    if (typeId == document::kAlphaAssociationNodeType)
        return QStringLiteral("unmult premultiply unpremultiply premult unpremult straight "
                              "remove color matting matte black white");
    // #504.
    if (typeId == document::kColorizeNodeType)
        return QStringLiteral("fill tint tritone duotone gradient map ramp colorama black white "
                              "monochrome greyscale grayscale toner channel mixer");
    // #505.
    if (typeId == document::kVignetteNodeType)
        return QStringLiteral("vignette lens falloff darken edges corners finishing exposure "
                              "light falloff");
    return {};
}

// A layer boundary node's display name is the layer's own durable name (the exact string the
// timeline row shows); every other node is named by its type. Both are the node's real name, read
// from document truth -- neither is invented here.
QString nodeDisplayName(const document::Composition& composition,
                        const document::NodeRecord& node) {
    for (const auto& boundary : composition.graph().layerOutputs()) {
        if (!boundary.entryOwned() && boundary.nodeId == node.id && !boundary.name.empty()) {
            return QString::fromStdString(boundary.name);
        }
    }
    return nodeTypeDisplayName(node.typeId);
}

// A layer boundary card carries its LAYER's name, so the card alone would no longer say what kind
// of node it is. The eyebrow is what still says it: one small line above the name, nothing else.
QString nodeEyebrow(const document::Composition&, const document::NodeRecord& node) {
    const auto* definition =
        document::builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
    return definition ? nodeCategoryName(*definition) : QString{};
}

const document::ParameterRecord* parameterForRole(const document::NodeRecord& node,
                                                  const document::Composition& composition,
                                                  const std::string_view role) {
    const auto binding = std::ranges::find_if(
        node.parameters, [role](const auto& candidate) { return candidate.role == role; });
    return binding == node.parameters.end() ? nullptr
                                            : composition.parameters().find(binding->parameterId);
}

// The read-only rendering for a bound parameter that has no editable kit primitive behind it (see
// NodeItem::ensureFields()). Unchanged from the pre-U4 projection.
QString parameterText(const document::ParameterRecord& parameter) {
    const auto* constant = std::get_if<document::ConstantValueSource>(&parameter.source);
    if (constant == nullptr) {
        return parameterSourceDescription(parameter);
    }

    return std::visit(
        [](const auto& value) -> QString {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, bool>) {
                return value ? QStringLiteral("On") : QStringLiteral("Off");
            } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                return QString::number(value);
            } else if constexpr (std::is_same_v<Value, double>) {
                return QString::number(value, 'f', 2);
            } else if constexpr (std::is_same_v<Value, document::Vec2d>) {
                return QStringLiteral("%1, %2").arg(value.x, 0, 'f', 1).arg(value.y, 0, 'f', 1);
            } else if constexpr (std::is_same_v<Value, document::Vec3d>) {
                return QStringLiteral("%1, %2, %3")
                    .arg(value.x, 0, 'f', 1)
                    .arg(value.y, 0, 'f', 1)
                    .arg(value.z, 0, 'f', 1);
            } else if constexpr (std::is_same_v<Value, core::Color4d>) {
                return exactColorText(value);
            } else if constexpr (std::is_same_v<Value, std::string>) {
                return QString::fromStdString(value).left(24);
            } else if constexpr (std::is_same_v<Value, document::PathValue>) {
                return QCoreApplication::translate("node_editor", "%1 anchors")
                    .arg(value.anchors.size());
            } else if constexpr (std::is_same_v<Value, document::GradientValue>) {
                return QCoreApplication::translate("node_editor", "%1 stops")
                    .arg(value.stops.size());
            } else {
                return QStringLiteral("%1/%2").arg(value.numerator()).arg(value.denominator());
            }
        },
        constant->value);
}

document::NodeId destinationNodeId(const document::InputPortRef& input) {
    return std::visit(
        [](const auto& destination) {
            using Destination = std::decay_t<decltype(destination)>;
            if constexpr (std::is_same_v<Destination, document::NodeInputRef>) {
                return destination.nodeId;
            } else {
                return destination.stackNodeId;
            }
        },
        input);
}

// A kit numeric cell sized for a node card: no internal label column (KValueField's own is meant
// for a Properties-style row), because the card paints the row's name itself in a column shared by
// every row. Its range, step, decimals, unit and display scale are the parameter's presentation.
kit::KValueField* makeCardField(const QString& objectName, const QString& accessibleName,
                                const std::string_view schemaKey,
                                const document::ParameterValueKind kind,
                                const std::size_t component, const std::string_view role) {
    auto* field = new kit::KValueField;
    field->setObjectName(objectName);
    field->setAccessibleName(accessibleName);
    field->setCompact(true);
    field->setOutlinedAtRest(true);
    configureComponentField(*field, schemaKey, kind, component, role);
    field->resize(field->sizeHint());
    return field;
}

void NodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) {
    const QRectF bounds = cardRect();
    if (reroute_) {
        // A dot in the kind's own colour, ringed like a socket so it reads as part of the wire,
        // with the selection outline the cards use so selecting one is the same gesture and the
        // same ink.
        painter->setRenderHint(QPainter::Antialiasing, true);
        const auto kind =
            sockets_.empty() ? document::SocketValueKind::Image : sockets_.front()->kind;
        painter->setPen(
            QPen(kit::color(option->state.testFlag(QStyle::State_Selected) ? kit::Color::Accent
                                                                           : kit::Color::Surface),
                 kSelectionEdgeWidth));
        painter->setBrush(kit::color(socketColorToken(kind)));
        painter->drawEllipse(bounds.center(), kRerouteDiameter / 2.0, kRerouteDiameter / 2.0);
        return;
    }
    const auto radiusToken = layout_.collapsed ? kit::Radius::Full : kCardRadius;
    const bool selected = option->state.testFlag(QStyle::State_Selected);
    painter->setRenderHint(QPainter::Antialiasing, true);

    // SurfaceRaised card + hairline Border, then the Accent inset edge when selected.
    painter->setOpacity(layout_.muted ? 0.5 : 1.0);

    kit::fillRoundedSurface(*painter, bounds, kit::color(kit::Color::SurfaceRaised),
                            kit::color(kit::Color::Border), radiusToken);
    painter->setOpacity(1.0);
    const auto selectionOutline = [&] {
        if (!selected)
            return;
        painter->setOpacity(1.0);
        const qreal inset = kSelectionEdgeWidth / 2.0;
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(kit::color(kit::Color::Accent), kSelectionEdgeWidth));
        const auto radius = static_cast<qreal>(kit::radiusPx(
            radiusToken, static_cast<int>(std::min(bounds.width(), bounds.height()))));
        painter->drawRoundedRect(bounds.adjusted(inset, inset, -inset, -inset), radius, radius);
    };

    // The header strip: one step down the surface ladder from the card so it reads as chrome, with
    // its own hairline foot rather than a second rounded rectangle. A collapsed card IS its header,
    // so the strip covers all of it.
    const qreal band = headerHeight();
    QPainterPath header;
    // Winding, not QPainterPath's default odd-even rule: the header is a rounded rectangle UNION a
    // square strip that squares off its bottom corners, and the two subpaths overlap. Under
    // odd-even the overlap cancels and the strip is left unpainted -- a dark band across the bottom
    // of every header, which is exactly what the pre-U4 projection drew.
    header.setFillRule(Qt::WindingFill);
    const auto headerRadius = static_cast<qreal>(
        kit::radiusPx(radiusToken, static_cast<int>(std::min(bounds.width(), band))));
    header.addRoundedRect(QRectF(0.0, 0.0, bounds.width(), band), headerRadius,
                          headerRadius);
    if (!layout_.collapsed)
        header.addRect(QRectF(0.0, band - headerRadius, bounds.width(), headerRadius));
    painter->setPen(Qt::NoPen);
    painter->fillPath(header, kit::color(kit::Color::Surface));
    kit::applyHairlinePen(*painter, kit::color(kit::Color::Border));
    if (!layout_.collapsed)
        painter->drawLine(QPointF(0.0, band), QPointF(bounds.width(), band));

    // The collapse toggle (task N1, item 6): a node editor's disclosure caret at the head of the
    // title -- down while the card is open, right while it is folded to its header.
    const auto caret =
        kit::iconPixmap(layout_.collapsed ? kit::IconId::CaretRight : kit::IconId::CaretDown,
                        kit::Size::IconSmall, kit::Color::Muted);
    const QRectF toggle = collapseToggleRect();
    painter->drawPixmap(
        toggle.center() - QPointF(caret.width() / caret.devicePixelRatio() / 2.0,
                                  caret.height() / caret.devicePixelRatio() / 2.0),
        caret);

    const auto [titleRect, categoryRect] = titleBandRects();
    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Muted));
    painter->drawText(categoryRect, Qt::AlignRight | Qt::AlignVCenter, eyebrow_);
    painter->setFont(kit::font(kit::TypeRole::Ui));
    painter->setPen(kit::color(kit::Color::Foreground));
    painter->drawText(
        titleRect, Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetricsF(painter->font()).elidedText(title_, Qt::ElideRight, titleRect.width()));

    // The header's trailing badges, right to left: muted, then a source whose asset cannot be read.
    // A source card carries no preview (task N1, item 2), so the header is where a missing or
    // unreadable asset is said.
    qreal badgeRight = bounds.width() - kCardPadding;
    const auto drawBadge = [&](const QPixmap& badge) {
        const qreal side = badge.width() / badge.devicePixelRatio();
        painter->drawPixmap(
            QPointF(badgeRight - side,
                    (band - badge.height() / badge.devicePixelRatio()) / 2.0),
            badge);
        badgeRight -= side + kCardPadding;
    };
    // #490: drawn before any early return below, so a muted card still shows what reaches it.
    const auto drawPreview = [&] {
        if (imagePreview_ && !previewRect_.isEmpty())
            paintImagePreview(*painter, previewRect_, session_, id_);
    };
    if (layout_.muted)
        drawBadge(
            kit::iconPixmap(kit::IconId::Hidden, kit::Size::IconSmall, kit::Color::Foreground));
    if (assetProblem())
        drawBadge(kit::iconPixmap(kit::IconId::Warning, kit::Size::IconSmall, kit::Color::Warn));
    if (layout_.collapsed) {
        selectionOutline();
        return;
    }
    painter->setOpacity(layout_.muted ? 0.5 : 1.0);
    drawPreview();

    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Muted));
    for (const auto* socket : sockets_) {
        // #499: a socket whose row a card widget keeps behind it is hidden, and so is its name;
        // its position is stale, so drawing the name would put it outside the card.
        if (parameterSocketY_.contains(socket->name) || !socket->isVisible())
            continue;
        const qreal rowExtent = socket->rowHeight();
        const QRectF row(kCardPadding, socket->pos().y() - rowExtent / 2,
                         std::max(0.0, width_ - (kCardPadding + kCardPadding)), rowExtent);
        painter->drawText(
            row,
            static_cast<int>(Qt::AlignVCenter | (socket->input ? Qt::AlignLeft : Qt::AlignRight)),
            painter->fontMetrics().elidedText(
                socket->multiInput() ? QStringLiteral("%1 (%2)")
                                           .arg(displayTypeName(socket->name.toStdString()))
                                           .arg(socket->orderedInputs().size())
                                     : displayTypeName(socket->name.toStdString()),
                Qt::ElideRight, static_cast<int>(row.width())));
    }

    selectionOutline();
}

bool NodeItem::assetProblem() const {
    if (!(imageSource_ || audioSource_) || session_ == nullptr)
        return false;
    const auto* asset = session_->snapshot().project().findAsset(imageAsset_);
    const auto* controller = session_->assetController();
    return asset == nullptr || asset->manifest.hasGaps() ||
           (controller != nullptr &&
            (controller->missing(imageAsset_) || !controller->problem(imageAsset_).isEmpty()));
}

QVariant NodeItem::itemChange(const GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionHasChanged || change == ItemSelectedHasChanged) {
        for (auto* edge : edges_) {
            if (change == ItemPositionHasChanged) {
                edge->updatePath();
            }
            edge->update();
        }
    }
    return QGraphicsObject::itemChange(change, value);
}

NodeItem* nodeItemAncestor(QGraphicsItem* item) {
    for (auto* candidate = item; candidate != nullptr; candidate = candidate->parentItem()) {
        if (auto* node = dynamic_cast<NodeItem*>(candidate)) {
            return node;
        }
    }
    return nullptr;
}

NodeGroupItem* groupItemAncestor(QGraphicsItem* item) {
    for (auto* candidate = item; candidate != nullptr; candidate = candidate->parentItem()) {
        if (auto* group = dynamic_cast<NodeGroupItem*>(candidate)) {
            return group;
        }
    }
    return nullptr;
}

NodeItem* firstNodeItem(const QList<QGraphicsItem*>& items) {
    for (auto* item : items) {
        if (auto* node = nodeItemAncestor(item)) {
            return node;
        }
    }
    return nullptr;
}

} // namespace node_editor
} // namespace bloom::ui

namespace bloom::ui::node_editor {
QString socketKindName(const document::SocketValueKind kind) {
    switch (kind) {
    case document::SocketValueKind::Image:
        return QStringLiteral("Image");
    case document::SocketValueKind::Audio:
        return QStringLiteral("Audio");
    case document::SocketValueKind::Color:
        return QStringLiteral("Color");
    case document::SocketValueKind::Scalar:
        return QStringLiteral("Scalar");
    case document::SocketValueKind::Vector2:
        return QStringLiteral("Vector2");
    case document::SocketValueKind::String:
        return QStringLiteral("String");
    case document::SocketValueKind::Integer:
        return QStringLiteral("Integer");
    case document::SocketValueKind::Boolean:
        return QStringLiteral("Boolean");
    case document::SocketValueKind::Vector3:
        return QStringLiteral("Vector3");
    case document::SocketValueKind::Mask:
        return QStringLiteral("Mask");
    case document::SocketValueKind::ChannelSet:
        return QStringLiteral("Channel Set");
    case document::SocketValueKind::ScalarField:
        return QStringLiteral("Scalar Field");
    case document::SocketValueKind::Vector2Field:
        return QStringLiteral("Vector Field");
    case document::SocketValueKind::ColorField:
        return QStringLiteral("Color Field");
    }
    return {};
}

QPainterPath linkPath(const QPointF start, const QPointF end, const LinkStyle style) {
    switch (style) {
    case LinkStyle::Straight: {
        QPainterPath path(start);
        path.lineTo(end);
        return path;
    }
    case LinkStyle::Angled: {
        // Orthogonal horizontal-vertical-horizontal: out of the source horizontally to the
        // midpoint, straight down (or up) to the destination's row, then horizontally into it.
        QPainterPath path(start);
        const qreal midX = (start.x() + end.x()) / 2.0;
        path.lineTo(midX, start.y());
        path.lineTo(midX, end.y());
        path.lineTo(end);
        return path;
    }
    case LinkStyle::Spline:
        break;
    }
    const qreal handle = std::max(static_cast<qreal>(kit::px(kit::Size::NodeLinkHandleMin)),
                                  std::abs(end.x() - start.x()) / 2.0);
    QPainterPath path(start);
    path.cubicTo(start + QPointF(handle, 0), end - QPointF(handle, 0), end);
    return path;
}

namespace {
// #543: a gradient stop's socket reads as its row does ("Gradient stop 2 color"); every other
// socket by its port name.
[[nodiscard]] QString socketDisplayName(const QString& name) {
    const auto stop = document::parseGradientStopPort(name.toStdString());
    if (!stop)
        return name;
    return stop->component == document::GradientStopComponent::Color
               ? QCoreApplication::translate("node_editor", "%1 stop %2 color")
                     .arg(QString::fromStdString(stop->role))
                     .arg(stop->stop + 1)
               : QCoreApplication::translate("node_editor", "%1 stop %2 position")
                     .arg(QString::fromStdString(stop->role))
                     .arg(stop->stop + 1);
}

// #543: how many stops a gradient parameter has, the most of any key when it is animated.
[[nodiscard]] std::size_t gradientStopCount(const document::Composition& composition,
                                            const document::ParameterId id) {
    const auto* record = composition.parameters().find(id);
    if (record == nullptr)
        return 0;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source))
        if (const auto* gradient = std::get_if<document::GradientValue>(&constant->value))
            return gradient->stops.size();
    std::size_t count = 0;
    if (const auto* animated = std::get_if<document::AnimationCurveSource>(&record->source))
        if (const auto* curve = composition.animationCurves().findGradient(animated->curveId))
            for (const auto& key : curve->keyframes)
                count = std::max(count, key.value.stops.size());
    return count;
}
} // namespace

SocketItem::SocketItem(const document::NodeId node, QString portName,
                       const document::SocketValueKind valueKind,
                       std::optional<document::InputPortRef> inputRef,
                       std::optional<document::OutputPortRef> outputRef, QGraphicsItem* parent)
    : QGraphicsItem(parent), name(std::move(portName)), kind(valueKind), input(std::move(inputRef)),
      output(std::move(outputRef)) {
    ++nodeCanvasWorkCounters().socketsCreated;
    setData(kNodeItemKindRole, QStringLiteral("socket"));
    setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(node.value()));
    setData(kNodeSocketNameRole, name);
    setData(kNodeSocketInputRole, input.has_value());
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setZValue(2);
    setCursor(draggable() ? Qt::CrossCursor : Qt::ForbiddenCursor);
    description_ = socketDisplayName(name) + QStringLiteral(" · ") + socketKindName(kind);
    setAuthoringEnabled(true);
}

qreal SocketItem::pillLength() const {
    if (!multiInput())
        return kSocketDiameter;
    // At least one ordinary row tall, then one pitch per ordered slot: the pill's length is the
    // stack's depth, read straight off the slot model.
    return std::max(kSocketRowHeight, static_cast<qreal>(orderedInputs_.size()) * kStackSlotPitch);
}

qreal SocketItem::rowHeight() const { return std::max(kSocketRowHeight, pillLength()); }

QRectF SocketItem::boundingRect() const {
    const qreal half = pillLength() / 2.0 + kSocketDiameter / 2.0 + kSocketHitSlop;
    return {-16.0, -half, 32.0, half * 2.0};
}

QPainterPath SocketItem::shape() const {
    QPainterPath hit;
    if (!multiInput()) {
        hit.addEllipse(boundingRect());
        return hit;
    }
    // A pill's hit shape is a pill: the same 12px of slop the round socket gets, around a longer
    // body, rather than one ellipse stretched over the whole of it.
    const QRectF bounds = boundingRect();
    hit.addRoundedRect(bounds, bounds.width() / 2.0, bounds.width() / 2.0);
    return hit;
}

void SocketItem::setOrderedInputs(std::vector<document::InputPortRef> inputs) {
    prepareGeometryChange();
    orderedInputs_ = std::move(inputs);
    stackPill_ = true;
    description_ =
        orderedInputs_.empty()
            ? name + QStringLiteral(" · ") + socketKindName(kind) +
                  QCoreApplication::translate(
                      "node_editor", "\nOrdered multi-input: empty; drop an image output here")
            : name + QStringLiteral(" · ") + socketKindName(kind) +
                  QCoreApplication::translate(
                      "node_editor", "\nOrdered multi-input: %1 in stack order, topmost first")
                      .arg(orderedInputs_.size());
    setToolTip(description_);
    update();
}

bool SocketItem::accepts(const document::InputPortRef& ref) const {
    if ((input.has_value() && *input == ref) ||
        std::ranges::find(orderedInputs_, ref) != orderedInputs_.end())
        return true;
    // Merge now carries one stack pill per typed role (content, audio): a slot's content and audio
    // edges land on their OWN pill, matched by role as well as by slot, so an audio edge never
    // draws onto the content pill or vice versa.
    const auto* slot = std::get_if<document::LayerStackInputRef>(&ref);
    return slot != nullptr &&
           std::ranges::any_of(orderedInputs_, [&](const document::InputPortRef& candidate) {
               const auto* ordered = std::get_if<document::LayerStackInputRef>(&candidate);
               return ordered != nullptr && ordered->stackNodeId == slot->stackNodeId &&
                      ordered->slotId == slot->slotId && ordered->role == slot->role;
           });
}

QPointF SocketItem::anchorFor(const document::InputPortRef& ref) const {
    if (!multiInput() || orderedInputs_.empty())
        return {};
    const auto* slot = std::get_if<document::LayerStackInputRef>(&ref);
    const auto found = std::ranges::find_if(orderedInputs_, [&](const auto& candidate) {
        if (candidate == ref)
            return true;
        const auto* ordered = std::get_if<document::LayerStackInputRef>(&candidate);
        return slot != nullptr && ordered != nullptr && ordered->stackNodeId == slot->stackNodeId &&
               ordered->slotId == slot->slotId && ordered->role == slot->role;
    });
    if (found == orderedInputs_.end())
        return {};
    const qreal length = pillLength();
    const qreal pitch = length / static_cast<qreal>(orderedInputs_.size());
    const auto index = static_cast<qreal>(std::distance(orderedInputs_.begin(), found));
    return {0.0, -length / 2.0 + pitch * (index + 0.5)};
}

void SocketItem::setDropIndicator(const std::optional<std::size_t> slotIndex) {
    if (dropIndicator_ == slotIndex)
        return;
    dropIndicator_ = slotIndex;
    update();
}

std::optional<document::LayerSlotId> SocketItem::slotInsertionAt(const QPointF localPoint) const {
    const auto index = slotIndexAt(localPoint);
    if (!index.has_value())
        return std::nullopt;
    // The caret sits ON a slot; a pointer in that slot's upper half means "above it", and in its
    // lower half "below it" -- which for the last slot is an append.
    const qreal pitch = pillLength() / static_cast<qreal>(orderedInputs_.size());
    const qreal top = -pillLength() / 2.0 + pitch * static_cast<qreal>(*index);
    const bool below = localPoint.y() > top + pitch / 2.0;
    const auto target = below ? *index + 1 : *index;
    if (target >= orderedInputs_.size())
        return std::nullopt;
    const auto* slot = std::get_if<document::LayerStackInputRef>(&orderedInputs_[target]);
    return slot == nullptr ? std::nullopt : std::optional(slot->slotId);
}

std::optional<std::size_t> SocketItem::slotIndexAt(const QPointF localPoint) const {
    if (!multiInput() || orderedInputs_.empty() || !shape().contains(localPoint))
        return std::nullopt;
    const qreal length = pillLength();
    const qreal pitch = length / static_cast<qreal>(orderedInputs_.size());
    const auto index = static_cast<std::ptrdiff_t>(
        std::floor((localPoint.y() + length / 2.0) / std::max(pitch, 0.001)));
    return static_cast<std::size_t>(std::clamp(
        index, std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(orderedInputs_.size()) - 1));
}

QColor SocketItem::paintedInk() const {
    const QColor base = kit::color(socketColorToken(kind));
    switch (affinity_) {
    case DragAffinity::Compatible:
        return kit::hoverFillFor(base);
    case DragAffinity::Incompatible:
        return kit::withOpacity(base, kit::kDisabledOpacity);
    case DragAffinity::Idle:
        break;
    }
    return base;
}

void SocketItem::setDragAffinity(const DragAffinity affinity, const QString& refusal) {
    const QString tip = affinity == DragAffinity::Incompatible && !refusal.isEmpty()
                            ? description_ + QStringLiteral("\n") + refusal
                            : description_;
    if (affinity_ == affinity && toolTip() == tip)
        return;
    affinity_ = affinity;
    setToolTip(tip);
    update();
}

void SocketItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(kit::color(kit::Color::Surface), kit::kHairlineWidth));
    painter->setBrush(paintedInk());
    const qreal radius = hovered_ ? 6.0 : kSocketDiameter / 2;
    // #525: a field socket is a diamond, so a field never reads as the value or image beside it.
    if (document::isFieldSocketKind(kind)) {
        const qreal r = radius * 1.25;
        painter->drawPolygon(QPolygonF({QPointF(0.0, -r), QPointF(r, 0.0), QPointF(0.0, r),
                                        QPointF(-r, 0.0)}));
        return;
    }
    if (!multiInput()) {
        painter->drawEllipse(QPointF(), radius, radius);
        return;
    }
    // The Merge node's one ordered multi-input: a vertical pill, divided into one segment per stack
    // slot so the port itself shows how many layers it carries and in what order. The segment
    // divisions are drawn in the card's own Surface ink, the same hairline that rings every socket.
    const qreal length = pillLength();
    const QRectF pill(-radius, -length / 2.0, radius * 2.0, length);
    painter->drawRoundedRect(pill, radius, radius);
    if (orderedInputs_.empty())
        return;
    // Not named `slots`: Qt's moc keyword macro takes that identifier.
    const auto slotCount = static_cast<qreal>(orderedInputs_.size());
    kit::applyHairlinePen(*painter, kit::color(kit::Color::Surface));
    for (std::size_t division = 1; division < orderedInputs_.size(); ++division) {
        const qreal y = pill.top() + length * static_cast<qreal>(division) / slotCount;
        painter->drawLine(QPointF(pill.left(), y), QPointF(pill.right(), y));
    }
    if (dropIndicator_.has_value()) {
        // The position the pointer is at in the order. Muted, not Accent: a stack slot is
        // structural, so this marks where the pointer IS and never promises that releasing there
        // lands a link -- the pill is dimmed as incompatible at the same moment.
        const qreal pitch = length / slotCount;
        const qreal y = pill.top() + pitch * (static_cast<qreal>(*dropIndicator_) + 0.5);
        painter->setPen(
            QPen(kit::color(kit::Color::Muted), kit::kEmphasisStroke, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(QPointF(pill.left() - radius, y), QPointF(pill.right() + radius, y));
    }
}
void SocketItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = true;
    setData(kNodeHoveredRole, true);
    update();
    QGraphicsItem::hoverEnterEvent(event);
}
void SocketItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = false;
    setData(kNodeHoveredRole, false);
    update();
    QGraphicsItem::hoverLeaveEvent(event);
}

namespace {
// One socket a card's definition asks for, before it is matched against the sockets the card
// already carries.
struct SocketSpec final {
    QString name;
    document::SocketValueKind kind = document::SocketValueKind::Image;
    std::optional<document::InputPortRef> input;
    std::optional<document::OutputPortRef> output;
    bool pill = false;
    std::vector<document::InputPortRef> ordered;
    bool anyKind = false;
    bool linked = false;
    bool pixelKinds = false;
    bool fieldKinds = false;
    bool imageMatte = false;
};
// The fields a SocketItem fixes at construction. Everything else a spec carries is updated on the
// kept item in place.
bool sameSocket(const SocketItem& socket, const SocketSpec& spec) {
    return socket.multiInput() == spec.pill && socket.kind == spec.kind && socket.name == spec.name &&
           socket.input == spec.input && socket.output == spec.output;
}
} // namespace

void NodeItem::buildSockets(const document::NodeRecord& node,
                            const document::Composition& composition,
                            const NodeProjectionIndex& links,
                            const document::NodeDefinitionRegistry& registry) {
    linkedInputs_.clear();
    std::vector<SocketSpec> specs;
    if (const auto* definition = registry.find(node.typeId, node.schemaVersion)) {
        // A reroute's sockets take the kind of the link it sits on (task FIX1, item I); the
        // definition's declared Image is only the placeholder a definition must name. An
        // unconnected reroute keeps the placeholder for painting and accepts anything, which is
        // what its own kind-less state means.
        const auto rerouteKind = document::isRerouteNodeType(node.typeId)
                                     ? composition.graph().rerouteKind(node.id, registry)
                                     : std::nullopt;
        const bool kindless = document::isRerouteNodeType(node.typeId) && !rerouteKind.has_value();
        for (const auto& port : definition->inputs) {
            document::InputPortRef input = document::NodeInputRef{node.id, port.name};
            // Two kinds of port, one question each. An OPERAND socket is linked when its parameter
            // carries a driver binding; an image transport port is linked when an edge terminates
            // on it. That split is not an inconsistency -- each has exactly one durable record of
            // where its value comes from, which is why an edge and a binding can never disagree
            // about a socket.
            const auto binding =
                std::ranges::find(node.parameters, port.name, &document::ParameterBinding::role);
            const bool linked =
                binding != node.parameters.end()
                    ? [&] {
                          const auto* parameter =
                              composition.parameters().find(binding->parameterId);
                          return parameter != nullptr &&
                                 std::holds_alternative<document::DriverBindingSource>(
                                     parameter->source);
                      }()
                    : links.edgeInto(input);
            if (linked)
                linkedInputs_.insert(QString::fromStdString(port.name));
            specs.push_back({QString::fromStdString(port.name),
                             rerouteKind.value_or(port.valueKind),
                             std::move(input),
                             std::nullopt,
                             false,
                             {},
                             kindless,
                             linked,
                             port.acceptsPixelKinds,
                             port.acceptsFieldKinds,
                             port.acceptsImageMatte});
        }
        // #543: each stop of a bound gradient has a colour and a position socket, which the card
        // puts beside the stop's rows; a linked one always shows.
        for (const auto& parameter : definition->parameters) {
            if (parameter.valueKind != document::ParameterValueKind::Gradient)
                continue;
            const auto gradient = std::ranges::find(node.parameters, parameter.role,
                                                    &document::ParameterBinding::role);
            if (gradient == node.parameters.end())
                continue;
            auto count = gradientStopCount(composition, gradient->parameterId);
            for (const auto& binding : node.parameters)
                if (const auto stop = document::parseGradientStopPort(binding.role);
                    stop && stop->role == parameter.role)
                    count = std::max<std::size_t>(count, stop->stop + 1U);
            for (std::uint32_t stop = 0; stop < count && stop < document::kMaximumGradientStops;
                 ++stop)
                for (const auto component : {document::GradientStopComponent::Color,
                                             document::GradientStopComponent::Position}) {
                    const auto port =
                        document::gradientStopPortName(parameter.role, stop, component);
                    const auto binding =
                        std::ranges::find(node.parameters, port, &document::ParameterBinding::role);
                    const auto* link = binding == node.parameters.end()
                                           ? nullptr
                                           : composition.parameters().find(binding->parameterId);
                    const bool linked =
                        link != nullptr &&
                        std::holds_alternative<document::DriverBindingSource>(link->source);
                    if (linked)
                        linkedInputs_.insert(QString::fromStdString(port));
                    specs.push_back({QString::fromStdString(port),
                                     component == document::GradientStopComponent::Color
                                         ? document::SocketValueKind::Color
                                         : document::SocketValueKind::Scalar,
                                     document::NodeInputRef{node.id, port},
                                     std::nullopt,
                                     false,
                                     {},
                                     false,
                                     linked,
                                     false});
                }
        }
        const auto* merge = composition.graph().merge(node.id);
        if (definition->layerSlotInput && merge) {
            // Task S1, item 7: ONE ordered multi-input for the whole stack, not one repeated row
            // per slot. The slot model underneath is exactly as it was -- these are its own slots,
            // in its own order -- and every edge that terminates on any of them terminates on this
            // one socket.
            //
            // Task FIX1, item B: the pill exists even when the stack is EMPTY, and its own `input`
            // is the invalid-slot sentinel that ConnectPorts reads as "make a new slot here".
            // Without it a fresh composition's Merge node had no port at all and the artist had
            // nothing to wire the first layer into.
            const auto& port = *definition->layerSlotInput;
            SocketSpec pill{QString::fromStdString(port.role), port.valueKind,
                            document::LayerStackInputRef{node.id, document::LayerSlotId{}, port.role},
                            std::nullopt, true, {}, false, false};
            for (const auto& slot : merge->entries())
                pill.ordered.push_back(document::LayerStackInputRef{node.id, slot.slotId, port.role});
            pill.linked = !pill.ordered.empty();
            specs.push_back(std::move(pill));
        }
        if (definition->audioLayerSlotInput && merge) {
            // Task FOLLOW-1: a second stack pill, below the content pill, for the audio transport
            // the same stable slots carry (docs/architecture/layer-graph-model.md, Audio Sources
            // And Layers). Unlike the content pill -- whose ordered inputs are every stack entry,
            // because a visible slot always has a content edge -- audio is optional per slot, so
            // this pill's ordered inputs are only the slots that actually carry an audio edge.
            const auto& port = *definition->audioLayerSlotInput;
            SocketSpec pill{QString::fromStdString(port.role), port.valueKind,
                            document::LayerStackInputRef{node.id, document::LayerSlotId{}, port.role},
                            std::nullopt, true, {}, false, false};
            for (const auto& slot : merge->entries()) {
                const document::InputPortRef ref =
                    document::LayerStackInputRef{node.id, slot.slotId, port.role};
                if (links.edgeInto(ref))
                    pill.ordered.push_back(ref);
            }
            pill.linked = !pill.ordered.empty();
            specs.push_back(std::move(pill));
        }
        // An output is linked when an edge or a driver binding reads from it.
        for (const auto& port : definition->outputs)
            specs.push_back({QString::fromStdString(port.name), port.valueKind, std::nullopt,
                             document::OutputPortRef{node.id, port.name}, false, {}, false,
                             links.outputLinked(node.id, port.name)});
    }

    auto previous = std::move(sockets_);
    sockets_.clear();
    sockets_.reserve(specs.size());
    for (auto& spec : specs) {
        SocketItem* socket = nullptr;
        if (const auto kept = std::ranges::find_if(
                previous, [&spec](const auto* old) { return old && sameSocket(*old, spec); });
            kept != previous.end()) {
            socket = *kept;
            *kept = nullptr;
        } else {
            socket = new SocketItem(node.id, spec.name, spec.kind, spec.input, spec.output, this);
        }
        socket->setAcceptsAnyKind(spec.anyKind);
        socket->setAcceptsPixelKinds(spec.pixelKinds);
        socket->setAcceptsFieldKinds(spec.fieldKinds);
        socket->setAcceptsImageMatte(spec.imageMatte);
        socket->linked = spec.linked;
        if (spec.pill && (!socket->multiInput() || socket->orderedInputs() != spec.ordered))
            socket->setOrderedInputs(std::move(spec.ordered));
        sockets_.push_back(socket);
    }
    for (auto* socket : previous)
        delete socket;
}

NodeEdgeItem::NodeEdgeItem(NodeItem& source, NodeItem& destination, SocketItem& output,
                           SocketItem& input, document::EdgeRecord record, const bool isStructural,
                           const LinkStyle style)
    : edge(std::move(record)), structural(isStructural), source_(source), destination_(destination),
      output_(output), input_(input), style_(style) {
    setData(kNodeItemKindRole, QStringLiteral("edge"));
    setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(edge.id.value()));
    setData(kNodeStructuralRole, structural);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::NoButton);
    setZValue(-1);
    reattach();
}
void NodeEdgeItem::reattach() {
    // Every link names both of its ends (task FIX1, item C), so hovering a wire in a dense graph
    // says what it connects instead of leaving the artist to trace it.
    const QString tip = QCoreApplication::translate("node_editor", "%1 · %2  →  %3 · %4")
                            .arg(source_.title(), output_.name, destination_.title(),
                                 socketDisplayName(input_.name));
    if (toolTip() != tip)
        setToolTip(tip);
    source_.addEdge(*this);
    destination_.addEdge(*this);
    updatePath();
}
void NodeEdgeItem::updatePath() {
    setPath(linkPath(output_.scenePos(), input_.mapToScene(input_.anchorFor(edge.destination)),
                     style_));
}
QPainterPath NodeEdgeItem::shape() const {
    QPainterPathStroker stroke;
    stroke.setWidth(kit::px(kit::Size::NodeLinkHit));
    return stroke.createStroke(path());
}
QPen NodeEdgeItem::linkPen() const {
    const bool emphasized = hovered_ || source_.isSelected() || destination_.isSelected();
    const QColor base = kit::color(socketColorToken(output_.kind));
    QPen pen(emphasized ? kit::emphasizedStrokeFor(base) : base, kit::kNodeLinkWidth,
             Qt::SolidLine, Qt::RoundCap);
    // Cosmetic: the width is in screen pixels, so a link reads as the same line at 25% as at 400%
    // instead of thinning to nothing when the artist zooms out.
    pen.setCosmetic(true);
    return pen;
}
QRectF NodeEdgeItem::boundingRect() const {
    // A cosmetic stroke is a fixed number of SCREEN pixels, which is this many scene units at the
    // smallest zoom the canvas allows; the rectangle must cover it there or the stroke leaves trails.
    const qreal overhang = kit::kNodeLinkWidth / ViewTransform::kMinZoom;
    return shape().boundingRect().adjusted(-overhang, -overhang, overhang, overhang);
}
void NodeEdgeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(linkPen());
    painter->drawPath(path());
}
void NodeEdgeItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event) {
    emphasize(true);
    setData(kNodeHoveredRole, true);
    QGraphicsPathItem::hoverEnterEvent(event);
}
void NodeEdgeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    emphasize(false);
    setData(kNodeHoveredRole, false);
    QGraphicsPathItem::hoverLeaveEvent(event);
}
} // namespace bloom::ui::node_editor

namespace bloom::ui::node_editor {
void NodeItem::setPreviewWidth(const qreal width) {
    layout_.width = width;
    relayout();
    for (auto* edge : edges_)
        edge->updatePath();
}
} // namespace bloom::ui::node_editor

namespace bloom::ui::node_editor {
// Retires a rename field for good: detached from the card and taken out of the scene NOW, then
// deferred-deleted. Hiding it and waiting for the deferred delete left a stale, invisible editor
// among the card's children, and fieldWidget() answers with the first child that matches a name --
// so the next rename's own field could not be found at all.
void NodeItem::retireRenameProxy() {
    auto* retired = renameProxy_;
    if (retired == nullptr)
        return;
    renameProxy_ = nullptr;
    retired->hide();
    retired->setParentItem(nullptr);
    if (scene() != nullptr)
        scene()->removeItem(retired);
    retired->deleteLater();
}

void NodeItem::startRename() {
    auto* graphScene = qobject_cast<NodeGraphicsScene*>(scene());
    if (!session_ || !session_->composition() || !graphScene || !graphScene->canSubmit())
        return;
    std::optional<document::LayerId> layer;
    for (const auto& boundary : session_->composition()->graph().layerOutputs())
        if (!boundary.entryOwned() && boundary.nodeId == id_)
            layer = boundary.layerId;
    if (!layer)
        return;
    if (renameProxy_) {
        renameProxy_->show();
        renameProxy_->widget()->setFocus();
        return;
    }
    auto* field = new kit::KLineEdit(title_);
    field->setObjectName(QStringLiteral("nodeRenameEditor"));
    field->setAccessibleName(tr("Layer name"));
    field->setFont(kit::font(kit::TypeRole::UiSmall));
    field->resize(static_cast<int>(std::ceil(width_ - (kCardPadding + kCardPadding))),
                  static_cast<int>(kCardHeaderHeight));
    hostTranslucent(*field);
    renameProxy_ = new QGraphicsProxyWidget(this);
    renameProxy_->setWidget(field);
    renameProxy_->setPos(kCardPadding, 0);
    renameProxy_->setZValue(5);
    const auto revision = session_->snapshot().revision();
    const auto composition = session_->compositionId();
    // The kit inline-edit policy (Escape confirms everywhere): every ending commits a changed
    // name once, and an empty name is refused rather than reverted in silence.
    auto& policy = field->useInlineEdit(kit::KInlineEdit::nameValidator());
    connect(&policy, &kit::KInlineEdit::accepted, this,
            [this, graphScene, layer = *layer, revision, composition,
             original = title_](const QString& text) {
                if (!renameProxy_ || !renameProxy_->isVisible())
                    return;
                retireRenameProxy();
                if (text == original)
                    return;
                commands::Transaction transaction("Rename Layer", revision);
                transaction.emplace<commands::RenameLayer>(composition, layer,
                                                           text.toStdString());
                (void)graphScene->submit(std::move(transaction));
            });
    connect(&policy, &kit::KInlineEdit::refused, this, [this] { retireRenameProxy(); });
    field->setFocus(Qt::OtherFocusReason);
    field->selectAll();
}
} // namespace bloom::ui::node_editor

namespace bloom::ui::node_editor {
void SocketItem::setAuthoringEnabled(const bool enabled) {
    setCursor(enabled && draggable() ? Qt::CrossCursor : Qt::ForbiddenCursor);
    setToolTip(enabled ? description_
                       : description_ + QStringLiteral("\nNode command submission is unavailable"));
}
} // namespace bloom::ui::node_editor

namespace bloom::ui::node_editor {
NodeGroupItem::NodeGroupItem(const document::NodeGroupId id, CompositionSession* session)
    : id_(id), session_(session) {
    setData(kNodeItemKindRole, QStringLiteral("node-group"));
    setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(id.value()));
    setAcceptHoverEvents(true);
    // Behind its members AND behind their links: a frame is the ground they sit on, not a pane over
    // them. Cards rest at 0 and links at -1.
    setZValue(-2);
}

void NodeGroupItem::refresh(const document::NodeGroupRecord& record) {
    title_ = QString::fromStdString(record.name);
    members_ = record.members;
    padding_ = record.padding;
    color_ = record.color ? QColor((*record.color)[0], (*record.color)[1], (*record.color)[2])
                          : QColor();
    setToolTip(QStringLiteral("%1\nNode group %2").arg(title_).arg(id_.value()));
    update();
}

void NodeGroupItem::setFrameRect(const QRectF rect) {
    setPos(rect.topLeft());
    if (size_ == rect.size())
        return;
    prepareGeometryChange();
    size_ = rect.size();
    if (renameProxy_ != nullptr && renameProxy_->widget() != nullptr)
        renameProxy_->widget()->resize(static_cast<int>(std::ceil(std::max(
                                           0.0, size_.width() - (kCardPadding + kCardPadding)))),
                                       static_cast<int>(kGroupTitleHeight));
    update();
}

void NodeGroupItem::setAuthoringEnabled(const bool enabled) {
    authoringEnabled_ = enabled;
    setCursor(enabled ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void NodeGroupItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    const QRectF bounds = boundingRect();
    if (bounds.isEmpty())
        return;
    painter->setRenderHint(QPainter::Antialiasing, true);
    // A coloured frame (task N1, item 5) takes its own tint in place of the raised surface, at the
    // same faint opacity, so it still reads as ground behind the cards it holds.
    painter->setOpacity(kGroupFillOpacity);
    kit::fillRoundedSurface(*painter, bounds,
                            color_.isValid() ? color_ : kit::color(kit::Color::SurfaceRaised),
                            QColor(), kGroupRadius);
    painter->setOpacity(1.0);
    // The border keeps full opacity: the fill is what recedes, while the hairline is what says
    // where the frame actually ends -- which is the line a drop is judged against.
    const auto radius = static_cast<qreal>(
        kit::radiusPx(kGroupRadius, static_cast<int>(std::min(bounds.width(), bounds.height()))));
    painter->setBrush(Qt::NoBrush);
    kit::applyHairlinePen(*painter, kit::color(kit::Color::Border));
    painter->drawRoundedRect(bounds, radius, radius);
    if (renameProxy_ != nullptr && renameProxy_->isVisible())
        return;
    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Muted));
    painter->drawText(
        titleRect().adjusted(kCardPadding, 0.0, -kCardPadding, 0.0),
        static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft),
        QFontMetricsF(painter->font())
            .elidedText(title_, Qt::ElideRight,
                        std::max(0.0, bounds.width() - (kCardPadding + kCardPadding))));
}

void NodeGroupItem::retireRenameProxy() {
    auto* retired = renameProxy_;
    if (retired == nullptr)
        return;
    renameProxy_ = nullptr;
    retired->hide();
    retired->setParentItem(nullptr);
    if (scene() != nullptr)
        scene()->removeItem(retired);
    retired->deleteLater();
    update();
}

// The same inline-editor shape a layer card's rename uses, over the frame's own title strip: commit
// through the kit inline-edit policy (Return, Escape, Tab and focus loss all commit), and one
// RenameGroup transaction for a changed name.
void NodeGroupItem::startRename() {
    auto* graphScene = qobject_cast<NodeGraphicsScene*>(scene());
    if (session_ == nullptr || session_->composition() == nullptr || graphScene == nullptr ||
        !graphScene->canSubmit())
        return;
    if (renameProxy_ != nullptr) {
        renameProxy_->show();
        renameProxy_->widget()->setFocus();
        return;
    }
    auto* field = new kit::KLineEdit(title_);
    field->setObjectName(QStringLiteral("nodeGroupRenameEditor"));
    field->setAccessibleName(tr("Node group name"));
    field->setFont(kit::font(kit::TypeRole::UiSmall));
    field->resize(
        static_cast<int>(std::ceil(std::max(0.0, size_.width() - (kCardPadding + kCardPadding)))),
        static_cast<int>(kGroupTitleHeight));
    field->setAttribute(Qt::WA_TranslucentBackground, true);
    field->setAttribute(Qt::WA_NoSystemBackground, true);
    field->setAutoFillBackground(false);
    renameProxy_ = new QGraphicsProxyWidget(this);
    renameProxy_->setWidget(field);
    renameProxy_->setPos(kCardPadding, 0);
    renameProxy_->setZValue(5);
    const auto revision = session_->snapshot().revision();
    const auto composition = session_->compositionId();
    auto& policy = field->useInlineEdit(kit::KInlineEdit::nameValidator());
    connect(&policy, &kit::KInlineEdit::accepted, this,
            [this, graphScene, revision, composition, original = title_](const QString& text) {
                if (renameProxy_ == nullptr || !renameProxy_->isVisible())
                    return;
                retireRenameProxy();
                if (text == original)
                    return;
                commands::Transaction transaction("Rename Node Group", revision);
                transaction.emplace<commands::RenameGroup>(composition, id_, text.toStdString());
                (void)graphScene->submit(std::move(transaction));
            });
    connect(&policy, &kit::KInlineEdit::refused, this, [this] { retireRenameProxy(); });
    field->setFocus(Qt::OtherFocusReason);
    field->selectAll();
    update();
}
} // namespace bloom::ui::node_editor
