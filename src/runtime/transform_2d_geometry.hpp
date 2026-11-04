#pragma once

// #336 standalone Transform 2D geometry, shared by the CPU reference (image_effect.cpp) and the
// production GPU scene preparation (gpu_scene_transform_2d_emission.ipp) so both routes resample
// through the same checked matrix and output data window.

#include "layer_parent_transform.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace bloom::runtime::detail {

// The values one frame resolves for a Transform2DKernel. `centre` is the full-resolution centre of
// the input's content bounds; the authored pivot is an offset from it, exactly like a Layer
// Output's anchor, so the default pivot turns content about its own centre.
struct Transform2DValues final {
    document::Vec2d translate{};
    document::Vec2d pivot{};
    document::Vec2d scale{1.0, 1.0};
    double rotation = 0.0;
    document::Vec2d centre{};
    friend bool operator==(const Transform2DValues&, const Transform2DValues&) = default;
};

struct PreparedTransform2D final {
    Transform2DExtent extent = Transform2DExtent::Transformed;
    std::int64_t filter = 0;
    Transform2DValues values;
    // Exact passthrough: no translation, unit scale and a rotation that reduces to exactly zero.
    // Clipping to the display window still runs the (exact, zero-weight) resample.
    [[nodiscard]] bool identity() const noexcept {
        return extent != Transform2DExtent::Display && values.translate.x == 0.0 &&
               values.translate.y == 0.0 && values.scale.x == 1.0 && values.scale.y == 1.0 &&
               std::remainder(values.rotation, 360.0) == 0.0;
    }
};

[[nodiscard]] inline bool validTransform2DKernel(const Transform2DKernel& kernel) noexcept {
    return static_cast<std::uint8_t>(kernel.extent) <=
               static_cast<std::uint8_t>(Transform2DExtent::Display) &&
           (kernel.filter == kResampleFilterBilinear || kernel.filter == kResampleFilterNearest);
}

[[nodiscard]] inline bool validTransform2DValues(const Transform2DValues& values) noexcept {
    return std::isfinite(values.translate.x) && std::isfinite(values.translate.y) &&
           std::isfinite(values.pivot.x) && std::isfinite(values.pivot.y) &&
           std::isfinite(values.scale.x) && std::isfinite(values.scale.y) &&
           std::isfinite(values.rotation) && std::isfinite(values.centre.x) &&
           std::isfinite(values.centre.y);
}

struct Transform2DGeometry final {
    // Author-space matrix the GPU affine command is built from, exactly as a Layer Output builds
    // its raster affine (LayerMatrix::authored about the pivot, conjugated by the proxy factors).
    LayerMatrix matrix;
    // The Layer Output's own CPU resampler for the same map. Absent when a zero scale collapses
    // the image.
    std::optional<render::LayerTransform> transform;
    // The output data window, or empty when the image collapses or its support misses the selected
    // extent: the effect then publishes no image, as a collapsed or entirely clipped layer does.
    std::optional<render::ImageWindow> window;
};

// Absent for an invalid kernel, a non-finite value or scale, or a transformed support beyond the
// supported +/-2^24 device coordinate range (the same limit as Layer Output).
[[nodiscard]] inline std::optional<Transform2DGeometry>
resolveTransform2DGeometry(const PreparedTransform2D& prepared,
                           const render::Rgba32fImageDescriptor& source, const double proxyX,
                           const double proxyY) {
    if (static_cast<std::uint8_t>(prepared.extent) >
            static_cast<std::uint8_t>(Transform2DExtent::Display) ||
        (prepared.filter != kResampleFilterBilinear && prepared.filter != kResampleFilterNearest) ||
        !validTransform2DValues(prepared.values) ||
        !std::isfinite(proxyX) || !std::isfinite(proxyY) || !(proxyX > 0.0) || !(proxyY > 0.0))
        return std::nullopt;
    const auto& values = prepared.values;
    // The pivot is an offset from the content-bounds centre, like a Layer anchor; the translation
    // moves that pivot. In the Layer's vocabulary: anchor = pivot, position = centre + pivot +
    // translate, so its authored translation is exactly `translate`.
    const document::Vec2d pivot{values.centre.x + values.pivot.x, values.centre.y + values.pivot.y};
    Transform2DGeometry geometry{
        LayerMatrix::authored({pivot.x + values.translate.x, pivot.y + values.translate.y}, pivot,
                              values.scale, values.rotation),
        std::nullopt, std::nullopt};
    if (values.scale.x == 0.0 || values.scale.y == 0.0)
        return geometry;
    const auto data = source.dataWindow();
    const double bufferCentreX = (static_cast<double>(data.originX()) / proxyX +
                                  static_cast<double>(data.maxXExclusive()) / proxyX) /
                                 2.0;
    const double bufferCentreY = (static_cast<double>(data.originY()) / proxyY +
                                  static_cast<double>(data.maxYExclusive()) / proxyY) /
                                 2.0;
    const render::LayerTransform::Authored authored{
        .translationX = values.translate.x,
        .translationY = values.translate.y,
        .anchorX = values.centre.x - bufferCentreX + values.pivot.x,
        .anchorY = values.centre.y - bufferCentreY + values.pivot.y,
        .scaleX = values.scale.x,
        .scaleY = values.scale.y,
        .rotationDegrees = values.rotation,
        .opacity = 1.0,
    };
    if (!std::isfinite(authored.translationX * proxyX) ||
        !std::isfinite(authored.translationY * proxyY))
        return std::nullopt;
    const auto transform = render::LayerTransform::create(authored, data, proxyX, proxyY);
    if (!transform)
        return std::nullopt;
    constexpr double limit = 16777214.0;
    const double right = static_cast<double>(data.extent().width());
    const double bottom = static_cast<double>(data.extent().height());
    const auto& resolved = *transform.value();
    for (const auto point :
         std::array{resolved.forwardMap(-1.0, -1.0), resolved.forwardMap(right, -1.0),
                    resolved.forwardMap(right, bottom), resolved.forwardMap(-1.0, bottom)})
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > limit ||
            std::abs(point.y) > limit)
            return std::nullopt;
    const auto working = render::ImageWindow::create(-16777216, -16777216, 33554432, 33554432);
    const auto clip = prepared.extent == Transform2DExtent::Input     ? data
                      : prepared.extent == Transform2DExtent::Display ? source.displayWindow()
                                                                      : *working.value();
    geometry.transform = resolved;
    geometry.window = resolved.supportBounds(clip);
    return geometry;
}

} // namespace bloom::runtime::detail
