// #507 Lens Flare. The element stack (lensFlarePixel, lensFlareProgramText) and the light's probe
// (lensFlareProbeValue, lensFlareProbeProgramText) over the same data. Only additions,
// multiplications, abs, min and max: no sqrt, no division, no pow, so the CPU reference and the GPU
// agree bit for bit. Rings are drawn from the squared distance (a ring's own width is small next to
// its radius), and every shape is a polynomial reaching exactly zero at its edge.
// docs/architecture/image-filters.md, "Lens Flare", is the specification.

#include "lens_flare_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/lens_flare_definition.hpp>
#include <bloom/render/image_coverage.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace bloom::runtime::detail {
namespace {

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

// --- The prepared table -------------------------------------------------------------------------

constexpr std::size_t kPosition = 0;
constexpr std::size_t kColor = 2;
constexpr std::size_t kAnamorphicColor = 6;
constexpr std::size_t kScalars = 10;
constexpr std::size_t kLightMode = kLensFlareOperands;
constexpr std::size_t kStreakCount = kLightMode + 1;
constexpr std::size_t kGhostCount = kLightMode + 2;
constexpr std::size_t kSeed = kLightMode + 3;
constexpr std::size_t kIrisCount = kLightMode + 4;
constexpr std::size_t kIrisBlades = kLightMode + 5;
constexpr std::size_t kOcclusionChannel = kLightMode + 6;

[[nodiscard]] double scalarOf(const std::vector<float>& table, const std::string_view role) {
    return static_cast<double>(table[kScalars + document::lensFlareScalarIndex(role)]);
}
// A percentage of the frame's height, in frame heights.
[[nodiscard]] double shareOf(const std::vector<float>& table, const std::string_view role) {
    return scalarOf(table, role) / 100.0;
}
[[nodiscard]] std::int64_t integerOf(const std::vector<float>& table, const std::size_t index) {
    return static_cast<std::int64_t>(table[index]);
}

// The light's offset from the frame's centre in full-resolution pixels (the 3D mode's projection
// applied). False behind the camera.
[[nodiscard]] bool lightOffset(const std::vector<float>& table, double& x, double& y) {
    double scale = 1.0;
    if (integerOf(table, kLightMode) == document::kLensFlareLight3D &&
        !document::lensFlareProjection(scalarOf(table, "cameraZoom"),
                                       scalarOf(table, "lightDistance"), scale))
        return false;
    x = static_cast<double>(table[kPosition]) * scale;
    y = static_cast<double>(table[kPosition + 1]) * scale;
    return true;
}

// --- The program data, in vec4s -----------------------------------------------------------------
//   0  output width, output height, frame centre x, y (pixel indices)
//   1  frame heights per pixel x, y; light x, y (frame heights from the centre)
//   2  away-from-the-light axis x, y; the smallest light kept; 0
//   3  source: read x, y; value; share
//   4  occlusion: read x, y; value; amount
//   5  1 / source white, invert, glow on, ring on
//   6  hoop on, anamorphic on, streak lines, ghosts
//   7  irises, blades, streaks' reach squared, 0
//   8  glow rgb, 1 / core radius squared
//   9  1 / glow radius squared, 0, 0, 0
//  10  streak rgb, 0
//  11  ring rgb, 1 / (2 radius width)
//  12  ring radius squared r, g, b; outer reach squared
//  13  hoop rgb, 1 / (2 radius width)
//  14  hoop radius squared r, g, b; 1 / radius
//  15  hoop inner reach squared, outer reach squared, 0, 0
//  16  anamorphic rgb, 0
//  17  anamorphic direction x, y; 1 / length, 1 / width
//  18  8 streak lines: direction x, y; 1 / length, 1 / width
//  26  16 ghosts, 3 vec4s each: centre x, y, reach squared, 1 / edge; 1 / radius squared r, g, b, 0;
//      colour r, g, b, 0
//  74  12 irises, 3 vec4s each: centre x, y, reach squared, 1 / edge; 1 / radius r, g, b, 0;
//      colour r, g, b, 0
// 110  12 polygon normals: x, y, 0, 0
constexpr std::size_t kStreaks = 18;
constexpr std::size_t kGhosts = 26;
constexpr std::size_t kIrises = 74;
constexpr std::size_t kNormals = 110;

// Below this a sum of the stack is no light at all: the shapes' tails underflow differently on
// devices that flush subnormals, and this is far under any display or file's precision.
constexpr float kSmallestLight = 0x1.0p-30F;
// A disc's soft edge, as a share of its radius squared (ghosts) or its radius (irises).
constexpr double kGhostEdge = 0.2;
constexpr double kIrisEdge = 0.12;
// How far the chromatic split moves the red and blue radii.
constexpr double kChromaticShare = 0.2;
// The smallest length the stack draws: its inverse stays well inside binary32.
constexpr double kSmallestLength = 1.0e-5;

[[nodiscard]] bool drawable(const double length) noexcept { return length >= kSmallestLength; }

[[nodiscard]] double elementOn(const bool on) noexcept { return on ? 1.0 : 0.0; }

// Every shape below is written out once here and once, in the same order, in the program text.
[[nodiscard]] float bump(const float t) noexcept {
    const float b = std::max(1.0F - t, 0.0F);
    return b * b;
}
[[nodiscard]] float unit(const float t) noexcept { return std::min(std::max(t, 0.0F), 1.0F); }

} // namespace

std::optional<PreparedColorGrade> prepareLensFlareGrade(const LensFlareKernel& kernel,
                                                        const std::span<const double> values) {
    if (values.size() != kLensFlareOperands)
        return std::nullopt;
    for (const double value : values)
        if (!std::isfinite(value))
            return std::nullopt;
    for (const double offset : {values[kPosition], values[kPosition + 1]})
        if (std::abs(offset) > document::kMaximumLensFlarePosition)
            return std::nullopt;
    for (const auto& spec : document::lensFlareParameters()) {
        if (spec.kind != document::LensFlareParameterKind::Scalar)
            continue;
        const double value = values[kScalars + document::lensFlareScalarIndex(spec.role)];
        if (value < spec.minimum || value > spec.maximum)
            return std::nullopt;
    }
    const auto within = [](const std::int64_t value, const std::int64_t lo, const std::int64_t hi) {
        return value >= lo && value <= hi;
    };
    if (!within(kernel.lightMode, document::kLensFlareLight2D, document::kLensFlareLight3D) ||
        !within(kernel.streakCount, 1, document::kMaximumLensFlareStreaks) ||
        !within(kernel.ghostCount, 0, document::kMaximumLensFlareGhosts) ||
        !within(kernel.seed, 0, 9999) ||
        !within(kernel.irisCount, 0, document::kMaximumLensFlareIrises) ||
        !within(kernel.irisBlades, document::kMinimumLensFlareBlades,
                document::kMaximumLensFlareBlades) ||
        !within(kernel.occlusionChannel, 0, render::kMatteSelectionStoredValueCount - 1))
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::LensFlare,
                             std::vector<float>(kLensFlareTableFloats, 0.0F), false};
    auto& d = grade.data;
    for (std::size_t index = 0; index < kLensFlareOperands; ++index)
        d[index] = static_cast<float>(values[index]);
    d[kLightMode] = static_cast<float>(kernel.lightMode);
    d[kStreakCount] = static_cast<float>(kernel.streakCount);
    d[kGhostCount] = static_cast<float>(kernel.ghostCount);
    d[kSeed] = static_cast<float>(kernel.seed);
    d[kIrisCount] = static_cast<float>(kernel.irisCount);
    d[kIrisBlades] = static_cast<float>(kernel.irisBlades);
    d[kOcclusionChannel] = static_cast<float>(kernel.occlusionChannel);
    double x = 0.0;
    double y = 0.0;
    const bool visible = lightOffset(d, x, y);
    const bool ghosts = scalarOf(d, "ghostAmount") > 0.0 && kernel.ghostCount > 0;
    const bool irises = scalarOf(d, "irisAmount") > 0.0 && kernel.irisCount > 0;
    const bool any = scalarOf(d, "glowAmount") > 0.0 || scalarOf(d, "streakAmount") > 0.0 ||
                     ghosts || irises || scalarOf(d, "ringAmount") > 0.0 ||
                     scalarOf(d, "hoopAmount") > 0.0 || scalarOf(d, "anamorphicAmount") > 0.0;
    // No light, no element or a light behind the camera adds nothing.
    grade.identity = !visible || !any || scalarOf(d, "intensity") == 0.0;
    return grade;
}

std::optional<LensFlareProbes>
lensFlareProbes(const PreparedColorGrade& grade, const render::ImageWindow& display,
                const double horizontalScale, const double verticalScale,
                const bool hasOcclusion) {
    if (grade.kind != ColorGradeKind::LensFlare || grade.data.size() != kLensFlareTableFloats ||
        !std::isfinite(horizontalScale) || !std::isfinite(verticalScale) ||
        !(horizontalScale > 0.0) || !(verticalScale > 0.0))
        return std::nullopt;
    const auto& table = grade.data;
    double offsetX = 0.0;
    double offsetY = 0.0;
    if (!lightOffset(table, offsetX, offsetY))
        return std::nullopt;
    // The disc in evaluation pixels: each sample a whole-pixel offset from the light's pixel.
    const double radius = scalarOf(table, "sampleRadius");
    std::array<std::array<float, 2>, kLensFlareProbeSamples> offsets{};
    std::int64_t reach = 0;
    for (std::size_t i = 0; i < kLensFlareProbeSamples; ++i) {
        // A sunflower spiral: an even disc from few samples, the first at the light itself.
        const double share = std::sqrt(static_cast<double>(i) /
                                       static_cast<double>(kLensFlareProbeSamples - 1));
        const double angle =
            static_cast<double>(i) * std::numbers::pi * (3.0 - std::sqrt(5.0));
        const auto dx = std::llround(radius * horizontalScale * share * std::cos(angle));
        const auto dy = std::llround(radius * verticalScale * share * std::sin(angle));
        offsets[i] = {static_cast<float>(dx), static_cast<float>(dy)};
        reach = std::max({reach, static_cast<std::int64_t>(std::llabs(dx)),
                          static_cast<std::int64_t>(std::llabs(dy))});
    }
    const double lightX = static_cast<double>(display.originX()) +
                          0.5 * static_cast<double>(display.extent().width()) +
                          offsetX * horizontalScale;
    const double lightY = static_cast<double>(display.originY()) +
                          0.5 * static_cast<double>(display.extent().height()) +
                          offsetY * verticalScale;
    constexpr double kLimit = 1.0e12;
    if (!std::isfinite(lightX) || !std::isfinite(lightY) || std::abs(lightX) > kLimit ||
        std::abs(lightY) > kLimit)
        return std::nullopt;
    const auto pixelX = static_cast<std::int64_t>(std::floor(lightX));
    const auto pixelY = static_cast<std::int64_t>(std::floor(lightY));
    const auto side = static_cast<std::uint64_t>(2 * reach + 1);
    const auto window = render::ImageWindow::create(pixelX - reach, pixelY - reach, side, side);
    if (!window)
        return std::nullopt;
    LensFlareProbes probes{*window.value(), false, false, {}, {}};
    probes.source = scalarOf(table, "sourceBrightness") > 0.0;
    probes.occlusion = hasOcclusion && scalarOf(table, "occlusionAmount") > 0.0;
    const auto dataFor = [&](const std::array<float, 4>& weights, const bool clampSamples) {
        std::vector<float> data(kLensFlareProbeFloats, 0.0F);
        data[0] = static_cast<float>(kLensFlareProbeSamples);
        data[1] = static_cast<float>(reach);
        data[2] = 1.0F / static_cast<float>(kLensFlareProbeSamples);
        data[3] = clampSamples ? 1.0F : 0.0F;
        std::copy(weights.begin(), weights.end(), data.begin() + 4);
        for (std::size_t i = 0; i < kLensFlareProbeSamples; ++i) {
            data[8 + i * 4] = offsets[i][0];
            data[8 + i * 4 + 1] = offsets[i][1];
        }
        return data;
    };
    if (probes.source)
        probes.sourceData = dataFor({0.2126F, 0.7152F, 0.0722F, 0.0F}, false);
    if (probes.occlusion) {
        const auto channel = integerOf(table, kOcclusionChannel) % 5;
        const std::array<std::array<float, 4>, 5> weights{{{0.0F, 0.0F, 0.0F, 1.0F},
                                                           {1.0F, 0.0F, 0.0F, 0.0F},
                                                           {0.0F, 1.0F, 0.0F, 0.0F},
                                                           {0.0F, 0.0F, 1.0F, 0.0F},
                                                           {0.2126F, 0.7152F, 0.0722F, 0.0F}}};
        probes.occlusionData = dataFor(weights[static_cast<std::size_t>(channel)], true);
    }
    return probes;
}

bool validLensFlareProbeData(const std::vector<float>& d) noexcept {
    if (d.size() != kLensFlareProbeFloats ||
        !std::ranges::all_of(d, [](const float value) { return std::isfinite(value); }))
        return false;
    return d[0] == static_cast<float>(kLensFlareProbeSamples) && d[1] >= 0.0F && d[2] > 0.0F &&
           (d[3] == 0.0F || d[3] == 1.0F);
}

float lensFlareProbeValue(const std::vector<float>& k, const render::ImageWindow& window,
                          const render::Rgba32fImage* const image) {
    if (image == nullptr)
        return 0.0F;
    const auto data = image->descriptor()->dataWindow();
    const auto reach = static_cast<std::int64_t>(k[1]);
    const auto centreX = window.originX() + reach;
    const auto centreY = window.originY() + reach;
    float sum = 0.0F;
    for (std::size_t i = 0; i < kLensFlareProbeSamples; ++i) {
        const auto x = centreX + static_cast<std::int64_t>(k[8 + i * 4]);
        const auto y = centreY + static_cast<std::int64_t>(k[8 + i * 4 + 1]);
        // Outside the image a sample is transparent, as the program's cut-out pads it.
        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        float a = 0.0F;
        if (x >= data.originX() && x < data.maxXExclusive() && y >= data.originY() &&
            y < data.maxYExclusive()) {
            const auto pixel = image->read(x, y);
            if (pixel) {
                r = pixel.value()->red();
                g = pixel.value()->green();
                b = pixel.value()->blue();
                a = pixel.value()->alpha();
            }
        }
        const float v = r * k[4] + g * k[5] + b * k[6] + a * k[7];
        sum = sum + (k[3] != 0.0F ? unit(v) : v);
    }
    return sum * k[2];
}

const std::string& lensFlareProbeProgramText() {
    // lensFlareProbeValue's operations; only the window's first pixel holds the mean, which the
    // host cuts out, so the disc is read once however large the window is.
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomLensFlareProbeData { vec4 "
        "values[]; } bloom_lens_flare_probe_data;\n"
        "vec4 bloom_lens_flare_probe(ivec2 c) {\n"
        " if (c.x != 0 || c.y != 0) { return vec4(0.0, 0.0, 0.0, 1.0); }\n"
        " vec4 k = bloom_lens_flare_probe_data.values[0];\n"
        " vec4 w = bloom_lens_flare_probe_data.values[1];\n"
        " int reach = int(k.y);\n"
        " precise float sum = 0.0;\n"
        " for (int i = 0; i < int(k.x); ++i) {\n"
        "  vec4 o = bloom_lens_flare_probe_data.values[2 + i];\n"
        "  vec4 s = imageLoad(bloom_ocio_input, ivec2(reach + int(o.x), reach + int(o.y)));\n"
        "  precise float v = s.r * w.x + s.g * w.y + s.b * w.z + s.a * w.w;\n"
        "  if (k.w != 0.0) { v = min(max(v, 0.0), 1.0); }\n"
        "  sum = sum + v;\n"
        " }\n"
        " precise float mean = sum * k.z;\n"
        " return vec4(mean, mean, mean, 1.0);\n"
        "}\n";
    return text;
}

std::optional<render::ImageWindow> lensFlareOutputWindow(const render::ImageWindow& data,
                                                         const render::ImageWindow& display) {
    const auto left = std::min(data.originX(), display.originX());
    const auto top = std::min(data.originY(), display.originY());
    const auto right = std::max(data.maxXExclusive(), display.maxXExclusive());
    const auto bottom = std::max(data.maxYExclusive(), display.maxYExclusive());
    const auto window = render::ImageWindow::create(left, top, static_cast<std::uint64_t>(right - left),
                                                    static_cast<std::uint64_t>(bottom - top));
    if (!window)
        return std::nullopt;
    return *window.value();
}

std::optional<std::vector<float>>
lensFlareProgramData(const PreparedColorGrade& grade, const render::ImageWindow& output,
                     const render::ImageWindow& display, const double pixelAspect,
                     const double horizontalScale, const double verticalScale,
                     const LensFlareProbeRead& source, const LensFlareProbeRead& occlusion) {
    if (grade.kind != ColorGradeKind::LensFlare || grade.data.size() != kLensFlareTableFloats ||
        !std::isfinite(pixelAspect) || !(pixelAspect > 0.0) || !std::isfinite(horizontalScale) ||
        !std::isfinite(verticalScale) || !(horizontalScale > 0.0) || !(verticalScale > 0.0))
        return std::nullopt;
    const auto& t = grade.data;
    const auto width = static_cast<double>(display.extent().width());
    const auto height = static_cast<double>(display.extent().height());
    if (!(width > 0.0) || !(height > 0.0))
        return std::nullopt;
    double offsetX = 0.0;
    double offsetY = 0.0;
    if (!lightOffset(t, offsetX, offsetY))
        return std::nullopt;
    // Everything below is measured in full-resolution frame heights, so a flare keeps its shape at
    // any proxy scale and pixel aspect.
    const double frameHeight = height / verticalScale;
    const double lightX = offsetX * pixelAspect / frameHeight;
    const double lightY = offsetY / frameHeight;
    const double lightLength = std::hypot(lightX, lightY);
    std::vector<float> d(kLensFlareProgramFloats, 0.0F);
    const auto set = [&](const std::size_t row, const std::array<double, 4>& values) {
        for (std::size_t i = 0; i < 4; ++i)
            d[row * 4 + i] = static_cast<float>(values[i]);
    };
    set(0, {static_cast<double>(output.extent().width()),
            static_cast<double>(output.extent().height()),
            static_cast<double>(display.originX() - output.originX()) + 0.5 * width - 0.5,
            static_cast<double>(display.originY() - output.originY()) + 0.5 * height - 0.5});
    set(1, {pixelAspect / (horizontalScale * frameHeight), 1.0 / (verticalScale * frameHeight),
            lightX, lightY});
    // Away from the light; it fades to nothing as the light nears the centre, so the hoop's arc
    // opens to a whole faint circle without a jump.
    const double axisShare = lightLength > 0.0 ? std::min(lightLength * 4.0, 1.0) / lightLength : 0.0;
    set(2, {-lightX * axisShare, -lightY * axisShare, static_cast<double>(kSmallestLight), 0.0});
    set(3, {static_cast<double>(source.x), static_cast<double>(source.y),
            static_cast<double>(source.value),
            source.active ? scalarOf(t, "sourceBrightness") : 0.0});
    set(4, {static_cast<double>(occlusion.x), static_cast<double>(occlusion.y),
            static_cast<double>(occlusion.value),
            occlusion.active ? scalarOf(t, "occlusionAmount") : 0.0});
    const double size = scalarOf(t, "flareSize");
    const double intensity = scalarOf(t, "intensity");
    const auto rgb = [&](const std::size_t base, const double amount) {
        return std::array<double, 3>{
            std::max(static_cast<double>(t[base]), 0.0) * intensity * amount,
            std::max(static_cast<double>(t[base + 1]), 0.0) * intensity * amount,
            std::max(static_cast<double>(t[base + 2]), 0.0) * intensity * amount};
    };
    const double split = kChromaticShare * scalarOf(t, "chromatic");
    const std::array<double, 3> channels{1.0 + split, 1.0, 1.0 - split};

    // The layout, in frame heights.
    document::LensFlareLayoutInput input;
    input.lightX = lightX;
    input.lightY = lightY;
    input.size = size;
    input.rotationDegrees = scalarOf(t, "flareRotation");
    input.seed = integerOf(t, kSeed);
    input.streakCount = integerOf(t, kStreakCount);
    input.streakLength = shareOf(t, "streakLength");
    input.ghostCount = integerOf(t, kGhostCount);
    input.ghostSize = shareOf(t, "ghostSize");
    input.ghostSpread = scalarOf(t, "ghostSpread");
    input.irisCount = integerOf(t, kIrisCount);
    input.irisBlades = integerOf(t, kIrisBlades);
    input.irisSize = shareOf(t, "irisSize");
    input.irisSpread = scalarOf(t, "irisSpread");
    const auto layout = document::lensFlareLayout(input);

    const double glowRadius = 3.0 * size * shareOf(t, "glowSize");
    const bool glow = scalarOf(t, "glowAmount") > 0.0 && drawable(glowRadius);
    const double ringRadius = size * shareOf(t, "ringSize");
    const double ringWidth = size * shareOf(t, "ringWidth");
    const bool ring = scalarOf(t, "ringAmount") > 0.0 && drawable(ringRadius) && drawable(ringWidth);
    const double hoopRadius = size * shareOf(t, "hoopSize");
    const double hoopWidth = size * shareOf(t, "hoopWidth");
    const bool hoop = scalarOf(t, "hoopAmount") > 0.0 && drawable(hoopRadius) && drawable(hoopWidth);
    const double anamorphicLength = size * shareOf(t, "anamorphicLength");
    const double anamorphicWidth = size * shareOf(t, "anamorphicWidth");
    const bool anamorphic = scalarOf(t, "anamorphicAmount") > 0.0 && drawable(anamorphicLength) &&
                            drawable(anamorphicWidth);
    const double streakWidth = size * shareOf(t, "streakWidth");
    const bool streaks = scalarOf(t, "streakAmount") > 0.0 && drawable(streakWidth) &&
                         std::ranges::all_of(layout.streaks, [](const auto& line) {
                             return drawable(line.length);
                         });
    const bool ghosts =
        scalarOf(t, "ghostAmount") > 0.0 &&
        std::ranges::all_of(layout.ghosts, [](const auto& disc) { return drawable(disc.radius); });
    const bool irises =
        scalarOf(t, "irisAmount") > 0.0 &&
        std::ranges::all_of(layout.irises, [](const auto& disc) { return drawable(disc.radius); });

    const auto sourceWhite = scalarOf(t, "sourceWhite");
    set(5, {1.0 / sourceWhite,
            integerOf(t, kOcclusionChannel) >= render::kMatteSelectionStoredValueCount / 2 ? 1.0
                                                                                           : 0.0,
            elementOn(glow), elementOn(ring)});
    double streakReach = 0.0;
    for (const auto& line : layout.streaks)
        streakReach = std::max(streakReach, line.length);
    set(6, {elementOn(hoop), elementOn(anamorphic),
            streaks ? static_cast<double>(layout.streaks.size()) : 0.0,
            ghosts ? static_cast<double>(layout.ghosts.size()) : 0.0});
    set(7, {irises ? static_cast<double>(layout.irises.size()) : 0.0,
            static_cast<double>(layout.irisNormals.size()), streakReach * streakReach, 0.0});
    if (glow) {
        const auto c = rgb(kColor, scalarOf(t, "glowAmount"));
        const double core = glowRadius / 3.0;
        set(8, {c[0], c[1], c[2], 1.0 / (core * core)});
        set(9, {1.0 / (glowRadius * glowRadius), 0.0, 0.0, 0.0});
    }
    if (streaks) {
        const auto c = rgb(kColor, scalarOf(t, "streakAmount"));
        set(10, {c[0], c[1], c[2], 0.0});
        for (std::size_t i = 0; i < layout.streaks.size(); ++i) {
            const auto& line = layout.streaks[i];
            set(kStreaks + i,
                {line.directionX, line.directionY, 1.0 / line.length, 1.0 / streakWidth});
        }
    }
    const auto radii = [&](const double radius) {
        return std::array<double, 3>{radius * channels[0] * radius * channels[0],
                                     radius * channels[1] * radius * channels[1],
                                     radius * channels[2] * radius * channels[2]};
    };
    if (ring) {
        const auto c = rgb(kColor, scalarOf(t, "ringAmount"));
        const auto r = radii(ringRadius);
        const double outer = ringRadius * channels[0] + 1.5 * ringWidth;
        set(11, {c[0], c[1], c[2], 1.0 / (2.0 * ringRadius * ringWidth)});
        set(12, {r[0], r[1], r[2], outer * outer});
    }
    if (hoop) {
        const auto c = rgb(kColor, scalarOf(t, "hoopAmount"));
        const auto r = radii(hoopRadius);
        const double outer = hoopRadius * channels[0] + 1.5 * hoopWidth;
        const double inner = std::max(hoopRadius * channels[2] - 1.5 * hoopWidth, 0.0);
        set(13, {c[0], c[1], c[2], 1.0 / (2.0 * hoopRadius * hoopWidth)});
        set(14, {r[0], r[1], r[2], 1.0 / hoopRadius});
        set(15, {inner * inner, outer * outer, 0.0, 0.0});
    }
    if (anamorphic) {
        const auto c = rgb(kAnamorphicColor, scalarOf(t, "anamorphicAmount"));
        const double turn = scalarOf(t, "flareRotation") * std::numbers::pi / 180.0;
        set(16, {c[0], c[1], c[2], 0.0});
        set(17, {std::cos(turn), std::sin(turn), 1.0 / anamorphicLength, 1.0 / anamorphicWidth});
    }
    const auto disc = [&](const std::size_t row, const document::LensFlareDisc& item,
                          const double amount, const double reachShare, const double edge,
                          const bool squared) {
        const double reach = item.radius * channels[0] * reachShare;
        set(row, {item.x, item.y, reach * reach, 1.0 / edge});
        std::array<double, 3> inverse{};
        for (std::size_t i = 0; i < 3; ++i) {
            const double radius = item.radius * channels[i];
            inverse[i] = squared ? 1.0 / (radius * radius) : 1.0 / radius;
        }
        set(row + 1, {inverse[0], inverse[1], inverse[2], 0.0});
        const auto c = rgb(kColor, amount);
        set(row + 2, {c[0] * item.tint[0], c[1] * item.tint[1], c[2] * item.tint[2], 0.0});
    };
    if (ghosts)
        for (std::size_t i = 0; i < layout.ghosts.size(); ++i)
            disc(kGhosts + 3 * i, layout.ghosts[i], scalarOf(t, "ghostAmount"), 1.0, kGhostEdge,
                 true);
    if (irises) {
        // The polygon's corners reach past its edges' distance by 1 / cos(pi / blades).
        const double corner =
            1.0 / std::cos(std::numbers::pi / static_cast<double>(layout.irisNormals.size()));
        for (std::size_t i = 0; i < layout.irises.size(); ++i)
            disc(kIrises + 3 * i, layout.irises[i], scalarOf(t, "irisAmount"), corner, kIrisEdge,
                 false);
        for (std::size_t i = 0; i < layout.irisNormals.size(); ++i)
            set(kNormals + i, {layout.irisNormals[i][0], layout.irisNormals[i][1], 0.0, 0.0});
    }
    if (!validLensFlareProgramData(d))
        return std::nullopt;
    return d;
}

bool validLensFlareProgramData(const std::vector<float>& d) noexcept {
    if (d.size() != kLensFlareProgramFloats ||
        !std::ranges::all_of(d, [](const float value) { return std::isfinite(value); }))
        return false;
    const auto at = [&](const std::size_t row, const std::size_t column) {
        return d[row * 4 + column];
    };
    return at(0, 0) >= 1.0F && at(0, 1) >= 1.0F &&
           at(6, 2) >= 0.0F && at(6, 2) <= static_cast<float>(document::kMaximumLensFlareStreaks) &&
           at(6, 3) >= 0.0F && at(6, 3) <= static_cast<float>(document::kMaximumLensFlareGhosts) &&
           at(7, 0) >= 0.0F && at(7, 0) <= static_cast<float>(document::kMaximumLensFlareIrises) &&
           at(7, 1) >= static_cast<float>(document::kMinimumLensFlareBlades) &&
           at(7, 1) <= static_cast<float>(document::kMaximumLensFlareBlades);
}

float lensFlareLevel(const float* const k, const float sourceValue,
                     const float occlusionValue) noexcept {
    float gain = 1.0F;
    const float share = k[3 * 4 + 3];
    if (share != 0.0F)
        gain = 1.0F + share * (unit(sourceValue * k[5 * 4]) - 1.0F);
    float visible = 1.0F;
    const float amount = k[4 * 4 + 3];
    if (amount != 0.0F) {
        float covered = unit(occlusionValue);
        if (k[5 * 4 + 1] != 0.0F)
            covered = 1.0F - covered;
        visible = 1.0F - amount * covered;
    }
    return gain * visible;
}

bool lensFlarePixel(const float* const k, const float level, const std::int64_t x,
                    const std::int64_t y, std::array<float, 4>& pixel) noexcept {
    const auto v = [k](const std::size_t row) { return k + row * 4; };
    const float a = pixel[3];
    if (level == 0.0F) {
        if (a == 0.0F)
            pixel = {0.0F, 0.0F, 0.0F, 0.0F};
        return true;
    }
    const float cx = (static_cast<float>(x) - v(0)[2]) * v(1)[0];
    const float cy = (static_cast<float>(y) - v(0)[3]) * v(1)[1];
    const float dx = cx - v(1)[2];
    const float dy = cy - v(1)[3];
    const float r2 = dx * dx + dy * dy;
    float fr = 0.0F;
    float fg = 0.0F;
    float fb = 0.0F;
    // Glow: a tight core over a wide halo.
    if (v(5)[2] != 0.0F) {
        const float core = bump(r2 * v(8)[3]);
        const float halo = std::max(1.0F - r2 * v(9)[0], 0.0F);
        const float g = 0.625F * (core * core * core * core) + 0.375F * (halo * halo * halo);
        fr = fr + v(8)[0] * g;
        fg = fg + v(8)[1] * g;
        fb = fb + v(8)[2] * g;
    }
    // Streaks: needles, narrowing to their tips.
    const auto streaks = static_cast<int>(v(6)[2]);
    if (streaks > 0 && r2 < v(7)[2]) {
        float s = 0.0F;
        for (int i = 0; i < streaks; ++i) {
            const float* line = v(kStreaks + static_cast<std::size_t>(i));
            const float along = std::abs(dx * line[0] + dy * line[1]);
            const float across = std::abs(dx * line[1] - dy * line[0]);
            const float b = std::max(1.0F - (along * line[2] + across * line[3]), 0.0F);
            s = s + b * b * b;
        }
        fr = fr + v(10)[0] * s;
        fg = fg + v(10)[1] * s;
        fb = fb + v(10)[2] * s;
    }
    // Ring: around the light, one radius a channel.
    if (v(5)[3] != 0.0F && r2 < v(12)[3]) {
        const float tr = (r2 - v(12)[0]) * v(11)[3];
        const float tg = (r2 - v(12)[1]) * v(11)[3];
        const float tb = (r2 - v(12)[2]) * v(11)[3];
        fr = fr + v(11)[0] * bump(tr * tr);
        fg = fg + v(11)[1] * bump(tg * tg);
        fb = fb + v(11)[2] * bump(tb * tb);
    }
    // Hoop: around the frame's centre, on the side away from the light.
    if (v(6)[0] != 0.0F) {
        const float c2 = cx * cx + cy * cy;
        if (c2 > v(15)[0] && c2 < v(15)[1]) {
            const float side =
                unit(0.5F + 0.5F * ((cx * v(2)[0] + cy * v(2)[1]) * v(14)[3]));
            const float arc = side * side;
            const float tr = (c2 - v(14)[0]) * v(13)[3];
            const float tg = (c2 - v(14)[1]) * v(13)[3];
            const float tb = (c2 - v(14)[2]) * v(13)[3];
            fr = fr + v(13)[0] * bump(tr * tr) * arc;
            fg = fg + v(13)[1] * bump(tg * tg) * arc;
            fb = fb + v(13)[2] * bump(tb * tb) * arc;
        }
    }
    // Anamorphic: one long thin streak in its own colour.
    if (v(6)[1] != 0.0F) {
        const float along = std::abs(dx * v(17)[0] + dy * v(17)[1]);
        const float across = std::abs(dx * v(17)[1] - dy * v(17)[0]);
        const float b = bump(along * v(17)[2] + across * v(17)[3]);
        fr = fr + v(16)[0] * b;
        fg = fg + v(16)[1] * b;
        fb = fb + v(16)[2] * b;
    }
    // Ghosts: soft discs brightening to their rims, one radius a channel.
    const auto ghosts = static_cast<int>(v(6)[3]);
    for (int i = 0; i < ghosts; ++i) {
        const float* g = v(kGhosts + 3 * static_cast<std::size_t>(i));
        const float qx = cx - g[0];
        const float qy = cy - g[1];
        const float q2 = qx * qx + qy * qy;
        if (q2 < g[2]) {
            const float tr = q2 * g[4];
            const float tg = q2 * g[5];
            const float tb = q2 * g[6];
            const float er = unit((1.0F - tr) * g[3]);
            const float eg = unit((1.0F - tg) * g[3]);
            const float eb = unit((1.0F - tb) * g[3]);
            fr = fr + g[8] * (er * (0.5625F + 0.4375F * (tr * tr)));
            fg = fg + g[9] * (eg * (0.5625F + 0.4375F * (tg * tg)));
            fb = fb + g[10] * (eb * (0.5625F + 0.4375F * (tb * tb)));
        }
    }
    // Iris: polygon ghosts, the aperture's shape.
    const auto irises = static_cast<int>(v(7)[0]);
    const auto blades = static_cast<int>(v(7)[1]);
    for (int i = 0; i < irises; ++i) {
        const float* g = v(kIrises + 3 * static_cast<std::size_t>(i));
        const float qx = cx - g[0];
        const float qy = cy - g[1];
        const float q2 = qx * qx + qy * qy;
        if (q2 < g[2]) {
            float edge = 0.0F;
            for (int b = 0; b < blades; ++b) {
                const float* n = v(kNormals + static_cast<std::size_t>(b));
                edge = std::max(edge, qx * n[0] + qy * n[1]);
            }
            const float tr = edge * g[4];
            const float tg = edge * g[5];
            const float tb = edge * g[6];
            const float er = unit((1.0F - tr) * g[3]);
            const float eg = unit((1.0F - tg) * g[3]);
            const float eb = unit((1.0F - tb) * g[3]);
            fr = fr + g[8] * (er * (0.625F + 0.375F * tr));
            fg = fg + g[9] * (eg * (0.625F + 0.375F * tg));
            fb = fb + g[10] * (eb * (0.625F + 0.375F * tb));
        }
    }
    fr = fr * level;
    fg = fg * level;
    fb = fb * level;
    const float light = std::max(std::max(fr, fg), fb);
    if (light < v(2)[2]) {
        if (a == 0.0F)
            pixel = {0.0F, 0.0F, 0.0F, 0.0F};
        return true;
    }
    pixel[0] = pixel[0] + fr;
    pixel[1] = pixel[1] + fg;
    pixel[2] = pixel[2] + fb;
    pixel[3] = a + (1.0F - a) * std::min(light, 1.0F);
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2]);
}

const std::string& lensFlareProgramText() {
    // lensFlarePixel's operations in the same order. The wrapper makes alpha 0 transparent black and
    // refuses a non-finite result.
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomLensFlareData { vec4 values[]; "
        "} bloom_lens_flare_data;\n"
        "precise float bloom_lens_flare_bump(float t) {\n"
        " precise float b = max(1.0 - t, 0.0);\n"
        " precise float r = b * b;\n"
        " return r;\n"
        "}\n"
        "float bloom_lens_flare_unit(float t) { return min(max(t, 0.0), 1.0); }\n"
        "float bloom_lens_flare_read(vec4 probe) {\n"
        " if (probe.x < 0.0) { return probe.z; }\n"
        " return imageLoad(bloom_ocio_input, ivec2(int(probe.x), int(probe.y))).r;\n"
        "}\n"
        "vec4 bloom_lens_flare(ivec2 c) {\n"
        " vec4 p = imageLoad(bloom_ocio_input, c);\n"
        " vec4 k0 = bloom_lens_flare_data.values[0];\n"
        " if (float(c.x) >= k0.x || float(c.y) >= k0.y) { return p; }\n"
        " float a = p.a;\n"
        " vec4 k1 = bloom_lens_flare_data.values[1];\n"
        " vec4 k2 = bloom_lens_flare_data.values[2];\n"
        " vec4 k3 = bloom_lens_flare_data.values[3];\n"
        " vec4 k4 = bloom_lens_flare_data.values[4];\n"
        " vec4 k5 = bloom_lens_flare_data.values[5];\n"
        " vec4 k6 = bloom_lens_flare_data.values[6];\n"
        " vec4 k7 = bloom_lens_flare_data.values[7];\n"
        " precise float gain = 1.0;\n"
        " if (k3.w != 0.0) {\n"
        "  precise float reading = bloom_lens_flare_read(k3) * k5.x;\n"
        "  gain = 1.0 + k3.w * (bloom_lens_flare_unit(reading) - 1.0);\n"
        " }\n"
        " precise float visible = 1.0;\n"
        " if (k4.w != 0.0) {\n"
        "  precise float covered = bloom_lens_flare_unit(bloom_lens_flare_read(k4));\n"
        "  if (k5.y != 0.0) { covered = 1.0 - covered; }\n"
        "  visible = 1.0 - k4.w * covered;\n"
        " }\n"
        " precise float level = gain * visible;\n"
        " if (level == 0.0) { return p; }\n"
        " precise float cx = (float(c.x) - k0.z) * k1.x;\n"
        " precise float cy = (float(c.y) - k0.w) * k1.y;\n"
        " precise float dx = cx - k1.z;\n"
        " precise float dy = cy - k1.w;\n"
        " precise float r2 = dx * dx + dy * dy;\n"
        " precise float fr = 0.0;\n"
        " precise float fg = 0.0;\n"
        " precise float fb = 0.0;\n"
        " if (k5.z != 0.0) {\n"
        "  vec4 g0 = bloom_lens_flare_data.values[8];\n"
        "  vec4 g1 = bloom_lens_flare_data.values[9];\n"
        "  precise float t = r2 * g0.w;\n"
        "  precise float core = bloom_lens_flare_bump(t);\n"
        "  precise float halo = max(1.0 - r2 * g1.x, 0.0);\n"
        "  precise float g = 0.625 * (core * core * core * core) + 0.375 * (halo * halo * halo);\n"
        "  fr = fr + g0.x * g;\n"
        "  fg = fg + g0.y * g;\n"
        "  fb = fb + g0.z * g;\n"
        " }\n"
        " int streaks = int(k6.z);\n"
        " if (streaks > 0 && r2 < k7.z) {\n"
        "  precise float s = 0.0;\n"
        "  for (int i = 0; i < streaks; ++i) {\n"
        "   vec4 line = bloom_lens_flare_data.values[18 + i];\n"
        "   precise float along = abs(dx * line.x + dy * line.y);\n"
        "   precise float across = abs(dx * line.y - dy * line.x);\n"
        "   precise float b = max(1.0 - (along * line.z + across * line.w), 0.0);\n"
        "   s = s + b * b * b;\n"
        "  }\n"
        "  vec4 m = bloom_lens_flare_data.values[10];\n"
        "  fr = fr + m.x * s;\n"
        "  fg = fg + m.y * s;\n"
        "  fb = fb + m.z * s;\n"
        " }\n"
        " if (k5.w != 0.0) {\n"
        "  vec4 m = bloom_lens_flare_data.values[11];\n"
        "  vec4 n = bloom_lens_flare_data.values[12];\n"
        "  if (r2 < n.w) {\n"
        "   precise float tr = (r2 - n.x) * m.w;\n"
        "   precise float tg = (r2 - n.y) * m.w;\n"
        "   precise float tb = (r2 - n.z) * m.w;\n"
        "   precise float sr = tr * tr;\n"
        "   precise float sg = tg * tg;\n"
        "   precise float sb = tb * tb;\n"
        "   fr = fr + m.x * bloom_lens_flare_bump(sr);\n"
        "   fg = fg + m.y * bloom_lens_flare_bump(sg);\n"
        "   fb = fb + m.z * bloom_lens_flare_bump(sb);\n"
        "  }\n"
        " }\n"
        " if (k6.x != 0.0) {\n"
        "  vec4 m = bloom_lens_flare_data.values[13];\n"
        "  vec4 n = bloom_lens_flare_data.values[14];\n"
        "  vec4 o = bloom_lens_flare_data.values[15];\n"
        "  precise float c2 = cx * cx + cy * cy;\n"
        "  if (c2 > o.x && c2 < o.y) {\n"
        "   precise float toward = 0.5 + 0.5 * ((cx * k2.x + cy * k2.y) * n.w);\n"
        "   precise float side = bloom_lens_flare_unit(toward);\n"
        "   precise float arc = side * side;\n"
        "   precise float tr = (c2 - n.x) * m.w;\n"
        "   precise float tg = (c2 - n.y) * m.w;\n"
        "   precise float tb = (c2 - n.z) * m.w;\n"
        "   precise float sr = tr * tr;\n"
        "   precise float sg = tg * tg;\n"
        "   precise float sb = tb * tb;\n"
        "   fr = fr + m.x * bloom_lens_flare_bump(sr) * arc;\n"
        "   fg = fg + m.y * bloom_lens_flare_bump(sg) * arc;\n"
        "   fb = fb + m.z * bloom_lens_flare_bump(sb) * arc;\n"
        "  }\n"
        " }\n"
        " if (k6.y != 0.0) {\n"
        "  vec4 m = bloom_lens_flare_data.values[16];\n"
        "  vec4 n = bloom_lens_flare_data.values[17];\n"
        "  precise float along = abs(dx * n.x + dy * n.y);\n"
        "  precise float across = abs(dx * n.y - dy * n.x);\n"
        "  precise float t = along * n.z + across * n.w;\n"
        "  precise float b = bloom_lens_flare_bump(t);\n"
        "  fr = fr + m.x * b;\n"
        "  fg = fg + m.y * b;\n"
        "  fb = fb + m.z * b;\n"
        " }\n"
        " int ghosts = int(k6.w);\n"
        " for (int i = 0; i < ghosts; ++i) {\n"
        "  vec4 g = bloom_lens_flare_data.values[26 + 3 * i];\n"
        "  precise float qx = cx - g.x;\n"
        "  precise float qy = cy - g.y;\n"
        "  precise float q2 = qx * qx + qy * qy;\n"
        "  if (q2 < g.z) {\n"
        "   vec4 u = bloom_lens_flare_data.values[27 + 3 * i];\n"
        "   vec4 m = bloom_lens_flare_data.values[28 + 3 * i];\n"
        "   precise float tr = q2 * u.x;\n"
        "   precise float tg = q2 * u.y;\n"
        "   precise float tb = q2 * u.z;\n"
        "   precise float lr = (1.0 - tr) * g.w;\n"
        "   precise float lg = (1.0 - tg) * g.w;\n"
        "   precise float lb = (1.0 - tb) * g.w;\n"
        "   float er = bloom_lens_flare_unit(lr);\n"
        "   float eg = bloom_lens_flare_unit(lg);\n"
        "   float eb = bloom_lens_flare_unit(lb);\n"
        "   fr = fr + m.x * (er * (0.5625 + 0.4375 * (tr * tr)));\n"
        "   fg = fg + m.y * (eg * (0.5625 + 0.4375 * (tg * tg)));\n"
        "   fb = fb + m.z * (eb * (0.5625 + 0.4375 * (tb * tb)));\n"
        "  }\n"
        " }\n"
        " int irises = int(k7.x);\n"
        " int blades = int(k7.y);\n"
        " for (int i = 0; i < irises; ++i) {\n"
        "  vec4 g = bloom_lens_flare_data.values[74 + 3 * i];\n"
        "  precise float qx = cx - g.x;\n"
        "  precise float qy = cy - g.y;\n"
        "  precise float q2 = qx * qx + qy * qy;\n"
        "  if (q2 < g.z) {\n"
        "   precise float edge = 0.0;\n"
        "   for (int b = 0; b < blades; ++b) {\n"
        "    vec4 n = bloom_lens_flare_data.values[110 + b];\n"
        "    precise float along = qx * n.x + qy * n.y;\n"
        "    edge = max(edge, along);\n"
        "   }\n"
        "   vec4 u = bloom_lens_flare_data.values[75 + 3 * i];\n"
        "   vec4 m = bloom_lens_flare_data.values[76 + 3 * i];\n"
        "   precise float tr = edge * u.x;\n"
        "   precise float tg = edge * u.y;\n"
        "   precise float tb = edge * u.z;\n"
        "   precise float lr = (1.0 - tr) * g.w;\n"
        "   precise float lg = (1.0 - tg) * g.w;\n"
        "   precise float lb = (1.0 - tb) * g.w;\n"
        "   float er = bloom_lens_flare_unit(lr);\n"
        "   float eg = bloom_lens_flare_unit(lg);\n"
        "   float eb = bloom_lens_flare_unit(lb);\n"
        "   fr = fr + m.x * (er * (0.625 + 0.375 * tr));\n"
        "   fg = fg + m.y * (eg * (0.625 + 0.375 * tg));\n"
        "   fb = fb + m.z * (eb * (0.625 + 0.375 * tb));\n"
        "  }\n"
        " }\n"
        " fr = fr * level;\n"
        " fg = fg * level;\n"
        " fb = fb * level;\n"
        " float light = max(max(fr, fg), fb);\n"
        " if (light < k2.z) { return p; }\n"
        " precise vec3 o = vec3(p.r + fr, p.g + fg, p.b + fb);\n"
        " precise float alpha = a + (1.0 - a) * min(light, 1.0);\n"
        " return vec4(o, alpha);\n"
        "}\n";
    return text;
}

ImageEffectResult applyLensFlare(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
                                 const render::Rgba32fImage* const occlusion,
                                 const bool hasOcclusion, const double horizontalScale,
                                 const double verticalScale, const std::size_t budget,
                                 const CancellationToken& cancellation,
                                 const OperationIndex operation,
                                 const EvaluationProgressCallback& progress,
                                 CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto& descriptor = *input.descriptor();
    const auto data = descriptor.dataWindow();
    const auto display = descriptor.displayWindow();
    const auto aspect = descriptor.pixelAspect();
    const double pixelAspect =
        static_cast<double>(aspect.numerator()) / static_cast<double>(aspect.denominator());
    const auto window = lensFlareOutputWindow(data, display);
    if (!window)
        return failed(EvaluationDiagnosticCode::InvalidParameter, "Lens Flare window overflows");
    const auto probes =
        lensFlareProbes(grade, display, horizontalScale, verticalScale, hasOcclusion);
    if (!probes)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Lens Flare geometry cannot be represented");
    LensFlareProbeRead source;
    LensFlareProbeRead covered;
    source.active = probes->source;
    covered.active = probes->occlusion;
    if (probes->source)
        source.value = lensFlareProbeValue(probes->sourceData, probes->window, &input);
    if (probes->occlusion)
        covered.value = lensFlareProbeValue(probes->occlusionData, probes->window, occlusion);
    const auto table = lensFlareProgramData(grade, *window, display, pixelAspect, horizontalScale,
                                            verticalScale, source, covered);
    if (!table)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Lens Flare geometry cannot be represented");
    const auto output =
        render::Rgba32fImageDescriptor::create(*window, display, descriptor.pixelAspect());
    if (!output)
        return failed(EvaluationDiagnosticCode::InvalidParameter, "Lens Flare window is invalid");
    auto builder = render::Rgba32fImageBuilder::create(*output.value(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Lens Flare output exceeds the pixel budget");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Lens Flare image is invalid");
    auto& image = *builder.value();
    const float* const k = table->data();
    const float level = lensFlareLevel(k, source.value, covered.value);
    const auto originX = window->originX();
    const auto originY = window->originY();
    const auto total = static_cast<std::uint32_t>(window->extent().height());
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    const auto& sourceView = *view.value();
    const auto outcome = runRowBandPass(
        rowBands, cancellation, total, originY,
        [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
            auto row = image.row(y);
            if (!row)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Lens Flare row is unavailable",
                                            {}};
            std::span<const render::Rgba32f> from;
            if (y >= data.originY() && y < data.maxYExclusive())
                if (const auto read = sourceView.row(y))
                    from = *read.value();
            for (std::size_t column = 0; column < row.value()->size(); ++column) {
                if ((column & 255U) == 0 && cancellation.isCancellationRequested())
                    return std::nullopt;
                const auto x = originX + static_cast<std::int64_t>(column);
                std::array<float, 4> p{};
                if (!from.empty() && x >= data.originX() && x < data.maxXExclusive()) {
                    const auto& s = from[static_cast<std::size_t>(x - data.originX())];
                    p = {s.red(), s.green(), s.blue(), s.alpha()};
                }
                const bool finite = lensFlarePixel(k, level, static_cast<std::int64_t>(column),
                                                   y - originY, p);
                const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
                if (!finite || !pixel)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Lens Flare produced an invalid pixel",
                                                {}};
                (*row.value())[column] = *pixel.value();
            }
            return std::nullopt;
        });
    if (outcome.cancelled || cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (outcome.incomplete || outcome.failure) {
        ImageEffectResult result;
        result.diagnostic = outcome.failure ? *outcome.failure
                                            : EvaluationDiagnostic{
                                                  EvaluationDiagnosticCode::AllocationFailure,
                                                  DiagnosticSeverity::Error,
                                                  {},
                                                  "A Lens Flare row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Lens Flare publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
