// #339 Drop Shadow, Glow and Light Wrap on the CPU reference. Each recipe runs through the
// production chain (compositing_recipes.hpp) and is compared with an independent double-precision
// formula over an HDR, partial-alpha gradient: an explicit two-dimensional Gaussian, the straight
// luma ramp, the normalized octave sum and the Porter-Duff composite, written here without the
// primitives. Then the chain's cache identity (a cold and a warm frame, a changed opacity reusing
// the softened coverage), ROI, animation, cancellation and budget refusal.

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
#include <stdexcept>
#include <string>
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

[[nodiscard]] Image glowReference(const Image& input, const Image& source, const Image* mask,
                                  const RecipeValues& values) {
    const double low = values.threshold - values.knee / 2.0;
    const double high = values.threshold + values.knee / 2.0;
    auto bright = source;
    for (auto y = bright.y0; y < bright.y0 + bright.height; ++y)
        for (auto x = bright.x0; x < bright.x0 + bright.width; ++x) {
            auto& pixel = bright(x, y);
            const double luma = straightLuma(pixel);
            double ramp = high > low ? std::clamp((luma - low) / (high - low), 0.0, 1.0)
                                     : (luma >= low ? 1.0 : 0.0);
            if (mask != nullptr)
                ramp *= mask->at(x, y)[3];
            for (auto& component : pixel)
                component *= ramp;
        }
    const auto octaves = values.octaves;
    Image sum;
    double total = 0.0;
    for (std::int64_t octave = octaves - 1; octave >= 0; --octave) {
        const double weight = std::pow(values.falloff, static_cast<double>(octave));
        const auto blurred =
            blur(bright, values.size * std::ldexp(1.0, static_cast<int>(octave - (octaves - 1))),
                 false, false);
        if (sum.pixels.empty()) {
            sum = Image::blank(blurred.x0, blurred.y0, blurred.width, blurred.height);
        }
        for (auto y = sum.y0; y < sum.y0 + sum.height; ++y)
            for (auto x = sum.x0; x < sum.x0 + sum.width; ++x) {
                const auto tap = blurred.at(x, y);
                for (std::size_t c = 0; c < 4; ++c)
                    sum(x, y)[c] += weight * tap[c];
            }
        total += weight;
    }
    const std::array<double, 3> gains{values.tint.red * values.intensity,
                                      values.tint.green * values.intensity,
                                      values.tint.blue * values.intensity};
    for (auto& pixel : sum.pixels) {
        for (std::size_t c = 0; c < 4; ++c)
            pixel[c] /= total;
        for (std::size_t c = 0; c < 3; ++c)
            pixel[c] *= gains[c];
    }
    return values.glowOnly ? sum : composite(sum, input, Composite::Add);
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
        return glowReference(input, source, values.mask ? &mask : nullptr, values);
    }
    case Recipe::LightWrap:
        return lightWrapReference(
            input, evaluated(evaluator, recipe, values, Published::Background), values);
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
    for (const auto recipe : {Recipe::DropShadow, Recipe::Glow, Recipe::LightWrap}) {
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
        testCacheAndRoi();
        testAnimation();
        testCancellationAndBudget();
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
