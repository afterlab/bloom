// #742 filtered sampling: the CPU reference of AffineBilinearV1's Trilinear and Anisotropic modes
// against an independent binary64 model.
//  - Mip levels: each level built by the production rule (the Bilinear composed-matrix resample of
//    the level above through affineMipReductionMatrix()) equals the binary64 box average of the
//    2^n x 2^n source block it covers, transparent outside, for even, odd and offset sources.
//  - Plans: at shrink factors 1, 1.5, 4, 8 and 16 and on rotated and skewed maps, the levels,
//    weight, tap count and tap steps match a closed-form singular value decomposition computed
//    another way (the half-sum and half-difference form, the major axis from atan2).
//  - Rows: affineFilteredResampleRow() matches a binary64 trilinear/anisotropic sampler over the
//    binary64 box pyramid, and a region of interest gets the same pixels as the full lattice.
//  - Aliasing: a checker whose cells are far finer than the output pixel grid, shrunk and moving
//    over five frames. Every frequency of the checker is above the output's Nyquist limit, so the
//    ideal band-limited picture is its mean and every deviation from it is aliased energy. Bilinear
//    and Nearest keep a large share of it and flicker between frames; Trilinear and Anisotropic
//    remove it.
// Pure host arithmetic: identical in the Vulkan and stub builds.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using bloom::render::GpuAffineFilter;
using bloom::render::GpuAffineFilterPlan;
using bloom::render::GpuAffineMatrix;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageView;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using Pixel = std::array<double, 4>;

// A binary64 image over a 0-based window, transparent outside.
struct Reference final {
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<Pixel> pixels;
    [[nodiscard]] Pixel at(const std::int64_t x, const std::int64_t y) const {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return {0.0, 0.0, 0.0, 0.0};
        return pixels[static_cast<std::size_t>(y * width + x)];
    }
};

[[nodiscard]] ImageWindow window(const std::int64_t x, const std::int64_t y, const std::int64_t w,
                                 const std::int64_t h) {
    const auto created =
        ImageWindow::create(x, y, static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h));
    if (!created)
        throw std::logic_error("test window is valid");
    return *created.value();
}

[[nodiscard]] Rgba32f pixelOf(const Pixel& p) {
    const auto made =
        Rgba32f::fromPremultiplied(static_cast<float>(p[0]), static_cast<float>(p[1]),
                                   static_cast<float>(p[2]), static_cast<float>(p[3]));
    if (!made)
        throw std::logic_error("test pixel is valid");
    return *made.value();
}

// An image over `data` whose pixels come from `value(localX, localY)` (rounded to Float32), with
// the binary64 reference of exactly the stored values.
struct Source final {
    Rgba32fImage image;
    Reference reference;
};
[[nodiscard]] Source makeSource(const ImageWindow data,
                                const std::function<Pixel(std::int64_t, std::int64_t)>& value) {
    const auto descriptor = bloom::render::Rgba32fImageDescriptor::create(
        data, data, bloom::core::PixelAspectRatio::square());
    if (!descriptor)
        throw std::logic_error("test descriptor is valid");
    auto builder = bloom::render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 28U);
    if (!builder)
        throw std::logic_error("test builder is created");
    const auto w = static_cast<std::int64_t>(data.extent().width());
    const auto h = static_cast<std::int64_t>(data.extent().height());
    Reference reference{w, h, {}};
    for (std::int64_t y = 0; y < h; ++y) {
        auto row = builder.value()->row(data.originY() + y);
        if (!row)
            throw std::logic_error("test row is writable");
        for (std::int64_t x = 0; x < w; ++x) {
            const auto stored = pixelOf(value(x, y));
            (*row.value())[static_cast<std::size_t>(x)] = stored;
            reference.pixels.push_back(
                {static_cast<double>(stored.red()), static_cast<double>(stored.green()),
                 static_cast<double>(stored.blue()), static_cast<double>(stored.alpha())});
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        throw std::logic_error("test image freezes");
    return Source{std::move(*frozen.value()), std::move(reference)};
}

[[nodiscard]] Rgba32fImageView viewOf(const Rgba32fImage& image) {
    const auto view = image.view();
    if (!view)
        throw std::logic_error("test image has a view");
    return *view.value();
}

// The production levels: level n + 1 is the Bilinear composed-matrix resample of level n through
// affineMipReductionMatrix(), over a 0-based window of the halved (ceil) extent.
[[nodiscard]] std::vector<Rgba32fImage> productionLevels(const Rgba32fImage& source,
                                                         const std::uint32_t deepest) {
    std::vector<Rgba32fImage> levels;
    const auto data = viewOf(source).descriptor()->dataWindow();
    for (std::uint32_t level = 1; level <= deepest; ++level) {
        const auto previous = viewOf(level == 1 ? source : levels.back());
        const auto [width, height] = bloom::render::affineMipLevelExtent(
            static_cast<std::uint32_t>(data.extent().width()),
            static_cast<std::uint32_t>(data.extent().height()), level);
        const auto out = window(0, 0, width, height);
        const auto descriptor = bloom::render::Rgba32fImageDescriptor::create(
            out, out, bloom::core::PixelAspectRatio::square());
        auto builder = bloom::render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 28U);
        for (std::int64_t y = 0; y < static_cast<std::int64_t>(height); ++y) {
            auto row = builder.value()->row(y);
            const auto status = bloom::render::affineMatrixResampleRow(
                previous, bloom::render::affineMipReductionMatrix(), 1.0F,
                GpuAffineFilter::Bilinear, out, out, y, *row.value());
            if (status)
                throw std::logic_error("production level row resamples");
        }
        auto frozen = std::move(*builder.value()).freeze();
        levels.push_back(std::move(*frozen.value()));
    }
    return levels;
}

// The binary64 box level: each texel the mean of the 2^n x 2^n source block it covers, transparent
// outside the source.
[[nodiscard]] Reference boxLevel(const Reference& source, const std::uint32_t level) {
    const std::int64_t side = std::int64_t{1} << level;
    Reference out;
    out.width = (source.width + side - 1) / side;
    out.height = (source.height + side - 1) / side;
    const double area = static_cast<double>(side * side);
    for (std::int64_t y = 0; y < out.height; ++y)
        for (std::int64_t x = 0; x < out.width; ++x) {
            Pixel sum{};
            for (std::int64_t j = 0; j < side; ++j)
                for (std::int64_t i = 0; i < side; ++i) {
                    const auto p = source.at(x * side + i, y * side + j);
                    for (std::size_t c = 0; c < 4; ++c)
                        sum[c] += p[c];
                }
            for (auto& c : sum)
                c /= area;
            out.pixels.push_back(sum);
        }
    return out;
}

[[nodiscard]] bool close(const double actual, const double expected, const double tolerance) {
    return std::isfinite(actual) &&
           std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected));
}

// A signed HDR translucent pattern: smooth gradients plus a hashed component, so every level and
// every tap has its own value.
[[nodiscard]] Pixel pattern(const std::int64_t x, const std::int64_t y) {
    const auto hash = static_cast<std::uint32_t>((x * 73856093) ^ (y * 19349663)) % 1009U;
    const double noise = static_cast<double>(hash) / 1009.0;
    const double alpha = 0.25 + 0.75 * (static_cast<double>((x + 2 * y) % 5) / 4.0);
    return {(-0.5 + 2.5 * noise) * alpha, (0.1 * static_cast<double>(x % 17) - 0.3) * alpha,
            (3.0 * noise * noise) * alpha, alpha};
}

void testLevels() {
    for (const auto& data : {window(0, 0, 32, 32), window(0, 0, 37, 23), window(-5, 3, 19, 41)}) {
        const auto source = makeSource(data, pattern);
        const auto deepest = bloom::render::affineMipDeepestLevel(
            static_cast<std::uint32_t>(data.extent().width()),
            static_cast<std::uint32_t>(data.extent().height()));
        const auto levels = productionLevels(source.image, deepest);
        bool exact = true;
        for (std::uint32_t level = 1; level <= deepest; ++level) {
            const auto expected = boxLevel(source.reference, level);
            const auto view = viewOf(levels[level - 1]);
            const auto extent = view.descriptor()->dataWindow().extent();
            exact = exact && static_cast<std::int64_t>(extent.width()) == expected.width &&
                    static_cast<std::int64_t>(extent.height()) == expected.height;
            for (std::int64_t y = 0; exact && y < expected.height; ++y) {
                const auto row = view.row(y);
                for (std::int64_t x = 0; exact && x < expected.width; ++x)
                    for (std::size_t c = 0; c < 4; ++c)
                        exact =
                            exact &&
                            close(static_cast<double>(
                                      (*row.value())[static_cast<std::size_t>(x)].components()[c]),
                                  expected.at(x, y)[c], 1e-6);
            }
        }
        const auto last = viewOf(levels.back()).descriptor()->dataWindow().extent();
        expect(exact && last.width() == 1 && last.height() == 1,
               "mip levels " + std::to_string(data.extent().width()) + "x" +
                   std::to_string(data.extent().height()) +
                   " are the binary64 box average of the block each covers, transparent outside, "
                   "down to 1x1");
    }
}

// The independent model of a map's footprint: the singular values of the inverse Jacobian from
// the half-sum/half-difference form and the major axis angle from atan2.
struct Footprint final {
    double major = 0.0;
    double minor = 0.0;
    double angle = 0.0;
};
[[nodiscard]] Footprint footprintOf(const GpuAffineMatrix& m) {
    const double det = m.a * m.d - m.b * m.c;
    const double j00 = m.d / det;
    const double j01 = -m.b / det;
    const double j10 = -m.c / det;
    const double j11 = m.a / det;
    const double s = std::hypot(j00 + j11, j10 - j01) * 0.5;
    const double t = std::hypot(j00 - j11, j10 + j01) * 0.5;
    const double e00 = j00 * j00 + j01 * j01;
    const double e11 = j10 * j10 + j11 * j11;
    const double e01 = j00 * j10 + j01 * j11;
    return {s + t, std::abs(s - t), 0.5 * std::atan2(2.0 * e01, e00 - e11)};
}

struct ReferencePlan final {
    bool filtered = false;
    std::uint32_t lower = 0;
    std::uint32_t upper = 0;
    double weight = 0.0;
    std::uint32_t taps = 1;
    double stepX = 0.0;
    double stepY = 0.0;
};
[[nodiscard]] ReferencePlan referencePlan(const GpuAffineMatrix& m, const GpuAffineFilter filter,
                                          const std::uint32_t w, const std::uint32_t h) {
    ReferencePlan plan;
    const auto f = footprintOf(m);
    if (f.major <= 1.0 + 1e-12)
        return plan;
    plan.filtered = true;
    double rho = f.major;
    if (filter == GpuAffineFilter::Anisotropic) {
        plan.taps = static_cast<std::uint32_t>(std::min(std::ceil(f.major / f.minor - 1e-9), 16.0));
        rho = f.major / plan.taps;
        if (plan.taps > 1) {
            plan.stepX = std::cos(f.angle) * rho;
            plan.stepY = std::sin(f.angle) * rho;
        }
    }
    std::uint32_t deepest = 0;
    for (std::uint32_t x = w, y = h; x > 1 || y > 1; x = (x + 1) / 2, y = (y + 1) / 2)
        ++deepest;
    if (rho > 1.0) {
        plan.lower = static_cast<std::uint32_t>(std::floor(std::log2(rho) + 1e-12));
        plan.weight = rho / std::ldexp(1.0, static_cast<int>(plan.lower)) - 1.0;
    }
    plan.upper = plan.lower + 1;
    if (plan.lower >= deepest) {
        plan.lower = deepest;
        plan.upper = deepest;
        plan.weight = 0.0;
    }
    if (plan.weight < 1e-12)
        plan.upper = plan.lower;
    return plan;
}

[[nodiscard]] GpuAffineMatrix shrink(const double sx, const double sy, const double degrees,
                                     const double skew, const double tx, const double ty) {
    // Forward: rotate(scale(skew(local))), with skew along x.
    const double r = degrees * std::numbers::pi / 180.0;
    const double c = std::cos(r);
    const double s = std::sin(r);
    const double a0 = sx;
    const double b0 = sx * skew;
    const double c0 = 0.0;
    const double d0 = sy;
    return {c * a0 - s * c0, c * b0 - s * d0, tx, s * a0 + c * c0, s * b0 + c * d0, ty};
}

struct MapCase final {
    GpuAffineMatrix matrix;
    std::string name;
};
[[nodiscard]] std::vector<MapCase> mapCases() {
    std::vector<MapCase> cases;
    for (const double factor : {1.0, 1.5, 4.0, 8.0, 16.0})
        cases.push_back({shrink(1.0 / factor, 1.0 / factor, 0.0, 0.0, 3.25, 2.5),
                         "shrink " + std::to_string(factor)});
    cases.push_back({shrink(0.25, 0.25, 30.0, 0.0, 20.0, 1.0), "shrink 4 rotated 30"});
    cases.push_back({shrink(0.125, 0.5, 17.0, 0.0, 12.0, 4.0), "anisotropic 8 x 2 rotated 17"});
    cases.push_back({shrink(0.2, 0.6, -40.0, 0.35, 18.0, 6.0), "skewed, rotated -40"});
    cases.push_back({shrink(0.05, 0.7, 90.0, 0.0, 30.0, 2.0), "anisotropic 20 x 1.4 quarter turn"});
    cases.push_back({shrink(2.0, 1.5, 10.0, 0.0, 1.0, 1.0), "magnified, rotated 10"});
    return cases;
}

void testPlans() {
    constexpr std::uint32_t kSide = 128;
    for (const auto& item : mapCases()) {
        for (const auto filter : {GpuAffineFilter::Trilinear, GpuAffineFilter::Anisotropic}) {
            const auto plan = bloom::render::planAffineFilter(item.matrix, filter, kSide, kSide);
            const auto expected = referencePlan(item.matrix, filter, kSide, kSide);
            const std::string name =
                item.name + (filter == GpuAffineFilter::Trilinear ? " Trilinear" : " Anisotropic");
            if (!expected.filtered) {
                expect(plan.filter == GpuAffineFilter::Bilinear && !plan.filtered(),
                       name + ": a map that shrinks nothing resolves to Bilinear exactly");
                continue;
            }
            const bool stepsMatch = (close(plan.tapStepX, expected.stepX, 1e-9) &&
                                     close(plan.tapStepY, expected.stepY, 1e-9)) ||
                                    (close(plan.tapStepX, -expected.stepX, 1e-9) &&
                                     close(plan.tapStepY, -expected.stepY, 1e-9));
            const double offset = -0.5 * static_cast<double>(plan.taps - 1);
            expect(plan.filter == filter && plan.lowerLevel == expected.lower &&
                       plan.upperLevel == expected.upper &&
                       close(static_cast<double>(plan.upperWeight), expected.weight, 1e-6) &&
                       plan.taps == expected.taps && stepsMatch &&
                       plan.tapStartX == plan.tapStepX * offset &&
                       plan.tapStartY == plan.tapStepY * offset &&
                       plan.tapWeight == 1.0F / static_cast<float>(plan.taps),
                   name + ": levels " + std::to_string(plan.lowerLevel) + "," +
                       std::to_string(plan.upperLevel) + " weight " +
                       std::to_string(plan.upperWeight) + " taps " + std::to_string(plan.taps) +
                       " match the closed-form footprint (want " + std::to_string(expected.lower) +
                       "," + std::to_string(expected.upper) + " " +
                       std::to_string(expected.weight) + " " + std::to_string(expected.taps) + ")");
        }
        for (const auto filter : {GpuAffineFilter::Bilinear, GpuAffineFilter::Nearest}) {
            const auto plan = bloom::render::planAffineFilter(item.matrix, filter, kSide, kSide);
            expect(plan.filter == filter && !plan.filtered() && plan.taps == 1,
                   item.name + ": Bilinear and Nearest resolve to themselves");
        }
    }
    // The 1.5 shrink blends the source and level 1 halfway; 16 reads level 4 alone.
    const auto half = bloom::render::planAffineFilter(shrink(1.0 / 1.5, 1.0 / 1.5, 0, 0, 0, 0),
                                                      GpuAffineFilter::Trilinear, kSide, kSide);
    expect(half.lowerLevel == 0 && half.upperLevel == 1 && half.upperWeight == 0.5F,
           "a 1.5x shrink blends level 0 and level 1 by one half");
    const auto sixteen = bloom::render::planAffineFilter(shrink(1.0 / 16, 1.0 / 16, 0, 0, 0, 0),
                                                         GpuAffineFilter::Trilinear, kSide, kSide);
    expect(sixteen.lowerLevel == 4 && sixteen.upperLevel == 4 && sixteen.upperWeight == 0.0F,
           "an exact 16x shrink reads level 4 alone");
    // Deeper than the pyramid: an 8x8 source shrunk 64x clamps at its 1x1 level.
    const auto clamped = bloom::render::planAffineFilter(shrink(1.0 / 64, 1.0 / 64, 0, 0, 0, 0),
                                                         GpuAffineFilter::Trilinear, 8, 8);
    expect(clamped.lowerLevel == 3 && clamped.upperLevel == 3 && clamped.upperWeight == 0.0F,
           "a footprint deeper than the pyramid clamps at the 1x1 level");
    // Anisotropy caps at 16 taps.
    const auto capped = bloom::render::planAffineFilter(shrink(1.0 / 64, 1.0, 0, 0, 0, 0),
                                                        GpuAffineFilter::Anisotropic, kSide, kSide);
    expect(capped.taps == 16 && capped.lowerLevel == 2 && capped.upperLevel == 2,
           "anisotropy is capped at 16 taps; the rest of the footprint goes to the level");
}

// The binary64 sampler: level n at the level-0 coordinate (x, y), bilinear over the box level,
// transparent outside.
[[nodiscard]] Pixel sampleLevel(const Reference& level, const std::uint32_t n, const double x,
                                const double y) {
    const double scale = std::ldexp(1.0, -static_cast<int>(n));
    const double lx = (x + 0.5) * scale - 0.5;
    const double ly = (y + 0.5) * scale - 0.5;
    const double fx = std::floor(lx);
    const double fy = std::floor(ly);
    const double tx = lx - fx;
    const double ty = ly - fy;
    const auto ix = static_cast<std::int64_t>(fx);
    const auto iy = static_cast<std::int64_t>(fy);
    Pixel out{};
    for (std::size_t c = 0; c < 4; ++c) {
        const double top = level.at(ix, iy)[c] * (1.0 - tx) + level.at(ix + 1, iy)[c] * tx;
        const double bottom =
            level.at(ix, iy + 1)[c] * (1.0 - tx) + level.at(ix + 1, iy + 1)[c] * tx;
        out[c] = top * (1.0 - ty) + bottom * ty;
    }
    return out;
}

[[nodiscard]] GpuAffineMatrix inverseOf(const GpuAffineMatrix& m) {
    const double det = m.a * m.d - m.b * m.c;
    return {m.d / det,  -m.b / det, (m.b * m.ty - m.d * m.tx) / det,
            -m.c / det, m.a / det,  (m.c * m.tx - m.a * m.ty) / det};
}

// The output window covering the forward image of the source, with a margin.
[[nodiscard]] ImageWindow coverOf(const GpuAffineMatrix& m, const std::int64_t w,
                                  const std::int64_t h) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& [x, y] :
         {std::pair{-0.5, -0.5}, std::pair{static_cast<double>(w) - 0.5, -0.5},
          std::pair{-0.5, static_cast<double>(h) - 0.5},
          std::pair{static_cast<double>(w) - 0.5, static_cast<double>(h) - 0.5}}) {
        const double ox = m.a * x + m.b * y + m.tx;
        const double oy = m.c * x + m.d * y + m.ty;
        x0 = std::min(x0, ox);
        y0 = std::min(y0, oy);
        x1 = std::max(x1, ox);
        y1 = std::max(y1, oy);
    }
    const auto left = static_cast<std::int64_t>(std::floor(x0)) - 2;
    const auto top = static_cast<std::int64_t>(std::floor(y0)) - 2;
    return window(left, top, static_cast<std::int64_t>(std::ceil(x1)) + 3 - left,
                  static_cast<std::int64_t>(std::ceil(y1)) + 3 - top);
}

[[nodiscard]] std::vector<Rgba32f>
filteredRows(const std::vector<Rgba32fImage>& levels, const Rgba32fImage& source,
             const GpuAffineMatrix& matrix, const GpuAffineFilterPlan& plan, const float opacity,
             const ImageWindow lattice, const ImageWindow out, bool& ok) {
    const auto levelView = [&](const std::uint32_t n) {
        return viewOf(n == 0 ? source : levels[n - 1]);
    };
    std::vector<Rgba32f> pixels;
    const auto width = static_cast<std::size_t>(out.extent().width());
    for (std::int64_t y = out.originY(); y < out.maxYExclusive(); ++y) {
        std::vector<Rgba32f> row(width, Rgba32f::transparent());
        ok = ok && !bloom::render::affineFilteredResampleRow(levelView(plan.lowerLevel),
                                                             levelView(plan.upperLevel), matrix,
                                                             plan, opacity, lattice, out, y, row);
        pixels.insert(pixels.end(), row.begin(), row.end());
    }
    return pixels;
}

void testRows() {
    constexpr std::int64_t kSide = 96;
    const auto data = window(0, 0, kSide, kSide);
    const auto source = makeSource(data, pattern);
    const auto deepest = bloom::render::affineMipDeepestLevel(kSide, kSide);
    const auto levels = productionLevels(source.image, deepest);
    std::vector<Reference> boxes{source.reference};
    for (std::uint32_t n = 1; n <= deepest; ++n)
        boxes.push_back(boxLevel(source.reference, n));
    constexpr float kOpacity = 0.75F;
    for (const auto& item : mapCases()) {
        for (const auto filter : {GpuAffineFilter::Trilinear, GpuAffineFilter::Anisotropic}) {
            const std::string name =
                item.name + (filter == GpuAffineFilter::Trilinear ? " Trilinear" : " Anisotropic");
            const auto plan = bloom::render::planAffineFilter(item.matrix, filter, kSide, kSide);
            const auto lattice = coverOf(item.matrix, kSide, kSide);
            if (!plan.filtered()) {
                // Unshrunk: the production route resamples Bilinear, and the filtered row refuses a
                // plan that reads no level.
                std::vector<Rgba32f> row(static_cast<std::size_t>(lattice.extent().width()),
                                         Rgba32f::transparent());
                expect(bloom::render::affineFilteredResampleRow(
                           viewOf(source.image), viewOf(source.image), item.matrix, plan, kOpacity,
                           lattice, lattice, lattice.originY(), row)
                           .has_value(),
                       name + ": the filtered row refuses an unfiltered plan");
                continue;
            }
            bool ok = true;
            const auto pixels = filteredRows(levels, source.image, item.matrix, plan, kOpacity,
                                             lattice, lattice, ok);
            const auto reference = referencePlan(item.matrix, filter, kSide, kSide);
            const auto inverse = inverseOf(item.matrix);
            double worst = 0.0;
            bool alphaInRange = true;
            std::size_t index = 0;
            for (std::int64_t y = lattice.originY(); y < lattice.maxYExclusive(); ++y)
                for (std::int64_t x = lattice.originX(); x < lattice.maxXExclusive(); ++x) {
                    const double qx = inverse.a * static_cast<double>(x) +
                                      inverse.b * static_cast<double>(y) + inverse.tx;
                    const double qy = inverse.c * static_cast<double>(x) +
                                      inverse.d * static_cast<double>(y) + inverse.ty;
                    Pixel expected{};
                    for (std::uint32_t t = 0; t < reference.taps; ++t) {
                        const double k =
                            static_cast<double>(t) - 0.5 * static_cast<double>(reference.taps - 1);
                        // The model's major axis may point the other way: the taps are centred, so
                        // the set is the same.
                        const double tx = qx + k * reference.stepX;
                        const double ty = qy + k * reference.stepY;
                        const auto lo =
                            sampleLevel(boxes[reference.lower], reference.lower, tx, ty);
                        const auto up =
                            sampleLevel(boxes[reference.upper], reference.upper, tx, ty);
                        for (std::size_t c = 0; c < 4; ++c)
                            expected[c] += lo[c] + (up[c] - lo[c]) * reference.weight;
                    }
                    const auto& actual = pixels[index++];
                    for (std::size_t c = 0; c < 4; ++c) {
                        const double want =
                            expected[c] / reference.taps * static_cast<double>(kOpacity);
                        worst = std::max(
                            worst, std::abs(static_cast<double>(actual.components()[c]) - want) /
                                       std::max(1.0, std::abs(want)));
                    }
                    alphaInRange = alphaInRange && actual.alpha() >= 0.0F && actual.alpha() <= 1.0F;
                }
            expect(ok && alphaInRange && worst <= 2e-5,
                   name +
                       ": the filtered rows match the binary64 trilinear/anisotropic sampler "
                       "over the binary64 box pyramid (worst " +
                       std::to_string(worst) + ")");
            // A region of interest inside the lattice gets the very same pixels.
            const auto inner = window(lattice.originX() + 2, lattice.originY() + 1,
                                      static_cast<std::int64_t>(lattice.extent().width()) / 2,
                                      static_cast<std::int64_t>(lattice.extent().height()) / 2);
            bool roiOk = true;
            const auto clipped = filteredRows(levels, source.image, item.matrix, plan, kOpacity,
                                              lattice, inner, roiOk);
            bool same = roiOk;
            std::size_t at = 0;
            const auto latticeWidth = static_cast<std::int64_t>(lattice.extent().width());
            for (std::int64_t y = inner.originY(); same && y < inner.maxYExclusive(); ++y)
                for (std::int64_t x = inner.originX(); same && x < inner.maxXExclusive(); ++x)
                    same = clipped[at++] ==
                           pixels[static_cast<std::size_t>((y - lattice.originY()) * latticeWidth +
                                                           (x - lattice.originX()))];
            expect(same,
                   name + ": a region of interest gets the full lattice's pixels bit for bit");
        }
    }
    // Levels whose extents are not the plan's are refused.
    const auto plan = bloom::render::planAffineFilter(shrink(0.25, 0.25, 0, 0, 0, 0),
                                                      GpuAffineFilter::Trilinear, kSide, kSide);
    const auto lattice = coverOf(shrink(0.25, 0.25, 0, 0, 0, 0), kSide, kSide);
    std::vector<Rgba32f> row(static_cast<std::size_t>(lattice.extent().width()),
                             Rgba32f::transparent());
    expect(bloom::render::affineFilteredResampleRow(viewOf(source.image), viewOf(source.image),
                                                    shrink(0.25, 0.25, 0, 0, 0, 0), plan, 1.0F,
                                                    lattice, lattice, lattice.originY(), row)
               .has_value(),
           "the filtered row refuses levels whose extents are not the plan's");
}

// The moving checker: 2 px cells (a period of four source pixels) shrunk 8x and rotated, so the
// checker's every frequency lies far above the output's Nyquist limit and the ideal band-limited
// picture is its mean. Five frames move by a fraction of a pixel and turn a little.
struct Aliasing final {
    double rms = 0.0;     // spatial: root mean square deviation from the mean (aliased energy)
    double shimmer = 0.0; // temporal: root mean square change of a pixel between frames
};
[[nodiscard]] Aliasing measureAliasing(const GpuAffineFilter filter) {
    constexpr std::int64_t kSide = 512;
    const auto source = makeSource(window(0, 0, kSide, kSide), [](const auto x, const auto y) {
        const bool on = ((x / 2) + (y / 2)) % 2 == 0;
        return on ? Pixel{1.0, 1.0, 1.0, 1.0} : Pixel{0.0, 0.0, 0.0, 1.0};
    });
    const auto deepest = bloom::render::affineMipDeepestLevel(kSide, kSide);
    const auto levels = productionLevels(source.image, deepest);
    // The interior of the shrunk image: well inside every frame's footprint of the source.
    const auto interior = window(44, 40, 24, 24);
    std::vector<std::vector<double>> frames;
    for (int frame = 0; frame < 5; ++frame) {
        const auto matrix =
            shrink(0.125, 0.125, 3.0 + 0.7 * frame, 0.0, 32.0 + 0.37 * frame, 28.0 + 0.23 * frame);
        const auto lattice = coverOf(matrix, kSide, kSide);
        const auto plan = bloom::render::planAffineFilter(matrix, filter, kSide, kSide);
        std::vector<double> luminance;
        for (std::int64_t y = interior.originY(); y < interior.maxYExclusive(); ++y) {
            std::vector<Rgba32f> row(static_cast<std::size_t>(interior.extent().width()),
                                     Rgba32f::transparent());
            const auto status =
                plan.filtered()
                    ? bloom::render::affineFilteredResampleRow(
                          viewOf(plan.lowerLevel == 0 ? source.image : levels[plan.lowerLevel - 1]),
                          viewOf(plan.upperLevel == 0 ? source.image : levels[plan.upperLevel - 1]),
                          matrix, plan, 1.0F, lattice, interior, y, row)
                    : bloom::render::affineMatrixResampleRow(viewOf(source.image), matrix, 1.0F,
                                                             plan.filter, lattice, interior, y,
                                                             row);
            if (status)
                throw std::logic_error("aliasing row resamples");
            for (const auto& pixel : row)
                luminance.push_back(static_cast<double>(pixel.red()));
        }
        frames.push_back(std::move(luminance));
    }
    Aliasing result;
    double spatial = 0.0;
    double temporal = 0.0;
    std::size_t count = 0;
    for (std::size_t f = 0; f < frames.size(); ++f)
        for (std::size_t i = 0; i < frames[f].size(); ++i) {
            const double deviation = frames[f][i] - 0.5;
            spatial += deviation * deviation;
            if (f > 0) {
                const double change = frames[f][i] - frames[f - 1][i];
                temporal += change * change;
            }
            ++count;
        }
    result.rms = std::sqrt(spatial / static_cast<double>(count));
    result.shimmer = std::sqrt(temporal / static_cast<double>(count - frames.front().size()));
    return result;
}

void testAliasing() {
    const auto bilinear = measureAliasing(GpuAffineFilter::Bilinear);
    const auto nearest = measureAliasing(GpuAffineFilter::Nearest);
    const auto trilinear = measureAliasing(GpuAffineFilter::Trilinear);
    const auto anisotropic = measureAliasing(GpuAffineFilter::Anisotropic);
    std::cout << "aliased energy (RMS deviation from the band-limited mean / frame-to-frame RMS): "
              << "Bilinear " << bilinear.rms << " / " << bilinear.shimmer << ", Nearest "
              << nearest.rms << " / " << nearest.shimmer << ", Trilinear " << trilinear.rms << " / "
              << trilinear.shimmer << ", Anisotropic " << anisotropic.rms << " / "
              << anisotropic.shimmer << '\n';
    expect(bilinear.rms > 0.1 && bilinear.shimmer > 0.1,
           "Bilinear keeps aliased energy above Nyquist and it flickers between frames");
    expect(nearest.rms > 0.4 && nearest.shimmer > 0.4,
           "Nearest keeps the checker's full contrast as aliasing");
    expect(trilinear.rms < 1e-5 && trilinear.shimmer < 1e-5,
           "Trilinear removes the aliased energy and the flicker");
    expect(anisotropic.rms < 1e-5 && anisotropic.shimmer < 1e-5,
           "Anisotropic removes the aliased energy and the flicker");
}

} // namespace

int main() {
    try {
        testLevels();
        testPlans();
        testRows();
        testAliasing();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " affine filter expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: affine filter CPU reference\n";
    return 0;
}
