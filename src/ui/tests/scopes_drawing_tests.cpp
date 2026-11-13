// #724 Scopes: the count-to-image mapping, as pure functions.
//
// Asserted, over known tables of a few pixels:
//   * trace brightness is log2(1 + n) / log2(1 + m) times the gain, clamped to 1, never below the
//     visible floor for a cell with a pixel, and 0 for a cell with none;
//   * a Waveform maps each count to the pixel of its column and level, additively in its channel
//     colors, and a Parade to three panes with gaps; a Histogram to rows of one shared scale whose
//     single pixel is never hidden; a Vectorscope and a CIE chromaticity table to the cell of their
//     count;
//   * clip summaries carry under and over counts.
// No widget, device or event loop is used.

#include <bloom/render/image_types.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/ui/scopes_drawing.hpp>

#include <QColor>
#include <QCoreApplication>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace scopes = bloom::ui::scopes;
namespace ui = bloom::ui;
using render::Rgba32f;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] Rgba32f px(const float r, const float g, const float b, const float a = 1.0F) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, a);
    if (!result)
        throw std::logic_error("invalid fixture pixel");
    return *result.value();
}

[[nodiscard]] runtime::ScopeTables tablesOf(const std::vector<Rgba32f>& signal,
                                            const std::uint32_t width, const std::uint32_t height,
                                            const std::uint32_t scopeBits,
                                            const std::vector<Rgba32f>& xyz = {}) {
    runtime::ScopeMeasureInput input;
    input.signal = signal;
    input.xyz = xyz;
    input.width = width;
    input.height = height;
    input.areaWidth = width;
    input.areaHeight = height;
    input.scopes = scopeBits;
    input.axisMaximum = 1;
    input.luma = runtime::scopeLuma(0.2126F, 0.7152F, 0.0722F);
    const auto counts = runtime::measureScopesCpu(input);
    if (!counts)
        throw std::logic_error("the fixture could not be measured");
    return runtime::ScopeTables::pack(*counts, runtime::ScopeRoute::Cpu);
}

// Pure red, green, blue and gray trace colors, so additive sums are easy to read.
[[nodiscard]] scopes::TraceColors primaries() {
    return {{qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(128, 128, 128)}};
}

[[nodiscard]] QRgb at(const QImage& image, const int x, const int y) { return image.pixel(x, y); }

void testBrightness(Expectations& e) {
    e.expect(scopes::traceBrightness(0, 100, 1.0F) == 0.0F, "a cell with no pixel is not drawn");
    e.expect(scopes::traceBrightness(100, 100, 1.0F) == 1.0F, "the largest count is full");
    e.expect(scopes::traceBrightness(1, 1, 1.0F) == 1.0F, "a single count in a table of one");
    const auto single = scopes::traceBrightness(1, 1'000'000, 1.0F);
    e.expect(single >= scopes::kTraceFloor && single < 0.2F,
             "one pixel among a million is drawn at the floor");
    const auto mid = scopes::traceBrightness(15, 255, 1.0F);
    // log2(16) / log2(256) = 4 / 8.
    e.expect(std::abs(mid - 0.5F) < 1e-6F, "brightness is log2(1 + n) / log2(1 + m)");
    e.expect(std::abs(scopes::traceBrightness(15, 255, 1.5F) - 0.75F) < 1e-6F,
             "the Brightness gain multiplies it");
    e.expect(scopes::traceBrightness(255, 255, 4.0F) == 1.0F, "it is clamped to 1");
    e.expect(scopes::traceBrightness(1, 1'000'000, 0.01F) == scopes::kTraceFloor,
             "no gain hides a cell that has a pixel");
    float last = 0.0F;
    bool monotonic = true;
    for (std::uint32_t n = 1; n <= 4096; n += 7) {
        const auto value = scopes::traceBrightness(n, 4096, 1.0F);
        monotonic = monotonic && value >= last;
        last = value;
    }
    e.expect(monotonic, "brightness never decreases with the count");
}

void testWaveformAndParade(Expectations& e) {
    // A horizontal ramp of four pixels: codes 0, 341, 682, 1023 (levels 0, 170, 341, 511).
    std::vector<Rgba32f> ramp;
    for (int x = 0; x < 4; ++x) {
        const float v = static_cast<float>(x) / 3.0F;
        ramp.push_back(px(v, v, v));
    }
    const auto tables = tablesOf(ramp, 4, 1, render::kScopeTableWaveform);
    const auto image =
        scopes::mapWaveform(tables, scopes::WaveformChannels::Rgb, primaries(), 1.0F, {4, 512});
    e.expect(image.size() == QSize(4, 512), "the waveform image has the size asked for");
    const std::array<int, 4> levels{0, 170, 341, 511};
    bool diagonal = !image.isNull();
    int lit = 0;
    for (int y = 0; y < 512 && !image.isNull(); ++y)
        for (int x = 0; x < 4; ++x) {
            const bool on = qAlpha(at(image, x, y)) != 0;
            lit += on ? 1 : 0;
            diagonal = diagonal && (on == (y == 511 - levels[static_cast<std::size_t>(x)]));
        }
    e.expect(diagonal && lit == 4, "a ramp is one lit pixel per column on a rising diagonal");
    // Gray in all three channels adds red, green and blue to white.
    e.expect(!image.isNull() && at(image, 3, 0) == qRgba(255, 255, 255, 255),
             "the three channels add to white");
    const auto luma =
        scopes::mapWaveform(tables, scopes::WaveformChannels::Luma, primaries(), 1.0F, {4, 512});
    // Premultiplied gray at half intensity: alpha is the trace's brightness times its gray.
    e.expect(!luma.isNull() && qAlpha(at(luma, 3, 0)) == 128 && qAlpha(at(luma, 0, 511)) == 128 &&
                 qAlpha(at(luma, 3, 5)) == 0,
             "the luma choice draws the luma trace in its gray");
    // The same table at half the height: two levels share a row, and no cell is lost.
    const auto small =
        scopes::mapWaveform(tables, scopes::WaveformChannels::Rgb, primaries(), 1.0F, {4, 256});
    int smallLit = 0;
    for (int y = 0; y < small.height(); ++y)
        for (int x = 0; x < small.width(); ++x)
            smallLit += qAlpha(at(small, x, y)) != 0 ? 1 : 0;
    e.expect(smallLit == 4, "a reduced image keeps every column's cell");

    // Parade: red, green and blue panes with a gap; a pure red pixel lights only the red pane.
    std::vector<Rgba32f> red(4, px(1.0F, 0.0F, 0.0F));
    const auto redTables = tablesOf(red, 4, 1, render::kScopeTableWaveform);
    const auto parade = scopes::mapParade(redTables, primaries(), 1.0F, {14, 512}, 1);
    e.expect(parade.size() == QSize(14, 512), "the parade image has the size asked for");
    bool redPane = !parade.isNull();
    bool others = true;
    if (!parade.isNull()) {
        // Panes of 4 pixels at x 0..3, 5..8 and 10..13; the gaps at 4 and 9 stay empty.
        redPane =
            at(parade, 0, 0) == qRgba(255, 0, 0, 255) && at(parade, 3, 0) == qRgba(255, 0, 0, 255);
        for (int y = 0; y < 512; ++y)
            for (const int x : {4, 9})
                others = others && qAlpha(at(parade, x, y)) == 0;
        // Green is 0 for pure red (code 0 level 0 at the bottom) and blue likewise: both trace
        // the bottom row of their pane in their own colors.
        others = others && at(parade, 5, 511) == qRgba(0, 255, 0, 255) &&
                 at(parade, 10, 511) == qRgba(0, 0, 255, 255) && qAlpha(at(parade, 5, 0)) == 0 &&
                 qAlpha(at(parade, 10, 0)) == 0;
    }
    e.expect(redPane && others, "the parade draws each channel in its own pane and color");
}

void testClipSummary(Expectations& e) {
    // Over: 2.0; under: -0.5; one pixel in range.
    std::vector<Rgba32f> pixels{px(2.0F, 2.0F, 2.0F), px(-0.5F, -0.5F, -0.5F), px(0.5F, 0.5F, 0.5F),
                                px(0.5F, 0.5F, 0.5F)};
    const auto tables =
        tablesOf(pixels, 4, 1, render::kScopeTableHistogram | render::kScopeTableWaveform);
    const auto histogram =
        scopes::clipSummary(tables, scopes::Kind::Histogram, scopes::WaveformChannels::Rgb,
                            scopes::HistogramChannels::Rgb);
    e.expect(histogram.over[0] == 1 && histogram.under[0] == 1 && histogram.over[2] == 1,
             "the histogram's clip counts are the tables' over and under");
    const auto waveform =
        scopes::clipSummary(tables, scopes::Kind::Waveform, scopes::WaveformChannels::Rgb,
                            scopes::HistogramChannels::Rgb);
    e.expect(waveform.columns.size() == 4 && waveform.columns[0] == 2 && waveform.columns[1] == 1 &&
                 waveform.columns[2] == 0 && waveform.columns[3] == 0,
             "a waveform column carries bit 1 for over and bit 0 for under");
}

void testHistogram(Expectations& e) {
    // A thousand red pixels: 500 at code 512, 499 at code 513 and one at code 1023. Green and blue
    // hold all of them at code 0, so the scale of every row is 1000.
    std::vector<Rgba32f> pixels(500, px(512.0F / 1023.0F, 0.0F, 0.0F));
    pixels.insert(pixels.end(), 499, px(513.0F / 1023.0F, 0.0F, 0.0F));
    pixels.push_back(px(1.0F, 0.0F, 0.0F));
    const auto tables = tablesOf(pixels, 1000, 1, render::kScopeTableHistogram);
    const auto image = scopes::mapHistogram(tables, scopes::HistogramChannels::Rgb,
                                            scopes::HistogramScale::Linear, primaries(), 20, 1024);
    e.expect(image.size() == QSize(1024, 60), "three rows of 20 pixels");
    const auto column = [&](const int x, const int row) {
        int height = 0;
        for (int y = 0; y < 20; ++y)
            height += qAlpha(at(image, x, row * 20 + y)) != 0 ? 1 : 0;
        return height;
    };
    e.expect(column(512, 0) == 10 && column(513, 0) == 10,
             "a bin is as tall as its share of the largest bin");
    e.expect(column(1023, 0) == 1, "a single pixel is one pixel tall, never hidden");
    e.expect(column(511, 0) == 0 && column(100, 0) == 0, "empty bins are empty");
    e.expect(column(0, 1) == 20 && column(0, 2) == 20,
             "every row is on the one scale of the largest bin");
    e.expect(at(image, 513, 10) == qRgba(255, 0, 0, 255),
             "the top of the curve is an outline in the channel's color");
    const auto fillAlpha = qAlpha(at(image, 513, 15));
    e.expect(fillAlpha > 0 && fillAlpha < 255, "the area below it is a translucent tint");
    // Logarithmic: one pixel among a thousand is log2(2) / log2(1001) of the row, taller than
    // linear.
    const auto logarithmic =
        scopes::mapHistogram(tables, scopes::HistogramChannels::Rgb,
                             scopes::HistogramScale::Logarithmic, primaries(), 20, 1024);
    int logHeight = 0;
    for (int y = 0; y < 20; ++y)
        logHeight += qAlpha(at(logarithmic, 1023, y)) != 0 ? 1 : 0;
    e.expect(logHeight > 1, "the logarithmic choice lifts a small count");
    const auto withLuma =
        scopes::mapHistogram(tables, scopes::HistogramChannels::RgbAndLuma,
                             scopes::HistogramScale::Linear, primaries(), 20, 1024);
    e.expect(withLuma.height() == 80, "luma adds a row");
    const auto onlyLuma =
        scopes::mapHistogram(tables, scopes::HistogramChannels::Luma,
                             scopes::HistogramScale::Linear, primaries(), 20, 1024);
    e.expect(onlyLuma.height() == 20, "the luma choice is one row");
}

void testVectorscopeAndChromaticity(Expectations& e) {
    // A flat gray: cb = cr = 0 sits on the corner of four cells; a neighborhood is lit.
    std::vector<Rgba32f> gray(4, px(0.5F, 0.5F, 0.5F));
    std::vector<Rgba32f> white(4, px(0.9505F, 1.0F, 1.089F));
    const auto tables = tablesOf(
        gray, 4, 1, render::kScopeTableVectorscope | render::kScopeTableChromaticity, white);
    const auto luma = runtime::scopeLuma(0.2126F, 0.7152F, 0.0722F);
    const auto vectorscope = scopes::mapVectorscope(tables, luma, 1.0F, {512, 512});
    int inside = 0;
    int outside = 0;
    for (int y = 0; y < 512 && !vectorscope.isNull(); ++y)
        for (int x = 0; x < 512; ++x) {
            if (qAlpha(at(vectorscope, x, y)) == 0)
                continue;
            (x >= 254 && x <= 257 && y >= 254 && y <= 257 ? inside : outside) += 1;
        }
    e.expect(inside >= 1 && outside == 0, "a flat gray is lit at the center of the vectorscope");
    // Its cells are the near-white of the center: no chroma, brightened toward white.
    const auto lit = [&] {
        for (int y = 254; y <= 257; ++y)
            for (int x = 254; x <= 257; ++x)
                if (qAlpha(at(vectorscope, x, y)) != 0)
                    return at(vectorscope, x, y);
        return QRgb{0};
    }();
    e.expect(std::abs(qRed(lit) - qBlue(lit)) <= 8 && qAlpha(lit) == 255,
             "the center cell is colorless and at full brightness");
    // A reduced image keeps the cell.
    const auto small = scopes::mapVectorscope(tables, luma, 1.0F, {128, 128});
    int smallLit = 0;
    for (int y = 0; y < 128 && !small.isNull(); ++y)
        for (int x = 0; x < 128; ++x)
            smallLit += qAlpha(at(small, x, y)) != 0 ? 1 : 0;
    e.expect(smallLit == 1, "a reduced vectorscope keeps the cell as one pixel");

    // CIE: D65 white (x 0.3127, y 0.3290) is cell (200, 187) of the 512 by 512 table over x 0 to
    // 0.8 and y 0 to 0.9; at 512 pixels the image is the table, with y up.
    const auto chromaticity = scopes::mapChromaticity(tables, 1.0F, {512, 512});
    int cieLit = 0;
    bool placed = false;
    for (int y = 0; y < 512 && !chromaticity.isNull(); ++y)
        for (int x = 0; x < 512; ++x)
            if (qAlpha(at(chromaticity, x, y)) != 0) {
                ++cieLit;
                placed = x >= 199 && x <= 201 && y >= 511 - 188 && y <= 511 - 186;
            }
    e.expect(cieLit == 1 && placed, "D65 white is lit at its chromaticity, y up");
    const auto size = scopes::chromaticityImageSize(500);
    e.expect(size == QSize(400, 450), "the chromaticity image covers x to 0.8 and y to 0.9");
    // A table that was not measured maps to nothing.
    e.expect(scopes::mapWaveform(tables, scopes::WaveformChannels::Rgb, primaries(), 1.0F, {4, 4})
                 .isNull(),
             "a scope that was not measured has no image");
    e.expect(scopes::mapVectorscope(tables, luma, 1.0F, {0, 0}).isNull(), "an empty size is empty");
    // A cancelled mapping gives nothing.
    e.expect(scopes::mapVectorscope(tables, luma, 1.0F, {512, 512}, [] { return true; }).isNull(),
             "a cancelled mapping gives no image");
}

void testRenderTrace(Expectations& e) {
    std::vector<Rgba32f> gray(4, px(0.5F, 0.5F, 0.5F));
    auto tables = std::make_shared<const runtime::ScopeTables>(
        tablesOf(gray, 4, 1, render::kScopeTableHistogram));
    scopes::TraceRequest request;
    request.kind = scopes::Kind::Histogram;
    request.tables = tables;
    request.colors = primaries();
    request.size = {64, 8};
    const auto result = scopes::renderTrace(request);
    e.expect(result.image.size() == QSize(64, 24), "a request maps to its image");
    request.kind = scopes::Kind::Vectorscope;
    e.expect(scopes::renderTrace(request).image.isNull(),
             "a scope the tables do not hold maps to nothing");
    scopes::TraceRequest empty;
    e.expect(scopes::renderTrace(empty).image.isNull(), "a request without tables is empty");
}

} // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication application(argc, argv);
        Expectations expectations;
        testBrightness(expectations);
        testWaveformAndParade(expectations);
        testClipSummary(expectations);
        testHistogram(expectations);
        testVectorscopeAndChromaticity(expectations);
        testRenderTrace(expectations);
        if (expectations.failures() != 0)
            return 1;
        std::cout << "PASS: scopes drawing\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
