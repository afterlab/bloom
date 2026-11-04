// #336 standalone Transform 2D: the CPU reference resample, its checked geometry and extents,
// degenerate transforms, signed/HDR/alpha preservation, proxy conjugation, budget refusal, row
// cancellation, and operation-cache identity/invalidation through the production evaluator.

#include "image_effect.hpp"
#include "transform_2d_geometry.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace bloom;
using runtime::Transform2DExtent;
using runtime::detail::PreparedTransform2D;
using runtime::detail::Transform2DValues;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

render::ImageWindow window(const std::int64_t x, const std::int64_t y, const std::uint64_t width,
                           const std::uint64_t height) {
    const auto created = render::ImageWindow::create(x, y, width, height);
    if (!created)
        throw std::logic_error("invalid Transform 2D fixture window");
    return *created.value();
}

// A 4x2 source of signed and HDR premultiplied RGB with partial coverage and one exact zero pixel.
render::Rgba32fImage
source(const render::ImageWindow data, const render::ImageWindow display,
       const core::PixelAspectRatio aspect = core::PixelAspectRatio::square()) {
    const auto descriptor = render::Rgba32fImageDescriptor::create(data, display, aspect);
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 20U);
    for (auto y = data.originY(); y < data.maxYExclusive(); ++y) {
        auto row = builder.value()->row(y);
        for (std::size_t column = 0; column < row.value()->size(); ++column) {
            const float a =
                column == 0 && y == data.originY() ? 0.0F : 0.25F + 0.125F * float(column);
            const auto pixel = render::Rgba32f::fromPremultiplied(
                -a, 4.0F * a, static_cast<float>(column + 1) * a * float(y - data.originY() + 1),
                a);
            (*row.value())[column] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

render::Rgba32fImage source() { return source(window(0, 0, 4, 2), window(0, 0, 4, 2)); }

Transform2DValues values(const document::Vec2d translate, const document::Vec2d scale = {1, 1},
                         const double rotation = 0.0, const document::Vec2d pivot = {},
                         const document::Vec2d centre = {2, 1}) {
    return {translate, pivot, scale, rotation, centre};
}

runtime::detail::ImageEffectResult
run(const Transform2DValues& sampled, const render::Rgba32fImage& input,
    const Transform2DExtent extent = Transform2DExtent::Transformed, const double sx = 1.0,
    const double sy = 1.0, const std::size_t budget = 1U << 20U,
    const std::int64_t filter = runtime::kResampleFilterBilinear) {
    runtime::detail::PreparedImageEffect effect;
    effect.transform2D = PreparedTransform2D{extent, filter, sampled};
    return runtime::detail::applyImageEffect(effect, input, nullptr, budget, {},
                                             runtime::OperationIndex::fromRaw(1), {}, sx, sy);
}

render::Rgba32f at(const render::Rgba32fImage& image, const std::int64_t x, const std::int64_t y) {
    const auto pixel = image.read(x, y);
    if (!pixel)
        throw std::logic_error("Transform 2D test coordinate outside the output data window");
    return *pixel.value();
}

void testIdentityAndPrepare() {
    runtime::detail::ImageEffectContext context;
    const runtime::Transform2DKernel kernel{};
    const runtime::CompiledImageEffect effect{document::NodeId::fromRaw(1),
                                              runtime::OperationIndex::fromRaw(0), kernel};
    const auto identity = context.prepare(effect, {}, {}, {}, {}, {}, 1.0, 1.0, {}, {},
                                          values({0, 0}, {1, 1}, 720.0));
    expect(identity.transform2D && identity.identity() && !identity.diagnostic,
           "unit scale, zero translation and whole turns are an exact passthrough");
    auto display = effect;
    std::get<runtime::Transform2DKernel>(display.kernel).extent = Transform2DExtent::Display;
    expect(
        !context.prepare(display, {}, {}, {}, {}, {}, 1.0, 1.0, {}, {}, values({0, 0})).identity(),
        "display clipping is applied even by an identity transform");
    const auto moved =
        context.prepare(effect, {}, {}, {}, {}, {}, 1.0, 1.0, {}, {}, values({0.5, 0}));
    expect(!moved.identity() && moved.cacheIdentity != identity.cacheIdentity,
           "every sampled value enters the prepared cache identity");
    const auto missing = context.prepare(effect, {}, {});
    expect(missing.diagnostic.has_value(), "unsampled parameters refuse with a diagnostic");
    const auto nan = context.prepare(effect, {}, {}, {}, {}, {}, 1.0, 1.0, {}, {},
                                     values({std::numeric_limits<double>::quiet_NaN(), 0}));
    expect(nan.diagnostic.has_value(), "non-finite parameters refuse with a diagnostic");
    auto badFilter = effect;
    std::get<runtime::Transform2DKernel>(badFilter.kernel).filter = 2;
    expect(context.prepare(badFilter, {}, {}, {}, {}, {}, 1.0, 1.0, {}, {}, values({1, 0}))
               .diagnostic.has_value(),
           "an unknown filter refuses with a diagnostic");
    auto nearest = effect;
    std::get<runtime::Transform2DKernel>(nearest.kernel).filter = runtime::kResampleFilterNearest;
    const auto nearestPrepared =
        context.prepare(nearest, {}, {}, {}, {}, {}, 1.0, 1.0, {}, {}, values({0.5, 0}));
    expect(!nearestPrepared.diagnostic && nearestPrepared.cacheIdentity != moved.cacheIdentity,
           "Nearest is accepted and keyed apart from Bilinear");
}

// #480: Nearest copies one stored tap per output pixel. A fractional move snaps to whole source
// pixels instead of blending neighbours, and a rotated, scaled image only ever holds source pixels
// or transparent black.
void testNearest() {
    const auto input = source();
    const auto moved = run(values({0.25, 0.75}), input, Transform2DExtent::Transformed, 1.0, 1.0,
                           1U << 20U, runtime::kResampleFilterNearest);
    const auto blended = run(values({0.25, 0.75}), input);
    expect(moved.image && blended.image, "a fractional Nearest move produces an image");
    if (moved.image && blended.image) {
        bool exact = true;
        for (const auto pixel : moved.image->pixels())
            exact = exact && (std::ranges::find(input.pixels(), pixel) != input.pixels().end() ||
                              pixel == render::Rgba32f::transparent());
        expect(exact, "a fractional Nearest move copies source pixels exactly");
        expect(!std::ranges::equal(moved.image->pixels(), blended.image->pixels()),
               "Bilinear blends the same fractional move");
    }
    const auto turned = run(values({0.3, -0.2}, {1.5, 0.75}, 33.0), input,
                            Transform2DExtent::Transformed, 1.0, 1.0, 1U << 20U,
                            runtime::kResampleFilterNearest);
    expect(turned.image != nullptr, "a rotated Nearest transform produces an image");
    if (turned.image) {
        bool exact = true;
        for (const auto pixel : turned.image->pixels())
            exact = exact && (std::ranges::find(input.pixels(), pixel) != input.pixels().end() ||
                              pixel == render::Rgba32f::transparent());
        expect(exact, "a rotated, scaled Nearest transform holds only source pixels");
    }
}

void testResample() {
    const auto input = source();
    // Whole-pixel translation is an exact sample copy (zero bilinear weights).
    const auto shifted = run(values({3, -1}), input);
    expect(shifted.image != nullptr, "integer translation produces an image");
    if (shifted.image) {
        bool exact = true;
        for (std::int64_t y = 0; y < 2; ++y)
            for (std::int64_t x = 0; x < 4; ++x)
                exact = exact && at(*shifted.image, x + 3, y - 1) == at(input, x, y);
        expect(exact, "integer translation copies signed/HDR/partial-alpha samples exactly");
        expect(shifted.image->descriptor()->displayWindow() ==
                       input.descriptor()->displayWindow() &&
                   shifted.image->descriptor()->pixelAspect() == input.descriptor()->pixelAspect(),
               "the display window and pixel aspect are retained");
        expect(shifted.image->descriptor()->dataWindow().contains(6, 0) &&
                   shifted.image->descriptor()->dataWindow().contains(3, -1),
               "the Transformed extent keeps overscan outside the display window");
    }
    // A clockwise quarter turn about the content centre maps pixel centres onto pixel centres.
    const auto turned = run(values({0, 0}, {1, 1}, 90.0), input);
    expect(turned.image != nullptr, "a quarter turn produces an image");
    if (turned.image) {
        expect(at(*turned.image, 2, -1) == at(input, 0, 0) &&
                   at(*turned.image, 1, -1) == at(input, 0, 1) &&
                   at(*turned.image, 2, 2) == at(input, 3, 0),
               "an exact quarter turn interpolates nothing");
    }
    // Nonuniform scale with a pivot offset: stretched X, squashed Y, bilinear in between.
    const auto scaled = run(values({0, 0}, {2.0, 0.5}, 0.0, {-2, -1}), input);
    expect(scaled.image != nullptr, "nonuniform scale produces an image");
    if (scaled.image) {
        const auto data = scaled.image->descriptor()->dataWindow();
        expect(data.originX() <= 0 && data.maxXExclusive() >= 8 && data.maxYExclusive() <= 3,
               "the output window covers the scaled support about the top-left pivot");
        bool negative = false, hdr = false, premultiplied = true;
        for (const auto pixel : scaled.image->pixels()) {
            negative = negative || pixel.red() < 0;
            hdr = hdr || pixel.green() > 1;
            premultiplied =
                premultiplied && (pixel.alpha() != 0 ||
                                  (pixel.red() == 0 && pixel.green() == 0 && pixel.blue() == 0));
        }
        expect(negative && hdr, "bilinear resampling preserves signed and HDR RGB");
        expect(premultiplied, "transparent taps remain exact transparent black");
    }
    // Fractional rotation: finite output, alpha bounded by the source maximum.
    const auto rotated = run(values({0.25, 0.5}, {1.5, 1.5}, 33.0, {0.5, 0}), input);
    expect(rotated.image != nullptr, "an arbitrary rotation produces an image");
    if (rotated.image) {
        bool bounded = true;
        for (const auto pixel : rotated.image->pixels())
            bounded = bounded && std::isfinite(pixel.red()) && pixel.alpha() >= 0.0F &&
                      pixel.alpha() <= 0.625F + 1e-6F;
        expect(bounded, "rotated coverage stays within the source coverage range");
    }
    const auto mirrored = run(values({0, 0}, {-1, 1}), input);
    expect(mirrored.image && at(*mirrored.image, 3, 0) == at(input, 0, 0) &&
               at(*mirrored.image, 0, 1) == at(input, 3, 1),
           "a negative scale mirrors about the content centre");
}

void testDegenerateAndExtents() {
    const auto input = source(window(-2, 0, 4, 2), window(0, 0, 6, 4));
    for (const document::Vec2d scale :
         {document::Vec2d{0, 1}, document::Vec2d{1, 0}, document::Vec2d{0, 0}}) {
        const auto collapsed = run(values({0, 0}, scale, 0.0, {}, {0, 1}), input);
        expect(collapsed.empty && !collapsed.image && !collapsed.diagnostic && !collapsed.cancelled,
               "a zero scale collapses to no image without a diagnostic");
    }
    const auto transformed = run(values({3, 0}, {1, 1}, 0.0, {}, {0, 1}), input);
    const auto clippedInput =
        run(values({3, 0}, {1, 1}, 0.0, {}, {0, 1}), input, Transform2DExtent::Input);
    const auto clippedDisplay =
        run(values({3, 0}, {1, 1}, 0.0, {}, {0, 1}), input, Transform2DExtent::Display);
    expect(transformed.image && clippedInput.image && clippedDisplay.image,
           "every extent produces an image for a partially overlapping translation");
    if (transformed.image && clippedInput.image && clippedDisplay.image) {
        const auto full = transformed.image->descriptor()->dataWindow();
        const auto inputWindow = clippedInput.image->descriptor()->dataWindow();
        const auto displayWindow = clippedDisplay.image->descriptor()->dataWindow();
        expect(inputWindow == window(0, 0, 2, 2),
               "Input clips the transformed support to the input data window");
        expect(displayWindow == window(0, 0, 6, 3),
               "Display clips the transformed support to the input display window");
        expect(full == window(0, -1, 6, 4), "Transformed keeps the whole transformed support");
        expect(at(*clippedInput.image, 1, 0) == at(*transformed.image, 1, 0) &&
                   at(*clippedDisplay.image, 2, 1) == at(*transformed.image, 2, 1),
               "clipping changes only the data window, never the resampled values");
    }
    const auto away =
        run(values({40, 0}, {1, 1}, 0.0, {}, {0, 1}), input, Transform2DExtent::Display);
    expect(away.empty && !away.diagnostic,
           "support outside the selected extent publishes no image without a diagnostic");
    const auto identityDisplay =
        run(values({0, 0}, {1, 1}, 0.0, {}, {0, 1}), input, Transform2DExtent::Display);
    expect(identityDisplay.image &&
               identityDisplay.image->descriptor()->dataWindow() == window(0, 0, 3, 3) &&
               at(*identityDisplay.image, 0, 1) == at(input, 0, 1),
           "identity with Display clips overscan and copies the visible samples exactly");
    const auto huge = run(values({1e9, 0}), source());
    expect(huge.diagnostic && !huge.image,
           "support beyond the supported coordinate range refuses with a diagnostic");
    const auto budget =
        run(values({0.5, 0}, {8, 8}), source(), Transform2DExtent::Transformed, 1.0, 1.0, 64);
    expect(budget.diagnostic && !budget.image, "allocation obeys the remaining pixel budget");
}

void testProxy() {
    // At an equal-axis half proxy the authored translation is conjugated, not re-authored.
    const auto proxyInput = source(window(0, 0, 4, 2), window(0, 0, 4, 2));
    const auto proxy = run(values({4, 2}, {1, 1}, 0.0, {}, {4, 2}), proxyInput,
                           Transform2DExtent::Transformed, 0.5, 0.5);
    expect(proxy.image && at(*proxy.image, 2, 1) == at(proxyInput, 0, 0) &&
               at(*proxy.image, 5, 2) == at(proxyInput, 3, 1),
           "a half proxy moves an authored 4x2 translation by 2x1 device pixels exactly");
    const auto geometry = runtime::detail::resolveTransform2DGeometry(
        PreparedTransform2D{Transform2DExtent::Transformed, 0, values({0, 0}, {1, 1}, 90.0)},
        *proxyInput.descriptor(), 0.5, 0.25);
    expect(geometry && geometry->window.has_value(),
           "an unequal-axis proxy rotation still resolves a checked output window");
}

void testCancellation() {
    const auto input = source();
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    const runtime::TaskOwner owner{runtime::TaskOwnerKind::Composition,
                                   runtime::TaskOwnerId::fromRaw(1)};
    auto group = scheduler.createGroup(owner, "Transform 2D cancellation");
    runtime::TaskRequest request{"Transform 2D row cancellation", owner};
    request.groupId = group.handle.id();
    std::atomic_bool observedCancellation = false;
    auto submission = scheduler.submit<void>(
        request, [&input, &group, &observedCancellation](runtime::TaskContext& context) {
            runtime::detail::PreparedImageEffect effect;
            effect.transform2D =
                PreparedTransform2D{Transform2DExtent::Transformed, 0, values({0.5, 0}, {4, 4})};
            const auto stopped = runtime::detail::applyImageEffect(
                effect, input, nullptr, 1U << 20U, context.cancellation(),
                runtime::OperationIndex::fromRaw(1), [&group](const auto& progress) {
                    if (progress.completed > 0)
                        group.handle.cancel();
                });
            observedCancellation.store(stopped.cancelled && !stopped.image);
            return stopped.cancelled && !stopped.image ? runtime::TaskResult<void>::cancelled()
                                                       : runtime::TaskResult<void>::succeeded();
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::optional<runtime::TaskResult<void>> stopped;
    while (!stopped && std::chrono::steady_clock::now() < deadline) {
        stopped = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(stopped && stopped->state() == runtime::TaskState::Cancelled &&
               observedCancellation.load(),
           "row cancellation retires the task without publishing a partial image");
    scheduler.beginShutdown();
}

// Solid -> Transform 2D -> Layer -> Merge -> Output, through the production CPU evaluator.
std::shared_ptr<const runtime::CompiledCompositionPlan> plan(const document::Vec2d translate,
                                                             const double rotation) {
    using namespace runtime;
    using namespace document;
    const auto op = [](const std::size_t i) { return OperationIndex::fromRaw(i); };
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(1),
                      {ParameterId::fromRaw(1), core::Color4d{-0.5, 2.0, 0.25, 0.5}},
                      {ParameterId::fromRaw(2), 4.0},
                      {ParameterId::fromRaw(3), 2.0}});
    operations.emplace_back(
        CompiledImageEffect{NodeId::fromRaw(2), op(0),
                            Transform2DKernel{{ParameterId::fromRaw(20), translate},
                                              {ParameterId::fromRaw(21), Vec2d{}},
                                              {ParameterId::fromRaw(22), Vec2d{1, 1}},
                                              {ParameterId::fromRaw(23), rotation}}});
    operations.emplace_back(CompiledLayerOutput{NodeId::fromRaw(3),
                                                LayerId::fromRaw(1),
                                                op(1),
                                                {ParameterId::fromRaw(10), Vec2d{4, 3}},
                                                {ParameterId::fromRaw(11), Vec2d{}},
                                                {ParameterId::fromRaw(12), Vec2d{1, 1}},
                                                {ParameterId::fromRaw(13), 0.0},
                                                {ParameterId::fromRaw(14), 1.0},
                                                ParameterId::fromRaw(15)});
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(4), {{LayerSlotId::fromRaw(1), LayerId::fromRaw(1), op(2)}}});
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(5), op(3)});
    const auto format = CompositionFormat::create(8, 6);
    if (!format)
        throw std::logic_error("Invalid fixture format");
    return std::make_shared<const CompiledCompositionPlan>(CompiledCompositionPlanDefinition{
        Revision::fromRaw(1), ProjectId::fromRaw(1), CompositionId::fromRaw(1), *format,
        std::move(operations), op(4)});
}

void testEvaluatorCache() {
    runtime::CpuCompositionEvaluator evaluator;
    const auto requestFor = [](const runtime::CompiledCompositionPlan& compiled) {
        runtime::EvaluationRequest request{
            {}, compiled.output(), runtime::CompositionFormatResolution{}};
        request.pixelStorageByteLimit = 1U << 20U;
        request.colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
        return request;
    };
    const auto still = plan({0, 0}, 0.0);
    const auto moved = plan({1, 0}, 0.0);
    const auto baseline = evaluator.evaluate(still, requestFor(*still), {});
    const auto cold = evaluator.evaluate(moved, requestFor(*moved), {});
    const auto warm = evaluator.evaluate(moved, requestFor(*moved), {});
    expect(baseline.frame() && cold.frame() && warm.frame(),
           "Transform 2D graphs evaluate through the production CPU evaluator");
    if (!baseline.frame() || !cold.frame() || !warm.frame())
        return;
    expect(!std::ranges::equal(baseline.frame()->processImage().pixels(),
                               cold.frame()->processImage().pixels()),
           "a translated Transform 2D stays visible through a downstream Layer");
    expect(std::ranges::equal(cold.frame()->processImage().pixels(),
                              warm.frame()->processImage().pixels()) &&
               warm.frame()->operationCacheStatistics().misses == 0 &&
               warm.frame()->operationCacheStatistics().hits > 0,
           "an unchanged Transform 2D frame is a warm operation-cache hit");
    const auto turned = plan({1, 0}, 45.0);
    const auto changed = evaluator.evaluate(turned, requestFor(*turned), {});
    expect(changed.frame() && changed.frame()->operationCacheStatistics().misses > 0 &&
               !std::ranges::equal(changed.frame()->processImage().pixels(),
                                   cold.frame()->processImage().pixels()),
           "a changed transform value invalidates the cached Transform 2D output");
    const auto collapsed = evaluator.evaluate(
        [] {
            auto definition = plan({0, 0}, 0.0)->copyDefinition();
            std::get<runtime::Transform2DKernel>(
                std::get<runtime::CompiledImageEffect>(definition.operations[1]).kernel)
                .scale.source = document::Vec2d{0, 1};
            return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
        }(),
        requestFor(*still), {});
    bool transparent = collapsed.frame() != nullptr;
    if (collapsed.frame())
        for (const auto pixel : collapsed.frame()->processImage().pixels())
            transparent = transparent && pixel.alpha() == 0.0F;
    expect(transparent, "a collapsed Transform 2D contributes nothing to the composition");
}
} // namespace

int main() {
    try {
        testIdentityAndPrepare();
        testResample();
        testNearest();
        testDegenerateAndExtents();
        testProxy();
        testCancellation();
        testEvaluatorCache();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
