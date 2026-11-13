#pragma once

// #724 Scopes: one scope of the Scopes panel. It paints the graticule and labels as vector drawing
// (one device pixel lines at every scale and device pixel ratio) over the trace image a worker
// mapped from the count tables (scopes_trace_renderer.hpp), and answers the hover readout. It
// measures nothing and maps nothing itself, and it computes no geometry: what it draws besides the
// trace is the graticule plan of runtime/scope_picture.hpp, the same plan the headless scope image
// draws, so the two cannot drift.

#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_picture.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_drawing.hpp>
#include <bloom/ui/scopes_trace_renderer.hpp>

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <memory>

namespace bloom::ui {

// The color roles and the spacing the panel draws with, from the interface tokens. The headless
// scope image uses runtime::scopes::defaultScopePalette() and ScopeMetrics{} (the same values,
// which bloom.ui.scopes-picture proves).
[[nodiscard]] runtime::scopes::ScopePalette scopePanelPalette();
[[nodiscard]] runtime::scopes::ScopeMetrics scopePanelMetrics();

// What a scope shows: the tables of one measured frame and how its signal is labeled.
struct ScopeViewData final {
    PreviewScopeSidecarHandle scopes;
    std::shared_ptr<const runtime::ScopeSignalColor> color;
    // The tables are another frame's than the Viewer's.
    bool behind = false;
    runtime::ScopeSignal signal = runtime::ScopeSignal::Display;
    // The Working signal's axis maximum M (1 for Display).
    std::uint32_t axisMaximum = 1;
};

class ScopeView final : public QWidget {
    Q_OBJECT

  public:
    ScopeView(scopes::Kind kind, ScopesTraceRenderer* renderer, QWidget* parent = nullptr);
    ~ScopeView() override;

    [[nodiscard]] scopes::Kind kind() const noexcept { return kind_; }
    void setData(const ScopeViewData& data);
    [[nodiscard]] const ScopeViewData& data() const noexcept { return data_; }
    // The Brightness gain of the trace; a change maps again and does not measure again.
    void setGain(float gain);
    void setWaveformChannels(scopes::WaveformChannels channels);
    void setHistogramChannels(scopes::HistogramChannels channels);
    void setHistogramScale(scopes::HistogramScale scale);
    // Whether the panel wants this scope's trace mapped (its section is open and the panel shown).
    void setActive(bool active);

    // The value under `position` (widget coordinates) in the scope's units, and the count of that
    // cell; empty when the pointer is outside the plot or nothing is measured.
    [[nodiscard]] QString readoutAt(const QPointF& position) const;
    // The plot rectangle in widget coordinates (the pane that holds the trace and its graticule).
    [[nodiscard]] QRectF plotRect() const;
    // The graticule plan this scope paints now at its size, clip markers included.
    [[nodiscard]] runtime::scopes::ScopeGraticule graticule() const;
    // The last finished image this scope paints (null before the first).
    [[nodiscard]] const QImage& traceImage() const noexcept { return trace_; }
    [[nodiscard]] const scopes::ClipSummary& clipSummary() const noexcept { return clip_; }
    // True while the scope paints data of another frame than the Viewer's: the tables are behind
    // (the controller says so) or the image on show was mapped from earlier tables. It is always
    // painted dimmed with the word "Behind".
    [[nodiscard]] bool showsBehind() const;
    // The trace request the view makes now, for tests.
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
    [[nodiscard]] runtime::scopes::GraticuleRequest planRequest(int width, int height,
                                                                bool withClip) const;
    [[nodiscard]] runtime::scopes::ScopeGraticule plan(int width, int height,
                                                       bool withClip = false) const;
    [[nodiscard]] bool measured() const;
    void requestTrace();
    void adoptFinished();
    void paintPlan(QPainter& painter, const runtime::scopes::ScopeGraticule& plan,
                   const runtime::scopes::ScopePalette& palette) const;
    void paintTrace(QPainter& painter, const QRectF& target, bool additive) const;
    [[nodiscard]] QString axisLabel(int code) const;

    scopes::Kind kind_;
    ScopesTraceRenderer* renderer_;
    ScopeViewData data_;
    float gain_ = 1.0F;
    scopes::WaveformChannels waveformChannels_ = scopes::WaveformChannels::Rgb;
    scopes::HistogramChannels histogramChannels_ = scopes::HistogramChannels::Rgb;
    scopes::HistogramScale histogramScale_ = scopes::HistogramScale::Linear;
    bool active_ = true;
    QImage trace_;
    // The tables `trace_` was mapped from.
    const runtime::ScopeTables* traceTables_ = nullptr;
    scopes::ClipSummary clip_;
    QSize requestedSize_;
};

} // namespace bloom::ui
