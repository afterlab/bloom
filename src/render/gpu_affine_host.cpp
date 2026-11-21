#include <bloom/render/gpu_affine.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// Always-compiled host half of AffineBilinearV1: the exact Float64 inverse-map preparation, the
// input factories, and the CPU reference of the composed-matrix form. It has no Vulkan dependency,
// so it is shared byte-for-byte by the Vulkan and CPU-unavailable stub translation units and cannot
// drift between them. The kernel only consumes the coordinate metadata; the CPU reference row below
// is the one place that resamples RGBA here, and it is never uploaded as a GPU result.

namespace bloom::render {
namespace {

// The exact CPU reduction of one inverse-mapped sample (layerTransformBilinearRow and the parented
// row share it): transparent when the sample is not strictly inside (-1, extent) on either axis,
// else floor and a single Float32 rounding of the fraction. The non-finite guard is redundant for
// a finite LayerTransform but keeps a hostile composed matrix from reaching an out-of-range
// int64->int32 conversion.
[[nodiscard]] GpuAffineSample reduceAffineSample(const double x, const double y,
                                                 const std::uint32_t sourceWidth,
                                                 const std::uint32_t sourceHeight) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x <= -1.0 || y <= -1.0 ||
        x >= static_cast<double>(sourceWidth) || y >= static_cast<double>(sourceHeight)) {
        return GpuAffineSample{kGpuAffineTransparentBase, 0, 0.0F, 0.0F};
    }
    const auto baseX = static_cast<std::int64_t>(std::floor(x));
    const auto baseY = static_cast<std::int64_t>(std::floor(y));
    return GpuAffineSample{static_cast<std::int32_t>(baseX), static_cast<std::int32_t>(baseY),
                           static_cast<float>(x - static_cast<double>(baseX)),
                           static_cast<float>(y - static_cast<double>(baseY))};
}


// #545: the kernel's error-free double-float arithmetic (affine_bilinear.comp), one IEEE binary32
// operation per statement so no compiler can contract or reassociate it (bloom_render also builds
// with -ffp-contract=off / /fp:strict). Every function below mirrors the shader function of the
// same name step for step; the shader marks each of these operations `precise`.
struct DoubleFloat final {
    float hi = 0.0F;
    float lo = 0.0F;
};

[[nodiscard]] DoubleFloat dfAdd(const DoubleFloat a, const DoubleFloat b) noexcept {
    const float sum = a.hi + b.hi;
    const float virtualB = sum - a.hi;
    const float virtualA = sum - virtualB;
    const float errorA = a.hi - virtualA;
    const float errorB = b.hi - virtualB;
    const float twoSumError = errorA + errorB;
    const float lows = a.lo + b.lo;
    const float error = twoSumError + lows;
    const float total = sum + error;
    const float totalLessError = total - error;
    const float sumResidual = sum - totalLessError;
    const float totalLessSum = total - sum;
    const float errorResidual = error - totalLessSum;
    return DoubleFloat{total, sumResidual + errorResidual};
}

[[nodiscard]] DoubleFloat dfMul(const DoubleFloat a, const DoubleFloat b) noexcept {
    const float product = a.hi * b.hi;
    const float productError = std::fma(a.hi, b.hi, -product);
    const float crossA = a.hi * b.lo;
    const float crossB = a.lo * b.hi;
    const float cross = crossA + crossB;
    const float error = productError + cross;
    const float total = product + error;
    const float totalLessError = total - error;
    const float productResidual = product - totalLessError;
    const float totalLessProduct = total - product;
    const float errorResidual = error - totalLessProduct;
    return DoubleFloat{total, productResidual + errorResidual};
}

[[nodiscard]] int dfCompare(const DoubleFloat a, const DoubleFloat b) noexcept {
    if (a.hi != b.hi) {
        return a.hi < b.hi ? -1 : 1;
    }
    if (a.lo != b.lo) {
        return a.lo < b.lo ? -1 : 1;
    }
    return 0;
}

// floor(hi + lo) and the single Float32 rounding of the fraction (bloom_df_floor).
void dfFloor(const DoubleFloat value, float& base, float& fraction) noexcept {
    base = std::floor(value.hi);
    float exact = value.hi - base;
    fraction = exact + value.lo;
    if (fraction < 0.0F) {
        base = base - 1.0F;
        exact = value.hi - base;
        fraction = exact + value.lo;
    } else if (fraction >= 1.0F) {
        base = base + 1.0F;
        exact = value.hi - base;
        fraction = exact + value.lo;
    }
}

// bloom_lerp: exact endpoints, otherwise fma(factor, end, (1 - factor) * start).
[[nodiscard]] float lerpComponent(const float start, const float end, const float factor) noexcept {
    if (factor == 0.0F) {
        return start;
    }
    if (factor == 1.0F) {
        return end;
    }
    const float inverse = 1.0F - factor;
    const float scaledStart = inverse * start;
    return std::fma(factor, end, scaledStart);
}

[[nodiscard]] std::array<float, 4> lerpPixel(const std::array<float, 4>& start,
                                             const std::array<float, 4>& end,
                                             const float factor) noexcept {
    return {lerpComponent(start[0], end[0], factor), lerpComponent(start[1], end[1], factor),
            lerpComponent(start[2], end[2], factor), lerpComponent(start[3], end[3], factor)};
}

[[nodiscard]] ImageError hostError(const ImageErrorCode code) noexcept {
    return ImageError::codeOnly(code);
}

// The largest lattice axis whose indices convert to Float32 exactly, as the kernel converts them.
constexpr std::uint32_t kMaxLatticeAxis = 1U << 24U;

[[nodiscard]] DoubleFloat splitDouble(const double value) noexcept {
    const auto high = static_cast<float>(value);
    return DoubleFloat{high, static_cast<float>(value - static_cast<double>(high))};
}

// #742 bloom_level_sample: the bilinear sample of one mip level at the level-0 coordinate (x, y).
// The level coordinate is (x + 0.5) * scale - 0.5 in double-float (exact for a power-of-two
// scale; skipped at level 0); then the kernel's range test, floor/factor reduction and bilinear
// gather with transparent black outside the level.
[[nodiscard]] std::array<float, 4> levelSample(const std::span<const Rgba32f> pixels,
                                               const std::uint32_t width,
                                               const std::uint32_t height, const float scale,
                                               DoubleFloat x, DoubleFloat y) noexcept {
    if (scale != 1.0F) {
        const DoubleFloat half{0.5F, 0.0F};
        const DoubleFloat minusHalf{-0.5F, 0.0F};
        const DoubleFloat factor{scale, 0.0F};
        x = dfAdd(dfMul(dfAdd(x, half), factor), minusHalf);
        y = dfAdd(dfMul(dfAdd(y, half), factor), minusHalf);
    }
    const DoubleFloat minusOne{-1.0F, 0.0F};
    if (dfCompare(x, minusOne) <= 0 ||
        dfCompare(x, DoubleFloat{static_cast<float>(width), 0.0F}) >= 0 ||
        dfCompare(y, minusOne) <= 0 ||
        dfCompare(y, DoubleFloat{static_cast<float>(height), 0.0F}) >= 0) {
        return {};
    }
    float baseX = 0.0F;
    float baseY = 0.0F;
    float factorX = 0.0F;
    float factorY = 0.0F;
    dfFloor(x, baseX, factorX);
    dfFloor(y, baseY, factorY);
    const auto bx = static_cast<std::int64_t>(baseX);
    const auto by = static_cast<std::int64_t>(baseY);
    const auto tap = [&](const std::int64_t tx, const std::int64_t ty) noexcept {
        if (tx < 0 || ty < 0 || tx >= width || ty >= height) {
            return std::array<float, 4>{};
        }
        const auto& pixel =
            pixels[static_cast<std::size_t>(ty) * width + static_cast<std::size_t>(tx)];
        return std::array<float, 4>{pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
    };
    const auto top = lerpPixel(tap(bx, by), tap(bx + 1, by), factorX);
    const auto bottom = lerpPixel(tap(bx, by + 1), tap(bx + 1, by + 1), factorX);
    return lerpPixel(top, bottom, factorY);
}
} // namespace

GpuAffineMatrix affineMipReductionMatrix() noexcept {
    // Output pixel i of the next level maps back to source coordinate 2i + 0.5: exactly between
    // taps 2i and 2i + 1 on each axis, so the bilinear factor is exactly 0.5 and the sample is the
    // 2x2 box average of the premultiplied taps.
    return GpuAffineMatrix{.a = 0.5, .b = 0.0, .tx = -0.25, .c = 0.0, .d = 0.5, .ty = -0.25};
}

std::pair<std::uint32_t, std::uint32_t> affineMipLevelExtent(std::uint32_t width,
                                                             std::uint32_t height,
                                                             const std::uint32_t level) noexcept {
    for (std::uint32_t index = 0; index < level && (width > 1 || height > 1); ++index) {
        width = width / 2 + width % 2;
        height = height / 2 + height % 2;
    }
    return {width, height};
}

std::uint32_t affineMipDeepestLevel(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t level = 0;
    while (width > 1 || height > 1) {
        width = width / 2 + width % 2;
        height = height / 2 + height % 2;
        ++level;
    }
    return level;
}

GpuAffineFilterPlan planAffineFilter(const GpuAffineMatrix& matrix, const GpuAffineFilter filter,
                                     const std::uint32_t sourceWidth,
                                     const std::uint32_t sourceHeight) noexcept {
    GpuAffineFilterPlan plan;
    plan.sourceWidth = sourceWidth;
    plan.sourceHeight = sourceHeight;
    plan.filter =
        filter == GpuAffineFilter::Nearest ? GpuAffineFilter::Nearest : GpuAffineFilter::Bilinear;
    if (filter != GpuAffineFilter::Trilinear && filter != GpuAffineFilter::Anisotropic) {
        return plan;
    }
    // One correctly rounded IEEE operation per statement (the build also turns contraction off),
    // so the plan is the same on every platform.
    const double ad = matrix.a * matrix.d;
    const double bc = matrix.b * matrix.c;
    const double determinant = ad - bc;
    if (!std::isfinite(determinant) || determinant == 0.0) {
        return plan;
    }
    // The inverse map's Jacobian: source pixels per output column (j00, j10) and row (j01, j11).
    const double j00 = matrix.d / determinant;
    const double j01 = -matrix.b / determinant;
    const double j10 = -matrix.c / determinant;
    const double j11 = matrix.a / determinant;
    return planAffineFilterFromJacobian(j00, j01, j10, j11, filter, sourceWidth, sourceHeight);
}

GpuAffineFilterPlan planAffineFilterFromJacobian(const double j00, const double j01,
                                                 const double j10, const double j11,
                                                 const GpuAffineFilter filter,
                                                 const std::uint32_t sourceWidth,
                                                 const std::uint32_t sourceHeight) noexcept {
    GpuAffineFilterPlan plan;
    plan.sourceWidth = sourceWidth;
    plan.sourceHeight = sourceHeight;
    plan.filter =
        filter == GpuAffineFilter::Nearest ? GpuAffineFilter::Nearest : GpuAffineFilter::Bilinear;
    if ((filter != GpuAffineFilter::Trilinear && filter != GpuAffineFilter::Anisotropic) ||
        sourceWidth == 0 || sourceHeight == 0) {
        return plan;
    }
    if (!std::isfinite(j00) || !std::isfinite(j01) || !std::isfinite(j10) || !std::isfinite(j11)) {
        return plan;
    }
    // E = J J^T, whose eigenvalues are the squared singular values of J.
    const double j00s = j00 * j00;
    const double j01s = j01 * j01;
    const double j10s = j10 * j10;
    const double j11s = j11 * j11;
    const double p = j00s + j01s;
    const double r = j10s + j11s;
    const double q0 = j00 * j10;
    const double q1 = j01 * j11;
    const double q = q0 + q1;
    const double sum = p + r;
    const double mean = sum * 0.5;
    const double difference = p - r;
    const double half = difference * 0.5;
    const double halfSquared = half * half;
    const double qSquared = q * q;
    const double radicand = halfSquared + qSquared;
    const double root = std::sqrt(radicand);
    const double major = mean + root;
    const double d0 = j00 * j11;
    const double d1 = j01 * j10;
    const double jacobianDeterminant = d0 - d1;
    const double jacobianSquared = jacobianDeterminant * jacobianDeterminant;
    const double minor = jacobianSquared / major;
    const double sigmaMajor = std::sqrt(major);
    const double sigmaMinor = std::sqrt(minor);
    if (!std::isfinite(sigmaMajor) || !std::isfinite(sigmaMinor) || !(sigmaMajor > 1.0)) {
        // Nothing shrinks: the filtered sampler is Bilinear exactly.
        return plan;
    }
    std::uint32_t taps = 1;
    double footprint = sigmaMajor;
    double stepX = 0.0;
    double stepY = 0.0;
    if (filter == GpuAffineFilter::Anisotropic) {
        const double ratio = sigmaMinor > 0.0 ? sigmaMajor / sigmaMinor : 0.0;
        const double count =
            sigmaMinor > 0.0 && std::isfinite(ratio)
                ? std::min(std::ceil(ratio), static_cast<double>(kGpuAffineMaxAnisotropicTaps))
                : static_cast<double>(kGpuAffineMaxAnisotropicTaps);
        taps = static_cast<std::uint32_t>(count);
        footprint = sigmaMajor / count;
        if (taps > 1) {
            // The major axis: the eigenvector of E for the larger eigenvalue, the better
            // conditioned of its two forms.
            double vx = 1.0;
            double vy = 0.0;
            if (q != 0.0) {
                const double ax = q;
                const double ay = major - p;
                const double bx = major - r;
                const double by = q;
                const double an0 = ax * ax;
                const double an1 = ay * ay;
                const double an = an0 + an1;
                const double bn0 = bx * bx;
                const double bn1 = by * by;
                const double bn = bn0 + bn1;
                vx = an >= bn ? ax : bx;
                vy = an >= bn ? ay : by;
            } else if (r > p) {
                vx = 0.0;
                vy = 1.0;
            }
            const double vxx = vx * vx;
            const double vyy = vy * vy;
            const double vn = vxx + vyy;
            const double length = std::sqrt(vn);
            const double unitX = vx / length;
            const double unitY = vy / length;
            const double spacing = sigmaMajor / count;
            stepX = unitX * spacing;
            stepY = unitY * spacing;
        }
    }
    std::uint32_t lower = 0;
    double weight = 0.0;
    if (footprint > 1.0) {
        int exponent = 0;
        const double mantissa = std::frexp(footprint, &exponent); // footprint = m 2^e, m in [.5, 1)
        lower = static_cast<std::uint32_t>(exponent - 1);
        const double twice = mantissa * 2.0;
        weight = twice - 1.0;
    }
    const std::uint32_t deepest = affineMipDeepestLevel(sourceWidth, sourceHeight);
    std::uint32_t upper = lower + 1;
    if (lower >= deepest) {
        lower = deepest;
        upper = deepest;
        weight = 0.0;
    }
    if (weight == 0.0) {
        upper = lower;
    }
    plan.filter = filter;
    plan.lowerLevel = lower;
    plan.upperLevel = upper;
    plan.upperWeight = static_cast<float>(weight);
    if (plan.upperWeight == 0.0F) {
        plan.upperLevel = lower;
    }
    plan.taps = taps;
    plan.tapStepX = stepX;
    plan.tapStepY = stepY;
    const double offset = static_cast<double>(taps - 1) * -0.5;
    plan.tapStartX = stepX * offset;
    plan.tapStartY = stepY * offset;
    plan.tapWeight = static_cast<float>(1.0 / static_cast<double>(taps));
    return plan;
}

std::vector<GpuAffineSample> prepareAffineSamples(const LayerTransform& transform,
                                                  const ImageWindow outputWindow) {
    const auto sourceWindow = transform.sourceWindow();
    const std::uint32_t sourceWidth = sourceWindow.extent().width();
    const std::uint32_t sourceHeight = sourceWindow.extent().height();
    const std::uint32_t outputWidth = outputWindow.extent().width();
    const std::uint32_t outputHeight = outputWindow.extent().height();
    std::vector<GpuAffineSample> samples(static_cast<std::size_t>(outputWidth) * outputHeight);
    for (std::uint32_t row = 0; row < outputHeight; ++row) {
        const double absoluteY = static_cast<double>(outputWindow.originY() + row);
        for (std::uint32_t column = 0; column < outputWidth; ++column) {
            const double absoluteX = static_cast<double>(outputWindow.originX() + column);
            const auto sample = transform.inverseMap(absoluteX, absoluteY);
            samples[static_cast<std::size_t>(row) * outputWidth + column] =
                reduceAffineSample(sample.x, sample.y, sourceWidth, sourceHeight);
        }
    }
    return samples;
}

std::vector<GpuAffineSample> prepareAffineMatrixSamples(const GpuAffineMatrix& matrix,
                                                        const ImageWindow sourceWindow,
                                                        const ImageWindow outputWindow) {
    const std::uint32_t sourceWidth = sourceWindow.extent().width();
    const std::uint32_t sourceHeight = sourceWindow.extent().height();
    const std::uint32_t outputWidth = outputWindow.extent().width();
    const std::uint32_t outputHeight = outputWindow.extent().height();
    std::vector<GpuAffineSample> samples(static_cast<std::size_t>(outputWidth) * outputHeight);
    const double determinant = matrix.a * matrix.d - matrix.b * matrix.c;
    if (!std::isfinite(determinant) || determinant == 0.0) {
        // No inverse: the placement collapses the layer onto a line or point. The CPU treats a
        // collapsed layer as empty (transparent), so every output pixel is the transparent
        // sentinel and no device work is needed.
        for (auto& sample : samples) {
            sample = GpuAffineSample{kGpuAffineTransparentBase, 0, 0.0F, 0.0F};
        }
        return samples;
    }
    for (std::uint32_t row = 0; row < outputHeight; ++row) {
        const double absoluteY = static_cast<double>(outputWindow.originY() + row);
        for (std::uint32_t column = 0; column < outputWidth; ++column) {
            const double absoluteX = static_cast<double>(outputWindow.originX() + column);
            const double relativeX = absoluteX - matrix.tx;
            const double relativeY = absoluteY - matrix.ty;
            const double localX = (matrix.d * relativeX - matrix.b * relativeY) / determinant;
            const double localY = (-matrix.c * relativeX + matrix.a * relativeY) / determinant;
            samples[static_cast<std::size_t>(row) * outputWidth + column] =
                reduceAffineSample(localX, localY, sourceWidth, sourceHeight);
        }
    }
    return samples;
}

GpuAffineMap prepareAffineMap(const LayerTransform& transform, const ImageWindow outputWindow) {
    // Probe the CPU oracle at the output origin and one local step along each lattice axis to build
    // a compact approximation of the inverse map over the window. The map is mathematically affine,
    // but the probed differences are Float64 subtractions of sampled values, so the reconstructed
    // per-pixel coordinate is NOT bit-identical to the oracle's own inverseMap() evaluation for
    // arbitrary origins and near-cancelling coefficients. Parity is therefore tolerance-qualified
    // (2e-6 abs-or-rel RGB, exact alpha endpoints) and is verified across the full output window by
    // the native gates, never asserted exact by construction.
    const auto originX = static_cast<double>(outputWindow.originX());
    const auto originY = static_cast<double>(outputWindow.originY());
    const auto atOrigin = transform.inverseMap(originX, originY);
    const auto atColumn = transform.inverseMap(originX + 1.0, originY);
    const auto atRow = transform.inverseMap(originX, originY + 1.0);
    GpuAffineMap map;
    map.localXAtOrigin = atOrigin.x;
    map.localYAtOrigin = atOrigin.y;
    map.stepXPerColumn = atColumn.x - atOrigin.x;
    map.stepXPerRow = atRow.x - atOrigin.x;
    map.stepYPerColumn = atColumn.y - atOrigin.y;
    map.stepYPerRow = atRow.y - atOrigin.y;
    return map;
}

GpuAffineMap prepareAffineMatrixMap(const GpuAffineMatrix& matrix, const ImageWindow outputWindow) {
    const double determinant = matrix.a * matrix.d - matrix.b * matrix.c;
    if (!std::isfinite(determinant) || determinant == 0.0) {
        // No inverse: the collapsed layer is the empty (all-transparent) result. A constant local
        // coordinate of -2.0 is strictly outside the CPU's in-range (-1, extent) interval on both
        // axes, so every pixel reduces to the transparent sentinel.
        return GpuAffineMap{.localXAtOrigin = -2.0,
                            .localYAtOrigin = -2.0,
                            .stepXPerColumn = 0.0,
                            .stepXPerRow = 0.0,
                            .stepYPerColumn = 0.0,
                            .stepYPerRow = 0.0};
    }
    const auto point = [&](const double absoluteX, const double absoluteY) {
        const double relativeX = absoluteX - matrix.tx;
        const double relativeY = absoluteY - matrix.ty;
        return LayerTransform::SamplePoint{
            (matrix.d * relativeX - matrix.b * relativeY) / determinant,
            (-matrix.c * relativeX + matrix.a * relativeY) / determinant};
    };
    const auto originX = static_cast<double>(outputWindow.originX());
    const auto originY = static_cast<double>(outputWindow.originY());
    const auto atOrigin = point(originX, originY);
    const auto atColumn = point(originX + 1.0, originY);
    const auto atRow = point(originX, originY + 1.0);
    GpuAffineMap map;
    map.localXAtOrigin = atOrigin.x;
    map.localYAtOrigin = atOrigin.y;
    map.stepXPerColumn = atColumn.x - atOrigin.x;
    map.stepXPerRow = atRow.x - atOrigin.x;
    map.stepYPerColumn = atColumn.y - atOrigin.y;
    map.stepYPerRow = atRow.y - atOrigin.y;
    return map;
}

std::array<float, 12> splitAffineMap(const GpuAffineMap& map) noexcept {
    std::array<float, 12> coefficients{};
    const auto split = [&coefficients](const std::size_t index, const double value) noexcept {
        const auto high = static_cast<float>(value);
        coefficients[index] = high;
        coefficients[index + 1] = static_cast<float>(value - static_cast<double>(high));
    };
    split(0, map.localXAtOrigin);
    split(2, map.localYAtOrigin);
    split(4, map.stepXPerColumn);
    split(6, map.stepXPerRow);
    split(8, map.stepYPerColumn);
    split(10, map.stepYPerRow);
    return coefficients;
}

GpuAffineSample reconstructAffineSample(const std::array<float, 12>& coefficients,
                                        const std::uint32_t column, const std::uint32_t row,
                                        const std::uint32_t sourceWidth,
                                        const std::uint32_t sourceHeight) noexcept {
    const auto coefficient = [&coefficients](const std::size_t index) noexcept {
        return DoubleFloat{coefficients[index * 2], coefficients[index * 2 + 1]};
    };
    const DoubleFloat columnValue{static_cast<float>(column), 0.0F};
    const DoubleFloat rowValue{static_cast<float>(row), 0.0F};
    const auto localX = dfAdd(dfAdd(coefficient(0), dfMul(coefficient(2), columnValue)),
                              dfMul(coefficient(3), rowValue));
    const auto localY = dfAdd(dfAdd(coefficient(1), dfMul(coefficient(4), columnValue)),
                              dfMul(coefficient(5), rowValue));
    const DoubleFloat minusOne{-1.0F, 0.0F};
    if (dfCompare(localX, minusOne) <= 0 ||
        dfCompare(localX, DoubleFloat{static_cast<float>(sourceWidth), 0.0F}) >= 0 ||
        dfCompare(localY, minusOne) <= 0 ||
        dfCompare(localY, DoubleFloat{static_cast<float>(sourceHeight), 0.0F}) >= 0) {
        return GpuAffineSample{kGpuAffineTransparentBase, 0, 0.0F, 0.0F};
    }
    float baseX = 0.0F;
    float baseY = 0.0F;
    float factorX = 0.0F;
    float factorY = 0.0F;
    dfFloor(localX, baseX, factorX);
    dfFloor(localY, baseY, factorY);
    return GpuAffineSample{static_cast<std::int32_t>(baseX), static_cast<std::int32_t>(baseY),
                           factorX, factorY};
}

ImageStatus affineMatrixResampleRow(const Rgba32fImageView source, const GpuAffineMatrix& matrix,
                                    const float opacity, const GpuAffineFilter filter,
                                    const ImageWindow latticeWindow,
                                    const ImageWindow outputWindow, const std::int64_t outputY,
                                    const std::span<Rgba32f> output) noexcept {
    const auto descriptor = source.descriptor();
    if (!descriptor.has_value() || source.pixels().size() != descriptor->layout().pixelCount) {
        return hostError(ImageErrorCode::InvalidState);
    }
    if (outputY < outputWindow.originY() || outputY >= outputWindow.maxYExclusive()) {
        return hostError(ImageErrorCode::CoordinateOutOfBounds);
    }
    if (outputWindow.originX() < latticeWindow.originX() ||
        outputWindow.originY() < latticeWindow.originY() ||
        outputWindow.maxXExclusive() > latticeWindow.maxXExclusive() ||
        outputWindow.maxYExclusive() > latticeWindow.maxYExclusive() ||
        latticeWindow.extent().width() >= kMaxLatticeAxis ||
        latticeWindow.extent().height() >= kMaxLatticeAxis) {
        return hostError(ImageErrorCode::InvalidParameter);
    }
    const auto expectedBytes =
        static_cast<std::size_t>(outputWindow.extent().width()) * sizeof(Rgba32f);
    if (output.size_bytes() != expectedBytes) {
        return ImageError::storageSizeMismatch(output.size_bytes(), expectedBytes);
    }
    if (opacity == 0.0F) {
        for (auto& pixel : output) {
            pixel = Rgba32f::transparent();
        }
        return std::nullopt;
    }

    const auto sourceWidth = descriptor->dataWindow().extent().width();
    const auto sourceHeight = descriptor->dataWindow().extent().height();
    const auto pixels = source.pixels();
    const auto tap = [&](const std::int64_t x, const std::int64_t y) noexcept {
        if (x < 0 || y < 0 || x >= sourceWidth || y >= sourceHeight) {
            return std::array<float, 4>{};
        }
        const auto& pixel =
            pixels[static_cast<std::size_t>(y) * sourceWidth + static_cast<std::size_t>(x)];
        return std::array<float, 4>{pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
    };
    const auto coefficients = splitAffineMap(prepareAffineMatrixMap(matrix, latticeWindow));
    const auto row = static_cast<std::uint32_t>(outputY - latticeWindow.originY());
    const auto firstColumn =
        static_cast<std::uint32_t>(outputWindow.originX() - latticeWindow.originX());
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto sample =
            reconstructAffineSample(coefficients, firstColumn + static_cast<std::uint32_t>(index),
                                    row, sourceWidth, sourceHeight);
        if (sample.baseX == kGpuAffineTransparentBase) {
            output[index] = Rgba32f::transparent();
            continue;
        }
        std::array<float, 4> sampled{};
        if (filter == GpuAffineFilter::Nearest) {
            sampled = tap(sample.baseX + (sample.factorX >= 0.5F ? 1 : 0),
                          sample.baseY + (sample.factorY >= 0.5F ? 1 : 0));
        } else {
            const auto top = lerpPixel(tap(sample.baseX, sample.baseY),
                                       tap(sample.baseX + 1, sample.baseY), sample.factorX);
            const auto bottom = lerpPixel(tap(sample.baseX, sample.baseY + 1),
                                          tap(sample.baseX + 1, sample.baseY + 1), sample.factorX);
            sampled = lerpPixel(top, bottom, sample.factorY);
        }
        if (opacity != 1.0F) {
            for (auto& component : sampled) {
                component = component * opacity;
            }
        }
        const auto pixel =
            Rgba32f::fromPremultiplied(sampled[0], sampled[1], sampled[2], sampled[3]);
        if (!pixel) {
            return *pixel.error();
        }
        output[index] = *pixel.value();
    }
    return std::nullopt;
}

ImageStatus affineFilteredResampleRow(const Rgba32fImageView lower, const Rgba32fImageView upper,
                                      const GpuAffineMatrix& matrix,
                                      const GpuAffineFilterPlan& plan, const float opacity,
                                      const ImageWindow latticeWindow,
                                      const ImageWindow outputWindow, const std::int64_t outputY,
                                      const std::span<Rgba32f> output) noexcept {
    const auto lowerDescriptor = lower.descriptor();
    const auto upperDescriptor = upper.descriptor();
    if (!lowerDescriptor.has_value() || !upperDescriptor.has_value() ||
        lower.pixels().size() != lowerDescriptor->layout().pixelCount ||
        upper.pixels().size() != upperDescriptor->layout().pixelCount) {
        return hostError(ImageErrorCode::InvalidState);
    }
    if (!plan.filtered() || plan.taps == 0 || plan.taps > kGpuAffineMaxAnisotropicTaps ||
        plan.upperLevel < plan.lowerLevel || plan.sourceWidth == 0 || plan.sourceHeight == 0) {
        return hostError(ImageErrorCode::InvalidParameter);
    }
    const auto lowerWidth = lowerDescriptor->dataWindow().extent().width();
    const auto lowerHeight = lowerDescriptor->dataWindow().extent().height();
    const auto upperWidth = upperDescriptor->dataWindow().extent().width();
    const auto upperHeight = upperDescriptor->dataWindow().extent().height();
    if (affineMipLevelExtent(plan.sourceWidth, plan.sourceHeight, plan.lowerLevel) !=
            std::pair{lowerWidth, lowerHeight} ||
        affineMipLevelExtent(plan.sourceWidth, plan.sourceHeight, plan.upperLevel) !=
            std::pair{upperWidth, upperHeight}) {
        return hostError(ImageErrorCode::InvalidParameter);
    }
    if (outputY < outputWindow.originY() || outputY >= outputWindow.maxYExclusive()) {
        return hostError(ImageErrorCode::CoordinateOutOfBounds);
    }
    if (outputWindow.originX() < latticeWindow.originX() ||
        outputWindow.originY() < latticeWindow.originY() ||
        outputWindow.maxXExclusive() > latticeWindow.maxXExclusive() ||
        outputWindow.maxYExclusive() > latticeWindow.maxYExclusive() ||
        latticeWindow.extent().width() >= kMaxLatticeAxis ||
        latticeWindow.extent().height() >= kMaxLatticeAxis) {
        return hostError(ImageErrorCode::InvalidParameter);
    }
    const auto expectedBytes =
        static_cast<std::size_t>(outputWindow.extent().width()) * sizeof(Rgba32f);
    if (output.size_bytes() != expectedBytes) {
        return ImageError::storageSizeMismatch(output.size_bytes(), expectedBytes);
    }
    if (opacity == 0.0F) {
        for (auto& pixel : output) {
            pixel = Rgba32f::transparent();
        }
        return std::nullopt;
    }
    const float lowerScale = std::ldexp(1.0F, -static_cast<int>(plan.lowerLevel));
    const float upperScale = std::ldexp(1.0F, -static_cast<int>(plan.upperLevel));
    const auto startX = splitDouble(plan.tapStartX);
    const auto startY = splitDouble(plan.tapStartY);
    const auto stepX = splitDouble(plan.tapStepX);
    const auto stepY = splitDouble(plan.tapStepY);
    const auto coefficients = splitAffineMap(prepareAffineMatrixMap(matrix, latticeWindow));
    const auto coefficient = [&coefficients](const std::size_t index) noexcept {
        return DoubleFloat{coefficients[index * 2], coefficients[index * 2 + 1]};
    };
    const auto row = static_cast<std::uint32_t>(outputY - latticeWindow.originY());
    const auto firstColumn =
        static_cast<std::uint32_t>(outputWindow.originX() - latticeWindow.originX());
    const DoubleFloat rowValue{static_cast<float>(row), 0.0F};
    for (std::size_t index = 0; index < output.size(); ++index) {
        const DoubleFloat columnValue{
            static_cast<float>(firstColumn + static_cast<std::uint32_t>(index)), 0.0F};
        const auto localX = dfAdd(dfAdd(coefficient(0), dfMul(coefficient(2), columnValue)),
                                  dfMul(coefficient(3), rowValue));
        const auto localY = dfAdd(dfAdd(coefficient(1), dfMul(coefficient(4), columnValue)),
                                  dfMul(coefficient(5), rowValue));
        std::array<float, 4> sum{};
        for (std::uint32_t tap = 0; tap < plan.taps; ++tap) {
            const DoubleFloat tapIndex{static_cast<float>(tap), 0.0F};
            const auto x = dfAdd(dfAdd(localX, startX), dfMul(stepX, tapIndex));
            const auto y = dfAdd(dfAdd(localY, startY), dfMul(stepY, tapIndex));
            auto sample = levelSample(lower.pixels(), lowerWidth, lowerHeight, lowerScale, x, y);
            if (plan.upperWeight != 0.0F) {
                const auto next =
                    levelSample(upper.pixels(), upperWidth, upperHeight, upperScale, x, y);
                sample = lerpPixel(sample, next, plan.upperWeight);
            }
            if (tap == 0) {
                sum = sample;
            } else {
                for (std::size_t component = 0; component < 4; ++component) {
                    sum[component] = sum[component] + sample[component];
                }
            }
        }
        if (plan.taps > 1) {
            for (auto& component : sum) {
                component = component * plan.tapWeight;
            }
        }
        if (opacity != 1.0F) {
            for (auto& component : sum) {
                component = component * opacity;
            }
        }
        const auto pixel = Rgba32f::fromPremultiplied(sum[0], sum[1], sum[2], sum[3]);
        if (!pixel) {
            return *pixel.error();
        }
        output[index] = *pixel.value();
    }
    return std::nullopt;
}

} // namespace bloom::render
