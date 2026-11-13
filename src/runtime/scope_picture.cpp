#include <bloom/runtime/scope_picture.hpp>

#include <bloom/render/embedded_fonts.hpp>
#include <bloom/render/text_raster.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace bloom::runtime::scopes {
namespace {

namespace render = bloom::render;

constexpr int kMaximumSide = 16384;
constexpr std::size_t kGlyphCoverageLimit = std::size_t{1} << 22U;

// The panel sets numbers in the monospaced Value role (11.5 design pixels) and names in the UiSmall
// role (10.5, Medium weight). The embedded faces have no monospaced one; both roles use the Medium
// Inter face, the weight of both roles. Inter's ascent plus descent is 1.21 em.
constexpr render::EmbeddedFace kFace = render::EmbeddedFace::InterMedium;
constexpr double kNumericPixelSize = 11.5;
constexpr double kNamePixelSize = 10.5;
constexpr double kLineHeightEm = 1.21;
// The thin space of the Parade's letter labels has no glyph in every face; it only offsets the
// letter from the edge, so the raster path drops it.
constexpr std::string_view kThinSpace = "\xE2\x80\x89";

[[nodiscard]] double pixelSizeOf(const LabelFont font) {
    return font == LabelFont::Numeric ? kNumericPixelSize : kNamePixelSize;
}

[[nodiscard]] std::string withoutThinSpace(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    while (!text.empty()) {
        if (text.starts_with(kThinSpace)) {
            text.remove_prefix(kThinSpace.size());
            continue;
        }
        result.push_back(text.front());
        text.remove_prefix(1);
    }
    return result;
}

[[nodiscard]] unsigned div255(const unsigned value) {
    const unsigned rounded = value + 128U;
    return (rounded + (rounded >> 8U)) >> 8U;
}

// Source-over of a premultiplied pixel.
[[nodiscard]] std::uint32_t over(const std::uint32_t destination, const std::uint32_t source) {
    const unsigned inverse = 255U - alphaOf(source);
    return packArgb(std::min(255U, alphaOf(source) + div255(alphaOf(destination) * inverse)),
                    std::min(255U, redOf(source) + div255(redOf(destination) * inverse)),
                    std::min(255U, greenOf(source) + div255(greenOf(destination) * inverse)),
                    std::min(255U, blueOf(source) + div255(blueOf(destination) * inverse)));
}

// A straight color 0xAARRGGBB at a coverage (0..1) as a premultiplied pixel; a role whose color has
// no alpha draws nothing.
[[nodiscard]] std::uint32_t covered(const std::uint32_t color, const float coverage) {
    const float opacity = static_cast<float>(alphaOf(color)) / 255.0F * coverage;
    const auto scale = [opacity](const unsigned component) {
        return static_cast<unsigned>(std::lround(static_cast<float>(component) * opacity));
    };
    return packArgb(static_cast<unsigned>(std::lround(255.0F * opacity)), scale(redOf(color)),
                    scale(greenOf(color)), scale(blueOf(color)));
}

[[nodiscard]] std::uint32_t plus(const std::uint32_t destination, const std::uint32_t source) {
    return packArgb(std::min(255U, alphaOf(destination) + alphaOf(source)),
                    std::min(255U, redOf(destination) + redOf(source)),
                    std::min(255U, greenOf(destination) + greenOf(source)),
                    std::min(255U, blueOf(destination) + blueOf(source)));
}

class Canvas final {
  public:
    Canvas(const int width, const int height, const std::uint32_t fill)
        : image_{width, height,
                 std::vector<std::uint32_t>(
                     static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill)} {}

    [[nodiscard]] ScopeImage take() { return std::move(image_); }
    [[nodiscard]] int width() const noexcept { return image_.width; }
    [[nodiscard]] int height() const noexcept { return image_.height; }

    void blend(const int x, const int y, const std::uint32_t source) {
        if (x < 0 || y < 0 || x >= image_.width || y >= image_.height)
            return;
        auto& pixel =
            image_.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(image_.width) +
                          static_cast<std::size_t>(x)];
        pixel = over(pixel, source);
    }
    void add(const int x, const int y, const std::uint32_t source) {
        if (x < 0 || y < 0 || x >= image_.width || y >= image_.height)
            return;
        auto& pixel =
            image_.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(image_.width) +
                          static_cast<std::size_t>(x)];
        pixel = plus(pixel, source);
    }
    // `color` is straight 0xAARRGGBB.
    void fill(const int left, const int top, const int right, const int bottom,
              const std::uint32_t color) {
        const auto source = covered(color, 1.0F);
        for (int y = std::max(top, 0); y < std::min(bottom, image_.height); ++y)
            for (int x = std::max(left, 0); x < std::min(right, image_.width); ++x)
                blend(x, y, source);
    }

  private:
    ScopeImage image_;
};

struct Segment final {
    double ax = 0.0;
    double ay = 0.0;
    double bx = 0.0;
    double by = 0.0;
};

// Antialiased coverage of segments `width` pixels wide, combined by the largest coverage so a joint
// never darkens twice, blended once. `extend` lengthens the ends by half the width (joined runs);
// `dashed` cuts a segment into dashes of four widths with gaps of two.
void stroke(Canvas& canvas, const std::vector<Segment>& segments, const double width,
            const bool extend, const bool dashed, const std::uint32_t color) {
    if (segments.empty())
        return;
    const double reach = width / 2 + 1.5;
    double minX = std::numeric_limits<double>::max();
    double minY = minX;
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = maxX;
    for (const auto& segment : segments) {
        minX = std::min({minX, segment.ax, segment.bx});
        maxX = std::max({maxX, segment.ax, segment.bx});
        minY = std::min({minY, segment.ay, segment.by});
        maxY = std::max({maxY, segment.ay, segment.by});
    }
    const int left = std::max(0, static_cast<int>(std::floor(minX - reach)));
    const int top = std::max(0, static_cast<int>(std::floor(minY - reach)));
    const int right = std::min(canvas.width(), static_cast<int>(std::ceil(maxX + reach)));
    const int bottom = std::min(canvas.height(), static_cast<int>(std::ceil(maxY + reach)));
    if (right <= left || bottom <= top)
        return;
    const auto boxWidth = static_cast<std::size_t>(right - left);
    std::vector<float> coverage(boxWidth * static_cast<std::size_t>(bottom - top), 0.0F);
    const double pad = extend ? width / 2 : 0.0;
    for (const auto& segment : segments) {
        const double dx = segment.bx - segment.ax;
        const double dy = segment.by - segment.ay;
        const double length = std::hypot(dx, dy);
        const double ux = length > 0.0 ? dx / length : 1.0;
        const double uy = length > 0.0 ? dy / length : 0.0;
        const int x0 =
            std::max(left, static_cast<int>(std::floor(std::min(segment.ax, segment.bx) - reach)));
        const int x1 =
            std::min(right, static_cast<int>(std::ceil(std::max(segment.ax, segment.bx) + reach)));
        const int y0 =
            std::max(top, static_cast<int>(std::floor(std::min(segment.ay, segment.by) - reach)));
        const int y1 =
            std::min(bottom, static_cast<int>(std::ceil(std::max(segment.ay, segment.by) + reach)));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const double px = x + 0.5 - segment.ax;
                const double py = y + 0.5 - segment.ay;
                const double along = px * ux + py * uy;
                const double across = -px * uy + py * ux;
                const double sideCover = std::clamp(width / 2 + 0.5 - std::abs(across), 0.0, 1.0);
                const double endCover = std::clamp(0.5 + along + pad, 0.0, 1.0) *
                                        std::clamp(0.5 + (length - along) + pad, 0.0, 1.0);
                double value = sideCover * endCover;
                if (dashed && std::fmod(std::max(0.0, along), width * 6.0) >= width * 4.0)
                    value = 0.0;
                auto& cell = coverage[static_cast<std::size_t>(y - top) * boxWidth +
                                      static_cast<std::size_t>(x - left)];
                cell = std::max(cell, static_cast<float>(value));
            }
    }
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x) {
            const float value = coverage[static_cast<std::size_t>(y - top) * boxWidth +
                                         static_cast<std::size_t>(x - left)];
            if (value > 0.0F)
                canvas.blend(x, y, covered(color, value));
        }
}

[[nodiscard]] int roundPixel(const double value) {
    return static_cast<int>(std::floor(value + 0.5));
}

// The span of device pixels a coordinate range covers: at least one.
void span(const double from, const double to, int& first, int& last) {
    first = roundPixel(std::min(from, to));
    last = std::max(first + 1, roundPixel(std::max(from, to)));
}

void drawLine(Canvas& canvas, const Line& line, const std::uint32_t color, const double dpr,
              const int widthPx) {
    const double ax = line.from.x * dpr;
    const double ay = line.from.y * dpr;
    const double bx = line.to.x * dpr;
    const double by = line.to.y * dpr;
    const bool horizontal = std::abs(ay - by) < 1e-9;
    const bool vertical = std::abs(ax - bx) < 1e-9;
    if (!line.antialiased && !line.dashed && (horizontal || vertical)) {
        if (horizontal) {
            const int row = static_cast<int>(std::floor(ay - widthPx / 2.0 + 0.5));
            int first = 0;
            int last = 0;
            span(ax, bx, first, last);
            canvas.fill(first, row, last, row + widthPx, color);
        } else {
            const int column = static_cast<int>(std::floor(ax - widthPx / 2.0 + 0.5));
            int first = 0;
            int last = 0;
            span(ay, by, first, last);
            canvas.fill(column, first, column + widthPx, last, color);
        }
        return;
    }
    stroke(canvas, {{ax, ay, bx, by}}, widthPx, false, line.dashed, color);
}

void drawPolyline(Canvas& canvas, const Polyline& polyline, const std::uint32_t color,
                  const double dpr, const int widthPx) {
    std::vector<Segment> segments;
    const auto& points = polyline.points;
    for (std::size_t index = 1; index < points.size(); ++index)
        segments.push_back({points[index - 1].x * dpr, points[index - 1].y * dpr,
                            points[index].x * dpr, points[index].y * dpr});
    if (polyline.closed && points.size() > 2)
        segments.push_back({points.back().x * dpr, points.back().y * dpr, points.front().x * dpr,
                            points.front().y * dpr});
    stroke(canvas, segments, widthPx, true, false, color);
}

void drawCircle(Canvas& canvas, const Circle& circle, const std::uint32_t color, const double dpr,
                const int widthPx) {
    const double radius = circle.radius * dpr;
    const int steps =
        std::clamp(static_cast<int>(std::ceil(radius * 2 * 3.14159265358979)), 24, 720);
    std::vector<Segment> segments;
    for (int step = 0; step < steps; ++step) {
        const double a0 = 2 * 3.14159265358979 * step / steps;
        const double a1 = 2 * 3.14159265358979 * (step + 1) / steps;
        segments.push_back({circle.center.x * dpr + std::cos(a0) * radius,
                            circle.center.y * dpr + std::sin(a0) * radius,
                            circle.center.x * dpr + std::cos(a1) * radius,
                            circle.center.y * dpr + std::sin(a1) * radius});
    }
    stroke(canvas, segments, widthPx, true, false, color);
}

[[nodiscard]] bool drawLabel(Canvas& canvas, const Label& label, const std::uint32_t color,
                             const double dpr, const MeasureText& measure) {
    const auto text = withoutThinSpace(label.text);
    if (text.empty())
        return true;
    const auto size = measure(label.text, label.font);
    const double left =
        label.anchor.x * dpr - size.width * dpr *
                                   (label.horizontal == HorizontalAlign::Left     ? 0.0
                                    : label.horizontal == HorizontalAlign::Center ? 0.5
                                                                                  : 1.0);
    const double top = label.anchor.y * dpr -
                       size.height * dpr * (label.vertical == VerticalAlign::Top ? 0.0 : 0.5);
    const auto parameters = render::TextRasterParameters::create(pixelSizeOf(label.font) * dpr,
                                                                 pixelSizeOf(label.font) * dpr);
    if (!parameters)
        return false;
    const auto bitmap = render::TextCoverageBitmap::rasterizeEmbeddedText(
        kFace, text, *parameters.value(), kGlyphCoverageLimit);
    if (!bitmap)
        return false;
    const auto& coverage = *bitmap.value();
    if (!coverage.hasCoverage())
        return true;
    const auto originX = static_cast<std::int64_t>(roundPixel(left)) + coverage.originX();
    const auto originY = static_cast<std::int64_t>(roundPixel(top)) + coverage.originY();
    for (std::uint32_t row = 0; row < coverage.height(); ++row) {
        const auto line = coverage.row(row);
        for (std::uint32_t column = 0; column < coverage.width() && column < line.size();
             ++column) {
            if (line[column] == 0)
                continue;
            const auto x = originX + static_cast<std::int64_t>(column);
            const auto y = originY + static_cast<std::int64_t>(row);
            if (x < 0 || y < 0 || x > std::numeric_limits<int>::max() ||
                y > std::numeric_limits<int>::max())
                continue;
            canvas.blend(static_cast<int>(x), static_cast<int>(y),
                         covered(color, static_cast<float>(line[column]) / 255.0F));
        }
    }
    return true;
}

[[nodiscard]] bool measured(const ScopeTables* tables, const Kind kind) {
    if (tables == nullptr || tables->empty())
        return false;
    const auto& layout = tables->layout();
    switch (kind) {
    case Kind::Waveform:
    case Kind::Parade:
        return layout.has(render::kScopeTableWaveform);
    case Kind::Vectorscope:
        return layout.has(render::kScopeTableVectorscope);
    case Kind::Histogram:
        return layout.has(render::kScopeTableHistogram);
    case Kind::Chromaticity:
        return layout.has(render::kScopeTableChromaticity);
    }
    return false;
}

[[nodiscard]] MeasureText measureOf(const PictureOptions& options) {
    return options.measure ? options.measure : embeddedTextMeasure(options.devicePixelRatio);
}

[[nodiscard]] bool validSide(const int value) { return value > 0 && value <= kMaximumSide; }

[[nodiscard]] bool bytesFit(const std::uint64_t width, const std::uint64_t height,
                            const std::size_t limit) {
    return width * height * 4U <= static_cast<std::uint64_t>(limit);
}

[[nodiscard]] GraticuleRequest graticuleRequest(const Kind kind, const double width,
                                                const double height, const PictureInput& input,
                                                const PictureOptions& options,
                                                const MeasureText& measure,
                                                const ClipSummary* clip) {
    GraticuleRequest request;
    request.kind = kind;
    request.width = width;
    request.height = height;
    request.devicePixelRatio = options.devicePixelRatio;
    request.signal = input.signal;
    request.axisMaximum = input.axisMaximum;
    request.waveformChannels = input.waveformChannels;
    request.histogramChannels = input.histogramChannels;
    request.histogramScale = input.histogramScale;
    request.luma = input.luma;
    request.colorimetry = input.colorimetry;
    request.clip = clip;
    request.metrics = options.metrics;
    request.measure = measure;
    return request;
}

// One scope into a canvas of its own. Returns the error, None when drawn.
[[nodiscard]] PictureError drawScope(const Kind kind, const int pixelWidth, const int pixelHeight,
                                     const PictureInput& input, const PictureOptions& options,
                                     const MeasureText& measure,
                                     const CancellationToken& cancellation, Canvas& canvas) {
    const double dpr = options.devicePixelRatio;
    const auto cancelled = [&cancellation] { return cancellation.isCancellationRequested(); };
    const auto& palette = options.palette;
    const int widthPx = std::max(1, static_cast<int>(std::lround(dpr)));

    // The trace first: its clip summary decides the markers of the plan.
    TraceResult trace;
    const bool haveTrace = measured(input.tables, kind);
    auto request = graticuleRequest(kind, pixelWidth / dpr, pixelHeight / dpr, input, options,
                                    measure, nullptr);
    auto plan = buildScopeGraticule(request);
    if (haveTrace) {
        TraceRequest mapping;
        mapping.kind = kind;
        mapping.tables = input.tables;
        mapping.luma = input.luma;
        mapping.colors = palette.traceColors();
        mapping.gain = input.gain;
        mapping.size = plan.traceRequestSize;
        mapping.gapPixels = static_cast<int>(std::lround(kParadeGap * dpr));
        mapping.waveformChannels = input.waveformChannels;
        mapping.histogramChannels = input.histogramChannels;
        mapping.histogramScale = input.histogramScale;
        trace = renderTrace(mapping, cancelled);
        if (cancelled())
            return PictureError::Cancelled;
        request.clip = &trace.clip;
        plan = buildScopeGraticule(request);
    }

    canvas.fill(0, 0, canvas.width(), canvas.height(), palette.canvas);
    for (const auto layer : {Layer::UnderTrace, Layer::OverTrace}) {
        for (const auto& line : plan.lines)
            if (line.layer == layer)
                drawLine(canvas, line, palette.color(line.color), dpr, widthPx);
        for (const auto& polyline : plan.polylines)
            if (polyline.layer == layer)
                drawPolyline(canvas, polyline, palette.color(polyline.color), dpr, widthPx);
        for (const auto& circle : plan.circles)
            if (circle.layer == layer)
                drawCircle(canvas, circle, palette.color(circle.color), dpr, widthPx);
        if (layer == Layer::UnderTrace) {
            if (!trace.image.empty()) {
                const int left = roundPixel(plan.trace.left() * dpr);
                const int top = roundPixel(plan.trace.top() * dpr);
                const bool additive = kind == Kind::Waveform || kind == Kind::Parade;
                for (int y = 0; y < trace.image.height; ++y)
                    for (int x = 0; x < trace.image.width; ++x) {
                        const auto pixel = trace.image.pixel(x, y);
                        if (pixel == 0U)
                            continue;
                        if (additive)
                            canvas.add(left + x, top + y, pixel);
                        else
                            canvas.blend(left + x, top + y, pixel);
                    }
            }
            if (cancelled())
                return PictureError::Cancelled;
        }
    }
    for (const auto& fill : plan.fills) {
        const int left = roundPixel(fill.rect.left() * dpr);
        const int top = roundPixel(fill.rect.top() * dpr);
        const int right = std::max(left + 1, roundPixel(fill.rect.right() * dpr));
        const int bottom = std::max(top + 1, roundPixel(fill.rect.bottom() * dpr));
        canvas.fill(left, top, right, bottom, palette.color(fill.color));
    }
    for (const auto& label : plan.labels) {
        if (!drawLabel(canvas, label, palette.color(label.color), dpr, measure))
            return PictureError::TextFailed;
    }
    if (cancelled())
        return PictureError::Cancelled;
    return PictureError::None;
}

} // namespace

std::uint32_t ScopePalette::color(const ColorRole role) const noexcept {
    switch (role) {
    case ColorRole::Canvas:
        return canvas;
    case ColorRole::Quiet:
        return quiet;
    case ColorRole::Stronger:
        return stronger;
    case ColorRole::Strongest:
        return strongest;
    case ColorRole::Label:
        return label;
    case ColorRole::Faint:
        return faint;
    case ColorRole::Warning:
        return warning;
    case ColorRole::Red:
        return channel[0];
    case ColorRole::Green:
        return channel[1];
    case ColorRole::Blue:
        return channel[2];
    case ColorRole::Luma:
        return channel[3];
    }
    return canvas;
}

ScopePalette defaultScopePalette() noexcept {
    ScopePalette palette;
    palette.canvas = 0xFF131313U;    // kit::Color::Canvas
    palette.quiet = 0xFF2E2E2EU;     // kit::Color::Border
    palette.stronger = 0xFF4A4A4AU;  // kit::Color::BorderHover
    palette.strongest = 0xFFFFFFFFU; // kit::Color::Foreground
    palette.label = 0xFF999999U;     // kit::Color::Muted
    palette.faint = 0xFF666666U;     // kit::Color::Faint
    palette.warning = 0xFFE6A23CU;   // kit::Color::Warn
    palette.channel = {0xFFE06C75U,  // kit::Color::ComponentX
                       0xFF8CC265U,  // kit::Color::ComponentY
                       0xFF7FC4F5U,  // kit::Color::ComponentZ
                       0xFFFFFFFFU}; // kit::Color::Foreground
    return palette;
}

MeasureText embeddedTextMeasure(const double devicePixelRatio) {
    const double dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    auto cache = std::make_shared<std::map<std::pair<std::string, int>, double>>();
    return [dpr, cache](const std::string_view text, const LabelFont font) {
        const double size = pixelSizeOf(font) * dpr;
        TextSize result;
        result.height = size * kLineHeightEm / dpr;
        const auto stripped = withoutThinSpace(text);
        if (stripped.empty())
            return result;
        const auto key = std::make_pair(stripped, static_cast<int>(font));
        if (const auto found = cache->find(key); found != cache->end()) {
            result.width = found->second;
            return result;
        }
        const auto parameters = render::TextRasterParameters::create(size, size);
        if (parameters) {
            const auto layout = render::layoutText(kFace, stripped, *parameters.value());
            if (layout && !layout.value()->lines.empty())
                result.width = layout.value()->lines.front().box.width / dpr;
        }
        cache->emplace(key, result.width);
        return result;
    };
}

PictureResult renderScopePicture(const Kind kind, const int pixelWidth, const int pixelHeight,
                                 const PictureInput& input, const PictureOptions& options,
                                 const CancellationToken& cancellation,
                                 const std::size_t byteLimit) {
    PictureResult result;
    if (!validSide(pixelWidth) || !validSide(pixelHeight) || !(options.devicePixelRatio > 0.0) ||
        !std::isfinite(options.devicePixelRatio)) {
        result.error = PictureError::InvalidSize;
        return result;
    }
    if (!bytesFit(static_cast<std::uint64_t>(pixelWidth), static_cast<std::uint64_t>(pixelHeight),
                  byteLimit)) {
        result.error = PictureError::ByteLimitExceeded;
        return result;
    }
    if (cancellation.isCancellationRequested()) {
        result.error = PictureError::Cancelled;
        return result;
    }
    Canvas canvas(pixelWidth, pixelHeight, options.palette.canvas);
    const auto measure = measureOf(options);
    result.error =
        drawScope(kind, pixelWidth, pixelHeight, input, options, measure, cancellation, canvas);
    if (result.ok())
        result.image = canvas.take();
    return result;
}

PixelSize allScopesPictureSize(const int pixelWidth, const PictureInput& input,
                               const PictureOptions& options) {
    if (!validSide(pixelWidth) || !(options.devicePixelRatio > 0.0) ||
        !std::isfinite(options.devicePixelRatio))
        return {};
    const auto measure = measureOf(options);
    const double dpr = options.devicePixelRatio;
    const int logicalWidth = static_cast<int>(std::lround(pixelWidth / dpr));
    std::uint64_t total = 0;
    for (int index = 0; index < kKindCount; ++index) {
        GraticuleRequest request;
        request.kind = static_cast<Kind>(index);
        request.metrics = options.metrics;
        request.measure = measure;
        request.histogramChannels = input.histogramChannels;
        total += static_cast<std::uint64_t>(std::max(
            1, static_cast<int>(std::lround(scopeHeightForWidth(request, logicalWidth) * dpr))));
    }
    if (total > static_cast<std::uint64_t>(kMaximumSide))
        return {};
    return {pixelWidth, static_cast<int>(total)};
}

AllScopesPicture renderAllScopesPicture(const int pixelWidth, const PictureInput& input,
                                        const PictureOptions& options,
                                        const CancellationToken& cancellation,
                                        const std::size_t byteLimit) {
    AllScopesPicture result;
    if (!validSide(pixelWidth) || !(options.devicePixelRatio > 0.0) ||
        !std::isfinite(options.devicePixelRatio)) {
        result.error = PictureError::InvalidSize;
        return result;
    }
    const auto measure = measureOf(options);
    const double dpr = options.devicePixelRatio;
    const int logicalWidth = static_cast<int>(std::lround(pixelWidth / dpr));
    std::uint64_t total = 0;
    for (int index = 0; index < kKindCount; ++index) {
        GraticuleRequest request;
        request.kind = static_cast<Kind>(index);
        request.metrics = options.metrics;
        request.measure = measure;
        request.histogramChannels = input.histogramChannels;
        const int height = std::max(
            1, static_cast<int>(std::lround(scopeHeightForWidth(request, logicalWidth) * dpr)));
        result.top[static_cast<std::size_t>(index)] = static_cast<int>(total);
        result.height[static_cast<std::size_t>(index)] = height;
        total += static_cast<std::uint64_t>(height);
        if (total > static_cast<std::uint64_t>(kMaximumSide)) {
            result.error = PictureError::InvalidSize;
            return result;
        }
    }
    if (!bytesFit(static_cast<std::uint64_t>(pixelWidth), total, byteLimit)) {
        result.error = PictureError::ByteLimitExceeded;
        return result;
    }
    if (cancellation.isCancellationRequested()) {
        result.error = PictureError::Cancelled;
        return result;
    }
    ScopeImage whole;
    whole.width = pixelWidth;
    whole.height = static_cast<int>(total);
    whole.pixels.reserve(static_cast<std::size_t>(pixelWidth) * static_cast<std::size_t>(total));
    for (int index = 0; index < kKindCount; ++index) {
        const auto slot = static_cast<std::size_t>(index);
        Canvas canvas(pixelWidth, result.height[slot], options.palette.canvas);
        const auto error = drawScope(static_cast<Kind>(index), pixelWidth, result.height[slot],
                                     input, options, measure, cancellation, canvas);
        if (error != PictureError::None) {
            result.error = error;
            return result;
        }
        auto part = canvas.take();
        whole.pixels.insert(whole.pixels.end(), part.pixels.begin(), part.pixels.end());
    }
    result.image = std::move(whole);
    return result;
}

} // namespace bloom::runtime::scopes
