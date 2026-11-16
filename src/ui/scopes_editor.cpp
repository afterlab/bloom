#include <bloom/ui/scopes_editor.hpp>

#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/scopes_trace_renderer.hpp>
#include <bloom/ui/scopes_view.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QMenu>
#include <QMetaObject>
#include <QScrollArea>
#include <QScrollBar>
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

constexpr std::array<std::uint32_t, 5> kAxisMaxima{1, 2, 4, 8, 16};

// The task owner of the panel's shown-frame measures (the trace images use 1 to 5).
constexpr std::uint64_t kMeasureOwner = 16;

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

[[nodiscard]] QAction* checkable(QMenu* const menu, const QString& text, const QString& name,
                                 QActionGroup* const group = nullptr) {
    auto* action = menu->addAction(text);
    action->setObjectName(name);
    action->setCheckable(true);
    if (group != nullptr)
        group->addAction(action);
    return action;
}

} // namespace

ScopesEditor::ScopesEditor(const ScopesServices& services, QWidget* const parent)
    : QWidget(parent), services_(services) {
    setObjectName(QStringLiteral("scopesEditor"));
    setAccessibleName(tr("Scopes"));
    setAccessibleDescription(
        tr("Waveform, parade, vectorscope, histogram and CIE chromaticity of the picture the "
           "active Viewer shows"));
    store_ = services_.store != nullptr ? services_.store : new ScopesRegionStore(this);
    if (services_.usable()) {
        renderer_ = new ScopesTraceRenderer(*services_.scheduler, *services_.bridge, this);
        // #733: the Display signal of the frame the Viewer shows is counted by the runtime, on the
        // preview device or a worker. The measurer's destructor waits out a callback in flight,
        // and it is destroyed first in ~ScopesEditor, so `this` is alive in every call.
        runtime::ShownFrameScopeMeasurer::Options options;
        options.scheduler = services_.scheduler;
        options.owner = {.kind = runtime::TaskOwnerKind::PanelRequest,
                         .id = runtime::TaskOwnerId::fromRaw(kMeasureOwner)};
        options.colors = store_->colors();
        options.ocio = [store = QPointer<ScopesRegionStore>(store_)] {
            // Asked on a worker: only the stored callable is read, which the application sets
            // once before any panel exists.
            return store != nullptr && store->ocio() ? store->ocio()() : nullptr;
        };
        options.device = services_.device;
        options.resultReady = [this] {
            QMetaObject::invokeMethod(this, [this] { onMeasureReady(); }, Qt::QueuedConnection);
        };
        options.beforeCpuCount = services_.measureObserver;
        measurer_ = std::make_unique<runtime::ShownFrameScopeMeasurer>(std::move(options));
    }

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
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
    buildHeader();

    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* const now) {
        for (auto* widget = now; widget != nullptr; widget = widget->parentWidget())
            if (auto* viewer = qobject_cast<ViewerEditor*>(widget)) {
                if (viewer != followedViewer())
                    followViewer(viewer);
                return;
            }
    });
    connect(store_, &ScopesRegionStore::regionChanged, this, [this] { refreshMenus(); });
    connect(store_, &ScopesRegionStore::armedChanged, this, [this] { refreshMenus(); });
    connect(store_, &ScopesRegionStore::drawingChanged, this, [this] {
        refreshMenus();
        for (auto* scopeView : views_)
            scopeView->setDrawing(store_->drawing());
    });
    // A change of what is counted leaves the traces on show until tables of the new settings
    // arrive: the shown frame counted again (Display), or its Working tables counted in the
    // background by the followed Viewer's preview.
    connect(store_, &ScopesRegionStore::measureChanged, this, [this] { onViewerState(); });
    for (auto* scopeView : views_)
        scopeView->setDrawing(store_->drawing());
    refreshMenus();
    refreshViewsActive();
}

ScopesEditor::~ScopesEditor() {
    // No result callback runs once this returned.
    measurer_.reset();
    if (countedVisible_)
        store_->panelHidden();
    QObject::disconnect(viewerConnection_);
    QObject::disconnect(viewerDestroyedConnection_);
    // The views read the renderer's finished images; they go first.
    delete scroll_;
    scroll_ = nullptr;
}

QSize ScopesEditor::minimumSizeHint() const {
    auto hint = QWidget::minimumSizeHint();
    // The scroll area clips what is wider than its viewport, so the body's own minimum width and
    // the vertical scroll bar beside it are part of the panel's.
    if (scroll_ != nullptr && scroll_->widget() != nullptr)
        hint.setWidth(std::max(hint.width(), scroll_->frameWidth() + scroll_->frameWidth() +
                                                 scroll_->widget()->minimumSizeHint().width() +
                                                 scroll_->verticalScrollBar()->sizeHint().width()));
    return hint;
}

void ScopesEditor::buildHeader() {
    auto& bar = chrome_.header;
    bar.owner = this;
    bar.objectName = QStringLiteral("scopesHeaderMenuBar");
    bar.overflowButtonName = QStringLiteral("scopesHeaderOverflowButton");
    bar.overflowMenuName = QStringLiteral("scopesHeaderOverflowMenu");

    // Signal: Display or Working, the Working signal's axis maximum, Ignore Transparent Pixels.
    signalMenu_ = kit::makeMenu(tr("Signal"), this);
    signalMenu_->setObjectName(QStringLiteral("scopesSignalMenu"));
    auto* signalGroup = new QActionGroup(signalMenu_);
    signalGroup->setExclusive(true);
    displayAction_ =
        checkable(signalMenu_, tr("Display"), QStringLiteral("scopesSignalDisplay"), signalGroup);
    workingAction_ = checkable(signalMenu_, tr("Working Space"),
                               QStringLiteral("scopesSignalWorking"), signalGroup);
    connect(displayAction_, &QAction::triggered, this,
            [this] { store_->setSignal(runtime::ScopeSignal::Display); });
    connect(workingAction_, &QAction::triggered, this,
            [this] { store_->setSignal(runtime::ScopeSignal::Working); });
    axisMenu_ = signalMenu_->addMenu(tr("Working Axis Maximum"));
    axisMenu_->setObjectName(QStringLiteral("scopesAxisMaximumMenu"));
    auto* maxima = new QActionGroup(axisMenu_);
    maxima->setExclusive(true);
    for (std::size_t index = 0; index < kAxisMaxima.size(); ++index) {
        const auto maximum = kAxisMaxima[index];
        axisActions_[index] = checkable(axisMenu_, QString::number(maximum),
                                        QStringLiteral("scopesAxisMaximum%1").arg(maximum), maxima);
        connect(axisActions_[index], &QAction::triggered, this,
                [this, maximum] { store_->setWorkingAxisMaximum(maximum); });
    }
    signalMenu_->addSeparator();
    ignoreTransparentAction_ = checkable(signalMenu_, tr("Ignore Transparent Pixels"),
                                         QStringLiteral("scopesIgnoreTransparent"));
    connect(ignoreTransparentAction_, &QAction::toggled, this,
            [this](const bool checked) { store_->setIgnoreTransparent(checked); });

    // Region: draw one in the active Viewer, or clear it.
    regionMenu_ = kit::makeMenu(tr("Region"), this);
    regionMenu_->setObjectName(QStringLiteral("scopesRegionMenu"));
    drawRegionAction_ =
        checkable(regionMenu_, tr("Draw Scope Region"), QStringLiteral("scopesDrawRegion"));
    drawRegionAction_->setToolTip(tr("Draw a rectangle in the Viewer to measure only that part"));
    connect(drawRegionAction_, &QAction::toggled, this,
            [this](const bool checked) { store_->setArmed(checked); });
    clearRegionAction_ = regionMenu_->addAction(tr("Clear Scope Region"));
    clearRegionAction_->setObjectName(QStringLiteral("scopesClearRegion"));
    connect(clearRegionAction_, &QAction::triggered, this, [this] {
        if (regionKey_)
            store_->clearRegion(*regionKey_);
    });

    // Scopes: each scope's modes.
    scopesMenu_ = kit::makeMenu(tr("Scopes"), this);
    scopesMenu_->setObjectName(QStringLiteral("scopesModesMenu"));
    auto* waveform = scopesMenu_->addMenu(tr("Waveform"));
    waveform->setObjectName(QStringLiteral("scopesWaveformMenu"));
    auto* waveformGroup = new QActionGroup(waveform);
    waveformActions_ = {
        checkable(waveform, tr("RGB"), QStringLiteral("scopesWaveformRgb"), waveformGroup),
        checkable(waveform, tr("Luma"), QStringLiteral("scopesWaveformLuma"), waveformGroup)};
    for (std::size_t index = 0; index < waveformActions_.size(); ++index)
        connect(waveformActions_[index], &QAction::triggered, this, [this, index] {
            auto drawing = store_->drawing();
            drawing.waveformChannels =
                index == 1 ? scopes::WaveformChannels::Luma : scopes::WaveformChannels::Rgb;
            store_->setDrawing(drawing);
        });
    auto* histogram = scopesMenu_->addMenu(tr("Histogram"));
    histogram->setObjectName(QStringLiteral("scopesHistogramMenu"));
    auto* channelGroup = new QActionGroup(histogram);
    histogramChannelActions_ = {
        checkable(histogram, tr("RGB"), QStringLiteral("scopesHistogramRgb"), channelGroup),
        checkable(histogram, tr("Luma"), QStringLiteral("scopesHistogramLuma"), channelGroup),
        checkable(histogram, tr("RGB and Luma"), QStringLiteral("scopesHistogramRgbAndLuma"),
                  channelGroup)};
    for (std::size_t index = 0; index < histogramChannelActions_.size(); ++index)
        connect(histogramChannelActions_[index], &QAction::triggered, this, [this, index] {
            auto drawing = store_->drawing();
            drawing.histogramChannels = index == 1   ? scopes::HistogramChannels::Luma
                                        : index == 2 ? scopes::HistogramChannels::RgbAndLuma
                                                     : scopes::HistogramChannels::Rgb;
            store_->setDrawing(drawing);
        });
    histogram->addSeparator();
    auto* scaleGroup = new QActionGroup(histogram);
    histogramScaleActions_ = {
        checkable(histogram, tr("Linear"), QStringLiteral("scopesHistogramLinear"), scaleGroup),
        checkable(histogram, tr("Logarithmic"), QStringLiteral("scopesHistogramLogarithmic"),
                  scaleGroup)};
    for (std::size_t index = 0; index < histogramScaleActions_.size(); ++index)
        connect(histogramScaleActions_[index], &QAction::triggered, this, [this, index] {
            auto drawing = store_->drawing();
            drawing.histogramScale =
                index == 1 ? scopes::HistogramScale::Logarithmic : scopes::HistogramScale::Linear;
            store_->setDrawing(drawing);
        });

    bar.addMenuButton(tr("Signal"), signalMenu_, QStringLiteral("scopesSignalMenuButton"));
    bar.addMenuButton(tr("Region"), regionMenu_, QStringLiteral("scopesRegionMenuButton"));
    bar.addMenuButton(tr("Scopes"), scopesMenu_, QStringLiteral("scopesModesMenuButton"));
    bar.addStretch();
    // The row lives in the area's header; until an area hosts it, it is hidden here.
    if (auto* row = EditorArea::buildChromeRow(bar, this))
        row->hide();
}

void ScopesEditor::buildSections(QWidget* const parent) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::Gutter),
                               kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS));
    layout->setSpacing(kit::px(kit::Spacing::Gutter));
    for (const auto& spec : kSections) {
        const auto slot = static_cast<std::size_t>(spec.kind);
        // A pane is its title, its collapse arrow and its scope: nothing else.
        auto* section = new kit::KSection(sectionTitle(spec.kind), parent);
        section->setObjectName(QStringLiteral("scopesSection_") + QLatin1String(spec.id));
        section->setResetEnabled(false);
        section->setBodyPadding(0);
        sections_[slot] = section;
        auto* view = new ScopeView(spec.kind, renderer_, section->body());
        section->bodyLayout()->addWidget(view);
        views_[slot] = view;
        layout->addWidget(section);
        connect(section, &kit::KSection::collapsedChanged, this,
                [this](bool) { refreshViewsActive(); });
    }
    layout->addStretch(1);
}

void ScopesEditor::refreshMenus() {
    const auto signal = store_->signal();
    const QSignalBlocker blockIgnore(ignoreTransparentAction_);
    const QSignalBlocker blockDraw(drawRegionAction_);
    displayAction_->setChecked(signal == runtime::ScopeSignal::Display);
    workingAction_->setChecked(signal == runtime::ScopeSignal::Working);
    for (std::size_t index = 0; index < kAxisMaxima.size(); ++index)
        axisActions_[index]->setChecked(kAxisMaxima[index] == store_->workingAxisMaximum());
    axisMenu_->setEnabled(signal == runtime::ScopeSignal::Working);
    ignoreTransparentAction_->setChecked(store_->ignoreTransparent());
    drawRegionAction_->setChecked(store_->armed());
    drawRegionAction_->setEnabled(regionKey_.has_value());
    clearRegionAction_->setEnabled(regionKey_.has_value() &&
                                   store_->region(*regionKey_).has_value());
    const auto& drawing = store_->drawing();
    waveformActions_[0]->setChecked(drawing.waveformChannels == scopes::WaveformChannels::Rgb);
    waveformActions_[1]->setChecked(drawing.waveformChannels == scopes::WaveformChannels::Luma);
    histogramChannelActions_[0]->setChecked(drawing.histogramChannels ==
                                            scopes::HistogramChannels::Rgb);
    histogramChannelActions_[1]->setChecked(drawing.histogramChannels ==
                                            scopes::HistogramChannels::Luma);
    histogramChannelActions_[2]->setChecked(drawing.histogramChannels ==
                                            scopes::HistogramChannels::RgbAndLuma);
    histogramScaleActions_[0]->setChecked(drawing.histogramScale == scopes::HistogramScale::Linear);
    histogramScaleActions_[1]->setChecked(drawing.histogramScale ==
                                          scopes::HistogramScale::Logarithmic);
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

void ScopesEditor::refreshViewsActive() {
    for (const auto& spec : kSections) {
        const auto slot = static_cast<std::size_t>(spec.kind);
        views_[slot]->setActive(shown_ && !sections_[slot]->isCollapsed());
    }
}

void ScopesEditor::handOut(std::shared_ptr<const runtime::PreviewFrameScopes> scopes) {
    if (scopes_ == scopes)
        return;
    scopes_ = std::move(scopes);
    for (auto* scopeView : views_)
        scopeView->setScopes(scopes_);
}

void ScopesEditor::forgetMeasure() {
    askedMeasure_.reset();
    if (measurer_ != nullptr)
        measurer_->clear();
}

void ScopesEditor::measureShown(const PreparedPreviewFrameHandle& frame,
                                const runtime::ScopeMeasureSettings& settings) {
    // A hidden panel counts nothing; it asks when it is shown again.
    if (measurer_ == nullptr || !shown_)
        return;
    const auto reduction = store_->tableReduction();
    if (askedMeasure_ && askedMeasure_->frame.lock() == frame &&
        askedMeasure_->settings == settings && askedMeasure_->reduction == reduction)
        return;
    const auto& plan = frame->processIdentity().plan;
    if (plan == nullptr)
        return;
    askedMeasure_ = AskedMeasure{.frame = frame, .settings = settings, .reduction = reduction};
    runtime::ShownFrameScopeRequest request;
    request.frame = frame;
    request.compositionWidth = static_cast<std::uint32_t>(plan->format().width());
    request.compositionHeight = static_cast<std::uint32_t>(plan->format().height());
    request.settings = settings;
    request.reduction = reduction;
    request.tag = ++measureTag_;
    measurer_->show(std::move(request));
}

void ScopesEditor::onMeasureReady() {
    if (measurer_ == nullptr)
        return;
    const auto result = measurer_->take();
    // Tables of settings since changed, or counted before the signal changed, are not drawn: the
    // measure asked after the change follows.
    if (!result || result->scopes == nullptr || !askedMeasure_ ||
        store_->signal() != runtime::ScopeSignal::Display ||
        result->scopes->settings != askedMeasure_->settings ||
        result->reduction != askedMeasure_->reduction)
        return;
    handOut(result->scopes);
}

runtime::ShownFrameScopeCounters ScopesEditor::measureCounters() const {
    return measurer_ != nullptr ? measurer_->counters() : runtime::ShownFrameScopeCounters{};
}

bool ScopesEditor::measuring() const { return measurer_ != nullptr && measurer_->busy(); }

void ScopesEditor::showViewerFrame(const PreparedPreviewFrameHandle& frame) {
    if (frame == nullptr) {
        // No picture at all: the empty graticule.
        forgetMeasure();
        handOut(nullptr);
        return;
    }
    const auto& identity = frame->desiredIdentity();
    const auto settings = store_->settings({identity.projectId, identity.compositionId});
    if (settings.signal == runtime::ScopeSignal::Display) {
        // #733: counted from the frame on show, at display time; nothing is rendered for it.
        measureShown(frame, settings);
        return;
    }
    forgetMeasure();
    const auto& carried = frame->scopes();
    // A frame that does not carry tables of the current settings (yet) changes nothing on show.
    if (carried == nullptr || carried->settings != settings ||
        carried->reduction != store_->tableReduction())
        return;
    handOut(carried);
}

ViewerEditor* ScopesEditor::followedViewer() const noexcept {
    return qobject_cast<ViewerEditor*>(followed_.data());
}

void ScopesEditor::followViewer(ViewerEditor* const viewer) {
    if (followedViewer() == viewer && viewer != nullptr)
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
    regionKey_.reset();
    refreshMenus();
}

void ScopesEditor::onViewerState() {
    auto* const viewer = followedViewer();
    if (viewer == nullptr)
        return;
    const auto key = viewer->scopesRegionKey();
    if (key != regionKey_) {
        regionKey_ = key;
        refreshMenus();
    }
    showViewerFrame(viewer->scopesSource().frame);
}

void ScopesEditor::pickViewer() {
    if (!followed_.isNull() || window() == nullptr)
        return;
    ViewerEditor* choice = nullptr;
    for (auto* candidate : window()->findChildren<ViewerEditor*>()) {
        if (choice == nullptr || (!choice->isVisible() && candidate->isVisible()))
            choice = candidate;
    }
    if (choice != nullptr)
        followViewer(choice);
}

void ScopesEditor::showEvent(QShowEvent* const event) {
    QWidget::showEvent(event);
    shown_ = true;
    if (!countedVisible_) {
        countedVisible_ = true;
        store_->panelShown();
    }
    adoptPersistence();
    if (followed_.isNull())
        pickViewer();
    refreshViewsActive();
    // #733: the frame on show is counted now; a hidden panel asked for nothing.
    onViewerState();
}

void ScopesEditor::hideEvent(QHideEvent* const event) {
    QWidget::hideEvent(event);
    shown_ = false;
    if (countedVisible_) {
        countedVisible_ = false;
        store_->panelHidden();
    }
    forgetMeasure();
    refreshViewsActive();
}

bool registerScopesEditor(EditorRegistry& registry, const ScopesServices& services) {
    return registry.registerEditor(
        {.id = "bloom.scopes",
         .displayName = ScopesEditor::tr("Scopes"),
         .create = [services](QWidget* parent) { return new ScopesEditor(services, parent); }});
}

} // namespace bloom::ui
