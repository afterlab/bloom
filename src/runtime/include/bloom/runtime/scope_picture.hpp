#pragma once

// #724 Scopes: the one Qt-free definition of what a scope looks like.
//
// The Scopes panel and the headless scope image (MCP and scripting sessions have no Qt) draw from
// the same three definitions, so they cannot drift:
//   1. The trace mapping: count tables to a premultiplied 8-bit image (the fixed, linear,
//      saturating intensity of #730). The panel maps on a task worker; the headless image maps in
//      place.
//   2. The graticule plan: from the scope kind, a logical size, a device pixel ratio, the settings
//      that affect drawing, the luma coefficients, the colorimetry and a text-measuring callback,
//      a plain description of everything a scope shows besides the trace: the plot and trace
//      rectangles, hairlines, polylines, circles and labels, each with a color
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

// The Chromaticity table's extent in x and y; the scope's axes run 0 to 1.
inline constexpr double kChromaticityXMaximum = 0.8;
inline constexpr double kChromaticityYMaximum = 0.9;
inline constexpr double kChromaticityAxisMaximum = 1.0;
// The Vectorscope's plot spans Cb and Cr from -kVectorscopeExtent to +kVectorscopeExtent (the
// table's extent), so its ring (|CbCr| = kScopeVectorscopeRing) sits just inside the plot.
inline constexpr double kVectorscopeExtent = 0.625;
// Logical pixels of one Histogram row and of a Parade gap.
inline constexpr int kHistogramRowHeight = 56;
inline constexpr int kParadeGap = 4;

// The trace intensity (#730, docs/architecture/scopes.md "Trace intensity"): light adds linearly
// and saturates. A screen pixel's density is the share of the pixels it stands for that land in
// it, relative to an even spread (1 when the pixels spread evenly over the whole axis or plane):
//   Waveform and Parade: the share of the screen column's source pixels (the pixels of the table
//     columns it shows, every one counted once) in the levels of the screen row, times 1024 over
//     the levels of that row;
//   Vectorscope: the share of the counted pixels in the cells of the screen pixel, times
//     640 x 640 over the cells of that pixel.
// Its intensity is the gain times the density, not clamped and not normalized to the frame; each
// channel adds its color times its intensity and every component is clamped to 1, so dense areas
// whiten. Nothing is raised to a visible floor.
inline constexpr float kWaveformTraceGain = 0.25F;
inline constexpr float kVectorscopeTraceGain = 0.05F;
// The Histogram's tallest column reaches this share of its row.
inline constexpr double kHistogramHeadroom = 0.9;
// The Histogram's filled area is its outline color at this share.
inline constexpr float kHistogramFillShare = 0.22F;

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

// A linear color, each component 0..1.
struct TraceColor final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
};
// The Waveform and Parade trace colors of red, green, blue and luma: a channel at full and the
// other two at a third, so the three channels at equal intensity add to white.
inline constexpr std::array<TraceColor, 4> kWaveformTraceColors{{{1.0F, 1.0F / 3, 1.0F / 3},
                                                                 {1.0F / 3, 1.0F, 1.0F / 3},
                                                                 {1.0F / 3, 1.0F / 3, 1.0F},
                                                                 {1.0F, 1.0F, 1.0F}}};
// The Histogram outlines of red, green, blue and luma.
inline constexpr std::array<TraceColor, 4> kHistogramTraceColors{
    {{0.875F, 0.204F, 0.204F}, {0.0F, 0.86F, 0.0F}, {0.0F, 0.46F, 0.73F}, {0.8F, 0.8F, 0.8F}}};

using Cancelled = std::function<bool()>;

// The channels each choice draws.
[[nodiscard]] std::vector<std::uint32_t> waveformChannelList(WaveformChannels channels);
[[nodiscard]] std::vector<std::uint32_t> histogramChannelList(HistogramChannels channels);

// `size` is the image's size in pixels. An empty image comes back for a cancelled call, a table
// that was not measured or an empty size. A Waveform level's or Histogram bin's count is drawn
// spread over the levels c - 3 to c + 3 by a tent of (4 - |d|) / 16 (one 8-bit step each side,
// mirrored at the axis ends), so 8-bit pictures draw no comb. Values outside the axis are not
// spread: they are drawn on its end lines (code 0 and 1023) and in its end bins.
[[nodiscard]] ScopeImage mapWaveform(const ScopeTables& tables, WaveformChannels channels,
                                     PixelSize size, const Cancelled& cancelled = {});
// Three panes (red, green, blue) of equal width with `gapPixels` between them.
[[nodiscard]] ScopeImage mapParade(const ScopeTables& tables, PixelSize size, int gapPixels,
                                   const Cancelled& cancelled = {});
// The image covers Cb and Cr from -kVectorscopeExtent to kVectorscopeExtent; a cell is drawn in
// the mean color of its pixels (from its Cb, Cr and mean luma), whitening as it gets denser.
[[nodiscard]] ScopeImage mapVectorscope(const ScopeTables& tables, const ScopeLuma& luma,
                                        PixelSize size, const Cancelled& cancelled = {});
// The image covers x 0 to kChromaticityXMaximum and y 0 to kChromaticityYMaximum of the axes (see
// chromaticityImageSize). `size` is that image's size. A cell with any pixel is drawn at full
// brightness in its own chromaticity's color: presence only.
[[nodiscard]] ScopeImage mapChromaticity(const ScopeTables& tables, PixelSize size,
                                         const Cancelled& cancelled = {});
// The image holds one row of `rowHeight` pixels per channel drawn, stacked top to bottom (red,
// green, blue, then luma), all on one scale: a screen column shows the mean count of its bins, and
// the tallest column of the channels drawn reaches kHistogramHeadroom of the row.
[[nodiscard]] ScopeImage mapHistogram(const ScopeTables& tables, HistogramChannels channels,
                                      HistogramScale scale, int rowHeight, int width,
                                      const Cancelled& cancelled = {});

// The size of the Chromaticity image for axes `side` pixels long.
[[nodiscard]] PixelSize chromaticityImageSize(int side);

// One request to map. Pointers are borrowed for the call.
struct TraceRequest final {
    Kind kind = Kind::Waveform;
    const ScopeTables* tables = nullptr;
    // The Vectorscope's luma coefficients; null maps no Vectorscope.
    const ScopeLuma* luma = nullptr;
    // Waveform, Parade, Vectorscope and Chromaticity: the image size in pixels (Chromaticity: the
    // axes' side, from which the image size is derived). Histogram: width and the row height.
    PixelSize size;
    int gapPixels = 4;
    WaveformChannels waveformChannels = WaveformChannels::Rgb;
    HistogramChannels histogramChannels = HistogramChannels::Rgb;
    HistogramScale histogramScale = HistogramScale::Linear;
};

// Maps one request. Pure; runs on the calling thread.
[[nodiscard]] ScopeImage renderTrace(const TraceRequest& request, const Cancelled& cancelled = {});

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
    // The skin tone line and the Planckian locus.
    Faint,
    Red,
    Green,
    Blue,
    Luma,
};

// The color roles of a scope as straight 0xAARRGGBB. The panel's roles are opaque; a role whose
// alpha is 0 is not drawn (a test uses that to isolate the trace).
struct ScopePalette final {
    std::uint32_t canvas = 0;
    std::uint32_t quiet = 0;
    std::uint32_t stronger = 0;
    std::uint32_t strongest = 0;
    std::uint32_t label = 0;
    std::uint32_t faint = 0;
    // Red, green, blue and luma.
    std::array<std::uint32_t, 4> channel{};

    [[nodiscard]] std::uint32_t color(ColorRole role) const noexcept;

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
    ScopeMetrics metrics;
    MeasureText measure;
};

// Everything a scope draws besides its trace. Painting order: the UnderTrace lines, polylines and
// circles, the trace, then the OverTrace lines, polylines and circles, then the labels.
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

// The Chromaticity's names for a colorimetry: the gamut ("Rec.709", "P3", "Rec.2020", "ACES AP1",
// "ACES AP0") when the primaries match one within 0.002 in x and y, else the color space's name;
// the white ("D65", "D60", "D50", "DCI") when it matches one, else empty.
struct ChromaticityGamutName final {
    std::string gamut;
    std::string white;
};
[[nodiscard]] ChromaticityGamutName
chromaticityGamutName(const color::ScopeColorimetry& colorimetry);

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
    WaveformChannels waveformChannels = WaveformChannels::Rgb;
    HistogramChannels histogramChannels = HistogramChannels::Rgb;
    HistogramScale histogramScale = HistogramScale::Linear;
};

struct PictureOptions final {
    // Device pixels per logical pixel; the logical size of a picture is its pixel size over this.
    double devicePixelRatio = 1.0;
    ScopePalette palette = defaultScopePalette();
    ScopeMetrics metrics;
    // The text measure the plan's geometry reads. Empty selects embeddedTextMeasure(
    // devicePixelRatio), the face the picture rasterizes. A caller that needs the geometry of
    // another renderer (a test comparing the panel's trace rectangle with the picture's) passes
    // that renderer's own measure.
    MeasureText measure;
};

// One scope drawn into `pixelWidth` x `pixelHeight` pixels: canvas, graticule, trace (additive for
// the Waveform and the Parade, as the panel composes it) and labels. Lines are one
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
