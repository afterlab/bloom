#include "row_inline_controls.hpp"

#include "properties_value_edits.hpp"

#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/document/image_math_nodes.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/angle_dial.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QCoreApplication>
#include <QRandomGenerator>
#include <QSignalBlocker>

#include <string>

namespace bloom::ui {

RowInline rowInlineFor(const std::string_view schemaKey) {
    using document::kCameraShakeNodeType;
    for (const auto role : {"rollAmount", "shutterAngle", "shutterPhase"})
        if (schemaKey == document::motionSchemaKey(kCameraShakeNodeType, role))
            return RowInline::AngleDial;
    // #509 Path Stroke's stamp rotation.
    if (schemaKey == document::kStampRotationParameterSchemaKey)
        return RowInline::AngleDial;
    // #349 Image Compare's split line turns about the middle of the image like a dial.
    if (schemaKey == document::imagePixelSchemaKey(document::kImageCompareNodeType, "angle"))
        return RowInline::AngleDial;
    if (schemaKey == document::motionSchemaKey(kCameraShakeNodeType, "seed") ||
        schemaKey == document::motionSchemaKey(document::kFrameRateHoldNodeType, "seed") ||
        // #527 Dither's pattern placement.
        schemaKey == document::colorCleanupSchemaKey(document::kDitherNodeType, "seed"))
        return RowInline::RandomiseSeed;
    return RowInline::None;
}

kit::KAngleDial* makeRowAngleDial(CompositionSession& session,
                                  std::function<document::ParameterId()> parameter,
                                  const QString& name, const double neutral, QWidget* parent) {
    auto* dial = new kit::KAngleDial(parent);
    dial->setObjectName(QStringLiteral("rowAngleDial"));
    dial->setAccessibleName(name);
    dial->setDefaultValue(neutral);
    dial->setResetText(QCoreApplication::translate("RowInlineControls", "Reset %1").arg(name));
    dial->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
    properties::bindValueEdit(session, *dial, parameter);
    QObject::connect(dial, &kit::KAngleDial::valueChanged, dial,
                     [&session, parameter, name](const double degrees) {
                         static_cast<void>(session.setParameterValue(
                             parameter(), degrees,
                             QCoreApplication::translate("RowInlineControls", "Set %1")
                                 .arg(name)));
                     });
    return dial;
}

void refreshRowAngleDial(CompositionSession& session, kit::KAngleDial& dial,
                         const std::optional<document::ParameterId> parameter,
                         const bool locked) {
    dial.setEnabled(parameter && !locked && scalarEditable(session, *parameter));
    // The value at the playhead (a key, a live edit or the constant), as the row's field shows it.
    const auto value = parameter ? session.effectiveScalarValue(*parameter) : std::nullopt;
    if (value && !dial.hasLiveEdit()) {
        const QSignalBlocker blocker(&dial);
        dial.setValue(*value);
    }
}

kit::KButton*
makeRandomiseSeedButton(CompositionSession& session,
                        std::function<std::optional<document::ParameterId>()> parameter,
                        QWidget* parent) {
    auto* button = new kit::KButton(
        QCoreApplication::translate("RowInlineControls", "Randomize seed"), parent);
    button->setObjectName(QStringLiteral("randomiseSeed"));
    button->setVariant(kit::KButton::Variant::Secondary);
    button->setToolTip(
        QCoreApplication::translate("RowInlineControls", "Give this node a new random seed"));
    button->setFixedWidth(button->sizeHint().width());
    QObject::connect(button, &kit::KButton::clicked, button, [&session, parameter] {
        if (const auto id = parameter())
            static_cast<void>(session.setParameterValue(
                *id, static_cast<std::int64_t>(QRandomGenerator::global()->bounded(1000000U)),
                QCoreApplication::translate("RowInlineControls", "Randomize Seed")));
    });
    return button;
}

} // namespace bloom::ui
