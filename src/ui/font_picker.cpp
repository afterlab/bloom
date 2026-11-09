#include "font_picker.hpp"

#include <bloom/platform/font_catalog.hpp>
#include <bloom/runtime/font_fallback.hpp>
#include <bloom/runtime/font_resolver.hpp>
#include <bloom/runtime/google_catalogue.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/search_popup.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QHBoxLayout>
#include <QListView>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <set>

namespace bloom::ui {
namespace {

namespace fonts = runtime::fonts;

constexpr int kEntryRole = Qt::UserRole;
constexpr int kMaximumRecent = 8;
constexpr int kMaximumRegistrationsPerTick = 6;

// The pixel size a row draws its name at: the interface text size, so a row is one row tall.
[[nodiscard]] int rowPixelSize() { return kit::px(kit::Size::Control) / 2 + 2; }

[[nodiscard]] QString weightName(const int weight) {
    switch (weight) {
    case 100: return QObject::tr("Thin");
    case 200: return QObject::tr("ExtraLight");
    case 300: return QObject::tr("Light");
    case 400: return QObject::tr("Regular");
    case 500: return QObject::tr("Medium");
    case 600: return QObject::tr("SemiBold");
    case 700: return QObject::tr("Bold");
    case 800: return QObject::tr("ExtraBold");
    case 900: return QObject::tr("Black");
    default: return QString::number(weight);
    }
}

[[nodiscard]] QString styleLabel(const int weight, const bool italic) {
    if (!italic)
        return weightName(weight);
    return weight == 400 ? QObject::tr("Italic")
                         : QObject::tr("%1 Italic").arg(weightName(weight));
}

[[nodiscard]] std::string standardStyleName(const int weight, const bool italic) {
    return styleLabel(weight, italic).toStdString();
}

[[nodiscard]] std::shared_ptr<const platform::FontCatalogue> installedFonts() {
    return fonts::processFontResolver().systemFonts().snapshot();
}

[[nodiscard]] std::vector<std::string> installedFamilies() {
    std::vector<std::string> families;
    const auto catalogue = installedFonts();
    if (!catalogue)
        return families;
    std::set<std::string> seen;
    for (const auto& face : catalogue->faces)
        if (!face.family.empty() && seen.insert(face.family).second)
            families.push_back(face.family);
    std::ranges::sort(families, [](const std::string& a, const std::string& b) {
        return QString::fromStdString(a).compare(QString::fromStdString(b), Qt::CaseInsensitive) < 0;
    });
    return families;
}

[[nodiscard]] fonts::FontCategory categoryOfInstalled(const std::string& family) {
    if (const auto* known = fonts::googleCatalogue().find(family))
        return known->category;
    document::FontId probe;
    probe.family = family;
    return fonts::categoryForMissingFont(probe);
}

} // namespace

namespace fontpicker {

document::FontId idForFamily(const document::FontSource source, const QString& family,
                             const document::FontId& current) {
    document::FontId id;
    id.source = source;
    id.family = family.toStdString();
    id.weight = current.weight;
    id.italic = current.italic;
    if (source == document::FontSource::Google) {
        const auto* known = fonts::googleCatalogue().find(id.family);
        if (known != nullptr) {
            id.family = known->family;
            id.familyVersion = known->version;
            const fonts::GoogleFontStyle* best = nullptr;
            int bestScore = 1 << 30;
            for (const auto& style : known->styles) {
                const int score =
                    std::abs(style.weight - current.weight) + (style.italic != current.italic ? 1000 : 0);
                if (score < bestScore) {
                    bestScore = score;
                    best = &style;
                }
            }
            if (best != nullptr) {
                id.weight = best->weight;
                id.italic = best->italic;
            } else if (const auto* wght = known->axis("wght")) {
                id.weight = static_cast<int>(
                    std::clamp<double>(current.weight, wght->minimum, wght->maximum));
            }
        }
        id.style = standardStyleName(id.weight, id.italic);
        return id;
    }
    if (const auto catalogue = installedFonts()) {
        if (const auto* face = fonts::closestSystemFace(*catalogue, id.family, current.weight,
                                                        current.italic)) {
            id.family = face->family;
            id.style = face->style;
            id.weight = face->weight;
            id.italic = face->italic;
            id.postscriptName = face->postscriptName;
            return id;
        }
    }
    id.style = standardStyleName(id.weight, id.italic);
    return id;
}

std::vector<document::FontId> recentFonts() {
    std::vector<document::FontId> result;
    QSettings settings;
    for (const auto& text : settings.value(QStringLiteral("fonts/recent")).toStringList())
        if (auto id = document::decodeFontId(text.toStdString()))
            result.push_back(std::move(*id));
    return result;
}

void rememberRecent(const document::FontId& id) {
    if (id.source == document::FontSource::Builtin)
        return;
    QSettings settings;
    QStringList list = settings.value(QStringLiteral("fonts/recent")).toStringList();
    const QString family = QString::fromStdString(id.family);
    // One entry per family: choosing another style of it moves it to the front.
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const QString& text) {
                                  const auto other = document::decodeFontId(text.toStdString());
                                  return !other || QString::fromStdString(other->family) == family;
                              }),
               list.end());
    list.prepend(QString::fromStdString(document::encodeFontId(id)));
    while (list.size() > kMaximumRecent)
        list.removeLast();
    settings.setValue(QStringLiteral("fonts/recent"), list);
}

std::vector<StyleChoice> stylesFor(const document::FontId& id) {
    std::vector<StyleChoice> choices;
    const auto add = [&](QString label, document::FontId choice) {
        choices.push_back({std::move(label), std::move(choice)});
    };
    if (id.source == document::FontSource::Google) {
        if (const auto* known = fonts::googleCatalogue().find(id.family)) {
            std::vector<fonts::GoogleFontStyle> styles = known->styles;
            if (const auto* wght = known->axis("wght")) {
                // A variable family serves every hundred inside its weight range.
                for (int weight = 100; weight <= 900; weight += 100) {
                    if (weight < wght->minimum || weight > wght->maximum)
                        continue;
                    for (const bool italic : {false, true}) {
                        if (italic && !known->hasItalic())
                            continue;
                        const fonts::GoogleFontStyle style{weight, italic};
                        if (std::ranges::find(styles, style) == styles.end())
                            styles.push_back(style);
                    }
                }
            }
            std::ranges::sort(styles, [](const auto& a, const auto& b) {
                return a.italic != b.italic ? !a.italic : a.weight < b.weight;
            });
            for (const auto& style : styles) {
                auto choice = id;
                choice.weight = style.weight;
                choice.italic = style.italic;
                choice.style = standardStyleName(style.weight, style.italic);
                add(styleLabel(style.weight, style.italic), std::move(choice));
            }
        }
    } else if (id.source == document::FontSource::System) {
        if (const auto catalogue = installedFonts()) {
            std::vector<const platform::FontFace*> faces;
            for (const auto& face : catalogue->faces)
                if (face.family == id.family)
                    faces.push_back(&face);
            std::ranges::sort(faces, [](const auto* a, const auto* b) {
                return a->italic != b->italic ? !a->italic : a->weight < b->weight;
            });
            for (const auto* face : faces) {
                auto choice = id;
                choice.style = face->style;
                choice.weight = face->weight;
                choice.italic = face->italic;
                choice.postscriptName = face->postscriptName;
                add(QString::fromStdString(face->style), std::move(choice));
            }
        }
    }
    return choices;
}

} // namespace fontpicker

// ---- FontPickerPopup ------------------------------------------------------------------------

FontPickerPopup::FontPickerPopup(QWidget* parent) : KPopupWindow(parent, kit::Elevation::Popup) {
    setObjectName(QStringLiteral("fontPickerPopup"));
    setAccessibleName(tr("Font picker"));
    list_ = new kit::KPopupList(this);
    list_->setObjectName(QStringLiteral("kDropdownSurface"));
    list_->setUniformRows(false);
    list_->setFilterVisible(true);
    auto* field = list_->filterField();
    field->setObjectName(QStringLiteral("fontPickerFilter"));
    field->setAccessibleName(tr("Search fonts"));
    field->setPlaceholderText(tr("Search fonts…"));
    category_ = new kit::KDropdown(this);
    category_->setObjectName(QStringLiteral("fontPickerCategory"));
    category_->setAccessibleName(tr("Font category"));
    category_->setControlSize(kit::KDropdown::ControlSize::Dense);
    category_->addItem(tr("All"), -1);
    category_->addItem(tr("Sans"), static_cast<int>(fonts::FontCategory::Sans));
    category_->addItem(tr("Serif"), static_cast<int>(fonts::FontCategory::Serif));
    category_->addItem(tr("Display"), static_cast<int>(fonts::FontCategory::Display));
    category_->addItem(tr("Handwriting"), static_cast<int>(fonts::FontCategory::Handwriting));
    category_->addItem(tr("Monospace"), static_cast<int>(fonts::FontCategory::Monospace));
    list_->setFilterAccessory(category_);
    model_ = new QStandardItemModel(this);
    list_->setModel(model_);
    setContent(list_, field);

    connect(category_, &kit::KDropdown::activated, this, [this] { rebuild(); });
    connect(list_, &kit::KPopupList::filterChanged, this, [this] { rebuild(); });
    connect(list_, &kit::KPopupList::rowsChanged, this, [this] {
        if (isVisible())
            refit(wantedFrameSize());
    });
    connect(list_, &kit::KPopupList::currentRowChanged, this, [this](const int row) {
        const auto* entry = entryAt(row);
        if (entry == nullptr)
            return;
        Q_EMIT previewed(fontpicker::idForFamily(entry->source, entry->family, current_));
    });
    connect(list_, &kit::KPopupList::rowChosen, this, [this](const int row) {
        const auto* entry = entryAt(row);
        if (entry == nullptr)
            return;
        const auto id = fontpicker::idForFamily(entry->source, entry->family, current_);
        choseSomething_ = true;
        close();
        Q_EMIT chosen(id);
    });
    connect(list_, &kit::KPopupList::dismissed, this, &QWidget::close);
    connect(list_, &kit::KPopupList::tabbed, this, &QWidget::close);
    poll_.setInterval(200);
    connect(&poll_, &QTimer::timeout, this, &FontPickerPopup::pollFonts);
}

FontPickerPopup::~FontPickerPopup() = default;

QSize FontPickerPopup::wantedFrameSize() const {
    const int border = std::max(1, static_cast<int>(std::lround(kit::kHairlineWidth)));
    return {kit::px(kit::Size::ControlRoomy) * 16 + 2 * border,
            list_->fittedHeight() + 2 * border};
}

int FontPickerPopup::rowCount() const { return model_->rowCount(); }

const fontpicker::FamilyEntry* FontPickerPopup::entryAt(const int row) const {
    if (row < 0 || row >= model_->rowCount())
        return nullptr;
    const auto value = model_->index(row, 0).data(kEntryRole);
    if (!value.isValid())
        return nullptr;
    const int index = value.toInt();
    return index >= 0 && index < static_cast<int>(entries_.size()) ? &entries_[static_cast<std::size_t>(index)] : nullptr;
}

bool FontPickerPopup::rowIsGoogle(const int row) const {
    const auto* entry = entryAt(row);
    return entry != nullptr && entry->source == document::FontSource::Google;
}

QString FontPickerPopup::rowFamily(const int row) const {
    const auto* entry = entryAt(row);
    return entry != nullptr ? entry->family : QString();
}

void FontPickerPopup::openForField(const QWidget& anchor, const document::FontId& current) {
    current_ = current;
    choseSomething_ = false;
    fonts::processFontResolver().systemFonts().refresh();
    {
        const QSignalBlocker blocker(list_);
        list_->filterField()->clear();
    }
    rebuild();
    seenPreviewEpoch_ = fonts::processFontResolver().previewEpoch();
    openFor(anchor, wantedFrameSize());
    list_->setFrameCorner(translucent() ? static_cast<qreal>(kit::radiusPx(kit::Radius::Small, 0)) -
                                              kit::kHairlineWidth
                                        : 0.0);
    list_->scrollToCurrent();
    poll_.start();
    pollFonts();
}

void FontPickerPopup::hideEvent(QHideEvent* event) {
    poll_.stop();
    KPopupWindow::hideEvent(event);
    if (!choseSomething_)
        Q_EMIT cancelled();
    choseSomething_ = true;
}

void FontPickerPopup::rebuild() {
    model_->clear();
    entries_.clear();
    const QString query = list_->filterField()->text().simplified();
    const int category = category_->currentData().toInt();
    const auto matches = [&](const QString& family, const fonts::FontCategory kind) {
        if (category >= 0 && static_cast<int>(kind) != category)
            return false;
        return query.isEmpty() || family.contains(query, Qt::CaseInsensitive);
    };
    const auto addHeading = [&](const QString& text) {
        auto* heading = new QStandardItem(text);
        heading->setData(true, kit::kListHeadingRole);
        heading->setFlags(Qt::NoItemFlags);
        model_->appendRow(heading);
    };
    const auto addEntry = [&](fontpicker::FamilyEntry entry, const QString& label) {
        auto* item = new QStandardItem(label);
        item->setData(static_cast<int>(entries_.size()), kEntryRole);
        const auto preview = previewFamilies_.constFind(entry.family);
        if (entry.source == document::FontSource::System) {
            item->setData(kit::pickerFont(entry.family, rowPixelSize()), Qt::FontRole);
        } else if (preview != previewFamilies_.cend() && !preview->isEmpty()) {
            item->setData(kit::pickerFont(*preview, rowPixelSize()), Qt::FontRole);
        }
        entries_.push_back(std::move(entry));
        model_->appendRow(item);
    };

    int committed = -1;
    const auto currentFamily = QString::fromStdString(current_.family);

    // Recent.
    std::vector<fontpicker::FamilyEntry> recent;
    for (const auto& id : fontpicker::recentFonts()) {
        const auto family = QString::fromStdString(id.family);
        const auto kind = id.source == document::FontSource::Google
                              ? (fonts::googleCatalogue().find(id.family) != nullptr
                                     ? fonts::googleCatalogue().find(id.family)->category
                                     : fonts::FontCategory::Sans)
                              : categoryOfInstalled(id.family);
        if (matches(family, kind))
            recent.push_back({id.source, family, tr("Recent"), static_cast<int>(kind), true, true});
    }
    if (!recent.empty()) {
        addHeading(tr("Recent"));
        for (auto& entry : recent) {
            const bool isCurrent = entry.family == currentFamily;
            const auto family = entry.family;
            addEntry(std::move(entry), family);
            if (isCurrent && committed < 0)
                committed = model_->rowCount() - 1;
        }
    }

    // Installed.
    const auto installed = installedFamilies();
    bool headed = false;
    for (const auto& name : installed) {
        const auto kind = categoryOfInstalled(name);
        const auto family = QString::fromStdString(name);
        if (!matches(family, kind))
            continue;
        if (!headed) {
            addHeading(tr("Installed"));
            headed = true;
        }
        const bool isCurrent = current_.source == document::FontSource::System && family == currentFamily;
        addEntry({document::FontSource::System, family, tr("Installed"), static_cast<int>(kind)},
                 family);
        if (isCurrent && committed < 0)
            committed = model_->rowCount() - 1;
    }
    if (!headed && fonts::processFontResolver().systemFonts().pending()) {
        addHeading(tr("Installed"));
        auto* scanning = new QStandardItem(tr("Looking for installed fonts…"));
        scanning->setEnabled(false);
        model_->appendRow(scanning);
    }

    // Google Fonts.
    headed = false;
    bool exact = false;
    for (const auto& family : fonts::googleCatalogue().families()) {
        const auto name = QString::fromStdString(family.family);
        if (!matches(name, family.category))
            continue;
        if (!headed) {
            addHeading(tr("Google Fonts"));
            headed = true;
        }
        exact = exact || name.compare(query, Qt::CaseInsensitive) == 0;
        const bool isCurrent = current_.source == document::FontSource::Google && name == currentFamily;
        addEntry({document::FontSource::Google, name, tr("Google Fonts"),
                  static_cast<int>(family.category)},
                 name);
        if (isCurrent && committed < 0)
            committed = model_->rowCount() - 1;
    }
    // A family the baked index does not list is still fetched by name.
    if (query.size() >= 2 && category < 0 && !exact) {
        if (!headed)
            addHeading(tr("Google Fonts"));
        addEntry({document::FontSource::Google, query, tr("Google Fonts"), 0, false},
                 tr("%1 (not in this version's catalogue)").arg(query));
        model_->item(model_->rowCount() - 1)->setToolTip(
            tr("Not in this version's catalogue. Bloom asks Google Fonts for it by name."));
    }
    if (model_->rowCount() == 0) {
        auto* none = new QStandardItem(tr("No matching fonts"));
        none->setEnabled(false);
        model_->appendRow(none);
    }
    list_->setCommittedRow(committed);
    if (committed >= 0)
        list_->setCurrentRow(committed);
    else {
        list_->setCurrentRow(-1);
    }
    if (isVisible())
        refit(wantedFrameSize());
}

void FontPickerPopup::registerVisiblePreviews() {
    auto* view = list_->view();
    if (view == nullptr)
        return;
    const auto top = view->indexAt(QPoint(4, 2));
    const int first = top.isValid() ? top.row() : 0;
    const int rows = std::min(model_->rowCount(), first + kit::kListPopupMaxRows + 2);
    auto& resolver = fonts::processFontResolver();
    int registered = 0;
    for (int row = first; row < rows; ++row) {
        const auto* entry = entryAt(row);
        if (entry == nullptr || entry->source != document::FontSource::Google)
            continue;
        if (previewFamilies_.contains(entry->family))
            continue;
        const auto bytes = resolver.previewSubset(entry->family.toStdString(), true);
        if (!bytes || bytes->empty() || registered >= kMaximumRegistrationsPerTick)
            continue;
        const QByteArray data(reinterpret_cast<const char*>(bytes->data()),
                              static_cast<qsizetype>(bytes->size()));
        const QString family = kit::registerPickerFont(data);
        previewFamilies_.insert(entry->family, family);
        ++registered;
        if (!family.isEmpty())
            model_->item(row)->setData(kit::pickerFont(family, rowPixelSize()), Qt::FontRole);
    }
    if (registered > 0)
        list_->view()->viewport()->update();
}

void FontPickerPopup::pollFonts() {
    auto& resolver = fonts::processFontResolver();
    // The first scan of the installed fonts finished: show them.
    if (resolver.systemFonts().poll())
        rebuild();
    registerVisiblePreviews();
    seenPreviewEpoch_ = resolver.previewEpoch();
}

// ---- FontField ------------------------------------------------------------------------------

FontField::FontField(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fontField"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    family_ = new kit::KDropdown(this);
    family_->setObjectName(QStringLiteral("propertiesTextFont"));
    family_->setAccessibleName(tr("Font"));
    family_->setExternalPopup(true);
    family_->setControlSize(kit::KDropdown::ControlSize::Dense);
    family_->setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth));
    family_->addItem(QString());
    layout->addWidget(family_, 1);
    style_ = new kit::KDropdown(this);
    style_->setObjectName(QStringLiteral("propertiesTextFontStyle"));
    style_->setAccessibleName(tr("Font style"));
    style_->setControlSize(kit::KDropdown::ControlSize::Dense);
    style_->setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth) * 2 / 3);
    layout->addWidget(style_);
    axes_ = new QWidget(this);
    axes_->setObjectName(QStringLiteral("propertiesTextFontAxes"));
    auto* axisLayout = new QHBoxLayout(axes_);
    axisLayout->setContentsMargins(0, 0, 0, 0);
    axisLayout->setSpacing(kit::px(kit::Spacing::XS));
    layout->addWidget(axes_);
    axes_->hide();
    connect(family_, &kit::KDropdown::openRequested, this, &FontField::openPicker);
    connect(style_, &kit::KDropdown::activated, this, &FontField::styleActivated);
    statusTimer_.setInterval(800);
    connect(&statusTimer_, &QTimer::timeout, this, &FontField::refreshStatus);
}

void FontField::setDense(const bool dense) {
    const auto size = dense ? kit::KDropdown::ControlSize::Dense : kit::KDropdown::ControlSize::Default;
    family_->setControlSize(size);
    style_->setControlSize(size);
}

void FontField::setFontText(const QString& text) {
    if (text == text_ && family_->count() > 0 && !family_->itemText(0).isEmpty()) {
        refreshStatus();
        return;
    }
    text_ = text;
    id_ = document::decodeFontId(text.toStdString()).value_or(document::defaultFontId());
    {
        const QSignalBlocker blocker(family_);
        family_->clearItems();
        family_->addItem(QString::fromStdString(id_.family));
    }
    rebuildStyles();
    refreshStatus();
}

void FontField::rebuildStyles() {
    styles_ = fontpicker::stylesFor(id_);
    const QSignalBlocker blocker(style_);
    style_->clearItems();
    int current = -1;
    for (std::size_t index = 0; index < styles_.size(); ++index) {
        style_->addItem(styles_[index].label);
        if (current < 0 && styles_[index].id.style == id_.style)
            current = static_cast<int>(index);
    }
    for (std::size_t index = 0; current < 0 && index < styles_.size(); ++index)
        if (styles_[index].id.weight == id_.weight && styles_[index].id.italic == id_.italic)
            current = static_cast<int>(index);
    if (current < 0) {
        // The family or style is not available here: show what the project recorded.
        current = style_->addItem(QString::fromStdString(id_.style.empty() ? "Regular" : id_.style));
    }
    style_->setCurrentIndex(current);

    // Variable axes other than weight: one small field each.
    auto* layout = axes_->layout();
    while (auto* child = layout->takeAt(0)) {
        delete child->widget();
        delete child;
    }
    bool any = false;
    if (id_.source == document::FontSource::Google) {
        if (const auto* known = fonts::googleCatalogue().find(id_.family)) {
            for (const auto& axis : known->axes) {
                if (axis.tag == "wght" || axis.tag == "ital" || axis.minimum >= axis.maximum)
                    continue;
                auto* field = new kit::KValueField(axes_);
                field->setObjectName(QStringLiteral("propertiesTextFontAxis_%1")
                                         .arg(QString::fromStdString(axis.tag)));
                field->setAccessibleName(tr("Font axis %1").arg(QString::fromStdString(axis.tag)));
                field->setCompact(true);
                field->setLabel(QString::fromStdString(axis.tag));
                field->setDecimals(0);
                field->setRange(axis.minimum, axis.maximum);
                double value = std::clamp(axis.tag == "wdth" ? 100.0 : axis.minimum, axis.minimum,
                                          axis.maximum);
                for (const auto& set : id_.axes)
                    if (set.tag == axis.tag)
                        value = set.value;
                field->setValue(value);
                const auto tag = QString::fromStdString(axis.tag);
                connect(field, &kit::KValueField::valueChanged, this,
                        [this, tag](const double v) { axisChanged(tag, v); });
                layout->addWidget(field);
                any = true;
            }
        }
    }
    axes_->setVisible(any);
}

void FontField::refreshStatus() {
    auto& resolver = fonts::processFontResolver();
    const auto status = resolver.status(id_, true);
    using State = fonts::FontStatus::State;
    const bool missing = status.state == State::Missing;
    family_->setValueTone(missing ? std::optional<kit::Color>(kit::Color::Error) : std::nullopt);
    style_->setValueTone(missing ? std::optional<kit::Color>(kit::Color::Error) : std::nullopt);
    QString tip = QString::fromStdString(document::fontDisplayName(id_));
    if (missing && !status.detail.empty())
        tip = QString::fromStdString(status.detail);
    else if (status.state == State::Downloading)
        tip = tr("Downloading %1…").arg(QString::fromStdString(id_.family));
    family_->setToolTip(tip);
    if (status.state == State::Downloading || status.state == State::Checking) {
        if (!statusTimer_.isActive())
            statusTimer_.start();
    } else {
        statusTimer_.stop();
    }
}

void FontField::openPicker() {
    if (popup_ == nullptr) {
        popup_ = new FontPickerPopup(this);
        connect(popup_, &FontPickerPopup::previewed, this, [this](const document::FontId& id) {
            Q_EMIT fontPreviewed(QString::fromStdString(document::encodeFontId(id)));
        });
        connect(popup_, &FontPickerPopup::chosen, this, [this](const document::FontId& id) {
            fontpicker::rememberRecent(id);
            commitId(id);
        });
        connect(popup_, &FontPickerPopup::cancelled, this, [this] { Q_EMIT previewCancelled(); });
        connect(popup_, &FontPickerPopup::closed, this,
                [this] { family_->setExternalPopupOpen(false); });
    }
    family_->setExternalPopupOpen(true);
    popup_->openForField(*family_, id_);
}

void FontField::commitId(const document::FontId& id) {
    id_ = id;
    text_ = QString::fromStdString(document::encodeFontId(id));
    {
        const QSignalBlocker blocker(family_);
        family_->clearItems();
        family_->addItem(QString::fromStdString(id_.family));
    }
    rebuildStyles();
    refreshStatus();
    Q_EMIT fontCommitted(text_);
}

void FontField::styleActivated() {
    const int index = style_->currentIndex();
    if (index < 0 || index >= static_cast<int>(styles_.size()))
        return;
    auto id = styles_[static_cast<std::size_t>(index)].id;
    id.axes = id_.axes;
    commitId(id);
}

void FontField::axisChanged(const QString& tag, const double value) {
    auto id = id_;
    const auto name = tag.toStdString();
    bool found = false;
    for (auto& axis : id.axes)
        if (axis.tag == name) {
            axis.value = value;
            found = true;
        }
    if (!found)
        id.axes.push_back({name, value});
    std::ranges::sort(id.axes, [](const auto& a, const auto& b) { return a.tag < b.tag; });
    if (document::isValidFontId(id))
        commitId(id);
}

// ---- binding --------------------------------------------------------------------------------

void bindFontFieldToParameter(FontField& field, CompositionSession& session,
                              std::function<std::optional<document::ParameterId>()> parameter) {
    struct State {
        std::optional<document::ParameterId> editing;
    };
    const auto state = std::make_shared<State>();
    QObject::connect(&field, &FontField::fontPreviewed, &field,
                     [&session, parameter, state](const QString& text) {
                         if (!state->editing || !session.isValueEditing(*state->editing)) {
                             const auto id = parameter();
                             if (!id || !session.beginValueEdit(*id))
                                 return;
                             state->editing = id;
                         }
                         (void)session.updateValueEdit(text.toStdString());
                     });
    QObject::connect(&field, &FontField::previewCancelled, &field, [&session, state] {
        if (state->editing && session.isValueEditing(*state->editing))
            session.cancelValueEdit();
        state->editing.reset();
    });
    QObject::connect(&field, &FontField::fontCommitted, &field,
                     [&session, parameter, state](const QString& text) {
                         // The live preview is dropped first so the commit is the one undo step.
                         if (state->editing && session.isValueEditing(*state->editing))
                             session.cancelValueEdit();
                         state->editing.reset();
                         if (const auto id = parameter())
                             (void)session.setParameterValue(*id, text.toStdString(),
                                                             QObject::tr("Set Text Font"));
                     });
}

} // namespace bloom::ui
