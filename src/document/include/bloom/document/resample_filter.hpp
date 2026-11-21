#pragma once

#include <cstdint>
#include <string_view>

namespace bloom::document {

// #742 resample filters of Transform 2D, Resize, Fit and the Layer transform: 0 Bilinear,
// 1 Nearest, 2 Trilinear, 3 Anisotropic and 4 Automatic, which the compiler resolves (Trilinear,
// equal to Bilinear unless the image shrinks). Stored values keep their meaning.
inline constexpr std::int64_t kResampleFilterAutomaticValue = 4;

// The Layer transform's filter (Layer node and entry layer). Optional: a layer saved without it
// resamples Bilinear, as it always did; layers added by Bloom bind Automatic.
inline constexpr std::string_view kLayerFilterParameterRole = "filter";
inline constexpr std::string_view kLayerFilterParameterSchemaKey = "bloom.layer.filter";

// #743 pixel snap of the layer's position (Layer node and entry layer): 0 Off, 1 On, 2 Automatic
// (on in a pixel-art composition, off otherwise). Optional: a layer saved without it does not
// snap; layers added by Bloom bind Automatic. Snapping rounds the final composed translation of
// the layer to whole pixels of the evaluation, after parenting.
inline constexpr std::int64_t kLayerSnapOff = 0;
inline constexpr std::int64_t kLayerSnapOn = 1;
inline constexpr std::int64_t kLayerSnapAutomatic = 2;
inline constexpr std::string_view kLayerSnapParameterRole = "snap";
inline constexpr std::string_view kLayerSnapParameterSchemaKey = "bloom.layer.snap";
// #743 Transform 2D's Snap to Pixels: a constant Boolean. Optional: absent is off.
inline constexpr std::string_view kTransform2DSnapParameterSchemaKey = "bloom.transform-2d.snap";

} // namespace bloom::document
