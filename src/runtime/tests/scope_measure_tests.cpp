// #724 Scopes: the CPU reference of the scope measure against the definitions of
// docs/architecture/scopes.md, computed here independently in double from the formulas: flat field,
// ramp, color bars, the counters, Ignore Transparent Pixels, areas that reach outside the picture,
// a region, the Waveform columns, the sum rule, the axis maximum, a generated pattern, the packed
// tables, the summary and cancellation.

#include <bloom/runtime/scope_measure.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using render::Rgba32f;
using runtime::ScopeCounts;
using runtime::ScopeTables;

int failures = 0;

void check(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] Rgba32f px(const float r, const float g, const float b, const float a = 1.0F) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, a);
    if (!result) {
        throw std::logic_error("invalid fixture pixel");
    }
    return *result.value();
}

// A pixel the checked factory would refuse (not finite): the same four floats, bit for bit.
[[nodiscard]] Rgba32f rawPixel(const float r, const float g, const float b, const float a) {
    return std::bit_cast<Rgba32f>(std::array<float, 4>{r, g, b, a});
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

constexpr double kKr = 0.2126;
constexpr double kKg = 0.7152;
constexpr double kKb = 0.0722;

struct Picture final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Rgba32f> signal;
    std::vector<Rgba32f> xyz;
};

struct Area final {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] Picture flat(const std::uint32_t w, const std::uint32_t h, const Rgba32f color) {
    Picture picture;
    picture.width = w;
    picture.height = h;
    picture.signal.assign(static_cast<std::size_t>(w) * h, color);
    picture.xyz.assign(static_cast<std::size_t>(w) * h, px(0.4F, 0.3F, 0.2F));
    return picture;
}

template <typename F>
[[nodiscard]] Picture generate(const std::uint32_t w, const std::uint32_t h, F f) {
    Picture picture = flat(w, h, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            picture.signal[static_cast<std::size_t>(y) * w + x] = f(x, y);
        }
    }
    return picture;
}

[[nodiscard]] runtime::ScopeMeasureInput inputFor(const Picture& picture, const Area& area,
                                                  const std::uint32_t scopes,
                                                  const std::uint32_t axisMaximum,
                                                  const bool ignore) {
    runtime::ScopeMeasureInput input;
    input.signal = picture.signal;
    input.xyz = picture.xyz;
    input.width = picture.width;
    input.height = picture.height;
    input.areaX = area.x;
    input.areaY = area.y;
    input.areaWidth = area.width;
    input.areaHeight = area.height;
    input.scopes = scopes;
    input.axisMaximum = axisMaximum;
    input.luma = runtime::scopeLuma(0.2126F, 0.7152F, 0.0722F);
    input.ignoreTransparent = ignore;
    return input;
}

// The sum rule and its relatives: every table accounts for every counted pixel.
void verifySums(const ScopeCounts& counts, const std::string& name) {
    const auto& layout = counts.layout;
    check(counts.words.size() == layout.wordCount, name + ": word count");
    if (layout.has(render::kScopeTableHistogram)) {
        for (std::uint32_t channel = 0; channel < 4; ++channel) {
            std::uint64_t sum = 0;
            for (std::uint32_t i = 0; i < render::kScopeHistogramChannelWords; ++i) {
                sum += counts.histogram(channel, i);
            }
            check(sum == counts.counted(),
                  name + ": histogram sum rule, channel " + std::to_string(channel));
        }
    }
    if (layout.has(render::kScopeTableWaveform)) {
        for (std::uint32_t channel = 0; channel < 4; ++channel) {
            std::uint64_t sum = 0;
            for (std::uint32_t c = 0; c < layout.columns; ++c) {
                for (std::uint32_t l = 0; l < render::kScopeWaveformColumnWords; ++l) {
                    sum += counts.waveform(channel, c, l);
                }
            }
            check(sum == counts.counted(),
                  name + ": waveform sum, channel " + std::to_string(channel));
        }
    }
    if (layout.has(render::kScopeTableVectorscope)) {
        std::uint64_t sum = counts.vectorscopeOutside();
        for (std::uint32_t y = 0; y < 512; ++y) {
            for (std::uint32_t x = 0; x < 512; ++x) {
                sum += counts.vectorscope(x, y);
            }
        }
        check(sum == counts.counted(), name + ": vectorscope sum");
    }
    if (layout.has(render::kScopeTableChromaticity)) {
        std::uint64_t sum = std::uint64_t{counts.chromaticityOutside()} + counts.noChromaticity();
        for (std::uint32_t y = 0; y < 512; ++y) {
            for (std::uint32_t x = 0; x < 512; ++x) {
                sum += counts.chromaticity(x, y);
            }
        }
        check(sum == counts.counted(), name + ": chromaticity sum");
    }
}

[[nodiscard]] std::optional<ScopeCounts>
measure(const Picture& picture, const Area& area, const std::string& name,
        const std::uint32_t scopes = render::kScopeTableAll, const std::uint32_t axisMaximum = 1,
        const bool ignore = false) {
    auto counts = runtime::measureScopesCpu(inputFor(picture, area, scopes, axisMaximum, ignore));
    check(counts.has_value(), name + ": measures");
    if (counts) {
        verifySums(*counts, name);
    }
    return counts;
}

[[nodiscard]] Area whole(const Picture& picture) { return {0, 0, picture.width, picture.height}; }

// The definitions in double. A code is 0..1023, 1024 under, 1025 over.
[[nodiscard]] std::uint32_t codeOf(const double value, const double axisMaximum = 1.0) {
    const double c = std::floor(value / axisMaximum * 1023.0 + 0.5);
    if (c < 0.0) {
        return 1024U;
    }
    return c > 1023.0 ? 1025U : static_cast<std::uint32_t>(c);
}

[[nodiscard]] double lumaOf(const double r, const double g, const double b) {
    return (kKr * r + kKg * g) + kKb * b;
}

// The Vectorscope position in cell units, before the floor.
[[nodiscard]] std::pair<double, double> vectorPosition(const double r, const double g,
                                                       const double b) {
    const double y = lumaOf(r, g, b);
    const double cb = (b - y) / (2.0 * (1.0 - kKb));
    const double cr = (r - y) / (2.0 * (1.0 - kKr));
    return {(cb + 0.5) * 512.0, (cr + 0.5) * 512.0};
}

[[nodiscard]] std::uint32_t nonZeroBins(const ScopeCounts& counts, const std::uint32_t channel) {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < render::kScopeHistogramChannelWords; ++i) {
        n += counts.histogram(channel, i) != 0U ? 1U : 0U;
    }
    return n;
}

[[nodiscard]] std::uint32_t nonZeroCells(const ScopeCounts& counts, const bool chromaticity) {
    std::uint32_t n = 0;
    for (std::uint32_t y = 0; y < 512; ++y) {
        for (std::uint32_t x = 0; x < 512; ++x) {
            const std::uint32_t v =
                chromaticity ? counts.chromaticity(x, y) : counts.vectorscope(x, y);
            n += v != 0U ? 1U : 0U;
        }
    }
    return n;
}

void testLuma() {
    const auto luma = runtime::scopeLuma(0.2126F, 0.7152F, 0.0722F);
    const float cbDen = 2.0F * (1.0F - 0.0722F);
    const float crDen = 2.0F * (1.0F - 0.2126F);
    check(luma.cbDenominator == cbDen && luma.crDenominator == crDen,
          "luma denominators in binary32");
    check(luma.red == 0.2126F && luma.green == 0.7152F && luma.blue == 0.0722F,
          "luma coefficients kept");
    for (const std::uint32_t m : {1U, 2U, 4U, 8U, 16U}) {
        check(runtime::validScopeAxisMaximum(m), "axis maximum valid " + std::to_string(m));
    }
    for (const std::uint32_t m : {0U, 3U, 5U, 6U, 12U, 32U}) {
        check(!runtime::validScopeAxisMaximum(m), "axis maximum invalid " + std::to_string(m));
    }
}

void testFlatField() {
    constexpr std::uint32_t kW = 16;
    constexpr std::uint32_t kH = 8;
    const double r = 0.75;
    const double g = 0.25;
    const double b = 0.5;
    const Picture picture = flat(kW, kH, px(0.75F, 0.25F, 0.5F));
    const auto counts = measure(picture, whole(picture), "flat");
    if (!counts) {
        return;
    }
    const std::uint32_t pixels = kW * kH;
    check(counts->counted() == pixels && counts->notFinite() == 0U, "flat: counters");
    const std::array<double, 4> values{r, g, b, lumaOf(r, g, b)};
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        const std::uint32_t code = codeOf(values[channel]);
        check(nonZeroBins(*counts, channel) == 1U && counts->histogram(channel, code) == pixels,
              "flat: one histogram bin, channel " + std::to_string(channel));
        check(counts->layout.columns == kW, "flat: waveform columns");
        for (std::uint32_t column = 0; column < kW; ++column) {
            check(counts->waveform(channel, column, code >> 1U) == kH,
                  "flat: waveform level, one per column");
            std::uint32_t levels = 0;
            for (std::uint32_t l = 0; l < render::kScopeWaveformColumnWords; ++l) {
                levels += counts->waveform(channel, column, l) != 0U ? 1U : 0U;
            }
            check(levels == 1U, "flat: one level in a column");
        }
    }
    const auto [cb, cr] = vectorPosition(r, g, b);
    check(nonZeroCells(*counts, false) == 1U &&
              counts->vectorscope(static_cast<std::uint32_t>(cb), static_cast<std::uint32_t>(cr)) ==
                  pixels,
          "flat: one vectorscope cell");
    const double s = 0.4 + 0.3 + 0.2;
    check(nonZeroCells(*counts, true) == 1U &&
              counts->chromaticity(static_cast<std::uint32_t>(0.4 / s / 0.8 * 512.0),
                                   static_cast<std::uint32_t>(0.3 / s / 0.9 * 512.0)) == pixels,
          "flat: one chromaticity cell");
}

void testRamp() {
    // Value x / 63 on all channels, 64 wide: 64 distinct codes (neighbors are about 16 codes
    // apart), so "flat" means each of the 64 codes of the red, green and blue channels (and of the
    // luma channel, whose codes differ from the gray by at most one) holds exactly H pixels.
    constexpr std::uint32_t kW = 64;
    constexpr std::uint32_t kH = 4;
    const Picture picture = generate(kW, kH, [](std::uint32_t x, std::uint32_t) {
        const float v = static_cast<float>(x) / 63.0F;
        return px(v, v, v);
    });
    const auto counts = measure(picture, whole(picture), "ramp");
    if (!counts) {
        return;
    }
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        check(nonZeroBins(*counts, channel) == kW,
              "ramp: 64 distinct codes, channel " + std::to_string(channel));
        for (std::uint32_t i = 0; i < render::kScopeCodes; ++i) {
            const std::uint32_t n = counts->histogram(channel, i);
            check(n == 0U || n == kH, "ramp: every occupied bin holds H pixels");
        }
    }
    check(counts->layout.columns == kW, "ramp: columns");
    std::uint32_t previous = 0;
    for (std::uint32_t x = 0; x < kW; ++x) {
        const std::uint32_t level =
            codeOf(static_cast<double>(static_cast<float>(x) / 63.0F)) >> 1U;
        check(counts->waveform(0, x, level) == kH,
              "ramp: waveform diagonal at column " + std::to_string(x));
        check(x == 0U || level > previous, "ramp: strictly rising");
        previous = level;
    }
    check(counts->vectorscopeOutside() == 0U, "ramp: gray stays inside the vectorscope");
}

struct Bar final {
    float r;
    float g;
    float b;
};

void colorBars(const float level, const std::string& name) {
    // White, yellow, cyan, green, magenta, red, blue, black; 8 pixels wide each.
    const std::array<Bar, 8> bars{
        {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}}};
    constexpr std::uint32_t kBarW = 8;
    constexpr std::uint32_t kH = 2;
    const Picture picture = generate(kBarW * 8, kH, [&](std::uint32_t x, std::uint32_t) {
        const Bar& bar = bars[x / kBarW];
        return px(bar.r * level, bar.g * level, bar.b * level);
    });
    const auto counts = measure(picture, whole(picture), name);
    if (!counts) {
        return;
    }
    // Expected boxes: the cells a bar's computed position can fall in. A position within 1e-3 of
    // a cell edge (the 100% bars and the 75% secondaries sit exactly on one) allows both cells; a
    // candidate outside the table is the `vectorscopeOutside` counter.
    using Box = std::array<std::int32_t, 4>;
    std::map<Box, std::uint32_t> boxes;
    std::uint32_t edgePixels = 0;
    for (const Bar& bar : bars) {
        const auto [px0, py0] =
            vectorPosition(static_cast<double>(bar.r * level), static_cast<double>(bar.g * level),
                           static_cast<double>(bar.b * level));
        const std::int32_t xLow = static_cast<std::int32_t>(std::floor(px0 - 1e-3));
        const std::int32_t xHigh = static_cast<std::int32_t>(std::floor(px0 + 1e-3));
        const std::int32_t yLow = static_cast<std::int32_t>(std::floor(py0 - 1e-3));
        const std::int32_t yHigh = static_cast<std::int32_t>(std::floor(py0 + 1e-3));
        const bool edge = xLow < 0 || xHigh > 511 || yLow < 0 || yHigh > 511;
        if (edge) {
            edgePixels += kBarW * kH;
            continue;
        }
        boxes[{xLow, xHigh, yLow, yHigh}] += kBarW * kH;
    }
    std::uint32_t inBoxes = 0;
    for (const auto& [box, expected] : boxes) {
        std::uint32_t sum = 0;
        for (std::int32_t y = box[2]; y <= box[3]; ++y) {
            for (std::int32_t x = box[0]; x <= box[1]; ++x) {
                sum += counts->vectorscope(static_cast<std::uint32_t>(x),
                                           static_cast<std::uint32_t>(y));
            }
        }
        check(sum == expected, name + ": vectorscope counts at the computed cells");
        inBoxes += sum;
    }
    // Everything else is a bar on the table's edge: in an edge cell or outside the table.
    check(inBoxes + edgePixels == counts->counted(), name + ": bars accounted");
    check(nonZeroCells(*counts, false) <=
              static_cast<std::uint32_t>(boxes.size()) * 4U + edgePixels,
          name + ": no stray cells");
    if (level < 1.0F) {
        check(edgePixels == 0U && counts->vectorscopeOutside() == 0U,
              name + ": 75% bars inside the table");
    }
}

void testColorBars() {
    colorBars(0.75F, "bars75");
    colorBars(1.0F, "bars100");
}

void testCounters() {
    // Pixel by pixel: normal; red under; green over (and luma over); not finite (NaN); not finite
    // (alpha infinite); transparent black; blue over with a position outside the Vectorscope; one
    // more normal one.
    Picture picture = flat(8, 1, px(0.5F, 0.5F, 0.5F));
    picture.signal[1] = px(-0.4F, 0.5F, 0.5F);
    picture.signal[2] = px(0.5F, 2.0F, 0.5F);
    picture.signal[3] = rawPixel(0.5F, 0.5F, kNaN, 1.0F);
    picture.signal[4] = rawPixel(0.5F, 0.5F, 0.5F, kInf);
    picture.signal[5] = Rgba32f::transparent();
    picture.signal[6] = px(0.0F, 0.0F, 2.0F);
    picture.xyz[1] = px(0.0F, 0.0F, 0.0F);              // no chromaticity (sum 0)
    picture.xyz[2] = px(0.9F, 0.05F, 0.05F);            // x = 0.9, outside 0..0.8
    picture.xyz[6] = rawPixel(kNaN, 0.3F, 0.2F, 1.0F);  // no chromaticity (not finite)
    picture.xyz[7] = rawPixel(-0.2F, 0.5F, 0.1F, 1.0F); // x < 0
    const auto counts = measure(picture, whole(picture), "counters");
    if (!counts) {
        return;
    }
    check(counts->counted() == 6U, "counters: counted");
    check(counts->notFinite() == 2U, "counters: notFinite");
    check(counts->noChromaticity() == 2U, "counters: noChromaticity");
    check(counts->chromaticityOutside() == 2U, "counters: chromaticityOutside");
    check(counts->vectorscopeOutside() == 2U, "counters: vectorscopeOutside");
    check(counts->histogram(0, render::kScopeCodeUnder) == 1U, "counters: red under");
    check(counts->histogram(0, render::kScopeCodeOver) == 0U, "counters: red over");
    check(counts->histogram(1, render::kScopeCodeOver) == 1U, "counters: green over");
    check(counts->histogram(2, render::kScopeCodeOver) == 1U, "counters: blue over");
    check(counts->histogram(3, render::kScopeCodeOver) == 1U, "counters: luma over");
    check(counts->waveform(0, 1, render::kScopeWaveformLevelUnder) == 1U,
          "counters: waveform under");
    check(counts->waveform(1, 2, render::kScopeWaveformLevelOver) == 1U, "counters: waveform over");

    const auto ignoring =
        measure(picture, whole(picture), "counters ignoring", render::kScopeTableAll, 1, true);
    if (ignoring) {
        check(ignoring->counted() == 5U && ignoring->notFinite() == 2U,
              "ignore transparent: counted");
        check(counts->histogram(0, 0) == 2U && ignoring->histogram(0, 0) == 1U,
              "ignore transparent: code 0 loses the transparent pixel (and keeps the black one)");
    }

    // A table not asked for is absent, and reads 0.
    const auto only =
        measure(picture, whole(picture), "counters waveform only", render::kScopeTableWaveform);
    if (only) {
        check(only->histogram(0, 0) == 0U && only->vectorscope(1, 1) == 0U, "absent tables read 0");
        check(only->layout.wordCount == render::kScopeCounterWords + 4U * 8U * 514U,
              "waveform-only layout");
        check(only->noChromaticity() == 0U && only->vectorscopeOutside() == 0U,
              "absent tables count no outside");
    }
}

void testAreas() {
    // An area reaching outside: outside pixels are transparent black, code 0 unless ignored.
    const Picture picture = flat(4, 4, px(1.0F, 1.0F, 1.0F));
    const Area area{-2, -1, 8, 6};
    const auto counted = measure(picture, area, "outside");
    if (counted) {
        check(counted->counted() == 48U, "outside: counted");
        check(counted->histogram(0, 0) == 32U && counted->histogram(0, 1023) == 16U,
              "outside: code 0 and 1023");
        check(counted->layout.columns == 8U, "outside: columns follow the area");
    }
    const auto ignored =
        measure(picture, area, "outside ignoring", render::kScopeTableAll, 1, true);
    if (ignored) {
        check(ignored->counted() == 16U && ignored->histogram(0, 0) == 0U, "outside ignored");
    }

    // A region smaller than the picture sees only its pixels.
    const Picture halves = generate(8, 8, [](std::uint32_t x, std::uint32_t) {
        return x < 4 ? px(0.25F, 0.25F, 0.25F) : px(0.75F, 0.75F, 0.75F);
    });
    const auto region = measure(halves, {4, 2, 4, 3}, "region");
    if (region) {
        check(region->counted() == 12U, "region: counted");
        const std::uint32_t code = codeOf(0.75);
        check(region->histogram(0, code) == 12U && nonZeroBins(*region, 0) == 1U,
              "region: only right half");
        check(region->layout.columns == 4U, "region: columns");
    }

    // Arithmetic of the area's far corner must not wrap.
    const auto far = measure(picture, {2'000'000'000, 2'000'000'000, 2, 2}, "far");
    if (far) {
        check(far->counted() == 4U && far->histogram(0, 0) == 4U, "far corner: all outside");
    }
}

void testWaveformColumns() {
    // Narrower than 512: one column per pixel (done above, 64 and 100). Wider: 512 columns, and
    // column floor(x * 512 / W).
    const Picture narrow = flat(100, 1, px(0.5F, 0.5F, 0.5F));
    const auto n = measure(narrow, whole(narrow), "narrow", render::kScopeTableWaveform);
    check(n && n->layout.columns == 100U, "waveform: columns of a 100-wide area");

    constexpr std::uint32_t kW = 1024;
    constexpr std::uint32_t kH = 2;
    const Picture wide = generate(kW, kH, [](std::uint32_t x, std::uint32_t) {
        const float v = static_cast<float>(x) / 1023.0F;
        return px(v, v, v);
    });
    const auto w = measure(wide, whole(wide), "wide", render::kScopeTableWaveform);
    if (w) {
        check(w->layout.columns == 512U, "waveform: 512 columns of a 1024-wide area");
        for (std::uint32_t column = 0; column < 512; ++column) {
            std::uint32_t sum = 0;
            for (std::uint32_t l = 0; l < render::kScopeWaveformColumnWords; ++l) {
                sum += w->waveform(0, column, l);
            }
            check(sum == 2U * kH, "waveform: two pixels of a row per column, times the rows");
        }
        // The first pixel of the last column is x = 1022, the ramp's last-but-one value.
        const std::uint32_t level = codeOf(static_cast<double>(1023.0F / 1023.0F)) >> 1U;
        check(w->waveform(0, 511, level) >= kH, "waveform: last column holds the top level");
    }
    const Picture odd = flat(768, 1, px(0.5F, 0.5F, 0.5F));
    const auto o = measure(odd, whole(odd), "768 wide", render::kScopeTableWaveform);
    if (o) {
        const std::uint32_t level = codeOf(0.5) >> 1U;
        for (std::uint32_t column = 0; column < 512; ++column) {
            std::uint32_t expected = 0;
            for (std::uint32_t x = 0; x < 768; ++x) {
                expected += (static_cast<std::uint64_t>(x) * 512U / 768U == column) ? 1U : 0U;
            }
            check(o->waveform(0, column, level) == expected,
                  "waveform: column population of 768 wide");
        }
    }
}

void testAxisMaximum() {
    const Picture picture = generate(3, 1, [](std::uint32_t x, std::uint32_t) {
        return x == 0 ? px(2.0F, 2.0F, 2.0F)
                      : (x == 1 ? px(1.0F, 1.0F, 1.0F) : px(8.0F, 8.0F, 8.0F));
    });
    const auto counts = measure(picture, whole(picture), "M=4", render::kScopeTableHistogram, 4);
    if (counts) {
        check(codeOf(2.0, 4.0) == 512U, "test formula: 2.0 at M=4 is code 512");
        check(counts->histogram(0, codeOf(2.0, 4.0)) == 1U,
              "M=4: 2.0 lands at floor(2/4*1023+0.5)");
        check(counts->histogram(0, codeOf(1.0, 4.0)) == 1U, "M=4: 1.0");
        check(counts->histogram(0, render::kScopeCodeOver) == 1U, "M=4: 8.0 is over");
    }
    for (const std::uint32_t bad : {0U, 3U, 32U}) {
        check(!runtime::measureScopesCpu(inputFor(picture, whole(picture), 1, bad, false))
                   .has_value(),
              "axis maximum " + std::to_string(bad) + " refused");
    }
}

void testCrossCheck() {
    // r = (4x + 1) / 128, g = (4y + 2) / 128, b = (4 ((7x + 13y + 3) mod 32) + 3) / 128: exact,
    // never equal in binary32. Waveform and Vectorscope against the formulas in double; the pattern
    // keeps every value away from a cell edge (checked).
    constexpr std::uint32_t kS = 32;
    const Picture picture = generate(kS, kS, [](std::uint32_t x, std::uint32_t y) {
        return px(static_cast<float>(4 * x + 1) / 128.0F, static_cast<float>(4 * y + 2) / 128.0F,
                  static_cast<float>(4 * ((x * 7 + y * 13 + 3) % 32) + 3) / 128.0F);
    });
    const auto counts = measure(picture, whole(picture), "cross-check",
                                render::kScopeTableWaveform | render::kScopeTableVectorscope);
    if (!counts) {
        return;
    }
    std::vector<std::uint32_t> waveform(4U * kS * 514U, 0U);
    std::vector<std::uint32_t> vectorscope(512U * 512U, 0U);
    std::uint32_t outside = 0;
    std::uint32_t ambiguous = 0;
    const auto margin = [](const double value) { return std::abs(value - std::round(value)); };
    for (std::uint32_t y = 0; y < kS; ++y) {
        for (std::uint32_t x = 0; x < kS; ++x) {
            const double r = (4.0 * x + 1.0) / 128.0;
            const double g = (4.0 * y + 2.0) / 128.0;
            const double b = (4.0 * ((x * 7 + y * 13 + 3) % 32) + 3.0) / 128.0;
            const std::array<double, 4> values{r, g, b, lumaOf(r, g, b)};
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                const double scaled = values[channel] * 1023.0 + 0.5;
                ambiguous += margin(scaled) < 1e-4 ? 1U : 0U;
                const std::uint32_t code = codeOf(values[channel]);
                const std::uint32_t level = code < 1024U ? code >> 1U : code - 512U;
                ++waveform[(channel * kS + x) * 514U + level];
            }
            const auto [cb, cr] = vectorPosition(r, g, b);
            ambiguous += (margin(cb) < 1e-4 || margin(cr) < 1e-4) ? 1U : 0U;
            if (cb < 0.0 || cb >= 512.0 || cr < 0.0 || cr >= 512.0) {
                ++outside;
            } else {
                ++vectorscope[static_cast<std::size_t>(cr) * 512U + static_cast<std::size_t>(cb)];
            }
        }
    }
    check(ambiguous == 0U, "cross-check: pattern stays away from cell edges, ambiguous " +
                               std::to_string(ambiguous));
    std::uint32_t wrong = 0;
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        for (std::uint32_t column = 0; column < kS; ++column) {
            for (std::uint32_t l = 0; l < 514U; ++l) {
                wrong += counts->waveform(channel, column, l) !=
                                 waveform[(channel * kS + column) * 514U + l]
                             ? 1U
                             : 0U;
            }
        }
    }
    for (std::uint32_t y = 0; y < 512; ++y) {
        for (std::uint32_t x = 0; x < 512; ++x) {
            wrong +=
                counts->vectorscope(x, y) != vectorscope[static_cast<std::size_t>(y) * 512U + x]
                    ? 1U
                    : 0U;
        }
    }
    check(wrong == 0U, "cross-check: waveform and vectorscope tables equal the formulas");
    check(counts->vectorscopeOutside() == outside, "cross-check: outside counter");
}

[[nodiscard]] std::uint32_t tableMaxOf(const ScopeCounts& counts, const std::uint32_t offset,
                                       const std::size_t size) {
    std::uint32_t maximum = 0;
    for (std::size_t i = 0; i < size; ++i) {
        maximum = std::max(maximum, counts.words[offset + i]);
    }
    return maximum;
}

void roundTrip(const ScopeCounts& counts, const std::string& name) {
    for (const auto route : {runtime::ScopeRoute::Cpu, runtime::ScopeRoute::Gpu}) {
        const ScopeTables packed = ScopeTables::pack(counts, route);
        const ScopeCounts back = packed.unpack();
        check(back.words == counts.words && back.layout == counts.layout,
              name + ": unpack(pack(c)) == c");
        check(packed.route() == route && !packed.empty(), name + ": route kept");
        check(packed.counted() == counts.counted() && packed.notFinite() == counts.notFinite() &&
                  packed.noChromaticity() == counts.noChromaticity() &&
                  packed.chromaticityOutside() == counts.chromaticityOutside() &&
                  packed.vectorscopeOutside() == counts.vectorscopeOutside(),
              name + ": counters kept at 32 bits");
        check(packed.histogram(1, 5) == counts.histogram(1, 5) &&
                  packed.waveform(2, 1, 3) == counts.waveform(2, 1, 3),
              name + ": packed accessors");
        check(packed == ScopeTables::pack(back, route), name + ": packing is canonical");
    }
}

void testPacking() {
    const auto widthFor = [](const std::uint32_t maximum) {
        return maximum <= 255U ? 1U : (maximum <= 65535U ? 2U : 4U);
    };
    const Picture tiny = flat(15, 17, px(0.75F, 0.25F, 0.5F));  // 255 pixels
    const Picture mid = flat(16, 16, px(0.75F, 0.25F, 0.5F));   // 256 pixels
    const Picture big = flat(300, 300, px(0.75F, 0.25F, 0.5F)); // 90000 pixels
    std::uint32_t expectedWidths[3][4] = {{1, 1, 1, 1}, {2, 1, 2, 2}, {4, 2, 4, 4}};
    const Picture* pictures[3] = {&tiny, &mid, &big};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto counts = measure(*pictures[i], whole(*pictures[i]), "pack " + std::to_string(i));
        if (!counts) {
            continue;
        }
        roundTrip(*counts, "pack " + std::to_string(i));
        const ScopeTables packed = ScopeTables::pack(*counts, runtime::ScopeRoute::Cpu);
        const auto& layout = counts->layout;
        const std::uint32_t maxima[4] = {
            tableMaxOf(*counts, layout.histogramOffset, 4U * render::kScopeHistogramChannelWords),
            tableMaxOf(*counts, layout.waveformOffset, 4U * layout.waveformChannelWords()),
            tableMaxOf(*counts, layout.vectorscopeOffset, 512U * 512U),
            tableMaxOf(*counts, layout.chromaticityOffset, 512U * 512U)};
        const std::uint32_t scopes[4] = {render::kScopeTableHistogram, render::kScopeTableWaveform,
                                         render::kScopeTableVectorscope,
                                         render::kScopeTableChromaticity};
        for (std::size_t t = 0; t < 4; ++t) {
            check(packed.tableMaximum(scopes[t]) == maxima[t], "pack: table maximum");
            check(packed.tableWidthBytes(scopes[t]) == widthFor(maxima[t]),
                  "pack: narrowest width");
            check(packed.tableWidthBytes(scopes[t]) == expectedWidths[i][t],
                  "pack: expected width " + std::to_string(i));
        }
        check(packed.byteSize() < counts->words.size() * sizeof(std::uint32_t),
              "pack: smaller than the words");
    }
    // A subset: absent tables have no width and no maximum.
    const auto subset = measure(mid, whole(mid), "pack subset",
                                render::kScopeTableWaveform | render::kScopeTableChromaticity);
    if (subset) {
        roundTrip(*subset, "pack subset");
        const ScopeTables packed = ScopeTables::pack(*subset, runtime::ScopeRoute::Cpu);
        check(packed.tableWidthBytes(render::kScopeTableHistogram) == 0U &&
                  packed.tableMaximum(render::kScopeTableVectorscope) == 0U &&
                  packed.histogram(0, 0) == 0U,
              "pack: absent tables");
    }
    check(ScopeTables{}.empty() && ScopeTables{}.byteSize() > 0U, "default tables are empty");
    check(ScopeTables{}.unpack().words.empty(), "empty unpack");
}

void testSummary() {
    // Red values 0, 0.5, 0.5, 1, -1 (under), 2 (over); gray, so every channel is the same except
    // that only red is exact in binary32.
    const std::array<float, 6> values{0.0F, 0.5F, 0.5F, 1.0F, -1.0F, 2.0F};
    const Picture picture = generate(
        6, 1, [&](std::uint32_t x, std::uint32_t) { return px(values[x], values[x], values[x]); });
    const auto counts = measure(picture, whole(picture), "summary");
    if (!counts) {
        return;
    }
    const auto summary = runtime::summarizeScopes(*counts);
    const auto& red = summary.channels[runtime::kScopeChannelRed];
    check(red.available, "summary: available");
    check(red.minimum == 0U && red.maximum == 1023U, "summary: min and max of the bins");
    check(std::abs(red.mean - (0.0 + 512.0 * 2.0 + 1023.0) / 4.0) < 1e-9,
          "summary: mean of the bins");
    // ceil(0.01 * 6) = 1: the under cell, reported as 0; ceil(0.5 * 6) = 3: under 1, bin 0 -> 2,
    // bin 512 -> 4; ceil(0.99 * 6) = 6: only the over cell reaches it, reported as 1023.
    check(red.percentile1 == 0U && red.percentile50 == 512U && red.percentile99 == 1023U,
          "summary: percentiles");
    check(red.clippedUnder == 1U && red.clippedOver == 1U, "summary: clipped");
    check(summary.vectorscopeAvailable && summary.meanSaturation < 0.01,
          "summary: gray has no saturation");
    check(summary.meanHueDegrees >= 0.0 && summary.meanHueDegrees < 360.0, "summary: hue range");

    // The Vectorscope summary from cells set by hand: 3 pixels at (cb 384, cr 256), 1 at (256,
    // 384).
    ScopeCounts hand;
    hand.layout = render::scopeTableLayout(render::kScopeTableVectorscope, 4);
    hand.words.assign(hand.layout.wordCount, 0U);
    hand.words[hand.layout.vectorscopeOffset + 256U * 512U + 384U] = 3U;
    hand.words[hand.layout.vectorscopeOffset + 384U * 512U + 256U] = 1U;
    const auto handSummary = runtime::summarizeScopes(hand);
    const double a = 384.5 / 512.0 - 0.5;
    const double c = 256.5 / 512.0 - 0.5;
    const double distance = std::sqrt(a * a + c * c);
    check(handSummary.vectorscopeAvailable && !handSummary.channels[0].available,
          "summary: hand tables");
    check(std::abs(handSummary.meanSaturation - distance / 0.5) < 1e-12,
          "summary: mean saturation");
    const double meanCb = (3.0 * a + 1.0 * c) / 4.0;
    const double meanCr = (3.0 * c + 1.0 * a) / 4.0;
    const double degrees = std::atan2(meanCr, meanCb) * 180.0 / 3.14159265358979323846;
    check(std::abs(handSummary.meanHueDegrees - degrees) < 1e-9, "summary: mean hue");
    check(!runtime::summarizeScopes(ScopeCounts{}).vectorscopeAvailable,
          "summary: nothing measured");
}

void testInvalidAndCancel() {
    const Picture picture = flat(4, 4, px(0.5F, 0.5F, 0.5F));
    std::uint32_t calls = 0;
    auto input = inputFor(picture, whole(picture), render::kScopeTableAll, 1, false);
    const auto counted = runtime::measureScopesCpu(input, [&calls] {
        ++calls;
        return false;
    });
    check(counted.has_value() && calls == 4U, "cancel is asked once per row");
    check(!runtime::measureScopesCpu(input, [] { return true; }).has_value(),
          "cancelled measure gives nothing");
    std::uint32_t seen = 0;
    check(!runtime::measureScopesCpu(input, [&seen] { return ++seen == 3U; }).has_value() &&
              seen == 3U,
          "cancel between rows stops there");

    auto empty = input;
    empty.areaWidth = 0;
    check(!runtime::measureScopesCpu(empty).has_value(), "empty area refused");
    auto shortSignal = input;
    shortSignal.height = 5;
    check(!runtime::measureScopesCpu(shortSignal).has_value(), "short picture refused");
    auto noXyz = input;
    noXyz.xyz = {};
    check(!runtime::measureScopesCpu(noXyz).has_value(), "chromaticity without XYZ refused");
    noXyz.scopes = render::kScopeTableHistogram;
    check(runtime::measureScopesCpu(noXyz).has_value(), "no XYZ needed without chromaticity");
    auto huge = input;
    huge.areaWidth = 65536U;
    huge.areaHeight = 65536U;
    check(!runtime::measureScopesCpu(huge).has_value(), "more than 2^32 - 1 pixels refused");
}

// The measured area: the display window, cut to the region of interest and to the region, which
// is in composition pixels and scales outward to the picture.
void testMeasuredArea() {
    using runtime::ScopeMeasuredArea;
    using runtime::ScopeRegion;
    const auto window = [](const std::int64_t x, const std::int64_t y, const std::uint64_t w,
                           const std::uint64_t h) {
        const auto created = render::ImageWindow::create(x, y, w, h);
        if (created.value() == nullptr)
            throw std::logic_error("the test window must be valid");
        return *created.value();
    };
    const auto display = window(0, 0, 200, 100);
    const auto area = [&](const render::ImageWindow& data,
                          const std::optional<render::ImageWindow>& roi,
                          const std::optional<ScopeRegion>& region, const std::uint32_t width = 200,
                          const std::uint32_t height = 100) {
        return runtime::scopeMeasuredArea(data, display, roi, region, width, height);
    };
    check(area(display, {}, {}) == ScopeMeasuredArea{0, 0, 200, 100},
          "no region measures the display window");
    // A data window larger than, and offset from, the display window: the area is the display
    // window, from the data window's first pixel.
    check(area(window(-10, -5, 220, 110), {}, {}) == ScopeMeasuredArea{10, 5, 200, 100},
          "the area is counted from the data window's first pixel");
    // A data window smaller than the display window: the area reaches outside the picture.
    check(area(window(50, 20, 40, 30), {}, {}) == ScopeMeasuredArea{-50, -20, 200, 100},
          "the area may reach outside the stored picture");
    check(area(display, window(20, 10, 50, 40), {}) == ScopeMeasuredArea{20, 10, 50, 40},
          "a region of interest cuts the area");
    check(area(display, {}, ScopeRegion{30, 20, 60, 50}) == ScopeMeasuredArea{30, 20, 60, 50},
          "a region at full resolution is its own pixels");
    check(area(display, {}, ScopeRegion{-20, -10, 60, 50}) == ScopeMeasuredArea{0, 0, 40, 40},
          "a region is cut to the display window");
    check(area(display, window(20, 10, 50, 40), ScopeRegion{30, 0, 100, 30}) ==
              ScopeMeasuredArea{30, 10, 40, 20},
          "a region and a region of interest cut each other");
    // A proxy picture of a 400 x 200 composition: the region scales outward.
    check(area(display, {}, ScopeRegion{31, 21, 60, 50}, 400, 200) ==
              ScopeMeasuredArea{15, 10, 31, 26},
          "a region scales to a proxy picture outward");
    check(area(display, {}, ScopeRegion{-3, -3, 2, 2}, 400, 200) == std::nullopt,
          "a region left of the picture is empty");
    check(area(display, {}, ScopeRegion{-3, -3, 4, 4}, 400, 200) == ScopeMeasuredArea{0, 0, 1, 1},
          "a region that touches the first pixel measures it");
    check(area(display, {}, ScopeRegion{500, 10, 20, 20}) == std::nullopt,
          "a region outside the picture is empty");
    check(area(display, {}, ScopeRegion{10, 10, 0, 20}) == std::nullopt,
          "a region with no width is empty");
    check(area(display, window(300, 0, 10, 10), {}) == std::nullopt,
          "a region of interest outside the display window is empty");
    check(area(display, {}, ScopeRegion{10, 10, 20, 20}, 0, 100) == std::nullopt,
          "a region needs the composition's size");
}

// ---- Detail (#724): the Vectorscope, CIE and Waveform tables at a coarser detail.

// A deterministic picture of 64 x 64 whose pixels sit on cell and code edges (k / 512 and m / 1023
// for small k and m, which land on the edges of the full and the coarser cells) and, from an LCG,
// everywhere else, including values below 0 and above 1 (so some pixels are outside the
// Vectorscope and the CIE table, under or over in the Histogram and the Waveform). Its XYZ picture
// varies too, so the CIE table has many cells.
[[nodiscard]] Picture detailPicture() {
    std::uint32_t state = 12345U;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<double>(state >> 8U) / 16777216.0;
    };
    Picture picture = generate(64, 64, [&](const std::uint32_t x, const std::uint32_t y) {
        const std::uint32_t index = y * 64U + x;
        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        if (index % 3U == 0U) {
            // Exactly on edges: cell edges (k / 512) and code edges (m / 1023).
            r = static_cast<float>(index % 61U) / 512.0F;
            g = static_cast<float>(index % 67U) / 1023.0F;
            b = static_cast<float>(index % 71U) / 512.0F;
        } else {
            r = static_cast<float>(next() * 1.4 - 0.2);
            g = static_cast<float>(next() * 1.4 - 0.2);
            b = static_cast<float>(next() * 1.4 - 0.2);
        }
        return px(r, g, b, 1.0F);
    });
    for (auto& xyz : picture.xyz) {
        xyz = px(static_cast<float>(next() * 0.9), static_cast<float>(next() * 1.0),
                 static_cast<float>(next() * 1.1));
    }
    return picture;
}

[[nodiscard]] std::optional<ScopeCounts>
measureAtDetail(const Picture& picture, const std::uint32_t scopes, const std::uint32_t detail) {
    auto input = inputFor(picture, whole(picture), scopes, 1, false);
    input.detail = detail;
    return runtime::measureScopesCpu(input);
}

[[nodiscard]] std::uint64_t layoutBytes(const std::uint32_t scopes, const std::uint32_t width,
                                        const std::uint32_t detail) {
    return render::scopeTableLayout(scopes, width, detail).byteCount();
}

void testDetailLayout() {
    using namespace render;
    // The bytes of a readback of every table on a picture at least 512 wide, from the layout and
    // from the definition (counters, Histogram, Waveform columns, two grids).
    const std::uint64_t full =
        8U * 4U + 4U * 1026U * 4U + 4U * 512U * 514U * 4U + 2U * 512U * 512U * 4U;
    const std::uint64_t one =
        8U * 4U + 4U * 1026U * 4U + 4U * 256U * 258U * 4U + 2U * 256U * 256U * 4U;
    const std::uint64_t two =
        8U * 4U + 4U * 1026U * 4U + 4U * 128U * 130U * 4U + 2U * 128U * 128U * 4U;
    check(full == 6324288U && one == 1597504U && two == 413760U, "detail: the byte counts");
    check(layoutBytes(kScopeTableAll, 512, 0) == full, "detail 0: all tables at 512 wide");
    check(layoutBytes(kScopeTableAll, 512, 1) == one, "detail 1: all tables at 512 wide");
    check(layoutBytes(kScopeTableAll, 512, 2) == two, "detail 2: all tables at 512 wide");
    check(layoutBytes(kScopeTableAll, 4000, 2) == two, "detail 2: wider pictures add no columns");
    // One scope each, at 512 wide: counters 32 bytes plus the table.
    const std::uint32_t scopes[4] = {kScopeTableHistogram, kScopeTableWaveform,
                                     kScopeTableVectorscope, kScopeTableChromaticity};
    const std::uint64_t expected[3][4] = {{32U + 4U * 1026U * 4U, 32U + 4U * 512U * 514U * 4U,
                                           32U + 512U * 512U * 4U, 32U + 512U * 512U * 4U},
                                          {32U + 4U * 1026U * 4U, 32U + 4U * 256U * 258U * 4U,
                                           32U + 256U * 256U * 4U, 32U + 256U * 256U * 4U},
                                          {32U + 4U * 1026U * 4U, 32U + 4U * 128U * 130U * 4U,
                                           32U + 128U * 128U * 4U, 32U + 128U * 128U * 4U}};
    for (std::uint32_t d = 0; d <= 2U; ++d) {
        for (std::size_t t = 0; t < 4; ++t) {
            check(layoutBytes(scopes[t], 512, d) == expected[d][t],
                  "detail " + std::to_string(d) + ": one scope " + std::to_string(t));
        }
        const auto layout = scopeTableLayout(kScopeTableAll, 64, d);
        check(layout.detail == d && layout.columns == 64U &&
                  layout.waveformColumnWords() == (512U >> d) + 2U &&
                  layout.cells() == (512U >> d) && layout.waveformLevelUnder() == (512U >> d) &&
                  layout.waveformLevelOver() == (512U >> d) + 1U,
              "detail " + std::to_string(d) + ": layout fields");
    }
    check(scopeTableLayout(kScopeTableWaveform, 300, 1).columns == 256U &&
              scopeTableLayout(kScopeTableWaveform, 200, 1).columns == 200U &&
              scopeTableLayout(kScopeTableWaveform, 300, 2).columns == 128U,
          "detail: columns are min(512 >> detail, width)");
    check(!validScopeDetail(3U) && validScopeDetail(2U), "detail: 3 is not valid");
}

void testDetailPooling() {
    const Picture picture = detailPicture();
    const auto full = measureAtDetail(picture, render::kScopeTableAll, 0);
    check(full.has_value(), "detail: full measure");
    if (!full) {
        return;
    }
    check(nonZeroCells(*full, false) > 100U && nonZeroCells(*full, true) > 100U &&
              full->vectorscopeOutside() > 0U && full->chromaticityOutside() > 0U &&
              full->histogram(0, render::kScopeCodeUnder) > 0U &&
              full->histogram(0, render::kScopeCodeOver) > 0U,
          "detail: the picture reaches cells, outside, under and over");
    for (std::uint32_t d = 1; d <= 2U; ++d) {
        const std::string name = "detail " + std::to_string(d);
        const auto coarse = measureAtDetail(picture, render::kScopeTableAll, d);
        check(coarse.has_value(), name + ": measures");
        if (!coarse) {
            continue;
        }
        const auto& layout = coarse->layout;
        check(layout.detail == d && coarse->words.size() == layout.wordCount, name + ": layout");
        const std::uint32_t block = 1U << d;
        // The counters and the Histogram do not depend on the detail.
        for (std::uint32_t i = 0; i < render::kScopeCounterWords; ++i) {
            check(coarse->words[i] == full->words[i], name + ": counter " + std::to_string(i));
        }
        for (std::uint32_t channel = 0; channel < 4U; ++channel) {
            for (std::uint32_t i = 0; i < render::kScopeHistogramChannelWords; ++i) {
                check(coarse->histogram(channel, i) == full->histogram(channel, i),
                      name + ": histogram equals full detail's");
            }
        }
        // The grids are the full tables with each block of cells summed.
        std::uint32_t wrongVector = 0;
        std::uint32_t wrongChroma = 0;
        std::uint64_t coarseTotal = 0;
        for (std::uint32_t row = 0; row < layout.cells(); ++row) {
            for (std::uint32_t column = 0; column < layout.cells(); ++column) {
                std::uint32_t sumVector = 0;
                std::uint32_t sumChroma = 0;
                for (std::uint32_t dy = 0; dy < block; ++dy) {
                    for (std::uint32_t dx = 0; dx < block; ++dx) {
                        sumVector += full->vectorscope(column * block + dx, row * block + dy);
                        sumChroma += full->chromaticity(column * block + dx, row * block + dy);
                    }
                }
                wrongVector += coarse->vectorscope(column, row) != sumVector ? 1U : 0U;
                wrongChroma += coarse->chromaticity(column, row) != sumChroma ? 1U : 0U;
                coarseTotal += coarse->vectorscope(column, row);
            }
        }
        check(wrongVector == 0U && wrongChroma == 0U, name + ": grids are pooled full grids");
        check(coarseTotal + coarse->vectorscopeOutside() == coarse->counted(),
              name + ": vectorscope sum rule");
        // Outside the detail's table the accessors give nothing.
        check(coarse->vectorscope(layout.cells(), 0) == 0U &&
                  coarse->chromaticity(0, layout.cells()) == 0U,
              name + ": cells past the detail's side are absent");
        // Each Waveform column's levels are sums of 2^d adjacent full-detail levels (the same
        // columns: the picture is 64 wide).
        check(layout.columns == full->layout.columns, name + ": same columns");
        std::uint32_t wrongWave = 0;
        for (std::uint32_t channel = 0; channel < 4U; ++channel) {
            for (std::uint32_t column = 0; column < layout.columns; ++column) {
                for (std::uint32_t level = 0; level < layout.waveformLevels(); ++level) {
                    std::uint32_t sum = 0;
                    for (std::uint32_t k = 0; k < block; ++k) {
                        sum += full->waveform(channel, column, level * block + k);
                    }
                    wrongWave += coarse->waveform(channel, column, level) != sum ? 1U : 0U;
                }
                wrongWave +=
                    coarse->waveform(channel, column, layout.waveformLevelUnder()) !=
                            full->waveform(channel, column, full->layout.waveformLevelUnder())
                        ? 1U
                        : 0U;
                wrongWave +=
                    coarse->waveform(channel, column, layout.waveformLevelOver()) !=
                            full->waveform(channel, column, full->layout.waveformLevelOver())
                        ? 1U
                        : 0U;
            }
        }
        check(wrongWave == 0U, name + ": waveform levels are pooled full levels");
        check(coarse->words.size() < full->words.size(), name + ": fewer words");

        // Packing and unpacking work at any detail.
        roundTrip(*coarse, name);
        const ScopeTables packed = ScopeTables::pack(*coarse, runtime::ScopeRoute::Gpu);
        check(packed.layout().detail == d &&
                  packed.vectorscope(3, 5) == coarse->vectorscope(3, 5) &&
                  packed.vectorscope(layout.cells(), 0) == 0U,
              name + ": packed accessors follow the detail");
    }
    // A wider picture: the Waveform has 512 >> d columns, and a table of one scope is only that.
    const Picture wide = flat(600, 2, px(0.25F, 0.5F, 0.75F));
    const auto wideCounts = measureAtDetail(wide, render::kScopeTableWaveform, 1);
    check(wideCounts.has_value() && wideCounts->layout.columns == 256U &&
              wideCounts->words.size() == 8U + 4U * 256U * 258U,
          "detail 1: a wide picture has 256 columns");
    // Detail 3 is refused.
    check(!measureAtDetail(picture, render::kScopeTableAll, 3).has_value(), "detail 3 is refused");
    // The detail is part of the settings' equality, so of every identity that embeds them.
    runtime::ScopeMeasureSettings a;
    runtime::ScopeMeasureSettings b;
    check(a == b && a.detail == 0U, "settings: detail defaults to 0");
    b.detail = 1;
    check(!(a == b), "settings: a detail is part of equality");
}

void testDetailSummary() {
    // A saturated picture (Cb about -0.2, Cr about 0.3 at luma 0.5, a little noise) so that the
    // mean vector is far from the center.
    const Picture picture = generate(64, 64, [](const std::uint32_t x, const std::uint32_t y) {
        const double wobbleR = static_cast<double>((x * 7U + y * 3U) % 11U) / 11.0 * 0.04 - 0.02;
        const double wobbleB = static_cast<double>((x * 5U + y * 13U) % 9U) / 9.0 * 0.04 - 0.02;
        const double lumaTarget = 0.5;
        const double b = lumaTarget + 2.0 * (1.0 - kKb) * -0.2 + wobbleB;
        const double r = lumaTarget + 2.0 * (1.0 - kKr) * 0.3 + wobbleR;
        const double g = (lumaTarget - kKr * r - kKb * b) / kKg;
        return px(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
    });
    const auto full = measureAtDetail(picture, render::kScopeTableAll, 0);
    check(full.has_value(), "detail summary: full");
    if (!full) {
        return;
    }
    const auto reference = runtime::summarizeScopes(*full);
    check(reference.vectorscopeAvailable && reference.meanSaturation > 0.5,
          "detail summary: the picture is saturated");
    for (std::uint32_t d = 1; d <= 2U; ++d) {
        const auto coarse = measureAtDetail(picture, render::kScopeTableAll, d);
        check(coarse.has_value(), "detail summary: coarse");
        if (!coarse) {
            continue;
        }
        const auto summary = runtime::summarizeScopes(*coarse);
        // The Histogram does not change: every channel summary is equal.
        check(summary.channels == reference.channels, "detail summary: channels are equal");
        // A pixel and the center of its detail cell are at most half a cell apart on each axis,
        // sqrt(2) * 2^d / 1024 in distance, which is sqrt(2) * 2^d / 512 of the 0.5 radius: the
        // tolerance on the mean saturation. The mean vector moves by at most the same distance
        // (in cb, cr); with the mean's length of at least 0.2 here that turns the hue by at most
        // asin(shift / 0.2) degrees.
        const double shift = std::sqrt(2.0) * static_cast<double>(1U << d) / 1024.0;
        check(summary.vectorscopeAvailable &&
                  std::abs(summary.meanSaturation - reference.meanSaturation) <= shift / 0.5,
              "detail summary: mean saturation within one detail cell");
        const double hueTolerance = std::asin(shift / 0.2) * 180.0 / 3.14159265358979323846;
        check(std::abs(summary.meanHueDegrees - reference.meanHueDegrees) <= hueTolerance,
              "detail summary: mean hue within the bound");
    }
}

} // namespace

int main() {
    try {
        testLuma();
        testFlatField();
        testRamp();
        testColorBars();
        testCounters();
        testAreas();
        testWaveformColumns();
        testAxisMaximum();
        testCrossCheck();
        testPacking();
        testSummary();
        testInvalidAndCancel();
        testMeasuredArea();
        testDetailLayout();
        testDetailPooling();
        testDetailSummary();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        return 1;
    }
    std::cout << "scope measure tests passed\n";
    return 0;
}
