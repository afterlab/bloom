#include "interpret_footage_dialog.hpp"

#include "assets_editor_internal.hpp"

#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QHBoxLayout>
#include <QSignalBlocker>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bloom::ui {
namespace {
using document::AssetKind;

struct RatioChoice final {
    const char* label;
    std::uint32_t numerator;
    std::uint32_t denominator;
};
// The square and common anamorphic and SD ratios, the Composition Settings list.
constexpr std::array<RatioChoice, 8> kPixelAspects{{
    {"Square Pixels (1.0)", 1, 1},
    {"D1/DV NTSC (0.91)", 10, 11},
    {"D1/DV NTSC Widescreen (1.21)", 40, 33},
    {"D1/DV PAL (1.09)", 59, 54},
    {"D1/DV PAL Widescreen (1.46)", 118, 81},
    {"HDV 1080 / DVCPRO HD 720 (1.33)", 4, 3},
    {"DVCPRO HD 1080 (1.5)", 3, 2},
    {"Anamorphic 2:1 (2.0)", 2, 1},
}};

[[nodiscard]] QString ratioData(const core::PixelAspectRatio ratio) {
    return QStringLiteral("%1:%2").arg(ratio.numerator()).arg(ratio.denominator());
}
[[nodiscard]] QString ratioText(const core::PixelAspectRatio ratio) {
    for (const auto& choice : kPixelAspects)
        if (choice.numerator == ratio.numerator() && choice.denominator == ratio.denominator())
            return QObject::tr(choice.label);
    return QStringLiteral("%1:%2 (%3)")
        .arg(ratio.numerator())
        .arg(ratio.denominator())
        .arg(static_cast<double>(ratio.numerator()) / ratio.denominator(), 0, 'f', 2);
}

[[nodiscard]] QString fieldOrderText(const std::uint32_t order) {
    switch (order) {
    case 1:
        return QObject::tr("Progressive");
    case 2:
    case 4:
        return QObject::tr("Upper Field First");
    case 3:
    case 5:
        return QObject::tr("Lower Field First");
    default:
        return QObject::tr("Unknown");
    }
}

[[nodiscard]] QWidget* row(QWidget* parent, std::initializer_list<QWidget*> widgets,
                           QWidget* stretch = nullptr) {
    auto* holder = new QWidget(parent);
    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    for (auto* widget : widgets)
        layout->addWidget(widget, widget == stretch ? 1 : 0);
    if (stretch == nullptr)
        layout->addStretch(1);
    return holder;
}

[[nodiscard]] kit::KDropdown* dropdown(QWidget* parent, const char* name, const QString& label) {
    auto* field = new kit::KDropdown(parent);
    field->setObjectName(QString::fromLatin1(name));
    field->setAccessibleName(label);
    return field;
}

void select(kit::KDropdown& field, const QVariant& data) {
    const auto index = field.findData(data);
    field.setCurrentIndex(index < 0 ? 0 : index);
}

} // namespace

std::optional<document::FrameRate> parseFootageFrameRate(const QString& raw) {
    return document::parseFootageFrameRate(raw.trimmed().toStdString());
}

QString footageFrameRateText(const document::FrameRate rate) {
    if (rate.denominator() == 1001)
        return assets::frameRateText(rate);
    if (rate.denominator() == 1)
        return QString::number(rate.numerator());
    const auto value = static_cast<double>(rate.numerator()) / rate.denominator();
    const auto decimal = QString::number(value, 'g', 10);
    // A rate a short decimal spells exactly reads as that decimal, any other as n/d.
    if (parseFootageFrameRate(decimal) == rate)
        return decimal;
    return QStringLiteral("%1/%2").arg(rate.numerator()).arg(rate.denominator());
}

QString interpretColorData(const document::AssetInterpretation& interpretation) {
    if (!interpretation.inputColorSpaceId.empty())
        return QStringLiteral("id:") + QString::fromStdString(interpretation.inputColorSpaceId);
    switch (interpretation.colorSpace) {
    case document::AssetColorSpace::Raw:
        return QStringLiteral("raw");
    case document::AssetColorSpace::Srgb:
        return QStringLiteral("srgb");
    case document::AssetColorSpace::Linear:
        return QStringLiteral("linear");
    case document::AssetColorSpace::Auto:
        break;
    }
    return QStringLiteral("auto");
}

InterpretFootageDialog::InterpretFootageDialog(const document::AssetRecord& asset,
                                               const std::size_t count,
                                               std::vector<document::AssetKind> kinds,
                                               std::vector<InterpretColorChoice> colors,
                                               QString resolvedColor, QWidget& owner)
    : kit::KDialog(count > 1 ? tr("Interpret Footage — %1 items").arg(count)
                             : tr("Interpret Footage — %1").arg(QString::fromStdString(asset.name)),
                   QStringLiteral("interpretFootageDialog"), owner, Qt::WindowModal, Width::Wide),
      asset_(asset) {
    const auto has = [&](const AssetKind kind) { return std::ranges::find(kinds, kind) != kinds.end(); };
    timed_ = has(AssetKind::Sequence) || has(AssetKind::Video);
    sequence_ = has(AssetKind::Sequence);
    const bool video = asset.kind == AssetKind::Video;
    const auto& value = asset.interpretation;
    auto* form = addForm();
    auto* body = static_cast<QWidget*>(form);

    // Frame rate: a sequence assumes one; a video plays at its own or a conformed rate.
    if (timed_) {
        rateMode_ = dropdown(body, "interpretFootageRateMode", tr("Frame Rate"));
        if (video) {
            const auto file = document::footageFileFrameRate(asset);
            rateMode_->addItem(file ? tr("Use the file's rate (%1 fps)").arg(footageFrameRateText(*file))
                                    : tr("Use the file's rate"),
                               QStringLiteral("file"));
            rateMode_->addItem(tr("Conform to"), QStringLiteral("override"));
        } else {
            rateMode_->addItem(tr("Match the composition"), QStringLiteral("file"));
            rateMode_->addItem(tr("Assume this frame rate"), QStringLiteral("override"));
        }
        rate_ = new kit::KLineEdit(body);
        rate_->setObjectName(QStringLiteral("interpretFootageRate"));
        rate_->setAccessibleName(tr("Frame rate in frames per second"));
        rate_->setPlaceholderText(tr("24, 23.976 or 30000/1001"));
        rate_->setFont(kit::font(kit::TypeRole::Value));
        rate_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* unit = new kit::KLabel(tr("fps"), body);
        unit->setTone(kit::Color::Muted);
        rateRow_ = row(body, {rateMode_, rate_, unit}, rate_);
        form->addRow(tr("Frame Rate"), rateRow_)
            ->setObjectName(QStringLiteral("interpretFootageRateError"));
        select(*rateMode_, value.frameRate ? QStringLiteral("override") : QStringLiteral("file"));
        const auto shown = value.frameRate ? value.frameRate : document::footageFileFrameRate(asset);
        rate_->setText(shown ? footageFrameRateText(*shown) : QStringLiteral("24"));
        connect(rateMode_, &kit::KDropdown::currentIndexChanged, this, [this](int) { refresh(); });
        connect(rate_, &QLineEdit::textChanged, this, [this](const QString&) { refresh(); });
        watch(rateMode_);
        watch(rate_);

        timecodeMode_ = dropdown(body, "interpretFootageTimecodeMode", tr("Start Timecode"));
        const auto source = [&] {
            auto copy = asset;
            copy.interpretation.startTimecode.reset();
            return document::footageStartTimecode(copy);
        }();
        const auto label = [this](const document::CompositionTimecode& timecode) {
            const auto rate = timecodeRate();
            const auto frames = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, timecodeStartFrame(rate, {timecode.start, false})));
            return formatTimelineFrameLabel(frames, rate, true, {core::RationalTime{}, timecode.dropFrame});
        };
        timecodeMode_->addItem(source ? tr("Use the source's (%1)").arg(label(*source))
                                      : tr("Use the source's (none)"),
                               QStringLiteral("source"));
        timecodeMode_->addItem(tr("Override"), QStringLiteral("override"));
        timecode_ = new kit::KLineEdit(body);
        timecode_->setObjectName(QStringLiteral("interpretFootageTimecode"));
        timecode_->setAccessibleName(tr("Start timecode"));
        timecode_->setPlaceholderText(tr("00:00:00:00"));
        timecode_->setFont(kit::font(kit::TypeRole::Value));
        timecodeRow_ = row(body, {timecodeMode_, timecode_}, timecode_);
        form->addRow(tr("Start Timecode"), timecodeRow_)
            ->setObjectName(QStringLiteral("interpretFootageTimecodeError"));
        select(*timecodeMode_,
               value.startTimecode ? QStringLiteral("override") : QStringLiteral("source"));
        const auto shownTimecode = value.startTimecode ? value.startTimecode : source;
        timecode_->setText(shownTimecode ? label(*shownTimecode) : QStringLiteral("00:00:00:00"));
        connect(timecodeMode_, &kit::KDropdown::currentIndexChanged, this, [this](int) { refresh(); });
        connect(timecode_, &QLineEdit::textChanged, this, [this](const QString&) { refresh(); });
        watch(timecodeMode_);
        watch(timecode_);
    }

    // Alpha.
    alpha_ = dropdown(body, "interpretFootageAlpha", tr("Alpha"));
    const bool declaredPremultiplied =
        value.alphaAssociation == document::AssetAlphaAssociation::Premultiplied;
    alpha_->addItem(declaredPremultiplied ? tr("Guess from the file (Premultiplied)")
                                          : tr("Guess from the file (Straight)"),
                    static_cast<int>(document::AssetAlphaMode::Guess));
    alpha_->addItem(tr("Ignore"), static_cast<int>(document::AssetAlphaMode::Ignore));
    alpha_->addItem(tr("Straight (Unmatted)"), static_cast<int>(document::AssetAlphaMode::Straight));
    alpha_->addItem(tr("Premultiplied (Matted with color)"),
                    static_cast<int>(document::AssetAlphaMode::Premultiplied));
    select(*alpha_, static_cast<int>(value.alphaMode));
    form->addRow(tr("Alpha"), alpha_);
    matte_ = new kit::KColorChip(body);
    matte_->setObjectName(QStringLiteral("interpretFootageMatte"));
    matte_->setAccessibleName(tr("Matte color"));
    matte_->setToolTip(tr("The color the premultiplied alpha was matted against"));
    matte_->setColor(kit::KColor::fromRgba(static_cast<float>(value.matteColor[0]),
                                           static_cast<float>(value.matteColor[1]),
                                           static_cast<float>(value.matteColor[2])));
    matteRow_ = row(body, {matte_});
    form->addRow(tr("Matte Color"), matteRow_);
    invert_ = new kit::KCheckBox(body);
    invert_->setObjectName(QStringLiteral("interpretFootageInvertAlpha"));
    invert_->setChecked(value.invertAlpha);
    (void)form->addCheckRow(QString{}, *invert_, tr("Invert alpha"));
    connect(alpha_, &kit::KDropdown::currentIndexChanged, this, [this](int) { refresh(); });
    watch(alpha_);
    watch(invert_);
    watch(matte_);

    // Fields.
    if (timed_) {
        fieldOrder_ = dropdown(body, "interpretFootageFieldOrder", tr("Field Order"));
        auto file = asset;
        file.interpretation.fieldOrder = document::AssetFieldOrder::File;
        fieldOrder_->addItem(tr("Use the file's (%1)").arg(fieldOrderText(document::footageFieldOrder(file))),
                             static_cast<int>(document::AssetFieldOrder::File));
        fieldOrder_->addItem(tr("Progressive"), static_cast<int>(document::AssetFieldOrder::Progressive));
        fieldOrder_->addItem(tr("Upper Field First"),
                             static_cast<int>(document::AssetFieldOrder::UpperFirst));
        fieldOrder_->addItem(tr("Lower Field First"),
                             static_cast<int>(document::AssetFieldOrder::LowerFirst));
        select(*fieldOrder_, static_cast<int>(value.fieldOrder));
        form->addRow(tr("Fields"), fieldOrder_);
        deinterlace_ = new kit::KCheckBox(body);
        deinterlace_->setObjectName(QStringLiteral("interpretFootageDeinterlace"));
        deinterlace_->setChecked(value.deinterlace);
        (void)form->addCheckRow(QString{}, *deinterlace_, tr("Deinterlace"));
        connect(fieldOrder_, &kit::KDropdown::currentIndexChanged, this, [this](int) { refresh(); });
        watch(fieldOrder_);
        watch(deinterlace_);
    }

    // Pixel aspect ratio.
    pixelAspect_ = dropdown(body, "interpretFootagePixelAspect", tr("Pixel Aspect Ratio"));
    pixelAspect_->addItem(tr("Use the file's (%1)").arg(ratioText(document::footageFilePixelAspect(asset))),
                          QString{});
    for (const auto& choice : kPixelAspects)
        pixelAspect_->addItem(tr(choice.label),
                              QStringLiteral("%1:%2").arg(choice.numerator).arg(choice.denominator));
    if (value.pixelAspect && pixelAspect_->findData(ratioData(*value.pixelAspect)) < 0)
        pixelAspect_->addItem(ratioText(*value.pixelAspect), ratioData(*value.pixelAspect));
    select(*pixelAspect_, value.pixelAspect ? ratioData(*value.pixelAspect) : QString{});
    form->addRow(tr("Pixel Aspect Ratio"), pixelAspect_);
    watch(pixelAspect_);

    // Loop and missing frames.
    if (timed_) {
        loop_ = new kit::KValueField(body);
        loop_->setObjectName(QStringLiteral("interpretFootageLoop"));
        loop_->setAccessibleName(tr("Loop"));
        loop_->setIntegerMode(true);
        loop_->setIntegerRange(1, document::kMaxAssetLoopCount);
        loop_->setIntegerValue(value.loopCount);
        loop_->setUnit(tr("times"));
        loop_->setDefaultValue(1.0);
        form->addRow(tr("Loop"), row(body, {loop_}));
        watch(loop_);
    }
    if (sequence_) {
        missing_ = dropdown(body, "interpretFootageMissingFrames", tr("Missing Frames"));
        missing_->addItem(tr("Hold the previous frame"),
                          static_cast<int>(document::AssetMissingFrames::HoldPrevious));
        missing_->addItem(tr("Show nothing"), static_cast<int>(document::AssetMissingFrames::Nothing));
        select(*missing_, static_cast<int>(value.missingFrames));
        form->addRow(tr("Missing Frames"), missing_);
        watch(missing_);
    }

    // Color management.
    color_ = dropdown(body, "interpretFootageColor", tr("Input Color"));
    color_->setSearchable(true);
    color_->setWidthFloor(kit::px(kit::Size::DropdownWidth) * 2);
    initialColor_ = interpretColorData(value);
    colorStatus_ = new kit::KLabel(body);
    colorStatus_->setObjectName(QStringLiteral("interpretFootageColorStatus"));
    colorStatus_->setWordWrap(true);
    colorStatus_->setTone(kit::Color::Muted);
    form->addRow(tr("Input Color"), color_);
    form->addRow(colorStatus_);
    resolvedColor_ = std::move(resolvedColor);
    setColorChoices(std::move(colors), {});
    watch(color_);

    buttonBox()->setObjectName(QStringLiteral("interpretFootageButtons"));
    addButton(Role::Accept, tr("OK"));
    addButton(Role::Reject, tr("Cancel"));
    setValidator([this]() -> std::optional<kit::KRefusal> {
        const auto reason = issue();
        if (reason.isEmpty())
            return std::nullopt;
        return kit::KRefusal{nullptr, reason};
    });
    setInitialFocus(rateMode_ != nullptr ? static_cast<QWidget*>(rateMode_) : alpha_);
    refresh();
}

void InterpretFootageDialog::setColorChoices(std::vector<InterpretColorChoice> colors,
                                             const QString& status) {
    const QString selected = color_->count() ? color_->currentData().toString() : initialColor_;
    {
        const QSignalBlocker blocked(color_);
        color_->clearItems();
        color_->addItem(resolvedColor_.isEmpty() ? tr("Auto")
                                                 : tr("Auto (%1)").arg(resolvedColor_),
                        QStringLiteral("auto"));
        color_->addItem(tr("Preserve RGB (no color conversion)"), QStringLiteral("raw"));
        color_->addItem(tr("Interpret as Linear"), QStringLiteral("linear"));
        color_->addItem(tr("Legacy sRGB"), QStringLiteral("srgb"));
        for (const auto& choice : colors)
            color_->addItem(choice.label, choice.data);
        if (color_->findData(selected) < 0 && selected.startsWith(QStringLiteral("id:")))
            color_->addItem(tr("Current space unavailable: %1").arg(selected.mid(3)), selected);
        select(*color_, selected);
    }
    if (!status.isEmpty())
        colorStatus_->setText(status);
    revalidate();
}

document::FrameRate InterpretFootageDialog::timecodeRate() const {
    if (rateMode_ != nullptr && rateMode_->currentData().toString() == QStringLiteral("override"))
        if (const auto rate = parsedRate())
            return *rate;
    return document::footageFileFrameRate(asset_).value_or(
        asset_.interpretation.frameRate.value_or(document::FrameRate::framesPerSecond24()));
}

std::optional<document::FrameRate> InterpretFootageDialog::parsedRate() const {
    return rate_ != nullptr ? parseFootageFrameRate(rate_->text()) : std::nullopt;
}

QString InterpretFootageDialog::issue() const {
    if (rateMode_ != nullptr && rateMode_->currentData().toString() == QStringLiteral("override")) {
        const auto rate = parsedRate();
        if (!rate || !document::isValidFootageFrameRate(*rate))
            return tr("Enter a frame rate from 0.01 to 1000 fps, such as 24, 23.976 or 30000/1001");
    }
    if (timecodeMode_ != nullptr &&
        timecodeMode_->currentData().toString() == QStringLiteral("override") &&
        !parseTimelineFrameLabel(timecode_->text(), timecodeRate(), true,
                                 {core::RationalTime{},
                                  timecode_->text().contains(QLatin1Char(';'))}))
        return tr("Enter the start timecode as HH:MM:SS:FF");
    if (!interpretation())
        return tr("The interpretation is out of range");
    return {};
}

std::optional<document::AssetInterpretation> InterpretFootageDialog::interpretation() const {
    auto value = asset_.interpretation;
    if (rateMode_ != nullptr) {
        if (rateMode_->currentData().toString() == QStringLiteral("override")) {
            const auto rate = parsedRate();
            if (!rate)
                return std::nullopt;
            value.frameRate = *rate;
        } else {
            value.frameRate.reset();
        }
    }
    if (timecodeMode_ != nullptr) {
        if (timecodeMode_->currentData().toString() == QStringLiteral("override")) {
            const auto rate = timecodeRate();
            const bool drop = timecode_->text().contains(QLatin1Char(';')) &&
                              document::hasDropFrameTimecode(rate);
            const auto frames = parseTimelineFrameLabel(timecode_->text(), rate, true,
                                                        {core::RationalTime{}, drop});
            const auto start = frames && *frames < (std::uint64_t{1} << 40)
                                   ? core::RationalTime::create(
                                         static_cast<std::int64_t>(*frames) * rate.denominator(),
                                         rate.numerator())
                                   : std::nullopt;
            if (!start)
                return std::nullopt;
            value.startTimecode = document::CompositionTimecode{*start, drop};
        } else {
            value.startTimecode.reset();
        }
    }
    value.alphaMode = static_cast<document::AssetAlphaMode>(alpha_->currentData().toInt());
    value.invertAlpha = invert_->isChecked();
    const auto matte = matte_->color();
    value.matteColor = {std::clamp(static_cast<double>(matte.red), 0.0, 1.0),
                        std::clamp(static_cast<double>(matte.green), 0.0, 1.0),
                        std::clamp(static_cast<double>(matte.blue), 0.0, 1.0)};
    if (fieldOrder_ != nullptr) {
        value.fieldOrder = static_cast<document::AssetFieldOrder>(fieldOrder_->currentData().toInt());
        value.deinterlace = deinterlace_->isChecked();
    }
    const auto ratio = pixelAspect_->currentData().toString().split(QLatin1Char(':'));
    value.pixelAspect = ratio.size() == 2 ? core::PixelAspectRatio::create(ratio[0].toULongLong(),
                                                                           ratio[1].toULongLong())
                                          : std::nullopt;
    if (loop_ != nullptr)
        value.loopCount = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            loop_->integerValue(), 1, document::kMaxAssetLoopCount));
    if (missing_ != nullptr)
        value.missingFrames =
            static_cast<document::AssetMissingFrames>(missing_->currentData().toInt());
    const auto color = color_->currentData().toString();
    value.inputColorSpaceId.clear();
    value.colorSpace = color == QStringLiteral("raw")      ? document::AssetColorSpace::Raw
                       : color == QStringLiteral("linear") ? document::AssetColorSpace::Linear
                       : color == QStringLiteral("srgb")   ? document::AssetColorSpace::Srgb
                                                           : document::AssetColorSpace::Auto;
    if (color.startsWith(QStringLiteral("id:")))
        value.inputColorSpaceId = color.mid(3).toStdString();
    if (!document::footageInterpretationIssue(value).empty())
        return std::nullopt;
    return value;
}

std::optional<commands::AssetInterpretationChange> InterpretFootageDialog::change() const {
    if (!issue().isEmpty())
        return std::nullopt;
    const auto value = interpretation();
    if (!value)
        return std::nullopt;
    const auto& before = asset_.interpretation;
    commands::AssetInterpretationChange change;
    if (value->frameRate != before.frameRate)
        change.frameRate = value->frameRate;
    if (value->startTimecode != before.startTimecode)
        change.startTimecode = value->startTimecode;
    if (value->alphaMode != before.alphaMode)
        change.alphaMode = value->alphaMode;
    if (value->invertAlpha != before.invertAlpha)
        change.invertAlpha = value->invertAlpha;
    if (value->matteColor != before.matteColor)
        change.matteColor = value->matteColor;
    if (value->fieldOrder != before.fieldOrder)
        change.fieldOrder = value->fieldOrder;
    if (value->deinterlace != before.deinterlace)
        change.deinterlace = value->deinterlace;
    if (value->pixelAspect != before.pixelAspect)
        change.pixelAspect = value->pixelAspect;
    if (value->loopCount != before.loopCount)
        change.loopCount = value->loopCount;
    if (value->missingFrames != before.missingFrames)
        change.missingFrames = value->missingFrames;
    if (value->colorSpace != before.colorSpace ||
        value->inputColorSpaceId != before.inputColorSpaceId)
        change.inputColor = std::pair{value->colorSpace, value->inputColorSpaceId};
    return change;
}

void InterpretFootageDialog::refresh() {
    if (rate_ != nullptr)
        rate_->setVisible(rateMode_->currentData().toString() == QStringLiteral("override"));
    if (timecode_ != nullptr)
        timecode_->setEnabled(timecodeMode_->currentData().toString() ==
                              QStringLiteral("override"));
    if (auto* form = findChild<kit::KForm*>())
        form->setRowVisible(matteRow_, static_cast<document::AssetAlphaMode>(
                                           alpha_->currentData().toInt()) ==
                                           document::AssetAlphaMode::Premultiplied);
    revalidate();
}

} // namespace bloom::ui
