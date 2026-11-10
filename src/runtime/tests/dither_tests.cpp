// #527 Dither on the CPU reference: every pixel against an independent double-precision reading of
// the documented rule (the Bayer matrix by its bit formula, each channel's placement, the threshold,
// the sRGB encoding and the stored value), both patterns, every Bayer side and bit depth, monochrome,
// premultiplication, alpha 0, HDR and negative values; identity at amplitude 0; the same seed giving
// the same pixels and another seed others; Held identical and Every Frame uncorrelated from frame to
// frame; a region of interest and a half-scale proxy reading the full-resolution pattern; the table's
// identity; the purpose (a gradient and a glow quantized to 8 bit band less with the dither, by the
// length of their constant runs and the low-frequency error); the program text, the pixel budget and
// cancellation; and the evaluator's frames, region and proxy.

#include "color_grade_image_effect.hpp"
#include "dither_image_effect.hpp"
#include "dither_test_support.hpp"
#include "image_effect.hpp"

#include <bloom/core/dither_pattern.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The reference is binary64 and the kernel binary32: comparing the two promotes and narrows on purpose.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-float-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#endif

namespace {

using namespace bloom;
namespace detail = runtime::detail;
using dither_test::DitherSettings;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using Pixel = std::array<float, 4>;

std::optional<detail::PreparedColorGrade> prepare(const DitherSettings& settings,
                                                  const double frame = 0.0) {
    return detail::prepareDitherGrade(dither_test::ditherKernel(settings, 1),
                                      dither_test::ditherOperands(settings, frame));
}

render::Rgba32fImage image(const std::int64_t originX, const std::int64_t originY,
                           const std::uint32_t width, const std::uint32_t height,
                           const std::function<Pixel(std::int64_t, std::int64_t)>& fill) {
    const auto window = render::ImageWindow::create(originX, originY, width, height);
    auto descriptor = render::Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 28U);
    for (std::int64_t y = originY; y < originY + static_cast<std::int64_t>(height); ++y)
        for (std::int64_t x = originX; x < originX + static_cast<std::int64_t>(width); ++x) {
            const auto p = fill(x, y);
            const auto stored = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!stored || builder.value()->write(x, y, *stored.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

Pixel at(const render::Rgba32fImage& image, const std::int64_t x, const std::int64_t y) {
    Pixel value{};
    if (const auto pixel = image.read(x, y))
        std::ranges::copy(pixel.value()->components(), value.begin());
    return value;
}

using Image = std::shared_ptr<const render::Rgba32fImage>;
Pixel at(const Image& image, const std::int64_t x, const std::int64_t y) { return at(*image, x, y); }

Image run(const detail::PreparedColorGrade& grade, const render::Rgba32fImage& input,
          const double scaleX = 1.0, const double scaleY = 1.0) {
    const auto result = detail::applyDither(grade, input, scaleX, scaleY, 1U << 28U, {},
                                            runtime::OperationIndex::fromRaw(0), {}, nullptr);
    if (!result.image)
        throw std::logic_error("the dither applies");
    return result.image;
}

// --- The independent reading ---------------------------------------------------------------------

// The Bayer matrix by its bit formula: each bit of x and y, from the highest, adds Q = 2 (x ^ y) + y
// times 4 to the power of its place from the lowest.
int bayerRank(const int x, const int y, const int side) {
    int bits = 0;
    while ((1 << bits) < side)
        ++bits;
    int rank = 0;
    for (int b = 0; b < bits; ++b) {
        const int xb = (x >> b) & 1;
        const int yb = (y >> b) & 1;
        rank += (2 * (xb ^ yb) + yb) << (2 * (bits - 1 - b));
    }
    return rank;
}

double encode(const double v) {
    const double a = std::abs(v);
    const double e = a <= 0.0031308 ? 12.92 * a : 1.055 * std::pow(a, 1.0 / 2.4) - 0.055;
    return v < 0.0 ? -e : e;
}
double decode(const double v) {
    const double a = std::abs(v);
    const double d = a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
    return v < 0.0 ? -d : d;
}

// The centred threshold of the full-resolution pixel (x, y) for a channel.
double threshold(const DitherSettings& s, const std::int64_t x, const std::int64_t y,
                 const int channel, const std::int64_t frame) {
    const int side = static_cast<int>(s.side);
    const auto o = core::dither::offset(s.seed, s.everyFrame ? frame : 0, s.monochrome ? 0 : channel,
                                        side);
    const auto wrap = [side](const std::int64_t v) {
        return static_cast<int>(((v % side) + side) % side);
    };
    const int cx = wrap(x + o.x);
    const int cy = wrap(y + o.y);
    const int rank = s.blueNoise
                         ? core::dither::blueNoiseTable()[static_cast<std::size_t>(cy * side + cx)]
                         : bayerRank(cx, cy, side);
    const int cells = side * side;
    const int rotated = (rank + static_cast<int>(o.rank)) % cells;
    return (rotated + 0.5) / cells - 0.5;
}

// The documented pixel: `full` the full-resolution position.
std::array<double, 4> reference(const DitherSettings& s, const Pixel p, const std::int64_t fx,
                                const std::int64_t fy, const std::int64_t frame) {
    std::array<double, 4> out{p[0], p[1], p[2], p[3]};
    const double a = p[3];
    if (a == 0.0)
        return out;
    const double lsb = 1.0 / (std::ldexp(1.0, static_cast<int>(s.bits)) - 1.0);
    for (int c = 0; c < 3; ++c) {
        const double amount = threshold(s, fx, fy, c, frame) * s.amplitude * lsb;
        if (s.stored) {
            out[static_cast<std::size_t>(c)] = p[static_cast<std::size_t>(c)] + amount * a;
        } else {
            const double straight = p[static_cast<std::size_t>(c)] / a;
            if (std::abs(straight) > 65504.0)
                continue;
            out[static_cast<std::size_t>(c)] = decode(encode(straight) + amount) * a;
        }
    }
    return out;
}

// The binary32 kernel against binary64: the stored path is one product and one sum; the display
// path rounds through the field kernel's log2 and exp2 polynomials (a few parts in 10^7 of the value).
bool near(const float actual, const double expected, const bool stored) {
    const double tolerance = stored ? 2.0e-6 : 6.0e-6;
    return std::abs(static_cast<double>(actual) - expected) <=
           tolerance * std::max(1.0e-3, std::abs(expected)) + 1.0e-9;
}

// A picture with every case: a gradient across both transfer segments into HDR, negative values,
// partial and zero alpha.
Pixel varied(const std::int64_t x, const std::int64_t y) {
    const float u = static_cast<float>(x) / 37.0F;
    const float v = static_cast<float>(y) / 23.0F;
    float a = (x + y) % 7 == 0 ? 0.5F : 1.0F;
    if ((x * 3 + y) % 29 == 0)
        a = 0.0F;
    const float r = 0.0005F + u * u * 2.5F;
    const float g = v - 0.2F;
    const float b = (x % 5 == 0) ? 120.0F : 0.03F * u;
    return {r * a, g * a, b * a, a};
}

void testReference() {
    std::vector<DitherSettings> cases;
    for (const std::int64_t side : {2, 4, 8, 16})
        cases.push_back(dither_test::ordered(side));
    cases.push_back({});
    for (const auto bits : {10, 12, 16}) {
        DitherSettings s;
        s.bits = bits;
        s.amplitude = 2.5;
        cases.push_back(s);
    }
    DitherSettings stored = dither_test::everyPath();
    stored.stored = true;
    cases.push_back(stored);
    DitherSettings mono = dither_test::everyPath();
    mono.monochrome = true;
    cases.push_back(mono);
    DitherSettings orderedStored = dither_test::ordered(8);
    orderedStored.stored = true;
    orderedStored.seed = 7;
    orderedStored.amplitude = 3.0;
    cases.push_back(orderedStored);
    DitherSettings held = dither_test::everyPath();
    held.everyFrame = false;
    cases.push_back(held);
    const auto input = image(-3, 5, 40, 28, varied);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& s = cases[i];
        const std::int64_t frame = 3;
        const auto grade = prepare(s, static_cast<double>(frame));
        expect(grade.has_value(), "the settings prepare (case " + std::to_string(i) + ")");
        if (!grade)
            continue;
        const auto output = run(*grade, input);
        bool same = true;
        bool alpha = true;
        int moved = 0;
        for (std::int64_t y = 5; y < 33; ++y)
            for (std::int64_t x = -3; x < 37; ++x) {
                const auto p = at(input, x, y);
                const auto q = at(output, x, y);
                const auto want = reference(s, p, x, y, frame);
                for (std::size_t c = 0; c < 3; ++c) {
                    same = same && near(q[c], want[c], s.stored);
                    moved += q[c] != p[c] ? 1 : 0;
                }
                alpha = alpha && q[3] == p[3];
                if (p[3] == 0.0F)
                    alpha = alpha && q == p;
            }
        expect(same, "every pixel follows the documented rule (case " + std::to_string(i) + ")");
        expect(alpha, "alpha never changes and alpha 0 is left alone (case " + std::to_string(i) + ")");
        expect(moved > 1000, "the dither moves the picture (case " + std::to_string(i) + ")");
    }
    // Beyond the largest half float the display encoding passes the value unchanged.
    const auto bright = image(0, 0, 4, 4, [](auto, auto) { return Pixel{7.0e4F, -7.0e4F, 1.0F, 1.0F}; });
    const auto grade = prepare(dither_test::everyPath());
    const auto out = run(*grade, bright);
    expect(at(out, 1, 1)[0] == 7.0e4F && at(out, 1, 1)[1] == -7.0e4F && at(out, 1, 1)[2] != 1.0F,
           "a straight value beyond 65504 passes unchanged in the display encoding");
}

void testIdentityAndDomain() {
    DitherSettings zero;
    zero.amplitude = 0.0;
    const auto grade = prepare(zero);
    expect(grade && grade->identity, "amplitude 0 is the identity");
    const auto input = image(0, 0, 16, 16, varied);
    if (grade) {
        const auto output = run(*grade, input);
        bool same = true;
        for (std::int64_t y = 0; y < 16; ++y)
            for (std::int64_t x = 0; x < 16; ++x)
                same = same && at(output, x, y) == at(input, x, y);
        expect(same, "amplitude 0 leaves every pixel exactly as it was");
    }
    expect(!prepare(dither_test::everyPath())->identity, "a dither is not the identity");
    DitherSettings bad = dither_test::ordered(32);
    expect(!prepare(bad), "a Bayer side outside 2, 4, 8 and 16 is refused");
    bad = {};
    bad.side = 32;
    expect(!prepare(bad), "a blue-noise side other than 64 is refused");
    bad = {};
    bad.bits = 9;
    expect(!prepare(bad), "a bit depth outside 8, 10, 12 and 16 is refused");
    bad = {};
    bad.amplitude = 16.5;
    expect(!prepare(bad), "an amplitude above 16 LSBs is refused");
    bad.amplitude = -0.5;
    expect(!prepare(bad), "a negative amplitude is refused");
    bad = {};
    bad.seed = -1;
    expect(!prepare(bad), "a negative seed is refused");
    // The program table is shaped as the kernel reads it.
    const auto window = render::ImageWindow::create(0, 0, 8, 8);
    const auto data = detail::ditherProgramData(*prepare({}), *window.value(), 1.0, 1.0);
    expect(data && detail::validDitherProgramData(*data) &&
               data->size() == detail::kDitherProgramHeaderFloats + 4096U,
           "the program table holds the header, the geometry and the 64 x 64 ranks");
    auto broken = *data;
    broken[detail::kDitherProgramHeaderFloats + 5] = 4096.0F;
    expect(!detail::validDitherProgramData(broken), "a rank out of range is refused");
    expect(!detail::ditherProgramData(*prepare({}), *window.value(), 0.0, 1.0),
           "a zero scale cannot be represented");
}

// The noise the dither adds to a flat mid grey: one value per pixel and channel, in LSBs.
std::vector<double> noise(const DitherSettings& s, const double frame, const int channel = 0) {
    const auto grade = prepare(s, frame);
    const auto input = image(0, 0, 64, 64, [](auto, auto) { return Pixel{0.5F, 0.5F, 0.5F, 1.0F}; });
    const auto output = run(*grade, input);
    std::vector<double> values;
    const double e = encode(0.5);
    for (std::int64_t y = 0; y < 64; ++y)
        for (std::int64_t x = 0; x < 64; ++x)
            values.push_back((encode(at(output, x, y)[static_cast<std::size_t>(channel)]) - e) * 255.0);
    return values;
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const double n = static_cast<double>(a.size());
    const double ma = std::accumulate(a.begin(), a.end(), 0.0) / n;
    const double mb = std::accumulate(b.begin(), b.end(), 0.0) / n;
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return ab / std::sqrt(aa * bb);
}

void testSeedsAndFrames() {
    for (const auto& base : {DitherSettings{}, dither_test::ordered(8)}) {
        const std::string name = base.blueNoise ? "blue noise" : "Bayer 8";
        const auto a = noise(base, 1);
        expect(a == noise(base, 1), name + ": the same seed and frame give the same pixels");
        auto other = base;
        other.seed = 2;
        expect(a != noise(other, 1), name + ": another seed gives other pixels");
        // The noise is the pattern: a uniform spread of 1 LSB peak to peak, mean 0.
        const auto [low, high] = std::ranges::minmax(a);
        const double mean = std::accumulate(a.begin(), a.end(), 0.0) / static_cast<double>(a.size());
        expect(low > -0.5 && high < 0.5 && high - low > 0.9 && std::abs(mean) < 0.01,
               name + ": 1 LSB of noise spans (-0.5, 0.5) LSB with mean 0");
        // Every Frame: consecutive frames are unrelated on average; Held is one pattern.
        double sum = 0.0;
        double worst = 0.0;
        constexpr int kPairs = 24;
        for (int f = 0; f < kPairs; ++f) {
            const double r = correlation(noise(base, f), noise(base, f + 1));
            sum += r;
            worst = std::max(worst, std::abs(r));
        }
        const double meanCorrelation = sum / kPairs;
        std::cout << name << ": mean frame-to-frame correlation " << meanCorrelation << ", largest "
                  << worst << '\n';
        expect(std::abs(meanCorrelation) < 0.15,
               name + ": Every Frame noise is uncorrelated from frame to frame on average");
        if (base.blueNoise)
            expect(worst < 0.3, "blue noise: no two consecutive frames are correlated");
        auto held = base;
        held.everyFrame = false;
        expect(noise(held, 1) == noise(held, 2) && noise(held, 0) == noise(held, 7),
               name + ": Held is the same pattern on every frame");
        // Per channel the channels are unrelated; Monochrome shares red's.
        const double channels = correlation(noise(base, 1, 0), noise(base, 1, 1));
        expect(std::abs(channels) < 0.3, name + ": per channel, red and green are unrelated");
        auto mono = base;
        mono.monochrome = true;
        expect(noise(mono, 1, 0) == noise(mono, 1, 2), name + ": Monochrome gives every channel red's");
    }
}

void testRegionAndProxy() {
    const auto s = dither_test::everyPath();
    const auto grade = prepare(s, 2);
    const auto whole = run(*grade, image(0, 0, 64, 48, varied));
    // A region of interest reads the same pattern at the same pixels.
    const auto part = run(*grade, image(20, 12, 16, 12, varied));
    bool same = true;
    for (std::int64_t y = 12; y < 24; ++y)
        for (std::int64_t x = 20; x < 36; ++x)
            same = same && at(part, x, y) == at(whole, x, y);
    expect(same, "a region's dither is the whole frame's, pixel for pixel");
    // A proxy pixel takes the threshold of the full-resolution pixel under its centre,
    // floor((x + 0.5) / scale): at half scale proxy (x, y) is full (2x + 1, 2y + 1). On a flat
    // picture the two are the same pixel.
    const auto flat = [](auto, auto) { return Pixel{0.3F, 0.02F, 0.6F, 0.8F}; };
    const auto full = run(*grade, image(0, 0, 64, 48, flat));
    const auto proxy = run(*grade, image(0, 0, 32, 24, flat), 0.5, 0.5);
    bool rule = true;
    for (std::int64_t y = 0; y < 24; ++y)
        for (std::int64_t x = 0; x < 32; ++x)
            rule = rule && at(proxy, x, y) == at(full, 2 * x + 1, 2 * y + 1);
    expect(rule, "half scale is the full-resolution dither at the pixel under each proxy centre");
    // A quarter-scale proxy whose data window starts off the origin keeps the same rule.
    const auto quarter = run(*grade, image(3, 2, 8, 6, flat), 0.25, 0.25);
    bool offset = true;
    for (std::int64_t y = 2; y < 8; ++y)
        for (std::int64_t x = 3; x < 11; ++x)
            offset = offset && at(quarter, x, y) == at(full, 4 * x + 2, 4 * y + 2);
    expect(offset, "a quarter-scale window off the origin reads the full-resolution pixel under each "
                   "centre");
}

void testIdentityKeys() {
    const auto key = [](const DitherSettings& s, const double frame = 1.0) {
        return detail::colorGradeIdentity(*prepare(s, frame));
    };
    const DitherSettings base;
    const auto k = key(base);
    const auto changed = [&](const std::string& what, const std::function<void(DitherSettings&)>& edit) {
        auto s = base;
        edit(s);
        expect(key(s) != k, what + " changes the table's identity");
    };
    changed("the amplitude", [](auto& s) { s.amplitude = 1.5; });
    changed("the seed", [](auto& s) { s.seed = 9; });
    changed("the pattern", [](auto& s) { s = dither_test::ordered(8); });
    changed("the bit depth", [](auto& s) { s.bits = 10; });
    changed("the domain", [](auto& s) { s.stored = true; });
    changed("Monochrome", [](auto& s) { s.monochrome = true; });
    expect(key(dither_test::ordered(4)) != key(dither_test::ordered(8)),
           "the matrix size changes the identity");
    expect(key(base, 1) != key(base, 2), "Every Frame: each frame has its own identity");
    auto held = base;
    held.everyFrame = false;
    expect(key(held, 1) == key(held, 2), "Held: every frame shares one identity");
    expect(key(base, 1) == key(base, 1), "the same settings and frame share one identity");
}

// --- The purpose: less banding at 8 bit -----------------------------------------------------------

// The 8-bit code value a delivery stores for a straight value (sRGB, rounded to nearest).
int code8(const double v) {
    return static_cast<int>(std::lround(std::clamp(encode(v), 0.0, 1.0) * 255.0));
}

struct Banding final {
    double meanRun = 0.0;   // the mean length of a run of one code along a row, in pixels
    double lowError = 0.0;  // the RMS of the error after a 9 x 9 box average, in LSBs
};

// Quantizes `picture` (straight values, alpha 1) to 8 bit with or without the dither and measures
// along rows how long a code holds and how far the local average strays from the input.
Banding banding(const std::uint32_t width, const std::uint32_t height,
                const std::function<double(std::int64_t, std::int64_t)>& picture,
                const std::optional<DitherSettings>& dither) {
    const auto input = image(0, 0, width, height, [&](auto x, auto y) {
        const auto v = static_cast<float>(picture(x, y));
        return Pixel{v, v, v, 1.0F};
    });
    const Image dithered = dither ? run(*prepare(*dither, 1), input) : nullptr;
    const auto& output = dithered ? *dithered : input;
    std::vector<int> codes(static_cast<std::size_t>(width) * height);
    std::vector<double> error(codes.size());
    for (std::int64_t y = 0; y < height; ++y)
        for (std::int64_t x = 0; x < width; ++x) {
            const auto i = static_cast<std::size_t>(y * width + x);
            codes[i] = code8(at(output, x, y)[0]);
            error[i] = static_cast<double>(codes[i]) - encode(picture(x, y)) * 255.0;
        }
    Banding result;
    std::size_t runs = 0;
    for (std::int64_t y = 0; y < height; ++y) {
        std::size_t row = 1;
        for (std::int64_t x = 1; x < width; ++x)
            if (codes[static_cast<std::size_t>(y * width + x)] !=
                codes[static_cast<std::size_t>(y * width + x - 1)])
                ++row;
        runs += row;
    }
    result.meanRun = static_cast<double>(width * height) / static_cast<double>(runs);
    double sum = 0.0;
    std::size_t count = 0;
    for (std::int64_t y = 4; y + 4 < height; ++y)
        for (std::int64_t x = 4; x + 4 < width; ++x) {
            double local = 0.0;
            for (std::int64_t j = -4; j <= 4; ++j)
                for (std::int64_t i = -4; i <= 4; ++i)
                    local += error[static_cast<std::size_t>((y + j) * width + x + i)];
            local /= 81.0;
            sum += local * local;
            ++count;
        }
    result.lowError = std::sqrt(sum / static_cast<double>(count));
    return result;
}

void testBanding() {
    // A shallow gradient across 512 pixels (a few codes of 8 bit) and a broad, dim glow's falloff,
    // where 8 bit draws visible rings.
    const auto gradient = [](const std::int64_t x, std::int64_t) {
        return 0.18 + 0.02 * static_cast<double>(x) / 511.0;
    };
    const auto glow = [](const std::int64_t x, const std::int64_t y) {
        const double dx = static_cast<double>(x) - 128.0;
        const double dy = static_cast<double>(y) - 128.0;
        return 0.01 + 0.03 * std::exp(-(dx * dx + dy * dy) / (2.0 * 150.0 * 150.0));
    };
    struct Case final {
        const char* name;
        std::uint32_t width;
        std::uint32_t height;
        std::function<double(std::int64_t, std::int64_t)> picture;
    };
    const std::array cases{Case{"gradient", 512, 32, gradient}, Case{"glow", 256, 256, glow}};
    for (const auto& c : cases) {
        const auto plain = banding(c.width, c.height, c.picture, std::nullopt);
        for (const auto& d : {DitherSettings{}, dither_test::ordered(8)}) {
            const auto dithered = banding(c.width, c.height, c.picture, d);
            std::cout << c.name << (d.blueNoise ? " blue noise" : " Bayer 8")
                      << ": mean run without " << plain.meanRun << " px, with " << dithered.meanRun
                      << " px; low-frequency error without " << plain.lowError << " LSB, with "
                      << dithered.lowError << " LSB\n";
            expect(dithered.meanRun * 4.0 < plain.meanRun,
                   std::string(c.name) + ": the dither breaks the bands into runs under a quarter "
                                         "as long");
            expect(dithered.lowError * 3.0 < plain.lowError,
                   std::string(c.name) + ": the dithered picture's local average is at least three "
                                         "times closer to the input");
        }
    }
}

void testProgramText() {
    const auto& text = detail::ditherProgramText();
    expect(text.find("vec4 bloom_dither(ivec2 c) {") != std::string::npos &&
               text.find("void bdk_dither(int column, int row") != std::string::npos &&
               text.find("#define BDK_P precise") != std::string::npos,
           "the program wraps the one dither kernel with precise arithmetic");
    const auto window = render::ImageWindow::create(0, 0, 4, 4);
    const auto data = detail::ditherProgramData(*prepare({}), *window.value(), 1.0, 1.0);
    const auto program = detail::buildColorGradeProgram(detail::ColorGradeKind::Dither, *data);
    expect(program.succeeded() && program.program()->functionName == "bloom_dither",
           "the GPU program builds from the table");
    expect(!detail::buildColorGradeProgram(detail::ColorGradeKind::Dither, prepare({})->data)
                .succeeded(),
           "a table without its geometry is refused");
}

void testBudgetAndCancellation() {
    const auto grade = prepare(dither_test::everyPath());
    const auto input = image(0, 0, 8, 4, varied);
    const auto refused = detail::applyDither(*grade, input, 1.0, 1.0, 4, {},
                                             runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(!refused.image && refused.diagnostic.has_value(), "a refused budget is an explicit error");
    const auto output = run(*grade, input);
    expect(output->descriptor()->dataWindow() == input.descriptor()->dataWindow(),
           "the data window is kept");

    const auto large = image(0, 0, 256, 256, varied);
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(527)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Dither cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result =
                detail::applyDither(*grade, large, 1.0, 1.0, 1U << 28U, context.cancellation(),
                                    runtime::OperationIndex::fromRaw(0), {}, nullptr);
            cancelled = result.cancelled && !result.image;
            return runtime::TaskResult<void>::cancelled();
        });
    expect(submission.accepted(), "the cancellation task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> taken;
    while (!taken && std::chrono::steady_clock::now() < deadline) {
        taken = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(taken && taken->state() == runtime::TaskState::Cancelled && cancelled,
           "a cancelled pass publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

void testEvaluator() {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto settings = dither_test::everyPath();
    const auto plan = dither_test::ditherPlan(settings);
    auto heldSettings = settings;
    heldSettings.everyFrame = false;
    const auto heldPlan = dither_test::ditherPlan(heldSettings);
    const auto effectIndex = runtime::OperationIndex::fromRaw(1);
    expect(plan->operationTimeDependent(effectIndex), "a dither drawn for every frame varies with time");
    expect(!heldPlan->operationTimeDependent(effectIndex), "a held dither does not");
    const auto request = [&](const auto& which, const std::int64_t frame) {
        runtime::EvaluationRequest r{*core::RationalTime::create(frame, 24), which->output(),
                                     runtime::CompositionFormatResolution{}};
        r.pixelStorageByteLimit = 1U << 26U;
        return r;
    };
    const auto pixels = [&](const auto& which, const std::int64_t frame) {
        const auto result = evaluator.evaluate(which, request(which, frame), {});
        if (result.frame() == nullptr)
            throw std::logic_error("the plan evaluates");
        const auto view = result.frame()->processImage().pixels();
        return std::vector<render::Rgba32f>(view.begin(), view.end());
    };
    const auto first = pixels(plan, 1);
    const auto second = pixels(plan, 2);
    expect(first != second, "a new frame is a new placement, through the operation cache");
    expect(first == pixels(plan, 1), "returning to a frame draws its dither again");
    expect(pixels(heldPlan, 1) == pixels(heldPlan, 2), "a held dither is one pattern on every frame");

    auto regional = request(plan, 1);
    const auto region = render::ImageWindow::create(20, 12, 16, 12);
    regional.roi = *region.value();
    const auto part = evaluator.evaluate(plan, regional, {});
    const auto whole = evaluator.evaluate(plan, request(plan, 1), {});
    expect(part.frame() != nullptr && whole.frame() != nullptr, "a region of interest evaluates");
    if (part.frame() != nullptr && whole.frame() != nullptr) {
        const auto& crop = part.frame()->processImage();
        const auto& frame = whole.frame()->processImage();
        bool same = true;
        const auto window = crop.descriptor()->dataWindow();
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
            for (auto x = window.originX(); x < window.maxXExclusive(); ++x)
                same = same && at(crop, x, y) == at(frame, x, y);
        expect(same, "a region's dither is the whole frame's");
    }
    auto proxied = request(plan, 1);
    const auto extent = render::ImageExtent::create(32, 24);
    proxied.resolution = runtime::ProxyResolution{*extent.value()};
    expect(evaluator.evaluate(plan, proxied, {}).frame() != nullptr, "a proxy evaluates");
}

} // namespace

int main() {
    try {
        testReference();
        testIdentityAndDomain();
        testSeedsAndFrames();
        testRegionAndProxy();
        testIdentityKeys();
        testBanding();
        testProgramText();
        testBudgetAndCancellation();
        testEvaluator();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
