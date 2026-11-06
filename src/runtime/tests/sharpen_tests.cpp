// #486 Sharpen on the CPU reference: the pixel against an independent double-precision reading over
// HDR, signed and partial-alpha values; the promises the node makes (alpha kept, a flat colour flat
// through an alpha edge, no clipping of highlights, no colour taken below black, exposure invariance,
// the threshold, the amount's overshoot on a step edge against an independent blur); the plan's two
// operations, frame-edge clamp, region of interest and proxy; identity, domains, the pixel budget and
// cancellation.

#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"
#include "sharpen_image_effect.hpp"
#include "sharpen_test_support.hpp"

#include <bloom/render/image_coverage.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
using runtime::detail::prepareColorGrade;
using sharpen_test::SharpenValues;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

runtime::SharpenKernel kernelOf(const SharpenValues& values) {
    return {{document::ParameterId::fromRaw(1), values.amount},
            {document::ParameterId::fromRaw(2), values.threshold},
            {{}, values.radius}};
}
std::optional<runtime::detail::PreparedColorGrade> prepare(const SharpenValues& values) {
    return prepareColorGrade(kernelOf(values),
                             std::vector<double>{values.amount, values.threshold, values.radius});
}

using Paint = std::function<std::array<float, 4>(std::int64_t, std::int64_t)>;
render::Rgba32fImage painted(const std::uint32_t width, const std::uint32_t height, const Paint& paint,
                             const std::int64_t originX = 0, const std::int64_t originY = 0) {
    const auto data = render::ImageWindow::create(originX, originY, width, height);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *data.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (auto y = originY; y < originY + static_cast<std::int64_t>(height); ++y)
        for (auto x = originX; x < originX + static_cast<std::int64_t>(width); ++x) {
            const auto c = paint(x, y);
            const auto pixel = render::Rgba32f::fromPremultiplied(c[0], c[1], c[2], c[3]);
            if (!pixel || builder.value()->write(x, y, *pixel.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

// The input's Gaussian blur through the production separable filter, frame edge clamped.
std::shared_ptr<const render::Rgba32fImage> blurOf(const render::Rgba32fImage& input,
                                                   const double radius) {
    const runtime::SeparableBlurKernel kernel{runtime::SeparableBlurKind::Gaussian,
                                              {{}, radius},
                                              runtime::BlurEdgeMode::Clamp,
                                              runtime::BlurChannels::Rgba};
    const auto prepared = runtime::detail::prepareSeparableBlur(kernel, radius, 1.0, 1.0);
    if (!prepared)
        throw std::logic_error("the blur prepares");
    auto result = runtime::detail::applySeparableBlur(*prepared, input, 1U << 26U, {},
                                                      runtime::OperationIndex::fromRaw(0), {});
    if (!result.image)
        throw std::logic_error("the blur applies");
    return result.image;
}

std::shared_ptr<const render::Rgba32fImage> sharpened(const SharpenValues& values,
                                                      const render::Rgba32fImage& input) {
    const auto grade = prepare(values);
    if (!grade)
        throw std::logic_error("the sharpen prepares");
    const auto blurred = blurOf(input, values.radius);
    auto result = runtime::detail::applySharpen(*grade, input, blurred.get(), 1U << 26U, {},
                                                runtime::OperationIndex::fromRaw(0), {}, nullptr);
    if (!result.image)
        throw std::logic_error("the sharpen applies");
    return result.image;
}

std::array<float, 4> at(const render::Rgba32fImage& picture, const std::int64_t x,
                        const std::int64_t y) {
    std::array<float, 4> value{};
    if (const auto pixel = picture.read(x, y))
        std::ranges::copy(pixel.value()->components(), value.begin());
    return value;
}

render::Rgba32f pixelOf(const float r, const float g, const float b, const float a) {
    const auto pixel = render::Rgba32f::fromPremultiplied(r, g, b, a);
    if (!pixel)
        throw std::logic_error("fixture pixel is valid");
    return *pixel.value();
}

void testPixel() {
    // The formula, read independently in double precision.
    const std::array bases{pixelOf(0.5F, 0.25F, 2.0F, 1.0F), pixelOf(9.0F, -0.5F, 0.125F, 0.5F),
                           pixelOf(0.01F, 0.02F, 0.03F, 0.0625F),
                           pixelOf(1500.0F, 0.25F, -3.0F, 1.0F)};
    const std::array blurs{pixelOf(0.25F, 0.5F, 1.75F, 1.0F), pixelOf(2.0F, 0.25F, 0.5F, 0.75F),
                           pixelOf(0.1F, 0.1F, 0.1F, 0.5F), pixelOf(1400.0F, 0.75F, -2.0F, 0.875F)};
    for (const auto amount : {0.5F, 1.0F, 3.5F})
        for (const auto threshold : {0.0F, 0.02F, 0.5F})
            for (const auto& base : bases)
                for (const auto& blur : blurs) {
                    const auto result = render::unsharpImagePixel(base, blur, amount, threshold);
                    expect(static_cast<bool>(result), "an unsharp pixel is representable");
                    if (!result)
                        continue;
                    const auto a = static_cast<double>(base.alpha());
                    for (std::size_t c = 0; c < 3; ++c) {
                        const auto value = static_cast<double>(base.components()[c]);
                        const double detail = value - a / static_cast<double>(blur.alpha()) *
                                                          static_cast<double>(blur.components()[c]);
                        const double limit = a * static_cast<double>(threshold);
                        const double cored = detail > limit    ? detail - limit
                                             : detail < -limit ? detail + limit
                                                               : 0.0;
                        const double expected =
                            std::max(value + static_cast<double>(amount) * cored,
                                     std::min(value, 0.0));
                        const double error =
                            std::abs(static_cast<double>(result.value()->components()[c]) - expected);
                        expect(error <= 4e-6 * std::max(1.0, std::abs(expected)),
                               "the pixel follows the formula");
                    }
                    expect(result.value()->alpha() == base.alpha(), "alpha is kept exactly");
                }
    // No alpha, or no blur there, adds nothing.
    const auto transparent =
        render::unsharpImagePixel(render::Rgba32f::transparent(), blurs[0], 2.0F, 0.0F);
    expect(transparent && *transparent.value() == render::Rgba32f::transparent(),
           "a transparent pixel stays transparent");
    const auto unblurred =
        render::unsharpImagePixel(bases[0], render::Rgba32f::transparent(), 2.0F, 0.0F);
    expect(unblurred && *unblurred.value() == bases[0], "no blur adds nothing");
    // A colour does not go below black, and one already below black keeps its own floor.
    const auto floor =
        render::unsharpImagePixel(pixelOf(0.1F, 0.1F, -0.5F, 1.0F),
                                  pixelOf(50.0F, 0.1F, 50.0F, 1.0F), 2.0F, 0.0F);
    expect(floor && floor.value()->red() == 0.0F && floor.value()->green() == 0.1F &&
               floor.value()->blue() == -0.5F,
           "a dark halo stops at black; a negative value keeps itself");
    // A highlight's overshoot is not clipped.
    const auto highlight =
        render::unsharpImagePixel(pixelOf(100.0F, 1.0F, 1.0F, 1.0F),
                                  pixelOf(50.0F, 1.0F, 1.0F, 1.0F), 1.0F, 0.0F);
    expect(highlight && highlight.value()->red() == 150.0F, "a highlight overshoots unclipped");
    expect(!render::unsharpImagePixel(pixelOf(std::numeric_limits<float>::max(), 0.0F, 0.0F, 1.0F),
                                      pixelOf(0.0F, 0.0F, 0.0F, 1.0F), 10.0F, 0.0F),
           "an overflow fails");
}

void testPicture() {
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 16;
    // A flat straight colour under an alpha ramp and a hard alpha edge stays that colour.
    const auto flat = painted(kWidth, kHeight, [](const std::int64_t x, std::int64_t) {
        const float alpha = x < 16 ? 0.0F : x < 48 ? static_cast<float>(x - 15) / 33.0F : 1.0F;
        return std::array{2.0F * alpha, 0.5F * alpha, 0.25F * alpha, alpha};
    });
    const auto flatResult = sharpened({3.0, 8.0, 0.0}, flat);
    bool stays = true;
    for (std::int64_t x = 0; x < kWidth; ++x) {
        const auto in = at(flat, x, 8);
        const auto out = at(*flatResult, x, 8);
        for (std::size_t c = 0; c < 3; ++c)
            stays = stays && std::abs(out[c] - in[c]) <= 2e-5F;
        stays = stays && out[3] == in[3];
    }
    expect(stays, "a flat colour stays flat through its alpha edges, and alpha is kept");

    // A step edge against an independent blur: the bright side overshoots by the amount times the
    // detail, the dark side undershoots, and past the radius nothing changes.
    constexpr double kRadius = 6.0;
    const auto step = painted(kWidth, kHeight, [](const std::int64_t x, std::int64_t) {
        const float value = x < 48 ? 0.2F : 0.8F;
        return std::array{value, value, value, 1.0F};
    });
    const auto stepResult = sharpened({1.5, kRadius, 0.0}, step);
    std::array<double, 13> weights{};
    double total = 0.0;
    for (int i = -6; i <= 6; ++i) {
        const double w = i == 0 ? 1.0 : std::exp(-0.5 * std::pow(i / (kRadius / 3.0), 2.0));
        weights[static_cast<std::size_t>(i + 6)] = w;
        total += w;
    }
    bool follows = true;
    for (std::int64_t x = 36; x < 60; ++x) {
        double blurred = 0.0;
        for (int i = -6; i <= 6; ++i)
            blurred += weights[static_cast<std::size_t>(i + 6)] / total * (x + i < 48 ? 0.2 : 0.8);
        const double value = x < 48 ? 0.2 : 0.8;
        const double expected = std::max(value + 1.5 * (value - blurred), 0.0);
        follows = follows && std::abs(static_cast<double>(at(*stepResult, x, 8)[0]) - expected) < 2e-5;
    }
    expect(follows, "a step edge's halo follows the amount and the blur");
    expect(at(*stepResult, 48, 8)[0] > 0.8F + 0.1F && at(*stepResult, 47, 8)[0] == 0.0F,
           "the bright side overshoots, and the dark side's halo stops at black");
    expect(std::abs(at(*stepResult, 40, 8)[0] - 0.2F) < 1e-5F &&
               std::abs(at(*stepResult, 56, 8)[0] - 0.8F) < 1e-5F,
           "past the radius nothing changes");

    // The threshold leaves small detail alone and larger detail starts from it, with no jump.
    const auto ripple = painted(kWidth, kHeight, [](const std::int64_t x, std::int64_t) {
        const float value = 0.5F + (x % 2 == 0 ? 0.01F : -0.01F) + (x < 48 ? 0.0F : 0.3F);
        return std::array{value, value, value, 1.0F};
    });
    const auto cored = sharpened({2.0, 3.0, 0.05}, ripple);
    expect(at(*cored, 20, 8) == at(ripple, 20, 8), "detail below the threshold is left alone");
    const auto uncored = sharpened({2.0, 3.0, 0.0}, ripple);
    expect(at(*uncored, 20, 8) != at(ripple, 20, 8), "without a threshold it is sharpened");
    expect(at(*cored, 48, 8)[0] > at(ripple, 48, 8)[0] && at(*cored, 48, 8)[0] < at(*uncored, 48, 8)[0],
           "detail above the threshold starts from it");

    // Exposure invariance: sharpening then exposing is exposing then sharpening.
    const auto exposed = painted(kWidth, kHeight, [&](const std::int64_t x, const std::int64_t y) {
        const auto c = at(step, x, y);
        return std::array{c[0] * 64.0F, c[1] * 64.0F, c[2] * 64.0F, c[3]};
    });
    const auto exposedResult = sharpened({1.5, kRadius, 0.0}, exposed);
    bool invariant = true;
    for (std::int64_t x = 36; x < 60; ++x)
        invariant = invariant && std::abs(at(*exposedResult, x, 8)[0] / 64.0F -
                                          at(*stepResult, x, 8)[0]) < 2e-5F;
    expect(invariant, "the result scales with the exposure");

    // A data window away from the origin reads the same picture, and no blur at all adds nothing.
    const auto moved = painted(
        kWidth, kHeight,
        [&](const std::int64_t x, const std::int64_t y) { return at(step, x + 7, y + 3); }, -7, -3);
    const auto movedResult = sharpened({1.5, kRadius, 0.0}, moved);
    expect(at(*movedResult, 41, 5) == at(*stepResult, 48, 8), "the data window moves nothing");
    const auto grade = prepare({1.5, kRadius, 0.0});
    const auto bare = runtime::detail::applySharpen(*grade, step, nullptr, 1U << 26U, {},
                                                    runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(bare.image && at(*bare.image, 48, 8) == at(step, 48, 8), "no blur image adds nothing");
}

runtime::EvaluationRequest requestFor(const runtime::CompiledCompositionPlan& plan) {
    return runtime::EvaluationRequest{
        .time = core::RationalTime::fromInteger(0),
        .output = plan.output(),
        .resolution = runtime::CompositionFormatResolution{},
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = 1ULL << 28U,
        .bypassOperationCache = true,
    };
}

void testEvaluator() {
    const auto plan = sharpen_test::sharpenPlan({2.0, 4.0, 0.0});
    const auto kernels = std::ranges::count_if(plan->operations(), [](const auto& operation) {
        const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
        return effect != nullptr && std::holds_alternative<runtime::SharpenKernel>(effect->kernel);
    });
    expect(kernels == 1, "the plan holds one Sharpen");
    const runtime::CpuCompositionEvaluator evaluator;
    const auto whole = evaluator.evaluate(plan, requestFor(*plan), {});
    expect(whole.frame() != nullptr, "the plan evaluates");
    if (whole.frame() == nullptr)
        return;
    const auto& frame = whole.frame()->processImage();

    // The amount changes the picture; no amount and no radius give the input's own picture.
    const auto plain = sharpen_test::sharpenPlan({0.0, 4.0, 0.0});
    const auto noRadius = sharpen_test::sharpenPlan({2.0, 0.0, 0.0});
    const auto base = evaluator.evaluate(plain, requestFor(*plain), {});
    const auto bare = evaluator.evaluate(noRadius, requestFor(*noRadius), {});
    expect(base.frame() != nullptr && bare.frame() != nullptr, "the identities evaluate");
    if (base.frame() == nullptr || bare.frame() == nullptr)
        return;
    const auto pixels = [](const render::Rgba32fImage& image) {
        const auto span = image.pixels();
        return std::vector<render::Rgba32f>(span.begin(), span.end());
    };
    expect(pixels(base.frame()->processImage()) == pixels(bare.frame()->processImage()),
           "no amount and no radius are the same picture");
    expect(pixels(base.frame()->processImage()) != pixels(frame), "an amount changes the picture");

    // A region of interest reads the same pixels as the whole frame.
    auto regional = requestFor(*plan);
    const auto region = render::ImageWindow::create(20, 12, 16, 12);
    regional.roi = *region.value();
    const auto part = evaluator.evaluate(plan, regional, {});
    expect(part.frame() != nullptr, "a region of interest evaluates");
    if (part.frame() != nullptr) {
        bool same = true;
        for (auto y = region.value()->originY(); y < region.value()->maxYExclusive(); ++y)
            for (auto x = region.value()->originX(); x < region.value()->maxXExclusive(); ++x)
                same = same && at(part.frame()->processImage(), x, y) == at(frame, x, y);
        expect(same, "a region's pixels are the whole frame's");
    }

    // A proxy scales the radius with the picture.
    auto proxied = requestFor(*plan);
    const auto extent = render::ImageExtent::create(32, 24);
    proxied.resolution = runtime::ProxyResolution{*extent.value()};
    expect(evaluator.evaluate(plan, proxied, {}).frame() != nullptr, "a proxy evaluates");
}

// A plan with time clocks claims the values of an operation's copies once, and Sharpen's mask is not
// a copy of its blur: the blur must not swallow the keyed amount on the mask, or the plan is refused
// as holding an unreferenced curve.
void testKeyedAmountOnClockedPlan() {
    auto definition = sharpen_test::sharpenPlan({1.0, 4.0, 0.0})->copyDefinition();
    definition.scalarCurves.push_back(runtime::CompiledScalarCurve{
        document::AnimationCurveId::fromRaw(48790),
        {{document::KeyframeId::fromRaw(48791), core::RationalTime::fromInteger(0), 0.0},
         {document::KeyframeId::fromRaw(48792), core::RationalTime::fromInteger(1), 2.0}}});
    auto keyed = false;
    for (auto& operation : definition.operations) {
        auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
        if (auto* kernel =
                effect != nullptr ? std::get_if<runtime::SharpenKernel>(&effect->kernel) : nullptr) {
            kernel->amount.source = runtime::ScalarCurveIndex::fromRaw(0);
            keyed = true;
        }
    }
    expect(keyed, "the plan holds a Sharpen to key");
    definition.clocks = {runtime::CompiledTimeClock{}};
    definition.operationClocks.assign(definition.operations.size(), 0U);
    const auto plan =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    const runtime::CpuCompositionEvaluator evaluator;
    const auto pictureAt = [&](const runtime::CompiledCompositionPlan& source,
                               const std::shared_ptr<const runtime::CompiledCompositionPlan>& held,
                               const std::int64_t frame) {
        auto request = requestFor(source);
        request.time = core::RationalTime::fromInteger(frame);
        const auto result = evaluator.evaluate(held, request, {});
        expect(result.frame() != nullptr, "the clocked plan evaluates");
        if (result.frame() == nullptr)
            return std::vector<render::Rgba32f>{};
        const auto span = result.frame()->processImage().pixels();
        return std::vector<render::Rgba32f>(span.begin(), span.end());
    };
    const auto none = sharpen_test::sharpenPlan({0.0, 4.0, 0.0});
    const auto strong = sharpen_test::sharpenPlan({2.0, 4.0, 0.0});
    expect(pictureAt(*plan, plan, 0) == pictureAt(*none, none, 0),
           "the keyed amount's first key gives the unsharpened picture");
    expect(pictureAt(*plan, plan, 1) == pictureAt(*strong, strong, 0),
           "the keyed amount's last key gives the strong picture");
}

void testPrepared() {
    using runtime::detail::ColorGradeKind;
    const auto grade = prepare({});
    expect(grade && grade->kind == ColorGradeKind::Sharpen && !grade->identity,
           "the default sharpen is a grade kind with work to do");
    expect(prepare({0.0, 3.0, 0.0})->identity, "no amount is an identity");
    expect(prepare({1.0, 0.0, 0.0})->identity, "no radius is an identity");
    expect(!prepare({2.0, 3.0, 0.5})->identity, "a threshold alone is not an identity");
    for (const auto& values :
         {SharpenValues{-0.1, 3.0, 0.0}, SharpenValues{10.5, 3.0, 0.0}, SharpenValues{1.0, -1.0, 0.0},
          SharpenValues{1.0, 257.0, 0.0}, SharpenValues{1.0, 3.0, -0.5},
          SharpenValues{1.0, 3.0, 2000.0},
          SharpenValues{std::numeric_limits<double>::quiet_NaN(), 3.0, 0.0}})
        expect(!prepare(values).has_value(), "a value outside its domain is refused");
    expect(!prepareColorGrade(kernelOf({}), std::vector<double>{1.0, 0.0}).has_value(),
           "operands without the radius are refused");
    expect(runtime::detail::colorGradeIdentity(*prepare({1.0, 3.0, 0.0})) !=
                   runtime::detail::colorGradeIdentity(*prepare({2.0, 3.0, 0.0})) &&
               runtime::detail::colorGradeIdentity(*prepare({1.0, 3.0, 0.0})) !=
                   runtime::detail::colorGradeIdentity(*prepare({1.0, 3.0, 0.5})),
           "another amount or threshold has another cache identity");
    // The radius belongs to the blur: changing it alone leaves this pass's table, and the blur's
    // pixels are its input.
    expect(runtime::detail::colorGradeIdentity(*prepare({1.0, 3.0, 0.0})) ==
               runtime::detail::colorGradeIdentity(*prepare({1.0, 9.0, 0.0})),
           "the radius is the blur's identity");
    expect(!runtime::detail::buildColorGradeProgram(ColorGradeKind::Sharpen, grade->data).succeeded(),
           "Sharpen has no grade program of its own");
    std::array<float, 4> pixel{0.5F, 0.5F, 0.5F, 1.0F};
    expect(!runtime::detail::colorGradePixel(*grade, pixel),
           "Sharpen is never applied without its blur");
}

void testBudgetAndCancellation() {
    const auto input =
        painted(96, 64, [](std::int64_t, std::int64_t) { return std::array{0.4F, 0.4F, 0.4F, 1.0F}; });
    const auto grade = prepare({});
    const auto blurred = blurOf(input, 3.0);
    auto refused =
        runtime::detail::applySharpen(*grade, input, blurred.get(), 16U, {},
                                      runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(refused.diagnostic &&
               refused.diagnostic->code ==
                   runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "the pixel budget refuses the output");
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(486)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Sharpen cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result = runtime::detail::applySharpen(
                *grade, input, blurred.get(), 1U << 26U, context.cancellation(),
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
           "a cancelled sharpen publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(scheduler.isQuiescent(), "the cancelled sharpen shuts down safely");
}

} // namespace

int main() {
    try {
        testPixel();
        testPicture();
        testEvaluator();
        testKeyedAmountOnClockedPlan();
        testPrepared();
        testBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "sharpen tests passed\n";
    return 0;
}
