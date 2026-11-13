#include <bloom/ui/scopes_editor.hpp>

#include <bloom/render/scope_table_layout.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/scopes_trace_renderer.hpp>
#include <bloom/ui/scopes_view.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui {
namespace {

struct SectionSpec final {
    scopes::Kind kind;
    const char* id;
};
constexpr std::array<SectionSpec, scopes::kKindCount> kSections{{
    {scopes::Kind::Waveform, "waveform"},
    {scopes::Kind::Parade, "parade"},
    {scopes::Kind::Vectorscope, "vectorscope"},
    {scopes::Kind::Histogram, "histogram"},
    {scopes::Kind::Chromaticity, "chromaticity"},
}};

constexpr double kBrightnessMinimum = 0.25;
constexpr double kBrightnessMaximum = 4.0;
constexpr std::array<std::uint32_t, 5> kAxisMaxima{1, 2, 4, 8, 16};

[[nodiscard]] QString sectionTitle(const scopes::Kind kind) {
    switch (kind) {
    case scopes::Kind::Waveform:
        return ScopesEditor::tr("Waveform");
    case scopes::Kind::Parade:
        return ScopesEditor::tr("Parade");
    case scopes::Kind::Vectorscope:
        return ScopesEditor::tr("Vectorscope");
    case scopes::Kind::Histogram:
        return ScopesEditor::tr("Histogram");
    case scopes::Kind::Chromaticity:
        return ScopesEditor::tr("CIE Chromaticity");
    }
    return {};
}

[[nodiscard]] std::uint32_t tableBitOf(const scopes::Kind kind) {
    switch (kind) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade:
        return render::kScopeTableWaveform;
    case scopes::Kind::Vectorscope:
        return render::kScopeTableVectorscope;
    case scopes::Kind::Histogram:
        return render::kScopeTableHistogram;
    case scopes::Kind::Chromaticity:
        return render::kScopeTableChromaticity;
    }
    return 0;
}

} // namespace

ScopesEditor::ScopesEditor(const ScopesServices& services, RamPreviewController* const ramPreview,
                           QWidget* const parent)
    : QWidget(parent), services_(services), ramPreview_(ramPreview) {
    setObjectName(QStringLiteral("scopesEditor"));
    setAccessibleName(tr("Scopes"));
    setAccessibleDescription(
        tr("Waveform, parade, vectorscope, histogram and CIE chromaticity of the picture the "
           "active Viewer shows"));
    if (services_.usable()) {
        controller_ = new ScopesController(*services_.scheduler, *services_.bridge, services_.cache,
                                           services_.measure, this);
        renderer_ = new ScopesTraceRenderer(*services_.scheduler, *services_.bridge, this);
        connect(controller_, &ScopesController::readingChanged, this, &ScopesEditor::onReading);
    }

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* top = new kit::KSurface(this, kit::Color::Background);
    top->setObjectName(QStringLiteral("scopesHeader"));
    outer->addWidget(top);
    buildHeader(top);
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("scopesScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll_, 1);
    auto* content = new kit::KSurface(nullptr, kit::Color::Background);
    content->setObjectName(QStringLiteral("scopesContent"));
    scroll_->setWidget(content);
    buildSections(content);

    if (ramPreview_ != nullptr)
        connect(ramPreview_, &RamPreviewController::stateChanged, this,
                [this] { setRamFillRunning(ramPreview_ != nullptr && ramPreview_->isCaching()); });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* const now) {
        for (auto* widget = now; widget != nullptr; widget = widget->parentWidget())
            if (auto* viewer = qobject_cast<ViewerEditor*>(widget)) {
                if (viewer != followedViewer())
                    followViewer(viewer);
                return;
            }
    });
    refreshControls();
    refreshViewsActive();
    refreshStatus();
}

ScopesEditor::~ScopesEditor() {
    QObject::disconnect(viewerConnection_);
    QObject::disconnect(viewerDestroyedConnection_);
    // The views read the renderer's finished images; they go first.
    delete scroll_;
    scroll_ = nullptr;
}

void ScopesEditor::buildHeader(QWidget* const parent) {
    auto* column = new QVBoxLayout(parent);
    column->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::Gutter),
                               kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS));
    column->setSpacing(kit::px(kit::Spacing::XS));

    // Signal and, for the Working signal, the axis maximum M.
    auto* signalRow = new QHBoxLayout;
    signalRow->setSpacing(kit::px(kit::Spacing::S));
    auto* signalLabel = new kit::KLabel(tr("Signal"), parent, kit::TypeRole::UiSmall);
    signalLabel->setTone(kit::Color::Muted);
    signalRow->addWidget(signalLabel);
    signalDropdown_ = new kit::KDropdown(parent);
    signalDropdown_->setObjectName(QStringLiteral("scopesSignal"));
    signalDropdown_->setAccessibleName(tr("Signal"));
    signalDropdown_->addItem(tr("Display"), static_cast<int>(runtime::ScopeSignal::Display));
    signalDropdown_->addItem(tr("Working space"), static_cast<int>(runtime::ScopeSignal::Working));
    signalDropdown_->setMinimumWidth(kit::px(kit::Size::DropdownWidth));
    signalRow->addWidget(signalDropdown_);
    auto* axisLabel = new kit::KLabel(tr("Axis maximum"), parent, kit::TypeRole::UiSmall);
    axisLabel->setObjectName(QStringLiteral("scopesAxisMaximumLabel"));
    axisLabel->setTone(kit::Color::Muted);
    signalRow->addWidget(axisLabel);
    axisDropdown_ = new kit::KDropdown(parent);
    axisDropdown_->setObjectName(QStringLiteral("scopesAxisMaximum"));
    axisDropdown_->setAccessibleName(tr("Axis maximum"));
    for (const auto maximum : kAxisMaxima)
        axisDropdown_->addItem(QString::number(maximum), maximum);
    axisDropdown_->setFixedWidth(kit::px(kit::Size::ViewerZoomWidth));
    signalRow->addWidget(axisDropdown_);
    signalRow->addStretch(1);
    column->addLayout(signalRow);
    connect(signalDropdown_, &kit::KDropdown::activated, this, [this](const int index) {
        setSignal(signalDropdown_->itemData(index).toInt() ==
                          static_cast<int>(runtime::ScopeSignal::Working)
                      ? runtime::ScopeSignal::Working
                      : runtime::ScopeSignal::Display);
    });
    connect(axisDropdown_, &kit::KDropdown::activated, this,
            [this](const int index) { setAxisMaximum(axisDropdown_->itemData(index).toUInt()); });

    // Region (toggle) and Clear Region.
    auto* regionRow = new QHBoxLayout;
    regionRow->setSpacing(kit::px(kit::Spacing::S));
    regionButton_ = new kit::KButton(tr("Region"), parent);
    regionButton_->setObjectName(QStringLiteral("scopesRegion"));
    regionButton_->setCheckable(true);
    regionButton_->setToolTip(tr("Draw a rectangle in the Viewer to measure only that part"));
    regionRow->addWidget(regionButton_);
    clearRegionButton_ = new kit::KButton(tr("Clear Region"), parent);
    clearRegionButton_->setObjectName(QStringLiteral("scopesClearRegion"));
    regionRow->addWidget(clearRegionButton_);
    regionRow->addStretch(1);
    column->addLayout(regionRow);
    connect(regionButton_, &kit::KButton::toggled, this,
            [this](const bool checked) { setRegionArmed(checked); });
    connect(clearRegionButton_, &kit::KButton::clicked, this, [this] { clearRegion(); });

    ignoreCheck_ = new kit::KCheckBox(tr("Ignore Transparent Pixels"), parent);
    ignoreCheck_->setObjectName(QStringLiteral("scopesIgnoreTransparent"));
    column->addWidget(ignoreCheck_);
    connect(ignoreCheck_, &kit::KCheckBox::toggled, this,
            [this](const bool checked) { setIgnoreTransparent(checked); });

    auto* brightnessRow = new QHBoxLayout;
    brightnessRow->setSpacing(kit::px(kit::Spacing::S));
    auto* brightnessLabel = new kit::KLabel(tr("Brightness"), parent, kit::TypeRole::UiSmall);
    brightnessLabel->setTone(kit::Color::Muted);
    brightnessRow->addWidget(brightnessLabel);
    brightnessSlider_ = new kit::KSlider(parent);
    brightnessSlider_->setObjectName(QStringLiteral("scopesBrightness"));
    brightnessSlider_->setAccessibleName(tr("Brightness"));
    brightnessSlider_->setRange(kBrightnessMinimum, kBrightnessMaximum);
    brightnessSlider_->setValue(static_cast<double>(brightness_));
    brightnessSlider_->setDefaultValue(static_cast<double>(scopes::traceBrightnessGainDefault()));
    brightnessRow->addWidget(brightnessSlider_, 1);
    column->addLayout(brightnessRow);
    connect(brightnessSlider_, &kit::KSlider::valueChanged, this,
            [this](const double value) { setBrightness(static_cast<float>(value)); });

    // The status line, always visible under the header.
    statusLine_ = new kit::KLabel(QString{}, parent, kit::TypeRole::UiSmall);
    statusLine_->setObjectName(QStringLiteral("scopesStatusLine"));
    statusLine_->setTone(kit::Color::Muted);
    statusLine_->setWordWrap(true);
    statusLine_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    column->addWidget(statusLine_);
    warningLine_ = new kit::KLabel(QString{}, parent, kit::TypeRole::UiSmall);
    warningLine_->setObjectName(QStringLiteral("scopesStatusWarnings"));
    warningLine_->setTone(kit::Color::Warn);
    warningLine_->setWordWrap(true);
    column->addWidget(warningLine_);
    behindLine_ = new kit::KLabel(QString{}, parent, kit::TypeRole::UiSmall);
    behindLine_->setObjectName(QStringLiteral("scopesStatusBehind"));
    behindLine_->setTone(kit::Color::Warn);
    behindLine_->setWordWrap(true);
    column->addWidget(behindLine_);
}

void ScopesEditor::buildSections(QWidget* const parent) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::Gutter),
                               kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS));
    layout->setSpacing(kit::px(kit::Spacing::Gutter));
    for (const auto& spec : kSections) {
        const auto slot = static_cast<std::size_t>(spec.kind);
        auto* section = new kit::KSection(sectionTitle(spec.kind), parent);
        section->setObjectName(QStringLiteral("scopesSection_") + QLatin1String(spec.id));
        section->setResetEnabled(false);
        section->setBodyPadding(kit::px(kit::Spacing::XS));
        section->bodyLayout()->setSpacing(kit::px(kit::Spacing::XS));
        sections_[slot] = section;
        auto* view = new ScopeView(spec.kind, renderer_, section->body());
        section->bodyLayout()->addWidget(view);
        views_[slot] = view;
        layout->addWidget(section);
        connect(section, &kit::KSection::collapsedChanged, this, [this](bool) {
            refreshViewsActive();
            applySettings();
        });

        const auto addChoice = [&](const QString& name, const QString& accessible,
                                   const QStringList& items) {
            auto* dropdown = new kit::KDropdown(section);
            dropdown->setObjectName(name);
            dropdown->setAccessibleName(accessible);
            for (int index = 0; index < items.size(); ++index)
                dropdown->addItem(items[index], index);
            dropdown->setFixedWidth(kit::px(kit::Size::DropdownWidth));
            section->addHeaderAction(dropdown);
            return dropdown;
        };
        switch (spec.kind) {
        case scopes::Kind::Waveform:
            waveformChannels_ = addChoice(QStringLiteral("scopesWaveformChannels"),
                                          tr("Waveform channels"), {tr("RGB"), tr("Luma")});
            connect(waveformChannels_, &kit::KDropdown::activated, this, [this](const int index) {
                views_[static_cast<std::size_t>(scopes::Kind::Waveform)]->setWaveformChannels(
                    index == 1 ? scopes::WaveformChannels::Luma : scopes::WaveformChannels::Rgb);
            });
            break;
        case scopes::Kind::Histogram:
            histogramChannels_ =
                addChoice(QStringLiteral("scopesHistogramChannels"), tr("Histogram channels"),
                          {tr("RGB"), tr("Luma"), tr("RGB and Luma")});
            histogramScale_ = addChoice(QStringLiteral("scopesHistogramScale"),
                                        tr("Histogram scale"), {tr("Linear"), tr("Logarithmic")});
            connect(histogramChannels_, &kit::KDropdown::activated, this, [this](const int index) {
                views_[static_cast<std::size_t>(scopes::Kind::Histogram)]->setHistogramChannels(
                    index == 1   ? scopes::HistogramChannels::Luma
                    : index == 2 ? scopes::HistogramChannels::RgbAndLuma
                                 : scopes::HistogramChannels::Rgb);
            });
            connect(histogramScale_, &kit::KDropdown::activated, this, [this](const int index) {
                views_[static_cast<std::size_t>(scopes::Kind::Histogram)]->setHistogramScale(
                    index == 1 ? scopes::HistogramScale::Logarithmic
                               : scopes::HistogramScale::Linear);
            });
            break;
        case scopes::Kind::Vectorscope:
            linearNote_ = new kit::KLabel(tr("Linear values"), section, kit::TypeRole::UiSmall);
            linearNote_->setObjectName(QStringLiteral("scopesLinearNote"));
            linearNote_->setTone(kit::Color::Muted);
            section->addHeaderAction(linearNote_);
            break;
        case scopes::Kind::Parade:
        case scopes::Kind::Chromaticity:
            break;
        }
    }
    layout->addStretch(1);
}

QString ScopesEditor::persistencePrefix() const {
    for (const auto* ancestor = parentWidget(); ancestor != nullptr;
         ancestor = ancestor->parentWidget())
        if (const auto* area = qobject_cast<const EditorArea*>(ancestor))
            return QStringLiteral("scopes/%1/sections/").arg(area->areaId());
    return QStringLiteral("scopes/default/sections/");
}

void ScopesEditor::adoptPersistence() {
    if (persistenceAdopted_)
        return;
    persistenceAdopted_ = true;
    const auto prefix = persistencePrefix();
    for (const auto& spec : kSections)
        sections_[static_cast<std::size_t>(spec.kind)]->setPersistenceKey(
            prefix + QLatin1String(spec.id) + QStringLiteral("/collapsed"));
}

std::uint32_t ScopesEditor::openScopeBits() const {
    std::uint32_t bits = 0;
    for (const auto& spec : kSections) {
        const auto* section = sections_[static_cast<std::size_t>(spec.kind)];
        if (section != nullptr && !section->isCollapsed())
            bits |= tableBitOf(spec.kind);
    }
    return bits;
}

runtime::ScopeMeasureSettings ScopesEditor::measureSettings() const {
    runtime::ScopeMeasureSettings settings;
    // A hidden panel measures nothing; a collapsed section's scope is left out.
    settings.scopes = shown_ ? openScopeBits() : 0U;
    settings.signal = signal_;
    settings.axisMaximum = signal_ == runtime::ScopeSignal::Working ? axisMaximum_ : 1U;
    settings.region = region_;
    settings.ignoreTransparent = ignoreTransparent_;
    return settings;
}

runtime::ScopeSignal ScopesEditor::signal() const noexcept { return signal_; }
std::uint32_t ScopesEditor::axisMaximum() const noexcept { return axisMaximum_; }
bool ScopesEditor::ignoreTransparent() const noexcept { return ignoreTransparent_; }
float ScopesEditor::brightness() const noexcept { return brightness_; }

void ScopesEditor::setSignal(const runtime::ScopeSignal signal) {
    if (signal_ == signal)
        return;
    signal_ = signal;
    refreshControls();
    applySettings();
}

void ScopesEditor::setAxisMaximum(const std::uint32_t axisMaximum) {
    if (!runtime::validScopeAxisMaximum(axisMaximum) || axisMaximum_ == axisMaximum)
        return;
    axisMaximum_ = axisMaximum;
    refreshControls();
    applySettings();
}

void ScopesEditor::setIgnoreTransparent(const bool ignore) {
    if (ignoreTransparent_ == ignore)
        return;
    ignoreTransparent_ = ignore;
    refreshControls();
    applySettings();
}

void ScopesEditor::setBrightness(const float gain) {
    const auto clamped = std::clamp(gain, static_cast<float>(kBrightnessMinimum),
                                    static_cast<float>(kBrightnessMaximum));
    if (brightness_ == clamped)
        return;
    brightness_ = clamped;
    refreshControls();
    // A Brightness change maps again and does not measure again.
    for (auto* scopeView : views_)
        scopeView->setGain(brightness_);
}

void ScopesEditor::setRegion(const std::optional<runtime::ScopeRegion>& region) {
    std::optional<runtime::ScopeRegion> next = region;
    if (next && (next->width == 0 || next->height == 0))
        next.reset();
    if (region_ == next)
        return;
    region_ = next;
    refreshControls();
    applySettings();
    emit regionChanged(region_);
}

void ScopesEditor::setRegionArmed(const bool armed) {
    if (regionArmed_ == armed)
        return;
    regionArmed_ = armed;
    refreshControls();
    emit regionArmedChanged(regionArmed_);
}

void ScopesEditor::refreshControls() {
    const QSignalBlocker blockSignal(signalDropdown_);
    const QSignalBlocker blockAxis(axisDropdown_);
    const QSignalBlocker blockRegion(regionButton_);
    const QSignalBlocker blockIgnore(ignoreCheck_);
    const QSignalBlocker blockBrightness(brightnessSlider_);
    signalDropdown_->setCurrentIndex(signalDropdown_->findData(static_cast<int>(signal_)));
    axisDropdown_->setCurrentIndex(axisDropdown_->findData(axisMaximum_));
    const bool working = signal_ == runtime::ScopeSignal::Working;
    axisDropdown_->setVisible(working);
    if (auto* label = findChild<QWidget*>(QStringLiteral("scopesAxisMaximumLabel")))
        label->setVisible(working);
    if (linearNote_ != nullptr)
        linearNote_->setVisible(working);
    regionButton_->setChecked(regionArmed_);
    clearRegionButton_->setEnabled(region_.has_value());
    ignoreCheck_->setChecked(ignoreTransparent_);
    brightnessSlider_->setValue(static_cast<double>(brightness_));
}

void ScopesEditor::refreshViewsActive() {
    for (const auto& spec : kSections) {
        const auto slot = static_cast<std::size_t>(spec.kind);
        views_[slot]->setActive(shown_ && !sections_[slot]->isCollapsed());
    }
}

void ScopesEditor::applySettings() {
    if (controller_ != nullptr)
        controller_->setSettings(measureSettings());
    refreshStatus();
    emit settingsChanged();
    updateRangeFill();
}

void ScopesEditor::setRamFillRunning(const bool running) {
    ramFillRunning_ = running;
    updateRangeFill();
}

void ScopesEditor::updateRangeFill() {
    if (controller_ == nullptr)
        return;
    // While a RAM preview fill is running and the panel is shown with a scope open, the frames the
    // fill retains are measured too.
    controller_->setRangeFillActive(shown_ && ramFillRunning_ && openScopeBits() != 0U);
}

void ScopesEditor::showViewerFrame(const PreparedPreviewFrameHandle& frame,
                                   const runtime::ViewAdjust viewAdjust) {
    hasViewer_ = true;
    viewAdjust_ = viewAdjust;
    if (controller_ != nullptr)
        controller_->showFrame(frame, viewAdjust);
    refreshStatus();
}

ViewerEditor* ScopesEditor::followedViewer() const noexcept {
    return qobject_cast<ViewerEditor*>(followed_.data());
}

void ScopesEditor::followViewer(ViewerEditor* const viewer) {
    if (followedViewer() == viewer && (viewer != nullptr || !hasViewer_))
        return;
    QObject::disconnect(viewerConnection_);
    QObject::disconnect(viewerDestroyedConnection_);
    followed_ = viewer;
    if (viewer != nullptr) {
        viewerConnection_ = connect(viewer, &ViewerEditor::displayedStateChanged, this,
                                    &ScopesEditor::onViewerState);
        viewerDestroyedConnection_ = connect(viewer, &QObject::destroyed, this, [this] {
            // The Viewer is half torn down: forget it, then look for another.
            QObject::disconnect(viewerConnection_);
            followed_.clear();
            pickViewer();
        });
        onViewerState();
        return;
    }
    if (controller_ != nullptr)
        controller_->showFrame(nullptr, {});
    hasViewer_ = false;
    viewAdjust_ = {};
    refreshStatus();
}

void ScopesEditor::onViewerState() {
    auto* const viewer = followedViewer();
    if (viewer == nullptr)
        return;
    const auto source = viewer->scopesSource();
    showViewerFrame(source.frame, source.viewAdjust);
}

void ScopesEditor::pickViewer() {
    if (!followed_.isNull())
        return;
    ViewerEditor* choice = nullptr;
    for (auto* candidate : window()->findChildren<ViewerEditor*>()) {
        if (choice == nullptr || (!choice->isVisible() && candidate->isVisible()))
            choice = candidate;
    }
    if (choice != nullptr)
        followViewer(choice);
}

ScopesStatusText ScopesEditor::statusText() const {
    ScopesStatusInput input;
    input.available = controller_ != nullptr;
    input.hasViewer = hasViewer_;
    input.hidden = !shown_;
    input.measuring = openScopeBits() != 0U;
    if (controller_ != nullptr) {
        input.reading = controller_->reading();
        input.settings = controller_->settings();
    }
    input.viewAdjust = viewAdjust_;
    input.compositionName = services_.compositionName;
    return describeScopesStatus(input);
}

void ScopesEditor::refreshStatus() {
    if (statusLine_ == nullptr)
        return;
    const auto status = statusText();
    statusLine_->setText(status.line);
    warningLine_->setText(status.warnings.join(QStringLiteral(" · ")));
    warningLine_->setVisible(!status.warnings.isEmpty());
    behindLine_->setText(status.behind);
    behindLine_->setVisible(!status.behind.isEmpty());
}

void ScopesEditor::onReading() {
    const auto& reading = controller_->reading();
    ScopeViewData data;
    data.scopes = reading.scopes;
    data.color = reading.color;
    data.behind = reading.behind;
    if (reading.scopes != nullptr) {
        data.signal = reading.scopes->identity.settings.signal;
        data.axisMaximum = reading.scopes->identity.settings.axisMaximum;
    }
    for (auto* scopeView : views_)
        scopeView->setData(data);
    refreshStatus();
}

void ScopesEditor::showEvent(QShowEvent* const event) {
    QWidget::showEvent(event);
    shown_ = true;
    adoptPersistence();
    if (followed_.isNull())
        pickViewer();
    refreshViewsActive();
    applySettings();
}

void ScopesEditor::hideEvent(QHideEvent* const event) {
    QWidget::hideEvent(event);
    shown_ = false;
    refreshViewsActive();
    applySettings();
}

bool registerScopesEditor(EditorRegistry& registry, const ScopesServices& services,
                          RamPreviewController* const ramPreview) {
    QPointer<RamPreviewController> guarded(ramPreview);
    return registry.registerEditor({.id = "bloom.scopes",
                                    .displayName = ScopesEditor::tr("Scopes"),
                                    .create = [services, guarded](QWidget* parent) {
                                        return new ScopesEditor(services, guarded.data(), parent);
                                    }});
}

} // namespace bloom::ui
