// #349 Histogram and Image Compare on the CPU reference: known answers read from real pixels (the
// counts of the bins, the heights of the plotted bars, the difference, the split line and the
// squares), the counting rules (range ends, alpha, straight and stored values, luminance, windows),
// tile independence against an independent count, domain refusal, the pixel budget, cancellation and
// the tables' cache identity.

#include "color_grade_image_effect.hpp"
#include "histogram_image_effect.hpp"
#include "image_effect.hpp"
#include "pixel_math_image_effect.hpp"
#include "pixel_math_test_support.hpp"

#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace bloom;
namespace detail = runtime::detail;
using runtime::HistogramRole;
using runtime::PixelMathOperation;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using Pixel = std::array<float, 4>;
using ImagePtr = std::shared_ptr<const render::Rgba32fImage>;

bool near(const float a, const float b, const float tolerance = 1e-5F) {
    return std::fabs(a - b) <= tolerance * std::max(1.0F, std::fabs(b));
}
bool same(const Pixel& a, const Pixel& b, const float tolerance = 1e-5F) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) &&
           near(a[2], b[2], tolerance) && near(a[3], b[3], tolerance);
}

render::Rgba32fImage make(const std::int64_t ox, const std::int64_t oy, const std::uint32_t w,
                          const std::uint32_t h,
                          const std::function<Pixel(std::int64_t, std::int64_t)>& fill,
                          const std::optional<render::ImageWindow>& display = std::nullopt) {
    const auto window = render::ImageWindow::create(ox, oy, w, h);
    auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), display ? *display : *window.value(), core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (auto y = oy; y < oy + static_cast<std::int64_t>(h); ++y)
        for (auto x = ox; x < ox + static_cast<std::int64_t>(w); ++x) {
            const auto p = fill(x, y);
            const auto stored = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!stored || builder.value()->write(x, y, *stored.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

Pixel at(const render::Rgba32fImage& picture, const std::int64_t x, const std::int64_t y) {
    Pixel value{};
    if (const auto pixel = picture.read(x, y))
        std::ranges::copy(pixel.value()->components(), value.begin());
    return value;
}

// ---- Image Compare ----

using Kernel = runtime::PixelMathKernel;

Kernel compareKernel(const std::int64_t mode, const double gain, const double position,
                     const double angle, const double cell) {
    return pixel_math_test::compare(0, mode, gain, position, angle, cell);
}

std::optional<detail::PreparedColorGrade> prepareCompare(const Kernel& kernel) {
    std::vector<double> values;
    for (std::size_t i = 0; i < 4; ++i)
        values.push_back(std::get<double>(kernel.values[i].source));
    return detail::preparePixelMathGrade(kernel, values);
}

ImagePtr compared(const Kernel& kernel, const render::Rgba32fImage& a,
                                 const render::Rgba32fImage* b) {
    const auto grade = prepareCompare(kernel);
    if (!grade)
        throw std::logic_error("the compare kernel prepares");
    const auto result = detail::applyPixelMath(*grade, a, b, 1U << 26U, {},
                                               runtime::OperationIndex::fromRaw(0), {});
    if (!result.image)
        throw std::logic_error("the compare pass applies");
    return result.image;
}

constexpr Pixel kA{0.75F, 0.25F, 0.0F, 1.0F};
constexpr Pixel kB{0.0F, 0.25F, 0.5F, 1.0F};

void testDifference() {
    const auto a = make(0, 0, 4, 2, [](auto, auto) { return Pixel{0.5F, 0.25F, 2.0F, 1.0F}; });
    const auto b = make(0, 0, 4, 2, [](auto, auto) { return Pixel{0.25F, 0.75F, 1.0F, 1.0F}; });
    auto result = compared(compareKernel(0, 2.0, 0.5, 0.0, 64.0), a, &b);
    expect(same(at(*result, 1, 1), {0.5F, 1.0F, 2.0F, 1.0F}), "Difference is |A - B| times the gain");
    result = compared(compareKernel(0, 1.0, 0.5, 0.0, 64.0), a, &a);
    expect(same(at(*result, 2, 0), {0.0F, 0.0F, 0.0F, 1.0F}), "equal images have no difference");
    // Alpha is the larger of the two; the stored colors differ by their stored values.
    const auto half = make(0, 0, 4, 2, [](auto, auto) { return Pixel{0.25F, 0.0F, 0.0F, 0.5F}; });
    const auto full = make(0, 0, 4, 2, [](auto, auto) { return Pixel{0.5F, 0.0F, 0.0F, 1.0F}; });
    result = compared(compareKernel(0, 1.0, 0.5, 0.0, 64.0), half, &full);
    expect(same(at(*result, 0, 0), {0.25F, 0.0F, 0.0F, 1.0F}), "Difference keeps the larger alpha");
    // A second image that covers less than the first is transparent black beyond its pixels.
    const auto small = make(1, 0, 2, 1, [](auto, auto) { return Pixel{0.5F, 0.25F, 2.0F, 1.0F}; });
    result = compared(compareKernel(0, 1.0, 0.5, 0.0, 64.0), a, &small);
    expect(same(at(*result, 1, 0), {0.0F, 0.0F, 0.0F, 1.0F}) &&
               same(at(*result, 0, 0), {0.5F, 0.25F, 2.0F, 1.0F}),
           "outside the second image it is transparent black");
    result = compared(compareKernel(0, 1.0, 0.5, 0.0, 64.0), a, nullptr);
    expect(same(at(*result, 0, 0), {0.5F, 0.25F, 2.0F, 1.0F}), "an absent second image is black");
    // A huge gain on a huge difference is not finite: it is 0 rather than an invalid pixel.
    const auto big = make(0, 0, 1, 1, [](auto, auto) { return Pixel{3.0e38F, 0.0F, 0.0F, 1.0F}; });
    const auto small2 = make(0, 0, 1, 1, [](auto, auto) { return Pixel{-3.0e38F, 0.0F, 0.0F, 1.0F}; });
    result = compared(compareKernel(0, 1.0, 0.5, 0.0, 64.0), big, &small2);
    expect(same(at(*result, 0, 0), {0.0F, 0.0F, 0.0F, 1.0F}), "an overflow is 0");
}

void testSplit() {
    const auto a = make(0, 0, 8, 4, [](auto, auto) { return kA; });
    const auto b = make(0, 0, 8, 4, [](auto, auto) { return kB; });
    const auto column = [&](const double position, const double angle, const std::int64_t x,
                            const std::int64_t y) {
        const auto result = compared(compareKernel(1, 1.0, position, angle, 64.0), a, &b);
        return at(*result, x, y);
    };
    expect(same(column(0.5, 0.0, 3, 1), kA) && same(column(0.5, 0.0, 4, 1), kB),
           "the middle split puts A left of the center line and B right of it");
    expect(same(column(0.25, 0.0, 1, 0), kA) && same(column(0.25, 0.0, 2, 0), kB),
           "a quarter split moves the line to two pixels");
    expect(same(column(0.0, 0.0, 0, 0), kB) && same(column(0.0, 0.0, 7, 3), kB),
           "position 0 shows B everywhere");
    expect(same(column(1.0, 0.0, 0, 0), kA) && same(column(1.0, 0.0, 7, 3), kA),
           "position 1 shows A everywhere");
    expect(same(column(0.5, 180.0, 3, 1), kB) && same(column(0.5, 180.0, 4, 1), kA),
           "a half turn swaps the sides");
    expect(same(column(0.5, 90.0, 5, 1), kA) && same(column(0.5, 90.0, 5, 2), kB),
           "a quarter turn splits top from bottom");
    expect(same(column(0.5, -90.0, 5, 1), kB) && same(column(0.5, -90.0, 5, 2), kA),
           "a quarter turn the other way splits bottom from top");
    // A diagonal line through the middle: the corners are on opposite sides.
    expect(same(column(0.5, 45.0, 0, 0), kA) && same(column(0.5, 45.0, 7, 3), kB),
           "a diagonal split separates opposite corners");
}

void testCheckerboard() {
    const auto a = make(0, 0, 8, 8, [](auto, auto) { return kA; });
    const auto b = make(0, 0, 8, 8, [](auto, auto) { return kB; });
    const auto result = compared(compareKernel(2, 1.0, 0.5, 0.0, 2.0), a, &b);
    expect(same(at(*result, 0, 0), kA) && same(at(*result, 1, 1), kA) &&
               same(at(*result, 2, 0), kB) && same(at(*result, 0, 2), kB) &&
               same(at(*result, 2, 2), kA) && same(at(*result, 3, 2), kA) &&
               same(at(*result, 7, 0), kB),
           "squares of two pixels alternate from the corner");
    const auto odd = compared(compareKernel(2, 1.0, 0.5, 0.0, 3.0), a, &b);
    expect(same(at(*odd, 2, 0), kA) && same(at(*odd, 3, 0), kB) && same(at(*odd, 3, 3), kA),
           "an odd cell size lays whole squares");
}

// The split and the squares are laid on the display window: the same pixel shows the same image
// whatever data window is rendered (a region of interest).
void testDisplayWindow() {
    const auto display = render::ImageWindow::create(0, 0, 8, 4);
    const auto fullA = make(0, 0, 8, 4, [](auto, auto) { return kA; });
    const auto fullB = make(0, 0, 8, 4, [](auto, auto) { return kB; });
    const auto clippedA = make(5, 1, 3, 2, [](auto, auto) { return kA; }, *display.value());
    const auto clippedB = make(5, 1, 3, 2, [](auto, auto) { return kB; }, *display.value());
    for (const auto& kernel : {compareKernel(1, 1.0, 0.7, 30.0, 64.0),
                               compareKernel(2, 1.0, 0.5, 0.0, 2.0)}) {
        const auto full = compared(kernel, fullA, &fullB);
        const auto clipped = compared(kernel, clippedA, &clippedB);
        bool equal = true;
        for (std::int64_t y = 1; y < 3; ++y)
            for (std::int64_t x = 5; x < 8; ++x)
                equal = equal && same(at(*full, x, y), at(*clipped, x, y), 0.0F);
        expect(equal, "a clipped data window shows the pixels the full window shows");
    }
}

void testCompareDomainAndIdentity() {
    expect(!prepareCompare(compareKernel(1, 1.0, 1.5, 0.0, 64.0)), "a position above 1 is refused");
    expect(!prepareCompare(compareKernel(1, -1.0, 0.5, 0.0, 64.0)), "a negative gain is refused");
    expect(!prepareCompare(compareKernel(1, 1.0, 0.5, 400.0, 64.0)), "an angle beyond a turn is refused");
    expect(!prepareCompare(compareKernel(2, 1.0, 0.5, 0.0, 0.5)), "a cell below a pixel is refused");
    expect(!prepareCompare(compareKernel(3, 1.0, 0.5, 0.0, 64.0)), "an unknown mode is refused");
    auto single = compareKernel(0, 1.0, 0.5, 0.0, 64.0);
    single.second = false;
    expect(!prepareCompare(single), "a compare without its second image is refused");
    const auto base = prepareCompare(compareKernel(1, 1.0, 0.5, 0.0, 64.0));
    expect(base && base->data == prepareCompare(compareKernel(1, 1.0, 0.5, 0.0, 64.0))->data,
           "equal settings give an equal table");
    for (const auto& changed : {compareKernel(0, 1.0, 0.5, 0.0, 64.0), compareKernel(1, 2.0, 0.5, 0.0, 64.0),
                                compareKernel(1, 1.0, 0.25, 0.0, 64.0),
                                compareKernel(1, 1.0, 0.5, 15.0, 64.0),
                                compareKernel(1, 1.0, 0.5, 0.0, 32.0)}) {
        const auto table = prepareCompare(changed);
        expect(table && table->data != base->data, "a changed setting changes the table");
    }
    expect(detail::pixelMathProgramData(*base, 8) == std::nullopt,
           "the GPU data needs the display window's place");
    const detail::PixelMathPlace place{2, 1, 8, 4};
    const auto program = detail::pixelMathProgramData(*base, 8, &place);
    expect(program && detail::validPixelMathProgramData(*program) &&
               program->size() == detail::kPixelMathCompareProgramFloats,
           "the GPU data carries the place and validates");
}

// ---- Histogram ----

runtime::HistogramKernel histogramKernel(const HistogramRole role, const std::int64_t channels,
                                         const bool straight, const bool logarithmic,
                                         const std::int64_t bins, const std::int64_t height,
                                         const double low, const double high) {
    using namespace pixel_math_test;
    runtime::HistogramKernel kernel;
    kernel.role = role;
    kernel.channels = channels;
    kernel.straight = straight;
    kernel.logarithmic = logarithmic;
    kernel.bins = bins;
    kernel.height = height;
    kernel.values = {scalar(1, low), scalar(2, high)};
    return kernel;
}

std::optional<detail::PreparedColorGrade> prepareHistogram(const runtime::HistogramKernel& kernel) {
    std::vector<double> values;
    if (kernel.role == HistogramRole::Bin)
        values = {std::get<double>(kernel.values[0].source), std::get<double>(kernel.values[1].source)};
    return detail::prepareHistogramGrade(kernel, values);
}

struct Chain final {
    runtime::HistogramKernel kernel;
    ImagePtr tiles;
    ImagePtr sums;
    ImagePtr plot;
};

ImagePtr pass(const runtime::HistogramKernel& kernel, const render::Rgba32fImage& in) {
    const auto grade = prepareHistogram(kernel);
    if (!grade)
        throw std::logic_error("the histogram kernel prepares");
    const auto result = detail::applyHistogram(*grade, &in, 1U << 26U, {},
                                               runtime::OperationIndex::fromRaw(0), {}, nullptr);
    if (!result.image)
        throw std::logic_error("the histogram pass applies");
    return result.image;
}

Chain chainOf(const render::Rgba32fImage& in, const std::int64_t channels, const bool straight,
              const bool logarithmic, const std::int64_t bins, const std::int64_t height,
              const double low, const double high) {
    Chain chain;
    chain.kernel = histogramKernel(HistogramRole::Bin, channels, straight, logarithmic, bins, height,
                                   low, high);
    chain.tiles = pass(chain.kernel, in);
    auto next = chain.kernel;
    next.role = HistogramRole::Sum;
    chain.sums = pass(next, *chain.tiles);
    next.role = HistogramRole::Plot;
    chain.plot = pass(next, *chain.sums);
    return chain;
}

// The counts of one counted channel, bin after bin, read from the Sum image.
std::vector<float> countsOf(const render::Rgba32fImage& sums, const std::int64_t channel,
                            const std::int64_t bins) {
    const auto groups = detail::histogramGroups(bins);
    const auto origin = sums.descriptor()->dataWindow();
    std::vector<float> counts;
    for (std::int64_t b = 0; b < bins; ++b) {
        const auto p = at(sums, origin.originX() + channel * groups + b / 3, origin.originY());
        counts.push_back(p[static_cast<std::size_t>(b % 3)]);
    }
    return counts;
}

float total(const std::vector<float>& counts) {
    float sum = 0.0F;
    for (const float count : counts)
        sum += count;
    return sum;
}

void testBinsAndSums() {
    // 12 by 10 pixels at a window origin that is not zero: left half 0.25, right half 0.75.
    const auto image = make(3, 2, 12, 10, [](const std::int64_t x, auto) {
        return x < 9 ? Pixel{0.25F, 0.25F, 0.25F, 1.0F} : Pixel{0.75F, 0.75F, 0.75F, 1.0F};
    });
    const auto chain = chainOf(image, document::kHistogramRed, true, false, 64, 128, 0.0, 1.0);
    const auto counts = countsOf(*chain.sums, 0, 64);
    expect(counts[16] == 60.0F && counts[48] == 60.0F && total(counts) == 120.0F,
           "0.25 and 0.75 fill bins 16 and 48 with 60 pixels each");
    const auto& window = chain.sums->descriptor()->dataWindow();
    expect(window.originX() == 3 && window.originY() == 2 && window.extent().height() == 1 &&
               static_cast<std::int64_t>(window.extent().width()) == detail::histogramGroups(64),
           "the sums are one row, a pixel per three bins, at the image's origin");
    const auto& plot = chain.plot->descriptor()->dataWindow();
    expect(plot.originX() == 3 && plot.originY() == 2 && plot.extent().width() == 64 &&
               plot.extent().height() == 128,
           "the plot is bins by height at the image's origin");
    // Three channels and luminance of a colored pixel.
    const auto colored = make(0, 0, 4, 4, [](auto, auto) { return Pixel{1.0F, 0.5F, 0.0F, 1.0F}; });
    const auto rgb = chainOf(colored, document::kHistogramRgbLuminance, true, false, 64, 64, 0.0, 1.0);
    expect(countsOf(*rgb.sums, 0, 64)[63] == 16.0F && countsOf(*rgb.sums, 1, 64)[32] == 16.0F &&
               countsOf(*rgb.sums, 2, 64)[0] == 16.0F,
           "red 1.0 is the last bin (the range end is in), green 0.5 bin 32, blue 0 the first");
    // Rec. 709 luminance 0.2126 + 0.7152 * 0.5 = 0.5702 -> bin 36.
    expect(countsOf(*rgb.sums, 3, 64)[36] == 16.0F, "the luminance of the color is counted");
}

void testRangeAlphaAndStored() {
    const auto image = make(0, 0, 4, 1, [](const std::int64_t x, auto) {
        const std::array<Pixel, 4> pixels{Pixel{2.0F, 2.0F, 2.0F, 1.0F}, Pixel{-1.0F, -1.0F, -1.0F, 1.0F},
                                          Pixel{0.1F, 0.1F, 0.1F, 0.5F}, Pixel{0.0F, 0.0F, 0.0F, 0.0F}};
        return pixels[static_cast<std::size_t>(x)];
    });
    // HDR and negative values outside the range are not counted anywhere.
    const auto stored = chainOf(image, document::kHistogramRed, false, false, 64, 64, 0.0, 1.0);
    const auto s = countsOf(*stored.sums, 0, 64);
    expect(total(s) == 2.0F && s[6] == 1.0F && s[0] == 1.0F,
           "stored values: 2 and -1 are outside the range; 0.1 is bin 6 and the empty pixel bin 0");
    // Straight: 0.1 stored at alpha 0.5 is 0.2 (bin 12); the transparent pixel has no color.
    const auto straight = chainOf(image, document::kHistogramRed, true, false, 64, 64, 0.0, 1.0);
    const auto t = countsOf(*straight.sums, 0, 64);
    expect(total(t) == 1.0F && t[12] == 1.0F,
           "straight color divides by alpha and a transparent pixel counts nowhere");
    // Alpha counts every pixel in the range: 1, 1, 0.5, 0.
    const auto alpha = chainOf(image, document::kHistogramAlpha, true, false, 64, 64, 0.0, 1.0);
    const auto a = countsOf(*alpha.sums, 0, 64);
    expect(total(a) == 4.0F && a[63] == 2.0F && a[32] == 1.0F && a[0] == 1.0F,
           "alpha counts every pixel, a transparent one in the first bin");
    // A range that ends where values do: a wider range moves the bins.
    const auto wide = chainOf(image, document::kHistogramRed, false, false, 64, 64, -2.0, 2.0);
    const auto w = countsOf(*wide.sums, 0, 64);
    expect(total(w) == 4.0F && w[63] == 1.0F && w[16] == 1.0F && w[33] == 1.0F && w[32] == 1.0F,
           "the range -2..2 counts 2.0 in the last bin, -1.0 in bin 16, 0.1 in 33 and 0 in 32");
    // An empty or reversed range counts nothing and is not a failure.
    const auto reversed = chainOf(image, document::kHistogramRed, false, false, 64, 64, 1.0, 0.0);
    expect(total(countsOf(*reversed.sums, 0, 64)) == 0.0F, "a reversed range counts nothing");
}

// The counts are independent of how the frame is cut into tiles: against an independent count.
void testTileIndependence() {
    for (const auto& [w, h] : {std::pair{1, 1}, std::pair{7, 5}, std::pair{33, 3}, std::pair{100, 80},
                               std::pair{257, 130}}) {
        const auto image = make(0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                [](const std::int64_t x, const std::int64_t y) {
                                    const auto bin = static_cast<float>((x * 7 + y * 13) % 64);
                                    const float v = (bin + 0.5F) / 64.0F;
                                    return Pixel{v, 1.0F - v, v * 0.5F, 1.0F};
                                });
        const auto chain = chainOf(image, document::kHistogramRgb, true, false, 64, 64, 0.0, 1.0);
        std::vector<float> expectedRed(64, 0.0F);
        std::vector<float> expectedGreen(64, 0.0F);
        for (std::int64_t y = 0; y < h; ++y)
            for (std::int64_t x = 0; x < w; ++x) {
                const auto bin = (x * 7 + y * 13) % 64;
                expectedRed[static_cast<std::size_t>(bin)] += 1.0F;
                expectedGreen[static_cast<std::size_t>(63 - bin)] += 1.0F;
            }
        expect(countsOf(*chain.sums, 0, 64) == expectedRed &&
                   countsOf(*chain.sums, 1, 64) == expectedGreen,
               "the counts equal an independent count for a " + std::to_string(w) + " by " +
                   std::to_string(h) + " image");
    }
}

void testPlot() {
    // 9 columns of 0.25 and 3 of 0.75, ten rows: 90 and 30 pixels.
    const auto image = make(0, 0, 12, 10, [](const std::int64_t x, auto) {
        return x < 9 ? Pixel{0.25F, 0.25F, 0.25F, 1.0F} : Pixel{0.75F, 0.75F, 0.75F, 1.0F};
    });
    constexpr std::int64_t height = 128;
    const auto linear = chainOf(image, document::kHistogramRed, true, false, 64, height, 0.0, 1.0);
    // Bin 16 is the tallest: the whole graph height. Bin 48 is a third of it.
    expect(near(at(*linear.plot, 16, 0)[0], 1.02F) && near(at(*linear.plot, 16, height - 1)[0], 1.02F),
           "the tallest bar fills the graph (the background plus red)");
    expect(near(at(*linear.plot, 20, 5)[0], 0.02F) && near(at(*linear.plot, 20, 5)[1], 0.02F),
           "an empty bin is the background");
    expect(near(at(*linear.plot, 16, 0)[1], 0.02F), "red bars add no green");
    // A third of 128 rows is 42 and two thirds: the row above 42 full rows is two thirds covered.
    const double fill = 128.0 * 30.0 / 90.0;
    const auto row = height - 1 - static_cast<std::int64_t>(fill);
    expect(near(at(*linear.plot, 48, row + 1)[0], 1.02F), "the third bar covers its full rows");
    expect(near(at(*linear.plot, 48, row)[0], static_cast<float>(0.02 + (fill - std::floor(fill))), 1e-4F),
           "the row above the full rows is partly covered");
    expect(near(at(*linear.plot, 48, row - 1)[0], 0.02F), "and above that is the background");
    const auto logarithmic = chainOf(image, document::kHistogramRed, true, true, 64, height, 0.0, 1.0);
    const double logFill = 128.0 * std::log2(31.0) / std::log2(91.0);
    const auto logRow = height - 1 - static_cast<std::int64_t>(logFill);
    expect(near(at(*logarithmic.plot, 16, 0)[0], 1.02F), "the tallest bar is the same on a log scale");
    expect(near(at(*logarithmic.plot, 48, logRow)[0], static_cast<float>(0.02 + (logFill - std::floor(logFill))), 1e-4F) &&
               near(at(*logarithmic.plot, 48, logRow + 1)[0], 1.02F),
           "a log scale draws log2(1 + count) against log2(1 + tallest)");
    // Three channels in their colors: a green bar adds only green.
    const auto green = make(0, 0, 4, 4, [](auto, auto) { return Pixel{0.0F, 0.5F, 0.0F, 1.0F}; });
    const auto rgb = chainOf(green, document::kHistogramRgb, true, false, 64, 64, 0.0, 1.0);
    expect(same(at(*rgb.plot, 32, 63), {0.02F, 1.02F, 0.02F, 1.0F}) &&
               same(at(*rgb.plot, 0, 63), {1.02F, 0.02F, 1.02F, 1.0F}),
           "bars are drawn in their channel's color, added over the background");
    // An empty frame (everything out of range) draws just the background.
    const auto out = chainOf(image, document::kHistogramRed, true, false, 64, 64, 2.0, 3.0);
    expect(same(at(*out.plot, 10, 10), {0.02F, 0.02F, 0.02F, 1.0F}), "no counts draw the background");
}

void testHistogramRefusals() {
    auto kernel = histogramKernel(HistogramRole::Bin, document::kHistogramRgb, true, false, 64, 64, 0.0, 1.0);
    const std::array<double, 2> nan{std::numeric_limits<double>::quiet_NaN(), 1.0};
    expect(!detail::prepareHistogramGrade(kernel, nan), "a NaN range is refused");
    const std::array<double, 2> huge{0.0, 2.0e6};
    expect(!detail::prepareHistogramGrade(kernel, huge), "a range beyond the domain is refused");
    const std::array<double, 1> one{0.0};
    expect(!detail::prepareHistogramGrade(kernel, one), "a Bin needs both range limits");
    auto sum = kernel;
    sum.role = HistogramRole::Sum;
    expect(!detail::prepareHistogramGrade(sum, std::array<double, 2>{0.0, 1.0}),
           "a Sum reads no range");
    auto bad = kernel;
    bad.channels = 99;
    expect(!prepareHistogram(bad), "an unknown channel choice is refused");
    const auto bin = prepareHistogram(kernel);
    auto changed = kernel;
    changed.values[1] = pixel_math_test::scalar(2, 2.0);
    auto channels = kernel;
    channels.channels = document::kHistogramLuminance;
    auto stored = kernel;
    stored.straight = false;
    auto fewer = kernel;
    fewer.bins = 128;
    for (const auto& other : {changed, channels, stored, fewer}) {
        const auto table = prepareHistogram(other);
        expect(bin && table && table->data != bin->data, "a changed setting changes the table");
    }
    expect(bin && bin->data == prepareHistogram(kernel)->data, "equal settings give an equal table");
    auto plot = kernel;
    plot.role = HistogramRole::Plot;
    plot.logarithmic = true;
    const auto plotTable = prepareHistogram(plot);
    auto linear = plot;
    linear.logarithmic = false;
    expect(plotTable && plotTable->data != prepareHistogram(linear)->data, "the scale is in the table");
    const auto program = detail::histogramProgramData(*bin, 12, 10);
    expect(program && detail::validHistogramProgramData(*program), "the GPU data validates");
    expect(!detail::histogramProgramData(*bin, 0, 10), "an empty source has no geometry");
    // No image to follow publishes none.
    const auto none = detail::applyHistogram(*bin, nullptr, 1U << 26U, {},
                                             runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(!none.image && none.empty, "no input publishes no image");
    const auto input = make(0, 0, 8, 8, [](auto, auto) { return Pixel{0.5F, 0.5F, 0.5F, 1.0F}; });
    const auto refused = detail::applyHistogram(*bin, &input, 4, {}, runtime::OperationIndex::fromRaw(0),
                                                {}, nullptr);
    expect(!refused.image && refused.diagnostic.has_value(), "a refused budget is an explicit error");
}

void testCancellation() {
    const auto input = make(0, 0, 256, 256, [](auto, auto) { return Pixel{0.5F, 0.5F, 0.5F, 1.0F}; });
    const auto grade = prepareHistogram(
        histogramKernel(HistogramRole::Bin, document::kHistogramRgb, true, false, 64, 64, 0.0, 1.0));
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(349)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Histogram cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result =
                detail::applyHistogram(*grade, &input, 1U << 26U, context.cancellation(),
                                       runtime::OperationIndex::fromRaw(0), {}, nullptr);
            cancelled = result.cancelled && !result.image;
            return runtime::TaskResult<void>::cancelled();
        });
    expect(submission.accepted(), "the cancellation task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> result;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(result && result->state() == runtime::TaskState::Cancelled && cancelled,
           "a cancelled pass publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

// BLOOM_HISTOGRAM_ARTIFACT_DIR: a graph of a ramp and of a two-level frame, and an Image Compare of
// two frames in each mode, as PPM pictures (display-clamped sRGB), with the values read at the
// pixels the pictures are checked against.
void writeCapture() {
    const char* directory = std::getenv("BLOOM_HISTOGRAM_ARTIFACT_DIR");
    if (directory == nullptr || *directory == '\0')
        return;
    const auto encode = [](const double linear) {
        const double v = std::clamp(linear, 0.0, 1.0);
        const double e = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        return static_cast<unsigned char>(std::lround(e * 255.0));
    };
    const auto save = [&](const render::Rgba32fImage& picture, const std::string& name,
                          const int scale) {
        const auto window = picture.descriptor()->dataWindow();
        const auto w = static_cast<int>(window.extent().width());
        const auto h = static_cast<int>(window.extent().height());
        std::vector<unsigned char> rgb(static_cast<std::size_t>(w * scale) * static_cast<std::size_t>(h * scale) * 3);
        for (int y = 0; y < h * scale; ++y)
            for (int x = 0; x < w * scale; ++x) {
                const auto p = at(picture, window.originX() + x / scale, window.originY() + y / scale);
                for (std::size_t c = 0; c < 3; ++c)
                    rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w * scale) + static_cast<std::size_t>(x)) * 3 + c] =
                        encode(static_cast<double>(p[c]));
            }
        std::ofstream out(std::string(directory) + "/" + name, std::ios::binary);
        out << "P6\n" << w * scale << ' ' << h * scale << "\n255\n";
        out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    };
    // 256 columns by 64 rows: red is the square of a ramp (a curve that piles up in the dark bins),
    // green a ramp to one half (a flat shelf over the lower half) and blue a constant (a spike).
    const auto ramp = make(0, 0, 256, 64, [](const std::int64_t x, auto) {
        const float v = static_cast<float>(x) / 255.0F;
        return Pixel{v * v, v * 0.5F, 0.25F, 1.0F};
    });
    const auto ramped = chainOf(ramp, document::kHistogramRgb, true, true, 256, 128, 0.0, 1.0);
    save(*ramped.plot, "histogram-ramp-rgb-log.ppm", 2);
    const auto reds = countsOf(*ramped.sums, 0, 256);
    const auto greens = countsOf(*ramped.sums, 1, 256);
    std::cout << "ramp red: bin0=" << reds[0] << " (expect 1024, x 0..15) bin255=" << reds[255]
              << " (expect 64, x 255) total=" << total(reds) << " (expect 16384)\n";
    std::cout << "ramp green: bin0=" << greens[0] << " (expect 128, x 0..1) bin127=" << greens[127]
              << " (expect 64, x 254) bin200=" << greens[200] << " (expect 0)\n";
    std::cout << "ramp blue 0.25: bin64=" << countsOf(*ramped.sums, 2, 256)[64]
              << " (expect 16384)\n";
    // A two-level frame, HDR: 0.2 for three fifths and 4.0 beyond the range's end.
    const auto levels = make(0, 0, 100, 50, [](const std::int64_t x, auto) {
        return x < 60 ? Pixel{0.2F, 0.2F, 0.2F, 1.0F} : (x < 90 ? Pixel{0.8F, 0.8F, 0.8F, 1.0F}
                                                                : Pixel{4.0F, 4.0F, 4.0F, 1.0F});
    });
    const auto leveled = chainOf(levels, document::kHistogramLuminance, true, false, 64, 128, 0.0, 1.0);
    save(*leveled.plot, "histogram-levels-luma.ppm", 4);
    const auto lc = countsOf(*leveled.sums, 0, 64);
    std::cout << "levels luma: bin12=" << lc[12] << " (expect 3000) bin51=" << lc[51]
              << " (expect 1500) total=" << total(lc) << " (expect 4500, the 4.0 pixels are out of range)\n";
    const auto logged = chainOf(levels, document::kHistogramLuminance, true, true, 64, 128, 0.0, 1.0);
    save(*logged.plot, "histogram-levels-luma-log.ppm", 4);
    // Image Compare of a frame and a copy with an offset and a tint.
    const auto picture = [](const std::int64_t shift, const float tint) {
        return make(0, 0, 128, 96, [shift, tint](const std::int64_t x, const std::int64_t y) {
            const float v = static_cast<float>(((x + shift) / 16 + y / 16) % 2) * 0.5F + 0.1F;
            return Pixel{v + tint, v, v * 0.6F, 1.0F};
        });
    };
    const auto left = picture(0, 0.0F);
    const auto right = picture(4, 0.1F);
    for (const auto& [mode, name, kernel] :
         {std::tuple{0, "compare-difference.ppm", compareKernel(0, 4.0, 0.5, 0.0, 32.0)},
          std::tuple{1, "compare-split.ppm", compareKernel(1, 1.0, 0.4, 20.0, 32.0)},
          std::tuple{2, "compare-checkerboard.ppm", compareKernel(2, 1.0, 0.5, 0.0, 24.0)}}) {
        static_cast<void>(mode);
        const auto result = compared(kernel, left, &right);
        save(*result, name, 3);
    }
    const auto diff = compared(compareKernel(0, 4.0, 0.5, 0.0, 32.0), left, &right);
    const auto d = at(*diff, 8, 8);
    std::cout << "difference at (8,8) gain 4: " << d[0] << ' ' << d[1] << ' ' << d[2]
              << " (A-B red 0.1+0.5*... see test)\n";
    save(left, "compare-a.ppm", 3);
    save(right, "compare-b.ppm", 3);
}

} // namespace

int main() {
    try {
        testDifference();
        testSplit();
        testCheckerboard();
        testDisplayWindow();
        testCompareDomainAndIdentity();
        testBinsAndSums();
        testRangeAlphaAndStored();
        testTileIndependence();
        testPlot();
        testHistogramRefusals();
        testCancellation();
        writeCapture();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "histogram and compare tests passed\n";
    return 0;
}
