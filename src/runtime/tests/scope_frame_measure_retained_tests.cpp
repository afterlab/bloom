// #724 Scopes: the measure of a retained display picture (the packed straight RGBA8 buffer a
// retained preview frame carries), counted on the calling thread. CPU only: no device.
//
// The expectations are computed here from the definitions of docs/architecture/scopes.md and of
// measureScopeFrameRetained, in double and in integers, not by calling the code under test:
//   * flat field, ramp and color bars against hand-computed cells and counts;
//   * the order of the signal: code / 255, exposure then gamma (clamped), then times alpha;
//   * an adjustment already baked into the picture is not applied again;
//   * the Working signal is refused with its message and counts nothing;
//   * the CIE table, and every other table, equal the CPU reference fed the same float values;
//   * a region (also of a proxy picture), Ignore Transparent Pixels, detail 1 and 2 pooled from
//     detail 0, an empty area and cancellation.

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include "await.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using render::Rgba8;
using render::Rgba32f;
using runtime::ScopeCounts;
using runtime::ScopeFrameMeasure;
using runtime::ScopeFrameStatus;
using runtime::ScopeMeasureSettings;
using runtime::ScopeSignal;
using runtime::ScopeSignalColor;
using runtime::ViewAdjust;

int failures = 0;

void check(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr double kKr = 0.2126;
constexpr double kKg = 0.7152;
constexpr double kKb = 0.0722;

[[nodiscard]] render::ImageWindow windowOf(const std::int64_t x, const std::int64_t y,
                                           const std::uint64_t w, const std::uint64_t h) {
    const auto created = render::ImageWindow::create(x, y, w, h);
    if (!created) {
        std::cerr << "FAIL: invalid fixture window\n";
        std::abort();
    }
    return *created.value();
}

// A retained picture and the view of it a frame would give.
struct Picture final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Rgba8> pixels;
    std::optional<render::ImageWindow> dataWindow;

    [[nodiscard]] runtime::PreviewDisplayBufferView view() const {
        const auto window = windowOf(0, 0, width, height);
        const auto stored = dataWindow.value_or(window);
        const auto w = static_cast<std::size_t>(stored.extent().width());
        const auto h = static_cast<std::size_t>(stored.extent().height());
        return runtime::PreviewDisplayBufferView{
            .displayWindow = window,
            .pixelAspect = core::PixelAspectRatio::square(),
            .layout = {.pixelCount = w * h,
                       .rowStrideBytes = w * sizeof(Rgba8),
                       .pixelStorageBytes = w * h * sizeof(Rgba8)},
            .pixels = pixels,
            .isOcioQualified = false,
            .dataWindow = dataWindow,
        };
    }
};

[[nodiscard]] Picture makePicture(const std::uint32_t width, const std::uint32_t height,
                                  const Rgba8 fill) {
    Picture picture;
    picture.width = width;
    picture.height = height;
    picture.pixels.assign(static_cast<std::size_t>(width) * height, fill);
    return picture;
}

template <typename F>
[[nodiscard]] Picture generate(const std::uint32_t width, const std::uint32_t height, F&& f) {
    auto picture = makePicture(width, height, Rgba8{0, 0, 0, 255});
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            picture.pixels[static_cast<std::size_t>(y) * width + x] = f(x, y);
    return picture;
}

// The code of a channel value k / 255 on the 1023 axis, in integers: floor(k * 1023 / 255 + 1/2).
// (k * 1023 / 255 is never half an integer, so the float rounding of the measure cannot differ.)
[[nodiscard]] std::uint32_t codeOfByte(const std::uint32_t k) {
    return (2U * k * 1023U + 255U) / 510U;
}

// The code of a double value on the 1023 axis; 1024 under, 1025 over.
[[nodiscard]] std::uint32_t codeOf(const double value) {
    const double c = std::floor(value * 1023.0 + 0.5);
    if (c < 0.0)
        return 1024U;
    return c > 1023.0 ? 1025U : static_cast<std::uint32_t>(c);
}

[[nodiscard]] double lumaOf(const double r, const double g, const double b) {
    return (kKr * r + kKg * g) + kKb * b;
}

// The Vectorscope position in cells, before the floor.
[[nodiscard]] std::pair<double, double> vectorPosition(const double r, const double g,
                                                       const double b) {
    const double y = lumaOf(r, g, b);
    return {((b - y) / (2.0 * (1.0 - kKb)) + 0.5) * 512.0,
            ((r - y) / (2.0 * (1.0 - kKr)) + 0.5) * 512.0};
}

// sRGB, written out here (not ViewAdjust): the display encoding's two directions.
[[nodiscard]] double srgbToLinear(const double e) {
    return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
}
[[nodiscard]] double linearToSrgb(const double v) {
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

[[nodiscard]] ScopeMeasureSettings settingsFor(const std::uint32_t scopes = render::kScopeTableAll) {
    ScopeMeasureSettings settings;
    settings.scopes = scopes;
    settings.signal = ScopeSignal::Display;
    return settings;
}

struct Colors final {
    std::shared_ptr<const ScopeSignalColor> display;
    std::shared_ptr<const ScopeSignalColor> working;
    std::shared_ptr<const color::ResolvedBloomNeutralConfig> config;

    [[nodiscard]] std::shared_ptr<const ScopeSignalColor> displayWith(const ViewAdjust adjust) const {
        auto prepared = ScopeSignalColor::prepare({.config = config,
                                                   .signal = ScopeSignal::Display,
                                                   .display = {},
                                                   .view = {},
                                                   .viewAdjust = adjust});
        check(static_cast<bool>(prepared), "the adjusted display color prepares");
        return prepared.color;
    }
};

[[nodiscard]] std::optional<Colors> prepareColors() {
    auto neutral =
        color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn,
                                  color::kBloomNeutralV1ConfigUri,
                                  color::ocioBuiltInContentRevision(
                                      color::OcioConfigLocatorKind::BloomBuiltIn,
                                      color::kBloomNeutralV1ConfigUri)
                                      .value_or(color::kBloomNeutralV1ConfigDigest),
                                  "lin_rec709_scene")
            .takeResolved();
    if (!neutral)
        return std::nullopt;
    Colors colors;
    colors.config =
        std::make_shared<const color::ResolvedBloomNeutralConfig>(std::move(*neutral));
    colors.display = colors.displayWith({});
    auto working = ScopeSignalColor::prepare({.config = colors.config,
                                              .signal = ScopeSignal::Working,
                                              .display = {},
                                              .view = {},
                                              .viewAdjust = {}});
    colors.working = working.color;
    if (colors.display == nullptr || colors.working == nullptr)
        return std::nullopt;
    return colors;
}

[[nodiscard]] ScopeFrameMeasure measure(const Picture& picture, const ScopeMeasureSettings& settings,
                                        const ScopeSignalColor& color,
                                        const runtime::RetainedScopePicture& info = {64, 64, {}},
                                        const runtime::CancellationToken& cancel = {}) {
    return runtime::measureScopeFrameRetained(picture.view(), info, settings, color, cancel);
}

[[nodiscard]] std::uint32_t nonZeroBins(const ScopeCounts& counts, const std::uint32_t channel) {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < render::kScopeHistogramChannelWords; ++i)
        n += counts.histogram(channel, i) != 0U ? 1U : 0U;
    return n;
}

[[nodiscard]] std::uint32_t nonZeroCells(const ScopeCounts& counts, const bool chromaticity) {
    const auto cells = counts.layout.cells();
    std::uint32_t n = 0;
    for (std::uint32_t y = 0; y < cells; ++y)
        for (std::uint32_t x = 0; x < cells; ++x)
            n += (chromaticity ? counts.chromaticity(x, y) : counts.vectorscope(x, y)) != 0U ? 1U
                                                                                              : 0U;
    return n;
}

[[nodiscard]] bool measured(const ScopeFrameMeasure& result, const std::string& name) {
    check(result.status == ScopeFrameStatus::Measured, name + ": measured: " + result.message);
    check(result.source == runtime::ScopeSource::Retained &&
              result.route == runtime::ScopeRoute::Cpu,
          name + ": a retained measure is Retained, counted by the CPU");
    return result.status == ScopeFrameStatus::Measured;
}

void testFlatField(const Colors& colors) {
    constexpr std::uint32_t kW = 16;
    constexpr std::uint32_t kH = 8;
    const auto picture = makePicture(kW, kH, Rgba8{192, 64, 128, 255});
    const auto result = measure(picture, settingsFor(), *colors.display);
    if (!measured(result, "flat"))
        return;
    const auto& counts = result.counts;
    check(result.area == runtime::ScopeMeasuredArea{0, 0, kW, kH}, "flat: the whole picture");
    check(counts.counted() == kW * kH && counts.notFinite() == 0U, "flat: counters");
    // Hand-computed: 192 -> 770, 64 -> 257, 128 -> 514 (literals) and the luma of those values.
    check(codeOfByte(192) == 770U && codeOfByte(64) == 257U && codeOfByte(128) == 514U &&
              codeOfByte(0) == 0U && codeOfByte(255) == 1023U,
          "flat: the integer code agrees with the hand-computed ones");
    const double r = 192.0 / 255.0;
    const double g = 64.0 / 255.0;
    const double b = 128.0 / 255.0;
    const std::array<std::uint32_t, 4> codes{770U, 257U, 514U, codeOf(lumaOf(r, g, b))};
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        check(nonZeroBins(counts, channel) == 1U &&
                  counts.histogram(channel, codes[channel]) == kW * kH,
              "flat: one histogram bin, channel " + std::to_string(channel));
        check(counts.layout.columns == kW, "flat: one waveform column per pixel column");
        for (std::uint32_t column = 0; column < kW; ++column)
            check(counts.waveform(channel, column, codes[channel] >> 1U) == kH,
                  "flat: one waveform level per column");
        check(counts.histogram(channel, render::kScopeCodeUnder) == 0U &&
                  counts.histogram(channel, render::kScopeCodeOver) == 0U,
              "flat: nothing under or over");
    }
    const auto [cb, cr] = vectorPosition(r, g, b);
    check(nonZeroCells(counts, false) == 1U &&
              counts.vectorscope(static_cast<std::uint32_t>(std::floor(cb)),
                                 static_cast<std::uint32_t>(std::floor(cr))) == kW * kH,
          "flat: one vectorscope cell");
    check(nonZeroCells(counts, true) == 1U, "flat: one chromaticity cell");
}

void testRamp(const Colors& colors) {
    constexpr std::uint32_t kH = 4;
    const auto picture = generate(256, kH, [](const std::uint32_t x, std::uint32_t) {
        return Rgba8{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x),
                     static_cast<std::uint8_t>(x), 255};
    });
    const auto result = measure(picture, settingsFor(), *colors.display);
    if (!measured(result, "ramp"))
        return;
    const auto& counts = result.counts;
    check(counts.counted() == 256U * kH && counts.layout.columns == 256U, "ramp: counters");
    for (std::uint32_t x = 0; x < 256; ++x) {
        const auto code = codeOfByte(x);
        for (std::uint32_t channel = 0; channel < 4; ++channel) {
            check(counts.histogram(channel, code) == kH,
                  "ramp: histogram bin of step " + std::to_string(x) + ", channel " +
                      std::to_string(channel));
            check(counts.waveform(channel, x, code >> 1U) == kH,
                  "ramp: waveform level of column " + std::to_string(x));
        }
    }
    check(nonZeroBins(counts, 0) == 256U, "ramp: 256 distinct bins");
    // A gray ramp sits on the vectorscope's center: its cell, or the one before it when the
    // binary32 luma differs from the pixel by one unit in the last place.
    std::uint32_t center = 0;
    for (std::uint32_t y = 255; y <= 256; ++y)
        for (std::uint32_t x = 255; x <= 256; ++x)
            center += counts.vectorscope(x, y);
    check(center == counts.counted(), "ramp: the vectorscope counts the center");
}

void testColorBars(const Colors& colors) {
    // Eight bars of 4 pixels, 4 rows: white, yellow, cyan, green, magenta, red, blue, black at 191.
    constexpr std::uint32_t kBar = 4;
    constexpr std::uint32_t kH = 4;
    constexpr std::uint8_t kLevel = 191;
    const std::array<std::array<bool, 3>, 8> bars{{{true, true, true},
                                                    {true, true, false},
                                                    {false, true, true},
                                                    {false, true, false},
                                                    {true, false, true},
                                                    {true, false, false},
                                                    {false, false, true},
                                                    {false, false, false}}};
    const auto picture = generate(8 * kBar, kH, [&](const std::uint32_t x, std::uint32_t) {
        const auto& bar = bars[x / kBar];
        return Rgba8{static_cast<std::uint8_t>(bar[0] ? kLevel : 0),
                     static_cast<std::uint8_t>(bar[1] ? kLevel : 0),
                     static_cast<std::uint8_t>(bar[2] ? kLevel : 0), 255};
    });
    const auto result = measure(picture, settingsFor(), *colors.display);
    if (!measured(result, "bars"))
        return;
    const auto& counts = result.counts;
    check(counts.counted() == 8U * kBar * kH, "bars: counted");
    const double level = static_cast<double>(kLevel) / 255.0;
    const std::uint32_t levelCode = codeOfByte(kLevel);
    check(levelCode == 766U, "bars: 191 is code 766 (hand-computed)");
    for (std::uint32_t channel = 0; channel < 3; ++channel) {
        std::uint32_t lit = 0;
        for (const auto& bar : bars)
            lit += bar[channel] ? 1U : 0U;
        check(counts.histogram(channel, levelCode) == lit * kBar * kH &&
                  counts.histogram(channel, 0U) == (8U - lit) * kBar * kH,
              "bars: histogram of channel " + std::to_string(channel));
        for (std::uint32_t bar = 0; bar < 8; ++bar)
            for (std::uint32_t x = bar * kBar; x < (bar + 1) * kBar; ++x)
                check(counts.waveform(channel, x, (bars[bar][channel] ? levelCode : 0U) >> 1U) ==
                          kH,
                      "bars: waveform level, channel " + std::to_string(channel));
    }
    // The chromatic bars sit on a cell computed in double; the gray ones (white, black) on the
    // center cell or its lower neighbor.
    std::uint32_t grays = 0;
    for (std::uint32_t bar = 0; bar < 8; ++bar) {
        const auto& v = bars[bar];
        if (v[0] == v[1] && v[1] == v[2]) {
            ++grays;
            continue;
        }
        const auto [cb, cr] = vectorPosition(v[0] ? level : 0.0, v[1] ? level : 0.0,
                                             v[2] ? level : 0.0);
        check(cb >= 0.0 && cb < 512.0 && cr >= 0.0 && cr < 512.0,
              "bars: the chromatic bar is inside the vectorscope");
        check(counts.vectorscope(static_cast<std::uint32_t>(std::floor(cb)),
                                 static_cast<std::uint32_t>(std::floor(cr))) == kBar * kH,
              "bars: vectorscope cell of bar " + std::to_string(bar));
    }
    std::uint32_t center = 0;
    for (std::uint32_t y = 255; y <= 256; ++y)
        for (std::uint32_t x = 255; x <= 256; ++x)
            center += counts.vectorscope(x, y);
    check(grays == 2U && center == grays * kBar * kH, "bars: white and black sit at the center");
    check(counts.vectorscopeOutside() == 0U, "bars: nothing outside the vectorscope");
}

// The expected stored value of one channel: code / 255, exposure in linear light, the display
// encoding, clamp, gamma, then times alpha. Written out here in double.
[[nodiscard]] double expectedChannel(const std::uint32_t k, const ViewAdjust adjust,
                                     const std::uint32_t alpha) {
    double value = static_cast<double>(k) / 255.0;
    if (adjust.exposure != 0.0)
        value = linearToSrgb(srgbToLinear(value) * std::exp2(adjust.exposure));
    value = std::clamp(value, 0.0, 1.0);
    if (adjust.gamma != 1.0)
        value = std::pow(value, 1.0 / adjust.gamma);
    return value * (static_cast<double>(alpha) / 255.0);
}

void testSignalOrder(const Colors& colors) {
    // One pixel: red 100, green 200 (which exposure pushes over 1: the clamp), blue 50, alpha 128.
    const auto picture = makePicture(4, 4, Rgba8{100, 200, 50, 128});
    const ViewAdjust adjust{.exposure = 1.0, .gamma = 2.0};
    const auto adjusted = colors.displayWith(adjust);
    if (adjusted == nullptr)
        return;
    const auto settings = settingsFor(render::kScopeTableHistogram);
    const auto result = measure(picture, settings, *adjusted);
    if (!measured(result, "order"))
        return;
    const auto& counts = result.counts;
    const std::array<std::uint32_t, 3> channels{100U, 200U, 50U};
    for (std::uint32_t channel = 0; channel < 3; ++channel) {
        const auto expected = codeOf(expectedChannel(channels[channel], adjust, 128));
        check(counts.histogram(channel, expected) == 16U,
              "order: channel " + std::to_string(channel) + " is in bin " +
                  std::to_string(expected));
        // The orders that differ must put the pixel elsewhere, or this test proves nothing.
        const double a = 128.0 / 255.0;
        const double c = static_cast<double>(channels[channel]) / 255.0;
        const auto alphaFirst = codeOf(
            std::pow(std::clamp(linearToSrgb(srgbToLinear(c * a) * 2.0), 0.0, 1.0), 0.5));
        check(alphaFirst != expected, "order: alpha before the adjustment would differ");
    }
    // The clamp: green over 1 after exposure is exactly alpha (1 * alpha), not above it.
    check(counts.histogram(1, codeOf(128.0 / 255.0)) == 16U,
          "order: the clamped green times alpha is alpha");
    check(counts.histogram(0, render::kScopeCodeOver) == 0U &&
              counts.histogram(1, render::kScopeCodeOver) == 0U &&
              counts.histogram(1, render::kScopeCodeUnder) == 0U,
          "order: the clamp leaves nothing over or under");
    // Gamma then clamp would differ from clamp then gamma for green: 1.156^(1/2) > 1.
    const double lin = srgbToLinear(200.0 / 255.0) * 2.0;
    check(lin > 1.0 && linearToSrgb(lin) > 1.0, "order: the fixture does clamp");

    // The same picture with the adjustment already baked in is not adjusted again.
    runtime::RetainedScopePicture baked{64, 64, adjust};
    const auto notTwice = measure(picture, settings, *adjusted, baked);
    if (measured(notTwice, "baked")) {
        for (std::uint32_t channel = 0; channel < 3; ++channel)
            check(notTwice.counts.histogram(channel,
                                            codeOf(expectedChannel(channels[channel],
                                                                   ViewAdjust{}, 128))) == 16U,
                  "baked: the adjustment is not applied twice, channel " +
                      std::to_string(channel));
    }
    // A picture that holds another non-neutral adjustment cannot be adjusted to this one.
    const auto other = measure(picture, settings, *adjusted,
                               runtime::RetainedScopePicture{64, 64, {.exposure = -1.0, .gamma = 1.0}});
    check(other.status == ScopeFrameStatus::Unavailable && !other.message.empty() &&
              other.counts.words.empty(),
          "baked: a different baked adjustment is Unavailable");
    // A neutral adjustment keeps the encoded values as they are (alpha 255).
    const auto neutral = measure(makePicture(2, 2, Rgba8{10, 130, 250, 255}), settings,
                                 *colors.display);
    if (measured(neutral, "neutral"))
        check(neutral.counts.histogram(0, codeOfByte(10)) == 4U &&
                  neutral.counts.histogram(1, codeOfByte(130)) == 4U &&
                  neutral.counts.histogram(2, codeOfByte(250)) == 4U,
              "neutral: the encoded values are counted as they are");
}

void testWorkingRefused(const Colors& colors) {
    ScopeMeasureSettings settings = settingsFor();
    settings.signal = ScopeSignal::Working;
    settings.axisMaximum = 4;
    const auto picture = makePicture(4, 4, Rgba8{10, 20, 30, 255});
    const auto result = measure(picture, settings, *colors.working);
    check(result.status == ScopeFrameStatus::Unavailable &&
              result.message.find("The Working signal is not available for a cached frame") == 0U &&
              result.counts.words.empty() && result.source == runtime::ScopeSource::Retained,
          "working: refused with its message and no tables: " + result.message);
}

[[nodiscard]] Rgba32f raw(const float r, const float g, const float b, const float a) {
    return std::bit_cast<Rgba32f>(std::array<float, 4>{r, g, b, a});
}

// The reference: the same float values through the CPU reference, converted independently here.
[[nodiscard]] std::optional<ScopeCounts>
reference(const Picture& picture, const ScopeSignalColor& color, const ViewAdjust adjust,
          const ScopeMeasureSettings& settings, const runtime::ScopeMeasuredArea& area) {
    std::vector<Rgba32f> signal(picture.pixels.size(), Rgba32f::transparent());
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const auto& p = picture.pixels[i];
        if (p.alpha == 0)
            continue;
        const float alpha = static_cast<float>(p.alpha) / 255.0F;
        const auto channel = [&](const std::uint8_t c) {
            const double e = static_cast<double>(static_cast<float>(c) / 255.0F);
            const float v = adjust.neutral() ? static_cast<float>(e)
                                             : static_cast<float>(ViewAdjust{adjust}.fromEncoded(e));
            return v * alpha;
        };
        signal[i] = raw(channel(p.red), channel(p.green), channel(p.blue), alpha);
    }
    std::vector<Rgba32f> xyz(signal.size(), Rgba32f::transparent());
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const std::array<float, 3> rgb{signal[i].red(), signal[i].green(), signal[i].blue()};
        std::array<float, 3> out{};
        if (!color.xyz().apply(rgb, out))
            return std::nullopt;
        xyz[i] = raw(out[0], out[1], out[2], signal[i].alpha());
    }
    runtime::ScopeMeasureInput input;
    input.signal = signal;
    input.xyz = xyz;
    input.width = picture.width;
    input.height = picture.height;
    input.areaX = area.x;
    input.areaY = area.y;
    input.areaWidth = area.width;
    input.areaHeight = area.height;
    input.scopes = settings.scopes;
    input.axisMaximum = settings.axisMaximum;
    input.luma = color.luma();
    input.ignoreTransparent = settings.ignoreTransparent;
    input.detail = settings.detail;
    return runtime::measureScopesCpu(input);
}

[[nodiscard]] Picture pattern(const std::uint32_t w, const std::uint32_t h) {
    return generate(w, h, [](const std::uint32_t x, const std::uint32_t y) {
        const std::uint32_t alpha = (x + y) % 7U == 0U ? 0U : (x % 5U == 0U ? 77U : 255U);
        return Rgba8{static_cast<std::uint8_t>((x * 4U + y) & 255U),
                     static_cast<std::uint8_t>((y * 7U + 40U) & 255U),
                     static_cast<std::uint8_t>((x * 3U + y * 5U) & 255U),
                     static_cast<std::uint8_t>(alpha)};
    });
}

void testEqualsReference(const Colors& colors) {
    const auto picture = pattern(64, 32);
    const runtime::ScopeMeasuredArea whole{0, 0, 64, 32};
    for (const bool adjusted : {false, true}) {
        const ViewAdjust adjust = adjusted ? ViewAdjust{.exposure = -0.5, .gamma = 1.4}
                                           : ViewAdjust{};
        const auto color = adjusted ? colors.displayWith(adjust) : colors.display;
        if (color == nullptr)
            continue;
        for (const bool ignore : {false, true}) {
            auto settings = settingsFor();
            settings.ignoreTransparent = ignore;
            const std::string name = std::string("reference ") + (adjusted ? "adjusted" : "neutral") +
                                     (ignore ? " ignoring" : "");
            const auto result = measure(picture, settings, *color);
            const auto expected = reference(picture, *color, adjust, settings, whole);
            check(expected.has_value(), name + ": the reference counts");
            if (measured(result, name) && expected) {
                check(result.counts == *expected, name + ": every table equals the CPU reference");
                check(result.counts.chromaticity(0, 0) == expected->chromaticity(0, 0) &&
                          nonZeroCells(result.counts, true) == nonZeroCells(*expected, true) &&
                          nonZeroCells(result.counts, true) > 1U,
                      name + ": the CIE table is populated and equal");
            }
        }
    }
}

void testRegionAndIgnore(const Colors& colors) {
    // 64 x 32: the region holds one color, the rest another; half of the region is transparent.
    const auto picture = generate(64, 32, [](const std::uint32_t x, const std::uint32_t y) {
        const bool inside = x >= 16 && x < 48 && y >= 8 && y < 24;
        if (!inside)
            return Rgba8{255, 255, 255, 255};
        return Rgba8{static_cast<std::uint8_t>(64), static_cast<std::uint8_t>(64),
                     static_cast<std::uint8_t>(64), static_cast<std::uint8_t>(y < 16 ? 0 : 255)};
    });
    auto settings = settingsFor();
    settings.region = runtime::ScopeRegion{16, 8, 32, 16};
    auto result = measure(picture, settings, *colors.display, {64, 32, {}});
    if (measured(result, "region")) {
        check(result.area == runtime::ScopeMeasuredArea{16, 8, 32, 16}, "region: the area");
        check(result.counts.counted() == 32U * 16U && result.counts.layout.columns == 32U,
              "region: only the region is counted");
        check(result.counts.histogram(0, 1023U) == 0U &&
                  result.counts.histogram(0, 257U) == 32U * 8U &&
                  result.counts.histogram(0, 0U) == 32U * 8U,
              "region: the transparent half counts at 0, the opaque half at code 257");
    }
    // A region drawn at 2x composition resolution measures the same part of the picture.
    settings.region = runtime::ScopeRegion{32, 16, 64, 32};
    result = measure(picture, settings, *colors.display, {128, 64, {}});
    if (measured(result, "proxy region"))
        check(result.area == runtime::ScopeMeasuredArea{16, 8, 32, 16},
              "proxy region: scaled to the picture");
    // Ignore Transparent Pixels: the transparent half is not counted.
    settings.region = runtime::ScopeRegion{16, 8, 32, 16};
    settings.ignoreTransparent = true;
    result = measure(picture, settings, *colors.display, {64, 32, {}});
    if (measured(result, "ignore"))
        check(result.counts.counted() == 32U * 8U && result.counts.histogram(0, 0U) == 0U &&
                  result.counts.histogram(0, 257U) == 32U * 8U,
              "ignore: transparent pixels are not counted at all");
    // A region outside the picture.
    settings.region = runtime::ScopeRegion{200, 200, 8, 8};
    result = measure(picture, settings, *colors.display, {64, 32, {}});
    check(result.status == ScopeFrameStatus::EmptyArea && result.counts.words.empty(),
          "region outside the picture is an empty area");
    // A packed region-of-interest picture holds only its data window.
    Picture packed = makePicture(16, 8, Rgba8{128, 128, 128, 255});
    packed.width = 64;
    packed.height = 32;
    packed.dataWindow = windowOf(8, 4, 16, 8);
    result = measure(packed, settingsFor(), *colors.display, {64, 32, {}});
    if (measured(result, "roi"))
        check(result.area == runtime::ScopeMeasuredArea{0, 0, 16, 8} &&
                  result.counts.counted() == 16U * 8U,
              "roi: the packed region of interest is the measured area");
}

void testDetail(const Colors& colors) {
    const auto picture = pattern(64, 32);
    auto settings = settingsFor();
    const auto full = measure(picture, settings, *colors.display);
    if (!measured(full, "detail 0"))
        return;
    for (std::uint32_t detail = 1; detail <= 2; ++detail) {
        settings.detail = detail;
        const auto pooled = measure(picture, settings, *colors.display);
        const std::string name = "detail " + std::to_string(detail);
        if (!measured(pooled, name))
            continue;
        const auto& a = full.counts;
        const auto& b = pooled.counts;
        const std::uint32_t block = 1U << detail;
        check(b.layout.detail == detail && b.counted() == a.counted() &&
                  b.notFinite() == a.notFinite() && b.noChromaticity() == a.noChromaticity() &&
                  b.chromaticityOutside() == a.chromaticityOutside() &&
                  b.vectorscopeOutside() == a.vectorscopeOutside(),
              name + ": counters equal");
        bool histogram = true;
        for (std::uint32_t channel = 0; channel < 4; ++channel)
            for (std::uint32_t i = 0; i < render::kScopeHistogramChannelWords; ++i)
                histogram = histogram && a.histogram(channel, i) == b.histogram(channel, i);
        check(histogram, name + ": histogram equal");
        bool grids = true;
        for (std::uint32_t y = 0; y < b.layout.cells(); ++y) {
            for (std::uint32_t x = 0; x < b.layout.cells(); ++x) {
                std::uint32_t vector = 0;
                std::uint32_t cie = 0;
                for (std::uint32_t dy = 0; dy < block; ++dy)
                    for (std::uint32_t dx = 0; dx < block; ++dx) {
                        vector += a.vectorscope(x * block + dx, y * block + dy);
                        cie += a.chromaticity(x * block + dx, y * block + dy);
                    }
                grids = grids && b.vectorscope(x, y) == vector && b.chromaticity(x, y) == cie;
            }
        }
        check(grids, name + ": vectorscope and CIE are the full tables with blocks summed");
        bool waveform = b.layout.columns == a.layout.columns;
        for (std::uint32_t channel = 0; waveform && channel < 4; ++channel)
            for (std::uint32_t column = 0; column < a.layout.columns; ++column) {
                for (std::uint32_t level = 0; level < b.layout.waveformLevels(); ++level) {
                    std::uint32_t sum = 0;
                    for (std::uint32_t k = 0; k < block; ++k)
                        sum += a.waveform(channel, column, level * block + k);
                    waveform = waveform && b.waveform(channel, column, level) == sum;
                }
                waveform = waveform &&
                           b.waveform(channel, column, b.layout.waveformLevelUnder()) ==
                               a.waveform(channel, column, a.layout.waveformLevelUnder()) &&
                           b.waveform(channel, column, b.layout.waveformLevelOver()) ==
                               a.waveform(channel, column, a.layout.waveformLevelOver());
            }
        check(waveform, name + ": waveform levels are sums of the full levels");
    }
}

void testWrongInputs(const Colors& colors) {
    // The color objects of the other signal.
    const auto picture = makePicture(4, 4, Rgba8{1, 2, 3, 255});
    auto result = measure(picture, settingsFor(), *colors.working);
    check(result.status == ScopeFrameStatus::Failed, "a mismatched signal and color fails");
    // Pixels that do not fill the window.
    auto broken = picture;
    broken.pixels.pop_back();
    auto view = broken.view();
    view.layout.pixelCount = 15;
    result = runtime::measureScopeFrameRetained(view, {64, 64, {}}, settingsFor(),
                                                *colors.display);
    check(result.status == ScopeFrameStatus::Failed, "a picture smaller than its window fails");
}

void testCancellation(const Colors& colors) {
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(724)};
    const auto picture = pattern(64, 32);
    ScopeFrameMeasure result;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Retained scope cancellation", owner),
        [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            result = measure(picture, settingsFor(), *colors.display, {64, 32, {}},
                             context.cancellation());
            return runtime::TaskResult<void>::cancelled();
        });
    check(submission.accepted(), "cancellation: the task is admitted");
    std::optional<runtime::TaskResult<void>> outcome;
    if (bloom::test_support::awaitCondition("the cancelled measure's task ends", [&] {
            outcome = submission.handle.tryTakeResult();
            return outcome.has_value();
        })) {
        check(result.status == ScopeFrameStatus::Cancelled && result.counts.words.empty() &&
                  result.source == runtime::ScopeSource::Retained,
              "cancellation: a cancelled measure returns Cancelled and no tables");
    }
    scheduler.beginShutdown();
    check(bloom::test_support::awaitCondition("the scheduler quiesces",
                                              [&] { return scheduler.isQuiescent(); }),
          "cancellation: the scheduler shuts down");
}

} // namespace

int main() {
    const auto colors = prepareColors();
    if (!colors) {
        std::cerr << "FAIL: Bloom Neutral color objects could not be prepared\n";
        return 1;
    }
    testFlatField(*colors);
    testRamp(*colors);
    testColorBars(*colors);
    testSignalOrder(*colors);
    testWorkingRefused(*colors);
    testEqualsReference(*colors);
    testRegionAndIgnore(*colors);
    testDetail(*colors);
    testWrongInputs(*colors);
    testCancellation(*colors);
    failures += bloom::test_support::awaitFailureCount();
    if (failures != 0) {
        std::cerr << failures << " retained scope measure expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: retained scope measure\n";
    return 0;
}
