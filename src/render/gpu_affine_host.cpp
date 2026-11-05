#include <bloom/render/gpu_affine.hpp>

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
} // namespace

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

} // namespace bloom::render
