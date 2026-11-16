#pragma once

// #733: the packed display frame as the runtime and the Viewer use it. The bit-exact rules live
// in render/display_pixel_packing.hpp, below the color and runtime modules that pack with them;
// this header is their one name for the runtime and the modules above it, and
// docs/architecture/scopes.md ("The cached display frame") is the specification.
//
//   Pack rule          render::displayCode10: code = floor(clamp(v, 0, 1) * 1023 + 0.5), binary32.
//   Layout rule        A frame is 10:10:10:2 (render::DisplayPixelLayout::Rgb10A2) when every
//                      pixel is exactly transparent or packs to the 8-bit alpha 255
//                      (render::displayAlphaAllowsRgb10A2); any other frame is straight RGBA8.
//                      Both layouts are four bytes a pixel.
//   Presentation rule  An 8-bit output shows render::presentedCode8 of a 10-bit code (the nearest
//                      8-bit code); a 10-bit output shows the code unchanged.
//   Display signal     render::displaySignalCodes: the 10-bit code a scope counts for a stored
//                      pixel of either layout, its color over black.
//   Frame counters     render::DisplayFrameCounters: under, over and not finite of the display
//                      transform's float result, counted by the frame job and kept with the frame.

#include <bloom/render/display_pixel_packing.hpp>

namespace bloom::runtime {

using render::DisplayFrameCounters;
using render::DisplayLumaCoefficients;
using render::DisplayPixelLayout;

} // namespace bloom::runtime
