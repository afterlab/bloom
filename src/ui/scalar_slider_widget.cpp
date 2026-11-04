#include <bloom/ui/scalar_slider_widget.hpp>

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include "properties_registry_row.hpp"

namespace bloom::ui {
namespace {

constexpr std::string_view kAmountRole = "amount";

struct ScalarSlider final {
    std::string_view schemaKey;
    double neutral;
    double minimum;
    double maximum;
    double ticks;
    bool signedValue;
};

[[nodiscard]] ScalarSlider sliderFor(const std::string_view typeId) {
    if (typeId == "bloom.exposure")
        return {"bloom.exposure.stops", 0.0, -4.0, 4.0, 1.0, true};
    if (typeId == "bloom.offset")
        return {"bloom.offset.amount", 0.0, -1.0, 1.0, 0.25, true};
    return {"bloom.multiply.factor", 1.0, 0.0, 2.0, 0.25, false};
}

} // namespace

bool isScalarSliderNodeType(const std::string_view typeId) noexcept {
    return typeId == "bloom.exposure" || typeId == "bloom.offset" || typeId == "bloom.multiply";
}

ScalarSliderCardWidget::ScalarSliderCardWidget(CompositionSession& session,
                                               const document::NodeId node,
                                               const std::string_view typeId, const bool card,
                                               QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeScalarSlider")
                       : QStringLiteral("propertiesScalarSlider"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    const auto spec = sliderFor(typeId);
    row_ = new GradingValueRow(session_, node_, std::string(kAmountRole), spec.schemaKey,
                               parameterRowLabel(spec.schemaKey, kAmountRole), spec.neutral,
                               this);
    row_->setShowsSign(spec.signedValue);
    GradingSliderSpec slider;
    slider.minimum = spec.minimum;
    slider.maximum = spec.maximum;
    slider.neutral = spec.neutral;
    slider.ticks = spec.ticks;
    slider.gradient = darkToLightStops();
    row_->setSlider(slider);
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &ScalarSliderCardWidget::refresh);
    refresh();
}

bool ScalarSliderCardWidget::showsRole(const std::string_view role) const {
    return role != kAmountRole;
}

std::optional<int> ScalarSliderCardWidget::socketY(const std::string_view role) const {
    if (role != kAmountRole)
        return std::nullopt;
    return row_->socketY();
}

int ScalarSliderCardWidget::heightAt(const int) const { return row_->heightHint(); }

int ScalarSliderCardWidget::minimumWidthHint() const { return row_->minimumWidthHint(); }

QSize ScalarSliderCardWidget::sizeHint() const {
    const int width = std::max(minimumWidthHint(), kit::px(kit::Size::DropdownWidthExpanded) -
                                                       kit::px(kit::Size::Control));
    return {width, heightAt(width)};
}

void ScalarSliderCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    row_->setGeometry(0, 0, width(), row_->heightHint());
}

void ScalarSliderCardWidget::refresh() {
    const auto* composition = session_.composition();
    row_->refresh(composition == nullptr || composition->nodeLocked(node_));
}

} // namespace bloom::ui
