#pragma once

// Transform 2D's skew: a shear along an axis, in degrees. The limit keeps tan(skew) finite and the
// shear invertible; the skew axis and the opacity share the transform's other parameters.

namespace bloom::document {

inline constexpr double kMaximumTransformSkew = 89.0;

} // namespace bloom::document
