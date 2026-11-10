#include <bloom/ui/dither_widget.hpp>

#include <bloom/core/dither_pattern.hpp>
#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QCoreApplication>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bloom::ui {
namespace {

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] const document::DepthParameterSpec& amplitudeSpec() {
    for (const auto& spec : document::color_cleanup_parameters::kDitherParameters)
        if (spec.role == "amplitude")
            return spec;
    return document::color_cleanup_parameters::kDitherParameters.front();
}

[[nodiscard]] std::int64_t choice(const CompositionSession& session, const document::NodeId node,
                                  const std::string_view role, const std::int64_t fallback) {
    if (const auto id = nodeRoleParameter(session, node, role))
        if (const auto live = session.liveValue(*id))
            if (const auto* stored = std::get_if<std::int64_t>(&*live))
                return *stored;
    return fallback;
}

[[nodiscard]] bool flag(const CompositionSession& session, const document::NodeId node,
                        const std::string_view role) {
    if (const auto id = nodeRoleParameter(session, node, role))
        if (const auto live = session.liveValue(*id))
            if (const auto* stored = std::get_if<bool>(&*live))
                return *stored;
    return false;
}

[[nodiscard]] double amplitude(const CompositionSession& session, const document::NodeId node) {
    const auto fallback = amplitudeSpec().value[0];
    const auto id = nodeRoleParameter(session, node, "amplitude");
    if (!id)
        return fallback;
    if (const auto live = liveScalar(session, *id))
        return *live;
    return session.effectiveScalarValue(*id).value_or(fallback);
}

} // namespace

// --- The preview --------------------------------------------------------------------------------

DitherPreview::DitherPreview(CompositionSession& session, const document::NodeId node,
                             QWidget* parent)
    : QWidget(parent), session_(session), node_(node) {
    setObjectName(QStringLiteral("ditherPreview"));
    setAccessibleName(tr("Dither preview"));
    setToolTip(tr("A ramp cut to a few levels: without the dither (top) its steps band, and with "
                  "this pattern, seed and amplitude (bottom) they break up into fine noise. The "
                  "node itself quantizes nothing"));
}

int DitherPreview::fixedHeight() {
    // commands::estimatedNodeCardSize holds the same height.
    return 48;
}

QImage DitherPreview::preview(const int width, const int height) const {
    const int w = std::max(1, width);
    const int h = std::max(2, height);
    const bool blueNoise =
        choice(session_, node_, "pattern", document::kDitherBlueNoise) == document::kDitherBlueNoise;
    const int side =
        blueNoise ? core::dither::kBlueNoiseSide
                  : document::ditherBayerSide(choice(session_, node_, "matrixSize", 2));
    std::vector<std::uint16_t> bayer;
    std::span<const std::uint16_t> ranks;
    if (blueNoise) {
        ranks = core::dither::blueNoiseTable();
    } else {
        bayer = core::dither::bayerMatrix(side > 0 ? side : 8);
        ranks = bayer;
    }
    const int n = blueNoise ? core::dither::kBlueNoiseSide : (side > 0 ? side : 8);
    const auto cells = static_cast<std::uint32_t>(n * n);
    const std::int64_t seed = choice(session_, node_, "seed", 1);
    const bool monochrome = flag(session_, node_, "monochrome");
    std::array<core::dither::Offset, 3> offsets{};
    for (int channel = 0; channel < 3; ++channel)
        offsets[static_cast<std::size_t>(channel)] =
            core::dither::offset(seed, 0, monochrome ? 0 : channel, n);
    // The amplitude in LSBs of the preview's few levels: the same share of a step as the node's of
    // its own bit depth.
    const double amount = amplitude(session_, node_);
    const int half = h / 2;
    QImage image(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        const bool dithered = y >= half;
        for (int x = 0; x < w; ++x) {
            // The ramp in display-encoded values, one preview pixel for one image pixel.
            const double value = (x + 0.5) / w * kPreviewSteps;
            std::array<int, 3> rgb{};
            for (std::size_t c = 0; c < 3; ++c) {
                double moved = value;
                if (dithered) {
                    const auto& o = offsets[c];
                    const auto cx = static_cast<std::uint32_t>(x + static_cast<int>(o.x)) &
                                    static_cast<std::uint32_t>(n - 1);
                    const auto cy = static_cast<std::uint32_t>(y + static_cast<int>(o.y)) &
                                    static_cast<std::uint32_t>(n - 1);
                    const auto rank = (ranks[cy * static_cast<std::uint32_t>(n) + cx] + o.rank) &
                                      (cells - 1U);
                    const double threshold = (static_cast<double>(rank) + 0.5) / cells - 0.5;
                    moved += threshold * amount;
                }
                const double level = std::clamp(std::round(moved), 0.0, double{kPreviewSteps});
                rgb[c] = static_cast<int>(std::lround(255.0 * level / kPreviewSteps));
            }
            line[x] = qRgb(rgb[0], rgb[1], rgb[2]);
        }
    }
    return image;
}

void DitherPreview::paintEvent(QPaintEvent* event) {
    static_cast<void>(event);
    QPainter painter(this);
    const auto ratio = devicePixelRatioF();
    const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    auto picture = preview(static_cast<int>(std::lround(area.width() * ratio)),
                           static_cast<int>(std::lround(area.height() * ratio)));
    picture.setDevicePixelRatio(ratio);
    painter.drawImage(area.topLeft(), picture);
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area);
}

// --- The card -----------------------------------------------------------------------------------

DitherCardWidget::DitherCardWidget(CompositionSession& session, const document::NodeId node,
                                   const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeDither") : QStringLiteral("propertiesDither"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    preview_ = new DitherPreview(session_, node_, this);
    const auto& spec = amplitudeSpec();
    amplitude_ = new GradingValueRow(
        session_, node_, std::string(spec.role),
        document::colorCleanupSchemaKey(document::kDitherNodeType, spec.role),
        QCoreApplication::translate("Dither", std::string(spec.label).c_str()), spec.value[0], this);
    amplitude_->setDescription(
        tr("How far the pattern moves a value, peak to peak, in least significant bits of the "
           "target bit depth: 1 LSB just breaks up the banding of a quantization to that depth"));
    GradingSliderSpec slider;
    slider.minimum = spec.softMinimum;
    slider.maximum = spec.softMaximum;
    slider.neutral = spec.value[0];
    slider.ticks = 1.0;
    slider.gradient = jogStops();
    amplitude_->setSlider(slider);
    readout_ = new kit::KLabel(this);
    readout_->setObjectName(QStringLiteral("ditherReadout"));
    readout_->setTone(kit::Color::Muted);
    readout_->setWordWrap(true);
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &DitherCardWidget::refresh);
    refresh();
    place();
}

QString DitherCardWidget::readout() const {
    const double lsb = amplitude(session_, node_);
    const int bits = document::ditherBits(choice(session_, node_, "bitDepth", 0));
    const double share = bits > 0 ? lsb / (std::ldexp(1.0, bits) - 1.0) : 0.0;
    const bool stored =
        choice(session_, node_, "domain", document::kDitherDisplay) == document::kDitherStored;
    const auto text = stored ? tr("%1 LSB of %2-bit = %3 of full scale, in the stored value")
                             : tr("%1 LSB of %2-bit = %3 of full scale, display encoded");
    return text.arg(QString::number(lsb, 'f', 2))
        .arg(bits)
        .arg(QString::number(share, 'g', 3));
}

bool DitherCardWidget::showsRole(const std::string_view role) const {
    // The pattern, bit depth, domain, animation, seed and Monochrome are the node's own rows, and
    // the matrix size while the Ordered pattern reads it; the amplitude is here.
    if (role == "matrixSize")
        return ordered_;
    return role != "amplitude";
}

std::optional<int> DitherCardWidget::socketY(const std::string_view role) const {
    if (role == "amplitude")
        return amplitude_->y() + amplitude_->socketY();
    return std::nullopt;
}

int DitherCardWidget::heightAt(const int width) const {
    return DitherPreview::fixedHeight() + small() + amplitude_->heightHint() + small() +
           readout_->heightForWidth(std::max(1, width));
}

int DitherCardWidget::minimumWidthHint() const {
    return std::max(kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                    amplitude_->minimumWidthHint());
}

QSize DitherCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void DitherCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void DitherCardWidget::place() {
    preview_->setGeometry(0, 0, width(), DitherPreview::fixedHeight());
    int y = preview_->height() + small();
    amplitude_->setLabelWidth(amplitude_->labelWidthHint());
    amplitude_->setGeometry(0, y, width(), amplitude_->heightHint());
    y += amplitude_->heightHint() + small();
    readout_->setGeometry(0, y, width(), readout_->heightForWidth(std::max(1, width())));
}

void DitherCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    amplitude_->refresh(locked);
    // The matrix size's row comes and goes with the pattern that reads it.
    const bool ordered =
        choice(session_, node_, "pattern", document::kDitherBlueNoise) == document::kDitherOrdered;
    if (ordered != ordered_) {
        ordered_ = ordered;
        emit rowsChanged();
    }
    const auto text = readout();
    if (readout_->text() != text) {
        const int before = readout_->heightForWidth(std::max(1, width()));
        readout_->setText(text);
        if (readout_->heightForWidth(std::max(1, width())) != before) {
            place();
            updateGeometry();
            emit rowsChanged();
        }
    }
    preview_->update();
}

} // namespace bloom::ui
