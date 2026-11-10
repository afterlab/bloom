// #519 Quantize on the CPU reference: Threshold (luma and per channel, hard and soft, inverted) and
// Posterize (linked and per-channel levels, alpha) against independent double-precision readings of
// the documented rules, exactly at the boundaries (the level, k / n) and one ulp either side, levels 2
// and 256, premultiplication, alpha, HDR and negative values, domain refusal, the pixel budget and
// row cancellation.

#include "color_grade_image_effect.hpp"
#include "color_tool_image_effect.hpp"
#include "color_tool_test_support.hpp"
#include "image_effect.hpp"

#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The reference is binary64 and the kernel binary32: comparing the two promotes and narrows on purpose.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-float-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#endif

namespace {

using namespace bloom;
namespace detail = runtime::detail;
using color_tool_test::QuantizeSettings;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using Pixel = std::array<float, 4>;

std::optional<detail::PreparedColorGrade> prepare(const runtime::ColorToolKernel& kernel) {
    std::vector<double> values;
    for (std::size_t index = 0; index < runtime::colorToolScalarCount(kernel.operation); ++index)
        values.push_back(std::get<double>(kernel.values[index].source));
    return detail::prepareColorToolGrade(kernel, values);
}

// Runs the kernel on a premultiplied pixel.
Pixel run(const QuantizeSettings& settings, Pixel pixel) {
    const auto grade = prepare(color_tool_test::quantize(1, settings));
    if (!grade)
        throw std::logic_error("the kernel prepares");
    expect(detail::colorToolPixel(grade->data, pixel), "a finite pixel always succeeds");
    return pixel;
}

// A pixel of straight color and alpha, premultiplied.
Pixel straight(const float r, const float g, const float b, const float a = 1.0F) {
    return {r * a, g * a, b * a, a};
}

float up(const float v) { return std::nextafter(v, std::numeric_limits<float>::infinity()); }
float down(const float v) { return std::nextafter(v, -std::numeric_limits<float>::infinity()); }

// The specification in binary64.
double stepReference(const float vf, const double level, const double soft) {
    const double v = static_cast<double>(vf);
    if (soft == 0.0)
        return v >= level ? 1.0 : 0.0;
    const double u = std::clamp((v - (level - 0.5 * soft)) / soft, 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}
double snapReference(const float xf, const double levels) {
    const double x = static_cast<double>(xf);
    const double steps = levels - 1.0;
    return std::floor(std::clamp(x, 0.0, 1.0) * steps + 0.5) / steps;
}
double lumaReference(const float r, const float g, const float b) {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) + 0.0722 * static_cast<double>(b);
}

bool near(const float a, const double b, const double tolerance = 2e-6) {
    return std::abs(static_cast<double>(a) - b) <= tolerance * std::max(1.0, std::abs(b));
}

void testHardThreshold() {
    QuantizeSettings luma;
    luma.level = 0.5;
    // The decisive boundary: a hard step is exactly 1 at and above the level and 0 below it.
    for (const double level : {0.25, 0.5, 1.0, 3.0}) {
        QuantizeSettings s;
        s.basis = document::kQuantizeBasisPerChannel;
        s.level = level;
        const auto l = static_cast<float>(level);
        const auto at = run(s, straight(l, up(l), down(l)));
        expect(at[0] == 1.0F && at[1] == 1.0F && at[2] == 0.0F && at[3] == 1.0F,
               "per channel: exactly at the level and one ulp above are white, one below is black (level " +
                   std::to_string(level) + ")");
    }
    const auto lumaBelow = run(luma, straight(0.45F, 0.45F, 0.45F, 0.5F));
    const auto lumaAbove = run(luma, straight(0.55F, 0.55F, 0.55F, 0.5F));
    expect(lumaBelow[0] == 0.0F && lumaBelow[3] == 0.5F, "below the level is black, alpha kept");
    expect(lumaAbove[0] == 0.5F && lumaAbove[1] == 0.5F && lumaAbove[2] == 0.5F && lumaAbove[3] == 0.5F,
           "a luma threshold gives the same value in every channel");
    // The four-parity-reference behaviours over a sweep, against the binary64 reading: luma, per channel.
    for (int i = 0; i <= 40; ++i) {
        const float r = 0.025F * static_cast<float>(i);
        const float g = 1.0F - r;
        const float b = 0.5F * r;
        QuantizeSettings lumaS;
        lumaS.level = 0.4;
        const auto got = run(lumaS, straight(r, g, b));
        const double want = stepReference(lumaReference(r, g, b), 0.4, 0.0);
        // The luma lies within an ulp of the level only by chance; skip those.
        if (std::abs(lumaReference(r, g, b) - 0.4) > 1e-6)
            expect(got[0] == want && got[1] == want && got[2] == want, "luma threshold sweep");
        QuantizeSettings chan;
        chan.basis = document::kQuantizeBasisPerChannel;
        chan.level = 0.4;
        const auto per = run(chan, straight(r, g, b));
        expect(per[0] == stepReference(r, 0.4, 0.0) && per[1] == stepReference(g, 0.4, 0.0) &&
                   per[2] == stepReference(b, 0.4, 0.0),
               "per-channel threshold sweep");
    }
}

void testSoftThreshold() {
    QuantizeSettings s;
    s.basis = document::kQuantizeBasisPerChannel;
    s.level = 0.5;
    s.softness = 0.2;
    // The ramp is centred on the level: exactly half at the level, 0 at and below the low edge, 1 at and
    // above the high edge, smooth between.
    expect(near(run(s, straight(0.5F, 0.5F, 0.5F))[0], 0.5, 1e-6), "a soft threshold is one half at the level");
    expect(run(s, straight(0.4F, 0.3F, 0.0F))[0] == 0.0F, "at the low edge and below is black");
    expect(run(s, straight(0.65F, 0.9F, 5.0F))[0] == 1.0F, "above the high edge is white");
    for (int i = 0; i <= 60; ++i) {
        const float v = 0.35F + 0.005F * static_cast<float>(i);
        const auto got = run(s, straight(v, v, v));
        expect(near(got[0], stepReference(v, 0.5, 0.2)),
               "soft ramp against the smoothstep at " + std::to_string(v));
    }
    // Monotonic.
    float previous = -1.0F;
    bool monotonic = true;
    for (int i = 0; i <= 100; ++i) {
        const float v = 0.3F + 0.004F * static_cast<float>(i);
        const auto got = run(s, straight(v, v, v))[0];
        monotonic = monotonic && got >= previous;
        previous = got;
    }
    expect(monotonic, "a soft threshold never decreases");
    // Soft on luma matches the smoothstep of the luma.
    QuantizeSettings luma;
    luma.level = 0.3;
    luma.softness = 0.5;
    const auto got = run(luma, straight(0.2F, 0.4F, 0.1F));
    expect(near(got[1], stepReference(lumaReference(0.2, 0.4, 0.1), 0.3, 0.5)), "soft luma threshold");
}

void testInvertAndAlpha() {
    QuantizeSettings s;
    s.basis = document::kQuantizeBasisPerChannel;
    s.invert = true;
    const auto got = run(s, straight(0.9F, 0.1F, 0.5F, 0.5F));
    expect(got[0] == 0.0F && near(got[1], 0.5) && near(got[2], 0.0) && got[3] == 0.5F,
           "an inverted threshold swaps white and black on the straight color, premultiplied by alpha");
    QuantizeSettings w;
    w.basis = document::kQuantizeBasisPerChannel;
    const auto white = run(w, straight(0.9F, 0.1F, 0.9F, 0.25F));
    expect(white[0] == 0.25F && white[1] == 0.0F && white[2] == 0.25F && white[3] == 0.25F,
           "white at alpha 0.25 is premultiplied 0.25 and alpha is kept");
    expect(run(w, {0.0F, 0.0F, 0.0F, 0.0F}) == Pixel{0.0F, 0.0F, 0.0F, 0.0F}, "transparent stays transparent");
    // HDR and negative values: a value far above the level is white, a negative one black.
    const auto hdr = run(w, straight(1000.0F, -5.0F, 0.5F));
    expect(hdr[0] == 1.0F && hdr[1] == 0.0F && hdr[2] == 1.0F, "HDR is white, negative is black");
    // The luma of a straight color uses the straight values, not the premultiplied ones.
    QuantizeSettings l;
    l.level = 0.5;
    const auto half = run(l, straight(0.8F, 0.8F, 0.8F, 0.3F));
    expect(half[0] == 0.3F && half[3] == 0.3F, "luma reads the straight color, not the premultiplied");
}

void testPosterize() {
    for (const double levels : {2.0, 3.0, 4.0, 8.0, 16.0, 256.0}) {
        QuantizeSettings s;
        s.mode = document::kQuantizePosterize;
        s.levels = levels;
        const auto steps = static_cast<float>(levels - 1.0);
        // Each channel at the middle of a step, a hair below and above the half-way boundary, 0 and 1.
        for (int k = 0; k < static_cast<int>(levels); k += std::max(1, static_cast<int>(levels) / 7)) {
            const float x = static_cast<float>(k) / steps;
            const auto got = run(s, straight(x, x, x));
            expect(near(got[0], snapReference(x, levels)) && near(got[0], x),
                   "an exact level is its own value (levels " + std::to_string(levels) + ")");
        }
    }
    // The half-way boundary between two levels, where the product x * steps is exact (steps a power of
    // two, so 0.5 / steps is representable and scaling it is exact): exactly half rounds up, one ulp
    // above rounds up, one ulp below rounds down.
    for (const double levels : {2.0, 3.0, 5.0, 9.0, 17.0, 129.0}) {
        QuantizeSettings s;
        s.mode = document::kQuantizePosterize;
        s.levels = levels;
        const auto steps = static_cast<float>(levels - 1.0);
        const float boundary = 0.5F / steps;
        const auto at = run(s, straight(boundary, up(boundary), down(boundary)));
        expect(near(at[0], 1.0 / steps, 1e-6) && near(at[1], 1.0 / steps, 1e-6) && at[2] == 0.0F,
               "exactly half and one ulp above round up, one ulp below rounds down (levels " +
                   std::to_string(levels) + ")");
    }
    // HDR above one is the top level, negative the bottom one.
    QuantizeSettings s;
    s.mode = document::kQuantizePosterize;
    s.levels = 4.0;
    const auto beyond = run(s, straight(7.0F, -3.0F, 0.34F));
    expect(beyond[0] == 1.0F && beyond[1] == 0.0F && near(beyond[2], 1.0 / 3.0), "beyond the range is the end levels");
    // Straight color and alpha: the premultiplied pixel is the quantized straight color times alpha.
    const auto partial = run(s, straight(0.4F, 0.4F, 0.4F, 0.5F));
    expect(near(partial[0], snapReference(0.4, 4.0) * 0.5) && partial[3] == 0.5F, "posterize keeps alpha");
    // Per-channel levels.
    QuantizeSettings per;
    per.mode = document::kQuantizePosterize;
    per.link = false;
    per.red = 2.0;
    per.green = 3.0;
    per.blue = 256.0;
    per.levels = 7.0; // unread while unlinked
    const auto got = run(per, straight(0.6F, 0.6F, 0.6F));
    expect(near(got[0], 1.0) && near(got[1], 0.5) && near(got[2], snapReference(0.6, 256.0)),
           "unlinked levels read the channels' own");
    // Linked levels read the master for all three.
    QuantizeSettings linked = per;
    linked.link = true;
    linked.levels = 3.0;
    const auto master = run(linked, straight(0.6F, 0.6F, 0.6F));
    expect(near(master[0], 0.5) && near(master[1], 0.5) && near(master[2], 0.5), "linked levels read the master");
    // Alpha posterize.
    QuantizeSettings alpha = linked;
    alpha.alpha = true;
    alpha.alphaLevels = 3.0;
    const auto a = run(alpha, straight(0.6F, 0.6F, 0.6F, 0.6F));
    expect(near(a[3], 0.5) && near(a[0], 0.5 * 0.5), "alpha snaps to its own levels and recolours");
    const auto gone = run(alpha, straight(0.6F, 0.6F, 0.6F, 0.2F));
    expect(gone == Pixel{0.0F, 0.0F, 0.0F, 0.0F}, "an alpha that snaps to zero is transparent black");
    // The identity of levels 256 against 8-bit values: k / 255 maps to itself for every k.
    QuantizeSettings full;
    full.mode = document::kQuantizePosterize;
    full.levels = 256.0;
    bool every = true;
    for (int k = 0; k < 256; ++k) {
        const float x = static_cast<float>(k) / 255.0F;
        every = every && std::abs(run(full, straight(x, x, x))[0] - x) <= 2e-7F;
    }
    expect(every, "levels 256 keeps every 8-bit value");
    // A sweep against the binary64 rule at levels 5, away from the boundaries.
    bool sweep = true;
    QuantizeSettings five;
    five.mode = document::kQuantizePosterize;
    five.levels = 5.0;
    for (int i = 0; i < 1000; ++i) {
        const float x = -0.1F + 1.2F * static_cast<float>(i) / 999.0F;
        const double scaled = std::clamp(static_cast<double>(x), 0.0, 1.0) * 4.0;
        if (std::abs(scaled - std::floor(scaled) - 0.5) < 1e-4)
            continue;
        sweep = sweep && near(run(five, straight(x, x, x))[1], snapReference(x, 5.0));
    }
    expect(sweep, "a posterize sweep equals the binary64 rule");
}

void testDomainAndIdentity() {
    const auto prepares = [](const QuantizeSettings& s) { return prepare(color_tool_test::quantize(1, s)).has_value(); };
    expect(prepares({}), "the defaults prepare");
    QuantizeSettings bad;
    bad.level = -0.1;
    expect(!prepares(bad), "a negative level is refused");
    bad = {};
    bad.softness = 17.0;
    expect(!prepares(bad), "a softness beyond the domain is refused");
    bad = {};
    bad.mode = document::kQuantizePosterize;
    bad.levels = 1.5;
    expect(!prepares(bad), "fewer than two levels is refused");
    bad.levels = 257.0;
    expect(!prepares(bad), "more than 256 levels is refused");
    bad = {};
    bad.mode = document::kQuantizePosterize;
    bad.link = false;
    bad.green = 1.0;
    expect(!prepares(bad), "an unlinked channel with fewer than two levels is refused");
    bad.link = true;
    expect(prepares(bad), "a channel level no mode reads is not checked");
    bad = {};
    bad.mode = 2;
    expect(!prepares(bad), "an unknown mode is refused");
    // A threshold that reads no level checks none of the posterize values and the reverse.
    QuantizeSettings threshold;
    threshold.levels = 0.0;
    expect(prepares(threshold), "a threshold does not read the levels");
    auto kernel = color_tool_test::quantize(1, {});
    const auto a = prepare(kernel);
    kernel.values[0] = color_tool_test::scalar(61, 0.6);
    const auto b = prepare(kernel);
    expect(a && b && detail::colorGradeIdentity(*a) != detail::colorGradeIdentity(*b),
           "the level changes the table's identity");
}

bloom::render::Rgba32fImage image(const std::uint32_t width, const std::uint32_t height, const Pixel& fill) {
    const auto window = render::ImageWindow::create(0, 0, width, height);
    auto descriptor = render::Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (std::int64_t y = 0; y < static_cast<std::int64_t>(height); ++y)
        for (std::int64_t x = 0; x < static_cast<std::int64_t>(width); ++x) {
            const auto stored = render::Rgba32f::fromPremultiplied(fill[0], fill[1], fill[2], fill[3]);
            if (!stored || builder.value()->write(x, y, *stored.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

void testImageBudgetAndCancellation() {
    QuantizeSettings settings;
    settings.mode = document::kQuantizePosterize;
    settings.levels = 4.0;
    const auto grade = prepare(color_tool_test::quantize(1, settings));
    const auto input = image(8, 4, {0.9F, 0.4F, 0.1F, 1.0F});
    const auto result = detail::applyColorGrade(*grade, input, 1U << 22U, {},
                                                runtime::OperationIndex::fromRaw(0), {});
    expect(result.image != nullptr, "the pass applies to an image");
    if (result.image) {
        Pixel value{};
        if (const auto pixel = result.image->read(3, 2))
            std::ranges::copy(pixel.value()->components(), value.begin());
        expect(near(value[0], 1.0) && near(value[1], 1.0 / 3.0) && near(value[2], 0.0) && value[3] == 1.0F,
               "every pixel of the image is posterized");
        expect(result.image->descriptor()->dataWindow() == input.descriptor()->dataWindow(),
               "the data window is kept");
    }
    const auto refused = detail::applyColorGrade(*grade, input, 4, {}, runtime::OperationIndex::fromRaw(0), {});
    expect(!refused.image && refused.diagnostic.has_value(), "a refused budget is an explicit error");

    const auto large = image(64, 64, {0.5F, 0.5F, 0.5F, 1.0F});
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(519)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Quantize cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto cancelledResult =
                detail::applyColorGrade(*grade, large, 1U << 26U, context.cancellation(),
                                        runtime::OperationIndex::fromRaw(0), {});
            cancelled = cancelledResult.cancelled && !cancelledResult.image;
            return runtime::TaskResult<void>::cancelled();
        });
    expect(submission.accepted(), "the cancellation task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> taken;
    while (!taken && std::chrono::steady_clock::now() < deadline) {
        taken = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(taken && taken->state() == runtime::TaskState::Cancelled && cancelled,
           "a cancelled pass publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

} // namespace

int main() {
    try {
        testHardThreshold();
        testSoftThreshold();
        testInvertAndAlpha();
        testPosterize();
        testDomainAndIdentity();
        testImageBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
