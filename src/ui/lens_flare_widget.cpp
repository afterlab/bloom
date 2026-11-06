#include <bloom/ui/lens_flare_widget.hpp>

#include <bloom/document/image_preview.hpp>
#include <bloom/document/lens_flare_definition.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/colorize_widget.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>

#include <QAction>
#include <QColor>
#include <QCoreApplication>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <utility>

namespace bloom::ui {
namespace {

using namespace std::string_view_literals;
using document::LensFlareGroup;
using document::LensFlareParameterKind;
using document::LensFlareParameterSpec;

// The picture's samples across; the pad scales it to the frame it draws.
constexpr int kPreviewSamples = 256;
// The values always on the card: the whole flare's.
constexpr std::array kAlwaysRoles{"intensity"sv, "flareSize"sv, "chromatic"sv};

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }
[[nodiscard]] int colorRowHeight() { return kit::px(kit::Size::Control); }

[[nodiscard]] const LensFlareParameterSpec& specOf(const std::string_view role) {
    for (const auto& spec : document::lensFlareParameters())
        if (spec.role == role)
            return spec;
    return document::lensFlareParameters().front();
}

[[nodiscard]] QRectF compositionFrame(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {0.0, 0.0, 1920.0, 1080.0};
    const auto format = composition->format();
    return {0.0, 0.0, static_cast<double>(format.width()), static_cast<double>(format.height())};
}

[[nodiscard]] QString description(const std::string_view role) {
    static const std::array<std::pair<std::string_view, const char*>, 26> kText{{
        {"intensity", QT_TRANSLATE_NOOP("LensFlare", "How bright the whole flare is.")},
        {"flareSize", QT_TRANSLATE_NOOP("LensFlare", "How large the whole flare is.")},
        {"chromatic",
         QT_TRANSLATE_NOOP("LensFlare",
                           "How far the ghosts, the iris, the ring and the hoop split into red and "
                           "blue edges.")},
        {"lightDistance",
         QT_TRANSLATE_NOOP("LensFlare",
                           "How far behind the frame the light is; a nearer light moves away from "
                           "the centre. Behind the camera there is no flare.")},
        {"cameraZoom",
         QT_TRANSLATE_NOOP("LensFlare", "The camera's zoom in pixels: how strong the perspective is.")},
        {"sourceBrightness",
         QT_TRANSLATE_NOOP("LensFlare",
                           "How much the flare follows the input's brightness at the light: at "
                           "100% a dark spot under the light gives no flare.")},
        {"sourceWhite",
         QT_TRANSLATE_NOOP("LensFlare",
                           "The input's brightness at the light that gives the full flare.")},
        {"occlusionAmount",
         QT_TRANSLATE_NOOP("LensFlare",
                           "How much the occlusion matte hides the flare where it covers the "
                           "light.")},
        {"sampleRadius",
         QT_TRANSLATE_NOOP("LensFlare",
                           "The disc around the light that the source and the matte are read over: "
                           "wider fades the flare more gently as the light passes behind an "
                           "edge.")},
        {"glowAmount", QT_TRANSLATE_NOOP("LensFlare", "The bloom around the light.")},
        {"glowSize", QT_TRANSLATE_NOOP("LensFlare", "How far the bloom reaches.")},
        {"streakAmount", QT_TRANSLATE_NOOP("LensFlare", "The needles through the light.")},
        {"streakLength", QT_TRANSLATE_NOOP("LensFlare", "How long the needles are.")},
        {"streakWidth", QT_TRANSLATE_NOOP("LensFlare", "How wide the needles are at the light.")},
        {"ghostAmount",
         QT_TRANSLATE_NOOP("LensFlare", "The soft discs on the line through the frame's centre.")},
        {"ghostSize", QT_TRANSLATE_NOOP("LensFlare", "How large the discs are.")},
        {"ghostSpread", QT_TRANSLATE_NOOP("LensFlare", "How far along the line the discs spread.")},
        {"ringAmount", QT_TRANSLATE_NOOP("LensFlare", "The thin ring around the light.")},
        {"ringWidth", QT_TRANSLATE_NOOP("LensFlare", "How wide the ring is.")},
        {"irisAmount",
         QT_TRANSLATE_NOOP("LensFlare", "The aperture-shaped ghosts on the same line.")},
        {"irisSpread", QT_TRANSLATE_NOOP("LensFlare", "How far along the line they spread.")},
        {"hoopAmount",
         QT_TRANSLATE_NOOP("LensFlare", "The wide arc around the frame, away from the light.")},
        {"hoopWidth", QT_TRANSLATE_NOOP("LensFlare", "How wide the arc is.")},
        {"anamorphicAmount",
         QT_TRANSLATE_NOOP("LensFlare", "The one long thin streak in its own colour.")},
        {"anamorphicLength", QT_TRANSLATE_NOOP("LensFlare", "How long the streak is.")},
        {"anamorphicWidth", QT_TRANSLATE_NOOP("LensFlare", "How thin the streak is.")},
    }};
    for (const auto& [name, text] : kText)
        if (name == role)
            return QCoreApplication::translate("LensFlare", text);
    return {};
}

[[nodiscard]] QString groupName(const LensFlareGroup group) {
    switch (group) {
    case LensFlareGroup::Light:
        return QCoreApplication::translate("LensFlare", "Light source");
    case LensFlareGroup::Glow:
        return QCoreApplication::translate("LensFlare", "Glow");
    case LensFlareGroup::Streaks:
        return QCoreApplication::translate("LensFlare", "Streaks");
    case LensFlareGroup::Ghosts:
        return QCoreApplication::translate("LensFlare", "Ghosts");
    case LensFlareGroup::Rings:
        return QCoreApplication::translate("LensFlare", "Ring");
    case LensFlareGroup::Iris:
        return QCoreApplication::translate("LensFlare", "Iris");
    case LensFlareGroup::Hoop:
        return QCoreApplication::translate("LensFlare", "Hoop");
    case LensFlareGroup::Anamorphic:
        return QCoreApplication::translate("LensFlare", "Anamorphic");
    }
    return {};
}

} // namespace

LensFlareCardWidget::LensFlareCardWidget(CompositionSession& session, const document::NodeId node,
                                         const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeLensFlare") : QStringLiteral("propertiesLensFlare"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    presets_ = new kit::KButton(kit::IconId::CaretDown, tr("Preset"), this);
    presets_->setObjectName(QStringLiteral("lensFlarePresets"));
    presets_->setControlSize(kit::KButton::ControlSize::Compact);
    presets_->setAccessibleName(tr("Lens Flare preset"));
    presets_->setToolTip(tr("Start from a look: sets the element stack, and leaves the light's "
                            "position, its intensity and its readings alone"));
    connect(presets_, &QAbstractButton::clicked, this, [this] {
        const std::unique_ptr<QMenu> menu(makePresetMenu());
        menu->exec(presets_->mapToGlobal(QPoint(0, presets_->height())));
    });

    PointPadRow::Spec pad;
    pad.points = {{"lightPosition", tr("Light"), true}};
    pad.commandLabel = tr("Move Lens Flare Light");
    pad.frame = [this] { return compositionFrame(session_); };
    pad.backdrop = [this] { return preview(kPreviewSamples); };
    pad_ = new PointPadRow(session_, node_, std::move(pad), this);
    pad_->pad()->setToolTip(tr("Drag the light; the picture shows where the elements land"));

    rotation_ = new AngleDialRow(
        session_, node_,
        AngleDialRow::Spec{"flareRotation", document::lensFlareSchemaKey("flareRotation"), true, false, false,
                           0.0},
        this);

    element_ = new kit::KDropdown(this);
    element_->setObjectName(QStringLiteral("lensFlareElement"));
    element_->setAccessibleName(tr("Element"));
    element_->setToolTip(tr("The part of the flare shown below"));
    for (std::size_t index = 0; index < document::kLensFlareGroupCount; ++index)
        element_->addItem(groupName(static_cast<LensFlareGroup>(index)), static_cast<int>(index));
    element_->setCurrentIndex(0);
    connect(element_, &kit::KDropdown::activated, this,
            [this](const int index) { setGroup(static_cast<LensFlareGroup>(index)); });

    const auto makeRow = [this](const LensFlareParameterSpec& spec) {
        const std::string role(spec.role);
        auto* row = new GradingValueRow(session_, node_, role, document::lensFlareSchemaKey(role),
                                        tr(std::string(spec.label).c_str()), spec.value[0], this);
        row->setDescription(description(spec.role));
        GradingSliderSpec slider;
        slider.minimum = spec.softMinimum;
        slider.maximum = spec.softMaximum;
        slider.neutral = spec.value[0];
        slider.gradient = jogStops();
        row->setSlider(slider);
        return row;
    };
    for (const auto role : kAlwaysRoles)
        always_.push_back(makeRow(specOf(role)));
    for (const auto& spec : document::lensFlareParameters()) {
        if (spec.kind != LensFlareParameterKind::Scalar || spec.role == "flareRotation"sv ||
            std::ranges::find(kAlwaysRoles, spec.role) != kAlwaysRoles.end())
            continue;
        values_.push_back(makeRow(spec));
    }
    const auto makeColor = [this](const std::string_view role) {
        const auto& spec = specOf(role);
        return new ColorizeColorRow(session_, node_, std::string(role),
                                    document::lensFlareSchemaKey(std::string(role)),
                                    tr(std::string(spec.label).c_str()), this);
    };
    lightColor_ = makeColor("lightColor");
    lightColor_->setDescription(tr("The colour of the whole flare's light."));
    anamorphicColor_ = makeColor("anamorphicColor");
    anamorphicColor_->setDescription(tr("The colour of the anamorphic streak."));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &LensFlareCardWidget::refresh);
    refresh();
}

GradingValueRow* LensFlareCardWidget::valueRow(const std::string_view role) const {
    for (const auto* rows : {&always_, &values_})
        for (auto* row : *rows)
            if (row->role() == role)
                return row;
    return nullptr;
}

ColorizeColorRow* LensFlareCardWidget::colorRow(const std::string_view role) const {
    if (role == "lightColor"sv)
        return lightColor_;
    return role == "anamorphicColor"sv ? anamorphicColor_ : nullptr;
}

void LensFlareCardWidget::setGroup(const LensFlareGroup group) {
    if (group == group_)
        return;
    group_ = group;
    element_->setCurrentIndex(static_cast<int>(group));
    place();
    updateGeometry();
    emit rowsChanged();
}

LensFlareCardWidget::State LensFlareCardWidget::readState() const {
    State state;
    if (const auto id = nodeRoleParameter(session_, node_, "lightMode"))
        if (const auto value = session_.liveValue(*id))
            if (const auto* stored = std::get_if<std::int64_t>(&*value))
                state.lightMode = *stored;
    if (const auto* composition = session_.composition())
        state.occlusionConnected =
            document::inspectedInputSource(composition->graph(), node_,
                                           document::kLensFlareOcclusionPort)
                .has_value();
    return state;
}

bool LensFlareCardWidget::shown(const LensFlareParameterSpec& spec) const {
    if (spec.group != group_)
        return false;
    if (spec.role == "lightDistance"sv || spec.role == "cameraZoom"sv)
        return state_.lightMode == document::kLensFlareLight3D;
    if (spec.role == "occlusionAmount"sv || spec.role == "occlusionChannel"sv)
        return state_.occlusionConnected;
    return true;
}

std::vector<GradingValueRow*> LensFlareCardWidget::shownRows() const {
    std::vector<GradingValueRow*> rows = always_;
    for (auto* row : values_)
        if (shown(specOf(row->role())))
            rows.push_back(row);
    return rows;
}

QMenu* LensFlareCardWidget::makePresetMenu() {
    auto* menu = kit::makeMenu(this);
    const auto& presets = document::lensFlarePresets();
    for (std::size_t index = 0; index < presets.size(); ++index) {
        auto* action = menu->addAction(tr(std::string(presets[index].name).c_str()));
        action->setObjectName(QStringLiteral("lensFlarePreset.%1").arg(index));
        connect(action, &QAction::triggered, this,
                [this, index] { static_cast<void>(applyPreset(index)); });
    }
    return menu;
}

bool LensFlareCardWidget::applyPreset(const std::size_t index) {
    const auto& presets = document::lensFlarePresets();
    if (index >= presets.size())
        return false;
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> values;
    // A driven value keeps its driver; a keyed one takes a key at the playhead.
    for (const auto& item : presets[index].values)
        if (const auto id = nodeRoleParameter(session_, node_, item.role);
            id && scalarEditable(session_, *id))
            values.emplace_back(*id, item.value);
    return session_.setParameterValues(
        std::move(values),
        tr("Lens Flare Preset %1").arg(tr(std::string(presets[index].name).c_str())));
}

QImage LensFlareCardWidget::preview(const int width) const {
    const auto scalar = [&](const std::string_view role) {
        const auto id = nodeRoleParameter(session_, node_, role);
        const auto fallback = specOf(role).value[0];
        return id ? liveScalar(session_, *id).value_or(fallback) : fallback;
    };
    const auto integer = [&](const std::string_view role) {
        const auto id = nodeRoleParameter(session_, node_, role);
        const auto live = id ? session_.liveValue(*id) : std::nullopt;
        const auto* stored = live ? std::get_if<std::int64_t>(&*live) : nullptr;
        return stored ? *stored : static_cast<std::int64_t>(specOf(role).value[0]);
    };
    const auto colour = [&](const std::string_view role, const double gain) {
        const auto id = nodeRoleParameter(session_, node_, role);
        const auto live = id ? session_.liveValue(*id) : std::nullopt;
        const auto* stored = live ? std::get_if<core::Color4d>(&*live) : nullptr;
        const auto& fallback = specOf(role).value;
        const std::array<double, 3> rgb{stored ? stored->red : fallback[0],
                                        stored ? stored->green : fallback[1],
                                        stored ? stored->blue : fallback[2]};
        const auto channel = [gain](const double value) {
            return static_cast<float>(std::clamp(value * gain, 0.0, 1.0));
        };
        return QColor::fromRgbF(channel(rgb[0]), channel(rgb[1]), channel(rgb[2]));
    };
    document::Vec2d position{};
    if (const auto id = nodeRoleParameter(session_, node_, "lightPosition"))
        if (const auto live = session_.liveValue(*id))
            if (const auto* stored = std::get_if<document::Vec2d>(&*live))
                position = *stored;
    const auto frame = compositionFrame(session_);
    double aspect = 1.0;
    if (const auto* composition = session_.composition())
        aspect = static_cast<double>(composition->format().pixelAspect().numerator()) /
                 static_cast<double>(composition->format().pixelAspect().denominator());
    const int w = std::max(1, width);
    const int h = std::max(1, static_cast<int>(std::lround(w * frame.height() / frame.width())));
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(18, 18, 20));
    double projection = 1.0;
    const bool visible =
        integer("lightMode") != document::kLensFlareLight3D ||
        document::lensFlareProjection(scalar("cameraZoom"), scalar("lightDistance"), projection);
    if (!visible)
        return image;

    document::LensFlareLayoutInput input;
    input.lightX = position.x * projection * aspect / frame.height();
    input.lightY = position.y * projection / frame.height();
    input.size = scalar("flareSize");
    input.rotationDegrees = scalar("flareRotation");
    input.seed = integer("seed");
    input.streakCount = integer("streakCount");
    input.streakLength = scalar("streakLength") / 100.0;
    input.ghostCount = integer("ghostCount");
    input.ghostSize = scalar("ghostSize") / 100.0;
    input.ghostSpread = scalar("ghostSpread");
    input.irisCount = integer("irisCount");
    input.irisBlades = integer("irisBlades");
    input.irisSize = scalar("irisSize") / 100.0;
    input.irisSpread = scalar("irisSpread");
    const auto layout = document::lensFlareLayout(input);

    // The picture is the flare's plan rather than its pixels: each element where it lands, at its
    // size, as bright as a display can show it.
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    const double perHeight = static_cast<double>(h);
    const QPointF centre(w / 2.0, h / 2.0);
    const auto at = [&](const double x, const double y) {
        return centre + QPointF(x / aspect * perHeight, y * perHeight);
    };
    const QPointF light = at(input.lightX, input.lightY);
    const double level = std::min(scalar("intensity"), 2.0);
    const auto tinted = [&](const std::array<double, 3>& tint, const double amount) {
        const auto base = colour("lightColor", 1.0);
        const auto channel = [&](const double value, const double t) {
            return static_cast<float>(std::clamp(value * t * amount * level, 0.0, 1.0));
        };
        return QColor::fromRgbF(channel(static_cast<double>(base.redF()), tint[0]),
                                channel(static_cast<double>(base.greenF()), tint[1]),
                                channel(static_cast<double>(base.blueF()), tint[2]));
    };
    constexpr std::array<double, 3> kWhite{1.0, 1.0, 1.0};
    // The hoop, on the side away from the light.
    if (const double amount = scalar("hoopAmount"); amount > 0.0) {
        const double radius = input.size * scalar("hoopSize") / 100.0 * perHeight;
        const double away =
            std::atan2(-(light.y() - centre.y()), -(light.x() - centre.x())) * 180.0 / std::numbers::pi;
        QPen pen(tinted(kWhite, std::min(amount * 2.0, 1.0)));
        pen.setWidthF(std::max(1.0, input.size * scalar("hoopWidth") / 100.0 * perHeight));
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(centre.x() - radius, centre.y() - radius, 2.0 * radius, 2.0 * radius),
                        static_cast<int>(std::lround((-away - 70.0) * 16.0)), 140 * 16);
    }
    // The glow.
    if (const double amount = scalar("glowAmount"); amount > 0.0) {
        const double radius = 3.0 * input.size * scalar("glowSize") / 100.0 * perHeight;
        QRadialGradient gradient(light, std::max(radius, 1.0));
        gradient.setColorAt(0.0, tinted(kWhite, std::min(amount, 1.0)));
        gradient.setColorAt(0.33, tinted(kWhite, std::min(amount, 1.0) * 0.25));
        gradient.setColorAt(1.0, QColor(0, 0, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        painter.drawEllipse(light, radius, radius);
    }
    // The ring.
    if (const double amount = scalar("ringAmount"); amount > 0.0) {
        const double radius = input.size * scalar("ringSize") / 100.0 * perHeight;
        QPen pen(tinted(kWhite, std::min(amount * 2.0, 1.0)));
        pen.setWidthF(std::max(1.0, input.size * scalar("ringWidth") / 100.0 * perHeight));
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(light, radius, radius);
    }
    // The ghosts.
    if (const double amount = scalar("ghostAmount"); amount > 0.0) {
        painter.setPen(Qt::NoPen);
        for (const auto& ghost : layout.ghosts) {
            painter.setBrush(tinted(ghost.tint, std::min(amount, 1.0)));
            const double radius = ghost.radius * perHeight;
            painter.drawEllipse(at(ghost.x, ghost.y), radius, radius);
        }
    }
    // The iris polygons.
    if (const double amount = scalar("irisAmount"); amount > 0.0) {
        painter.setPen(Qt::NoPen);
        for (const auto& iris : layout.irises) {
            QPainterPath path;
            const double radius = iris.radius * perHeight;
            const auto middle = at(iris.x, iris.y);
            const double corner =
                1.0 / std::cos(std::numbers::pi / static_cast<double>(layout.irisNormals.size()));
            const double half = std::numbers::pi / static_cast<double>(layout.irisNormals.size());
            for (std::size_t edge = 0; edge < layout.irisNormals.size(); ++edge) {
                const double angle = std::atan2(layout.irisNormals[edge][1],
                                                layout.irisNormals[edge][0]) + half;
                const QPointF point =
                    middle + QPointF(std::cos(angle), std::sin(angle)) * (radius * corner);
                if (edge == 0)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            path.closeSubpath();
            painter.setBrush(tinted(iris.tint, std::min(amount, 1.0)));
            painter.drawPath(path);
        }
    }
    // The streaks.
    if (const double amount = scalar("streakAmount"); amount > 0.0) {
        QPen pen(tinted(kWhite, std::min(amount * 1.5, 1.0)));
        pen.setWidthF(1.0);
        painter.setPen(pen);
        for (const auto& line : layout.streaks) {
            const QPointF reach =
                QPointF(line.directionX, line.directionY) * (line.length * perHeight);
            painter.drawLine(light - reach, light + reach);
        }
    }
    // The anamorphic streak.
    if (const double amount = scalar("anamorphicAmount"); amount > 0.0) {
        const double length = input.size * scalar("anamorphicLength") / 100.0 * perHeight;
        const double turn = input.rotationDegrees * std::numbers::pi / 180.0;
        const QPointF reach = QPointF(std::cos(turn), std::sin(turn)) * length;
        QPen pen(colour("anamorphicColor", std::min(amount, 1.0) * level));
        pen.setWidthF(std::max(1.0, input.size * scalar("anamorphicWidth") / 100.0 * perHeight));
        painter.setPen(pen);
        painter.drawLine(light - reach, light + reach);
    }
    return image;
}

bool LensFlareCardWidget::showsRole(const std::string_view role) const {
    // The light's X/Y row stays beside the pad; a count, a pattern or a choice shows with its part;
    // the card draws the scalars and the colours.
    if (role == "lightPosition"sv)
        return true;
    for (const auto& spec : document::lensFlareParameters())
        if (spec.role == role)
            return spec.kind != LensFlareParameterKind::Scalar &&
                   spec.kind != LensFlareParameterKind::Color && shown(spec);
    return false;
}

std::optional<int> LensFlareCardWidget::socketY(const std::string_view role) const {
    if (role == "flareRotation"sv)
        return rotation_->y() + rotation_->socketY();
    if (role == "lightColor"sv)
        return lightColor_->y() + lightColor_->socketY();
    if (role == "anamorphicColor"sv && anamorphicColor_->isVisibleTo(this))
        return anamorphicColor_->y() + anamorphicColor_->socketY();
    for (const auto* row : shownRows())
        if (row->role() == role)
            return row->y() + row->socketY();
    return std::nullopt;
}

int LensFlareCardWidget::heightAt(const int width) const {
    int height = presets_->sizeHint().height() + small() + pad_->heightAt(width) + small() +
                 rotation_->heightHint() + small() + element_->sizeHint().height() + small() +
                 colorRowHeight() + small();
    if (group_ == LensFlareGroup::Anamorphic)
        height += colorRowHeight() + small();
    for (const auto* row : shownRows())
        height += row->heightHint() + small();
    return std::max(0, height - small());
}

int LensFlareCardWidget::minimumWidthHint() const {
    int width = std::max({kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                          pad_->minimumWidthHint(), rotation_->minimumWidthHint(),
                          presets_->sizeHint().width(), lightColor_->minimumWidthHint(),
                          anamorphicColor_->minimumWidthHint()});
    // Every row of every part, so the card keeps its width when the picker changes.
    for (const auto* rows : {&always_, &values_})
        for (const auto* row : *rows)
            width = std::max(width, row->minimumWidthHint());
    return width;
}

QSize LensFlareCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void LensFlareCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void LensFlareCardWidget::place() {
    const auto rows = shownRows();
    // One label column for every row, wide enough for the longest name: labels never truncate.
    int labels = std::max({rotation_->labelWidthHint(), lightColor_->labelWidthHint(),
                           anamorphicColor_->labelWidthHint()});
    for (const auto* group : {&always_, &values_})
        for (const auto* row : *group)
            labels = std::max(labels, row->labelWidthHint());
    rotation_->setLabelWidth(labels);
    lightColor_->setLabelWidth(labels);
    anamorphicColor_->setLabelWidth(labels);
    for (const auto* group : {&always_, &values_})
        for (auto* row : *group) {
            row->setLabelWidth(labels);
            row->setVisible(std::ranges::find(rows, row) != rows.end());
        }
    int y = 0;
    const int presetHeight = presets_->sizeHint().height();
    presets_->setGeometry(0, y, std::min(width(), presets_->sizeHint().width()), presetHeight);
    y += presetHeight + small();
    const int padHeight = pad_->heightAt(width());
    pad_->setGeometry(0, y, width(), padHeight);
    y += padHeight + small();
    rotation_->setGeometry(0, y, width(), rotation_->heightHint());
    y += rotation_->heightHint() + small();
    const auto placeRow = [&](GradingValueRow* row) {
        row->setGeometry(0, y, width(), row->heightHint());
        y += row->heightHint() + small();
    };
    for (auto* row : always_)
        placeRow(row);
    lightColor_->setGeometry(0, y, width(), colorRowHeight());
    y += colorRowHeight() + small();
    const int elementHeight = element_->sizeHint().height();
    element_->setGeometry(0, y, width(), elementHeight);
    y += elementHeight + small();
    for (auto* row : rows)
        if (std::ranges::find(always_, row) == always_.end())
            placeRow(row);
    const bool anamorphic = group_ == LensFlareGroup::Anamorphic;
    anamorphicColor_->setVisible(anamorphic);
    if (anamorphic)
        anamorphicColor_->setGeometry(0, y, width(), colorRowHeight());
}

void LensFlareCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    const auto state = readState();
    const bool changed = !stateKnown_ || state != state_;
    state_ = state;
    stateKnown_ = true;
    for (const auto* rows : {&always_, &values_})
        for (auto* row : *rows)
            row->refresh(locked);
    rotation_->refresh(locked);
    pad_->refresh(locked);
    lightColor_->refresh(locked);
    anamorphicColor_->refresh(locked);
    presets_->setEnabled(!locked);
    if (changed) {
        place();
        updateGeometry();
        emit rowsChanged();
    }
}

} // namespace bloom::ui
