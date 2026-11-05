#include <bloom/ui/depth_effect_widget.hpp>

#include <bloom/document/asset.hpp>
#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/document/image_preview.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/image_preview_controller.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>

#include <QFontMetrics>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace bloom::ui {
namespace {

using namespace std::string_view_literals;

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

// A colour picture's share of spread pixels above which the card warns.
constexpr double kSpreadWarningFraction = 0.05;

[[nodiscard]] QRectF compositionFrame(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {0.0, 0.0, 1920.0, 1080.0};
    const auto format = composition->format();
    return {0.0, 0.0, static_cast<double>(format.width()), static_cast<double>(format.height())};
}

[[nodiscard]] std::int64_t liveInteger(const CompositionSession& session,
                                       const document::NodeId node, const std::string_view role,
                                       const std::int64_t fallback = 0) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<std::int64_t>(&*value) : nullptr;
    return stored ? *stored : fallback;
}

[[nodiscard]] std::string liveText(const CompositionSession& session, const document::NodeId node,
                                   const std::string_view role) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<std::string>(&*value) : nullptr;
    return stored ? *stored : std::string{};
}

[[nodiscard]] std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// The connected depth output's kind, or nullopt when nothing is connected.
[[nodiscard]] std::optional<document::SocketValueKind>
depthSourceKind(const CompositionSession& session, const document::NodeId node) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return std::nullopt;
    const auto source = document::inspectedInputSource(composition->graph(), node,
                                                       document::kDepthEffectDepthPort);
    if (!source)
        return std::nullopt;
    return source->kind;
}

} // namespace

DepthEffectCardWidget::DepthEffectCardWidget(CompositionSession& session,
                                             const document::NodeId node, const bool card,
                                             QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeDepthEffect") : QStringLiteral("propertiesDepthEffect"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    const auto* composition = session_.composition();
    const auto* record = composition ? composition->graph().findNode(node_) : nullptr;
    defocus_ = record == nullptr || record->typeId == document::kZDefocusNodeType;
    const auto typeId = defocus_ ? document::kZDefocusNodeType : document::kDepthFogNodeType;
    const auto key = [typeId](const std::string_view role) {
        return document::depthSchemaKey(typeId, role);
    };

    colorWarning_ = new kit::KLabel(this);
    colorWarning_->setObjectName(QStringLiteral("depthColorWarning"));
    colorWarning_->setWordWrap(true);
    colorWarning_->setTone(kit::Color::Warn);
    readAsData_ = new kit::KButton(tr("Read as data"), this);
    readAsData_->setObjectName(QStringLiteral("depthReadAsData"));
    readAsData_->setVariant(kit::KButton::Variant::Secondary);
    readAsData_->setToolTip(tr("Read the depth file without a colour transform (Raw), as a depth, "
                               "mist or size pass needs"));
    connect(readAsData_, &QAbstractButton::clicked, this,
            [this] { static_cast<void>(readDepthAsData()); });
    spreadWarning_ = new kit::KLabel(this);
    spreadWarning_->setObjectName(QStringLiteral("depthSpreadWarning"));
    spreadWarning_->setWordWrap(true);
    spreadWarning_->setTone(kit::Color::Warn);

    const auto range = [&](const char* low, const char* high, QString name, const double maximum) {
        auto* row = new RangeRow(session_, node_,
                                 RangeRow::Spec{low, high, key(low), key(high), std::move(name),
                                                0.0, maximum, true},
                                 this);
        return row;
    };
    nearFarUnit_ = range("near", "far", tr("Near / far distance"), 1.0);
    nearFarScene_ = range("near", "far", tr("Near / far distance"), 1000.0);
    if (!defocus_) {
        fogUnit_ = range("start", "end", tr("Fog start / end"), 1.0);
        fogScene_ = range("start", "end", tr("Fog start / end"), 100.0);
        curveLabel_ = new kit::KLabel(tr("Falloff curve"), this);
        curveLabel_->setToolTip(tr("How the fog thickens from its start (left) to its end (right)"));
        curve_ = new CurveGraphCanvas(session_, node_, "curve", this);
        curve_->setObjectName(QStringLiteral("depthFogCurve"));
        curve_->setAccessibleName(tr("Falloff curve"));
    } else {
        PointPadRow::Spec pad;
        pad.points = {{"focusPoint", tr("Focus point"), true}};
        pad.commandLabel = tr("Move Focus Point");
        pad.frame = [this] { return compositionFrame(session_); };
        pad_ = new PointPadRow(session_, node_, std::move(pad), this);
        pad_->pad()->setToolTip(tr("Drag the point the lens focuses on; the depth is read as the "
                                   "mean of the 5 x 5 pixels around it"));
        pickFocus_ = new kit::KButton(kit::IconId::Eyedropper, tr("Pick focus in Viewer"), this);
        pickFocus_->setObjectName(QStringLiteral("depthPickFocus"));
        pickFocus_->setVariant(kit::KButton::Variant::Secondary);
        pickFocus_->setToolTip(tr("Click the subject in the Viewer to focus on it (Shift-click "
                                  "keeps picking, for a rack focus; Escape cancels)"));
        connect(pickFocus_, &QAbstractButton::clicked, this,
                [this] { session_.requestFocusPick(node_); });
        rotation_ = new AngleDialRow(
            session_, node_, AngleDialRow::Spec{"rotation", key("rotation"), true, false, false, 0.0},
            this);
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &DepthEffectCardWidget::refresh);
    if (auto* previews = session_.imagePreviews())
        connect(previews, &ImagePreviewController::changed, this,
                &DepthEffectCardWidget::refreshWarnings);
    refresh();
}

DepthEffectCardWidget::State DepthEffectCardWidget::readState() const {
    State state;
    state.mode = defocus_ ? liveInteger(session_, node_, "mode") : document::kDefocusModeDepth;
    state.interpretation = liveInteger(session_, node_, "interpretation");
    state.focusFrom = liveInteger(session_, node_, "focusFrom");
    state.shape = liveInteger(session_, node_, "shape");
    state.falloff = liveInteger(session_, node_, "falloff");
    state.background = liveInteger(session_, node_, "background");
    const auto kind = depthSourceKind(session_, node_);
    state.depthConnected = kind.has_value();
    state.channelSet = kind == document::SocketValueKind::ChannelSet;
    return state;
}

bool DepthEffectCardWidget::reads(const std::string_view role) const {
    const auto& s = state_;
    const bool uniform = defocus_ && s.mode == document::kDefocusModeUniform;
    const bool lens = !defocus_ || s.mode == document::kDefocusModeDepth ||
                      s.mode == document::kDefocusModePhysical;
    const bool physical = defocus_ && s.mode == document::kDefocusModePhysical;
    const bool remapped = s.interpretation == document::kDepthInterpretationNormalised ||
                          s.interpretation == document::kDepthInterpretationMist;
    if (role == "depthChannel"sv || role == "background"sv)
        return !uniform;
    if (role == "interpretation"sv)
        return lens;
    if (role == "near"sv || role == "far"sv)
        return lens && (remapped || s.background == document::kDepthBackgroundFar);
    if (role == "invert"sv)
        return lens && remapped;
    if (role == "mistFalloff"sv)
        return lens && s.interpretation == document::kDepthInterpretationMist;
    if (!defocus_) {
        const bool exponential = s.falloff == document::kFogFalloffExponential ||
                                 s.falloff == document::kFogFalloffExponentialSquared;
        if (role == "end"sv)
            return !exponential;
        if (role == "density"sv)
            return exponential;
        if (role == "curve"sv)
            return s.falloff == document::kFogFalloffCurve;
        return true;
    }
    if (role == "units"sv || role == "focalLength"sv || role == "sensorWidth"sv ||
        role == "fStop"sv)
        return physical;
    if (role == "focusFrom"sv || role == "depthOfField"sv || role == "blurRegion"sv)
        return lens;
    if (role == "focusDistance"sv)
        return lens && s.focusFrom == document::kFocusFromDistance;
    if (role == "focusPoint"sv)
        return lens && s.focusFrom == document::kFocusFromPoint;
    if (role == "size"sv)
        return !physical;
    if (role == "maxSize"sv)
        return lens;
    if (role == "edgeExpand"sv)
        return !uniform;
    if (role == "blades"sv || role == "roundness"sv || role == "rotation"sv)
        return s.shape == document::kBokehPolygon;
    return true;
}

RangeRow* DepthEffectCardWidget::nearFarRow() const {
    if (!reads("near"))
        return nullptr;
    const auto id = nodeRoleParameter(session_, node_, "far");
    const auto far = id ? liveScalar(session_, *id) : std::nullopt;
    return far && *far <= 1.0 ? nearFarUnit_ : nearFarScene_;
}

RangeRow* DepthEffectCardWidget::fogRangeRow() const {
    if (defocus_ || !reads("end"))
        return nullptr;
    // The fog's start and end are distances in the depth's own units: 0-1 when a normalised or
    // mist depth maps to 0-1, else scene units.
    const auto id = nodeRoleParameter(session_, node_, "far");
    const auto far = id ? liveScalar(session_, *id) : std::nullopt;
    const bool remapped = state_.interpretation == document::kDepthInterpretationNormalised ||
                          state_.interpretation == document::kDepthInterpretationMist;
    return remapped && far && *far <= 1.0 ? fogUnit_ : fogScene_;
}

bool DepthEffectCardWidget::showsRole(const std::string_view role) const {
    // The card draws the range pairs, the rotation dial and the curve; the focus point keeps its
    // own X/Y row beside the pad and the depth channel its picker row. A role the current state
    // does not read is hidden.
    if (role == "curve"sv || role == "rotation"sv)
        return false;
    if (role == "near"sv || role == "far"sv)
        return false;
    if (!defocus_ && (role == "start"sv || role == "end"sv) && fogRangeRow() != nullptr)
        return false;
    return reads(role);
}

std::optional<int> DepthEffectCardWidget::socketY(const std::string_view role) const {
    if (auto* row = nearFarRow(); row != nullptr)
        if (const auto at = row->socketY(role))
            return row->y() + *at;
    if (auto* row = fogRangeRow(); row != nullptr)
        if (const auto at = row->socketY(role))
            return row->y() + *at;
    if (rotation_ != nullptr && role == "rotation"sv && rotation_->isVisibleTo(this))
        return rotation_->y() + rotation_->socketY();
    return std::nullopt;
}

std::vector<QWidget*> DepthEffectCardWidget::shownWidgets() const {
    std::vector<QWidget*> widgets;
    if (!colorWarningText_.isEmpty()) {
        widgets.push_back(colorWarning_);
        widgets.push_back(readAsData_);
    }
    if (!spreadWarningText_.isEmpty())
        widgets.push_back(spreadWarning_);
    if (auto* row = nearFarRow())
        widgets.push_back(row);
    if (auto* row = fogRangeRow())
        widgets.push_back(row);
    if (defocus_) {
        if (reads("focusPoint"))
            widgets.push_back(pad_);
        if (reads("focusFrom"))
            widgets.push_back(pickFocus_);
        if (reads("rotation"))
            widgets.push_back(rotation_);
    } else if (reads("curve")) {
        widgets.push_back(curveLabel_);
        widgets.push_back(curve_);
    }
    return widgets;
}

int DepthEffectCardWidget::widgetHeight(const QWidget* widget, const int width) const {
    if (widget == colorWarning_ || widget == spreadWarning_)
        return static_cast<const QLabel*>(widget)->heightForWidth(width);
    if (widget == pad_)
        return pad_->heightAt(width);
    if (widget == rotation_)
        return rotation_->heightHint();
    if (widget == curve_) {
        const auto hint = curve_->sizeHint();
        return hint.width() > 0 ? width * hint.height() / hint.width() : hint.height();
    }
    for (const auto* row : {nearFarUnit_, nearFarScene_, fogUnit_, fogScene_})
        if (row != nullptr && widget == row)
            return row->heightHint();
    return widget->sizeHint().height();
}

int DepthEffectCardWidget::heightAt(const int width) const {
    int height = 0;
    for (const auto* widget : shownWidgets())
        height += widgetHeight(widget, width) + small();
    return std::max(0, height - small());
}

int DepthEffectCardWidget::minimumWidthHint() const {
    int width = kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
    width = std::max(width, readAsData_->sizeHint().width());
    for (const auto* row : {nearFarUnit_, nearFarScene_, fogUnit_, fogScene_})
        if (row != nullptr)
            width = std::max(width, row->minimumWidthHint());
    if (pad_ != nullptr)
        width = std::max({width, pad_->minimumWidthHint(), pickFocus_->sizeHint().width(),
                          rotation_->minimumWidthHint()});
    if (curve_ != nullptr)
        width = std::max({width, curve_->minimumSizeHint().width(),
                          curveLabel_->sizeHint().width()});
    return width;
}

QSize DepthEffectCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void DepthEffectCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void DepthEffectCardWidget::place() {
    const auto shown = shownWidgets();
    for (auto* widget : std::initializer_list<QWidget*>{
             colorWarning_, readAsData_, spreadWarning_, nearFarUnit_, nearFarScene_, fogUnit_,
             fogScene_, pad_, pickFocus_, rotation_, curveLabel_, curve_})
        if (widget != nullptr)
            widget->setVisible(std::ranges::find(shown, widget) != shown.end());
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = 0;
    for (const auto* row : {nearFarUnit_, nearFarScene_, fogUnit_, fogScene_})
        if (row != nullptr)
            labels = std::max(labels, row->labelWidthHint());
    if (rotation_ != nullptr)
        labels = std::max(labels, rotation_->labelWidthHint());
    for (auto* row : {nearFarUnit_, nearFarScene_, fogUnit_, fogScene_})
        if (row != nullptr)
            row->setLabelWidth(labels);
    if (rotation_ != nullptr)
        rotation_->setLabelWidth(labels);
    int y = 0;
    for (auto* widget : shown) {
        const int h = widgetHeight(widget, width());
        if (widget == readAsData_) {
            widget->setGeometry(0, y, std::min(width(), readAsData_->sizeHint().width()), h);
        } else {
            widget->setGeometry(0, y, width(), h);
        }
        y += h + small();
    }
}

bool DepthEffectCardWidget::readDepthAsData() {
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return false;
    const auto transform =
        document::depthInputColorTransform(session_.snapshot().project(), *composition, node_);
    if (!transform)
        return false;
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> values;
    if (const auto space = nodeRoleParameter(session_, transform->source, "colorSpace"))
        values.emplace_back(*space,
                            static_cast<std::int64_t>(document::AssetColorSpace::Raw));
    if (const auto id = nodeRoleParameter(session_, transform->source,
                                          document::kImageInputColorSpaceIdParameterRole))
        values.emplace_back(*id, std::string{});
    if (values.empty())
        return false;
    return session_.setParameterValues(std::move(values), tr("Read Depth as Data"));
}

void DepthEffectCardWidget::refreshWarnings() {
    QString color;
    QString spread;
    const auto* composition = session_.composition();
    if (composition != nullptr && state_.depthConnected && reads("depthChannel")) {
        if (const auto transform = document::depthInputColorTransform(
                session_.snapshot().project(), *composition, node_)) {
            const auto* source = composition->graph().findNode(transform->source);
            const auto name = source != nullptr ? tr("its image source") : tr("the source");
            color = tr("The depth is read through the %1 colour transform of %2. A depth pass is "
                       "data: a colour transform bends its values. Read the file as data.")
                        .arg(QString::fromStdString(transform->colorSpace), name);
        }
        // A grey depth pass has R = G = B; a picture whose colour channels differ is a beauty or
        // a normal pass connected by mistake. Only the image channels are checked: a channel set
        // is read by name, and alpha is one channel.
        const auto channel = lowered(liveText(session_, node_, "depthChannel"));
        const bool colourChannel = channel.empty() || channel == "r" || channel == "red" ||
                                   channel == "g" || channel == "green" || channel == "b" ||
                                   channel == "blue";
        auto* previews = session_.imagePreviews();
        if (!state_.channelSet && colourChannel && previews != nullptr &&
            depthSourceKind(session_, node_) == document::SocketValueKind::Image) {
            const auto measured = previews->depthChannelSpread(node_);
            if (measured.state == ImagePreviewController::State::Ready &&
                measured.fraction > kSpreadWarningFraction)
                spread = tr("The depth input looks like a colour picture: its red, green and blue "
                            "differ across %1% of the frame. Connect a Z, depth or mist pass, or "
                            "pick the one channel that holds the depth.")
                             .arg(static_cast<int>(std::lround(measured.fraction * 100.0)));
        }
    }
    if (color == colorWarningText_ && spread == spreadWarningText_)
        return;
    colorWarningText_ = color;
    spreadWarningText_ = spread;
    colorWarning_->setText(color);
    spreadWarning_->setText(spread);
    place();
    updateGeometry();
    emit rowsChanged();
}

void DepthEffectCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    const auto state = readState();
    const bool changed = !stateKnown_ || state != state_;
    const auto* nearFarBefore = stateKnown_ ? nearFarRow() : nullptr;
    const auto* fogBefore = stateKnown_ ? fogRangeRow() : nullptr;
    state_ = state;
    stateKnown_ = true;
    for (auto* row : {nearFarUnit_, nearFarScene_, fogUnit_, fogScene_})
        if (row != nullptr)
            row->refresh(locked);
    if (pad_ != nullptr)
        pad_->refresh(locked);
    if (rotation_ != nullptr)
        rotation_->refresh(locked);
    if (pickFocus_ != nullptr)
        pickFocus_->setEnabled(!locked && state_.depthConnected);
    readAsData_->setEnabled(!locked);
    if (curve_ != nullptr) {
        curve_->setEnabled(!locked && curve_->parameter().has_value());
        curve_->update();
    }
    const auto colorBefore = colorWarningText_;
    const auto spreadBefore = spreadWarningText_;
    refreshWarnings();
    const bool warningsChanged =
        colorBefore != colorWarningText_ || spreadBefore != spreadWarningText_;
    if ((changed || nearFarBefore != nearFarRow() || fogBefore != fogRangeRow()) &&
        !warningsChanged) {
        place();
        updateGeometry();
        emit rowsChanged();
    }
}

} // namespace bloom::ui
