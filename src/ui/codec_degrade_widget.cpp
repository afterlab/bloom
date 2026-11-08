#include <bloom/ui/codec_degrade_widget.hpp>

#include <bloom/document/codec_degrade_definition.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <string>

namespace bloom::ui {
namespace {

using document::CodecDegradeParameterKind;

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] QString description(const std::string_view role) {
    if (role == "quality")
        return QCoreApplication::translate("CodecDegrade",
                                           "How finely each block keeps its detail: low is blocky "
                                           "and smeared, 100 is nearly clean.");
    if (role == "persistence")
        return QCoreApplication::translate("CodecDegrade",
                                           "How much of the previous frame's blocks carries into "
                                           "the next, between keyframes. At 100% a block stays "
                                           "until the next keyframe.");
    return {};
}

} // namespace

CodecDegradeCardWidget::CodecDegradeCardWidget(CompositionSession& session,
                                               const document::NodeId node, const bool card,
                                               QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeCodecDegrade")
                       : QStringLiteral("propertiesCodecDegrade"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    for (const auto& spec : document::codecDegradeParameters()) {
        if (spec.kind != CodecDegradeParameterKind::Scalar)
            continue;
        auto* row = new GradingValueRow(
            session_, node_, std::string(spec.role), document::codecDegradeSchemaKey(spec.role),
            QCoreApplication::translate("CodecDegrade", std::string(spec.label).c_str()), spec.value,
            this);
        row->setDescription(description(spec.role));
        GradingSliderSpec slider;
        slider.minimum = spec.softMinimum;
        slider.maximum = spec.softMaximum;
        slider.neutral = spec.value;
        slider.ticks = (spec.softMaximum - spec.softMinimum) / 4.0;
        slider.gradient = jogStops();
        row->setSlider(slider);
        values_.push_back(row);
    }
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &CodecDegradeCardWidget::refresh);
    refresh();
}

GradingValueRow* CodecDegradeCardWidget::valueRow(const std::string_view role) const {
    const auto found =
        std::ranges::find_if(values_, [&](const auto* row) { return row->role() == role; });
    return found == values_.end() ? nullptr : *found;
}

bool CodecDegradeCardWidget::showsRole(const std::string_view role) const {
    // The two scalars are here; the block size, the GOP length and the chroma bleed are the node's own
    // rows.
    return valueRow(role) == nullptr;
}

std::optional<int> CodecDegradeCardWidget::socketY(const std::string_view role) const {
    if (const auto* row = valueRow(role))
        return row->y() + row->socketY();
    return std::nullopt;
}

int CodecDegradeCardWidget::heightAt(const int width) const {
    static_cast<void>(width);
    int height = 0;
    for (const auto* row : values_)
        height += row->heightHint() + small();
    return std::max(0, height - small());
}

int CodecDegradeCardWidget::minimumWidthHint() const {
    int width = kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control);
    for (const auto* row : values_)
        width = std::max(width, row->minimumWidthHint());
    return width;
}

QSize CodecDegradeCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void CodecDegradeCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void CodecDegradeCardWidget::place() {
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = 0;
    for (const auto* row : values_)
        labels = std::max(labels, row->labelWidthHint());
    int y = 0;
    for (auto* row : values_) {
        row->setLabelWidth(labels);
        row->setGeometry(0, y, width(), row->heightHint());
        y += row->heightHint() + small();
    }
}

void CodecDegradeCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    for (auto* row : values_)
        row->refresh(locked);
}

} // namespace bloom::ui
