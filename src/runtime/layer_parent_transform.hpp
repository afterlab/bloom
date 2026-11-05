#pragma once

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
                                              document::Vec2d scale, double degrees) {
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
        const auto moved = result.map(pivot);
        result.x = position.x - moved.x;
        result.y = position.y - moved.y;
        return result;
    }
};

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
        : matrix_(matrix), window_(window), sx_(sx), sy_(sy), opacity_(opacity),
          determinant_(matrix.a * matrix.d - matrix.b * matrix.c), nearest_(nearest) {}
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
    [[nodiscard]] std::optional<render::ImageWindow> supportBounds(render::ImageWindow clip) const {
        const double w = window_.extent().width(), h = window_.extent().height();
        const std::array points{forwardMap(-1, -1), forwardMap(w, -1), forwardMap(w, h),
                                forwardMap(-1, h)};
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
    // The device-space composed matrix AffineBilinearV1 receives for this placement.
    [[nodiscard]] render::GpuAffineMatrix deviceMatrix() const {
        return toGpuAffineMatrix(matrix_, window_, sx_, sy_);
    }
    // One output row. #545: the sample coordinate is AffineBilinearV1's own double-float
    // reconstruction over `latticeWindow` (render::affineMatrixResampleRow), so this row and the
    // GPU command agree bit for bit. `latticeWindow` is the window the GPU command covers: the
    // layer's unclipped support. A region-of-interest clip passes its smaller `outputWindow` and
    // gets the same pixels the full window has there.
    [[nodiscard]] render::ImageStatus row(render::Rgba32fImageView source,
                                          render::ImageWindow latticeWindow,
                                          render::ImageWindow outputWindow, std::int64_t outputY,
                                          std::span<render::Rgba32f> output) const {
        return render::affineMatrixResampleRow(
            source, deviceMatrix(), opacity_,
            nearest_ ? render::GpuAffineFilter::Nearest : render::GpuAffineFilter::Bilinear,
            latticeWindow, outputWindow, outputY, output);
    }
    [[nodiscard]] render::ImageStatus row(render::Rgba32fImageView source,
                                          render::ImageWindow outputWindow, std::int64_t outputY,
                                          std::span<render::Rgba32f> output) const {
        return row(source, outputWindow, outputWindow, outputY, output);
    }

  private:
    LayerMatrix matrix_;
    render::ImageWindow window_;
    double sx_, sy_;
    float opacity_;
    double determinant_;
    bool nearest_ = false;
};
} // namespace bloom::runtime::detail
