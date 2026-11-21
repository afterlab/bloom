#pragma once

#include "affine_mip_levels.hpp"
#include <algorithm>
#include <array>
#include <bloom/document/parameter.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_affine.hpp>
#include <cmath>
#include <numbers>

namespace bloom::runtime::detail {
// Author-space affine matrix. Unlike a rotation/scale decomposition this retains shear produced
// by rotated, nonuniformly scaled ancestors. Opacity is deliberately not inherited.
struct LayerMatrix {
    double a = 1, b = 0, c = 0, d = 1, x = 0, y = 0;
    [[nodiscard]] document::Vec2d map(document::Vec2d p) const {
        return {a * p.x + b * p.y + x, c * p.x + d * p.y + y};
    }
    [[nodiscard]] LayerMatrix times(const LayerMatrix& q) const {
        const auto offset = map({q.x, q.y});
        return {a * q.a + b * q.c, a * q.b + b * q.d, c * q.a + d * q.c,
                c * q.b + d * q.d, offset.x,          offset.y};
    }
    [[nodiscard]] static LayerMatrix authored(document::Vec2d position, document::Vec2d pivot,
                                              document::Vec2d scale, double degrees,
                                              double skewDegrees = 0.0,
                                              double skewAxisDegrees = 0.0) {
        const double angle = std::remainder(degrees, 360.0);
        double cosine = std::cos(angle * std::numbers::pi / 180.0);
        double sine = std::sin(angle * std::numbers::pi / 180.0);
        if (angle == 0) {
            cosine = 1;
            sine = 0;
        } else if (angle == 90) {
            cosine = 0;
            sine = 1;
        } else if (angle == -90) {
            cosine = 0;
            sine = -1;
        } else if (std::abs(angle) == 180) {
            cosine = -1;
            sine = 0;
        }
        LayerMatrix result{
            cosine * scale.x, -sine * scale.y, sine * scale.x, cosine * scale.y, 0, 0};
        if (skewDegrees != 0.0) {
            // R * S * K, K the shear by tan(skew) along the axis (the same map as
            // render::LayerTransform): K = [[1 - t s c, t c^2], [-t s^2, 1 + t s c]].
            const double axis = std::remainder(skewAxisDegrees, 360.0);
            double axisCosine = std::cos(axis * std::numbers::pi / 180.0);
            double axisSine = std::sin(axis * std::numbers::pi / 180.0);
            if (axis == 0 || std::abs(axis) == 180) {
                axisCosine = axis == 0 ? 1 : -1;
                axisSine = 0;
            } else if (std::abs(axis) == 90) {
                axisCosine = 0;
                axisSine = axis > 0 ? 1 : -1;
            }
            // The same snap to a whole tangent as render::LayerTransform.
            const double rawTangent = std::tan(skewDegrees * std::numbers::pi / 180.0);
            const double t = std::abs(rawTangent - std::round(rawTangent)) < 1.0e-12
                                 ? std::round(rawTangent)
                                 : rawTangent;
            const double k11 = 1.0 - t * axisSine * axisCosine;
            const double k12 = t * axisCosine * axisCosine;
            const double k21 = -t * axisSine * axisSine;
            const double k22 = 1.0 + t * axisSine * axisCosine;
            const double m11 = scale.x * k11, m12 = scale.x * k12;
            const double m21 = scale.y * k21, m22 = scale.y * k22;
            result = {cosine * m11 - sine * m21, cosine * m12 - sine * m22,
                      sine * m11 + cosine * m21, sine * m12 + cosine * m22, 0, 0};
        }
        const auto moved = result.map(pivot);
        result.x = position.x - moved.x;
        result.y = position.y - moved.y;
        return result;
    }
    // The matrix a layer resamples its own content through: authored(), with the footage pixel
    // aspect on its x scale (#381) and, #642, the content drawn `contentScale` times larger about
    // the origin mapped back (pivot times contentScale, scale divided by it). At a pixel aspect and
    // a content scale of 1 it is authored() exactly.
    [[nodiscard]] static LayerMatrix content(document::Vec2d position, document::Vec2d pivot,
                                             document::Vec2d scale, double degrees,
                                             const double pixelAspect, const double contentScale) {
        return authored(position, {pivot.x * contentScale, pivot.y * contentScale},
                        {scale.x * pixelAspect / contentScale, scale.y / contentScale}, degrees);
    }
    // #642: this matrix followed by a scale of `factor` about the origin (a layer drawing into a
    // picture `factor` times larger, CompiledLayerOutput::outputScale). Exact for a power of two;
    // the matrix itself at a factor of 1.
    [[nodiscard]] LayerMatrix enlarged(const double factor) const {
        if (factor == 1.0)
            return *this;
        return {a * factor, b * factor, c * factor, d * factor, x * factor, y * factor};
    }
};

// #743 pixel snap: `matrix` with its translation rounded to whole pixels of the evaluation at scale
// (sx, sy), halves up: the device translation sx x (sy y) of the author origin becomes the nearest
// whole number, so a layer that only moves copies its pixels exactly and a moving one steps by whole
// pixels (no frame resamples it by a fraction). The linear part is unchanged. Both routes snap the
// same binary64 matrix, so they agree.
[[nodiscard]] inline LayerMatrix snapLayerMatrix(LayerMatrix matrix, const double sx,
                                                 const double sy) {
    if (!(sx > 0.0) || !(sy > 0.0))
        return matrix;
    const double x = std::floor(matrix.x * sx + 0.5);
    const double y = std::floor(matrix.y * sy + 0.5);
    if (std::isfinite(x) && std::isfinite(y)) {
        matrix.x = x / sx;
        matrix.y = y / sy;
    }
    return matrix;
}

// The exact conversion from the runtime's AUTHOR-space LayerMatrix (pixel-centre + 0.5 convention)
// to GpuAffine's source-local-0-based-pixel-centre to absolute-output-device-index convention,
// folding the proxy scales exactly as ParentedLayerTransform::forwardMap does:
//   X = a*lx + (sx/sy)*b*ly + a*ox + (sx/sy)*b*oy + sx*m.x - 0.5
//   Y = (sy/sx)*c*lx + d*ly + (sy/sx)*c*ox + d*oy + sy*m.y - 0.5
// where (ox, oy) is the source data-window origin and (lx, ly) a 0-based source pixel centre. At
// sx == sy == 1 this is the accepted native affine conversion. The GPU command and the CPU row
// (ParentedLayerTransform::row) both resample through this matrix.
[[nodiscard]] inline render::GpuAffineMatrix
toGpuAffineMatrix(const LayerMatrix& matrix, const render::ImageWindow sourceWindow,
                  const double scaleX, const double scaleY) {
    const double ox = static_cast<double>(sourceWindow.originX()) + 0.5;
    const double oy = static_cast<double>(sourceWindow.originY()) + 0.5;
    const double b = (scaleX / scaleY) * matrix.b;
    const double c = (scaleY / scaleX) * matrix.c;
    return render::GpuAffineMatrix{.a = matrix.a,
                                   .b = b,
                                   .tx = matrix.a * ox + b * oy + scaleX * matrix.x - 0.5,
                                   .c = c,
                                   .d = matrix.d,
                                   .ty = c * ox + matrix.d * oy + scaleY * matrix.y - 0.5};
}

// Single resample through a composed author-space matrix, with the same pixel-edge/proxy convention
// as LayerTransform: parented and concatenated layers, an unparented layer that does more than
// move (#545), Transform 2D and Resize/Fit. A pure move keeps LayerTransform's translate-only row.
// `nearest` (#480 Resize,
// Fit and Transform 2D Nearest) copies the one tap at floor + (factor >= 0.5) on each axis instead
// of interpolating, the CPU reference of AffineBilinearV1's nearest sampling.
class ParentedLayerTransform {
  public:
    ParentedLayerTransform(LayerMatrix matrix, render::ImageWindow window, double sx, double sy,
                           float opacity, bool nearest = false)
        : ParentedLayerTransform(matrix, window, sx, sy, opacity,
                                 nearest ? render::GpuAffineFilter::Nearest
                                         : render::GpuAffineFilter::Bilinear) {}
    // #742: any filter. Trilinear and Anisotropic resolve through render::planAffineFilter() of
    // the device matrix, exactly as the GPU command's emission does.
    ParentedLayerTransform(LayerMatrix matrix, render::ImageWindow window, double sx, double sy,
                           float opacity, render::GpuAffineFilter filter)
        : matrix_(matrix), window_(window), sx_(sx), sy_(sy), opacity_(opacity),
          determinant_(matrix.a * matrix.d - matrix.b * matrix.c),
          plan_(render::planAffineFilter(toGpuAffineMatrix(matrix, window, sx, sy), filter,
                                         window.extent().width(), window.extent().height())) {}
    // The resolved sampling: Bilinear or Nearest, or a filtered plan reading mip levels.
    [[nodiscard]] const render::GpuAffineFilterPlan& filterPlan() const noexcept { return plan_; }
    [[nodiscard]] bool finite() const {
        return std::isfinite(determinant_) && std::isfinite(matrix_.a) &&
               std::isfinite(matrix_.b) && std::isfinite(matrix_.c) && std::isfinite(matrix_.d) &&
               std::isfinite(matrix_.x) && std::isfinite(matrix_.y);
    }
    [[nodiscard]] bool invertible() const {
        return determinant_ != 0 && std::isfinite(determinant_);
    }
    [[nodiscard]] render::LayerTransform::SamplePoint forwardMap(double x, double y) const {
        const auto p = matrix_.map({(x + static_cast<double>(window_.originX()) + 0.5) / sx_,
                                    (y + static_cast<double>(window_.originY()) + 0.5) / sy_});
        return {p.x * sx_ - 0.5, p.y * sy_ - 0.5};
    }
    // The output pixels the resample can write, clipped to `clip`: the forward image of the
    // support box [-m, extent - 1 + m] source pixels, m the plan's filterSupportMargin() (1 for
    // Bilinear and Nearest, wider for a filtered plan reading a mip level, #742).
    [[nodiscard]] std::optional<render::ImageWindow> supportBounds(render::ImageWindow clip) const;
    // The device-space composed matrix AffineBilinearV1 receives for this placement.
    [[nodiscard]] render::GpuAffineMatrix deviceMatrix() const {
        return toGpuAffineMatrix(matrix_, window_, sx_, sy_);
    }
    // One output row. #545: the sample coordinate is AffineBilinearV1's own double-float
    // reconstruction over `latticeWindow` (render::affineMatrixResampleRow), so this row and the
    // GPU command agree bit for bit. `latticeWindow` is the window the GPU command covers: the
    // layer's unclipped support. A region-of-interest clip passes its smaller `outputWindow` and
    // gets the same pixels the full window has there.
    // A filtered plan (#742) reads `levels`, the mip levels of `source` built for filterPlan()
    // (AffineMipLevels); it is refused without them.
    [[nodiscard]] render::ImageStatus row(render::Rgba32fImageView source,
                                          render::ImageWindow latticeWindow,
                                          render::ImageWindow outputWindow, std::int64_t outputY,
                                          std::span<render::Rgba32f> output,
                                          const AffineMipLevels* levels = nullptr) const {
        if (plan_.filtered()) {
            const auto lower = levels ? levels->level(plan_.lowerLevel) : std::nullopt;
            const auto upper = levels ? levels->level(plan_.upperLevel) : std::nullopt;
            if (!lower || !upper)
                return render::ImageError::codeOnly(render::ImageErrorCode::InvalidState);
            return render::affineFilteredResampleRow(*lower, *upper, deviceMatrix(), plan_,
                                                     opacity_, latticeWindow, outputWindow, outputY,
                                                     output);
        }
        return render::affineMatrixResampleRow(source, deviceMatrix(), opacity_, plan_.filter,
                                               latticeWindow, outputWindow, outputY, output);
    }
    [[nodiscard]] render::ImageStatus row(render::Rgba32fImageView source,
                                          render::ImageWindow outputWindow, std::int64_t outputY,
                                          std::span<render::Rgba32f> output,
                                          const AffineMipLevels* levels = nullptr) const {
        return row(source, outputWindow, outputWindow, outputY, output, levels);
    }

  private:
    LayerMatrix matrix_;
    render::ImageWindow window_;
    double sx_, sy_;
    float opacity_;
    double determinant_;
    render::GpuAffineFilterPlan plan_{};
};

// #742: how far beyond a source window's edge, in level-0 source pixels, a resample through `plan`
// reads a non-zero texel: 1 for Bilinear and Nearest (the box [-1, extent]); for a filtered plan,
// level n's bilinear support reaches 1.5 * 2^n + 0.5 level-0 pixels past the edge (its texels
// cover 2^n pixels, its extent rounds up), bounded by 2^(n + 1) for the deepest level read, plus
// the farthest Anisotropic tap. Every route sizes a filtered layer's window by it, so the
// filtered fringe of an edge is never clipped.
struct FilterSupportMargin final {
    double x = 1.0;
    double y = 1.0;
};
[[nodiscard]] inline FilterSupportMargin
filterSupportMargin(const render::GpuAffineFilterPlan& plan) noexcept {
    if (!plan.filtered())
        return {};
    const double level = std::ldexp(1.0, static_cast<int>(plan.upperLevel) + 1);
    return {level + std::abs(plan.tapStartX), level + std::abs(plan.tapStartY)};
}

inline std::optional<render::ImageWindow>
ParentedLayerTransform::supportBounds(const render::ImageWindow clip) const {
    const auto margin = filterSupportMargin(plan_);
    const double w = static_cast<double>(window_.extent().width()) - 1.0 + margin.x;
    const double h = static_cast<double>(window_.extent().height()) - 1.0 + margin.y;
    const std::array points{forwardMap(-margin.x, -margin.y), forwardMap(w, -margin.y),
                            forwardMap(w, h), forwardMap(-margin.x, h)};
    double left = points[0].x, right = left, top = points[0].y, bottom = top;
    for (const auto p : points) {
        left = std::min(left, p.x);
        right = std::max(right, p.x);
        top = std::min(top, p.y);
        bottom = std::max(bottom, p.y);
    }
    const auto x0 = std::max(clip.originX(), static_cast<std::int64_t>(std::floor(left)));
    const auto y0 = std::max(clip.originY(), static_cast<std::int64_t>(std::floor(top)));
    const auto x1 =
        std::min(clip.maxXExclusive() - 1, static_cast<std::int64_t>(std::ceil(right)));
    const auto y1 =
        std::min(clip.maxYExclusive() - 1, static_cast<std::int64_t>(std::ceil(bottom)));
    if (x1 < x0 || y1 < y0)
        return std::nullopt;
    const auto result =
        render::ImageWindow::create(x0, y0, static_cast<std::uint32_t>(x1 - x0 + 1),
                                    static_cast<std::uint32_t>(y1 - y0 + 1));
    return result ? std::optional{*result.value()} : std::nullopt;
}

// #742: the compiled resample filter (kResampleFilter*) as the affine command's filter.
[[nodiscard]] inline render::GpuAffineFilter gpuAffineFilterOf(const std::int64_t filter) noexcept {
    switch (filter) {
    case 1:
        return render::GpuAffineFilter::Nearest;
    case 2:
        return render::GpuAffineFilter::Trilinear;
    case 3:
        return render::GpuAffineFilter::Anisotropic;
    default:
        return render::GpuAffineFilter::Bilinear;
    }
}
} // namespace bloom::runtime::detail
