#pragma once

// #724, #730 Scopes: one scope of the Scopes panel. It paints the graticule and labels as vector
// drawing (one device pixel lines at every scale and device pixel ratio) over the trace image a
// worker mapped from the count tables (scopes_trace_renderer.hpp), and answers the hover readout.
// It measures nothing and maps nothing itself, and it computes no geometry: what it draws besides
// the trace is the graticule plan of runtime/scope_picture.hpp, the same plan the headless scope
// image draws, so the two cannot drift.
//
// It never dims, fades, blanks or labels a trace while new tables arrive or a trace is mapped again
// (#730): the trace on show stays at full strength, with the graticule it was drawn for, until the
// new image replaces both in one swap. Tables of a frame whose region held nothing or that could
// not be counted draw the empty graticule, silently.

#include <bloom/runtime/preview_frame_scopes.hpp>
#include <bloom/runtime/scope_picture.hpp>
#include <bloom/ui/scopes_drawing.hpp>
#include <bloom/ui/scopes_region_store.hpp>
#include <bloom/ui/scopes_trace_renderer.hpp>

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>

namespace bloom::ui {

// The color roles and the spacing the panel draws with, from the interface tokens. The headless
// scope image uses runtime::scopes::defaultScopePalette() and ScopeMetrics{} (the same values,
// which bloom.ui.scopes-picture proves).
[[nodiscard]] runtime::scopes::ScopePalette scopePanelPalette();
[[nodiscard]] runtime::scopes::ScopeMetrics scopePanelMetrics();

class ScopeView final : public QWidget {
    Q_OBJECT

  public:
    ScopeView(scopes::Kind kind, ScopesTraceRenderer* renderer, QWidget* parent = nullptr);
    ~ScopeView() override;

    [[nodiscard]] scopes::Kind kind() const noexcept { return kind_; }
    // The tables of the frame the followed Viewer shows (null: nothing to show). A record without
    // tables (an empty region, a frame that could not be counted) draws the empty graticule.
    void setScopes(std::shared_ptr<const runtime::PreviewFrameScopes> scopes);
    // The tables asked for last, and the ones the trace and graticule on show were drawn from.
    [[nodiscard]] const std::shared_ptr<const runtime::PreviewFrameScopes>&
    scopes() const noexcept {
        return scopes_;
    }
    [[nodiscard]] const std::shared_ptr<const runtime::PreviewFrameScopes>&
    shownScopes() const noexcept {
        return shown_;
    }
    // How the scope is drawn (the header menu's modes); maps again, measures nothing.
    void setDrawing(const ScopesRegionStore::Drawing& drawing);
    // Whether the panel wants this scope's trace mapped (its section is open and the panel shown).
    void setActive(bool active);

    // The value under `position` (widget coordinates) in the scope's units, and the count of that
    // cell; empty when the pointer is outside the plot or nothing is measured.
    [[nodiscard]] QString readoutAt(const QPointF& position) const;
    // The plot rectangle in widget coordinates (the pane that holds the trace and its graticule).
    [[nodiscard]] QRectF plotRect() const;
    // The text measure of the panel's fonts that the plan's geometry reads.
    [[nodiscard]] static runtime::scopes::MeasureText textMeasure();
    // The graticule plan this scope paints now at its size.
    [[nodiscard]] runtime::scopes::ScopeGraticule graticule() const;
    // The trace image this scope paints (null when it paints none).
    [[nodiscard]] const QImage& traceImage() const noexcept { return trace_; }
    // The trace request the view would make now, for tests.
    [[nodiscard]] scopes::TraceRequest currentRequest() const;

    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool event(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    [[nodiscard]] runtime::scopes::GraticuleRequest
    planRequest(int width, int height, const runtime::PreviewFrameScopes* scopes) const;
    [[nodiscard]] runtime::scopes::ScopeGraticule plan(int width, int height) const;
    [[nodiscard]] bool measures(const runtime::PreviewFrameScopes* scopes) const;
    void requestTrace();
    void adoptFinished();
    void paintPlan(QPainter& painter, const runtime::scopes::ScopeGraticule& plan,
                   const runtime::scopes::ScopePalette& palette) const;
    [[nodiscard]] QString axisLabel(int code) const;

    scopes::Kind kind_;
    ScopesTraceRenderer* renderer_;
    // The tables asked for last.
    std::shared_ptr<const runtime::PreviewFrameScopes> scopes_;
    // The tables the trace on show was mapped from, and the graticule is drawn for.
    std::shared_ptr<const runtime::PreviewFrameScopes> shown_;
    ScopesRegionStore::Drawing drawing_;
    // The modes the trace on show was mapped with.
    ScopesRegionStore::Drawing shownDrawing_;
    bool active_ = true;
    QImage trace_;
    QSize requestedSize_;
};

} // namespace bloom::ui
