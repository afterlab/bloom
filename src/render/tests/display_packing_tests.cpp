// #733: the bit-exact rules of the packed display pixel (render/display_pixel_packing.hpp): the
// 10-bit pack rule and its boundaries, the round trip of every code through the scope code rule,
// the layout rule, the Display signal of both layouts, the 8-bit presentation, the frame counters
// and the row packers of the built-in reference display.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/image.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace render = bloom::render;
using render::DisplayFrameCounters;
using render::DisplayPixelLayout;
using render::Rgba8;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// The scope code rule of runtime/scope_measure.cpp for an axis maximum of 1, written out again.
[[nodiscard]] long scopeCode(const float value) {
    const float coded = value * 1023.0F;
    const float rounded = coded + 0.5F;
    return static_cast<long>(std::floor(rounded));
}

void testAllCodesRoundTrip() {
    bool packed = true;
    bool counted = true;
    bool fields = true;
    for (std::uint32_t code = 0; code <= 1023U; ++code) {
        const float value = render::displayCode10Value(code);
        packed = packed && render::displayCode10(value) == code;
        counted = counted && scopeCode(value) == static_cast<long>(code);
        // Every channel holds every code independently of its neighbors.
        const auto cell = render::rgb10A2Cell({code, 1023U - code, (code * 7U) % 1024U, 3U});
        const auto back = render::rgb10A2Codes(cell);
        fields = fields && back.red == code && back.green == 1023U - code &&
                 back.blue == (code * 7U) % 1024U && back.alpha == 3U;
        const auto signal = render::displaySignalCodes(DisplayPixelLayout::Rgb10A2, cell);
        fields = fields && signal[0] == code && signal[1] == 1023U - code;
    }
    expect(packed, "every 10-bit code packs back to itself from its float value");
    expect(counted, "the scope code rule counts every stored 10-bit code as that code");
    expect(fields, "the three 10-bit fields of a word are independent and read back exactly");
    expect(render::displayWord(render::displayCell(0xC00FFC00U)) == 0xC00FFC00U &&
               render::displayCell(0x04030201U) == Rgba8{1, 2, 3, 4},
           "a word and its four bytes convert both ways, least significant byte first");
    expect(sizeof(Rgba8) == 4 && render::rgb10A2Cell({1023U, 1023U, 1023U, 3U}) ==
                                     Rgba8{255, 255, 255, 255},
           "a 10-bit pixel costs the four bytes of an 8-bit pixel");
}

void testPackBoundaries() {
    bool boundaries = true;
    for (std::uint32_t code = 1; code <= 1023U; ++code) {
        // The exact boundary between code - 1 and code is (code - 0.5) / 1023. The nearest
        // floats around it must pack to one of the two codes, never further away, and in order.
        const float boundary = (static_cast<float>(code) - 0.5F) / 1023.0F;
        const float below = std::nextafter(boundary, 0.0F);
        const float above = std::nextafter(boundary, 2.0F);
        const auto low = render::displayCode10(below);
        const auto middle = render::displayCode10(boundary);
        const auto high = render::displayCode10(above);
        boundaries = boundaries && low <= middle && middle <= high && low + 1U >= code &&
                     high <= code && render::displayCode10(below - 0.0002F) == code - 1U &&
                     render::displayCode10(above + 0.0002F) == code;
    }
    expect(boundaries, "each code starts at (code - 0.5) / 1023");
    expect(render::displayCode10(-0.25F) == 0 && render::displayCode10(0.0F) == 0 &&
               render::displayCode10(-0.0F) == 0 && render::displayCode10(1.0F) == 1023U &&
               render::displayCode10(7.5F) == 1023U &&
               render::displayCode10(std::numeric_limits<float>::infinity()) == 1023U &&
               render::displayCode10(-std::numeric_limits<float>::infinity()) == 0 &&
               render::displayCode10(std::numeric_limits<float>::quiet_NaN()) == 0,
           "values outside 0..1 clamp and a value that is not a number packs to 0");
    expect(render::displayCode8(0.5F) == 128 && render::displayCode8(-1.0F) == 0 &&
               render::displayCode8(2.0F) == 255 &&
               render::displayCode8(std::numeric_limits<float>::quiet_NaN()) == 0,
           "the 8-bit rule is the long-standing straight-rgba8 rule");
}

void testLayoutRule() {
    expect(render::displayAlphaAllowsRgb10A2(0.0F) && render::displayAlphaAllowsRgb10A2(1.0F) &&
               render::displayAlphaAllowsRgb10A2(0.999F),
           "transparent and opaque pixels fit the 10-bit layout");
    expect(!render::displayAlphaAllowsRgb10A2(0.5F) && !render::displayAlphaAllowsRgb10A2(0.99F) &&
               !render::displayAlphaAllowsRgb10A2(0.001F) &&
               !render::displayAlphaAllowsRgb10A2(std::numeric_limits<float>::min()),
           "a partly transparent pixel, however faint, keeps the frame at 8 bits");
    expect(render::packRgb10A2(0.25F, 0.5F, 1.0F, 0.0F) == Rgba8{0, 0, 0, 0},
           "a transparent pixel is the word 0");
    const auto opaque = render::rgb10A2Codes(render::packRgb10A2(0.25F, 0.5F, 2.0F, 1.0F));
    expect(opaque == render::Rgb10A2Codes{256U, 512U, 1023U, 3U},
           "an opaque pixel holds its three 10-bit codes and alpha 3");
    expect(render::packStraight8888(0.25F, 0.5F, 2.0F, 0.5F) == Rgba8{64, 128, 255, 128},
           "the 8-bit layout keeps straight color and an 8-bit alpha");
    expect(render::displayPixelLayoutId(DisplayPixelLayout::Straight8888) == "straight-rgba8" &&
               render::displayPixelLayoutId(DisplayPixelLayout::Rgb10A2) == "preview-rgb10-a2",
           "each layout has its own identity string and the 8-bit one is unchanged");
}

void testSignalAndPresentation() {
    using Layout = DisplayPixelLayout;
    expect(render::displaySignalCodes(Layout::Straight8888, Rgba8{255, 128, 0, 255}) ==
                   std::array<std::uint32_t, 3>{1023U, 514U, 0U} &&
               render::displaySignalCodes(Layout::Straight8888, Rgba8{255, 255, 255, 128}) ==
                   std::array<std::uint32_t, 3>{514U, 514U, 514U} &&
               render::displaySignalCodes(Layout::Straight8888, Rgba8{255, 255, 255, 0}) ==
                   std::array<std::uint32_t, 3>{0U, 0U, 0U},
           "the 8-bit layout's Display signal is color times alpha on the 10-bit axis");
    bool rounded = true;
    for (std::uint32_t color = 0; color <= 255U; ++color) {
        for (std::uint32_t alpha = 0; alpha <= 255U; ++alpha) {
            const auto code = render::displaySignalCodes(
                Layout::Straight8888, Rgba8{static_cast<std::uint8_t>(color), 0, 0,
                                            static_cast<std::uint8_t>(alpha)})[0];
            const double exact = static_cast<double>(color * alpha) * 1023.0 / 65025.0;
            rounded = rounded && std::abs(static_cast<double>(code) - exact) <= 0.5;
        }
    }
    expect(rounded, "the 8-bit layout's signal code is the nearest code, for every color and alpha");
    expect(render::displaySignalAlpha(Layout::Rgb10A2, render::rgb10A2Cell({5U, 6U, 7U, 3U})) ==
                   1.0F &&
               render::displaySignalAlpha(Layout::Rgb10A2, Rgba8{0, 0, 0, 0}) == 0.0F &&
               render::displaySignalAlpha(Layout::Straight8888, Rgba8{9, 9, 9, 255}) == 1.0F &&
               render::displaySignalAlpha(Layout::Straight8888, Rgba8{9, 9, 9, 0}) == 0.0F,
           "the signal's alpha is 1 for an opaque and 0 for a transparent pixel in both layouts");

    bool nearest = true;
    bool monotonic = true;
    for (std::uint32_t code = 0; code <= 1023U; ++code) {
        const auto shown = render::presentedCode8(code);
        const double exact = static_cast<double>(code) * 255.0 / 1023.0;
        nearest = nearest && std::abs(static_cast<double>(shown) - exact) <= 0.5;
        monotonic = monotonic && (code == 0 || shown >= render::presentedCode8(code - 1U));
    }
    expect(nearest && monotonic && render::presentedCode8(0) == 0 &&
               render::presentedCode8(1023U) == 255,
           "an 8-bit output shows the nearest 8-bit code of every 10-bit code");
    bool everyByte = true;
    for (std::uint32_t byte = 0; byte <= 255U; ++byte) {
        // A value on an 8-bit step survives 10-bit storage and 8-bit presentation unchanged.
        const float value = static_cast<float>(byte) / 255.0F;
        everyByte = everyByte && render::presentedCode8(render::displayCode10(value)) == byte;
    }
    expect(everyByte, "a picture whose values are 8-bit steps is shown as the same 8-bit codes");
    expect(render::presentedRgba8(Layout::Rgb10A2, render::rgb10A2Cell({1023U, 512U, 0U, 3U})) ==
                   Rgba8{255, 128, 0, 255} &&
               render::presentedRgba8(Layout::Rgb10A2, Rgba8{0, 0, 0, 0}) == Rgba8{0, 0, 0, 0} &&
               render::presentedRgba8(Layout::Straight8888, Rgba8{1, 2, 3, 4}) == Rgba8{1, 2, 3, 4},
           "the shown 8-bit pixel of either layout");
}

void testCounters() {
    DisplayFrameCounters counters;
    const std::optional<render::DisplayLumaCoefficients> rec709{{0.2126F, 0.7152F, 0.0722F}};
    render::countDisplayPixel(counters, 0.5F, 0.5F, 0.5F, 1.0F, rec709);    // inside
    render::countDisplayPixel(counters, 1.25F, 0.5F, -0.1F, 1.0F, rec709);  // red over, blue under
    render::countDisplayPixel(counters, 1.25F, 1.25F, 1.25F, 1.0F, rec709); // all over, luma too
    render::countDisplayPixel(counters, 1.25F, 1.25F, 1.25F, 0.5F, rec709); // over black: inside
    render::countDisplayPixel(counters, -0.5F, -0.5F, -0.5F, 1.0F, rec709); // all under
    render::countDisplayPixel(counters, 9.0F, 9.0F, 9.0F, 0.0F, rec709);    // transparent: inside
    render::countDisplayPixel(counters, std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F,
                              rec709);
    render::countDisplayPixel(counters, 0.0F, std::numeric_limits<float>::infinity(), 0.0F, 1.0F,
                              rec709);
    expect(counters.over == std::array<std::uint32_t, 4>{2U, 1U, 1U, 1U} &&
               counters.under == std::array<std::uint32_t, 4>{1U, 1U, 2U, 1U} &&
               counters.notFinite == 2U && counters.lumaCounted(),
           "under and over are counted per channel of the signal over black, and luma with them");
    // The edge of the axis is the scope's: a value that still rounds to code 0 or 1023 is inside.
    DisplayFrameCounters edge;
    render::countDisplayPixel(edge, 1.0F + 0.4F / 1023.0F, -0.4F / 1023.0F, 0.0F, 1.0F, rec709);
    render::countDisplayPixel(edge, 1.0F + 0.6F / 1023.0F, -0.6F / 1023.0F, 0.0F, 1.0F, rec709);
    expect(edge.over[0] == 1U && edge.under[1] == 1U,
           "a value within half a code of the axis is inside it, beyond that it is under or over");

    DisplayFrameCounters withoutLuma;
    withoutLuma.markLumaAbsent();
    render::countDisplayPixel(withoutLuma, 2.0F, 2.0F, 2.0F, 1.0F, std::nullopt);
    expect(!withoutLuma.lumaCounted() && withoutLuma.over[0] == 1U &&
               withoutLuma.over[3] == render::kDisplayCounterAbsent,
           "without the display's coefficients the luma pair is marked absent");
    DisplayFrameCounters sum = counters;
    sum.add(counters);
    expect(sum.over[0] == 4U && sum.under[2] == 4U && sum.notFinite == 4U && sum.lumaCounted(),
           "the counters of two parts of a frame add up");
    sum.add(withoutLuma);
    expect(sum.over[0] == 5U && !sum.lumaCounted(), "a part without luma leaves the frame without");
    DisplayFrameCounters full;
    full.notFinite = render::kDisplayCounterAbsent - 2U;
    full.add(counters);
    expect(full.notFinite == render::kDisplayCounterAbsent - 1U,
           "a count stops one short of the absent marker");
    expect(sizeof(DisplayFrameCounters) == 36, "the counters a frame carries are 36 bytes");
}

// The value of a result that must have one.
template <typename T> [[nodiscard]] T must(render::ImageResult<T> result) {
    if (!result) {
        std::abort();
    }
    return std::move(*result.value());
}

[[nodiscard]] render::Rgba32fImage image(const std::uint32_t width,
                                         const std::vector<std::array<float, 4>>& straight) {
    const auto window = must(render::ImageWindow::create(0, 0, width, straight.size() / width));
    const auto descriptor = must(render::Rgba32fImageDescriptor::create(
        window, window, bloom::core::PixelAspectRatio::square()));
    auto builder = must(
        render::Rgba32fImageBuilder::create(descriptor, descriptor.layout().pixelStorageBytes));
    for (std::uint32_t y = 0; y < window.extent().height(); ++y) {
        const auto row = must(builder.row(y));
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto& value = straight[static_cast<std::size_t>(y) * width + x];
            row[x] = must(render::Rgba32f::fromPremultiplied(
                value[0] * value[3], value[1] * value[3], value[2] * value[3], value[3]));
        }
    }
    return must(std::move(builder).freeze());
}

void testReferenceRows() {
    // Scene-linear values: 0, the sRGB toe, mid gray, 1 and beyond 1.
    const auto opaque =
        image(4, {{0.0F, 0.0F, 0.0F, 1.0F}, {0.001F, 0.18F, 0.5F, 1.0F},
                  {1.0F, 2.0F, 0.25F, 1.0F}, {0.0F, 0.0F, 0.0F, 0.0F}});
    const auto view = must(opaque.view());
    expect(render::rgba32fAllowsRgb10A2(view), "an opaque and transparent picture fits 10 bits");
    std::vector<Rgba8> wide(4);
    std::vector<Rgba8> narrow(4);
    const auto window = view.descriptor()->displayWindow();
    expect(!render::mapLinearRec709SceneToSrgbRgb10A2Row(view, window, 0, wide).has_value() &&
               !render::mapLinearRec709SceneToSrgbRow(view, window, 0, narrow).has_value(),
           "both row packers accept the row");
    const auto srgb = [](const double linear) {
        return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    };
    const auto second = render::rgb10A2Codes(wide[1]);
    expect(render::rgb10A2Codes(wide[0]) == render::Rgb10A2Codes{0U, 0U, 0U, 3U} &&
               second.red == static_cast<std::uint32_t>(std::lround(srgb(0.001) * 1023.0)) &&
               second.green == static_cast<std::uint32_t>(std::lround(srgb(0.18) * 1023.0)) &&
               second.blue == static_cast<std::uint32_t>(std::lround(srgb(0.5) * 1023.0)) &&
               second.alpha == 3U,
           "the reference display packs the sRGB encoding to 10-bit codes");
    expect(render::rgb10A2Codes(wide[2]).red == 1023U && render::rgb10A2Codes(wide[2]).green == 1023U &&
               wide[3] == Rgba8{0, 0, 0, 0},
           "the reference display clips at 1 and keeps a transparent pixel as the word 0");
    bool agrees = true;
    for (std::size_t index = 0; index < wide.size(); ++index) {
        const auto shown = render::presentedRgba8(DisplayPixelLayout::Rgb10A2, wide[index]);
        const auto reference = narrow[index];
        // Rounding to 10 bits and then to 8 can land one 8-bit code from rounding to 8 at once.
        agrees = agrees && std::abs(shown.red - reference.red) <= 1 &&
                 std::abs(shown.green - reference.green) <= 1 &&
                 std::abs(shown.blue - reference.blue) <= 1 && shown.alpha == reference.alpha;
    }
    expect(agrees, "the 10-bit row shown at 8 bits is within one code of the 8-bit row");

    const auto soft = image(2, {{1.0F, 1.0F, 1.0F, 0.5F}, {1.0F, 1.0F, 1.0F, 1.0F}});
    expect(!render::rgba32fAllowsRgb10A2(must(soft.view())),
           "a picture with a partly transparent pixel does not fit 10 bits");

    DisplayFrameCounters counters;
    render::countLinearRec709SceneDisplayRow(view, window, 0, counters);
    expect(counters.over == std::array<std::uint32_t, 4>{0U, 1U, 0U, 1U} &&
               counters.under == std::array<std::uint32_t, 4>{0U, 0U, 0U, 0U} &&
               counters.notFinite == 0U,
           "the reference display counts the values its encoding leaves above 1");
}

void testDescriptorCarriesTheLayout() {
    const auto window = must(render::ImageWindow::create(0, 0, 2, 1));
    const auto square = bloom::core::PixelAspectRatio::square();
    const auto narrow = render::ReferenceDisplayBufferDescriptor::create(window, square);
    const auto wide = render::ReferenceDisplayBufferDescriptor::create(
        window, square, DisplayPixelLayout::Rgb10A2);
    expect(narrow && wide &&
               narrow.value()->pixelLayout() == DisplayPixelLayout::Straight8888 &&
               wide.value()->pixelLayout() == DisplayPixelLayout::Rgb10A2 &&
               !(*narrow.value() == *wide.value()) &&
               narrow.value()->layout() == wide.value()->layout(),
           "a display buffer's descriptor names its pixel layout, and both cost the same bytes");
    const std::vector<Rgba8> pixels{render::rgb10A2Cell({1U, 2U, 3U, 3U}), Rgba8{0, 0, 0, 0}};
    const auto buffer = render::PreparedReferenceDisplayBuffer::create(
        *wide.value(), pixels, std::numeric_limits<std::size_t>::max());
    expect(buffer && buffer.value()->descriptor()->pixelLayout() == DisplayPixelLayout::Rgb10A2 &&
               buffer.value()->pixelStorageByteCost() == 8U &&
               must(buffer.value()->view()).descriptor()->pixelLayout() ==
                   DisplayPixelLayout::Rgb10A2,
           "a prepared buffer and its view keep the layout with the pixels");
}

} // namespace

int main() {
    testAllCodesRoundTrip();
    testPackBoundaries();
    testLayoutRule();
    testSignalAndPresentation();
    testCounters();
    testReferenceRows();
    testDescriptorCarriesTheLayout();
    if (failures != 0) {
        std::cerr << failures << " display packing expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: display packing\n";
    return EXIT_SUCCESS;
}
