// #483 Z Defocus and Depth Fog on the CPU reference. The depth reading of every interpretation
// against the document's double-precision readDepth; the circle of confusion of the Depth,
// Physical and Size map modes, the in-focus range, the blur regions, the empty-depth rules, the
// focus point and the edge expansion against an independent formula; every fog falloff; the
// gather's promises (a flat region stays flat, energy is kept, an in-focus foreground is never
// touched by the blurred background behind it, a sharp edge throws no halo into the blur, a
// blurred foreground spreads over a sharp background, highlights boost, chromatic bokeh spreads
// red widest); the production chains through the evaluator (ROI, cache, Uniform mode, fog modes);
// the fixed programs whose values are data; the pixel budget and cancellation.

#include "color_grade_image_effect.hpp"
#include "depth_effect_image_effect.hpp"
#include "depth_effect_test_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using runtime::DefocusKernel;
using runtime::DepthMapKernel;
using runtime::DepthMapOutput;
using Pixel = std::array<float, 4>;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-5) {
    return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
}
[[nodiscard]] bool near(const float a, const double b, const double tolerance = 1e-5) {
    return near(static_cast<double>(a), b, tolerance);
}

render::Rgba32fImage image(const std::int64_t width, const std::int64_t height,
                           const std::function<Pixel(std::int64_t, std::int64_t)>& value,
                           const std::int64_t originX = 0, const std::int64_t originY = 0) {
    const auto data = render::ImageWindow::create(originX, originY, static_cast<std::uint64_t>(width),
                                                  static_cast<std::uint64_t>(height));
    const auto display = render::ImageWindow::create(0, 0, static_cast<std::uint64_t>(width),
                                                     static_cast<std::uint64_t>(height));
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 24U);
    for (std::int64_t y = originY; y < originY + height; ++y)
        for (std::int64_t x = originX; x < originX + width; ++x) {
            const auto p = value(x, y);
            const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!pixel || builder.value()->write(x, y, *pixel.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

Pixel at(const render::Rgba32fImage& source, const std::int64_t x, const std::int64_t y) {
    const auto view = source.view();
    const auto row = view.value()->row(y);
    const auto window = source.descriptor()->dataWindow();
    const auto& p = (*row.value())[static_cast<std::size_t>(x - window.originX())];
    return {p.red(), p.green(), p.blue(), p.alpha()};
}

// A copy of an evaluated image (results are shared and immutable).
render::Rgba32fImage copyOf(const render::Rgba32fImage& source) {
    const auto& descriptor = *source.descriptor();
    const auto window = descriptor.dataWindow();
    auto builder = render::Rgba32fImageBuilder::create(descriptor, 1U << 26U);
    const auto w = static_cast<std::int64_t>(window.extent().width());
    const auto h = static_cast<std::int64_t>(window.extent().height());
    for (std::int64_t y = window.originY(); y < window.originY() + h; ++y)
        for (std::int64_t x = window.originX(); x < window.originX() + w; ++x) {
            const auto p = at(source, x, y);
            const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!pixel || builder.value()->write(x, y, *pixel.value()))
                throw std::logic_error("copied pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

// --- The depth map ------------------------------------------------------------------------------

struct Lens final {
    std::int64_t mode = document::kDefocusModeDepth;
    std::int64_t interpretation = document::kDepthInterpretationZ;
    std::uint8_t channel = 0;
    bool invert = false;
    std::int64_t mistFalloff = document::kMistFalloffLinear;
    std::int64_t background = document::kDepthBackgroundInfinite;
    std::int64_t focusFrom = document::kFocusFromDistance;
    std::int64_t blurRegion = document::kDefocusBlurNearAndFar;
    std::int64_t units = document::kSceneUnitMetres;
    std::int64_t edgeExpand = 0;
    double nearDistance = 1.0;
    double farDistance = 50.0;
    double focus = 10.0;
    double pointX = 0.0;
    double pointY = 0.0;
    double depthOfField = 0.0;
    double size = 16.0;
    double maxSize = 64.0;
    double focal = 50.0;
    double sensor = 36.0;
    double fStop = 2.0;
};

DepthMapKernel cocKernel(const Lens& lens) {
    DepthMapKernel kernel;
    kernel.output = DepthMapOutput::CircleOfConfusion;
    kernel.channel = lens.channel;
    kernel.interpretation = lens.interpretation;
    kernel.invert = lens.invert;
    kernel.mistFalloff = lens.mistFalloff;
    kernel.background = lens.background;
    kernel.mode = lens.mode;
    kernel.focusFrom = lens.focusFrom;
    kernel.blurRegion = lens.blurRegion;
    kernel.units = lens.units;
    kernel.edgeExpand = lens.edgeExpand;
    return kernel;
}

std::vector<double> cocValues(const Lens& lens) {
    return {lens.nearDistance, lens.farDistance, lens.focus,  lens.pointX,
            lens.pointY,       lens.depthOfField, lens.size,  lens.maxSize,
            lens.focal,        lens.sensor,       lens.fStop};
}

render::Rgba32fImage cocOf(const Lens& lens, const render::Rgba32fImage& depth,
                           const double scale = 1.0) {
    const auto grade = runtime::detail::prepareColorGrade(cocKernel(lens), cocValues(lens));
    if (!grade)
        throw std::logic_error("coc prepares");
    auto result = runtime::detail::applyDepthMap(*grade, depth, scale, 1U << 24U, {},
                                                 runtime::OperationIndex::fromRaw(0), {}, nullptr);
    if (!result.image)
        throw std::logic_error("coc applies");
    return copyOf(*result.image);
}

// The specification, in double precision (docs/architecture/image-filters.md, "Depth effects").
double referenceCoc(const Lens& lens, const document::DepthDistance distance,
                    const double frameWidth) {
    const double widest = lens.mode == document::kDefocusModeSizeMap ? lens.size : lens.maxSize;
    double c = 0.0;
    if (distance.empty && lens.background == document::kDepthBackgroundUnaffected)
        return 0.0;
    const bool infinite = distance.empty && lens.background == document::kDepthBackgroundInfinite;
    const double d = distance.empty && !infinite ? lens.farDistance : distance.distance;
    if (lens.mode == document::kDefocusModePhysical) {
        const double focusMm = lens.focus * 1000.0 / document::sceneUnitsPerMetre(lens.units);
        const double k = lens.focal * lens.focal / (lens.fStop * (focusMm - lens.focal)) *
                         frameWidth / (2.0 * lens.sensor);
        c = infinite ? k : k * (d - lens.focus) / d;
    } else {
        const double h = lens.depthOfField / 2.0;
        if (infinite)
            c = lens.size;
        else if (d > lens.focus + h)
            c = lens.size * (d - lens.focus - h) / d;
        else if (d < lens.focus - h)
            c = -lens.size * (lens.focus - h - d) / d;
    }
    if (lens.blurRegion == document::kDefocusBlurFarOnly)
        c = std::max(c, 0.0);
    if (lens.blurRegion == document::kDefocusBlurNearOnly)
        c = std::min(c, 0.0);
    return std::clamp(c, -widest, widest);
}

void testReadDepth() {
    using document::DepthReading;
    const DepthReading z{document::kDepthInterpretationZ, 0.0, 1.0, false};
    expect(near(document::readDepth(z, 12.5).distance, 12.5), "Z is the distance");
    expect(document::readDepth(z, 0.0).empty && document::readDepth(z, 1.0e9).empty &&
               document::readDepth(z, -1.0).empty &&
               document::readDepth(z, std::numeric_limits<double>::infinity()).empty,
           "Z at or below 0, at 1e9 or more and non-finite is empty");
    const DepthReading inverse{document::kDepthInterpretationInverse, 0.0, 1.0, false};
    expect(near(document::readDepth(inverse, 0.25).distance, 4.0) &&
               document::readDepth(inverse, 0.0).empty,
           "1/Z is the reciprocal; 0 is empty");
    const DepthReading normalised{document::kDepthInterpretationNormalised, 2.0, 12.0, false};
    expect(near(document::readDepth(normalised, 0.25).distance, 4.5) &&
               near(document::readDepth(normalised, 2.0).distance, 12.0),
           "a normalised value runs linearly from near to far, clamped");
    const DepthReading inverted{document::kDepthInterpretationNormalised, 2.0, 12.0, true};
    expect(near(document::readDepth(inverted, 0.25).distance, 9.5), "white is near when inverted");
    const DepthReading mist{document::kDepthInterpretationMist, 0.0, 100.0, false,
                            document::kMistFalloffQuadratic};
    expect(near(document::readDepth(mist, 0.25).distance, 50.0),
           "a quadratic mist is undone before the near-far ramp");
    const DepthReading inverseMist{document::kDepthInterpretationMist, 0.0, 100.0, false,
                                   document::kMistFalloffInverseQuadratic};
    expect(near(document::readDepth(inverseMist, 0.5).distance, 25.0),
           "an inverse quadratic mist is squared before the ramp");
    expect(near(document::sceneUnitsPerMetre(document::kSceneUnitCentimetres), 100.0) &&
               near(document::sceneUnitsPerMetre(document::kSceneUnitFeet), 1.0 / 0.3048),
           "scene units per metre");
}

void testCircleOfConfusion() {
    // A Z ramp 1..40 along x, with transparent (empty) pixels in the last column.
    const auto depth = image(40, 6, [](const std::int64_t x, const std::int64_t) -> Pixel {
        if (x == 39)
            return {0.0F, 0.0F, 0.0F, 0.0F};
        const float d = 1.0F + static_cast<float>(x);
        return {d, d, d, 1.0F};
    });
    const auto check = [&](const Lens& lens, const std::string& name, const double scale = 1.0) {
        const auto map = cocOf(lens, depth, scale);
        const document::DepthReading reading{lens.interpretation, lens.nearDistance,
                                             lens.farDistance, lens.invert, lens.mistFalloff,
                                             lens.background};
        bool ok = true;
        for (std::int64_t x = 0; x < 40; ++x) {
            const auto distance = x == 39 ? document::DepthDistance{0.0, true, false}
                                          : document::readDepth(reading, 1.0 + static_cast<double>(x));
            const double expected = referenceCoc(lens, distance, 40.0) * scale;
            const auto p = at(map, x, 2);
            const double widest =
                (lens.mode == document::kDefocusModeSizeMap ? lens.size : lens.maxSize) * scale;
            ok = ok && near(p[0], expected, 1e-5) &&
                 near(p[3], std::min(std::abs(expected) / widest, 1.0), 1e-5);
        }
        expect(ok, name + ": the circle of confusion matches the formula");
    };
    check({}, "depth mode");
    check({.depthOfField = 6.0}, "an in-focus range");
    check({.blurRegion = document::kDefocusBlurFarOnly}, "far only");
    check({.blurRegion = document::kDefocusBlurNearOnly}, "near only");
    check({.background = document::kDepthBackgroundFar}, "empty at the far distance");
    check({.background = document::kDepthBackgroundUnaffected}, "empty unaffected");
    check({.mode = document::kDefocusModePhysical, .maxSize = 200.0}, "physical lens");
    check({.mode = document::kDefocusModePhysical, .units = document::kSceneUnitCentimetres,
           .focus = 20.0, .maxSize = 200.0},
          "physical lens in centimetres");
    check({.maxSize = 6.0}, "clamped to the maximum");
    check({}, "a half-resolution proxy", 0.5);
    // The focus point: the mean depth of the 5 x 5 area at the point (an offset from the frame's
    // centre), so that depth is in focus.
    {
        Lens lens{.focusFrom = document::kFocusFromPoint, .pointX = -10.0, .pointY = 0.0};
        const auto map = cocOf(lens, depth);
        expect(near(at(map, 10, 2)[0], 0.0, 1e-6) && at(map, 30, 2)[0] > 0.0F &&
                   at(map, 3, 2)[0] < 0.0F,
               "the focus point's depth is in focus");
        lens.pointX = 1000.0;
        const auto fallback = cocOf(lens, depth);
        expect(near(at(fallback, 9, 2)[0], 0.0, 1e-6),
               "a focus point off the image falls back to the focus distance");
    }
    // Edge expansion: the nearest circle of confusion in reach wins.
    {
        const auto step = image(12, 3, [](const std::int64_t x, const std::int64_t) -> Pixel {
            const float d = x < 6 ? 2.0F : 40.0F;
            return {d, d, d, 1.0F};
        });
        const Lens lens{.edgeExpand = 2};
        const auto map = cocOf(lens, step);
        expect(at(map, 7, 1)[0] < 0.0F && at(map, 8, 1)[0] > 0.0F,
               "edge expansion moves the near side out by its reach");
    }
    // Size map: the value times the size, from the picked channel, unpremultiplied.
    {
        const auto map = image(4, 1, [](const std::int64_t x, const std::int64_t) -> Pixel {
            const float a = 0.5F;
            const float v = 0.25F * static_cast<float>(x);
            return {0.0F, v * a, 0.0F, a};
        });
        const Lens lens{.mode = document::kDefocusModeSizeMap, .channel = 1, .size = 8.0};
        const auto coc = cocOf(lens, map);
        expect(near(at(coc, 2, 0)[0], 4.0) && near(at(coc, 3, 0)[0], 6.0),
               "a size map scales the size by its straight value");
    }
}

// --- Fog --------------------------------------------------------------------------------------

void testFog() {
    const auto depth = image(32, 1, [](const std::int64_t x, const std::int64_t) -> Pixel {
        if (x == 31)
            return {0.0F, 0.0F, 0.0F, 0.0F};
        const float d = static_cast<float>(x);
        return {d, d, d, 1.0F};
    });
    for (const auto falloff :
         {document::kFogFalloffLinear, document::kFogFalloffQuadratic,
          document::kFogFalloffInverseQuadratic, document::kFogFalloffExponential,
          document::kFogFalloffExponentialSquared, document::kFogFalloffCurve}) {
        DepthMapKernel kernel;
        kernel.output = DepthMapOutput::Fog;
        kernel.interpretation = document::kDepthInterpretationZ;
        kernel.falloff = falloff;
        const double amount = 0.9, start = 4.0, end = 24.0, density = 0.2;
        std::vector<double> values{0.0, 100.0, amount, start, end, density};
        if (falloff == document::kFogFalloffCurve)
            values.insert(values.end(), {2.0, 0.0, 0.0, 1.0, 1.0});
        const auto grade = runtime::detail::prepareColorGrade(kernel, values);
        expect(grade.has_value(), "the fog prepares");
        if (!grade)
            continue;
        const auto result = runtime::detail::applyDepthMap(
            *grade, depth, 1.0, 1U << 24U, {}, runtime::OperationIndex::fromRaw(0), {}, nullptr);
        expect(result.image != nullptr, "the fog applies");
        if (!result.image)
            continue;
        // x = 0 (Z 0) and x = 31 (alpha 0) are empty: infinitely far, so fully fogged.
        bool ok = near(at(*result.image, 0, 0)[3], amount, 1e-6);
        for (std::int64_t x = 1; x < 31; ++x) {
            const double d = static_cast<double>(x);
            const double t = std::clamp((d - start) / (end - start), 0.0, 1.0);
            const double z = std::max(d - start, 0.0);
            const double f = falloff == document::kFogFalloffLinear      ? t
                             : falloff == document::kFogFalloffQuadratic ? t * t
                             : falloff == document::kFogFalloffInverseQuadratic
                                 ? std::sqrt(t)
                             : falloff == document::kFogFalloffExponential
                                 ? 1.0 - std::exp(-density * z)
                             : falloff == document::kFogFalloffExponentialSquared
                                 ? 1.0 - std::exp(-(density * z) * (density * z))
                                 : t;
            const auto p = at(*result.image, x, 0);
            ok = ok && near(p[3], amount * f, 2e-5) && p[0] == p[3];
        }
        ok = ok && near(at(*result.image, 31, 0)[3], amount, 1e-6);
        expect(ok, "fog falloff " + std::to_string(falloff) +
                       " matches the formula and an empty pixel is infinitely far");
    }
}

// --- The gather -------------------------------------------------------------------------------

struct Gather final {
    std::int64_t mode = document::kDefocusModeDepth;
    std::int64_t shape = document::kBokehDisk;
    std::int64_t quality = document::kDefocusQualityNormal;
    double size = 6.0;
    double maxSize = 6.0;
    double rotation = 0.0;
    double roundness = 0.0;
    double gain = 0.0;
    double threshold = 1.0;
    double chromatic = 0.0;
};

DefocusKernel defocusKernel(const Gather& g, const bool coc, const bool bokeh = false) {
    DefocusKernel kernel;
    kernel.mode = g.mode;
    kernel.shape = g.shape;
    kernel.blades = 6;
    kernel.quality = g.quality;
    if (coc)
        kernel.slotImages[runtime::kDefocusTileSlot] = 0;
    if (bokeh)
        kernel.slotImages[runtime::kDefocusBokehSlot] = coc ? 1 : 0;
    return kernel;
}

std::vector<double> gatherValues(const Gather& g) {
    return {g.size, g.maxSize, g.rotation, g.roundness, g.gain, g.threshold, g.chromatic};
}

// The tile maximum the chain derives (a Dilate of the map's coverage carrying each pixel's own
// colour), here a direct square max: the map's signed radius in red, the neighbourhood's widest
// coverage in alpha.
render::Rgba32fImage tileOf(const render::Rgba32fImage& coc, const std::int64_t radius) {
    const auto window = coc.descriptor()->dataWindow();
    const auto w = static_cast<std::int64_t>(window.extent().width());
    const auto h = static_cast<std::int64_t>(window.extent().height());
    return image(
        w, h,
        [&](const std::int64_t x, const std::int64_t y) -> Pixel {
            float m = 0.0F;
            for (std::int64_t j = -radius; j <= radius; ++j)
                for (std::int64_t i = -radius; i <= radius; ++i) {
                    const auto qx = x + i, qy = y + j;
                    if (qx < window.originX() || qy < window.originY() ||
                        qx >= window.originX() + w || qy >= window.originY() + h)
                        continue;
                    m = std::max(m, at(coc, qx, qy)[3]);
                }
            return {at(coc, x, y)[0], 0.0F, 0.0F, m};
        },
        window.originX(), window.originY());
}

render::Rgba32fImage gathered(const Gather& g, const render::Rgba32fImage& color,
                              const render::Rgba32fImage* coc, const render::Rgba32fImage* tile,
                              const render::Rgba32fImage* bokeh = nullptr) {
    const auto grade = runtime::detail::prepareColorGrade(defocusKernel(g, coc != nullptr, bokeh != nullptr),
                                                          gatherValues(g));
    if (!grade)
        throw std::logic_error("defocus prepares");
    // The tile carries the map (`coc` names the depth-driven gather; `tile` is its slot).
    const std::array<const render::Rgba32fImage*, runtime::kDefocusSlots> slots{tile, nullptr,
                                                                                bokeh};
    auto result = runtime::detail::applyDefocus(*grade, color, slots, 1.0, 1U << 26U, {},
                                                runtime::OperationIndex::fromRaw(0), {}, nullptr);
    if (!result.image)
        throw std::logic_error("defocus applies");
    return copyOf(*result.image);
}

// A circle of confusion map of constant signed radius c, or by a function.
render::Rgba32fImage cocMap(const std::int64_t w, const std::int64_t h, const double widest,
                            const std::function<float(std::int64_t, std::int64_t)>& c) {
    return image(w, h, [&](const std::int64_t x, const std::int64_t y) -> Pixel {
        const float value = c(x, y);
        return {value, 0.0F, 0.0F,
                static_cast<float>(std::min(std::abs(static_cast<double>(value)) / widest, 1.0))};
    });
}

void testGather() {
    // A flat opaque region stays flat (away from the frame's edges).
    {
        const auto color = image(40, 40, [](auto, auto) -> Pixel { return {0.4F, 0.2F, 0.1F, 1.0F}; });
        const Gather g{.mode = document::kDefocusModeUniform, .size = 6.0};
        const auto out = gathered(g, color, nullptr, nullptr);
        const auto window = out.descriptor()->dataWindow();
        expect(window.originX() < 0 && window.extent().width() > 40,
               "the gather grows the window by its reach");
        const auto p = at(out, 20, 20);
        expect(near(p[0], 0.4, 1e-5) && near(p[1], 0.2, 1e-5) && near(p[3], 1.0, 1e-6),
               "a flat region stays flat");
        // The frame edge fades to transparent like a blur with Transparent edges.
        expect(at(out, -2, 20)[3] > 0.0F && at(out, -2, 20)[3] < 0.5F,
               "the uniform blur reaches past the edge with partial coverage");
    }
    // Energy: one highlight on black spreads over a disk of the size and keeps its energy.
    {
        const auto color = image(41, 41, [](const std::int64_t x, const std::int64_t y) -> Pixel {
            const float v = x == 20 && y == 20 ? 100.0F : 0.0F;
            return {v, v, v, 1.0F};
        });
        for (const auto shape : {document::kBokehDisk, document::kBokehPolygon}) {
            const Gather g{.mode = document::kDefocusModeUniform, .shape = shape, .size = 8.0};
            const auto out = gathered(g, color, nullptr, nullptr);
            double sum = 0.0;
            for (std::int64_t y = 0; y < 41; ++y)
                for (std::int64_t x = 0; x < 41; ++x)
                    sum += static_cast<double>(at(out, x, y)[0]);
            expect(std::abs(sum - 100.0) < 12.0,
                   "a uniform blur keeps the highlight's energy (" + std::to_string(sum) + ")");
            expect(at(out, 25, 20)[0] > 0.1F && at(out, 31, 20)[0] < 1e-6F,
                   "the highlight spreads over the bokeh and no further");
        }
    }
    // An in-focus foreground square over a blurred background: the foreground keeps every pixel,
    // and the background never takes the foreground's colour (no halo).
    {
        const std::int64_t n = 48;
        const auto inside = [](const std::int64_t x, const std::int64_t y) {
            return x >= 16 && x < 32 && y >= 16 && y < 32;
        };
        const auto color = image(n, n, [&](const std::int64_t x, const std::int64_t y) -> Pixel {
            if (inside(x, y))
                return {1.0F, 0.0F, 0.0F, 1.0F};
            const float stripe = ((x / 2 + y / 3) % 2 == 0) ? 0.8F : 0.1F;
            return {0.0F, stripe, 1.0F, 1.0F};
        });
        const Gather g{.size = 6.0, .maxSize = 6.0};
        const auto coc = cocMap(n, n, 6.0, [&](auto x, auto y) { return inside(x, y) ? 0.0F : 6.0F; });
        const auto tile = tileOf(coc, 8);
        const auto out = gathered(g, color, &coc, &tile);
        bool sharp = true;
        for (std::int64_t y = 16; y < 32; ++y)
            for (std::int64_t x = 16; x < 32; ++x)
                sharp = sharp && at(out, x, y) == at(color, x, y);
        expect(sharp, "the in-focus foreground is untouched by the blurred background");
        bool clean = true;
        for (std::int64_t y = 4; y < 44; ++y)
            for (std::int64_t x = 4; x < 44; ++x)
                if (!inside(x, y))
                    clean = clean && at(out, x, y)[0] < 1e-6F;
        expect(clean, "the sharp foreground throws no halo into the blurred background");
        expect(std::abs(at(out, 8, 8)[1] - at(color, 8, 8)[1]) > 0.05F,
               "the background is blurred");
    }
    // A blurred near foreground spreads over a sharp background.
    {
        const std::int64_t n = 48;
        const auto inside = [](const std::int64_t x, const std::int64_t y) {
            return x >= 16 && x < 32 && y >= 16 && y < 32;
        };
        const auto color = image(n, n, [&](const std::int64_t x, const std::int64_t y) -> Pixel {
            return inside(x, y) ? Pixel{1.0F, 0.0F, 0.0F, 1.0F} : Pixel{0.0F, 0.0F, 1.0F, 1.0F};
        });
        const Gather g{.size = 6.0, .maxSize = 6.0};
        const auto coc =
            cocMap(n, n, 6.0, [&](auto x, auto y) { return inside(x, y) ? -6.0F : 0.0F; });
        const auto tile = tileOf(coc, 8);
        const auto out = gathered(g, color, &coc, &tile);
        expect(at(out, 34, 24)[0] > 0.1F, "a blurred foreground spreads over the background");
        expect(at(out, 2, 2) == at(color, 2, 2), "the sharp background far away is untouched");
    }
    // Highlight boost brightens the bokeh of a highlight; chromatic bokeh spreads red widest.
    {
        const auto color = image(41, 41, [](const std::int64_t x, const std::int64_t y) -> Pixel {
            const float v = x == 20 && y == 20 ? 20.0F : 0.2F;
            return {v, v, v, 1.0F};
        });
        const Gather plain{.mode = document::kDefocusModeUniform, .size = 6.0};
        const Gather boosted{.mode = document::kDefocusModeUniform, .size = 6.0, .gain = 4.0};
        expect(at(gathered(boosted, color, nullptr, nullptr), 22, 20)[0] >
                   at(gathered(plain, color, nullptr, nullptr), 22, 20)[0] * 1.5F,
               "the highlight boost brightens a highlight's bokeh");
        const Gather chroma{.mode = document::kDefocusModeUniform, .size = 6.0, .chromatic = 1.0};
        const auto out = gathered(chroma, color, nullptr, nullptr);
        expect(at(out, 27, 20)[0] > at(out, 27, 20)[2], "chromatic bokeh spreads red widest");
    }
    // A bokeh image weights the gather by its picture.
    {
        const auto color = image(41, 41, [](const std::int64_t x, const std::int64_t y) -> Pixel {
            const float v = x == 20 && y == 20 ? 50.0F : 0.0F;
            return {v, v, v, 1.0F};
        });
        const auto kernel = image(9, 9, [](const std::int64_t x, const std::int64_t) -> Pixel {
            const float v = x < 4 ? 1.0F : 0.0F;
            return {v, v, v, 1.0F};
        });
        const Gather g{.mode = document::kDefocusModeUniform, .shape = document::kBokehImage,
                       .size = 6.0};
        const auto out = gathered(g, color, nullptr, nullptr, &kernel);
        expect(at(out, 23, 20)[0] > 0.1F && at(out, 17, 20)[0] < 1e-6F,
               "a half-black bokeh image blurs to one side only");
    }
}

// --- The production chains through the evaluator ---------------------------------------------

runtime::EvaluationRequest request(const runtime::OperationIndex output) {
    runtime::EvaluationRequest value{{}, output, runtime::CompositionFormatResolution{}};
    value.pixelStorageByteLimit = 1U << 26U;
    return value;
}

void testChains() {
    const runtime::CpuCompositionEvaluator evaluator;
    using depth_test::DepthValues;
    using depth_test::Effect;
    const std::vector<std::pair<std::string, DepthValues>> cases{
        {"depth", {}},
        {"physical", {.mode = document::kDefocusModePhysical, .maxSize = 8.0}},
        {"size map", {.mist = true, .mode = document::kDefocusModeSizeMap}},
        {"uniform", {.mode = document::kDefocusModeUniform}},
        {"polygon", {.shape = document::kBokehPolygon, .quality = document::kDefocusQualityHigh}},
        {"image bokeh", {.shape = document::kBokehImage, .bokeh = true}},
        {"chromatic boost", {.highlightGain = 2.0, .chromatic = 0.5}},
        {"focus point", {.focusFrom = document::kFocusFromPoint}},
        {"fog linear", {.effect = Effect::Fog}},
        {"fog mist", {.effect = Effect::Fog, .mist = true}},
        {"fog exponential", {.effect = Effect::Fog, .falloff = document::kFogFalloffExponential}},
        {"fog curve", {.effect = Effect::Fog, .falloff = document::kFogFalloffCurve}},
        {"fog fill", {.effect = Effect::Fog, .fillEmpty = true}},
        {"fog image", {.effect = Effect::Fog, .fogImage = true}},
    };
    for (const auto& [name, values] : cases) {
        const auto plan = depth_test::depthPlan(values).plan;
        const auto cold = evaluator.evaluate(plan, request(plan->output()), {});
        expect(cold.frame() != nullptr, name + ": the production chain evaluates");
        if (!cold.frame()) {
            for (const auto& diagnostic : cold.diagnostics())
                std::cerr << "  " << diagnostic.summary << ": " << diagnostic.detail << '\n';
            continue;
        }
        const auto warm = evaluator.evaluate(plan, request(plan->output()), {});
        expect(warm.frame() && std::ranges::equal(warm.frame()->processImage().pixels(),
                                                  cold.frame()->processImage().pixels()),
               name + ": a warm evaluation is identical");
        auto roiRequest = request(plan->output());
        const auto roiWindow = render::ImageWindow::create(6, 5, 8, 6);
        roiRequest.roi = *roiWindow.value();
        const auto roi = evaluator.evaluate(plan, roiRequest, {});
        bool same = roi.frame() != nullptr;
        if (same) {
            const auto& full = cold.frame()->processImage();
            const auto& part = roi.frame()->processImage();
            for (std::int64_t y = 5; y < 11 && same; ++y)
                for (std::int64_t x = 6; x < 14 && same; ++x)
                    same = at(part, x, y) == at(full, x, y);
        }
        expect(same, name + ": an ROI matches the full frame inside it");
    }
    // The effect changes the image (the fog adds colour, the defocus blurs the far side).
    const auto plain = evaluator.evaluate(depth_test::depthPlan({.size = 0.0, .maxSize = 0.0}).plan,
                                          request(depth_test::depthPlan({}).plan->output()), {});
    expect(plain.frame() != nullptr, "a zero-size defocus evaluates");
}

// --- Programs, budget and cancellation --------------------------------------------------------

void testPrograms() {
    const auto depth = image(8, 4, [](auto x, auto) -> Pixel {
        const float d = 1.0F + static_cast<float>(x);
        return {d, d, d, 1.0F};
    });
    const auto grade = runtime::detail::prepareColorGrade(cocKernel({}), cocValues({}));
    const auto data = runtime::detail::depthMapProgramData(
        *grade, depth.descriptor()->dataWindow(), depth.descriptor()->displayWindow(), 1.0);
    expect(data && runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::DepthMap,
                                                           *data)
                        .succeeded(),
           "the depth map's one program builds from its data");
    expect(!runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::DepthMap,
                                                    grade->data)
                .succeeded(),
           "the depth map program refuses a table without its geometry");
    const auto gatherGrade = runtime::detail::prepareColorGrade(
        defocusKernel({}, true), gatherValues({}));
    runtime::detail::DefocusPlacement placement;
    placement.color = {0, 0, 8, 4, 6, 6};
    placement.slots[runtime::kDefocusTileSlot] = {0, 0, 8, 4, 20, 0};
    const auto gatherData = runtime::detail::defocusProgramData(*gatherGrade, placement, 1.0);
    const auto program = gatherData ? runtime::detail::buildColorGradeProgram(
                                          runtime::detail::ColorGradeKind::Defocus, *gatherData)
                                    : render::OcioGpuProgramResult::failure(
                                          render::OcioGpuProgramError::InvalidRequest);
    expect(program.succeeded(), "the gather's one program builds from its data");
    Gather wider{};
    wider.size = 9.0;
    wider.maxSize = 9.0;
    const auto widerGrade =
        runtime::detail::prepareColorGrade(defocusKernel(wider, true), gatherValues(wider));
    const auto widerData = runtime::detail::defocusProgramData(*widerGrade, placement, 1.0);
    const auto widerProgram = runtime::detail::buildColorGradeProgram(
        runtime::detail::ColorGradeKind::Defocus, *widerData);
    expect(program.succeeded() && widerProgram.succeeded() &&
               program.program()->shaderText == widerProgram.program()->shaderText &&
               program.program()->uniformBufferData != widerProgram.program()->uniformBufferData,
           "a changed size changes only the program's data");
    expect(runtime::detail::colorGradeIdentity(*gatherGrade) !=
               runtime::detail::colorGradeIdentity(*widerGrade),
           "a changed size changes the cache identity");
    // The prefiltered colour mixes in only when the widest ring spacing is over a pixel.
    {
        Gather small{};
        small.size = small.maxSize = 8.0;
        Gather large{};
        large.size = large.maxSize = 9.0;
        const auto smallGrade =
            runtime::detail::prepareColorGrade(defocusKernel(small, true), gatherValues(small));
        const auto largeGrade =
            runtime::detail::prepareColorGrade(defocusKernel(large, true), gatherValues(large));
        expect(smallGrade && largeGrade &&
                   !runtime::detail::defocusPrefilterUsed(*smallGrade, 1.0) &&
                   runtime::detail::defocusPrefilterUsed(*largeGrade, 1.0) &&
                   runtime::detail::defocusPrefilterUsed(*smallGrade, 2.0),
               "the prefilter is used only past one pixel of ring spacing (8 rings at Normal)");
    }
    expect(!runtime::detail::prepareColorGrade(defocusKernel({.size = 300.0}, true),
                                               gatherValues({.size = 300.0})),
           "a size outside its domain is refused");
    Gather none{};
    none.mode = document::kDefocusModeUniform;
    none.size = 0.0;
    const auto zero = runtime::detail::prepareColorGrade(defocusKernel(none, false), gatherValues(none));
    expect(zero && zero->identity, "no blur is the identity");
}

void testBudgetAndCancellation() {
    const auto color = image(32, 32, [](auto, auto) -> Pixel { return {0.5F, 0.5F, 0.5F, 1.0F}; });
    const Gather g{.mode = document::kDefocusModeUniform, .size = 6.0};
    const auto grade = runtime::detail::prepareColorGrade(defocusKernel(g, false), gatherValues(g));
    const std::array<const render::Rgba32fImage*, 4> slots{};
    const auto refused = runtime::detail::applyDefocus(*grade, color, slots, 1.0, 64, {},
                                                       runtime::OperationIndex::fromRaw(0), {},
                                                       nullptr);
    expect(!refused.image && refused.diagnostic &&
               refused.diagnostic->code ==
                   runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "a defocus beyond the pixel budget is refused");
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(483)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Defocus cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result = runtime::detail::applyDefocus(
                *grade, color, slots, 1.0, 1U << 24U, context.cancellation(),
                runtime::OperationIndex::fromRaw(0), {}, nullptr);
            cancelled = result.cancelled && !result.image;
            return runtime::TaskResult<void>::cancelled();
        });
    expect(submission.accepted(), "the cancellation task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> result;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(result && result->state() == runtime::TaskState::Cancelled && cancelled,
           "a cancelled defocus publishes no image");
}

} // namespace

int main() {
    try {
        testReadDepth();
        testCircleOfConfusion();
        testFog();
        testGather();
        testChains();
        testPrograms();
        testBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " depth effect expectation(s) failed\n";
        return 1;
    }
    std::cout << "depth effect tests passed\n";
    return 0;
}
