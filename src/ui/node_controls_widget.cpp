#include <bloom/ui/node_controls_widget.hpp>
#include <bloom/document/resample_path_nodes.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/shape_morph_nodes.hpp>
#include <bloom/document/warp_path_nodes.hpp>
#include <bloom/ui/motion_blur_widget.hpp>

#include "properties_registry_row.hpp"
#include "properties_value_edits.hpp"

#include <bloom/document/four_color_gradient_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/denoise_nodes.hpp>
#include <bloom/document/parameter_activity.hpp>
#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/field_nodes.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/gradient_parameter_row.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/angle_dial.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/range_selector.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QCoreApplication>
#include <QFontMetrics>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <variant>

namespace bloom::ui {
namespace {

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] int dialSide() { return kit::px(kit::Size::Control) + kit::px(kit::Size::ToggleCell); }
// #525: a curve graph keeps its own aspect at the card's width.
[[nodiscard]] int curveHeight(const QWidget* canvas, const int width) {
    const auto hint = canvas->sizeHint();
    return hint.width() > 0 ? width * hint.height() / hint.width() : hint.height();
}

[[nodiscard]] const document::NodeDefinition* definitionOf(const CompositionSession& session,
                                                           const document::NodeId node) {
    const auto* composition = session.composition();
    const auto* record = composition ? composition->graph().findNode(node) : nullptr;
    return record ? document::builtInNodeDefinitions().find(record->typeId, record->schemaVersion)
                  : nullptr;
}

// The node's own default for `role`, the value a reset returns to.
[[nodiscard]] double defaultScalar(const CompositionSession& session, const document::NodeId node,
                                   const std::string_view role) {
    const auto* definition = definitionOf(session, node);
    if (definition == nullptr)
        return 0.0;
    const auto found =
        std::ranges::find(definition->parameters, role, &document::ParameterDefinition::role);
    const auto* scalar =
        found == definition->parameters.end() ? nullptr : std::get_if<double>(&found->defaultValue);
    return scalar ? *scalar : 0.0;
}

[[nodiscard]] std::optional<document::Vec2d> defaultVector(const CompositionSession& session,
                                                           const document::NodeId node,
                                                           const std::string_view role) {
    const auto* definition = definitionOf(session, node);
    if (definition == nullptr)
        return std::nullopt;
    const auto found =
        std::ranges::find(definition->parameters, role, &document::ParameterDefinition::role);
    const auto* vector = found == definition->parameters.end()
                             ? nullptr
                             : std::get_if<document::Vec2d>(&found->defaultValue);
    return vector ? std::optional<document::Vec2d>(*vector) : std::nullopt;
}

[[nodiscard]] std::optional<document::Vec2d> liveVector(const CompositionSession& session,
                                                        const document::ParameterId id) {
    const auto value = session.liveValue(id);
    const auto* vector = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
    return vector ? std::optional<document::Vec2d>(*vector) : std::nullopt;
}

[[nodiscard]] QRectF compositionFrame(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {0.0, 0.0, 1920.0, 1080.0};
    const auto format = composition->format();
    return {0.0, 0.0, static_cast<double>(format.width()), static_cast<double>(format.height())};
}

constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi;

} // namespace

// --- AngleDialRow -------------------------------------------------------------------------------

AngleDialRow::AngleDialRow(CompositionSession& session, const document::NodeId node, Spec spec,
                           QWidget* parent)
    : QWidget(parent), session_(session), node_(node), spec_(std::move(spec)) {
    const auto objectRole = QString::fromStdString(spec_.role);
    setObjectName(QStringLiteral("angleDialRow.%1").arg(objectRole));
    const auto name = parameterRowLabel(spec_.schemaKey, spec_.role);
    row_ = new GradingValueRow(session_, node_, spec_.role, spec_.schemaKey, name, spec_.neutral,
                               this);
    dial_ = new kit::KAngleDial(this);
    dial_->setObjectName(QStringLiteral("angleDial.%1").arg(objectRole));
    dial_->setAccessibleName(name);
    dial_->setZero(spec_.zeroRight ? kit::KAngleDial::Zero::Right : kit::KAngleDial::Zero::Up);
    dial_->setCounterClockwise(spec_.counterClockwise);
    const double scale = spec_.radians ? kDegreesPerRadian : 1.0;
    dial_->setDefaultValue(spec_.neutral * scale);
    dial_->setResetText(tr("Reset %1").arg(name));
    dial_->addAction(row_->keyAction());
    properties::bindValueEdit(session_, *dial_, [this] {
        return row_->parameter().value_or(document::ParameterId{});
    });
    connect(dial_, &kit::KAngleDial::valueChanged, this, [this, scale, name](const double degrees) {
        if (const auto id = row_->parameter())
            static_cast<void>(
                session_.setParameterValue(*id, degrees / scale, tr("Set %1").arg(name)));
    });
}

void AngleDialRow::setLabelWidth(const int width) { row_->setLabelWidth(width); }

int AngleDialRow::labelWidthHint() const { return row_->labelWidthHint(); }

int AngleDialRow::heightHint() const { return std::max(dialSide(), row_->heightHint()); }

int AngleDialRow::minimumWidthHint() const {
    return row_->minimumWidthHint() + small() + dialSide();
}

int AngleDialRow::socketY() const { return (heightHint() - row_->heightHint()) / 2 + row_->socketY(); }

void AngleDialRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int side = dialSide();
    const int rowWidth = std::max(0, width() - side - small());
    row_->setGeometry(0, (height() - row_->heightHint()) / 2, rowWidth, row_->heightHint());
    dial_->setGeometry(width() - side, (height() - side) / 2, side, side);
}

void AngleDialRow::refresh(const bool locked) {
    row_->refresh(locked);
    const auto id = row_->parameter();
    // A driven angle shows its label and socket and nothing else, like every other card row: the
    // link owns the value, so neither the number nor the dial is offered.
    const auto* composition = session_.composition();
    const auto* record = composition && id ? composition->parameters().find(*id) : nullptr;
    const bool linked =
        record != nullptr && std::holds_alternative<document::DriverBindingSource>(record->source);
    row_->field()->setVisible(!linked);
    dial_->setVisible(!linked);
    dial_->setEnabled(id && !locked && scalarEditable(session_, *id));
    if (const auto value = id ? liveScalar(session_, *id) : std::nullopt;
        value && !dial_->hasLiveEdit()) {
        const QSignalBlocker blocker(dial_);
        dial_->setValue(*value * (spec_.radians ? kDegreesPerRadian : 1.0));
    }
}

// --- RangeRow -----------------------------------------------------------------------------------

RangeRow::RangeRow(CompositionSession& session, const document::NodeId node, Spec spec,
                   QWidget* parent)
    : QWidget(parent), session_(session), node_(node), spec_(std::move(spec)) {
    setObjectName(QStringLiteral("rangeRow.%1").arg(QString::fromStdString(spec_.lowRole)));
    low_ = new GradingValueRow(session_, node_, spec_.lowRole, spec_.schemaKey,
                               parameterRowLabel(spec_.schemaKey, spec_.lowRole),
                               defaultScalar(session_, node_, spec_.lowRole), this);
    if (spec_.highSchemaKey.empty())
        spec_.highSchemaKey = spec_.schemaKey;
    high_ = new GradingValueRow(session_, node_, spec_.highRole, spec_.highSchemaKey,
                                parameterRowLabel(spec_.highSchemaKey, spec_.highRole),
                                defaultScalar(session_, node_, spec_.highRole), this);
    selector_ = new kit::KRangeSelector(this);
    selector_->setObjectName(
        QStringLiteral("rangeSelector.%1").arg(QString::fromStdString(spec_.lowRole)));
    selector_->setAccessibleName(spec_.name);
    selector_->setHandlesMayCross(spec_.mayCross);
    selector_->setRange(spec_.minimum, spec_.maximum);
    selector_->setLumaStrip(spec_.lumaStrip);
    selector_->setToolTip(tr("Drag either end of %1; the numbers above take exact values")
                              .arg(spec_.name.toLower()));
    connect(selector_, &kit::KRangeSelector::editStarted, this, [this] {
        const auto low = low_->parameter();
        const auto high = high_->parameter();
        const auto lowValue = low ? liveScalar(session_, *low) : std::nullopt;
        const auto highValue = high ? liveScalar(session_, *high) : std::nullopt;
        editing_ = low && high && lowValue && highValue &&
                   session_.beginValueEdits({*low, *high}, tr("Set %1").arg(spec_.name));
        if (editing_) {
            editLow_ = *lowValue;
            editHigh_ = *highValue;
            trackLow_ = spec_.centreWidth ? *lowValue - 0.5 * *highValue : *lowValue;
            trackHigh_ = spec_.centreWidth ? *lowValue + 0.5 * *highValue : *highValue;
        }
    });
    const auto update = [this] {
        if (editing_ && !session_.updateValueEdits({editLow_, editHigh_}))
            selector_->cancelEdit();
    };
    // A centre and a width: the two handles are the span's ends, the document keeps its middle and
    // its extent.
    const auto place = [this](const bool lower, const double v) {
        if (spec_.centreWidth) {
            (lower ? trackLow_ : trackHigh_) = v;
            editLow_ = 0.5 * (trackLow_ + trackHigh_);
            editHigh_ = std::max(0.0, trackHigh_ - trackLow_);
        } else {
            (lower ? editLow_ : editHigh_) = v;
        }
    };
    connect(selector_, &kit::KRangeSelector::lowerValueChanged, this, [this, update, place](double v) {
        if (!editing_)
            return;
        place(true, v);
        update();
    });
    connect(selector_, &kit::KRangeSelector::upperValueChanged, this, [this, update, place](double v) {
        if (!editing_)
            return;
        place(false, v);
        update();
    });
    connect(selector_, &kit::KRangeSelector::editFinished, this, [this] {
        const bool owned = editing_ && session_.valueEditActive();
        editing_ = false;
        if (owned)
            static_cast<void>(session_.commitValueEdit());
    });
    connect(selector_, &kit::KRangeSelector::editCancelled, this, [this] {
        const bool owned = editing_ && session_.valueEditActive();
        editing_ = false;
        if (owned)
            session_.cancelValueEdit();
    });
    connect(&session_, &CompositionSession::liveValueChanged, this, [this] {
        // The session dropped the gesture (an undo, a playhead move): so does the track.
        if (editing_ && !session_.valueEditActive()) {
            editing_ = false;
            selector_->cancelEdit();
        }
    });
    connect(selector_, &QObject::destroyed, &session_, [this] {
        if (editing_ && session_.valueEditActive())
            session_.cancelValueEdit();
    });
}

bool RangeRow::draws(const std::string_view role) const {
    return role == spec_.lowRole || role == spec_.highRole;
}

void RangeRow::setLabelWidth(const int width) {
    low_->setLabelWidth(width);
    high_->setLabelWidth(width);
}

int RangeRow::labelWidthHint() const {
    return std::max(low_->labelWidthHint(), high_->labelWidthHint());
}

int RangeRow::heightHint() const {
    return low_->heightHint() + small() + high_->heightHint() + small() + control();
}

int RangeRow::minimumWidthHint() const {
    return std::max(low_->minimumWidthHint(), high_->minimumWidthHint());
}

std::optional<int> RangeRow::socketY(const std::string_view role) const {
    if (role == spec_.lowRole)
        return low_->y() + low_->socketY();
    if (role == spec_.highRole)
        return high_->y() + high_->socketY();
    return std::nullopt;
}

void RangeRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place();
}

void RangeRow::place() {
    int y = 0;
    low_->setGeometry(0, y, width(), low_->heightHint());
    y += low_->heightHint() + small();
    high_->setGeometry(0, y, width(), high_->heightHint());
    y += high_->heightHint() + small();
    selector_->setGeometry(0, y, width(), control());
}

void RangeRow::refresh(const bool locked) {
    low_->refresh(locked);
    high_->refresh(locked);
    const auto low = low_->parameter();
    const auto high = high_->parameter();
    selector_->setEnabled(low && high && !locked && scalarEditable(session_, *low) &&
                          scalarEditable(session_, *high));
    if (selector_->hasLiveEdit())
        return;
    const auto lowValue = low ? liveScalar(session_, *low) : std::nullopt;
    const auto highValue = high ? liveScalar(session_, *high) : std::nullopt;
    if (lowValue && highValue) {
        const QSignalBlocker blocker(selector_);
        if (spec_.centreWidth)
            selector_->setValues(*lowValue - 0.5 * *highValue, *lowValue + 0.5 * *highValue);
        else
            selector_->setValues(*lowValue, *highValue);
    }
}

// --- PointPadRow --------------------------------------------------------------------------------

PointPadRow::PointPadRow(CompositionSession& session, const document::NodeId node, Spec spec,
                         QWidget* parent)
    : QWidget(parent), session_(session), node_(node), spec_(std::move(spec)) {
    setObjectName(QStringLiteral("pointPadRow"));
    pad_ = new kit::KPointPad(this);
    pad_->setObjectName(QStringLiteral("pointPad"));
    pad_->setConnected(spec_.connected);
    pad_->setToolTip(tr("Drag a point to move it; Shift-click to send the selected point there"));
    connect(pad_, &kit::KPointPad::editStarted, this, [this] {
        const auto ids = parameters();
        editing_ = !ids.empty() && ids.size() == visibleCount() &&
                   session_.beginValueEdits(ids, spec_.commandLabel);
    });
    connect(pad_, &kit::KPointPad::pointsChanged, this, [this] {
        if (!editing_)
            return;
        const auto frame = spec_.frame ? spec_.frame() : compositionFrame(session_);
        std::vector<document::ParameterValue> values;
        std::size_t shown = 0;
        for (std::size_t index = 0; index < spec_.points.size(); ++index) {
            if (!pointShown(index))
                continue;
            auto at = pad_->value(shown++);
            if (spec_.points[index].corner)
                at -= frame.center() + QPointF(spec_.points[index].corner->x() * frame.width(),
                                               spec_.points[index].corner->y() * frame.height());
            else if (spec_.points[index].fromCentre)
                at -= frame.center();
            if (spec_.points[index].yUp)
                at.setY(-at.y());
            values.emplace_back(document::Vec2d{at.x(), at.y()});
        }
        if (!session_.updateValueEdits(values))
            pad_->cancelEdit();
    });
    connect(pad_, &kit::KPointPad::editFinished, this, [this] {
        const bool owned = editing_ && session_.valueEditActive();
        editing_ = false;
        if (owned)
            static_cast<void>(session_.commitValueEdit());
    });
    connect(pad_, &kit::KPointPad::editCancelled, this, [this] {
        const bool owned = editing_ && session_.valueEditActive();
        editing_ = false;
        if (owned)
            session_.cancelValueEdit();
    });
    connect(&session_, &CompositionSession::liveValueChanged, this, [this] {
        if (editing_ && !session_.valueEditActive()) {
            editing_ = false;
            pad_->cancelEdit();
        }
    });
    connect(pad_, &QObject::destroyed, &session_, [this] {
        if (editing_ && session_.valueEditActive())
            session_.cancelValueEdit();
    });
}

std::vector<document::ParameterId> PointPadRow::parameters() const {
    std::vector<document::ParameterId> ids;
    for (std::size_t index = 0; index < spec_.points.size(); ++index)
        if (pointShown(index))
            if (const auto id = nodeRoleParameter(session_, node_, spec_.points[index].role))
                ids.push_back(*id);
    return ids;
}

bool PointPadRow::pointShown(const std::size_t index) const {
    return std::ranges::find(inactive_, spec_.points[index].role) == inactive_.end();
}

std::size_t PointPadRow::visibleCount() const {
    std::size_t count = 0;
    for (std::size_t index = 0; index < spec_.points.size(); ++index)
        count += pointShown(index) ? 1U : 0U;
    return count;
}

void PointPadRow::setInactive(std::vector<std::string> roles) {
    if (roles == inactive_)
        return;
    inactive_ = std::move(roles);
    if (!pad_->hasLiveEdit())
        refresh(!pad_->isEnabled());
}

int PointPadRow::heightAt(const int width) const { return pad_->heightForWidth(width); }

int PointPadRow::minimumWidthHint() const {
    // The pad keeps its points' own X/Y rows below it. Its floor is a label plus two position
    // cells at a wide reading, so the card's width is settled before any drag: dragging a point
    // never grows the card (and so never rescales the pad under the pointer).
    const QFontMetrics value(kit::font(kit::TypeRole::Value));
    const int cell = value.horizontalAdvance(QStringLiteral("-4096.00 px")) + kit::px(kit::Spacing::L);
    return std::max(pad_->minimumSizeHint().width(),
                    kit::px(kit::Size::PropertiesLabelWidth) + 2 * cell + 3 * small() +
                        kit::px(kit::Size::IconSmall));
}

void PointPadRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    pad_->setGeometry(0, 0, width(), height());
}

void PointPadRow::refresh(const bool locked) {
    const auto frame = spec_.frame ? spec_.frame() : compositionFrame(session_);
    pad_->setFrame(frame);
    bool editable = !locked;
    std::vector<kit::KPointPad::Point> points;
    for (std::size_t index = 0; index < spec_.points.size(); ++index) {
        if (!pointShown(index))
            continue;
        const auto& point = spec_.points[index];
        const auto id = nodeRoleParameter(session_, node_, point.role);
        const auto value = id ? liveVector(session_, *id) : std::nullopt;
        editable = editable && id && value && scalarEditable(session_, *id);
        const QPointF origin =
            point.corner ? frame.center() + QPointF(point.corner->x() * frame.width(),
                                                    point.corner->y() * frame.height())
                         : (point.fromCentre ? frame.center() : QPointF());
        const auto fallback = defaultVector(session_, node_, point.role);
        const double flip = point.yUp ? -1.0 : 1.0;
        kit::KPointPad::Point shown{point.label,
                                    value ? origin + QPointF(value->x, flip * value->y) : origin,
                                    std::nullopt};
        if (fallback)
            shown.defaultValue = origin + QPointF(fallback->x, flip * fallback->y);
        points.push_back(std::move(shown));
    }
    pad_->setEnabled(editable);
    if (!pad_->hasLiveEdit())
        pad_->setPoints(std::move(points));
    // The backdrop follows the live values, a drag of the pad's own points included.
    if (spec_.backdrop)
        pad_->setBackdrop(spec_.backdrop());
}

// --- NodeControlsCardWidget ---------------------------------------------------------------------

namespace {

struct Controls final {
    std::vector<PointPadRow::Point> points;
    bool connected = false;
    // The frame: the node's own width and height, or the composition's.
    bool nodeSizedFrame = false;
    QString padLabel;
    std::vector<RangeRow::Spec> ranges;
    std::vector<AngleDialRow::Spec> angles;
    // #637: one value drawn as a row with a slider under its name and number.
    struct ValueSlider final {
        std::string role;
        std::string schemaKey;
        double minimum = 0.0;
        double maximum = 100.0;
        double neutral = 0.0;
        double ticks = 0.0;
    };
    std::vector<ValueSlider> sliders;
    std::vector<GradientParameterRow::Spec> gradients;
    struct Curve final {
        std::string role;
        QString name;
        QString description;
    };
    std::vector<Curve> curves;
    // #484: a role is shown only while the node's motion blur reads it (Transform 2D).
    bool readsMotion = false;
    // #514: the pad is shown only while this parameter is active, when set.
    std::string padActivityKey;
};

[[nodiscard]] std::optional<Controls> controlsFor(const std::string_view typeId) {
    const auto angle = [](std::string role, std::string schemaKey, const bool zeroRight,
                          const bool counterClockwise, const bool radians = false) {
        return AngleDialRow::Spec{std::move(role), std::move(schemaKey), zeroRight,
                                  counterClockwise, radians, 0.0};
    };
    const auto operandRange = [](std::string low, std::string high, QString name) {
        return RangeRow::Spec{std::move(low), std::move(high), "bloom.operand.scalar",
                              "bloom.operand.scalar", std::move(name), 0.0, 1.0, true};
    };
    Controls controls;
    if (typeId == "bloom.drop-shadow") {
        // Clockwise from up, as the shadow is cast.
        controls.angles.push_back(angle("direction", "bloom.drop-shadow.direction", false, false));
    } else if (typeId == document::kInnerShadowNodeType) {
        // #528: clockwise from up, as Drop Shadow's; the colour is the card's swatch row.
        controls.angles.push_back(angle("direction", "bloom.inner-shadow.direction", false, false));
    } else if (typeId == "bloom.glow") {
        // The octaves' colours with the gradient on: the tightest octave the start, the widest
        // the end.
        controls.gradients.push_back(
            {"tintGradient", std::string(document::kGlowTintSchemaKey),
             QCoreApplication::translate("NodeControls", "Tint gradient"),
             QCoreApplication::translate("NodeControls",
                                         "With Gradient on, the octaves' colours: the tightest "
                                         "octave takes the left end, the widest the right."),
             QStringLiteral("glowTintGradient"),
             {}});
        // A streak's angle is clockwise from screen right.
        controls.angles.push_back(angle("angle", "bloom.glow.angle", true, false));
    } else if (typeId == "bloom.layer-output") {
        controls.angles.push_back(angle("rotation", "bloom.transform.rotation", false, false));
    } else if (typeId == "bloom.transform-2d") {
        controls.points = {{"pivot", QCoreApplication::translate("NodeControls", "Pivot"), false},
                           {"translate", QCoreApplication::translate("NodeControls", "Offset"),
                            true}};
        controls.padLabel = QCoreApplication::translate("NodeControls", "Move");
        controls.angles.push_back(angle("rotation", "bloom.transform-2d.rotation", false, false));
        // The skew axis, clockwise from screen right.
        controls.angles.push_back(angle("skewAxis", "bloom.transform-2d.skew-axis", true, false));
        // #484: its motion blur's shutter, shown while the motion blur is on.
        controls.angles.push_back(
            angle("shutterAngle", "bloom.transform-2d.shutterAngle", false, false));
        controls.angles.push_back(
            angle("shutterPhase", "bloom.transform-2d.shutterPhase", false, false));
        controls.curves.push_back(
            {"shutterShape", QCoreApplication::translate("NodeControls", "Shutter curve"),
             QCoreApplication::translate("NodeControls",
                                         "How much light the shutter lets through from opening "
                                         "(left) to closing (right)")});
        controls.readsMotion = true;
    } else if (typeId == document::kCameraShakeNodeType) {
        // #512: the roll and the shutter's dials and the seed's button sit on their own rows (see
        // row_inline_controls.hpp); the widget keeps the custom shutter curve's graph and decides
        // which of the shutter's rows the motion blur reads.
        controls.curves.push_back(
            {"shutterShape", QCoreApplication::translate("NodeControls", "Shutter curve"),
             QCoreApplication::translate("NodeControls",
                                         "How much light the shutter lets through from opening "
                                         "(left) to closing (right)")});
        controls.readsMotion = true;
    } else if (typeId == "bloom.linear-gradient" || typeId == "bloom.radial-gradient") {
        controls.gradients.push_back(
            {"gradient", std::string(document::kColorOperandParameterSchemaKey),
             QCoreApplication::translate("NodeControls", "Colors"),
             QCoreApplication::translate("NodeControls",
                                         "The colours from the start point (left) to the end "
                                         "point (right)."),
             QStringLiteral("generatorGradient"),
             {}});
        controls.points = {{"start", QCoreApplication::translate("NodeControls", "Start"), false},
                           {"end", QCoreApplication::translate("NodeControls", "End"), false}};
        controls.connected = true;
        controls.nodeSizedFrame = true;
        controls.padLabel = QCoreApplication::translate("NodeControls", "Move Gradient");
    } else if (typeId == document::kTileMirrorNodeType) {
        // #511: the one point each mode moves, on one pad (the others are inactive and not drawn),
        // and the two angles: the mirror line's, clockwise from screen right (90 is vertical), and
        // the kaleidoscope's turn, clockwise from up.
        const auto tr = [](const char* text) {
            return QCoreApplication::translate("NodeControls", text);
        };
        controls.points = {{"tileCenter", tr("Tile Center"), true},
                           {"offset", tr("Offset"), true},
                           {"mirrorPoint", tr("Line Point"), true},
                           {"kaleidoscopeCenter", tr("Center"), true}};
        controls.padLabel = tr("Move Point");
        controls.angles.push_back(angle("mirrorAngle", "bloom.tile-mirror.mirrorAngle", true, false));
        controls.angles.push_back(
            angle("kaleidoscopeAngle", "bloom.tile-mirror.kaleidoscopeAngle", false, false));
    } else if (typeId == document::kFourColorGradientNodeType) {
        // The four points, in field units (pixels from the centre, +y up), moved on one pad.
        const auto point = [](std::string role, QString label) {
            return PointPadRow::Point{std::move(role), std::move(label), true, true};
        };
        controls.points = {point("point1", QCoreApplication::translate("NodeControls", "Point 1")),
                           point("point2", QCoreApplication::translate("NodeControls", "Point 2")),
                           point("point3", QCoreApplication::translate("NodeControls", "Point 3")),
                           point("point4", QCoreApplication::translate("NodeControls", "Point 4"))};
        controls.nodeSizedFrame = true;
        controls.padLabel = QCoreApplication::translate("NodeControls", "Move Points");
    } else if (typeId == document::kDenoiseNodeType) {
        // #514: the two opposite corners of the region Auto strength measures the noise over, in
        // field units (pixels from the centre, +y up), on one pad shown while Auto strength is on.
        const auto point = [](std::string role, QString label) {
            return PointPadRow::Point{std::move(role), std::move(label), true, true};
        };
        controls.points = {
            point("sampleFrom", QCoreApplication::translate("NodeControls", "Sample corner 1")),
            point("sampleTo", QCoreApplication::translate("NodeControls", "Sample corner 2"))};
        controls.connected = true;
        controls.padLabel = QCoreApplication::translate("NodeControls", "Move Sample Region");
        controls.padActivityKey = std::string(document::kDenoiseSampleFromSchemaKey);
    } else if (typeId == document::kResamplePathNodeType) {
        // The share of points and the start offset are placed on a track, in percent.
        controls.sliders.push_back({"percentage", std::string(document::kResamplePercentageSchemaKey),
                                    0.0, 200.0, 100.0, 50.0});
        controls.sliders.push_back({"startOffset", std::string(document::kResampleStartOffsetSchemaKey),
                                    0.0, 100.0, 0.0, 25.0});
    } else if (typeId == document::kShapeMorphNodeType) {
        // #597: how far from the first shape to the second, on a track, in percent.
        controls.sliders.push_back({"amount", std::string(document::kMorphAmountSchemaKey), 0.0,
                                    100.0, 0.0, 25.0});
    } else if (typeId == document::kTransformPathNodeType ||
               typeId == document::kWarpPathNodeType ||
               typeId == document::kWarpPathFunctionNodeType) {
        // #598: the matrix's position and anchor on one pad (path coordinates, from the centre,
        // +y down), its turn and the skew's direction on dials (clockwise on screen), the slant
        // and, for the warp, its amount on tracks.
        controls.points = {{"position", QCoreApplication::translate("NodeControls", "Position"), true},
                           {"anchor", QCoreApplication::translate("NodeControls", "Anchor"), true}};
        controls.padLabel = QCoreApplication::translate("NodeControls", "Move Path");
        controls.angles.push_back(angle("rotation", "bloom.operand.scalar", false, false));
        controls.angles.push_back(
            angle("skewAxis", std::string(document::kPathSkewAxisSchemaKey), true, false));
        if (typeId != document::kTransformPathNodeType)
            controls.sliders.push_back({"amount", std::string(document::kWarpAmountSchemaKey),
                                        -360.0, 360.0, 0.0, 90.0});
        controls.sliders.push_back({"skew", std::string(document::kPathSkewSchemaKey),
                                    -document::kMaximumPathSkewDegrees,
                                    document::kMaximumPathSkewDegrees, 0.0, 15.0});
    } else if (typeId == "bloom.luma-key") {
        controls.ranges.push_back({"low", "high", "bloom.luma-key.low", "bloom.luma-key.high",
                                   QCoreApplication::translate("NodeControls", "Key range"), 0.0,
                                   1.0, true});
    } else if (typeId == document::kQuantizeNodeType) {
        // #519: the threshold as a span on a luma strip, its centre the level and its width the
        // softness (a hard step is a span of no width).
        RangeRow::Spec spec{"level", "softness",
                            document::colorCleanupSchemaKey(typeId, "level"),
                            document::colorCleanupSchemaKey(typeId, "softness"),
                            QCoreApplication::translate("NodeControls", "Threshold"), 0.0, 1.0,
                            false};
        spec.centreWidth = true;
        spec.lumaStrip = true;
        controls.ranges.push_back(std::move(spec));
    } else if (typeId == document::kToneMapNodeType) {
        // #518: where the highlight roll-off starts and the value it approaches, on one track.
        controls.ranges.push_back(
            {"knee", "ceiling", document::colorCleanupSchemaKey(typeId, "knee"),
             document::colorCleanupSchemaKey(typeId, "ceiling"),
             QCoreApplication::translate("NodeControls", "Roll-off range"), 0.0, 4.0, true});
    } else if (typeId == "bloom.map-range") {
        controls.ranges.push_back(operandRange(
            "fromMin", "fromMax", QCoreApplication::translate("NodeControls", "From range")));
        controls.ranges.push_back(operandRange(
            "toMin", "toMax", QCoreApplication::translate("NodeControls", "To range")));
    } else if (typeId == "bloom.clamp" || typeId == "bloom.in-range" || typeId == "bloom.random" ||
               typeId == "bloom.wrap") {
        controls.ranges.push_back(
            operandRange("min", "max", QCoreApplication::translate("NodeControls", "Range")));
    } else if (typeId == "bloom.smoothstep") {
        controls.ranges.push_back(
            operandRange("edge0", "edge1", QCoreApplication::translate("NodeControls", "Edges")));
    } else if (typeId == "bloom.hue-shift") {
        controls.angles.push_back(angle("degrees", "bloom.operand.scalar", false, false));
    } else if (typeId == "bloom.rotate-2d" || typeId == "bloom.polar-to-cartesian" ||
               typeId == "bloom.degrees-to-radians") {
        // Counter-clockwise from +X in a Y-up frame (value-node-library.md).
        controls.angles.push_back(angle("degrees", "bloom.operand.scalar", true, true));
    } else if (typeId == "bloom.radians-to-degrees") {
        controls.angles.push_back(angle("radians", "bloom.operand.scalar", true, true, true));
    } else if (document::isFieldNodeType(typeId)) {
        // #525: field positions are composition pixels from the centre, +y up; field angles turn
        // counter-clockwise (docs/architecture/fields.md).
        const std::string type(typeId);
        const auto key = [&type](const std::string_view role) {
            return type + "." + std::string(role);
        };
        const auto tr = [](const char* text) {
            return QCoreApplication::translate("NodeControls", text);
        };
        const auto point = [](std::string role, QString label) {
            return PointPadRow::Point{std::move(role), std::move(label), true, true};
        };
        const auto fieldRange = [&key](std::string low, std::string high, QString name,
                                       const double minimum, const double maximum) {
            return RangeRow::Spec{low, high, key(low), key(high), std::move(name), minimum, maximum,
                                  true};
        };
        if (typeId == document::kFieldNoiseNodeType) {
            // Sub Offset moves each octave against the one before, not a place in the frame, so
            // it stays a value row.
            controls.points = {point("offset", tr("Offset"))};
            controls.padLabel = tr("Move Noise");
            controls.angles.push_back(angle("rotation", key("rotation"), false, true));
            controls.angles.push_back(angle("octaveRotation", key("octaveRotation"), false, true));
        } else if (typeId == document::kFieldPatternNodeType) {
            controls.points = {point("centre", tr("Centre"))};
            controls.padLabel = tr("Move Pattern");
            controls.angles.push_back(angle("rotation", key("rotation"), false, true));
        } else if (typeId == document::kFieldGradientNodeType) {
            controls.points = {point("start", tr("Start")), point("end", tr("End"))};
            controls.connected = true;
            controls.padLabel = tr("Move Gradient");
        } else if (typeId == document::kFieldShapeDistanceNodeType) {
            controls.points = {point("centre", tr("Centre"))};
            controls.padLabel = tr("Move Shape");
            controls.angles.push_back(angle("rotation", key("rotation"), false, true));
        } else if (typeId == document::kFieldForceNodeType) {
            controls.points = {point("centre", tr("Centre"))};
            controls.padLabel = tr("Move Force");
            controls.angles.push_back(angle("angle", key("angle"), true, true));
            controls.curves.push_back(
                {"curve", tr("Falloff curve"),
                 tr("With Falloff set to Curve: the strength (up) over the distance from the "
                    "centre as a fraction of the radius (across).")});
        } else if (typeId == document::kFieldMathNodeType) {
            controls.angles.push_back(angle("angle", key("angle"), true, true));
        } else if (typeId == document::kFieldRemapNodeType) {
            controls.ranges.push_back(
                fieldRange("inLow", "inHigh", tr("Input range"), 0.0, 1.0));
            controls.ranges.push_back(
                fieldRange("outLow", "outHigh", tr("Output range"), 0.0, 1.0));
            controls.curves.push_back(
                {"curve", tr("Remap curve"),
                 tr("The output (up) for the input (across) after the input range is mapped to "
                    "0 to 1; the identity diagonal changes nothing.")});
        } else if (typeId == document::kFieldTransformNodeType) {
            controls.points = {point("translate", tr("Translate")), point("pivot", tr("Pivot"))};
            controls.padLabel = tr("Move Field");
            controls.angles.push_back(angle("rotation", key("rotation"), false, true));
        } else if (typeId == document::kFieldSampleNodeType) {
            controls.points = {point("position", tr("Position"))};
            controls.padLabel = tr("Move Sample");
        } else if (typeId == document::kFieldRasterizeNodeType) {
            controls.gradients.push_back(
                {"ramp", std::string(document::kColorOperandParameterSchemaKey), tr("Ramp"),
                 tr("Colour (Ramp) and Heatmap: the colours from Ramp low (left) to Ramp high "
                    "(right)."),
                 QStringLiteral("fieldRampGradient"),
                 {}});
            controls.ranges.push_back(
                fieldRange("rampLow", "rampHigh", tr("Ramp range"), 0.0, 1.0));
            controls.points = {point("explicitCentre", tr("Explicit centre"))};
            controls.padLabel = tr("Move Extent");
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    return controls;
}

} // namespace

bool hasNodeControlsWidget(const std::string_view typeId) noexcept {
    return controlsFor(typeId).has_value();
}

NodeControlsCardWidget::NodeControlsCardWidget(CompositionSession& session,
                                               const document::NodeId node,
                                               const std::string_view typeId, const bool card,
                                               QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeControls") : QStringLiteral("propertiesControls"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    auto controls = controlsFor(typeId).value_or(Controls{});
    if (controls.readsMotion)
        typeId_ = std::string(typeId);
    if (!controls.points.empty()) {
        PointPadRow::Spec spec;
        spec.points = controls.points;
        spec.connected = controls.connected;
        spec.commandLabel = controls.padLabel;
        if (controls.nodeSizedFrame)
            spec.frame = [this] {
                const auto size = [this](const std::string_view role, const double fallback) {
                    const auto id = nodeRoleParameter(session_, node_, role);
                    const auto value = id ? liveScalar(session_, *id) : std::nullopt;
                    return value && *value > 0.0 ? *value : fallback;
                };
                const auto frame = compositionFrame(session_);
                return QRectF(0.0, 0.0, size("width", frame.width()),
                              size("height", frame.height()));
            };
        else
            spec.frame = [this] { return compositionFrame(session_); };
        pad_ = new PointPadRow(session_, node_, std::move(spec), this);
        padActivityKey_ = std::move(controls.padActivityKey);
        padShown_ = padShown();
    }
    inactive_ = inactiveRoles();
    if (pad_ != nullptr) {
        pad_->setInactive(inactive_);
        padShown_ = padShown();
    }
    for (auto& gradient : controls.gradients) {
        gradients_.push_back(new GradientParameterRow(session_, node_, std::move(gradient), this));
        // #543: the stops' rows open, close and follow their links.
        connect(gradients_.back(), &GradientParameterRow::layoutChanged, this, [this] {
            place();
            emit rowsChanged();
        });
    }
    for (auto& range : controls.ranges)
        ranges_.push_back(new RangeRow(session_, node_, std::move(range), this));
    for (auto& angle : controls.angles) {
        angle.neutral = defaultScalar(session_, node_, angle.role);
        angles_.push_back(new AngleDialRow(session_, node_, std::move(angle), this));
    }
    for (const auto& slider : controls.sliders) {
        auto* row = new GradingValueRow(session_, node_, slider.role, slider.schemaKey,
                                        parameterRowLabel(slider.schemaKey, slider.role),
                                        slider.neutral, this);
        GradingSliderSpec spec;
        spec.mode = kit::KSlider::DragMode::Absolute;
        spec.minimum = slider.minimum;
        spec.maximum = slider.maximum;
        spec.neutral = slider.neutral;
        spec.ticks = slider.ticks;
        row->setSlider(spec);
        sliders_.push_back(row);
    }
    for (auto& curve : controls.curves) {
        CurveControl control;
        control.role = curve.role;
        control.label = new kit::KLabel(curve.name, this);
        control.label->setToolTip(curve.description);
        control.canvas = new CurveGraphCanvas(session_, node_, curve.role, this);
        control.canvas->setAccessibleName(curve.name);
        control.canvas->setObjectName(QStringLiteral("fieldCurve_") +
                                      QString::fromStdString(curve.role));
        curves_.push_back(control);
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &NodeControlsCardWidget::refresh);
    refresh();
}

bool NodeControlsCardWidget::readsRole(const std::string_view role) const {
    return typeId_.empty() || motionRoleRead(session_, node_, typeId_, role);
}

bool NodeControlsCardWidget::padShown() const {
    if (pad_ == nullptr)
        return false;
    // A pad whose every point is inactive in the node's mode is not drawn.
    if (pad_->visibleCount() == 0)
        return false;
    if (padActivityKey_.empty())
        return true;
    const auto* composition = session_.composition();
    const auto* node = composition != nullptr ? composition->graph().findNode(node_) : nullptr;
    return node == nullptr || !document::isParameterInactive(*composition, *node, padActivityKey_);
}

std::vector<std::string> NodeControlsCardWidget::inactiveRoles() const {
    std::vector<std::string> roles;
    const auto* composition = session_.composition();
    const auto* node = composition != nullptr ? composition->graph().findNode(node_) : nullptr;
    const auto* definition = definitionOf(session_, node_);
    if (node == nullptr || definition == nullptr)
        return roles;
    for (const auto& parameter : definition->parameters)
        if (document::isParameterInactive(*composition, *node, parameter.schemaKey))
            roles.push_back(parameter.role);
    return roles;
}

bool NodeControlsCardWidget::roleActive(const std::string_view role) const {
    return std::ranges::find(inactive_, role) == inactive_.end();
}

bool NodeControlsCardWidget::showsRole(const std::string_view role) const {
    if (!readsRole(role) || !roleActive(role))
        return false;
    // The pad's points keep their own X/Y rows (the exact numbers, keys and sockets); a range or
    // an angle row or a gradient row is drawn here.
    for (const auto* gradient : gradients_)
        if (gradient->role() == role)
            return false;
    for (const auto* range : ranges_)
        if (range->draws(role))
            return false;
    for (const auto* angle : angles_)
        if (angle->role() == role)
            return false;
    for (const auto* slider : sliders_)
        if (slider->role() == role)
            return false;
    for (const auto& curve : curves_)
        if (curve.role == role)
            return false;
    return true;
}

std::optional<int> NodeControlsCardWidget::socketY(const std::string_view role) const {
    for (const auto* gradient : gradients_)
        if (const auto at = gradient->socketY(role))
            return gradient->y() + *at;
    for (const auto* range : ranges_)
        if (const auto at = range->socketY(role))
            return range->y() + *at;
    for (const auto* angle : angles_)
        if (angle->role() == role && readsRole(role) && roleActive(role))
            return angle->y() + angle->socketY();
    for (const auto* slider : sliders_)
        if (slider->role() == role && roleActive(role))
            return slider->y() + slider->socketY();
    return std::nullopt;
}

int NodeControlsCardWidget::heightAt(const int width) const {
    int height = 0;
    for (const auto* gradient : gradients_)
        height += gradient->heightHint() + small();
    if (padShown_)
        height += pad_->heightAt(width) + small();
    for (const auto* range : ranges_)
        if (roleActive(range->lowRole()))
            height += range->heightHint() + small();
    for (const auto* angle : angles_)
        if (readsRole(angle->role()) && roleActive(angle->role()))
            height += angle->heightHint() + small();
    for (const auto* slider : sliders_)
        if (roleActive(slider->role()))
            height += slider->heightHint() + small();
    for (const auto& curve : curves_)
        if (readsRole(curve.role))
            height +=
                curve.label->sizeHint().height() + curveHeight(curve.canvas, width) + 2 * small();
    return std::max(0, height - small());
}

int NodeControlsCardWidget::minimumWidthHint() const {
    int width = kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
    if (pad_ != nullptr)
        width = std::max(width, pad_->minimumWidthHint());
    for (const auto* gradient : gradients_)
        width = std::max(width, gradient->minimumWidthHint());
    for (const auto* range : ranges_)
        width = std::max(width, range->minimumWidthHint());
    for (const auto* angle : angles_)
        width = std::max(width, angle->minimumWidthHint());
    for (const auto* slider : sliders_)
        width = std::max(width, slider->minimumWidthHint());
    for (const auto& curve : curves_)
        width = std::max({width, curve.canvas->minimumSizeHint().width(),
                          curve.label->sizeHint().width()});
    return width;
}

QSize NodeControlsCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void NodeControlsCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void NodeControlsCardWidget::place() {
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = 0;
    for (const auto* gradient : gradients_)
        labels = std::max(labels, gradient->labelWidthHint());
    for (const auto* range : ranges_)
        labels = std::max(labels, range->labelWidthHint());
    for (const auto* angle : angles_)
        labels = std::max(labels, angle->labelWidthHint());
    for (const auto* slider : sliders_)
        labels = std::max(labels, slider->labelWidthHint());
    int y = 0;
    for (auto* gradient : gradients_) {
        gradient->setLabelWidth(labels);
        gradient->setGeometry(0, y, width(), gradient->heightHint());
        y += gradient->heightHint() + small();
    }
    if (pad_ != nullptr) {
        pad_->setVisible(padShown_);
        if (padShown_) {
            const int h = pad_->heightAt(width());
            pad_->setGeometry(0, y, width(), h);
            y += h + small();
        }
    }
    for (auto* range : ranges_) {
        // A range follows the mode that reads it (Quantize's threshold is hidden while posterizing).
        const bool shown = roleActive(range->lowRole());
        range->setVisible(shown);
        if (!shown)
            continue;
        range->setLabelWidth(labels);
        range->setGeometry(0, y, width(), range->heightHint());
        y += range->heightHint() + small();
    }
    for (auto* angle : angles_) {
        const bool shown = readsRole(angle->role()) && roleActive(angle->role());
        angle->setVisible(shown);
        if (!shown)
            continue;
        angle->setLabelWidth(labels);
        angle->setGeometry(0, y, width(), angle->heightHint());
        y += angle->heightHint() + small();
    }
    for (auto* slider : sliders_) {
        const bool shown = roleActive(slider->role());
        slider->setVisible(shown);
        if (!shown)
            continue;
        slider->setLabelWidth(labels);
        slider->setGeometry(0, y, width(), slider->heightHint());
        y += slider->heightHint() + small();
    }
    for (const auto& curve : curves_) {
        const bool shown = readsRole(curve.role);
        curve.label->setVisible(shown);
        curve.canvas->setVisible(shown);
        if (!shown)
            continue;
        const int labelHeight = curve.label->sizeHint().height();
        curve.label->setGeometry(0, y, width(), labelHeight);
        y += labelHeight + small();
        const int h = curveHeight(curve.canvas, width());
        curve.canvas->setGeometry(0, y, width(), h);
        y += h + small();
    }
}

void NodeControlsCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    for (auto* gradient : gradients_)
        gradient->refresh(locked);
    // A value row appears or goes with what the node reads.
    if (auto inactive = inactiveRoles(); inactive != inactive_) {
        inactive_ = std::move(inactive);
        if (pad_ != nullptr)
            pad_->setInactive(inactive_);
        place();
        updateGeometry();
        emit rowsChanged();
    }
    if (pad_ != nullptr) {
        pad_->refresh(locked);
        // The pad appears or goes with the parameter it follows.
        if (const bool shown = padShown(); shown != padShown_) {
            padShown_ = shown;
            place();
            updateGeometry();
            emit rowsChanged();
        }
    }
    for (auto* range : ranges_)
        range->refresh(locked);
    for (auto* angle : angles_)
        angle->refresh(locked);
    for (auto* slider : sliders_)
        slider->refresh(locked);
    for (const auto& curve : curves_) {
        curve.canvas->setEnabled(!locked && curve.canvas->parameter().has_value());
        curve.canvas->update();
    }
    // A shutter row appears or goes with the motion blur that reads it.
    if (!typeId_.empty()) {
        std::vector<bool> reads;
        for (const auto* angle : angles_)
            reads.push_back(readsRole(angle->role()));
        for (const auto& curve : curves_)
            reads.push_back(readsRole(curve.role));
        for (const auto role : {std::string_view{"quality"}, std::string_view{"minSamples"},
                                std::string_view{"loopLength"}, std::string_view{"octaves"},
                                std::string_view{"roughness"}})
            reads.push_back(readsRole(role));
        if (reads != reads_) {
            reads_ = std::move(reads);
            place();
            updateGeometry();
            emit rowsChanged();
        }
    }
}

} // namespace bloom::ui
