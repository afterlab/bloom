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

} // namespace bloom::document
