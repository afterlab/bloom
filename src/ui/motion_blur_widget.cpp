#include <bloom/ui/motion_blur_widget.hpp>

#include <bloom/document/image_preview.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>

#include <algorithm>
#include <initializer_list>
#include <variant>

namespace bloom::ui {
namespace {

using namespace std::string_view_literals;

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] std::int64_t liveInteger(const CompositionSession& session,
                                       const document::NodeId node, const std::string_view role,
                                       const std::int64_t fallback = 0) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<std::int64_t>(&*value) : nullptr;
    return stored ? *stored : fallback;
}

[[nodiscard]] bool liveBool(const CompositionSession& session, const document::NodeId node,
                            const std::string_view role) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<bool>(&*value) : nullptr;
    return stored != nullptr && *stored;
}

[[nodiscard]] double defaultScalar(const CompositionSession& session, const document::NodeId node,
                                   const std::string_view role) {
    const auto* composition = session.composition();
    const auto* record = composition != nullptr ? composition->graph().findNode(node) : nullptr;
    const auto* definition =
        record != nullptr
            ? document::builtInNodeDefinitions().find(record->typeId, record->schemaVersion)
            : nullptr;
    if (definition == nullptr)
        return 0.0;
    const auto found =
        std::ranges::find(definition->parameters, role, &document::ParameterDefinition::role);
    const auto* scalar =
        found == definition->parameters.end() ? nullptr : std::get_if<double>(&found->defaultValue);
    return scalar ? *scalar : 0.0;
}

[[nodiscard]] bool connected(const CompositionSession& session, const document::NodeId node,
                             const std::string_view port) {
    const auto* composition = session.composition();
    return composition != nullptr &&
           document::inspectedInputSource(composition->graph(), node, port).has_value();
}

// How many times a sub-frame shutter renders its upstream: its quality's count, Final's maximum
// (rounded up to the power of two the engine takes).
[[nodiscard]] std::int64_t subFrameSamples(const CompositionSession& session,
                                           const document::NodeId node) {
    switch (liveInteger(session, node, "quality", document::kMotionQualityStandard)) {
    case document::kMotionQualityDraft:
        return 4;
    case document::kMotionQualityStandard:
        return 16;
    case document::kMotionQualityHigh:
        return 64;
    default:
        break;
    }
    const auto maximum = std::clamp<std::int64_t>(liveInteger(session, node, "maxSamples", 64), 1,
                                                  document::kMaximumMotionSamples);
    std::int64_t count = 1;
    while (count < maximum)
        count *= 2;
    return count;
}

} // namespace

bool motionRoleRead(const CompositionSession& session, const document::NodeId node,
                    const std::string_view typeId, const std::string_view role) {
    const auto quality = liveInteger(session, node, "quality", document::kMotionQualityStandard);
    // The shutter's own rows, on every node that has them.
    if (role == "shutterPhase"sv)
        return liveInteger(session, node, "shutterOffset") == document::kShutterOffsetCustom &&
               (typeId != document::kTransform2DNodeType || liveBool(session, node, "motionBlur"));
    if (role == "shutterShape"sv)
        return liveInteger(session, node, "shutterCurve") == document::kShutterCurveCustom &&
               (typeId != document::kTransform2DNodeType || liveBool(session, node, "motionBlur"));
    if (role == "minSamples"sv || role == "maxSamples"sv)
        return quality == document::kMotionQualityFinal &&
               (typeId != document::kTransform2DNodeType || liveBool(session, node, "motionBlur"));
    if (typeId == document::kTransform2DNodeType) {
        if (role == "shutterAngle"sv || role == "shutterOffset"sv || role == "shutterCurve"sv ||
            role == "quality"sv || role == "jitter"sv)
            return liveBool(session, node, "motionBlur");
        return true;
    }
    if (typeId == document::kMotionBlurNodeType) {
        if (role == "vectorLayer"sv) {
            const auto footage = liveInteger(session, node, "footage");
            return footage == document::kFootageAuto || footage == document::kFootageVectors;
        }
        return true;
    }
    if (typeId == document::kVectorMotionBlurNodeType) {
        const auto convention = liveInteger(session, node, "convention");
        const bool custom = convention == document::kVectorConventionCustom;
        if (role == "vectorScale"sv || role == "vectorOffset"sv || role == "flipY"sv ||
            role == "layout"sv || role == "units"sv || role == "alphaValid"sv)
            return custom;
        if (role == "maxDisplace"sv)
            return convention == document::kVectorConventionRsmb;
        if (role == "depthChannel"sv || role == "softDepth"sv)
            return connected(session, node, document::kMotionDepthPort);
        return true;
    }
    if (typeId == document::kOpticalFlowNodeType) {
        if (role == "cutThreshold"sv)
            return liveBool(session, node, "cutDetection");
        return true;
    }
    if (typeId == document::kOpticalFlowMotionBlurNodeType) {
        if (role == "holeFill"sv || role == "matteShrink"sv || role == "alphaBoost"sv)
            return connected(session, node, document::kMotionMattePort);
        return true;
    }
    if (typeId == document::kEchoNodeType) {
        if (role == "posterizeRate"sv)
            return liveBool(session, node, "posterize");
        if (role == "intensity"sv || role == "decay"sv || role == "operator"sv)
            return liveInteger(session, node, "mode") == document::kEchoModeEcho;
        return true;
    }
    return true;
}

MotionBlurCardWidget::MotionBlurCardWidget(CompositionSession& session, const document::NodeId node,
                                           const std::string_view typeId, const bool card,
                                           QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node), typeId_(typeId) {
    setObjectName(card ? QStringLiteral("nodeMotionBlur") : QStringLiteral("propertiesMotionBlur"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    const auto key = [typeId](const std::string_view role) {
        return document::motionSchemaKey(typeId, role);
    };
    const auto has = [&](const std::string_view role) {
        return nodeRoleParameter(session_, node_, role).has_value();
    };
    const auto defaultOf = [&](const std::string_view role) {
        return defaultScalar(session_, node_, role);
    };
    if (has("shutterAngle")) {
        shutter_ = new AngleDialRow(session_, node_,
                                    AngleDialRow::Spec{"shutterAngle", key("shutterAngle"), false,
                                                       false, false, defaultOf("shutterAngle")},
                                    this);
        shutter_->setToolTip(tr("The time the shutter stays open: 360 degrees is a whole frame"));
    }
    if (has("shutterPhase"))
        phase_ = new AngleDialRow(session_, node_,
                                  AngleDialRow::Spec{"shutterPhase", key("shutterPhase"), false,
                                                     false, false, defaultOf("shutterPhase")},
                                  this);
    if (typeId == document::kDirectionalBlurNodeType)
        direction_ = new AngleDialRow(
            session_, node_,
            AngleDialRow::Spec{"angle", key("angle"), true, false, false, defaultOf("angle")}, this);
    if (typeId == document::kSpinBlurNodeType)
        direction_ = new AngleDialRow(
            session_, node_,
            AngleDialRow::Spec{"amount", key("amount"), false, false, false, defaultOf("amount")},
            this);
    if (typeId == document::kSpinBlurNodeType || typeId == document::kZoomBlurNodeType) {
        PointPadRow::Spec pad;
        pad.points = {{"centre", tr("Centre"), true}};
        pad.commandLabel = tr("Move Centre");
        pad_ = new PointPadRow(session_, node_, std::move(pad), this);
        pad_->pad()->setToolTip(tr("Drag the point the blur turns or zooms around"));
    }
    if (has("shutterShape")) {
        curveLabel_ = new kit::KLabel(tr("Shutter curve"), this);
        curveLabel_->setToolTip(
            tr("How much light the shutter lets through from opening (left) to closing (right)"));
        curve_ = new CurveGraphCanvas(session_, node_, "shutterShape", this);
        curve_->setObjectName(QStringLiteral("motionShutterCurve"));
        curve_->setAccessibleName(tr("Shutter curve"));
    }
    if (typeId == document::kMotionBlurNodeType) {
        cost_ = new kit::KLabel(this);
        cost_->setObjectName(QStringLiteral("motionBlurCost"));
        cost_->setWordWrap(true);
        cost_->setTone(kit::Color::Muted);
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &MotionBlurCardWidget::refresh);
    refresh();
}

bool MotionBlurCardWidget::showsRole(const std::string_view role) const {
    // The card draws the dials and the curve; a role the current state does not read is hidden.
    if (shutter_ != nullptr && role == "shutterAngle"sv)
        return false;
    if (phase_ != nullptr && role == "shutterPhase"sv)
        return false;
    if (direction_ != nullptr && role == direction_->role())
        return false;
    if (role == "shutterShape"sv)
        return false;
    return motionRoleRead(session_, node_, typeId_, role);
}

std::optional<int> MotionBlurCardWidget::socketY(const std::string_view role) const {
    for (const auto* row : {shutter_, phase_, direction_})
        if (row != nullptr && row->role() == role && row->isVisibleTo(this))
            return row->y() + row->socketY();
    return std::nullopt;
}

std::vector<bool> MotionBlurCardWidget::readState() const {
    std::vector<bool> state;
    for (const auto role : {"shutterPhase"sv, "shutterShape"sv, "minSamples"sv, "vectorLayer"sv,
                            "vectorScale"sv, "maxDisplace"sv, "depthChannel"sv, "cutThreshold"sv,
                            "holeFill"sv, "posterizeRate"sv, "decay"sv})
        state.push_back(motionRoleRead(session_, node_, typeId_, role));
    return state;
}

std::vector<QWidget*> MotionBlurCardWidget::shownWidgets() const {
    std::vector<QWidget*> widgets;
    if (cost_ != nullptr)
        widgets.push_back(cost_);
    if (direction_ != nullptr)
        widgets.push_back(direction_);
    if (pad_ != nullptr)
        widgets.push_back(pad_);
    if (shutter_ != nullptr)
        widgets.push_back(shutter_);
    if (phase_ != nullptr && motionRoleRead(session_, node_, typeId_, "shutterPhase"))
        widgets.push_back(phase_);
    if (curve_ != nullptr && motionRoleRead(session_, node_, typeId_, "shutterShape")) {
        widgets.push_back(curveLabel_);
        widgets.push_back(curve_);
    }
    return widgets;
}

int MotionBlurCardWidget::widgetHeight(const QWidget* widget, const int width) const {
    if (widget == cost_)
        return cost_->heightForWidth(width);
    if (widget == pad_)
        return pad_->heightAt(width);
    for (const auto* row : {shutter_, phase_, direction_})
        if (widget == row)
            return row->heightHint();
    if (widget == curve_) {
        const auto hint = curve_->sizeHint();
        return hint.width() > 0 ? width * hint.height() / hint.width() : hint.height();
    }
    return widget->sizeHint().height();
}

int MotionBlurCardWidget::heightAt(const int width) const {
    int height = 0;
    for (const auto* widget : shownWidgets())
        height += widgetHeight(widget, width) + small();
    return std::max(0, height - small());
}

int MotionBlurCardWidget::minimumWidthHint() const {
    int width = kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
    for (const auto* row : {shutter_, phase_, direction_})
        if (row != nullptr)
            width = std::max(width, row->minimumWidthHint());
    if (pad_ != nullptr)
        width = std::max(width, pad_->minimumWidthHint());
    if (curve_ != nullptr)
        width = std::max({width, curve_->minimumSizeHint().width(),
                          curveLabel_->sizeHint().width()});
    return width;
}

QSize MotionBlurCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void MotionBlurCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void MotionBlurCardWidget::place() {
    const auto shown = shownWidgets();
    for (auto* widget : std::initializer_list<QWidget*>{shutter_, phase_, direction_, pad_,
                                                        curveLabel_, curve_, cost_})
        if (widget != nullptr)
            widget->setVisible(std::ranges::find(shown, widget) != shown.end());
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = 0;
    for (const auto* row : {shutter_, phase_, direction_})
        if (row != nullptr)
            labels = std::max(labels, row->labelWidthHint());
    for (auto* row : {shutter_, phase_, direction_})
        if (row != nullptr)
            row->setLabelWidth(labels);
    int y = 0;
    for (auto* widget : shown) {
        const int h = widgetHeight(widget, width());
        widget->setGeometry(0, y, width(), h);
        y += h + small();
    }
}

void MotionBlurCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    for (auto* row : {shutter_, phase_, direction_})
        if (row != nullptr)
            row->refresh(locked);
    if (pad_ != nullptr)
        pad_->refresh(locked);
    if (curve_ != nullptr) {
        curve_->setEnabled(!locked && curve_->parameter().has_value());
        curve_->update();
    }
    bool costChanged = false;
    if (cost_ != nullptr) {
        const auto samples = subFrameSamples(session_, node_);
        const auto text = samples == 1
                              ? tr("Renders everything upstream once a frame.")
                              : tr("Renders everything upstream %1 times a frame.").arg(samples);
        costChanged = cost_->text() != text;
        cost_->setText(text);
    }
    auto state = readState();
    if (state != state_ || costChanged) {
        state_ = std::move(state);
        place();
        updateGeometry();
        emit rowsChanged();
    }
}

} // namespace bloom::ui
