// #507 Lens Flare on the CPU reference. The glow, ring and ghost against independent double-precision
// formulas; what the node promises (light added over HDR and signed colour, alpha kept on an opaque
// image and raised on a transparent one, the frame's output window, the same flare at a proxy scale
// and pixel aspect, the chromatic split, each element off at amount 0); the light's readings (source
// brightness and white, occlusion by every matte state, a disc that fades over an edge); the 3D
// projection; identity and domains; the production plan through the evaluator (ROI, cache); the
// fixed programs whose values are data; the presets; the pixel budget and cancellation.

#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"
#include "lens_flare_image_effect.hpp"
#include "lens_flare_test_support.hpp"

#include <bloom/document/lens_flare_definition.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using lens_flare_test::FlareValues;
using lens_flare_test::flareKernel;
using lens_flare_test::flareOperands;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-4) {
    return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
}

struct Frame final {
    std::int64_t dataX = 0;
    std::int64_t dataY = 0;
    std::uint32_t dataWidth = 64;
    std::uint32_t dataHeight = 48;
    std::uint32_t displayWidth = 64;
    std::uint32_t displayHeight = 48;
    core::PixelAspectRatio aspect = core::PixelAspectRatio::square();
    std::array<float, 4> pixel{0.0F, 0.0F, 0.0F, 1.0F};
};

render::Rgba32fImage image(const Frame& frame) {
    const auto data =
        render::ImageWindow::create(frame.dataX, frame.dataY, frame.dataWidth, frame.dataHeight);
    const auto display = render::ImageWindow::create(0, 0, frame.displayWidth, frame.displayHeight);
    auto descriptor =
        render::Rgba32fImageDescriptor::create(*data.value(), *display.value(), frame.aspect);
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 24U);
    const auto pixel = render::Rgba32f::fromPremultiplied(frame.pixel[0], frame.pixel[1],
                                                          frame.pixel[2], frame.pixel[3]);
    for (std::int64_t y = frame.dataY; y < frame.dataY + frame.dataHeight; ++y)
        for (std::int64_t x = frame.dataX; x < frame.dataX + frame.dataWidth; ++x)
            if (!pixel || builder.value()->write(x, y, *pixel.value()))
                throw std::logic_error("fixture pixel writes");
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

// Only the named elements on (every other amount 0).
FlareValues only(std::map<std::string, double> scalars) {
    FlareValues values;
    values.scalars = {{"glowAmount", 0.0}, {"streakAmount", 0.0}, {"ghostAmount", 0.0},
                      {"ringAmount", 0.0}, {"irisAmount", 0.0},   {"hoopAmount", 0.0},
                      {"anamorphicAmount", 0.0}};
    values.color = core::Color4d{1.0, 1.0, 1.0, 1.0};
    for (const auto& [role, value] : scalars)
        values.scalars[role] = value;
    return values;
}

std::optional<runtime::detail::PreparedColorGrade> prepare(const FlareValues& values) {
    return runtime::detail::prepareColorGrade(flareKernel(values, 1), flareOperands(values));
}

std::shared_ptr<const render::Rgba32fImage>
apply(const FlareValues& values, const render::Rgba32fImage& input,
      const render::Rgba32fImage* const matte = nullptr, const bool hasMatte = false,
      const double hScale = 1.0, const double vScale = 1.0) {
    const auto grade = prepare(values);
    if (!grade)
        throw std::logic_error("the flare prepares");
    auto result = runtime::detail::applyLensFlare(*grade, input, matte, hasMatte, hScale, vScale,
                                                  1U << 24U, {}, runtime::OperationIndex::fromRaw(0),
                                                  {}, nullptr);
    if (!result.image)
        throw std::logic_error("the flare evaluates");
    return result.image;
}

std::array<double, 4> at(const render::Rgba32fImage& result, const std::int64_t x,
                        const std::int64_t y) {
    const auto pixel = result.read(x, y);
    if (!pixel)
        throw std::logic_error("the pixel reads");
    return {static_cast<double>(pixel.value()->red()), static_cast<double>(pixel.value()->green()),
            static_cast<double>(pixel.value()->blue()), static_cast<double>(pixel.value()->alpha())};
}

// The glow, ring and ghost as the specification writes them, in double precision.
void testFormulas() {
    const render::Rgba32fImage black = image({});
    constexpr double height = 48.0;
    constexpr double cx = 31.5;
    constexpr double cy = 23.5;
    constexpr double lx = -12.0 / height;
    constexpr double ly = -8.0 / height;
    {
        const auto flare = only({{"glowAmount", 0.5}, {"glowSize", 25.0}});
        const auto result = apply(flare, black);
        for (std::int64_t y = 0; y < 48; y += 3)
            for (std::int64_t x = 0; x < 64; x += 3) {
                const double u = (static_cast<double>(x) - cx) / height - lx;
                const double v = (static_cast<double>(y) - cy) / height - ly;
                const double r2 = u * u + v * v;
                const double wide = 3.0 * 0.25;
                const double narrow = 0.25;
                const double core = std::pow(std::max(1.0 - r2 / (narrow * narrow), 0.0), 8.0);
                const double halo = std::pow(std::max(1.0 - r2 / (wide * wide), 0.0), 3.0);
                const double expected = 0.5 * (0.625 * core + 0.375 * halo);
                expect(near(at(*result, x, y)[0], expected),
                       "the glow follows its formula at " + std::to_string(x) + ", " +
                           std::to_string(y));
            }
    }
    {
        // A ring of radius 0.5 and width 0.1, red pushed out and blue pulled in by the split.
        const auto flare =
            only({{"ringAmount", 1.0}, {"ringSize", 50.0}, {"ringWidth", 10.0}, {"chromatic", 0.5}});
        const auto result = apply(flare, black);
        const double split = 0.2 * 0.5;
        for (std::int64_t y = 0; y < 48; y += 2)
            for (std::int64_t x = 0; x < 64; x += 2) {
                const double u = (static_cast<double>(x) - cx) / height - lx;
                const double v = (static_cast<double>(y) - cy) / height - ly;
                const double r2 = u * u + v * v;
                const auto channel = [&](const double radius) {
                    const double t = (r2 - radius * radius) / (2.0 * 0.5 * 0.1);
                    const double b = std::max(1.0 - t * t, 0.0);
                    const double outer = 0.5 * (1.0 + split) + 0.15;
                    return r2 < outer * outer ? b * b : 0.0;
                };
                const auto p = at(*result, x, y);
                expect(near(p[0], channel(0.5 * (1.0 + split))) && near(p[1], channel(0.5)) &&
                           near(p[2], channel(0.5 * (1.0 - split))),
                       "the ring follows its formula per channel");
            }
    }
    {
        // One ghost, placed by the shared layout.
        auto flare = only({{"ghostAmount", 1.0}, {"ghostSize", 20.0}, {"chromatic", 0.0}});
        flare.integers = {{"ghostCount", 1}};
        flare.position = {-14.0, 10.0};
        document::LensFlareLayoutInput input;
        input.lightX = -14.0 / height;
        input.lightY = 10.0 / height;
        input.ghostCount = 1;
        input.ghostSize = 0.2;
        input.seed = 1;
        input.ghostSpread = 1.0;
        const auto layout = document::lensFlareLayout(input);
        const auto& ghost = layout.ghosts.at(0);
        const auto result = apply(flare, black);
        for (std::int64_t y = 0; y < 48; ++y)
            for (std::int64_t x = 0; x < 64; ++x) {
                const double u = (static_cast<double>(x) - cx) / height - ghost.x;
                const double v = (static_cast<double>(y) - cy) / height - ghost.y;
                const double t = (u * u + v * v) / (ghost.radius * ghost.radius);
                const double expected =
                    t < 1.0 ? std::clamp((1.0 - t) / 0.2, 0.0, 1.0) * (0.5625 + 0.4375 * t * t) *
                                  ghost.tint[0]
                            : 0.0;
                expect(near(at(*result, x, y)[0], expected),
                       "the ghost follows its formula at " + std::to_string(x) + ", " +
                           std::to_string(y));
            }
    }
}

void testPromises() {
    // Light is added over any colour, and an opaque image stays opaque.
    Frame hdr;
    hdr.pixel = {4.0F, -0.5F, 0.25F, 1.0F};
    const auto input = image(hdr);
    const FlareValues all;
    const auto over = apply(all, input);
    const auto black = apply(all, image({}));
    bool added = true;
    for (std::int64_t y = 0; y < 48; ++y)
        for (std::int64_t x = 0; x < 64; ++x) {
            const auto p = at(*over, x, y);
            const auto light = at(*black, x, y);
            added = added && near(p[0], 4.0 + light[0]) && near(p[1], -0.5 + light[1]) &&
                    near(p[2], 0.25 + light[2]) && p[3] == 1.0;
        }
    expect(added, "the flare adds its light over HDR and signed colour and keeps opaque alpha");
    // The light itself is the brightest place.
    const auto peak = at(*black, 19, 15);
    expect(peak[0] > at(*black, 60, 44)[0] * 2.0 && peak[0] > 0.5,
           "the flare is brightest at its light");
    // A transparent image gains the flare's coverage, so it still composites as light.
    Frame clear;
    clear.pixel = {0.0F, 0.0F, 0.0F, 0.0F};
    const auto glowOnly = only({{"glowAmount", 0.5}});
    const auto overClear = apply(glowOnly, image(clear));
    const auto clearPeak = at(*overClear, 19, 15);
    expect(clearPeak[3] > 0.4 && clearPeak[3] <= 1.0 && near(clearPeak[0], clearPeak[3]),
           "a transparent image takes the light's own coverage");
    // A part alpha image rises by the screen of its own alpha and the light.
    Frame part;
    part.pixel = {0.25F, 0.25F, 0.25F, 0.5F};
    const auto partResult = at(*apply(glowOnly, image(part)), 19, 15);
    expect(partResult[3] > 0.5 && partResult[3] < 1.0, "a part-alpha image's alpha rises");

    // The output covers the input's data window joined with the display window.
    Frame small;
    small.dataX = 10;
    small.dataY = -6;
    small.dataWidth = 20;
    small.dataHeight = 12;
    const auto padded = apply(glowOnly, image(small));
    const auto window = padded->descriptor()->dataWindow();
    expect(window.originX() == 0 && window.originY() == -6 && window.maxXExclusive() == 64 &&
               window.maxYExclusive() == 48,
           "the flare lights the whole frame beyond a small input");
    expect(at(*padded, 19, 15)[0] > 0.3, "the padded area takes the light");
    expect(at(*padded, 15, -3)[3] == 1.0, "a pixel outside the frame keeps the input's alpha");

    // Each element draws something on its own, and none draws at amount 0.
    for (const auto* role : {"glowAmount", "streakAmount", "ghostAmount", "ringAmount", "irisAmount",
                             "hoopAmount", "anamorphicAmount"}) {
        auto flare = only({{role, 1.0}});
        flare.integers = {{"ghostCount", 16}, {"irisCount", 12}};
        // The default anamorphic streak is thinner than a pixel of this small frame.
        flare.scalars["anamorphicWidth"] = 4.0;
        const auto result = apply(flare, image({}));
        double sum = 0.0;
        for (std::int64_t y = 0; y < 48; ++y)
            for (std::int64_t x = 0; x < 64; ++x)
                sum += static_cast<double>(at(*result, x, y)[1]);
        expect(sum > 0.5, std::string(role) + " draws by itself");
    }

    // The same flare at half-resolution: same place, same value.
    const auto full = apply(glowOnly, image({}));
    Frame half;
    half.dataWidth = 32;
    half.dataHeight = 24;
    half.displayWidth = 32;
    half.displayHeight = 24;
    const auto proxy = apply(glowOnly, image(half), nullptr, false, 0.5, 0.5);
    // A half-scale pixel's centre (x + 0.5) * 2 lies between two full-scale pixels.
    const auto fullAt = [&](const std::int64_t x, const std::int64_t y) {
        return 0.25 * (static_cast<double>(at(*full, 2 * x, 2 * y)[0]) +
                       static_cast<double>(at(*full, 2 * x + 1, 2 * y)[0]) +
                       static_cast<double>(at(*full, 2 * x, 2 * y + 1)[0]) +
                       static_cast<double>(at(*full, 2 * x + 1, 2 * y + 1)[0]));
    };
    expect(std::abs(static_cast<double>(at(*proxy, 10, 8)[0]) - fullAt(10, 8)) < 0.02 &&
               std::abs(static_cast<double>(at(*proxy, 25, 20)[0]) - fullAt(25, 20)) < 0.02,
           "a proxy draws the same flare");

    // A 2:1 pixel aspect keeps the glow round: half as many pixels sideways for the same distance.
    Frame wide;
    wide.aspect = *core::PixelAspectRatio::create(2, 1);
    auto centred = glowOnly;
    centred.position = {0.0, 0.0};
    const auto stretched = apply(centred, image(wide));
    expect(near(at(*stretched, 32 + 5, 24)[0], at(*stretched, 32, 24 + 10)[0], 0.05),
           "a wide pixel aspect keeps the glow round");

    // The chromatic split pushes red out beyond blue.
    auto split = only({{"ghostAmount", 1.0}, {"ghostSize", 25.0}, {"chromatic", 1.0}});
    split.integers = {{"ghostCount", 1}};
    const auto splitResult = apply(split, image({}));
    double red = 0.0;
    double blue = 0.0;
    for (std::int64_t y = 0; y < 48; ++y)
        for (std::int64_t x = 0; x < 64; ++x) {
            const auto p = at(*splitResult, x, y);
            red += static_cast<double>(p[0]);
            blue += static_cast<double>(p[2]);
        }
    expect(red > blue * 1.1, "the chromatic split makes the red disc the largest");
}

void testReadings() {
    // Brightness from the source: the input's own value under the light scales the flare.
    auto follow = only({{"glowAmount", 1.0}, {"sourceBrightness", 1.0}, {"sourceWhite", 2.0}});
    Frame dark;
    const auto none = at(*apply(follow, image(dark)), 19, 15);
    expect(none[0] == 0.0, "a black source under the light gives no flare at full share");
    Frame mid;
    mid.pixel = {1.0F, 1.0F, 1.0F, 1.0F};
    const auto half = at(*apply(follow, image(mid)), 19, 15);
    auto plain = only({{"glowAmount", 1.0}});
    const auto reference = at(*apply(plain, image({})), 19, 15);
    expect(near(static_cast<double>(half[0]) - 1.0, 0.5 * static_cast<double>(reference[0]), 1e-3),
           "a source at half its white gives half the flare");
    Frame bright;
    bright.pixel = {8.0F, 8.0F, 8.0F, 1.0F};
    const auto whole = at(*apply(follow, image(bright)), 19, 15);
    expect(near(static_cast<double>(whole[0]) - 8.0, static_cast<double>(reference[0]), 1e-3),
           "a source above its white gives the whole flare");
    follow.scalars["sourceBrightness"] = 0.25;
    const auto share = at(*apply(follow, image(dark)), 19, 15);
    expect(near(share[0], 0.75 * static_cast<double>(reference[0]), 1e-3),
           "the share leaves the rest of the flare");

    // Occlusion: a matte covering the light hides the flare; one away from it does not.
    Frame covering;
    covering.pixel = {1.0F, 1.0F, 1.0F, 1.0F};
    const auto matte = image(covering);
    auto hidden = plain;
    const auto gone = at(*apply(hidden, image({}), &matte, true), 19, 15);
    expect(gone[0] == 0.0, "a matte over the light hides the flare");
    hidden.scalars["occlusionAmount"] = 0.5;
    expect(near(at(*apply(hidden, image({}), &matte, true), 19, 15)[0],
                0.5 * static_cast<double>(reference[0]), 1e-3),
           "the occlusion amount leaves the rest");
    auto inverted = plain;
    inverted.integers = {{"occlusionChannel", 5}};
    expect(near(at(*apply(inverted, image({}), &matte, true), 19, 15)[0],
                static_cast<double>(reference[0]), 1e-3),
           "an inverted matte over the light shows the flare");
    // No pixels under the disc: nothing covers the light, and the inverted reading covers it all.
    Frame elsewhere;
    elsewhere.dataX = 50;
    elsewhere.dataWidth = 14;
    elsewhere.pixel = {1.0F, 1.0F, 1.0F, 1.0F};
    const auto away = image(elsewhere);
    expect(near(at(*apply(plain, image({}), &away, true), 19, 15)[0],
                static_cast<double>(reference[0]), 1e-3),
           "a matte away from the light leaves the flare");
    expect(at(*apply(inverted, image({}), &away, true), 19, 15)[0] == 0.0,
           "an inverted matte away from the light hides the flare");
    expect(near(at(*apply(plain, image({}), nullptr, true), 19, 15)[0],
                static_cast<double>(reference[0]), 1e-3),
           "a matte with no pixels leaves the flare");
    // The channel selector: a black opaque matte covers by alpha and not by luma.
    Frame blackOpaque;
    const auto opaque = image(blackOpaque);
    auto luma = plain;
    luma.integers = {{"occlusionChannel", 4}};
    expect(at(*apply(plain, image({}), &opaque, true), 19, 15)[0] == 0.0 &&
               near(at(*apply(luma, image({}), &opaque, true), 19, 15)[0],
                    static_cast<double>(reference[0]), 1e-3),
           "the occlusion channel picks what covers the light");
    // A matte's edge through the light: the disc's samples fade the flare between the extremes.
    Frame edge;
    edge.dataWidth = 20; // the light's own pixel is (20, 16), just beyond this edge
    edge.pixel = {1.0F, 1.0F, 1.0F, 1.0F};
    const auto half2 = image(edge);
    auto soft = plain;
    soft.scalars["sampleRadius"] = 8.0;
    const auto partly = at(*apply(soft, image({}), &half2, true), 19, 15)[0];
    expect(partly > 0.2 * reference[0] && partly < 0.8 * reference[0],
           "the sample disc fades the flare over a matte's edge");
    // Radius 0 reads the light's one pixel: uncovered just beyond the edge, covered one pixel on.
    soft.scalars["sampleRadius"] = 0.0;
    expect(near(at(*apply(soft, image({}), &half2, true), 19, 15)[0], reference[0], 1e-3),
           "a zero radius ignores a matte beside the light's pixel");
    edge.dataWidth = 21;
    const auto over = image(edge);
    expect(at(*apply(soft, image({}), &over, true), 19, 15)[0] == 0.0,
           "a zero radius reads the light's own pixel");

    // The 3D projection: a light as far behind the frame as the camera's zoom sits half as far out.
    auto projected = plain;
    projected.integers = {{"lightMode", document::kLensFlareLight3D}};
    projected.scalars["cameraZoom"] = 500.0;
    projected.scalars["lightDistance"] = 500.0;
    projected.position = {-24.0, -16.0};
    const auto flat = plain;
    const auto projectedResult = apply(projected, image({}));
    const auto flatResult = apply(flat, image({}));
    bool same = true;
    for (std::int64_t y = 0; y < 48; y += 4)
        for (std::int64_t x = 0; x < 64; x += 4)
            same = same && at(*projectedResult, x, y)[0] == at(*flatResult, x, y)[0];
    expect(same, "the 3D mode projects the light through the camera");
    projected.scalars["lightDistance"] = -600.0;
    const auto behind = prepare(projected);
    expect(behind && behind->identity, "a light behind the camera adds nothing");
}

void testIdentityAndDomains() {
    FlareValues dark;
    dark.scalars = {{"intensity", 0.0}};
    expect(prepare(dark) && prepare(dark)->identity, "no intensity is the identity");
    const auto none = only({});
    expect(prepare(none) && prepare(none)->identity, "no element is the identity");
    auto noCounts = only({{"ghostAmount", 1.0}, {"irisAmount", 1.0}});
    noCounts.integers = {{"ghostCount", 0}, {"irisCount", 0}};
    expect(prepare(noCounts) && prepare(noCounts)->identity, "no ghosts and no irises draw nothing");
    expect(prepare({}) && !prepare({})->identity, "the default flare draws");

    const auto refused = [](const std::string& role, const double value) {
        FlareValues values;
        values.scalars[role] = value;
        return !prepare(values).has_value();
    };
    expect(refused("intensity", -1.0) && refused("flareSize", 0.0) && refused("chromatic", 2.0) &&
               refused("sampleRadius", 1000.0) && refused("sourceWhite", 0.0) &&
               refused("glowSize", std::nan("")),
           "a value outside its domain is refused");
    const auto refusedInteger = [](const std::string& role, const std::int64_t value) {
        FlareValues values;
        values.integers[role] = value;
        return !prepare(values).has_value();
    };
    expect(refusedInteger("streakCount", 0) && refusedInteger("streakCount", 9) &&
               refusedInteger("ghostCount", 17) && refusedInteger("irisCount", 13) &&
               refusedInteger("irisBlades", 2) && refusedInteger("irisBlades", 13) &&
               refusedInteger("occlusionChannel", 10) && refusedInteger("lightMode", 2),
           "a count outside its table is refused");
    FlareValues far;
    far.position = {2.0e6, 0.0};
    expect(!prepare(far), "a light beyond the position's domain is refused");
    expect(!runtime::detail::prepareColorGrade(flareKernel({}, 1), std::vector<double>{1.0}),
           "a wrong operand count is refused");
}

void testPrograms() {
    const auto first = prepare(lens_flare_test::everyElement());
    auto changedValues = lens_flare_test::everyElement();
    changedValues.position = {3.0, 4.0};
    const auto changed = prepare(changedValues);
    expect(first && changed, "both flares prepare");
    if (!first || !changed)
        return;
    const auto displayWindow = render::ImageWindow::create(0, 0, 64, 48);
    const auto& display = *displayWindow.value();
    const auto dataOf = [&](const runtime::detail::PreparedColorGrade& grade) {
        return runtime::detail::lensFlareProgramData(grade, display, display, 1.0, 1.0, 1.0, {}, {});
    };
    const auto a = dataOf(*first);
    const auto b = dataOf(*changed);
    expect(a && b && *a != *b, "a moved light changes the program data");
    if (!a || !b)
        return;
    expect(a->size() == runtime::detail::kLensFlareProgramFloats &&
               runtime::detail::validLensFlareProgramData(*a),
           "the program data is valid");
    const auto programA =
        runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::LensFlare, *a);
    const auto programB =
        runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::LensFlare, *b);
    expect(programA.succeeded() && programB.succeeded() &&
               render::validateOcioGpuProgram(*programA.program(), {}) ==
                   render::OcioGpuProgramError::None &&
               programA.program()->semanticsId == "bloom.render.lens-flare.v1" &&
               programA.program()->shaderText == programB.program()->shaderText &&
               programA.program()->uniformBufferData != programB.program()->uniformBufferData,
           "one fixed program: a value changes its data, never its shader");
    auto broken = *a;
    broken[0] = std::numeric_limits<float>::infinity();
    expect(!runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::LensFlare,
                                                    broken)
                .succeeded(),
           "a non-finite table is refused");
    expect(runtime::detail::colorGradeIdentity(*first) !=
               runtime::detail::colorGradeIdentity(*changed),
           "a moved light changes the cache identity");

    // Every path of the probe: the disc's offsets stay inside the window the host cuts.
    FlareValues disc;
    disc.scalars = {{"sourceBrightness", 1.0}, {"sampleRadius", 12.0}};
    const auto probed = prepare(disc);
    const auto probes =
        probed ? runtime::detail::lensFlareProbes(*probed, display, 1.0, 1.0, true) : std::nullopt;
    expect(probes && probes->source && probes->occlusion &&
               probes->window.extent().width() == probes->window.extent().height() &&
               probes->window.extent().width() % 2U == 1U &&
               probes->window.extent().width() >= 21 && probes->window.extent().width() <= 25,
           "the probe's window holds its disc");
    if (probes) {
        expect(runtime::detail::validLensFlareProbeData(probes->sourceData) &&
                   runtime::detail::validLensFlareProbeData(probes->occlusionData),
               "the probe data is valid");
        const auto program = runtime::detail::buildColorGradeProgram(
            runtime::detail::ColorGradeKind::LensFlareProbe, probes->sourceData);
        expect(program.succeeded(), "the probe program builds");
    }
}

void testPresetsAndLayout() {
    const auto& presets = document::lensFlarePresets();
    expect(presets.size() >= 5, "the node offers presets");
    for (const auto& preset : presets) {
        expect(!preset.name.empty(), "a preset has a name");
        for (const auto& item : preset.values)
            expect(document::isLensFlareParameterValueValid(document::lensFlareSchemaKey(item.role),
                                                            item.value),
                   std::string(preset.name) + "'s " + std::string(item.role) +
                       " lies in its domain");
    }
    document::LensFlareLayoutInput input;
    input.lightX = 0.4;
    input.lightY = -0.2;
    input.ghostCount = 16;
    input.ghostSize = 0.05;
    input.irisCount = 12;
    input.irisSize = 0.05;
    input.streakCount = 8;
    input.streakLength = 0.5;
    const auto one = document::lensFlareLayout(input);
    const auto two = document::lensFlareLayout(input);
    bool same = one.ghosts.size() == 16 && one.irises.size() == 12 && one.streaks.size() == 8 &&
                one.irisNormals.size() == 6;
    for (std::size_t i = 0; same && i < one.ghosts.size(); ++i)
        same = one.ghosts[i].x == two.ghosts[i].x && one.ghosts[i].radius == two.ghosts[i].radius;
    expect(same, "the layout is deterministic");
    // Ghosts lie on the axis through the light and the frame's centre.
    bool onAxis = true;
    for (const auto& ghost : one.ghosts)
        onAxis = onAxis && std::abs(ghost.x * input.lightY - ghost.y * input.lightX) < 1e-12;
    expect(onAxis, "the ghosts lie on the light's axis");
    auto other = input;
    other.seed = 2;
    expect(document::lensFlareLayout(other).ghosts[0].x != one.ghosts[0].x,
           "the pattern value changes the ghosts' places");
}

void testEvaluator() {
    const runtime::CpuCompositionEvaluator evaluator;
    auto values = lens_flare_test::everyElement();
    values.scalars["sourceBrightness"] = 0.5;
    values.matteWidth = 10;
    const auto plan = lens_flare_test::flarePlan(values);
    runtime::EvaluationRequest request{core::RationalTime{}, plan->output(),
                                       runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = 1U << 26U;
    const auto whole = evaluator.evaluate(plan, request, {});
    expect(whole.frame() != nullptr, "the plan evaluates");
    if (whole.frame() == nullptr)
        return;
    // A region reads the light where it is, so its pixels are the whole frame's.
    auto regional = request;
    const auto region = render::ImageWindow::create(40, 30, 16, 12);
    regional.roi = *region.value();
    const auto part = evaluator.evaluate(plan, regional, {});
    expect(part.frame() != nullptr, "a region of interest evaluates");
    if (part.frame() != nullptr) {
        const auto& crop = part.frame()->processImage();
        const auto& frame = whole.frame()->processImage();
        bool same = true;
        const auto window = crop.descriptor()->dataWindow();
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
            for (auto x = window.originX(); x < window.maxXExclusive(); ++x)
                same = same && at(crop, x, y) == at(frame, x, y);
        expect(same, "a region of interest draws the whole frame's pixels");
    }
    // A small input is padded to the frame.
    auto smallValues = values;
    smallValues.smallInput = true;
    const auto smallPlan = lens_flare_test::flarePlan(smallValues, 51700);
    auto smallRequest = request;
    smallRequest.output = smallPlan->output();
    const auto padded = evaluator.evaluate(smallPlan, smallRequest, {});
    expect(padded.frame() != nullptr, "a small input evaluates");
    if (padded.frame() != nullptr) {
        const auto window = padded.frame()->processImage().descriptor()->dataWindow();
        expect(window.extent().width() == lens_flare_test::kFlareWidth &&
                   window.extent().height() == lens_flare_test::kFlareHeight,
               "the small input's flare covers the frame");
    }
}

void testBudgetAndCancellation() {
    const auto input = image({});
    const auto grade = prepare({});
    if (!grade)
        throw std::logic_error("the flare prepares");
    auto refused = runtime::detail::applyLensFlare(*grade, input, nullptr, false, 1.0, 1.0, 16U, {},
                                                   runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(refused.diagnostic &&
               refused.diagnostic->code ==
                   runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "the pixel budget refuses the output");
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(507)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Lens Flare cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result = runtime::detail::applyLensFlare(
                *grade, input, nullptr, false, 1.0, 1.0, 1U << 24U, context.cancellation(),
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
           "a cancelled flare publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(scheduler.isQuiescent(), "the cancelled flare shuts down safely");
}

} // namespace

int main() {
    try {
        testFormulas();
        testPromises();
        testReadings();
        testIdentityAndDomains();
        testPrograms();
        testPresetsAndLayout();
        testEvaluator();
        testBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        ++failures;
    }
    if (failures == 0)
        std::cout << "lens flare tests passed\n";
    return failures == 0 ? 0 : 1;
}
