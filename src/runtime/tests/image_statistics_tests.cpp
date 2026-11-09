// #349 Image Statistics on the CPU reference: known answers read from real pixels (extremes, mean
// and deviation of a split picture, a channel, luminance, alpha), the counting rules (straight and
// stored values, pixels with alpha, values that are not usable numbers, windows with an origin),
// tile independence against an independent double precision measure, domain refusal, the pixel
// budget, cancellation, the tables' cache identity, the statistics cache (a frame's own statistics,
// no other frame's, nothing cached for a failure, bounded and retired), and a frame whose size an
// Image Statistics value drives.

#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"
#include "statistics_image_effect.hpp"
#include "statistics_test_support.hpp"

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/image_statistics.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
namespace detail = runtime::detail;
namespace doc = bloom::document;
namespace st = bloom::statistics_test;
using runtime::StatisticsRole;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using Pixel = std::array<float, 4>;
using ImagePtr = std::shared_ptr<const render::Rgba32fImage>;

bool near(const double a, const double b, const double tolerance = 1e-5) {
    return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b));
}

render::Rgba32fImage make(const std::int64_t ox, const std::int64_t oy, const std::uint32_t w,
                          const std::uint32_t h,
                          const std::function<Pixel(std::int64_t, std::int64_t)>& fill) {
    const auto window = render::ImageWindow::create(ox, oy, w, h);
    auto descriptor = render::Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (auto y = oy; y < oy + static_cast<std::int64_t>(h); ++y)
        for (auto x = ox; x < ox + static_cast<std::int64_t>(w); ++x) {
            const auto p = fill(x, y);
            const auto stored = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!stored || builder.value()->write(x, y, *stored.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

runtime::StatisticsKernel kernel(const StatisticsRole role, const std::int64_t channel,
                                 const bool straight, const bool opaqueOnly,
                                 const std::int64_t grid = 32) {
    runtime::StatisticsKernel result;
    result.role = role;
    result.channel = channel;
    result.straight = straight;
    result.opaqueOnly = opaqueOnly;
    result.grid = grid;
    return result;
}

std::optional<detail::PreparedColorGrade> prepare(const runtime::StatisticsKernel& value) {
    return detail::prepareStatisticsGrade(value);
}

// Reduce then Sum, the chain the compiler lowers to.
struct Chain final {
    ImagePtr tiles;
    ImagePtr result;
};
Chain chainOf(const render::Rgba32fImage& picture, const std::int64_t channel, const bool straight,
              const bool opaqueOnly, const std::int64_t grid = 32) {
    const auto reduce = prepare(kernel(StatisticsRole::Reduce, channel, straight, opaqueOnly, grid));
    const auto sum = prepare(kernel(StatisticsRole::Sum, channel, straight, opaqueOnly, grid));
    if (!reduce || !sum)
        throw std::logic_error("the kernels prepare");
    Chain chain;
    chain.tiles = detail::applyStatistics(*reduce, &picture, 1U << 26U, {},
                                          runtime::OperationIndex::fromRaw(0), {}, nullptr)
                      .image;
    if (!chain.tiles)
        throw std::logic_error("the reduction applies");
    chain.result = detail::applyStatistics(*sum, chain.tiles.get(), 1U << 26U, {},
                                           runtime::OperationIndex::fromRaw(1), {}, nullptr)
                       .image;
    if (!chain.result)
        throw std::logic_error("the sum applies");
    return chain;
}

runtime::ImageStatisticsValues measure(const render::Rgba32fImage& picture,
                                       const std::int64_t channel, const bool straight,
                                       const bool opaqueOnly, const std::int64_t grid = 32) {
    const auto chain = chainOf(picture, channel, straight, opaqueOnly, grid);
    const auto values =
        runtime::readImageStatistics(*chain.result, runtime::ImageStatisticsRoute::CpuReference);
    if (!values)
        throw std::logic_error("the result reads");
    return *values;
}

void expectValues(const runtime::ImageStatisticsValues& v, const double minimum, const double maximum,
                  const double mean, const double deviation, const double count,
                  const std::string& what) {
    expect(near(v.minimum, minimum) && near(v.maximum, maximum) && near(v.mean, mean) &&
               near(v.deviation, deviation) && v.count == count,
           what + " (got min " + std::to_string(v.minimum) + " max " + std::to_string(v.maximum) +
               " mean " + std::to_string(v.mean) + " dev " + std::to_string(v.deviation) +
               " count " + std::to_string(v.count) + ")");
}

void testKnownAnswers() {
    // 12 by 10 pixels at a window origin that is not zero: nine columns 0.25, three columns 0.75.
    const auto image = make(3, 2, 12, 10, [](const std::int64_t x, auto) {
        return x < 12 ? Pixel{0.25F, 0.25F, 0.25F, 1.0F} : Pixel{0.75F, 0.75F, 0.75F, 1.0F};
    });
    const auto v = measure(image, doc::kStatisticsRed, true, false);
    expectValues(v, 0.25, 0.75, 0.375, std::sqrt(0.046875), 120.0,
                 "a split picture: extremes, mean 0.375 and the population deviation");
    const auto& window = chainOf(image, doc::kStatisticsRed, true, false).result->descriptor()->dataWindow();
    expect(window.originX() == 0 && window.originY() == 0 && window.extent().width() == 2 &&
               window.extent().height() == 1,
           "the result is two pixels at the origin of the composition, not the picture's");
    // A constant picture has a deviation of exactly 0.
    const auto flat = make(0, 0, 33, 17, [](auto, auto) { return Pixel{0.5F, 0.5F, 0.5F, 1.0F}; });
    expectValues(measure(flat, doc::kStatisticsGreen, true, false), 0.5, 0.5, 0.5, 0.0, 561.0,
                 "a constant picture: equal extremes, no deviation");
    // Channels of a colored pixel: red 1, green 0.5, blue 0, alpha 1, luminance 0.2126 + 0.3576.
    const auto colored = make(0, 0, 4, 4, [](auto, auto) { return Pixel{1.0F, 0.5F, 0.0F, 1.0F}; });
    expectValues(measure(colored, doc::kStatisticsRed, true, false), 1.0, 1.0, 1.0, 0.0, 16.0, "red");
    expectValues(measure(colored, doc::kStatisticsGreen, true, false), 0.5, 0.5, 0.5, 0.0, 16.0, "green");
    expectValues(measure(colored, doc::kStatisticsBlue, true, false), 0.0, 0.0, 0.0, 0.0, 16.0, "blue");
    expectValues(measure(colored, doc::kStatisticsAlpha, true, false), 1.0, 1.0, 1.0, 0.0, 16.0, "alpha");
    expectValues(measure(colored, doc::kStatisticsLuminance, true, false), 0.5702, 0.5702, 0.5702,
                 0.0, 16.0, "the Rec. 709 luminance");
    // HDR and negative values are measured as they are.
    const auto hdr = make(0, 0, 2, 1, [](const std::int64_t x, auto) {
        return x == 0 ? Pixel{-3.0F, 0.0F, 0.0F, 1.0F} : Pixel{1000.0F, 0.0F, 0.0F, 1.0F};
    });
    expectValues(measure(hdr, doc::kStatisticsRed, false, false), -3.0, 1000.0, 498.5, 501.5, 2.0,
                 "HDR and negative values are not clamped");
}

void testCountingRules() {
    // Four pixels: opaque 0.5, half alpha stored 0.1 (straight 0.2), transparent, opaque 2.
    const auto image = make(0, 0, 4, 1, [](const std::int64_t x, auto) {
        const std::array<Pixel, 4> pixels{Pixel{0.5F, 0.5F, 0.5F, 1.0F}, Pixel{0.1F, 0.1F, 0.1F, 0.5F},
                                          Pixel{0.0F, 0.0F, 0.0F, 0.0F}, Pixel{2.0F, 2.0F, 2.0F, 1.0F}};
        return pixels[static_cast<std::size_t>(x)];
    });
    // Straight: the transparent pixel has no color; 0.2 is the half-alpha pixel's.
    expectValues(measure(image, doc::kStatisticsRed, true, false), 0.2, 2.0, 2.7 / 3.0,
                 std::sqrt(((0.5 - 0.9) * (0.5 - 0.9) + (0.2 - 0.9) * (0.2 - 0.9) +
                            (2.0 - 0.9) * (2.0 - 0.9)) /
                           3.0),
                 3.0, "straight color divides by alpha and a transparent pixel counts nowhere");
    // Stored: every pixel counts, the transparent one as 0.
    expectValues(measure(image, doc::kStatisticsRed, false, false), 0.0, 2.0, 2.6 / 4.0,
                 std::sqrt(((0.5 - 0.65) * (0.5 - 0.65) + (0.1 - 0.65) * (0.1 - 0.65) +
                            (0.0 - 0.65) * (0.0 - 0.65) + (2.0 - 0.65) * (2.0 - 0.65)) /
                           4.0),
                 4.0, "stored values count as they are");
    // Pixels with alpha: the transparent pixel is not counted even in stored values.
    expectValues(measure(image, doc::kStatisticsRed, false, true), 0.1, 2.0, 2.6 / 3.0,
                 std::sqrt(((0.5 - 2.6 / 3.0) * (0.5 - 2.6 / 3.0) + (0.1 - 2.6 / 3.0) * (0.1 - 2.6 / 3.0) +
                            (2.0 - 2.6 / 3.0) * (2.0 - 2.6 / 3.0)) /
                           3.0),
                 3.0, "only pixels with alpha above zero count");
    // Alpha itself: all four pixels, 1, 0.5, 0 and 1.
    expectValues(measure(image, doc::kStatisticsAlpha, true, false), 0.0, 1.0, 0.625,
                 std::sqrt(((0.375) * (0.375) + (0.125) * (0.125) + (0.625) * (0.625) + (0.375) * (0.375)) / 4.0),
                 4.0, "alpha counts every pixel");
    // Values that are not usable numbers: above the limit, and a straight division that overflows.
    const auto huge = make(0, 0, 3, 1, [](const std::int64_t x, auto) {
        const std::array<Pixel, 3> pixels{Pixel{4.0F, 0.0F, 0.0F, 1.0F}, Pixel{2.0e13F, 0.0F, 0.0F, 1.0F},
                                          Pixel{1.0F, 0.0F, 0.0F, 1.0e-39F}};
        return pixels[static_cast<std::size_t>(x)];
    });
    expectValues(measure(huge, doc::kStatisticsRed, true, false), 4.0, 4.0, 4.0, 0.0, 1.0,
                 "a value above 1e12 and a straight value that overflows are not counted");
    // Nothing counted: every statistic and the count are 0.
    const auto clear = make(0, 0, 8, 8, [](auto, auto) { return Pixel{0.0F, 0.0F, 0.0F, 0.0F}; });
    expectValues(measure(clear, doc::kStatisticsRed, true, false), 0.0, 0.0, 0.0, 0.0, 0.0,
                 "a picture with nothing to count gives zeros and a count of 0");
    // A picture smaller than the result (one pixel) still reduces.
    const auto one = make(5, 7, 1, 1, [](auto, auto) { return Pixel{0.75F, 0.0F, 0.0F, 1.0F}; });
    expectValues(measure(one, doc::kStatisticsRed, true, false), 0.75, 0.75, 0.75, 0.0, 1.0,
                 "a one-pixel picture");
}

void testTileIndependence() {
    // A picture that is not a multiple of any tile size, measured with several grids against an
    // independent double precision measure of the same pixels.
    constexpr std::int64_t kWidth = 131;
    constexpr std::int64_t kHeight = 77;
    std::mt19937 random(349);
    std::uniform_real_distribution<float> values(-4.0F, 40.0F);
    std::vector<Pixel> pixels(static_cast<std::size_t>(kWidth * kHeight));
    for (auto& pixel : pixels)
        pixel = {values(random), values(random), values(random), 1.0F};
    const auto image = make(-9, 4, kWidth, kHeight, [&](const std::int64_t x, const std::int64_t y) {
        return pixels[static_cast<std::size_t>((y - 4) * kWidth + (x + 9))];
    });
    double sum = 0.0;
    double low = 1e30;
    double high = -1e30;
    for (const auto& pixel : pixels) {
        sum += static_cast<double>(pixel[1]);
        low = std::min(low, static_cast<double>(pixel[1]));
        high = std::max(high, static_cast<double>(pixel[1]));
    }
    const double mean = sum / static_cast<double>(pixels.size());
    double squares = 0.0;
    for (const auto& pixel : pixels)
        squares += (static_cast<double>(pixel[1]) - mean) * (static_cast<double>(pixel[1]) - mean);
    const double deviation = std::sqrt(squares / static_cast<double>(pixels.size()));
    for (const std::int64_t grid : {1, 2, 7, 32, 256}) {
        const auto v = measure(image, doc::kStatisticsGreen, true, false, grid);
        expect(v.minimum == low && v.maximum == high && v.count == static_cast<double>(pixels.size()) &&
                   near(v.mean, mean, 1e-5) && near(v.deviation, deviation, 1e-4),
               "the statistics do not depend on the tile grid " + std::to_string(grid));
    }
    // The grid does change the table.
    expect(prepare(kernel(StatisticsRole::Reduce, 1, true, false, 8))->data !=
               prepare(kernel(StatisticsRole::Reduce, 1, true, false, 9))->data,
           "a different grid is a different table");
}

void testRefusalsAndIdentity() {
    expect(!prepare(kernel(StatisticsRole::Reduce, 9, true, false)), "an unknown channel is refused");
    expect(!prepare(kernel(StatisticsRole::Reduce, -1, true, false)), "a negative channel is refused");
    expect(!prepare(kernel(StatisticsRole::Reduce, 0, true, false, 0)), "a grid of 0 is refused");
    expect(!prepare(kernel(StatisticsRole::Reduce, 0, true, false, 257)), "a grid beyond 256 is refused");
    const auto base = prepare(kernel(StatisticsRole::Reduce, 1, true, false));
    for (const auto& other :
         {kernel(StatisticsRole::Reduce, 2, true, false), kernel(StatisticsRole::Reduce, 1, false, false),
          kernel(StatisticsRole::Reduce, 1, true, true), kernel(StatisticsRole::Sum, 1, true, false)}) {
        const auto table = prepare(other);
        expect(base && table && table->data != base->data, "a changed setting changes the table");
    }
    expect(base && base->data == prepare(kernel(StatisticsRole::Reduce, 1, true, false))->data,
           "equal settings give an equal table");
    const auto picture = make(0, 0, 64, 64, [](auto, auto) { return Pixel{0.5F, 0.5F, 0.5F, 1.0F}; });
    const auto reduce = prepare(kernel(StatisticsRole::Reduce, 1, true, false));
    const auto over =
        detail::applyStatistics(*reduce, &picture, 16, {}, runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(!over.image && over.diagnostic &&
               over.diagnostic->code == runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "a pixel budget that cannot hold the result is refused");
    const auto none = detail::applyStatistics(*reduce, nullptr, 1U << 20U, {},
                                              runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(none.empty && !none.image, "a pass with no picture publishes none");
    expect(detail::statisticsProgramData(*reduce, 64, 64).has_value() &&
               detail::validStatisticsProgramData(*detail::statisticsProgramData(*reduce, 64, 64)),
           "the program table is valid");
    expect(!detail::statisticsProgramData(*reduce, 0, 64), "a picture with no width has no program");
    const auto wrong = make(0, 0, 3, 1, [](auto, auto) { return Pixel{}; });
    expect(!runtime::readImageStatistics(wrong, runtime::ImageStatisticsRoute::CpuReference),
           "a result of the wrong size is not read");
}

void testCancellation() {
    const auto input = make(0, 0, 256, 256, [](auto, auto) { return Pixel{0.5F, 0.5F, 0.5F, 1.0F}; });
    const auto grade = prepare(kernel(StatisticsRole::Reduce, 1, true, false));
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(3490)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Image Statistics cancellation", owner),
        [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result =
                detail::applyStatistics(*grade, &input, 1U << 26U, context.cancellation(),
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
           "a cancelled pass publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

// ---- the statistics cache ----

runtime::ImageStatisticsValues sample(const double count) {
    runtime::ImageStatisticsValues v;
    v.count = count;
    v.maximum = count;
    return v;
}

void testCache() {
    runtime::clearImageStatistics();
    const auto planA = st::statisticsPlan({}, 35000);
    const auto planB = st::statisticsPlan({}, 35100);
    const runtime::ImageStatisticsQuery frame0{doc::NodeId::fromRaw(1), planA,
                                               core::RationalTime::fromInteger(0)};
    const runtime::ImageStatisticsQuery frame1{doc::NodeId::fromRaw(1), planA,
                                               core::RationalTime::fromInteger(1)};
    int calls = 0;
    const auto provider = [&](const runtime::ImageStatisticsQuery& query,
                              const runtime::CancellationToken&) -> runtime::ImageStatisticsRun {
        ++calls;
        return {{runtime::ImageStatisticsOutcome::Ready, {}},
                sample(static_cast<double>(query.time.numerator()) + 10.0)};
    };
    expect(!runtime::ImageStatisticsPreparation::findHere(frame0) &&
               !runtime::ImageStatisticsPreparation::prepareHere(frame0),
           "with no preparation on the thread nothing is found or computed: not ready");
    {
        const runtime::ImageStatisticsPreparation preparation("frame", provider, {});
        expect(runtime::ImageStatisticsPreparation::installedHere(), "a preparation is installed here");
        expect(!runtime::ImageStatisticsPreparation::findHere(frame0),
               "before it is prepared a frame's statistics are not found");
        expect(runtime::ImageStatisticsPreparation::prepareHere(frame0)->outcome ==
                   runtime::ImageStatisticsOutcome::Ready,
               "the frame's statistics are prepared");
        const auto found0 = runtime::ImageStatisticsPreparation::findHere(frame0);
        expect(found0 && found0->count == 10.0, "the frame reads its own statistics");
        expect(!runtime::ImageStatisticsPreparation::findHere(frame1),
               "another frame does not read them: it is not ready");
        expect(runtime::ImageStatisticsPreparation::prepareHere(frame0)->outcome ==
                       runtime::ImageStatisticsOutcome::Ready && calls == 1,
               "the same frame again is a hit and measures nothing");
        static_cast<void>(runtime::ImageStatisticsPreparation::prepareHere(frame1));
        const auto found1 = runtime::ImageStatisticsPreparation::findHere(frame1);
        expect(calls == 2 && found1 && found1->count == 11.0 && found0->count == 10.0,
               "another time is another measure and each frame keeps its own");
        // The same time of a plan with other content is another measure, never the old one.
        const runtime::ImageStatisticsQuery other{doc::NodeId::fromRaw(1), planB,
                                                  core::RationalTime::fromInteger(0)};
        expect(!runtime::ImageStatisticsPreparation::findHere(other),
               "another plan does not read the first plan's statistics");
    }
    {
        const runtime::ImageStatisticsPreparation preparation("other frame", provider, {});
        expect(!runtime::ImageStatisticsPreparation::findHere(frame0),
               "a frame at another resolution or color does not read them");
    }
    {
        // A failed or cancelled measure caches nothing.
        runtime::clearImageStatistics();
        int attempts = 0;
        const runtime::ImageStatisticsPreparation preparation(
            "frame",
            [&](const runtime::ImageStatisticsQuery&,
                const runtime::CancellationToken&) -> runtime::ImageStatisticsRun {
                ++attempts;
                if (attempts == 1)
                    return {{runtime::ImageStatisticsOutcome::Unavailable, "no picture"}, std::nullopt};
                if (attempts == 2)
                    return {{runtime::ImageStatisticsOutcome::Cancelled, {}}, std::nullopt};
                return {{runtime::ImageStatisticsOutcome::Ready, {}}, sample(3.0)};
            },
            {});
        expect(runtime::ImageStatisticsPreparation::prepareHere(frame0)->outcome ==
                   runtime::ImageStatisticsOutcome::Unavailable,
               "an unavailable picture is reported");
        expect(runtime::ImageStatisticsPreparation::prepareHere(frame0)->outcome ==
                   runtime::ImageStatisticsOutcome::Cancelled,
               "a cancelled measure is reported");
        expect(!runtime::ImageStatisticsPreparation::findHere(frame0) &&
                   runtime::imageStatisticsCacheStatistics().entries == 0,
               "nothing is cached for a failed or cancelled measure");
        expect(runtime::ImageStatisticsPreparation::prepareHere(frame0)->outcome ==
                       runtime::ImageStatisticsOutcome::Ready &&
                   attempts == 3,
               "the next attempt measures again");
    }
    {
        // Retirement: the cache is bounded and a purge empties it.
        runtime::clearImageStatistics();
        const runtime::ImageStatisticsPreparation preparation("frame", provider, {});
        for (std::int64_t frame = 0; frame < static_cast<std::int64_t>(runtime::kImageStatisticsCacheEntries) + 40; ++frame)
            static_cast<void>(runtime::ImageStatisticsPreparation::prepareHere(
                {doc::NodeId::fromRaw(1), planA, core::RationalTime::fromInteger(frame)}));
        const auto statistics = runtime::imageStatisticsCacheStatistics();
        expect(statistics.entries == runtime::kImageStatisticsCacheEntries && statistics.evicted == 40,
               "the cache holds its bound and retires the least recently used");
        runtime::clearImageStatistics();
        expect(runtime::imageStatisticsCacheStatistics().entries == 0, "a purge empties the cache");
    }
}

// ---- the value graph ----

struct Rendered final {
    bool evaluated = false;
    std::string diagnostic;
    std::int64_t top = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

Rendered render(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                const std::int64_t frame = 0) {
    runtime::CpuCompositionEvaluator evaluator;
    runtime::EvaluationRequest request{core::RationalTime::fromInteger(frame), plan->output(),
                                       runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = 1U << 28U;
    const auto result = evaluator.evaluate(plan, request, {});
    Rendered out;
    for (const auto& diagnostic : result.diagnostics())
        out.diagnostic += diagnostic.summary + "; ";
    if (result.status() != runtime::EvaluationStatus::Evaluated || result.frame() == nullptr)
        return out;
    out.evaluated = true;
    const auto window = result.frame()->processImage().descriptor()->dataWindow();
    out.top = window.originY();
    out.width = 40;
    // The height of the white Solid: the opaque pixels down the middle of the composition.
    const auto& image = result.frame()->processImage();
    for (std::int64_t y = window.originY(); y < window.originY() + static_cast<std::int64_t>(window.extent().height()); ++y)
        if (const auto pixel = image.read(32, y); pixel && pixel.value()->alpha() > 0.5F)
            ++out.height;
    return out;
}

void testValueGraph() {
    runtime::clearImageStatistics();
    // The statistics plan alone, through the evaluator: its output is the two-pixel result.
    st::StatisticsValues red;
    red.gradient = false;
    red.color = core::Color4d{24.0, 0.0, 0.0, 1.0};
    red.channel = doc::kStatisticsRed;
    const auto tap = st::statisticsPlan(red, 35200);
    runtime::CpuCompositionEvaluator evaluator;
    runtime::EvaluationRequest request{core::RationalTime::fromInteger(0), tap->output(),
                                       runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = 1U << 28U;
    request.output = tap->output();
    const auto resultWindow = render::ImageWindow::create(0, 0, 2, 1);
    request.roi = *resultWindow.value();
    const auto direct = evaluator.evaluate(tap, request, {});
    std::string why;
    for (const auto& diagnostic : direct.diagnostics())
        why += diagnostic.summary + " / " + diagnostic.detail + "; ";
    expect(direct.status() == runtime::EvaluationStatus::Evaluated && direct.frame() != nullptr,
           "a statistics plan evaluates: " + why);
    if (direct.frame()) {
        const auto w = direct.frame()->processImage().descriptor()->dataWindow();
        expect(w.originX() == 0 && w.originY() == 0 && w.extent().width() == 2 && w.extent().height() == 1,
               "the plan's frame is the two-pixel window asked for");
        const auto values = runtime::readImageStatistics(direct.frame()->processImage(),
                                                         runtime::ImageStatisticsRoute::CpuReference);
        expectValues(values.value_or(runtime::ImageStatisticsValues{}), 24.0, 24.0, 24.0, 0.0, 3072.0,
                     "the plan's output holds the picture's statistics (a 64 by 48 red 24 picture)");
    }
    // A frame whose height an Image Statistics value drives: the picture's maximum is 24.
    const auto driven = st::drivenPlan(tap, 1, 35300);
    const auto frame = render(driven);
    expect(frame.evaluated, "a frame driven by Image Statistics evaluates: " + frame.diagnostic);
    expect(frame.height == 24 && frame.width == 40,
           "the frame is as high as the picture's maximum, 24 (got " + std::to_string(frame.height) + ")");
    const auto count = render(st::drivenPlan(tap, 4, 35400));
    expect(count.evaluated && count.height == 64,
           "the count of 3072 pixels drives a height clamped to the composition");
    // The frame's own statistics were prepared, cached once and labelled with the route.
    expect(runtime::imageStatisticsCacheStatistics().computed >= 1, "the statistics were computed");
    // Another picture at the same time is another measure: no stale statistics.
    red.color = core::Color4d{10.0, 0.0, 0.0, 1.0};
    const auto other = render(st::drivenPlan(st::statisticsPlan(red, 35200), 1, 35300));
    expect(other.evaluated && other.height == 10,
           "an edited picture gives its own statistics, not the cache's (got " +
               std::to_string(other.height) + ")");
    // Outside a frame's preparation the value is not ready: zeros and a diagnostic, not a value.
    const auto operations = driven->valueOperations();
    const auto notReady = runtime::evaluateValueGraph(operations, driven->valueOutputCount(),
                                                      core::RationalTime::fromInteger(0),
                                                      document::FrameRate::framesPerSecond24());
    bool zeros = notReady.outputs.size() == 5;
    for (const auto& value : notReady.outputs)
        zeros = zeros && std::get<double>(value) == 0.0;
    expect(zeros && notReady.diagnostics.size() == 1 &&
               notReady.diagnostics[0].summary == "Image Statistics are not ready",
           "outside a worker's preparation the statistics are reported as not ready");
    // With no picture wired the outputs are 0 and nothing is reported.
    const auto none = std::make_shared<const runtime::CompiledValueOperation>(runtime::CompiledValueOperation{
        doc::NodeId::fromRaw(7), runtime::ValueOutputIndex::fromRaw(0), 5,
        runtime::CompiledValueImageStatistics{doc::NodeId::fromRaw(7), nullptr}});
    const auto unwired = runtime::evaluateValueGraph(std::span(none.get(), 1), 5,
                                                     core::RationalTime::fromInteger(0),
                                                     document::FrameRate::framesPerSecond24());
    expect(unwired.diagnostics.empty() && unwired.outputs.size() == 5, "an unwired input reads zeros");
}

// BLOOM_STATISTICS_ARTIFACT_DIR: the frames an Image Statistics value drives, as PPM pictures (display
// clamped, four pixels to each), with the statistics they were driven by in the file name: a white
// bar as high as the maximum of the red 12, 24 and 48 pictures it measured, drawn on a mid grey.
void writeCapture() {
    const char* directory = std::getenv("BLOOM_STATISTICS_ARTIFACT_DIR");
    if (directory == nullptr || *directory == '\0')
        return;
    for (const double level : {12.0, 24.0, 48.0}) {
        st::StatisticsValues red;
        red.gradient = false;
        red.color = core::Color4d{level, 0.0, 0.0, 1.0};
        red.channel = doc::kStatisticsRed;
        const auto tap = st::statisticsPlan(red, 36000);
        const auto plan = st::drivenPlan(tap, 1, 36100 + static_cast<std::uint64_t>(level));
        runtime::CpuCompositionEvaluator evaluator;
        runtime::EvaluationRequest request{core::RationalTime::fromInteger(0), plan->output(),
                                           runtime::CompositionFormatResolution{}};
        request.pixelStorageByteLimit = 1U << 28U;
        const auto result = evaluator.evaluate(plan, request, {});
        if (result.frame() == nullptr)
            continue;
        const auto& image = result.frame()->processImage();
        std::ofstream file(std::string(directory) + "/image-statistics-maximum-" +
                               std::to_string(static_cast<int>(level)) + ".ppm",
                           std::ios::binary);
        constexpr int kScale = 4;
        file << "P6\n" << 64 * kScale << " " << 64 * kScale << "\n255\n";
        for (int y = 0; y < 64 * kScale; ++y)
            for (int x = 0; x < 64 * kScale; ++x) {
                const auto pixel = image.read(x / kScale, y / kScale);
                const float alpha = pixel ? pixel.value()->alpha() : 0.0F;
                const float value = 0.18F * (1.0F - alpha) + 1.0F * alpha;
                const auto byte = static_cast<char>(static_cast<unsigned char>(
                    std::clamp(std::pow(value, 1.0F / 2.2F), 0.0F, 1.0F) * 255.0F + 0.5F));
                file.put(byte).put(byte).put(byte);
            }
    }
}

} // namespace

int main() {
    try {
        testKnownAnswers();
        testCountingRules();
        testTileIndependence();
        testRefusalsAndIdentity();
        testCancellation();
        testCache();
        testValueGraph();
        writeCapture();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "image statistics tests passed\n";
    return 0;
}
