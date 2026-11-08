#include <bloom/ui/light_rays_widget.hpp>

#include <bloom/document/light_rays_nodes.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/colorize_widget.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <string>

namespace bloom::ui {
namespace {

constexpr std::string_view kSourceRole = "source";
constexpr std::string_view kCenterRole = "center";
constexpr std::string_view kTintRole = "tint";

struct SliderSpec final {
    std::string_view role;
    const char* label;
    const char* description;
    std::string_view schemaKey;
    double value;
    double minimum;
    double maximum;
};

// The three slider rows, in card order.
constexpr std::array kSliders{
    SliderSpec{"length", "Length",
               "How far each ray reaches towards the center, as a percentage of the pixel's "
               "distance to it. Zero adds no rays.",
               document::kLightRaysLengthSchemaKey, document::kLightRaysLengthDefault,
               document::kMinimumLightRaysLength, document::kMaximumLightRaysLength},
    SliderSpec{"falloff", "Falloff",
               "How quickly a ray fades along its length. Zero keeps it even; higher values "
               "fade it faster.",
               document::kLightRaysFalloffSchemaKey, document::kLightRaysFalloffDefault,
               document::kMinimumLightRaysFalloff, document::kLightRaysFalloffSoftMaximum},
    SliderSpec{"boost", "Boost",
               "A multiplier on the rays' brightness. Zero adds no rays.",
               document::kLightRaysBoostSchemaKey, document::kLightRaysBoostDefault,
               document::kMinimumLightRaysBoost, document::kLightRaysBoostSoftMaximum},
};

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] int colorRowHeight() { return kit::px(kit::Size::Control); }

[[nodiscard]] QRectF compositionFrame(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {0.0, 0.0, 1920.0, 1080.0};
    const auto format = composition->format();
    return {0.0, 0.0, static_cast<double>(format.width()), static_cast<double>(format.height())};
}

} // namespace

LightRaysCardWidget::LightRaysCardWidget(CompositionSession& session,
                                                   const document::NodeId node, const bool card,
                                                   QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeLightRays")
                       : QStringLiteral("propertiesLightRays"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    PointPadRow::Spec pad;
    pad.points = {{std::string(kCenterRole), tr("Center"), true}};
    pad.commandLabel = tr("Move Light Rays Center");
    pad.frame = [this] { return compositionFrame(session_); };
    pad_ = new PointPadRow(session_, node_, std::move(pad), this);
    pad_->pad()->setToolTip(tr("Drag the point the rays radiate from"));

    for (const auto& spec : kSliders) {
        auto* row = new GradingValueRow(
            session_, node_, std::string(spec.role), std::string(spec.schemaKey),
            QCoreApplication::translate("LightRays", spec.label), spec.value, this);
        row->setDescription(QCoreApplication::translate("LightRays", spec.description));
        GradingSliderSpec slider;
        slider.minimum = spec.minimum;
        slider.maximum = spec.maximum;
        slider.neutral = spec.value;
        slider.ticks = (spec.maximum - spec.minimum) / 4.0;
        slider.gradient = jogStops();
        row->setSlider(slider);
        values_.push_back(row);
    }
    color_ = new ColorizeColorRow(session_, node_, std::string(kTintRole),
                                  document::kLightRaysColorSchemaKey, tr("Tint"), this);
    color_->setDescription(tr("The tint of the rays. White leaves them the color of what they "
                              "come from."));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &LightRaysCardWidget::refresh);
    refresh();
}

GradingValueRow* LightRaysCardWidget::valueRow(const std::string_view role) const {
    const auto found =
        std::ranges::find_if(values_, [&](const auto* row) { return row->role() == role; });
    return found == values_.end() ? nullptr : *found;
}

bool LightRaysCardWidget::showsRole(const std::string_view role) const {
    // The source and the center's X/Y row are the node's own rows; the sliders and the color are
    // here.
    return role == kSourceRole || role == kCenterRole;
}

std::optional<int> LightRaysCardWidget::socketY(const std::string_view role) const {
    if (const auto* row = valueRow(role))
        return row->y() + row->socketY();
    return std::nullopt;
}

int LightRaysCardWidget::heightAt(const int width) const {
    int height = pad_->heightAt(width) + small();
    for (const auto* row : values_)
        height += row->heightHint() + small();
    return height + colorRowHeight();
}

int LightRaysCardWidget::minimumWidthHint() const {
    int width = std::max(kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                         pad_->minimumWidthHint());
    for (const auto* row : values_)
        width = std::max(width, row->minimumWidthHint());
    return std::max(width, color_->minimumWidthHint());
}

QSize LightRaysCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void LightRaysCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void LightRaysCardWidget::place() {
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = color_->labelWidthHint();
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
    color_->setLabelWidth(labels);
    color_->setGeometry(0, y, width(), colorRowHeight());
}

void LightRaysCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    for (auto* row : values_)
        row->refresh(locked);
    color_->refresh(locked);
    pad_->refresh(locked);
}

} // namespace bloom::ui
