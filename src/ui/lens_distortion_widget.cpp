#include <bloom/ui/lens_distortion_widget.hpp>

#include <bloom/document/lens_distortion_definition.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <string>

namespace bloom::ui {
namespace {

constexpr std::string_view kModeRole = "mode";
constexpr std::string_view kCenterRole = "center";

using document::LensDistortionParameterKind;

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] QString description(const std::string_view role) {
    if (role == "k1")
        return QCoreApplication::translate(
            "LensDistortion", "Radial distortion near the centre: below zero is barrel (straight "
                              "lines bow outward), above zero is pincushion (they bow inward).");
    if (role == "k2")
        return QCoreApplication::translate(
            "LensDistortion", "Radial distortion that grows faster towards the corners, "
                              "refining k1 at the edge of the frame.");
    if (role == "p1" || role == "p2")
        return QCoreApplication::translate(
            "LensDistortion", "Tangential distortion from a lens that is not centred on the "
                              "sensor: it tilts and shears the picture.");
    return {};
}

[[nodiscard]] QRectF compositionFrame(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {0.0, 0.0, 1920.0, 1080.0};
    const auto format = composition->format();
    return {0.0, 0.0, static_cast<double>(format.width()), static_cast<double>(format.height())};
}

} // namespace

LensDistortionCardWidget::LensDistortionCardWidget(CompositionSession& session,
                                                   const document::NodeId node, const bool card,
                                                   QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeLensDistortion")
                       : QStringLiteral("propertiesLensDistortion"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    PointPadRow::Spec pad;
    pad.points = {{std::string(kCenterRole), tr("Center"), true}};
    pad.commandLabel = tr("Move Lens Center");
    pad.frame = [this] { return compositionFrame(session_); };
    pad_ = new PointPadRow(session_, node_, std::move(pad), this);
    pad_->pad()->setToolTip(tr("Drag the point the lens distorts around"));

    for (const auto& spec : document::lensDistortionParameters()) {
        if (spec.kind != LensDistortionParameterKind::Scalar)
            continue;
        auto* row = new GradingValueRow(
            session_, node_, std::string(spec.role), document::lensDistortionSchemaKey(spec.role),
            QCoreApplication::translate("LensDistortion", std::string(spec.label).c_str()),
            spec.value, this);
        row->setDescription(description(spec.role));
        GradingSliderSpec slider;
        slider.minimum = spec.softMinimum;
        slider.maximum = spec.softMaximum;
        slider.neutral = spec.value;
        slider.ticks = (spec.softMaximum - spec.softMinimum) / 4.0;
        slider.gradient = jogStops();
        row->setSlider(slider);
        row->setShowsSign(true);
        values_.push_back(row);
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &LensDistortionCardWidget::refresh);
    refresh();
}

GradingValueRow* LensDistortionCardWidget::valueRow(const std::string_view role) const {
    const auto found =
        std::ranges::find_if(values_, [&](const auto* row) { return row->role() == role; });
    return found == values_.end() ? nullptr : *found;
}

bool LensDistortionCardWidget::showsRole(const std::string_view role) const {
    // The mode and the centre's X/Y row are the node's own rows; every coefficient is here.
    return role == kModeRole || role == kCenterRole;
}

std::optional<int> LensDistortionCardWidget::socketY(const std::string_view role) const {
    if (const auto* row = valueRow(role))
        return row->y() + row->socketY();
    return std::nullopt;
}

int LensDistortionCardWidget::heightAt(const int width) const {
    int height = pad_->heightAt(width) + small();
    for (const auto* row : values_)
        height += row->heightHint() + small();
    return std::max(0, height - small());
}

int LensDistortionCardWidget::minimumWidthHint() const {
    int width = std::max(kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                         pad_->minimumWidthHint());
    for (const auto* row : values_)
        width = std::max(width, row->minimumWidthHint());
    return width;
}

QSize LensDistortionCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void LensDistortionCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void LensDistortionCardWidget::place() {
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = 0;
    for (const auto* row : values_)
        labels = std::max(labels, row->labelWidthHint());
    const int padHeight = pad_->heightAt(width());
    pad_->setGeometry(0, 0, width(), padHeight);
    int y = padHeight + small();
    for (auto* row : values_) {
        row->setLabelWidth(labels);
        row->setGeometry(0, y, width(), row->heightHint());
        y += row->heightHint() + small();
    }
}

void LensDistortionCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    for (auto* row : values_)
        row->refresh(locked);
    pad_->refresh(locked);
}

} // namespace bloom::ui
