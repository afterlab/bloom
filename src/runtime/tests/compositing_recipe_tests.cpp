// #339 Drop Shadow, Glow and Light Wrap on the CPU reference. Each recipe runs through the
// production chain (compositing_recipes.hpp) and is compared with an independent double-precision
// formula over an HDR, partial-alpha gradient: an explicit two-dimensional Gaussian, the straight
// luma ramp, the normalized octave sum and the Porter-Duff composite, written here without the
// primitives. Then the chain's cache identity (a cold and a warm frame, a changed opacity reusing
// the softened coverage), ROI, animation, cancellation and budget refusal. #506 adds Glow's
// Physical and Streak modes and shared controls against the same independent reference (premultiplied
// luma keying, the inverse-power octave weights, bilinear ray passes written directly, per-channel
// radius, gradient, lens dirt and roll-off), Physical's energy balance, and the Streak chain's
// cache, ROI, cancellation and budget behaviour.

#include "compositing_recipe_test_support.hpp"

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <future>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace bloom;
using recipe_test::Recipe;
using recipe_test::RecipeValues;
using Pixel = std::array<double, 4>;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << message << '\n';
        ++failures;
    }
}

// An independent image: a window and double premultiplied pixels, transparent outside.
struct Image final {
    std::int64_t x0 = 0;
    std::int64_t y0 = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<Pixel> pixels;
    [[nodiscard]] bool inside(const std::int64_t x, const std::int64_t y) const {
        return x >= x0 && y >= y0 && x < x0 + width && y < y0 + height;
    }
    [[nodiscard]] Pixel at(const std::int64_t x, const std::int64_t y) const {
        return inside(x, y) ? pixels[static_cast<std::size_t>((y - y0) * width + (x - x0))]
                            : Pixel{};
    }
    [[nodiscard]] Pixel clamped(std::int64_t x, std::int64_t y) const {
        x = std::clamp(x, x0, x0 + width - 1);
        y = std::clamp(y, y0, y0 + height - 1);
        return at(x, y);
    }
    [[nodiscard]] static Image blank(const std::int64_t x0, const std::int64_t y0,
                                     const std::int64_t width, const std::int64_t height) {
        return {x0, y0, width, height,
                std::vector<Pixel>(static_cast<std::size_t>(width * height), Pixel{})};
    }
    Pixel& operator()(const std::int64_t x, const std::int64_t y) {
        return pixels[static_cast<std::size_t>((y - y0) * width + (x - x0))];
    }
};

[[nodiscard]] runtime::EvaluationRequest request(const runtime::OperationIndex output,
                                                 const core::RationalTime time = {}) {
    runtime::EvaluationRequest value{time, output, runtime::CompositionFormatResolution{}};
    value.pixelStorageByteLimit = 1U << 26U;
    return value;
}

[[nodiscard]] Image imageOf(const render::Rgba32fImage& image) {
    const auto window = image.descriptor()->dataWindow();
    auto result = Image::blank(window.originX(), window.originY(),
                               static_cast<std::int64_t>(window.extent().width()),
                               static_cast<std::int64_t>(window.extent().height()));
    for (auto y = result.y0; y < result.y0 + result.height; ++y)
        for (auto x = result.x0; x < result.x0 + result.width; ++x) {
            const auto pixel = image.read(x, y);
            if (!pixel)
                throw std::logic_error("fixture pixel is unreadable");
            const auto& value = *pixel.value();
            result(x, y) = {static_cast<double>(value.red()), static_cast<double>(value.green()),
                            static_cast<double>(value.blue()), static_cast<double>(value.alpha())};
        }
    return result;
}

[[nodiscard]] Image evaluated(const runtime::CpuCompositionEvaluator& evaluator,
                              const Recipe recipe, const RecipeValues& values,
                              const recipe_test::Published published) {
    const auto plan = recipe_test::recipePlan(recipe, values, 3900, published).plan;
    const auto result = evaluator.evaluate(plan, request(plan->output()), {});
    if (!result.frame())
        throw std::logic_error("recipe fixture input does not evaluate");
    return imageOf(result.frame()->processImage());
}

// sigma radius / 3 over support ceil(radius), normalized (docs/architecture/image-filters.md).
[[nodiscard]] std::vector<double> gaussian(const double radius) {
    const auto support = static_cast<std::int64_t>(std::ceil(radius));
    std::vector<double> weights(static_cast<std::size_t>(2 * support + 1), 0.0);
    if (support == 0) {
        weights[0] = 1.0;
        return weights;
    }
    const double sigma = radius / 3.0;
    double sum = 0.0;
    for (std::int64_t k = -support; k <= support; ++k) {
        const double weight = std::exp(-static_cast<double>(k * k) / (2.0 * sigma * sigma));
        weights[static_cast<std::size_t>(k + support)] = weight;
        sum += weight;
    }
    for (auto& weight : weights)
        weight /= sum;
    return weights;
}

// A direct two-dimensional convolution (not separable passes): Transparent grows the window by
// the support, Clamp keeps it; Coverage filters alpha and writes it to every channel.
[[nodiscard]] Image blur(const Image& source, const double radius, const bool clamp,
                         const bool coverage) {
    const auto weights = gaussian(radius);
    const auto support = static_cast<std::int64_t>(weights.size() / 2);
    const auto grow = clamp ? 0 : support;
    auto out = Image::blank(source.x0 - grow, source.y0 - grow, source.width + 2 * grow,
                            source.height + 2 * grow);
    for (auto y = out.y0; y < out.y0 + out.height; ++y)
        for (auto x = out.x0; x < out.x0 + out.width; ++x) {
            Pixel sum{};
            for (std::int64_t j = -support; j <= support; ++j)
                for (std::int64_t i = -support; i <= support; ++i) {
                    const auto tap = clamp ? source.clamped(x - i, y - j) : source.at(x - i, y - j);
                    const double weight = weights[static_cast<std::size_t>(i + support)] *
                                          weights[static_cast<std::size_t>(j + support)];
                    for (std::size_t c = 0; c < 4; ++c)
                        sum[c] += weight * tap[c];
                }
            if (coverage)
                sum = {sum[3], sum[3], sum[3], sum[3]};
            out(x, y) = sum;
        }
    return out;
}

[[nodiscard]] double straightLuma(const Pixel& pixel) {
    return pixel[3] > 0.0 ? (0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]) / pixel[3]
                          : 0.0;
}

// `top` over `bottom` with a Porter-Duff or Add composite, over the union window.
enum class Composite : std::uint8_t { Over, Add, Atop, In };
[[nodiscard]] Image composite(const Image& top, const Image& bottom, const Composite mode) {
    const auto x0 = std::min(top.x0, bottom.x0);
    const auto y0 = std::min(top.y0, bottom.y0);
    const auto x1 = std::max(top.x0 + top.width, bottom.x0 + bottom.width);
    const auto y1 = std::max(top.y0 + top.height, bottom.y0 + bottom.height);
    auto out = Image::blank(x0, y0, x1 - x0, y1 - y0);
    for (auto y = y0; y < y1; ++y)
        for (auto x = x0; x < x1; ++x) {
            const auto s = top.at(x, y);
            const auto b = bottom.at(x, y);
            Pixel value{};
            for (std::size_t c = 0; c < 4; ++c) {
                switch (mode) {
                case Composite::Over:
                    value[c] = s[c] + b[c] * (1.0 - s[3]);
                    break;
                case Composite::Add:
                    value[c] = c < 3 ? s[c] + b[c] : s[3] + b[3] - s[3] * b[3];
                    break;
                case Composite::Atop:
                    value[c] = s[c] * b[3] + b[c] * (1.0 - s[3]);
                    break;
                case Composite::In:
                    value[c] = s[c] * b[3];
                    break;
                }
            }
            out(x, y) = value;
        }
    return out;
}

// Compares over the union of both windows (transparent outside each).
[[nodiscard]] bool matches(const render::Rgba32fImage& actual, const Image& expected,
                           std::string& detail, const double tolerance = 1e-4) {
    // Composition Output crops to the composition window, so that is where the published
    // pixels are compared; the recipe's overscan halo is checked by its window there.
    const Image got = imageOf(actual);
    for (std::int64_t y = 0; y < recipe_test::kRecipeHeight; ++y)
        for (std::int64_t x = 0; x < recipe_test::kRecipeWidth; ++x) {
            const auto a = got.at(x, y);
            const auto e = expected.at(x, y);
            for (std::size_t c = 0; c < 4; ++c)
                if (std::abs(a[c] - e[c]) > tolerance * std::max(1.0, std::abs(e[c]))) {
                    detail = "at (" + std::to_string(x) + ", " + std::to_string(y) + ") c" +
                             std::to_string(c) + " got " + std::to_string(a[c]) + " want " +
                             std::to_string(e[c]);
                    return false;
                }
        }
    return true;
}

// Drop Shadow: softened coverage, shifted by (0, distance) for direction 180, coloured and faded,
// under the input.
[[nodiscard]] Image dropShadowReference(const Image& input, const RecipeValues& values) {
    const auto softened = blur(input, values.softness, false, true);
    const auto dy = static_cast<std::int64_t>(std::llround(values.distance));
    const double fade = values.opacity * values.shadowColor.alpha;
    auto shadow = Image::blank(softened.x0, softened.y0 + dy, softened.width, softened.height);
    for (auto y = shadow.y0; y < shadow.y0 + shadow.height; ++y)
        for (auto x = shadow.x0; x < shadow.x0 + shadow.width; ++x) {
            const double coverage = softened.at(x, y - dy)[3] * fade;
            shadow(x, y) = {values.shadowColor.red * coverage, values.shadowColor.green * coverage,
                            values.shadowColor.blue * coverage, coverage};
        }
    return values.shadowOnly ? shadow : composite(input, shadow, Composite::Over);
}

[[nodiscard]] double premultipliedLuma(const Pixel& pixel) {
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
}

// The coverage a pixel's luma ramp gives: Classic and Streak ramp the straight luma and keep the
// straight colour (premultiplied colour and alpha times the ramp); Physical (#506) ramps the
// premultiplied luma, the light a pixel emits.
[[nodiscard]] double glowRamp(const double luma, const RecipeValues& values) {
    const double low = values.threshold - values.knee / 2.0;
    const double high = values.threshold + values.knee / 2.0;
    return high > low ? std::clamp((luma - low) / (high - low), 0.0, 1.0)
                      : (luma >= low ? 1.0 : 0.0);
}

[[nodiscard]] Image scaled(Image image, const double colour, const double alpha) {
    for (auto& pixel : image.pixels) {
        for (std::size_t c = 0; c < 3; ++c)
            pixel[c] *= colour;
        pixel[3] *= alpha;
    }
    return image;
}

// A bilinear sample at a real position (pixel centres at integers), transparent outside.
[[nodiscard]] Pixel bilinear(const Image& image, const double x, const double y) {
    const double x0 = std::floor(x);
    const double y0 = std::floor(y);
    const double tx = x - x0;
    const double ty = y - y0;
    const auto ix = static_cast<std::int64_t>(x0);
    const auto iy = static_cast<std::int64_t>(y0);
    Pixel out{};
    const std::array<std::tuple<std::int64_t, std::int64_t, double>, 4> taps{
        {{ix, iy, (1.0 - tx) * (1.0 - ty)},
         {ix + 1, iy, tx * (1.0 - ty)},
         {ix, iy + 1, (1.0 - tx) * ty},
         {ix + 1, iy + 1, tx * ty}}};
    for (const auto& [px, py, weight] : taps) {
        const auto value = image.at(px, py);
        for (std::size_t c = 0; c < 4; ++c)
            out[c] += weight * value[c];
    }
    return out;
}

// One ray pass written directly: four taps i * spacing back along the angle, weights decay^i.
[[nodiscard]] Image rayPass(const Image& source, const double angle, const double spacing,
                            const double decay) {
    const double radians = angle * std::numbers::pi / 180.0;
    const double dx = spacing * std::cos(radians);
    const double dy = spacing * std::sin(radians);
    const auto grow = static_cast<std::int64_t>(std::ceil(3.0 * spacing)) + 1;
    auto out = Image::blank(source.x0 - grow, source.y0 - grow, source.width + 2 * grow,
                            source.height + 2 * grow);
    double total = 0.0;
    for (int i = 0; i < 4; ++i)
        total += std::pow(decay, i);
    for (auto y = out.y0; y < out.y0 + out.height; ++y)
        for (auto x = out.x0; x < out.x0 + out.width; ++x) {
            Pixel sum{};
            for (int i = 0; i < 4; ++i) {
                const auto tap = bilinear(source, static_cast<double>(x) - i * dx,
                                          static_cast<double>(y) - i * dy);
                for (std::size_t c = 0; c < 4; ++c)
                    sum[c] += std::pow(decay, i) / total * tap[c];
            }
            out(x, y) = sum;
        }
    return out;
}

// The glow before its gains: the octave fold (Classic falloff, Physical 2^(2 - exponent)), with
// the gradient's per-octave colour, or the Streak rays, at `scale` times the size or length.
[[nodiscard]] Image spread(const Image& bright, const RecipeValues& values, const double scale) {
    using runtime::detail::GlowMode;
    if (values.mode == GlowMode::Streak) {
        Image sum;
        for (std::int64_t ray = 0; ray < values.points; ++ray) {
            auto image = bright;
            for (int pass = 0; pass < 5; ++pass) {
                const double reach = std::ldexp(1.0, 2 * pass) / 1023.0;
                image = rayPass(image,
                                values.angle + 360.0 * static_cast<double>(ray) /
                                                   static_cast<double>(values.points),
                                values.length * scale * reach, std::pow(0.01, reach));
            }
            if (values.points > 1)
                image = scaled(image, 1.0 / static_cast<double>(values.points),
                               1.0 / static_cast<double>(values.points));
            sum = sum.pixels.empty() ? image : composite(image, sum, Composite::Add);
        }
        return sum;
    }
    const double ratio = values.mode == GlowMode::Physical ? std::exp2(2.0 - values.exponent)
                                                           : values.falloff;
    const auto octaves = values.octaves;
    Image sum;
    double total = 0.0;
    for (std::int64_t octave = octaves - 1; octave >= 0; --octave) {
        const double weight = std::pow(ratio, static_cast<double>(octave));
        const auto blurred = blur(
            bright, values.size * scale * std::ldexp(1.0, static_cast<int>(octave - (octaves - 1))),
            false, false);
        if (sum.pixels.empty())
            sum = Image::blank(blurred.x0, blurred.y0, blurred.width, blurred.height);
        const double t = octaves > 1 ? static_cast<double>(octave) / static_cast<double>(octaves - 1) : 0.0;
        std::array<double, 3> colour{
            values.tint.red + (values.outerTint.red - values.tint.red) * t,
            values.tint.green + (values.outerTint.green - values.tint.green) * t,
            values.tint.blue + (values.outerTint.blue - values.tint.blue) * t};
        if (values.tintGradient) {
            // An independent walk of the stops, sorted by position (ties in stored order): held
            // outside them, linear inside the segment that holds t.
            auto stops = values.tintGradient->stops;
            std::ranges::stable_sort(stops, {}, &document::GradientStop::position);
            const auto rgb = [](const core::Color4d& c) {
                return std::array<double, 3>{c.red, c.green, c.blue};
            };
            colour = t <= stops.front().position ? rgb(stops.front().color) : rgb(stops.back().color);
            for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
                const auto& a = stops[i];
                const auto& b = stops[i + 1];
                if (t > a.position && t <= b.position && b.position > a.position) {
                    const double u = (t - a.position) / (b.position - a.position);
                    colour = {a.color.red + (b.color.red - a.color.red) * u,
                              a.color.green + (b.color.green - a.color.green) * u,
                              a.color.blue + (b.color.blue - a.color.blue) * u};
                    break;
                }
            }
        }
        for (auto y = sum.y0; y < sum.y0 + sum.height; ++y)
            for (auto x = sum.x0; x < sum.x0 + sum.width; ++x) {
                const auto tap = blurred.at(x, y);
                for (std::size_t c = 0; c < 4; ++c)
                    sum(x, y)[c] +=
                        weight * tap[c] * (values.gradient && c < 3 ? colour[c] : 1.0);
            }
        total += weight;
    }
    for (auto& pixel : sum.pixels)
        for (auto& component : pixel)
            component /= total;
    return sum;
}

[[nodiscard]] Image glowReference(const Image& input, const Image& source, const Image* mask,
                                  const Image* dirt, const RecipeValues& values) {
    using runtime::detail::GlowMode;
    const bool physical = values.mode == GlowMode::Physical;
    auto bright = source;
    auto coverage = Image::blank(source.x0, source.y0, source.width, source.height);
    for (auto y = bright.y0; y < bright.y0 + bright.height; ++y)
        for (auto x = bright.x0; x < bright.x0 + bright.width; ++x) {
            auto& pixel = bright(x, y);
            double ramp = glowRamp(physical ? premultipliedLuma(pixel) : straightLuma(pixel),
                                   values);
            if (mask != nullptr)
                ramp *= mask->at(x, y)[3];
            coverage(x, y) = {ramp, ramp, ramp, ramp};
            for (auto& component : pixel)
                component *= ramp;
        }
    Image glow;
    const bool chromatic = values.mode == GlowMode::Streak && values.chromatic != 0.0;
    if (!chromatic && values.channelRadius == std::array<double, 3>{1.0, 1.0, 1.0}) {
        glow = spread(bright, values, 1.0);
    } else {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            double scale = values.channelRadius[channel];
            if (chromatic)
                scale *= 1.0 - values.chromatic * static_cast<double>(channel) / 3.0;
            auto image = scaled(spread(bright, values, scale), 1.0, 1.0 / 3.0);
            for (auto& pixel : image.pixels)
                for (std::size_t c = 0; c < 3; ++c)
                    if (c != channel)
                        pixel[c] = 0.0;
            glow = glow.pixels.empty() ? image : composite(image, glow, Composite::Add);
        }
    }
    const bool tinted = !values.gradient || values.mode == GlowMode::Streak;
    for (auto& pixel : glow.pixels) {
        pixel[0] *= values.intensity * (tinted ? values.tint.red : 1.0);
        pixel[1] *= values.intensity * (tinted ? values.tint.green : 1.0);
        pixel[2] *= values.intensity * (tinted ? values.tint.blue : 1.0);
    }
    if (dirt != nullptr) {
        auto dirty = glow;
        for (auto y = dirty.y0; y < dirty.y0 + dirty.height; ++y)
            for (auto x = dirty.x0; x < dirty.x0 + dirty.width; ++x) {
                const auto d = dirt->at(x, y);
                const double luma = d[3] > 0.0 ? std::clamp(premultipliedLuma(d), 0.0, 1.0) : 0.0;
                auto& pixel = dirty(x, y);
                for (std::size_t c = 0; c < 4; ++c)
                    pixel[c] *= luma * (c < 3 ? values.dirtAmount : 1.0);
            }
        glow = composite(dirty, glow, Composite::Add);
    }
    if (values.rollOff != 0.0)
        for (auto& pixel : glow.pixels) {
            const double divisor = 1.0 + values.rollOff * std::max(premultipliedLuma(pixel), 0.0);
            for (std::size_t c = 0; c < 3; ++c)
                pixel[c] /= divisor;
        }
    if (values.glowOnly)
        return glow;
    auto base = input;
    if (physical && !values.key) {
        const double scatter = std::min(values.intensity, 1.0);
        for (auto y = base.y0; y < base.y0 + base.height; ++y)
            for (auto x = base.x0; x < base.x0 + base.width; ++x)
                for (std::size_t c = 0; c < 3; ++c)
                    base(x, y)[c] *= 1.0 - scatter * coverage.at(x, y)[3];
    }
    return composite(glow, base, Composite::Add);
}

[[nodiscard]] Image lightWrapReference(const Image& input, const Image& background,
                                       const RecipeValues& values) {
    const auto softened = blur(background, values.radius, true, false);
    const auto spread = blur(input, values.radius, false, true);
    auto wrap = softened;
    for (auto y = wrap.y0; y < wrap.y0 + wrap.height; ++y)
        for (auto x = wrap.x0; x < wrap.x0 + wrap.width; ++x) {
            const double weight = values.amount * (1.0 - spread.at(x, y)[3]);
            for (auto& component : wrap(x, y))
                component *= weight;
        }
    return composite(wrap, input, values.wrapOnly ? Composite::In : Composite::Atop);
}

// Edge Extend: the input weighted by its alpha, blurred, divided by its blurred alpha (the straight
// colour of the nearby pixels, solid ones counting for most), put back under the input's alpha, and
// mixed in by amount * (1 - alpha). Alpha itself never changes.
[[nodiscard]] Image edgeExtendReference(const Image& input, const double distance,
                                        const double amount) {
    auto weighted = input;
    for (auto& pixel : weighted.pixels) {
        const double a = pixel[3];
        for (auto& component : pixel)
            component *= a;
    }
    const auto spread = blur(weighted, distance, false, false);
    auto out = input;
    for (auto y = out.y0; y < out.y0 + out.height; ++y)
        for (auto x = out.x0; x < out.x0 + out.width; ++x) {
            const auto p = input.at(x, y);
            const auto s = spread.at(x, y);
            const double divisor = s[3] > 0x1p-20 ? s[3] : 1.0;
            const double t = amount * (1.0 - p[3]);
            for (std::size_t c = 0; c < 3; ++c) {
                const double extended = s[c] / divisor * p[3];
                out(x, y)[c] = p[c] + t * (extended - p[c]);
            }
        }
    return out;
}

[[nodiscard]] Image reference(const runtime::CpuCompositionEvaluator& evaluator,
                              const Recipe recipe, const RecipeValues& values) {
    using recipe_test::Published;
    const auto input = evaluated(evaluator, recipe, values, Published::Input);
    switch (recipe) {
    case Recipe::DropShadow:
        return dropShadowReference(input, values);
    case Recipe::Glow: {
        const auto source =
            values.key ? evaluated(evaluator, recipe, values, Published::Background) : input;
        const auto mask = evaluated(evaluator, recipe, values, Published::Mask);
        return glowReference(input, source, values.mask ? &mask : nullptr,
                             values.dirt ? &mask : nullptr, values);
    }
    case Recipe::LightWrap:
        return lightWrapReference(
            input, evaluated(evaluator, recipe, values, Published::Background), values);
    case Recipe::EdgeExtend:
        return edgeExtendReference(input, values.extendDistance, values.extendAmount);
    case Recipe::ChannelBlur:
    case Recipe::Denoise:
        return {}; // held to the CPU reference by the separable blur tests, and on a device
    }
    return {};
}

void testReference(const Recipe recipe, const RecipeValues& values, const std::string& name) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto plan = recipe_test::recipePlan(recipe, values);
    const auto result = evaluator.evaluate(plan.plan, request(plan.plan->output()), {});
    std::string why;
    for (const auto& diagnostic : result.diagnostics())
        why += ": " + diagnostic.summary + " " + diagnostic.detail;
    expect(result.frame() && result.diagnostics().empty(),
           name + " evaluates without diagnostics" + why);
    if (!result.frame())
        return;
    std::string detail;
    expect(matches(result.frame()->processImage(), reference(evaluator, recipe, values), detail),
           name + " matches the independent reference: " + detail);
}

// Cold, then warm with every operation reused; an ROI equals the full frame there; a changed
// opacity reuses the softened, moved coverage and re-runs only the colour and the composite.
void testCacheAndRoi() {
    const runtime::CpuCompositionEvaluator evaluator;
    RecipeValues values;
    const auto plan = recipe_test::recipePlan(Recipe::DropShadow, values);
    const auto cold = evaluator.evaluate(plan.plan, request(plan.plan->output()), {});
    const auto warm = evaluator.evaluate(plan.plan, request(plan.plan->output()), {});
    expect(cold.frame() && warm.frame() &&
               std::ranges::equal(cold.frame()->processImage().pixels(),
                                  warm.frame()->processImage().pixels()) &&
               warm.frame()->operationCacheStatistics().misses == 0,
           "a warm Drop Shadow frame reuses every operation with identical pixels");
    values.opacity = 0.5;
    const auto changed = recipe_test::recipePlan(Recipe::DropShadow, values);
    const auto faded = evaluator.evaluate(changed.plan, request(changed.plan->output()), {});
    expect(faded.frame() && faded.frame()->operationCacheStatistics().misses == 4 &&
               !std::ranges::equal(cold.frame()->processImage().pixels(),
                                   faded.frame()->processImage().pixels()),
           "a changed opacity re-runs only Levels, the recipe's Merge and the output's Merge and "
           "Composition Output");
    for (const auto recipe :
         {Recipe::DropShadow, Recipe::Glow, Recipe::LightWrap, Recipe::EdgeExtend}) {
        const auto roiPlan = recipe_test::recipePlan(recipe, {});
        const auto full = evaluator.evaluate(roiPlan.plan, request(roiPlan.plan->output()), {});
        auto roiRequest = request(roiPlan.plan->output());
        const auto roiWindow = render::ImageWindow::create(5, 4, 9, 7);
        roiRequest.roi = *roiWindow.value();
        const auto roi = evaluator.evaluate(roiPlan.plan, roiRequest, {});
        bool same = full.frame() && roi.frame();
        if (same)
            for (std::int64_t y = 4; y < 11; ++y)
                for (std::int64_t x = 5; x < 14; ++x) {
                    const auto a = roi.frame()->processImage().read(x, y);
                    const auto b = full.frame()->processImage().read(x, y);
                    same = same && a && b && *a.value() == *b.value();
                }
        expect(same, "recipe ROI pixels equal the full frame's");
    }
}

// An animated distance (Drop Shadow) or intensity (Glow) derives its value per frame: the chain is
// time dependent from the animated step on, and the frame at t = 1 equals the constant recipe.
void testAnimation() {
    const runtime::CpuCompositionEvaluator evaluator;
    for (const auto recipe : {Recipe::DropShadow, Recipe::Glow}) {
        RecipeValues values;
        values.animated = true;
        const auto animated = recipe_test::recipePlan(recipe, values);
        values.animated = false;
        const auto constant = recipe_test::recipePlan(recipe, values);
        expect(!animated.plan->valueOperations().empty() &&
                   animated.plan->operationTimeDependent(animated.recipe) &&
                   !animated.plan->operationTimeDependent(animated.input),
               "an animated recipe value is derived per frame and invalidates downstream");
        const auto start = evaluator.evaluate(animated.plan, request(animated.plan->output()), {});
        const auto end = evaluator.evaluate(
            animated.plan, request(animated.plan->output(), core::RationalTime::fromInteger(1)),
            {});
        const auto still = evaluator.evaluate(constant.plan, request(constant.plan->output()), {});
        std::string detail;
        expect(start.frame() && end.frame() && still.frame() &&
                   matches(end.frame()->processImage(), imageOf(still.frame()->processImage()),
                           detail, 1e-6) &&
                   !std::ranges::equal(start.frame()->processImage().pixels(),
                                       end.frame()->processImage().pixels()),
               "the animated recipe equals the constant one at its key: " + detail);
    }
}

// #516 Edge Extend. The known answers: a pixel of alpha 1 is exactly the input; alpha is never
// changed; amount 0 is the input; a uniform straight colour is a fixed point; the colour under a
// soft edge moves towards the solid pixels beside it (here by the reference above, which the
// recipe test already matched). The same checks run at a reduced render scale (a proxy half the
// composition's size), where the blur reaches half as many pixels.
[[nodiscard]] Image evaluatedAt(const runtime::CpuCompositionEvaluator& evaluator,
                                const RecipeValues& values, const bool edgeExtend,
                                const runtime::EvaluationResolution resolution) {
    const auto plan = recipe_test::recipePlan(Recipe::EdgeExtend, values, 3900,
                                              edgeExtend ? recipe_test::Published::Recipe
                                                         : recipe_test::Published::Input)
                          .plan;
    auto value = request(plan->output());
    value.resolution = resolution;
    const auto result = evaluator.evaluate(plan, value, {});
    if (!result.frame())
        throw std::logic_error("Edge Extend fixture does not evaluate");
    return imageOf(result.frame()->processImage());
}

void testEdgeExtend() {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto halfExtent = render::ImageExtent::create(recipe_test::kRecipeWidth / 2,
                                                        recipe_test::kRecipeHeight / 2);
    if (!halfExtent)
        throw std::logic_error("the proxy extent is valid");
    for (const auto& [resolution, label] :
         {std::pair<runtime::EvaluationResolution, const char*>{
              runtime::CompositionFormatResolution{}, "full scale"},
          std::pair<runtime::EvaluationResolution, const char*>{
              runtime::ProxyResolution{*halfExtent.value()}, "half scale"}}) {
        const std::string where = std::string("Edge Extend at ") + label + ": ";
        RecipeValues values;
        const auto source = evaluatedAt(evaluator, values, false, resolution);
        const auto extended = evaluatedAt(evaluator, values, true, resolution);
        bool alphaKept = true;
        bool solidKept = true;
        bool softMoved = false;
        for (auto y = source.y0; y < source.y0 + source.height; ++y)
            for (auto x = source.x0; x < source.x0 + source.width; ++x) {
                const auto a = source.at(x, y);
                const auto b = extended.at(x, y);
                alphaKept = alphaKept && std::abs(a[3] - b[3]) <= 1e-6;
                if (a[3] == 1.0)
                    solidKept = solidKept && a == b;
                else if (std::abs(a[0] - b[0]) > 1e-4)
                    softMoved = true;
            }
        expect(alphaKept, where + "alpha is never changed");
        expect(solidKept, where + "a solid pixel is exactly the input");
        expect(softMoved, where + "the colour under a soft edge changes");
        // The gradient's straight colour grows with x while its alpha is below one, so the pixels
        // beside a soft one are brighter and Edge Extend lifts it.
        const auto at = [&](const Image& image) { return image.at(source.x0 + 3, source.y0 + 3); };
        expect(at(extended)[0] > at(source)[0], where + "a soft pixel takes the brighter colour near it");
        values.extendAmount = 0.0;
        const auto off = evaluatedAt(evaluator, values, true, resolution);
        expect(off.pixels == source.pixels, where + "amount 0 is the input");
        values.extendAmount = 1.0;
        values.extendDistance = 0.0;
        const auto none = evaluatedAt(evaluator, values, true, resolution);
        bool same = true;
        for (std::size_t i = 0; i < none.pixels.size(); ++i)
            for (std::size_t c = 0; c < 4; ++c)
                same = same && std::abs(none.pixels[i][c] - source.pixels[i][c]) <= 1e-6;
        expect(same, where + "distance 0 is the input");
        RecipeValues uniform;
        uniform.solidInput = true;
        const auto flat = evaluatedAt(evaluator, uniform, false, resolution);
        const auto flatExtended = evaluatedAt(evaluator, uniform, true, resolution);
        bool fixedPoint = true;
        for (std::size_t i = 0; i < flat.pixels.size(); ++i)
            for (std::size_t c = 0; c < 4; ++c)
                fixedPoint = fixedPoint &&
                             std::abs(flat.pixels[i][c] - flatExtended.pixels[i][c]) <= 1e-5;
        expect(fixedPoint, where + "one uniform colour is a fixed point");
    }
}

// A cancelled frame publishes nothing; cancellation arriving inside the chain (while the softened
// coverage runs) stops it; an exhausted pixel budget refuses the frame.
void testCancellationAndBudget() {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto plan = recipe_test::recipePlan(Recipe::Glow, {});
    runtime::TaskScheduler scheduler;
    const runtime::TaskOwner owner{runtime::TaskOwnerKind::PanelRequest,
                                   runtime::TaskOwnerId::fromRaw(3390)};
    std::promise<bool> outcome;
    auto future = outcome.get_future();
    const auto task = scheduler.submit<void>(
        runtime::TaskRequest("Recipe cancellation", owner), [&](runtime::TaskContext& context) {
            std::atomic_bool cancelled{false};
            const auto result =
                evaluator.evaluate(plan.plan, request(plan.plan->output()), context.cancellation(),
                                   [&](const runtime::EvaluationProgress& progress) {
                                       // Inside the chain: an octave blur after the key.
                                       if (progress.operation &&
                                           progress.operation->value() > plan.input.value() + 2 &&
                                           !cancelled.exchange(true))
                                           static_cast<void>(scheduler.cancelOwner(owner));
                                   });
            outcome.set_value(result.status() == runtime::EvaluationStatus::Cancelled &&
                              !result.frame());
            return runtime::TaskResult<void>::cancelled();
        });
    expect(task.accepted() && future.get(),
           "cancellation inside a recipe chain publishes no frame");
    auto small = request(plan.plan->output());
    small.pixelStorageByteLimit = 4096;
    const auto refused = evaluator.evaluate(plan.plan, small, {});
    expect(!refused.frame() && refused.status() != runtime::EvaluationStatus::Cancelled,
           "a recipe beyond the pixel budget is refused, never partially published");
}

// #506 Physical conserves light: with a white tint and intensity <= 1 the glow carries exactly
// the light it removes from the keyed pixels, so the frame's total colour is unchanged (the input
// is centred, so no scattered pixel leaves the composition window); above 1 the excess is added.
void testPhysicalEnergy() {
    const runtime::CpuCompositionEvaluator evaluator;
    RecipeValues values;
    values.mode = runtime::detail::GlowMode::Physical;
    values.centred = true;
    values.tint = {1.0, 1.0, 1.0, 1.0};
    values.threshold = 0.5;
    values.size = 3.0;
    const auto total = [&](const RecipeValues& item, const recipe_test::Published published) {
        const auto image = evaluated(evaluator, Recipe::Glow, item, published);
        std::array<double, 3> sum{};
        for (const auto& pixel : image.pixels)
            for (std::size_t c = 0; c < 3; ++c)
                sum[c] += pixel[c];
        return sum;
    };
    const auto before = total(values, recipe_test::Published::Input);
    for (const double intensity : {0.4, 1.0, 2.5}) {
        values.intensity = intensity;
        const auto after = total(values, recipe_test::Published::Recipe);
        auto glowOnly = values;
        glowOnly.glowOnly = true;
        const auto scattered = total(glowOnly, recipe_test::Published::Recipe);
        bool conserved = true;
        for (std::size_t c = 0; c < 3; ++c) {
            // The light that leaves the keyed pixels is min(intensity, 1) / intensity of the glow.
            const double expected =
                before[c] + scattered[c] * (1.0 - std::min(intensity, 1.0) / intensity);
            conserved = conserved && std::abs(after[c] - expected) <= 1e-4 * before[c];
        }
        expect(conserved && scattered[0] > 0.0,
               "Physical Glow conserves the frame's light up to intensity 1 and adds the excess "
               "above it (intensity " +
                   std::to_string(intensity) + ")");
    }
}

// #506: the mode-specific variants against the independent reference.
void testModes() {
    using runtime::detail::GlowMode;
    RecipeValues values;
    values.mode = GlowMode::Physical;
    testReference(Recipe::Glow, values, "Glow Physical");
    values.exponent = 3.0;
    values.intensity = 0.5;
    testReference(Recipe::Glow, values, "Glow Physical (inverse cube, partial scatter)");
    values = {};
    values.mode = GlowMode::Physical;
    values.key = true;
    values.mask = true;
    values.threshold = 0.5;
    testReference(Recipe::Glow, values, "Glow Physical (key pass and Mask, nothing removed)");
    values.glowOnly = true;
    testReference(Recipe::Glow, values, "Glow Physical (key pass and Mask, glow only)");
    values = {};
    values.mode = GlowMode::Streak;
    testReference(Recipe::Glow, values, "Glow Streak (four-point star)");
    values.points = 2;
    values.angle = 0.0;
    values.chromatic = 0.6;
    testReference(Recipe::Glow, values, "Glow Streak (chromatic anamorphic streak)");
    values.points = 5;
    values.angle = -17.0;
    values.chromatic = 0.0;
    values.glowOnly = true;
    testReference(Recipe::Glow, values, "Glow Streak (odd star, glow only)");
    values = {};
    values.channelRadius = {1.0, 0.5, 0.25};
    values.gradient = true;
    values.dirt = true;
    values.rollOff = 0.5;
    testReference(Recipe::Glow, values,
                  "Glow Classic (per-channel radius, gradient, lens dirt, roll-off)");
    values.mode = GlowMode::Physical;
    testReference(Recipe::Glow, values,
                  "Glow Physical (per-channel radius, gradient, lens dirt, roll-off)");
    values.mode = GlowMode::Streak;
    values.channelRadius = {0.5, 1.0, 0.75};
    values.points = 3;
    testReference(Recipe::Glow, values, "Glow Streak (per-channel length, lens dirt, roll-off)");
    // #541: a four-stop tint gradient, unsorted, with HDR colour: octave j of 5 takes the gradient
    // at j / 4.
    values = {};
    values.gradient = true;
    values.tintGradient = document::GradientValue{{{1.0, {0.2, 0.4, 2.0, 1.0}},
                                                   {0.0, {1.0, 1.0, 1.0, 1.0}},
                                                   {0.3, {1.5, 0.25, 0.1, 1.0}},
                                                   {0.6, {0.0, 1.0, 0.5, 1.0}}}};
    testReference(Recipe::Glow, values, "Glow Classic (four-stop tint gradient)");
    values.mode = GlowMode::Physical;
    testReference(Recipe::Glow, values, "Glow Physical (four-stop tint gradient)");
}

// #506: the Streak rays' cache identity: a warm frame reuses every pass; a changed length
// re-runs the rays; an ROI equals the full frame; cancellation inside a ray stops the frame.
void testStreakCacheRoiAndCancellation() {
    const runtime::CpuCompositionEvaluator evaluator;
    RecipeValues values;
    values.mode = runtime::detail::GlowMode::Streak;
    const auto plan = recipe_test::recipePlan(Recipe::Glow, values);
    const auto cold = evaluator.evaluate(plan.plan, request(plan.plan->output()), {});
    const auto warm = evaluator.evaluate(plan.plan, request(plan.plan->output()), {});
    expect(cold.frame() && warm.frame() &&
               std::ranges::equal(cold.frame()->processImage().pixels(),
                                  warm.frame()->processImage().pixels()) &&
               warm.frame()->operationCacheStatistics().misses == 0,
           "a warm Streak frame reuses every ray pass with identical pixels");
    values.intensity = 3.0;
    const auto brighter = recipe_test::recipePlan(Recipe::Glow, values);
    const auto rerun = evaluator.evaluate(brighter.plan, request(brighter.plan->output()), {});
    // Levels, the recipe's Merge, the output's Merge and Composition Output.
    expect(rerun.frame() && rerun.frame()->operationCacheStatistics().misses == 4,
           "a changed Streak intensity reuses every ray and re-runs only the gains on");
    auto roiRequest = request(plan.plan->output());
    const auto roiWindow = render::ImageWindow::create(5, 4, 9, 7);
    roiRequest.roi = *roiWindow.value();
    const auto roi = evaluator.evaluate(plan.plan, roiRequest, {});
    bool same = cold.frame() && roi.frame();
    if (same)
        for (std::int64_t y = 4; y < 11; ++y)
            for (std::int64_t x = 5; x < 14; ++x) {
                const auto a = roi.frame()->processImage().read(x, y);
                const auto b = cold.frame()->processImage().read(x, y);
                same = same && a && b && *a.value() == *b.value();
            }
    expect(same, "Streak ROI pixels equal the full frame's");
    runtime::TaskScheduler scheduler;
    const runtime::TaskOwner owner{runtime::TaskOwnerKind::PanelRequest,
                                   runtime::TaskOwnerId::fromRaw(5060)};
    std::promise<bool> outcome;
    auto future = outcome.get_future();
    const auto task = scheduler.submit<void>(
        runtime::TaskRequest("Streak cancellation", owner), [&](runtime::TaskContext& context) {
            std::atomic_bool cancelled{false};
            const auto result = evaluator.evaluate(
                plan.plan, request(plan.plan->output()), context.cancellation(),
                [&](const runtime::EvaluationProgress& progress) {
                    // Inside the first ray's passes.
                    if (progress.operation && progress.operation->value() > plan.input.value() + 4 &&
                        !cancelled.exchange(true))
                        static_cast<void>(scheduler.cancelOwner(owner));
                });
            outcome.set_value(result.status() == runtime::EvaluationStatus::Cancelled &&
                              !result.frame());
            return runtime::TaskResult<void>::cancelled();
        });
    expect(task.accepted() && future.get(), "cancellation inside a Streak ray publishes no frame");
    auto small = request(plan.plan->output());
    small.pixelStorageByteLimit = 4096;
    const auto refused = evaluator.evaluate(plan.plan, small, {});
    expect(!refused.frame() && refused.status() != runtime::EvaluationStatus::Cancelled,
           "a Streak beyond the pixel budget is refused, never partially published");
}

} // namespace

int main() {
    try {
        RecipeValues values;
        testReference(Recipe::DropShadow, values, "Drop Shadow");
        values.shadowOnly = true;
        testReference(Recipe::DropShadow, values, "Drop Shadow (shadow only)");
        values = {};
        testReference(Recipe::Glow, values, "Glow");
        values.glowOnly = true;
        values.octaves = 1;
        values.knee = 0.0;
        testReference(Recipe::Glow, values, "Glow (hard key, one octave, glow only)");
        values = {};
        values.key = true;
        values.mask = true;
        values.threshold = 0.5;
        testReference(Recipe::Glow, values, "Glow (key pass and Mask)");
        values = {};
        values.solidInput = true;
        testReference(Recipe::Glow, values, "Glow (solid input, as on the device)");
        values = {};
        testReference(Recipe::LightWrap, values, "Light Wrap");
        values.wrapOnly = true;
        testReference(Recipe::LightWrap, values, "Light Wrap (wrap only)");
        values = {};
        testReference(Recipe::EdgeExtend, values, "Edge Extend");
        values.extendDistance = 2.5;
        values.extendAmount = 0.6;
        testReference(Recipe::EdgeExtend, values, "Edge Extend (a part of the way, fractional reach)");
        testEdgeExtend();
        testModes();
        testPhysicalEnergy();
        testCacheAndRoi();
        testAnimation();
        testCancellationAndBudget();
        testStreakCacheRoiAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "compositing recipe tests threw: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " compositing recipe expectation(s) failed\n";
        return 1;
    }
    std::cout << "compositing recipe tests passed\n";
    return 0;
}
