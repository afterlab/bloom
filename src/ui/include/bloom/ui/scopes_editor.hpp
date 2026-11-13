#pragma once

// #724 Scopes: the Scopes panel, editor `bloom.scopes`, "Scopes" (docs/ux/ui-grammar.md, "Scopes
// panel"). It follows the active Viewer, asks its ScopesController for that frame's count tables,
// and draws five scopes in collapsible sections: Waveform, Parade, Vectorscope, Histogram and CIE
// Chromaticity. Measuring happens on task workers behind the controller and the count-to-image
// mapping on task workers behind the trace renderer; this widget only lays out, labels and paints.
//
// One controller per panel: each panel has its own signal, axis maximum, region and open sections,
// and its own measure to ask for. The color objects of a signal are shared by every panel through
// the one ScopesMeasure the composition root builds.

#include <bloom/document/ids.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/view_adjust.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_controller.hpp>
#include <bloom/ui/scopes_drawing.hpp>
#include <bloom/ui/scopes_status.hpp>

#include <QPointer>
#include <QString>
#include <QWidget>

#include <array>
#include <functional>
#include <memory>
#include <optional>

class QScrollArea;

namespace bloom::runtime {
class TaskScheduler;
}

namespace bloom::ui {

class EditorRegistry;
class RamPreviewController;
class ScopeView;
class ScopesTraceRenderer;
class TaskUiBridge;
class ViewerEditor;

namespace kit {
class KButton;
class KCheckBox;
class KDropdown;
class KLabel;
class KSection;
class KSlider;
} // namespace kit

// What the panel measures with. All of it outlives every panel; null (or an empty measure) makes a
// panel that says scopes are not available in this session.
struct ScopesServices final {
    runtime::TaskScheduler* scheduler = nullptr;
    TaskUiBridge* bridge = nullptr;
    PreviewFrameCacheHandle cache;
    ScopesMeasure measure;
    // The composition's name for the status line; may be empty.
    std::function<QString(document::ProjectId, document::CompositionId)> compositionName;

    [[nodiscard]] bool usable() const noexcept {
        return scheduler != nullptr && bridge != nullptr && cache != nullptr &&
               static_cast<bool>(measure);
    }
};

class ScopesEditor final : public QWidget {
    Q_OBJECT

  public:
    // `ramPreview` (optional) tells the panel when a RAM preview fill is running, so the frames it
    // retains are measured too.
    explicit ScopesEditor(const ScopesServices& services,
                          RamPreviewController* ramPreview = nullptr, QWidget* parent = nullptr);
    ~ScopesEditor() override;

    // ---- What is measured ----------------------------------------------------------------
    // The measure the panel asks for now: the open sections' tables (none while hidden), the
    // signal and axis maximum, the region and Ignore Transparent Pixels.
    [[nodiscard]] runtime::ScopeMeasureSettings measureSettings() const;
    [[nodiscard]] runtime::ScopeSignal signal() const noexcept;
    [[nodiscard]] std::uint32_t axisMaximum() const noexcept;
    void setSignal(runtime::ScopeSignal signal);
    void setAxisMaximum(std::uint32_t axisMaximum);
    [[nodiscard]] bool ignoreTransparent() const noexcept;
    void setIgnoreTransparent(bool ignore);
    [[nodiscard]] float brightness() const noexcept;
    void setBrightness(float gain);

    // ---- The scope region (the seam the Viewer's region gesture connects to) -----------------
    // The rectangle, in composition pixels, that limits what is measured; nothing: the whole
    // frame. Setting it measures again. Session state of the panel: never project state.
    void setRegion(const std::optional<runtime::ScopeRegion>& region);
    [[nodiscard]] const std::optional<runtime::ScopeRegion>& region() const noexcept {
        return region_;
    }
    void clearRegion() { setRegion(std::nullopt); }
    // The Region toggle: true while the panel asks the active Viewer to take a region drag.
    void setRegionArmed(bool armed);
    [[nodiscard]] bool regionArmed() const noexcept { return regionArmed_; }

    // ---- The followed Viewer -------------------------------------------------------------
    void followViewer(ViewerEditor* viewer);
    [[nodiscard]] ViewerEditor* followedViewer() const noexcept;
    // The frame the Viewer shows now (its neutral frame; null: none) and the exposure and gamma it
    // applies. What a followed Viewer's displayedStateChanged() hands to the controller; also the
    // seam tests drive. A call states that a Viewer is being followed.
    void showViewerFrame(const PreparedPreviewFrameHandle& frame, runtime::ViewAdjust viewAdjust);

    // A RAM preview fill is running (the panel follows RamPreviewController::stateChanged). While
    // the panel is shown with a scope open, the frames the fill retains are measured too.
    void setRamFillRunning(bool running);
    [[nodiscard]] bool ramFillRunning() const noexcept { return ramFillRunning_; }

    // ---- Parts ----------------------------------------------------------------------------
    [[nodiscard]] ScopesController* controller() const noexcept { return controller_; }
    [[nodiscard]] ScopesTraceRenderer* renderer() const noexcept { return renderer_; }
    [[nodiscard]] ScopesStatusText statusText() const;
    [[nodiscard]] kit::KSection* section(scopes::Kind kind) const noexcept {
        return sections_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] ScopeView* view(scopes::Kind kind) const noexcept {
        return views_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] bool panelShown() const noexcept { return shown_; }

  signals:
    // Region and Clear Region (and a Viewer's drag, through setRegion) change the region.
    void regionChanged(const std::optional<bloom::runtime::ScopeRegion>& region);
    // The Region toggle changed: true asks the active Viewer to take a region drag.
    void regionArmedChanged(bool armed);
    // The measure the panel asks for changed.
    void settingsChanged();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void buildHeader(QWidget* parent);
    void buildSections(QWidget* parent);
    void applySettings();
    void onReading();
    void refreshStatus();
    void refreshControls();
    void refreshViewsActive();
    void updateRangeFill();
    void adoptPersistence();
    void pickViewer();
    void onViewerState();
    [[nodiscard]] std::uint32_t openScopeBits() const;
    [[nodiscard]] QString persistencePrefix() const;

    ScopesServices services_;
    QPointer<RamPreviewController> ramPreview_;
    ScopesController* controller_ = nullptr;
    ScopesTraceRenderer* renderer_ = nullptr;
    QPointer<QWidget> followed_;
    QMetaObject::Connection viewerConnection_;
    QMetaObject::Connection viewerDestroyedConnection_;
    runtime::ViewAdjust viewAdjust_{};
    bool hasViewer_ = false;
    bool shown_ = false;
    bool persistenceAdopted_ = false;
    bool ramFillRunning_ = false;

    runtime::ScopeSignal signal_ = runtime::ScopeSignal::Display;
    std::uint32_t axisMaximum_ = 1;
    bool ignoreTransparent_ = false;
    float brightness_ = scopes::traceBrightnessGainDefault();
    std::optional<runtime::ScopeRegion> region_;
    bool regionArmed_ = false;

    kit::KDropdown* signalDropdown_ = nullptr;
    kit::KDropdown* axisDropdown_ = nullptr;
    kit::KButton* regionButton_ = nullptr;
    kit::KButton* clearRegionButton_ = nullptr;
    kit::KCheckBox* ignoreCheck_ = nullptr;
    kit::KSlider* brightnessSlider_ = nullptr;
    kit::KLabel* statusLine_ = nullptr;
    kit::KLabel* warningLine_ = nullptr;
    kit::KLabel* behindLine_ = nullptr;
    kit::KLabel* linearNote_ = nullptr;
    kit::KDropdown* waveformChannels_ = nullptr;
    kit::KDropdown* histogramChannels_ = nullptr;
    kit::KDropdown* histogramScale_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    std::array<kit::KSection*, scopes::kKindCount> sections_{};
    std::array<ScopeView*, scopes::kKindCount> views_{};
};

// Registers `bloom.scopes`. `services` is copied into the factory.
[[nodiscard]] bool registerScopesEditor(EditorRegistry& registry, const ScopesServices& services,
                                        RamPreviewController* ramPreview);

} // namespace bloom::ui
