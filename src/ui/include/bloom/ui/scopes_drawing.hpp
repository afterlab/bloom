#pragma once

// #724, #730 Scopes: the panel's side of the count-to-image mapping (docs/architecture/scopes.md,
// "Drawing the counts"). The mapping itself is Qt-free and lives in runtime/scope_picture.hpp,
// where the headless scope image uses it too; this header wraps its premultiplied 8-bit buffers in
// QImage for the panel. The tables are the truth; the mapping is presentation and has no effect on
// them. It is pure: it reads packed tables and returns an image, so the panel runs it in a
// cancellable task on a worker and the interface thread only paints the finished image.
//
// The trace intensity is fixed (runtime/scope_picture.hpp, "The trace intensity"): light adds
// linearly and saturates; there is no brightness control, no floor and no normalization to the
// frame.
//
// Every image is Format_ARGB32_Premultiplied with a transparent background. The Waveform, the
// Parade and the Vectorscope are light: they are painted with the Plus composition mode over the
// scope's black canvas; the Histogram and the Chromaticity are painted with source-over.

#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/scope_picture.hpp>

#include <QImage>
#include <QSize>

#include <memory>

namespace bloom::ui::scopes {

using runtime::scopes::Cancelled;
using runtime::scopes::histogramChannelList;
using runtime::scopes::HistogramChannels;
using runtime::scopes::HistogramScale;
using runtime::scopes::kChromaticityAxisMaximum;
using runtime::scopes::kChromaticityXMaximum;
using runtime::scopes::kChromaticityYMaximum;
using runtime::scopes::Kind;
using runtime::scopes::kKindCount;
using runtime::scopes::waveformChannelList;
using runtime::scopes::WaveformChannels;

// A premultiplied buffer of the Qt-free mapping as a QImage (a copy; null for an empty buffer).
[[nodiscard]] QImage toQImage(const runtime::scopes::ScopeImage& image);

// Whether the trace of `kind` is drawn additively (Plus) rather than source-over.
[[nodiscard]] constexpr bool additiveTrace(const Kind kind) noexcept {
    return kind == Kind::Waveform || kind == Kind::Parade || kind == Kind::Vectorscope;
}

// One request to map: everything the mapping reads, by value or by shared handle, so the task owns
// it.
struct TraceRequest final {
    Kind kind = Kind::Waveform;
    std::shared_ptr<const runtime::ScopeTables> tables;
    std::shared_ptr<const runtime::ScopeSignalColor> color;
    // Waveform, Parade, Vectorscope and Chromaticity: the image size in pixels (Chromaticity: the
    // axes' side, from which the image size is derived). Histogram: width and the row height.
    QSize size;
    int gapPixels = 4;
    WaveformChannels waveformChannels = WaveformChannels::Rgb;
    HistogramChannels histogramChannels = HistogramChannels::Rgb;
    HistogramScale histogramScale = HistogramScale::Linear;
};

// Maps one request. Pure; runs on the calling thread. Null for a cancelled call or nothing to map.
[[nodiscard]] QImage renderTrace(const TraceRequest& request, const Cancelled& cancelled = {});

} // namespace bloom::ui::scopes
