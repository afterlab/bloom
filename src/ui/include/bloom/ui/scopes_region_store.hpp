#pragma once

// #724, #730 Scopes: the session's scope state, shared by every Scopes panel and every Viewer of a
// session (docs/ux/ui-grammar.md, "Scopes panel"; docs/architecture/scopes.md). It is session state
// of the application window: never serialized, never part of the document, a command or undo, and
// it changes nothing that renders or exports.
//
// It holds:
//   - one scope region per composition, in whole composition pixels, and the Draw Scope Region
//     toggle's armed flag;
//   - the settings every Scopes panel draws with (the header menu's Signal, Working axis maximum,
//     Ignore Transparent Pixels, and each scope's mode). One set per session: a frame carries one
//     set of tables, so two panels never ask for different ones;
//   - how many Scopes panels are visible. While one is, every Viewer, read-ahead and RAM Preview
//     request asks its frame's preparation to count the scope tables too (request()); with none,
//     nothing is counted;
//   - the color objects of the signals measured and the shared OCIO context the device's scope
//     color steps are prepared from.
// A Viewer's region gesture and a panel's header menu only write here; the frame pipeline counts.

#include <bloom/document/ids.hpp>
#include <bloom/runtime/preview_frame_scopes.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/scope_picture.hpp>

#include <QObject>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace bloom::runtime {
struct GpuSceneOcioContext;
}

namespace bloom::ui {

class ScopesRegionStore final : public QObject {
    Q_OBJECT

  public:
    struct Key final {
        document::ProjectId project;
        document::CompositionId composition;

        friend auto operator<=>(const Key&, const Key&) = default;
    };

    // How the panels draw the scopes: changing it draws again from the same tables.
    struct Drawing final {
        runtime::scopes::WaveformChannels waveformChannels = runtime::scopes::WaveformChannels::Rgb;
        runtime::scopes::HistogramChannels histogramChannels =
            runtime::scopes::HistogramChannels::Rgb;
        runtime::scopes::HistogramScale histogramScale = runtime::scopes::HistogramScale::Linear;

        friend bool operator==(const Drawing&, const Drawing&) = default;
    };

    explicit ScopesRegionStore(QObject* parent = nullptr);

    // ---- The region ---------------------------------------------------------------------
    // The region of one composition; nothing: the whole frame.
    [[nodiscard]] std::optional<runtime::ScopeRegion> region(const Key& key) const;
    // An empty rectangle is no region. Emits regionChanged when the stored value changes.
    void setRegion(const Key& key, std::optional<runtime::ScopeRegion> region);
    void clearRegion(const Key& key) { setRegion(key, std::nullopt); }

    // Draw Scope Region: true while the active Viewer takes a region drag.
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    void setArmed(bool armed);

    // ---- What a frame counts ----------------------------------------------------------------
    [[nodiscard]] runtime::ScopeSignal signal() const noexcept { return signal_; }
    void setSignal(runtime::ScopeSignal signal);
    // The Working signal's axis maximum M (1, 2, 4, 8 or 16); the Display signal's is 1.
    [[nodiscard]] std::uint32_t workingAxisMaximum() const noexcept { return axisMaximum_; }
    void setWorkingAxisMaximum(std::uint32_t axisMaximum);
    [[nodiscard]] bool ignoreTransparent() const noexcept { return ignoreTransparent_; }
    void setIgnoreTransparent(bool ignore);
    // The settings a frame of `key`'s composition is counted under: every table, the signal, the
    // axis maximum, the composition's region and Ignore Transparent Pixels.
    [[nodiscard]] runtime::ScopeMeasureSettings settings(const Key& key) const;
    // What a request for a frame of `key`'s composition asks its preparation to count; null while
    // no Scopes panel is visible. The same object is returned until the settings change.
    [[nodiscard]] std::shared_ptr<const runtime::PreviewScopeRequest> request(const Key& key) const;

    // The shared OCIO context the device's scope color steps are prepared from (resolved on a
    // worker). Without one the frames of the device route count their tables on the CPU.
    void setOcio(std::function<std::shared_ptr<const runtime::GpuSceneOcioContext>()> ocio);
    [[nodiscard]] const std::shared_ptr<runtime::ScopeSignalColorCache>& colors() const noexcept {
        return colors_;
    }

    // ---- How the scopes are drawn ---------------------------------------------------------
    [[nodiscard]] const Drawing& drawing() const noexcept { return drawing_; }
    void setDrawing(const Drawing& drawing);

    // ---- The panels -----------------------------------------------------------------------
    // Scopes panels currently shown. With none, no Viewer draws the region or takes input for it,
    // and no frame counts tables.
    [[nodiscard]] int visiblePanels() const noexcept { return visiblePanels_; }
    [[nodiscard]] bool active() const noexcept { return visiblePanels_ > 0; }
    void panelShown();
    void panelHidden();

  signals:
    void regionChanged(const bloom::ui::ScopesRegionStore::Key& key);
    void armedChanged(bool armed);
    void activeChanged(bool active);
    // What a frame counts changed: the signal, the axis maximum, Ignore Transparent Pixels, a
    // region, or the first panel became visible. Viewers request their frame again.
    void measureChanged();
    // The header menu changed how the scopes are drawn, or what a panel shows of the settings.
    void drawingChanged();

  private:
    void measureSettingsChanged();

    std::map<Key, runtime::ScopeRegion> regions_;
    bool armed_ = false;
    int visiblePanels_ = 0;
    runtime::ScopeSignal signal_ = runtime::ScopeSignal::Display;
    std::uint32_t axisMaximum_ = 1;
    bool ignoreTransparent_ = false;
    Drawing drawing_;
    std::shared_ptr<runtime::ScopeSignalColorCache> colors_;
    std::function<std::shared_ptr<const runtime::GpuSceneOcioContext>()> ocio_;
    mutable std::map<Key, std::shared_ptr<const runtime::PreviewScopeRequest>> requests_;
};

} // namespace bloom::ui
