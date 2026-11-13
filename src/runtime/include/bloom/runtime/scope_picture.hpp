#pragma once

// #724 Scopes: the one Qt-free definition of what a scope looks like.
//
// The Scopes panel and the headless scope image (MCP and scripting sessions have no Qt) draw from
// the same three definitions, so they cannot drift:
//   1. The trace mapping: count tables to a premultiplied 8-bit image (brightness, the visible
//      floor, the clip summary). The panel maps on a task worker; the headless image maps in place.
//   2. The graticule plan: from the scope kind, a logical size, a device pixel ratio, the settings
//      that affect drawing, the luma coefficients, the colorimetry and a text-measuring callback,
//      a plain description of everything a scope shows besides the trace: the plot and trace
//      rectangles, hairlines, polylines, circles, filled clip markers and labels, each with a color
//      role, in logical coordinates. The panel paints it with its own vector painter and the UI
//      fonts; the headless image rasterizes it with one device pixel lines and the embedded face.
//   3. The palette: the color roles a scope uses. The panel fills it from the interface tokens; the
//      headless image uses defaultScopePalette(), whose values a UI test ties to those tokens.
//
// See docs/architecture/scopes.md ("The scope picture").

#include <bloom/color/scope_colorimetry.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime::scopes {

// ---------------------------------------------------------------------------------------------
// Settings and constants

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
// Logical pixels of one Histogram row, a Parade gap, and the clip markers.
inline constexpr int kHistogramRowHeight = 56;
inline constexpr int kParadeGap = 4;
inline constexpr int kClipMarkerWidth = 3;

// ---------------------------------------------------------------------------------------------
// The trace mapping

// A plain size in pixels.
struct PixelSize final {
    int width = 0;
    int height = 0;

    [[nodiscard]] bool isEmpty() const noexcept { return width <= 0 || height <= 0; }
    friend bool operator==(const PixelSize&, const PixelSize&) = default;
};

// Premultiplied 8-bit RGBA, row-major and tightly packed, one pixel per 32-bit word as
// 0xAARRGGBB (the channel order of a little-endian ARGB32 premultiplied surface).
struct ScopeImage final {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;

    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
    [[nodiscard]] std::uint32_t pixel(int x, int y) const noexcept {
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)];
    }
    friend bool operator==(const ScopeImage&, const ScopeImage&) = default;
};

[[nodiscard]] constexpr std::uint32_t packArgb(const unsigned alpha, const unsigned red,
                                               const unsigned green, const unsigned blue) noexcept {
    return (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
}
[[nodiscard]] constexpr unsigned alphaOf(const std::uint32_t pixel) noexcept {
    return pixel >> 24U;
}
[[nodiscard]] constexpr unsigned redOf(const std::uint32_t pixel) noexcept {
    return (pixel >> 16U) & 0xFFU;
}
[[nodiscard]] constexpr unsigned greenOf(const std::uint32_t pixel) noexcept {
    return (pixel >> 8U) & 0xFFU;
}
[[nodiscard]] constexpr unsigned blueOf(const std::uint32_t pixel) noexcept {
    return pixel & 0xFFU;
}

// Red, green, blue and luma trace colors as 0xAARRGGBB (the alpha is ignored).
struct TraceColors final {
    std::array<std::uint32_t, 4> channel{};
};

[[nodiscard]] constexpr float traceBrightnessGainDefault() noexcept { return 1.0F; }

// 0 for a cell with no pixel; otherwise in [kTraceFloor, 1]. log2(1 + n) / log2(1 + m) times the
// gain with core::specifiedLog2, clamped to 1.
[[nodiscard]] float traceBrightness(std::uint32_t count, std::uint32_t largest,
                                    float gain) noexcept;

using Cancelled = std::function<bool()>;

// What the tables say about clipping, for the clip ticks and markers.
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

// `size` is the image's size in pixels. An empty image comes back for a cancelled call, a table
// that was not measured or an empty size.
[[nodiscard]] ScopeImage mapWaveform(const ScopeTables& tables, WaveformChannels channels,
                                     const TraceColors& colors, float gain, PixelSize size,
                                     const Cancelled& cancelled = {});
// Three panes (red, green, blue) of equal width with `gapPixels` between them.
[[nodiscard]] ScopeImage mapParade(const ScopeTables& tables, const TraceColors& colors, float gain,
                                   PixelSize size, int gapPixels, const Cancelled& cancelled = {});
[[nodiscard]] ScopeImage mapVectorscope(const ScopeTables& tables, const ScopeLuma& luma,
                                        float gain, PixelSize size,
                                        const Cancelled& cancelled = {});
// The image covers x 0 to kChromaticityXMaximum and y 0 to kChromaticityYMaximum of the axes (see
// chromaticityImageSize). `size` is that image's size.
[[nodiscard]] ScopeImage mapChromaticity(const ScopeTables& tables, float gain, PixelSize size,
                                         const Cancelled& cancelled = {});
// The image holds one row of `rowHeight` pixels per channel drawn, stacked top to bottom (red,
// green, blue, then luma), all on one scale: the largest bin of the channels drawn.
[[nodiscard]] ScopeImage mapHistogram(const ScopeTables& tables, HistogramChannels channels,
                                      HistogramScale scale, const TraceColors& colors,
                                      int rowHeight, int width, const Cancelled& cancelled = {});

// The size of the Chromaticity image for axes `side` pixels long.
[[nodiscard]] PixelSize chromaticityImageSize(int side);

[[nodiscard]] ClipSummary clipSummary(const ScopeTables& tables, Kind kind,
                                      WaveformChannels waveformChannels,
                                      HistogramChannels histogramChannels);

// One request to map. Pointers are borrowed for the call.
struct TraceRequest final {
    Kind kind = Kind::Waveform;
    const ScopeTables* tables = nullptr;
    // The Vectorscope's luma coefficients; null maps no Vectorscope.
    const ScopeLuma* luma = nullptr;
    TraceColors colors;
    float gain = 1.0F;
    // Waveform, Parade, Vectorscope and Chromaticity: the image size in pixels (Chromaticity: the
    // axes' side, from which the image size is derived). Histogram: width and the row height.
    PixelSize size;
    int gapPixels = 4;
    WaveformChannels waveformChannels = WaveformChannels::Rgb;
    HistogramChannels histogramChannels = HistogramChannels::Rgb;
    HistogramScale histogramScale = HistogramScale::Linear;
};

struct TraceResult final {
    ScopeImage image;
    ClipSummary clip;
};

// Maps one request. Pure; runs on the calling thread.
[[nodiscard]] TraceResult renderTrace(const TraceRequest& request, const Cancelled& cancelled = {});

// ---------------------------------------------------------------------------------------------
// The palette

enum class ColorRole : std::uint8_t {
    Canvas,
    // Graticule lines: quiet, stronger, strongest.
    Quiet,
    Stronger,
    Strongest,
    // Labels, the Vectorscope's ticks and brackets, the gamut triangle.
    Label,
    // The skin tone line.
    Faint,
    // Clip markers.
    Warning,
    Red,
    Green,
    Blue,
    Luma,
};

// The color roles of a scope as 0xAARRGGBB with full alpha.
struct ScopePalette final {
    std::uint32_t canvas = 0;
    std::uint32_t quiet = 0;
    std::uint32_t stronger = 0;
    std::uint32_t strongest = 0;
    std::uint32_t label = 0;
    std::uint32_t faint = 0;
    std::uint32_t warning = 0;
    // Red, green, blue and luma.
    std::array<std::uint32_t, 4> channel{};

    [[nodiscard]] std::uint32_t color(ColorRole role) const noexcept;
    [[nodiscard]] TraceColors traceColors() const noexcept { return {channel}; }

    friend bool operator==(const ScopePalette&, const ScopePalette&) = default;
};

// The values of the interface tokens the panel uses (bloom.ui.scopes-picture proves it). This is
// the one place literal color values appear outside the kit, and only because that test ties each
// of them to its token.
[[nodiscard]] ScopePalette defaultScopePalette() noexcept;

// ---------------------------------------------------------------------------------------------
// The graticule plan

struct Point final {
    double x = 0.0;
    double y = 0.0;
};

struct Rect final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] double left() const noexcept { return x; }
    [[nodiscard]] double top() const noexcept { return y; }
    [[nodiscard]] double right() const noexcept { return x + width; }
    [[nodiscard]] double bottom() const noexcept { return y + height; }
    [[nodiscard]] Point center() const noexcept { return {x + width / 2, y + height / 2}; }
};

// Whether a primitive is painted under the trace (the graticule) or over it.
enum class Layer : std::uint8_t { UnderTrace, OverTrace };

// One hairline, one device pixel wide at every scale. An antialiased line may sit between device
// pixels; a line that is not antialiased is axis aligned and already snapped to device pixels.
struct Line final {
    Point from;
    Point to;
    ColorRole color = ColorRole::Quiet;
    Layer layer = Layer::UnderTrace;
    bool antialiased = false;
    bool dashed = false;
};

// An open or closed run of hairlines: the spectral locus, the gamut triangle.
struct Polyline final {
    std::vector<Point> points;
    bool closed = false;
    ColorRole color = ColorRole::Quiet;
    Layer layer = Layer::OverTrace;
};

// An antialiased hairline circle: the Vectorscope's ring and the white point.
struct Circle final {
    Point center;
    double radius = 0.0;
    ColorRole color = ColorRole::Quiet;
    Layer layer = Layer::UnderTrace;
};

// A filled rectangle: a clip marker. Always over the trace.
struct Fill final {
    Rect rect;
    ColorRole color = ColorRole::Warning;
};

// Which of the two type roles a label uses: numbers and axis values in the numeric face, names
// (channel letters, targets, the color space) in the small interface face.
enum class LabelFont : std::uint8_t { Numeric, Name };
enum class HorizontalAlign : std::uint8_t { Left, Center, Right };
enum class VerticalAlign : std::uint8_t { Top, Middle };

// A label: its text box has the anchor point at the given horizontal edge or center and the given
// top or middle. Always over the trace.
struct Label final {
    std::string text;
    Point anchor;
    HorizontalAlign horizontal = HorizontalAlign::Left;
    VerticalAlign vertical = VerticalAlign::Top;
    LabelFont font = LabelFont::Numeric;
    ColorRole color = ColorRole::Label;
};

struct TextSize final {
    double width = 0.0;
    double height = 0.0;
};
// The logical size of `text` set in the given face. The height depends on the face only.
using MeasureText = std::function<TextSize(std::string_view text, LabelFont font)>;

// The spacing and tick tokens the geometry uses, in logical (design) pixels. The defaults equal the
// interface tokens (Spacing::S, Spacing::XS, Spacing::XXS, Size::MajorTick, Size::MinorTick); the
// panel fills them from the tokens and bloom.ui.scopes-picture proves the defaults agree.
struct ScopeMetrics final {
    double padding = 8.0;
    double small = 4.0;
    double tiny = 2.0;
    double majorTick = 8.0;
    double minorTick = 4.0;
};

// What the plan reads. `measure` is required.
struct GraticuleRequest final {
    Kind kind = Kind::Waveform;
    // The scope's logical size.
    double width = 0.0;
    double height = 0.0;
    double devicePixelRatio = 1.0;
    ScopeSignal signal = ScopeSignal::Display;
    // The Working signal's axis maximum M (1 for Display).
    std::uint32_t axisMaximum = 1;
    WaveformChannels waveformChannels = WaveformChannels::Rgb;
    HistogramChannels histogramChannels = HistogramChannels::Rgb;
    HistogramScale histogramScale = HistogramScale::Linear;
    // The Vectorscope's targets and the Chromaticity's gamut triangle need these; without them the
    // plan omits those parts.
    const ScopeLuma* luma = nullptr;
    const color::ScopeColorimetry* colorimetry = nullptr;
    // The clip markers; none without it.
    const ClipSummary* clip = nullptr;
    ScopeMetrics metrics;
    MeasureText measure;
};

// Everything a scope draws besides its trace. Painting order: the UnderTrace lines, polylines and
// circles, the trace, then the OverTrace lines, polylines and circles, the fills, the labels.
struct ScopeGraticule final {
    Kind kind = Kind::Waveform;
    double width = 0.0;
    double height = 0.0;
    double devicePixelRatio = 1.0;
    // A whole number of device pixels, in logical pixels.
    double hairlineWidth = 1.0;
    // The pane that holds the trace and its graticule.
    Rect plot;
    // Where the trace image goes inside the plot, before snapping to device pixels.
    Rect image;
    // `image` snapped to device pixels with the size of the mapped image: where the trace is drawn.
    Rect trace;
    // Histogram: the height of one row and the rows drawn.
    double rowHeight = 0.0;
    int rows = 0;
    // The size to request for the trace mapping (see TraceRequest::size), and the size of the image
    // that mapping returns.
    PixelSize traceRequestSize;
    PixelSize traceImageSize;
    // The Waveform's pane, or the Parade's three.
    std::vector<Rect> panes;
    std::vector<Line> lines;
    std::vector<Polyline> polylines;
    std::vector<Circle> circles;
    std::vector<Fill> fills;
    std::vector<Label> labels;
};

// Pure; no allocation beyond the result. An empty or non-finite size gives an empty plan.
[[nodiscard]] ScopeGraticule buildScopeGraticule(const GraticuleRequest& request);

// The height a scope wants at `width` logical pixels (the panel's heightForWidth). Reads the kind,
// the Histogram channels, the metrics and `measure`.
[[nodiscard]] int scopeHeightForWidth(const GraticuleRequest& request, int width);

// The text of a code label: the code for the Display signal, the linear value for the Working
// signal at the axis maximum, three significant digits.
[[nodiscard]] std::string scopeAxisLabel(int code, ScopeSignal signal, std::uint32_t axisMaximum);

// The Vectorscope's 75% bar target names and positions (cb, cr) for the luma coefficients.
struct VectorscopeTarget final {
    const char* name;
    double cb;
    double cr;
};
[[nodiscard]] std::array<VectorscopeTarget, 6> vectorscopeTargets(const ScopeLuma& luma);
inline constexpr double kSkinToneDegrees = 123.0;

// ---------------------------------------------------------------------------------------------
// The picture

enum class PictureError : std::uint8_t {
    None,
    Cancelled,
    // A size that is not positive, too large, or a device pixel ratio that is not positive.
    InvalidSize,
    // The picture would need more bytes than the limit.
    ByteLimitExceeded,
    // A glyph could not be rasterized (a build-integrity failure of the embedded face).
    TextFailed,
};

struct PictureResult final {
    PictureError error = PictureError::None;
    ScopeImage image;

    [[nodiscard]] bool ok() const noexcept { return error == PictureError::None; }
};

// What a picture shows. Pointers are borrowed for the call; a scope without measured tables draws
// its canvas and graticule only.
struct PictureInput final {
    const ScopeTables* tables = nullptr;
    const ScopeLuma* luma = nullptr;
    const color::ScopeColorimetry* colorimetry = nullptr;
    ScopeSignal signal = ScopeSignal::Display;
    std::uint32_t axisMaximum = 1;
    float gain = 1.0F;
    WaveformChannels waveformChannels = WaveformChannels::Rgb;
    HistogramChannels histogramChannels = HistogramChannels::Rgb;
    HistogramScale histogramScale = HistogramScale::Linear;
};

struct PictureOptions final {
    // Device pixels per logical pixel; the logical size of a picture is its pixel size over this.
    double devicePixelRatio = 1.0;
    ScopePalette palette = defaultScopePalette();
    ScopeMetrics metrics;
};

// One scope drawn into `pixelWidth` x `pixelHeight` pixels: canvas, graticule, trace (additive for
// the Waveform and the Parade, as the panel composes it), clip markers and labels. Lines are one
// device pixel; labels are rasterized with the embedded face closest to the panel's. Never throws
// for bad input; the error says why nothing was drawn. `byteLimit` bounds the picture's bytes.
[[nodiscard]] PictureResult renderScopePicture(Kind kind, int pixelWidth, int pixelHeight,
                                               const PictureInput& input,
                                               const PictureOptions& options,
                                               const CancellationToken& cancellation,
                                               std::size_t byteLimit);

// One picture of the five scopes stacked in the panel's order (Waveform, Parade, Vectorscope,
// Histogram, Chromaticity), each as tall as the panel's heightForWidth gives for `pixelWidth`.
struct AllScopesPicture final {
    PictureError error = PictureError::None;
    ScopeImage image;
    // Each scope's part: its top row and height in the picture, in pixels.
    std::array<int, kKindCount> top{};
    std::array<int, kKindCount> height{};

    [[nodiscard]] bool ok() const noexcept { return error == PictureError::None; }
};
[[nodiscard]] AllScopesPicture renderAllScopesPicture(int pixelWidth, const PictureInput& input,
                                                      const PictureOptions& options,
                                                      const CancellationToken& cancellation,
                                                      std::size_t byteLimit);

// The size in pixels that renderAllScopesPicture gives for `pixelWidth`, without drawing; the
// empty size for a width that is not valid. Only the Histogram channels of `input` matter.
[[nodiscard]] PixelSize allScopesPictureSize(int pixelWidth, const PictureInput& input,
                                             const PictureOptions& options);

// A text measure with the embedded face the picture uses, for the logical sizes of
// `devicePixelRatio`.
[[nodiscard]] MeasureText embeddedTextMeasure(double devicePixelRatio);

// ---------------------------------------------------------------------------------------------
// PNG

enum class PngError : std::uint8_t {
    None,
    // An empty image or one whose pixel count does not match its size.
    InvalidImage,
    // The encoded bytes would exceed the limit.
    ByteLimitExceeded,
    CompressorFailure,
};

struct PngResult final {
    PngError error = PngError::None;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] bool ok() const noexcept { return error == PngError::None; }
};

// The image as a PNG: 8-bit RGBA (color type 6), straight alpha, sRGB chunk, one IDAT stream, no
// other chunk. Independent of a project and of the export job. `byteLimit` bounds the encoded
// size.
[[nodiscard]] PngResult encodeScopePng(const ScopeImage& image, std::size_t byteLimit);

} // namespace bloom::runtime::scopes
