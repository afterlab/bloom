#pragma once

// #724, #730 Scopes: the Scopes panel, editor `bloom.scopes`, "Scopes" (docs/ux/ui-grammar.md,
// "Scopes panel"; docs/architecture/scopes.md). It follows the active Viewer and draws the scope
// tables the frame it shows carries (PreparedPreviewFrame::scopes) in five collapsible panes:
// Waveform, Parade, Vectorscope, Histogram and CIE Chromaticity. The frame pipeline counts the
// tables; the panel counts nothing. The count-to-image mapping runs on task workers behind the
// trace renderer; this widget only lays out and paints.
//
// The panel is the panes and nothing else: no status, warning or other text, no slider. Its
// settings are in its header menus (EditorChromeProvider): Signal (Display or Working, the Working
// axis maximum, Ignore Transparent Pixels), Region (Draw Scope Region, Clear Scope Region) and
// Scopes (each scope's modes). They are the session's (ScopesRegionStore), shared by every Scopes
// panel. A pane keeps only its title and collapse arrow.
//
// It never dims, fades, blanks or flashes (#730): a frame that does not carry tables of the
// current settings yet leaves the traces on show as they are; the next frame that does replaces
// them, each pane in one swap.

#include <bloom/runtime/preview_frame_scopes.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_drawing.hpp>
#include <bloom/ui/scopes_region_store.hpp>

#include <QPointer>
#include <QWidget>

#include <array>
#include <memory>
#include <optional>

class QAction;
class QActionGroup;
class QMenu;
class QScrollArea;

namespace bloom::runtime {
class TaskScheduler;
}

namespace bloom::ui {

class EditorRegistry;
class ScopeView;
class ScopesTraceRenderer;
class TaskUiBridge;
class ViewerEditor;

namespace kit {
class KSection;
} // namespace kit

// What the panel draws with. All of it outlives every panel.
struct ScopesServices final {
    runtime::TaskScheduler* scheduler = nullptr;
    TaskUiBridge* bridge = nullptr;
    // The session's scope state, shared by every panel and Viewer. Null: the panel keeps a store
    // of its own (no Viewer then counts tables for it).
    ScopesRegionStore* store = nullptr;

    [[nodiscard]] bool usable() const noexcept { return scheduler != nullptr && bridge != nullptr; }
};

class ScopesEditor final : public QWidget, public EditorChromeProvider {
    Q_OBJECT

  public:
    explicit ScopesEditor(const ScopesServices& services, QWidget* parent = nullptr);
    ~ScopesEditor() override;

    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    [[nodiscard]] ScopesRegionStore* store() const noexcept { return store_; }

    // ---- The followed Viewer -------------------------------------------------------------
    void followViewer(ViewerEditor* viewer);
    [[nodiscard]] ViewerEditor* followedViewer() const noexcept;
    // The frame the Viewer shows now (its neutral frame; null: none). What a followed Viewer's
    // displayedStateChanged() hands to the panel; also the seam tests drive.
    void showViewerFrame(const PreparedPreviewFrameHandle& frame);
    // The tables the panes were last handed: the newest frame's that carried tables of the
    // current settings.
    [[nodiscard]] const std::shared_ptr<const runtime::PreviewFrameScopes>&
    scopes() const noexcept {
        return scopes_;
    }

    // ---- Parts ----------------------------------------------------------------------------
    [[nodiscard]] ScopesTraceRenderer* renderer() const noexcept { return renderer_; }
    [[nodiscard]] kit::KSection* section(scopes::Kind kind) const noexcept {
        return sections_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] ScopeView* view(scopes::Kind kind) const noexcept {
        return views_[static_cast<std::size_t>(kind)];
    }
    // The header menus: Signal, Region and Scopes.
    [[nodiscard]] QMenu* signalMenu() const noexcept { return signalMenu_; }
    [[nodiscard]] QMenu* regionMenu() const noexcept { return regionMenu_; }
    [[nodiscard]] QMenu* scopesMenu() const noexcept { return scopesMenu_; }
    [[nodiscard]] bool panelShown() const noexcept { return shown_; }
    // The followed Viewer's composition, whose region the Region menu draws and clears.
    [[nodiscard]] const std::optional<ScopesRegionStore::Key>& regionKey() const noexcept {
        return regionKey_;
    }
    // The narrowest panel that still fits every scope beside the scroll bar.
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void buildHeader();
    void buildSections(QWidget* parent);
    void refreshMenus();
    void refreshViewsActive();
    void adoptPersistence();
    void pickViewer();
    void onViewerState();
    void handOut(std::shared_ptr<const runtime::PreviewFrameScopes> scopes);
    [[nodiscard]] QString persistencePrefix() const;

    ScopesServices services_;
    ScopesRegionStore* store_ = nullptr;
    ScopesTraceRenderer* renderer_ = nullptr;
    EditorChromeSpec chrome_;
    QPointer<QWidget> followed_;
    QMetaObject::Connection viewerConnection_;
    QMetaObject::Connection viewerDestroyedConnection_;
    std::shared_ptr<const runtime::PreviewFrameScopes> scopes_;
    // The followed Viewer's composition, whose region the Region menu draws and clears.
    std::optional<ScopesRegionStore::Key> regionKey_;
    bool shown_ = false;
    bool persistenceAdopted_ = false;
    bool countedVisible_ = false;

    QMenu* signalMenu_ = nullptr;
    QMenu* axisMenu_ = nullptr;
    QMenu* regionMenu_ = nullptr;
    QMenu* scopesMenu_ = nullptr;
    QAction* displayAction_ = nullptr;
    QAction* workingAction_ = nullptr;
    std::array<QAction*, 5> axisActions_{};
    QAction* ignoreTransparentAction_ = nullptr;
    QAction* drawRegionAction_ = nullptr;
    QAction* clearRegionAction_ = nullptr;
    std::array<QAction*, 2> waveformActions_{};
    std::array<QAction*, 3> histogramChannelActions_{};
    std::array<QAction*, 2> histogramScaleActions_{};
    QScrollArea* scroll_ = nullptr;
    std::array<kit::KSection*, scopes::kKindCount> sections_{};
    std::array<ScopeView*, scopes::kKindCount> views_{};
};

// Registers `bloom.scopes`. `services` is copied into the factory.
[[nodiscard]] bool registerScopesEditor(EditorRegistry& registry, const ScopesServices& services);

} // namespace bloom::ui
