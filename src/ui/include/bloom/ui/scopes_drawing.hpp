#pragma once

// #724 Scopes: the mapping from count tables to trace images (docs/architecture/scopes.md and the
// Scopes specification, "Drawing the counts"). The tables are the truth; these functions are
// presentation and have no effect on them. They are pure: they read a packed table and return an
// image, so the panel runs them in a cancellable task on a worker and the interface thread only
// paints the finished image.
//
// Trace brightness of a cell with count n and the largest count m of its table is
// log2(1 + n) / log2(1 + m) (core::specifiedLog2) times the Brightness gain, clamped to 1; a cell
// with n >= 1 is never drawn below kTraceFloor, so a single pixel is never hidden.
//
// Every image is Format_ARGB32_Premultiplied with a transparent background. The Waveform and the
// Parade add their channels, so they are meant to be painted with the Plus composition mode over
// the scope's canvas; the other images are painted with source-over.

#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <QImage>
#include <QRgb>
#include <QSize>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace bloom::ui::scopes {

enum class Kind : std::uint8_t { Waveform, Parade, Vectorscope, Histogram, Chromaticity };
inline constexpr int kKindCount = 5;

enum class WaveformChannels : std::uint8_t { Rgb, Luma };
enum class HistogramChannels : std::uint8_t { Rgb, Luma, RgbAndLuma };
enum class HistogramScale : std::uint8_t { Linear, Logarithmic };

// The least brightness of a cell that holds at least one pixel.
inline constexpr float kTraceFloor = 0.15F;
// The Chromaticity table's extent in x and y; the scope's axes run 0 to 1.
inline constexpr double kChromaticityXMaximum = 0.8;
inline constexpr double kChromaticityYMaximum = 0.9;
inline constexpr double kChromaticityAxisMaximum = 1.0;
// The luma at which the Vectorscope's cells take their color.
inline constexpr float kVectorscopeCellLuma = 0.7F;

// Red, green, blue and luma trace colors.
struct TraceColors final {
    std::array<QRgb, 4> channel{};
};

[[nodiscard]] constexpr float traceBrightnessGainDefault() noexcept { return 1.0F; }

// 0 for a cell with no pixel; otherwise in [kTraceFloor, 1].
[[nodiscard]] float traceBrightness(std::uint32_t count, std::uint32_t largest,
                                    float gain) noexcept;

using Cancelled = std::function<bool()>;

// What the tables say about clipping, for the clip ticks and markers the panel draws.
struct ClipSummary final {
    // Histogram: red, green, blue, luma.
    std::array<std::uint32_t, 4> under{};
    std::array<std::uint32_t, 4> over{};
    // Waveform: per table column, bit 0 under and bit 1 over, for the channels drawn.
    std::vector<std::uint8_t> columns;
};

// The channels each choice draws.
[[nodiscard]] std::vector<std::uint32_t> waveformChannelList(WaveformChannels channels);
[[nodiscard]] std::vector<std::uint32_t> histogramChannelList(HistogramChannels channels);

// `size` is the image's size in pixels. A null image comes back for a cancelled call, a table that
// was not measured or an empty size.
[[nodiscard]] QImage mapWaveform(const runtime::ScopeTables& tables, WaveformChannels channels,
                                 const TraceColors& colors, float gain, QSize size,
                                 const Cancelled& cancelled = {});
// Three panes (red, green, blue) of equal width with `gapPixels` between them.
[[nodiscard]] QImage mapParade(const runtime::ScopeTables& tables, const TraceColors& colors,
                               float gain, QSize size, int gapPixels,
                               const Cancelled& cancelled = {});
[[nodiscard]] QImage mapVectorscope(const runtime::ScopeTables& tables,
                                    const runtime::ScopeLuma& luma, float gain, QSize size,
                                    const Cancelled& cancelled = {});
// The image covers x 0 to kChromaticityXMaximum and y 0 to kChromaticityYMaximum of the axes (see
// chromaticityImageSize). `size` is that image's size.
[[nodiscard]] QImage mapChromaticity(const runtime::ScopeTables& tables, float gain, QSize size,
                                     const Cancelled& cancelled = {});
// The image holds one row of `rowHeight` pixels per channel drawn, stacked top to bottom (red,
// green, blue, then luma), all on one scale: the largest bin of the channels drawn.
[[nodiscard]] QImage mapHistogram(const runtime::ScopeTables& tables, HistogramChannels channels,
                                  HistogramScale scale, const TraceColors& colors, int rowHeight,
                                  int width, const Cancelled& cancelled = {});

// The size of the Chromaticity image for axes `side` pixels long.
[[nodiscard]] QSize chromaticityImageSize(int side);

[[nodiscard]] ClipSummary clipSummary(const runtime::ScopeTables& tables, Kind kind,
                                      WaveformChannels waveformChannels,
                                      HistogramChannels histogramChannels);

// One request to map: everything the mapping reads, by value or by shared handle, so the task owns
// it.
struct TraceRequest final {
    Kind kind = Kind::Waveform;
    std::shared_ptr<const runtime::ScopeTables> tables;
    std::shared_ptr<const runtime::ScopeSignalColor> color;
    TraceColors colors;
    float gain = 1.0F;
    // Waveform, Parade, Vectorscope and Chromaticity: the image size in pixels (Chromaticity: the
    // axes' side, from which the image size is derived). Histogram: width and the row height.
    QSize size;
    int gapPixels = 4;
    WaveformChannels waveformChannels = WaveformChannels::Rgb;
    HistogramChannels histogramChannels = HistogramChannels::Rgb;
    HistogramScale histogramScale = HistogramScale::Linear;
};

struct TraceResult final {
    QImage image;
    ClipSummary clip;
};

// Maps one request. Pure; runs on the calling thread.
[[nodiscard]] TraceResult renderTrace(const TraceRequest& request, const Cancelled& cancelled = {});

} // namespace bloom::ui::scopes
