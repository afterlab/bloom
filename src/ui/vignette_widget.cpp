#include <bloom/ui/vignette_widget.hpp>

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>

#include <QColor>
#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bloom::ui {
namespace {

constexpr std::string_view kShapeRole = "shape";
constexpr std::string_view kModeRole = "mode";
constexpr std::string_view kCenterRole = "center";
// The falloff picture's samples across; the pad scales it to the frame it draws.
constexpr int kPreviewSamples = 256;

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] const document::NodeDefinition* definition() {
    return document::builtInNodeDefinitions().find(document::kVignetteNodeType, 1);
}

// The node's own default for `role`, the value a reset returns to.
[[nodiscard]] double defaultScalar(const std::string_view role) {
    const auto* found = definition();
    if (found == nullptr)
        return 0.0;
    const auto parameter =
        std::ranges::find(found->parameters, role, &document::ParameterDefinition::role);
    const auto* value = parameter == found->parameters.end()
                            ? nullptr
                            : std::get_if<double>(&parameter->defaultValue);
    return value ? *value : 0.0;
}

// A grey ramp from `left` to `right` (display values), with the neutral grey between.
[[nodiscard]] QGradientStops greys(const double left, const double right) {
    const auto grey = [](const double value) {
        const auto v = static_cast<float>(std::clamp(value, 0.0, 1.0));
        return QColor::fromRgbF(v, v, v);
    };
    return {{0.0, grey(left)}, {0.5, grey(0.45)}, {1.0, grey(right)}};
}

[[nodiscard]] QRectF compositionFrame(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {0.0, 0.0, 1920.0, 1080.0};
    const auto format = composition->format();
    return {0.0, 0.0, static_cast<double>(format.width()), static_cast<double>(format.height())};
}

} // namespace

VignetteCardWidget::VignetteCardWidget(CompositionSession& session, const document::NodeId node,
                                       const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeVignette") : QStringLiteral("propertiesVignette"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    PointPadRow::Spec pad;
    pad.points = {{std::string(kCenterRole), tr("Center"), true}};
    pad.commandLabel = tr("Move Vignette Center");
    pad.frame = [this] { return compositionFrame(session_); };
    pad.backdrop = [this] { return preview(kPreviewSamples); };
    pad_ = new PointPadRow(session_, node_, std::move(pad), this);
    pad_->pad()->setToolTip(tr("Drag the centre to move the vignette; the picture shows where it "
                               "darkens the frame"));

    const auto value = [&](const char* role, const std::string_view schemaKey, QString name,
                           QString description, GradingSliderSpec slider) {
        auto* row = new GradingValueRow(session_, node_, role, schemaKey, std::move(name),
                                        defaultScalar(role), this);
        row->setDescription(description);
        row->setSlider(slider);
        values_.push_back(row);
        return row;
    };
    GradingSliderSpec amount;
    amount.minimum = -1.0;
    amount.maximum = 1.0;
    amount.neutral = 0.0;
    amount.ticks = 0.25;
    amount.gradient = greys(0.9, 0.04);
    value("amount", document::kVignetteAmountSchemaKey, tr("Amount"),
          tr("How much the edges darken; a negative amount brightens them instead."), amount)
        ->setShowsSign(true);
    GradingSliderSpec exposure;
    exposure.minimum = -4.0;
    exposure.maximum = 4.0;
    exposure.neutral = 0.0;
    exposure.ticks = 1.0;
    exposure.gradient = darkToLightStops();
    value("exposure", document::kVignetteExposureSchemaKey, tr("Exposure"),
          tr("The exposure change at the edges, in stops: negative darkens, positive brightens."),
          exposure)
        ->setShowsSign(true);
    const auto share = [](const double neutral, const double ticks) {
        GradingSliderSpec slider;
        slider.minimum = 0.0;
        slider.maximum = 1.0;
        slider.neutral = neutral;
        slider.ticks = ticks;
        slider.gradient = jogStops();
        return slider;
    };
    value("midpoint", document::kVignetteMidpointSchemaKey, tr("Midpoint"),
          tr("How far out the falloff sits: low values reach into the middle of the frame, high "
             "values keep it to the corners."),
          share(defaultScalar("midpoint"), 0.25));
    GradingSliderSpec roundness;
    roundness.minimum = -1.0;
    roundness.maximum = 1.0;
    roundness.neutral = 0.0;
    roundness.ticks = 0.5;
    roundness.gradient = jogStops();
    value("roundness", document::kVignetteRoundnessSchemaKey, tr("Roundness"),
          tr("Above zero the shape becomes a circle (or a square) whatever the frame's aspect; "
             "below zero its corners square off."),
          roundness)
        ->setShowsSign(true);
    value("feather", document::kVignetteFeatherSchemaKey, tr("Feather"),
          tr("How soft the falloff is: zero is a hard edge."),
          share(defaultScalar("feather"), 0.25));
    value("highlightProtection", document::kVignetteHighlightProtectionSchemaKey,
          tr("Highlight protection"),
          tr("How much of their brightness highlights keep: none at mid grey, this share at 1.0 "
             "and above."),
          share(0.0, 0.25));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &VignetteCardWidget::refresh);
    refresh();
}

GradingValueRow* VignetteCardWidget::valueRow(const std::string_view role) const {
    const auto found =
        std::ranges::find_if(values_, [&](const auto* row) { return row->role() == role; });
    return found == values_.end() ? nullptr : *found;
}

std::vector<GradingValueRow*> VignetteCardWidget::shownRows() const {
    std::vector<GradingValueRow*> rows;
    for (auto* row : values_) {
        if (row->role() == (mode_ == document::kVignetteExposure ? "amount" : "exposure"))
            continue;
        rows.push_back(row);
    }
    return rows;
}

QImage VignetteCardWidget::preview(const int width) const {
    const auto scalar = [&](const std::string_view role) {
        const auto id = nodeRoleParameter(session_, node_, role);
        return id ? liveScalar(session_, *id).value_or(defaultScalar(role)) : defaultScalar(role);
    };
    const auto integer = [&](const std::string_view role) -> std::int64_t {
        const auto id = nodeRoleParameter(session_, node_, role);
        const auto live = id ? session_.liveValue(*id) : std::nullopt;
        const auto* stored = live ? std::get_if<std::int64_t>(&*live) : nullptr;
        return stored ? *stored : 0;
    };
    document::Vec2d centre{};
    if (const auto id = nodeRoleParameter(session_, node_, kCenterRole))
        if (const auto live = session_.liveValue(*id))
            if (const auto* stored = std::get_if<document::Vec2d>(&*live))
                centre = *stored;
    const auto frame = compositionFrame(session_);
    double aspect = 1.0;
    if (const auto* composition = session_.composition())
        aspect = static_cast<double>(composition->format().pixelAspect().numerator()) /
                 static_cast<double>(composition->format().pixelAspect().denominator());
    const int w = std::max(1, width);
    const int h = std::max(1, static_cast<int>(std::lround(w * frame.height() / frame.width())));
    const auto falloff = document::vignetteFalloff(integer(kShapeRole), scalar("midpoint"),
                                                   scalar("roundness"), scalar("feather"));
    const auto axes =
        document::vignetteHalfAxes(frame.width() * aspect, frame.height(), falloff.circle);
    const bool exposure = integer(kModeRole) == document::kVignetteExposure;
    const double strength = exposure ? scalar("exposure") : scalar("amount");
    // A light grey frame under the vignette's gain, in display terms, with the line where the
    // vignette reaches half its strength drawn over it, so the shape reads at any strength.
    std::vector<double> masks(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    const double toFrame = frame.width() / w;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double fx = (x + 0.5) * toFrame - (frame.width() / 2.0 + centre.x);
            const double fy = (y + 0.5) * toFrame - (frame.height() / 2.0 + centre.y);
            masks[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                  static_cast<std::size_t>(x)] =
                document::vignetteMask(falloff, fx * aspect / axes.x, fy / axes.y);
        }
    const auto maskAt = [&](const int x, const int y) {
        return masks[static_cast<std::size_t>(std::clamp(y, 0, h - 1)) *
                         static_cast<std::size_t>(w) +
                     static_cast<std::size_t>(std::clamp(x, 0, w - 1))];
    };
    const bool visible = strength != 0.0;
    QImage image(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double mask = maskAt(x, y);
            const double gain = exposure ? std::exp2(strength * mask) : 1.0 - strength * mask;
            const double linear = std::clamp(0.4 * gain, 0.0, 1.0);
            int v = static_cast<int>(std::lround(255.0 * std::pow(linear, 1.0 / 2.2)));
            const bool inside = mask >= 0.5;
            const bool edge = visible && (inside != (maskAt(x + 1, y) >= 0.5) ||
                                          inside != (maskAt(x, y + 1) >= 0.5));
            if (edge)
                v = std::min(255, v + 70);
            line[x] = qRgb(v, v, v);
        }
    }
    return image;
}

bool VignetteCardWidget::showsRole(const std::string_view role) const {
    // The two choices and the centre's X/Y row are the node's own rows; every value is here.
    return role == kShapeRole || role == kModeRole || role == kCenterRole;
}

std::optional<int> VignetteCardWidget::socketY(const std::string_view role) const {
    for (const auto* row : shownRows())
        if (row->role() == role)
            return row->y() + row->socketY();
    return std::nullopt;
}

int VignetteCardWidget::heightAt(const int width) const {
    int height = pad_->heightAt(width) + small();
    for (const auto* row : shownRows())
        height += row->heightHint() + small();
    return std::max(0, height - small());
}

int VignetteCardWidget::minimumWidthHint() const {
    int width = std::max(kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                         pad_->minimumWidthHint());
    // Every row of both modes, so the card keeps its width when the mode changes.
    for (const auto* row : values_)
        width = std::max(width, row->minimumWidthHint());
    return width;
}

QSize VignetteCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void VignetteCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void VignetteCardWidget::place() {
    const auto rows = shownRows();
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = 0;
    for (const auto* row : values_)
        labels = std::max(labels, row->labelWidthHint());
    const int padHeight = pad_->heightAt(width());
    pad_->setGeometry(0, 0, width(), padHeight);
    int y = padHeight + small();
    for (auto* row : values_) {
        row->setLabelWidth(labels);
        row->setVisible(std::ranges::find(rows, row) != rows.end());
    }
    for (auto* row : rows) {
        row->setGeometry(0, y, width(), row->heightHint());
        y += row->heightHint() + small();
    }
}

void VignetteCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    std::int64_t mode = document::kVignetteMultiply;
    if (const auto id = nodeRoleParameter(session_, node_, kModeRole))
        if (const auto value = session_.liveValue(*id))
            if (const auto* stored = std::get_if<std::int64_t>(&*value))
                mode = *stored;
    const bool changed = mode != mode_;
    mode_ = mode;
    for (auto* row : values_)
        row->refresh(locked);
    pad_->refresh(locked);
    if (changed) {
        place();
        updateGeometry();
        emit rowsChanged();
    }
}

} // namespace bloom::ui
