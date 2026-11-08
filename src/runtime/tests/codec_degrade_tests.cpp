// #563 Codec Degrade on the CPU reference: the colour round trip; a keyframe's blocks and quality,
// on the frame's own pixel grid; 4:2:0 chroma; a predicted frame's persistence, block motion and the
// quantiser's coarsening with motion; the pass a step is by its place in the group of pictures;
// absent images, domains, the one fixed program, the pixel budget, cancellation and band
// independence; and the look-back chain through the evaluator -- that a frame renders the same on its
// own and after the frames before it, that a keyframe forgets everything before it, and that a
// parameter changes the frame it is drawn on.

#include "codec_degrade_image_effect.hpp"
#include "codec_degrade_test_support.hpp"
#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
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
namespace detail = runtime::detail;
using codec_test::CodecValues;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using Pixel = std::array<float, 4>;

// An image over a data window whose pixel (x, y) is `pixel(x, y)`, given as straight colour and
// alpha and stored premultiplied.
render::Rgba32fImage image(const std::int64_t originX, const std::int64_t originY,
                           const std::uint32_t width, const std::uint32_t height,
                           const std::function<Pixel(std::int64_t, std::int64_t)>& pixel) {
    const auto data = render::ImageWindow::create(originX, originY, width, height);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *data.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (auto y = originY; y < originY + static_cast<std::int64_t>(height); ++y)
        for (auto x = originX; x < originX + static_cast<std::int64_t>(width); ++x) {
            const auto p = pixel(x, y);
            const auto stored = render::Rgba32f::fromPremultiplied(p[0] * p[3], p[1] * p[3],
                                                                   p[2] * p[3], p[3]);
            if (!stored || builder.value()->write(x, y, *stored.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

Pixel at(const render::Rgba32fImage& picture, const std::int64_t x, const std::int64_t y) {
    Pixel value{};
    if (const auto pixel = picture.read(x, y))
        std::ranges::copy(pixel.value()->components(), value.begin());
    return value;
}

runtime::CodecDegradeKernel kernelOf(const runtime::CodecDegradeRole role, const std::int64_t step = 0,
                                     const std::int64_t block = 8, const std::int64_t gop = 4,
                                     const bool chroma = true) {
    runtime::CodecDegradeKernel kernel;
    kernel.role = role;
    kernel.step = step;
    kernel.blockPixels = block;
    kernel.gopFrames = gop;
    kernel.chromaBleed = chroma;
    return kernel;
}

std::optional<detail::PreparedColorGrade> prepareStep(const std::int64_t step, const double quality,
                                                      const double persistence,
                                                      const double frame, const std::int64_t block = 8,
                                                      const std::int64_t gop = 4,
                                                      const bool chroma = true) {
    return detail::prepareColorGrade(kernelOf(runtime::CodecDegradeRole::Step, step, block, gop, chroma),
                                     std::array<double, 3>{quality, persistence, frame});
}
std::optional<detail::PreparedColorGrade> prepareRole(const runtime::CodecDegradeRole role,
                                                      const std::int64_t block = 8,
                                                      const bool chroma = true) {
    return detail::prepareColorGrade(kernelOf(role, 0, block, 4, chroma), std::span<const double>{});
}

std::shared_ptr<const render::Rgba32fImage>
apply(const detail::PreparedColorGrade& grade, const render::Rgba32fImage* input,
      const render::Rgba32fImage* colour = nullptr, const render::Rgba32fImage* flow = nullptr,
      const double scale = 1.0, runtime::CpuRowBandExecutor* bands = nullptr) {
    const auto result = detail::applyCodecDegrade(grade, input, colour, flow, scale, 1U << 26U, {},
                                                  runtime::OperationIndex::fromRaw(0), {}, bands);
    if (!result.image)
        throw std::logic_error("the pass applies");
    return result.image;
}

// Encode -> keyframe -> Finish.
std::shared_ptr<const render::Rgba32fImage> keyframe(const render::Rgba32fImage& source,
                                                     const double quality,
                                                     const std::int64_t block = 8,
                                                     const bool chroma = true,
                                                     const double scale = 1.0,
                                                     runtime::CpuRowBandExecutor* bands = nullptr) {
    const auto encode = prepareRole(runtime::CodecDegradeRole::Encode, block, chroma);
    const auto step = prepareStep(0, quality, 0.0, 0.0, block, 4, chroma);
    const auto finish = prepareRole(runtime::CodecDegradeRole::Finish, block, chroma);
    if (!encode || !step || !finish)
        throw std::logic_error("the passes prepare");
    const auto encoded = apply(*encode, &source, nullptr, nullptr, scale, bands);
    const auto coded = apply(*step, encoded.get(), nullptr, nullptr, scale, bands);
    return apply(*finish, coded.get(), nullptr, nullptr, scale, bands);
}

double maxDifference(const render::Rgba32fImage& a, const render::Rgba32fImage& b,
                     const std::int64_t x0, const std::int64_t y0, const std::int64_t x1,
                     const std::int64_t y1) {
    double worst = 0.0;
    for (auto y = y0; y < y1; ++y)
        for (auto x = x0; x < x1; ++x) {
            const auto p = at(a, x, y);
            const auto q = at(b, x, y);
            for (std::size_t c = 0; c < 4; ++c)
                worst = std::max(worst, std::abs(static_cast<double>(p[c]) - static_cast<double>(q[c])));
        }
    return worst;
}
double meanDifference(const render::Rgba32fImage& a, const render::Rgba32fImage& b,
                      const std::int64_t x0, const std::int64_t y0, const std::int64_t x1,
                      const std::int64_t y1) {
    double sum = 0.0;
    for (auto y = y0; y < y1; ++y)
        for (auto x = x0; x < x1; ++x) {
            const auto p = at(a, x, y);
            const auto q = at(b, x, y);
            for (std::size_t c = 0; c < 3; ++c)
                sum += std::abs(static_cast<double>(p[c]) - static_cast<double>(q[c]));
        }
    return sum / (3.0 * static_cast<double>((x1 - x0) * (y1 - y0)));
}
bool identical(const render::Rgba32fImage& a, const render::Rgba32fImage& b) {
    const auto wa = a.descriptor()->dataWindow();
    if (wa != b.descriptor()->dataWindow())
        return false;
    return maxDifference(a, b, wa.originX(), wa.originY(), wa.maxXExclusive(), wa.maxYExclusive()) == 0.0;
}

// A smooth picture with a few edges, as the sort of thing a camera sees.
Pixel scene(const std::int64_t x, const std::int64_t y) {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    Pixel p{0.2F + 0.012F * fx, 0.3F + 0.01F * fy, 0.5F - 0.006F * fx + 0.004F * fy, 1.0F};
    if (x % 13 < 4 && y % 11 < 6) {
        p[0] = 0.9F;
        p[1] = 0.15F;
    }
    return p;
}

void testColourRoundTrip() {
    const auto source = image(0, 0, 16, 16, [](const std::int64_t x, const std::int64_t y) {
        Pixel p{static_cast<float>(x) / 16.0F, static_cast<float>(y) / 16.0F, 0.5F, 0.75F};
        if (x == 3 && y == 3)
            p = {2.0F, 0.5F, 0.1F, 1.0F};
        if (x == 5 && y == 5)
            p = {0.0F, 0.0F, 0.0F, 0.0F};
        return p;
    });
    const auto encode = prepareRole(runtime::CodecDegradeRole::Encode);
    const auto finish = prepareRole(runtime::CodecDegradeRole::Finish);
    if (!encode || !finish)
        throw std::logic_error("the colour passes prepare");
    const auto encoded = apply(*encode, &source);
    const auto back = apply(*finish, encoded.get());
    expect(maxDifference(source, *back, 0, 0, 3, 16) < 5e-6, "the colour passes round trip");
    expect(at(*back, 3, 3)[0] == 1.0F && std::abs(at(*back, 3, 3)[1] - 0.5F) < 5e-6F,
           "a colour above 1 is clamped, as a codec's range is");
    expect(at(*back, 5, 5) == Pixel{0.0F, 0.0F, 0.0F, 0.0F}, "a transparent pixel stays transparent");
    expect(at(*encoded, 9, 9)[3] == 0.75F, "the encoded picture keeps the alpha");
    const auto e = at(*encoded, 9, 9);
    expect(e[0] > 9.0F / 16.0F && e[0] <= 1.0F, "the curve lifts the darks (display-referred)");
}

void testKeyframe() {
    const auto source = image(0, 0, 32, 24, scene);
    const auto fine = keyframe(source, 100.0, 8, false);
    const auto coarse = keyframe(source, 0.0);
    const auto middle = keyframe(source, 40.0);
    expect(maxDifference(source, *fine, 0, 0, 32, 24) < 0.02, "quality 100 is nearly lossless");
    const double coarseError = meanDifference(source, *coarse, 0, 0, 32, 24);
    const double middleError = meanDifference(source, *middle, 0, 0, 32, 24);
    const double fineError = meanDifference(source, *fine, 0, 0, 32, 24);
    expect(coarseError > middleError && middleError > fineError && coarseError > 0.03,
           "a lower quality loses more");
    expect(maxDifference(source, *coarse, 0, 0, 32, 24) < 1.0 &&
               at(*coarse, 4, 4)[3] == 1.0F,
           "alpha is kept");
    // At the coarsest quality an 8 pixel block is nearly flat: far less spread inside a block than
    // the picture's own.
    const auto spread = [](const render::Rgba32fImage& picture, const std::int64_t bx,
                           const std::int64_t by, const std::int64_t size) {
        float low = 1e9F;
        float high = -1e9F;
        for (auto y = by; y < by + size; ++y)
            for (auto x = bx; x < bx + size; ++x) {
                low = std::min(low, at(picture, x, y)[1]);
                high = std::max(high, at(picture, x, y)[1]);
            }
        return high - low;
    };
    expect(spread(*coarse, 8, 8, 8) < 0.5F * spread(source, 8, 8, 8), "a coarse block is flat");
    expect(maxDifference(*coarse, *keyframe(source, 0.0, 8, true, 1.0, nullptr), 0, 0, 32, 24) == 0.0,
           "a keyframe is deterministic");

    // 16 px blocks are a different picture from 8 px blocks.
    const auto big = keyframe(source, 20.0, 16);
    const auto small = keyframe(source, 20.0, 8);
    expect(maxDifference(*big, *small, 0, 0, 32, 24) > 0.01, "the block size changes the picture");

    // The block grid is the frame's own pixel grid: the same picture a whole number of blocks over
    // codes the same, and a shift that is not a whole number of blocks does not.
    const auto shifted16 = image(16, 16, 32, 32, scene);
    const auto shifted3 = image(3, 5, 48, 40, scene);
    const auto base = keyframe(image(0, 0, 64, 48, scene), 20.0, 8);
    expect(maxDifference(*base, *keyframe(shifted16, 20.0, 8), 16, 16, 48, 40) == 0.0,
           "a picture is coded on the frame's pixel grid, not its window's");
    expect(maxDifference(*base, *keyframe(shifted3, 20.0, 8), 16, 16, 32, 32) == 0.0,
           "a window that is not on the block grid does not move the grid");
}

void testChroma() {
    // A red field and a blue one meet at x = 17, in the middle of a pair of chroma samples.
    const auto source = image(0, 0, 32, 24, [](const std::int64_t x, const std::int64_t) -> Pixel {
        return x < 17 ? Pixel{0.8F, 0.1F, 0.1F, 1.0F} : Pixel{0.1F, 0.1F, 0.8F, 1.0F};
    });
    const auto bled = keyframe(source, 100.0, 8, true);
    const auto full = keyframe(source, 100.0, 8, false);
    expect(maxDifference(*full, source, 0, 0, 32, 24) < 0.02, "full resolution chroma keeps the edge");
    expect(maxDifference(*bled, source, 14, 0, 15, 24) < 0.02 &&
               maxDifference(*bled, source, 20, 0, 21, 24) < 0.02,
           "chroma away from the edge is kept");
    const auto a = at(*bled, 16, 10);
    const auto b = at(source, 16, 10);
    expect(std::abs(a[0] - b[0]) > 0.08F && std::abs(a[2] - b[2]) > 0.08F,
           "4:2:0 chroma bleeds across the edge");
}

void testPredicted() {
    const auto previous = image(0, 0, 32, 24, [](const std::int64_t x, const std::int64_t y) -> Pixel {
        return {static_cast<float>((x * 7 + y * 3) % 32) / 40.0F,
                static_cast<float>((x * 5 + y * 11) % 32) / 40.0F,
                static_cast<float>((x * 3 + y * 13) % 32) / 40.0F, 1.0F};
    });
    const auto colour = image(0, 0, 32, 24, [](const std::int64_t x, const std::int64_t y) -> Pixel {
        return {0.9F - static_cast<float>(x) / 60.0F, static_cast<float>(y) / 30.0F, 0.4F, 0.5F};
    });
    // A picture the codec itself could have left: smooth in colour.
    const auto smooth = image(0, 0, 32, 24, [](const std::int64_t x, const std::int64_t y) -> Pixel {
        return {0.2F + 0.01F * static_cast<float>(x), 0.3F + 0.01F * static_cast<float>(y), 0.5F, 1.0F};
    });
    const auto flowing = [](const float u, const float v) {
        return image(0, 0, 32, 24, [u, v](const std::int64_t, const std::int64_t) -> Pixel {
            // A flow image is [u, v, 0, 1] straight (premultiplied by 1).
            return {u, v, 0.0F, 1.0F};
        });
    };
    const auto flow = flowing(2.0F, 1.0F);

    // Persistence 100%: the previous picture, moved by the block's flow; the colour is not read.
    {
        const auto grade = prepareStep(1, 40.0, 1.0, 1.0);
        const auto out = apply(*grade, &previous, &colour, &flow);
        double worst = 0.0;
        for (std::int64_t y = 0; y < 24; ++y)
            for (std::int64_t x = 0; x < 32; ++x) {
                const auto want = at(previous, std::min<std::int64_t>(x + 2, 31),
                                     std::min<std::int64_t>(y + 1, 23));
                const auto got = at(*out, x, y);
                for (std::size_t c = 0; c < 3; ++c)
                    worst = std::max(worst, std::abs(static_cast<double>(got[c] - want[c])));
            }
        expect(worst < 3e-6, "full persistence moves the previous picture by the flow");
        expect(at(*out, 10, 10)[3] == 0.5F, "the predicted picture follows the colour's alpha");
    }
    // Persistence 0% at the best quality: the colour, whatever came before.
    {
        const auto grade = prepareStep(1, 100.0, 0.0, 1.0);
        const auto out = apply(*grade, &smooth, &colour, &flow);
        double worst = 0.0;
        for (std::int64_t y = 0; y < 24; ++y)
            for (std::int64_t x = 0; x < 32; ++x) {
                const auto got = at(*out, x, y);
                const auto want = at(colour, x, y);
                // The pass reads the stored numbers of the picture it follows: the Encoded colour.
                for (std::size_t c = 0; c < 3; ++c)
                    worst = std::max(worst, std::abs(static_cast<double>(got[c] - want[c])));
            }
        expect(worst < 0.05, "no persistence at the best quality refreshes the whole picture");
    }
    // Half persistence is halfway between.
    {
        const auto half = apply(*prepareStep(1, 100.0, 0.5, 1.0), &smooth, &colour, &flow);
        const auto none = apply(*prepareStep(1, 100.0, 0.0, 1.0), &smooth, &colour, &flow);
        const auto all = apply(*prepareStep(1, 100.0, 1.0, 1.0), &smooth, &colour, &flow);
        const auto h = at(*half, 12, 9);
        const auto n = at(*none, 12, 9);
        const auto a = at(*all, 12, 9);
        bool between = true;
        for (std::size_t c = 0; c < 3; ++c)
            between = between && std::abs(h[c] - 0.5F * (n[c] + a[c])) < 0.03F;
        expect(between, "half persistence keeps half of the previous picture");
    }
    // Motion makes the quantiser coarser: the same residual against a flat previous picture codes
    // worse with a flow of a block's width than with none.
    {
        const auto flat = image(0, 0, 32, 24, [](const std::int64_t, const std::int64_t) -> Pixel {
            return {0.5F, 0.5F, 0.5F, 1.0F};
        });
        const auto opaque = image(0, 0, 32, 24, scene);
        const auto zero = flowing(0.0F, 0.0F);
        const auto fast = flowing(8.0F, 0.0F);
        const auto encode = prepareRole(runtime::CodecDegradeRole::Encode);
        const auto encoded = apply(*encode, &opaque);
        const auto calm = apply(*prepareStep(1, 60.0, 0.0, 1.0, 8, 4, false), &flat, encoded.get(), &zero);
        const auto busy = apply(*prepareStep(1, 60.0, 0.0, 1.0, 8, 4, false), &flat, encoded.get(), &fast);
        expect(meanDifference(*busy, *encoded, 0, 0, 32, 24) >
                   1.2 * meanDifference(*calm, *encoded, 0, 0, 32, 24),
               "blocks in motion are coded more coarsely");
    }
    // Without a previous picture a predicted frame is a keyframe; without its colour it is none.
    {
        const auto opaque = image(0, 0, 32, 24, scene);
        const auto fresh = apply(*prepareStep(1, 40.0, 0.0, 1.0), nullptr, &opaque, nullptr);
        const auto intra = apply(*prepareStep(0, 40.0, 0.0, 0.0), &opaque);
        expect(identical(*fresh, *intra), "a predicted frame with no previous picture is a keyframe");
        const auto none = detail::applyCodecDegrade(*prepareStep(1, 40.0, 0.5, 1.0), &previous, nullptr,
                                                    &flow, 1.0, 1U << 26U, {},
                                                    runtime::OperationIndex::fromRaw(0), {}, nullptr);
        expect(none.empty && !none.image && !none.diagnostic, "a frame with no colour publishes no image");
    }
}

void testWindows() {
    // A colour window that is not the previous picture's: the output follows the colour, and the
    // previous picture and the flow are read where they are, clamped at their edges.
    const auto previous = image(4, 2, 24, 16, [](const std::int64_t x, const std::int64_t y) -> Pixel {
        return {static_cast<float>(x % 8) / 8.0F, static_cast<float>(y % 8) / 8.0F, 0.3F, 1.0F};
    });
    const auto colour = image(-3, 1, 20, 20, scene);
    const auto flow = image(0, 0, 16, 16, [](const std::int64_t, const std::int64_t) -> Pixel {
        return {1.0F, 0.0F, 0.0F, 1.0F};
    });
    const auto out = apply(*prepareStep(2, 30.0, 0.5, 2.0), &previous, &colour, &flow);
    expect(out->descriptor()->dataWindow() == colour.descriptor()->dataWindow(),
           "the predicted picture has the colour's window");
    bool finite = true;
    for (auto y = 1; y < 21; ++y)
        for (auto x = -3; x < 17; ++x)
            for (const auto c : at(*out, x, y))
                finite = finite && std::isfinite(c);
    expect(finite, "mismatched windows read finite values");
    // The proxy scale: blocks and chroma shrink with the evaluation resolution.
    const auto half = apply(*prepareStep(0, 30.0, 0.0, 0.0), &colour, nullptr, nullptr, 0.5);
    expect(half->descriptor()->dataWindow() == colour.descriptor()->dataWindow(),
           "a proxy keyframe keeps its window");
    const auto g = prepareStep(0, 30.0, 0.0, 0.0);
    expect(detail::codecBlock(*g, 0.5) == 4 && detail::codecBlock(*g, 1.0) == 8 &&
               detail::codecBlock(*g, 0.05) == 2 && detail::codecChromaStep(*g, 1.0) == 2 &&
               detail::codecChromaStep(*g, 0.5) == 1,
           "the block and the chroma step follow the resolution scale");
}

void testStages() {
    const auto stageOf = [](const std::optional<detail::PreparedColorGrade>& g) {
        return g ? detail::codecDegradeStage(*g) : detail::CodecStage::Encode;
    };
    expect(stageOf(prepareStep(0, 40.0, 0.5, 7.0)) == detail::CodecStage::Intra &&
               !prepareStep(0, 40.0, 0.5, 7.0)->identity,
           "step 0 is always the keyframe");
    // A group of four: frames 4..7 are positions 0..3.
    for (std::int64_t frame = 4; frame < 8; ++frame)
        for (std::int64_t step = 1; step < 4; ++step) {
            const auto grade = prepareStep(step, 40.0, 0.5, static_cast<double>(frame));
            expect(grade && grade->identity == (step > frame - 4),
                   "a step is coded on or before the frame's place and the identity after it");
        }
    // Frames before the first keyframe count back from it.
    expect(prepareStep(3, 40.0, 0.5, -1.0)->identity == false &&
               prepareStep(3, 40.0, 0.5, -3.0)->identity == true,
           "negative frames have a place in the group of pictures too");
    expect(prepareStep(0, 40.0, 0.5, 9.0, 8, 1) && !prepareStep(1, 40.0, 0.5, 9.0, 8, 1),
           "a group of one is a keyframe and has no further step");
    expect(!prepareStep(0, 101.0, 0.5, 0.0) && !prepareStep(0, -1.0, 0.5, 0.0) &&
               !prepareStep(0, 40.0, 1.5, 0.0) && !prepareStep(0, 40.0, -0.1, 0.0) &&
               !prepareStep(0, std::nan(""), 0.5, 0.0) && !prepareStep(0, 40.0, 0.5, std::nan("")),
           "values outside their domains are refused");
    expect(!prepareStep(0, 40.0, 0.5, 0.0, 12) && !prepareStep(4, 40.0, 0.5, 0.0, 8, 4) &&
               !prepareStep(0, 40.0, 0.5, 0.0, 8, 31) && !prepareStep(0, 40.0, 0.5, 0.0, 8, 0),
           "a block, a group or a step outside the node's domain is refused");
    expect(!detail::prepareColorGrade(kernelOf(runtime::CodecDegradeRole::Step),
                                      std::array<double, 2>{40.0, 0.5}),
           "a step without its frame is refused");
    expect(!detail::prepareColorGrade(kernelOf(runtime::CodecDegradeRole::Encode),
                                      std::array<double, 1>{1.0}),
           "an Encode takes no values");
    // One Encode table whatever the step reaches it: the same cache identity.
    expect(detail::colorGradeIdentity(*prepareRole(runtime::CodecDegradeRole::Encode)) ==
               detail::colorGradeIdentity(*detail::prepareColorGrade(
                   kernelOf(runtime::CodecDegradeRole::Encode, 3, 8, 12), std::span<const double>{})),
           "an Encode names nothing of its step");
}

void testProgram() {
    using detail::buildColorGradeProgram;
    using detail::ColorGradeKind;
    const auto grade = prepareStep(1, 40.0, 0.5, 1.0);
    detail::CodecPlacement placement;
    placement.colour = {0, 0, 32, 24, 0, 0};
    placement.previous = {0, 0, 32, 24, 32, 0};
    placement.flow = {0, 0, 32, 24, 64, 0};
    const auto data = detail::codecDegradeProgramData(*grade, placement, 1.0);
    expect(data && data->size() == detail::kCodecDegradeProgramFloats &&
               detail::validCodecDegradeProgramData(*data),
           "the program data is the fixed table");
    if (!data)
        return;
    const auto program = buildColorGradeProgram(ColorGradeKind::CodecDegrade, *data);
    expect(program.succeeded(), "the codec program builds");
    // One fixed program: another quality, block and chroma are data.
    const auto other = prepareStep(1, 90.0, 0.1, 1.0, 16, 4, false);
    const auto otherData = detail::codecDegradeProgramData(*other, placement, 1.0);
    const auto otherProgram = otherData ? buildColorGradeProgram(ColorGradeKind::CodecDegrade, *otherData)
                                        : render::OcioGpuProgramResult::failure(
                                              render::OcioGpuProgramError::InvalidRequest);
    expect(program.succeeded() && otherProgram.succeeded() &&
               program.program()->shaderText == otherProgram.program()->shaderText &&
               program.program()->uniformBufferData != otherProgram.program()->uniformBufferData,
           "every setting is data of one program");
    auto broken = *data;
    broken.pop_back();
    expect(!detail::validCodecDegradeProgramData(broken) &&
               !buildColorGradeProgram(ColorGradeKind::CodecDegrade, broken).succeeded(),
           "a table of the wrong size is refused");
    broken = *data;
    broken[1] = 17.0F;
    expect(!detail::validCodecDegradeProgramData(broken), "a block that is not 2..16 is refused");
    broken = *data;
    broken[2] = 3.0F;
    expect(!detail::validCodecDegradeProgramData(broken), "a chroma step that is not 1 or 2 is refused");
    broken = *data;
    broken[40] = std::numeric_limits<float>::infinity();
    expect(!detail::validCodecDegradeProgramData(broken), "a non-finite table is refused");
    detail::CodecPlacement empty;
    expect(!detail::codecDegradeProgramData(*grade, empty, 1.0), "no colour image has no table");
    // The basis is orthonormal and the weights run 1 to 8.
    const auto& d = *data;
    bool orthonormal = true;
    for (int u = 0; u < 8; ++u)
        for (int v = 0; v < 8; ++v) {
            double dot = 0.0;
            for (int n = 0; n < 8; ++n)
                dot += static_cast<double>(d[static_cast<std::size_t>(36 + u * 8 + n)]) *
                       static_cast<double>(d[static_cast<std::size_t>(36 + v * 8 + n)]);
            orthonormal = orthonormal && std::abs(dot - (u == v ? 1.0 : 0.0)) < 1e-6;
        }
    expect(orthonormal, "the DCT basis is orthonormal");
    expect(d[292] == 1.0F && std::abs(d[292 + 14] - 8.0F) < 1e-6F, "the quantiser weights run from 1 to 8");
}

void testBudgetCancellationAndBands() {
    const auto source = image(0, 0, 32, 24, scene);
    const auto grade = prepareStep(0, 40.0, 0.0, 0.0);
    const auto refused = detail::applyCodecDegrade(*grade, &source, nullptr, nullptr, 1.0, 16U, {},
                                                   runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(refused.diagnostic &&
               refused.diagnostic->code == runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "the pixel budget refuses the output");
    // The bands of any width code the same picture.
    runtime::CpuRowBandExecutor narrow(1);
    runtime::CpuRowBandExecutor wide(6);
    expect(identical(*keyframe(source, 25.0, 8, true, 1.0, nullptr),
                     *keyframe(source, 25.0, 8, true, 1.0, &narrow)) &&
               identical(*keyframe(source, 25.0, 8, true, 1.0, nullptr),
                         *keyframe(source, 25.0, 8, true, 1.0, &wide)),
           "the row bands do not change the picture");
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(563)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Codec Degrade cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result = detail::applyCodecDegrade(*grade, &source, nullptr, nullptr, 1.0,
                                                          1U << 26U, context.cancellation(),
                                                          runtime::OperationIndex::fromRaw(0), {},
                                                          nullptr);
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
    expect(scheduler.isQuiescent(), "the cancelled pass shuts down safely");
}

// --- The look-back chain through the evaluator -------------------------------------------------

std::vector<render::Rgba32f> frameOf(const runtime::CpuCompositionEvaluator& evaluator,
                                     const codec_test::CodecPlan& which, const std::int64_t frame,
                                     runtime::OperationCache* cache = nullptr) {
    runtime::EvaluationRequest request{*core::RationalTime::create(frame, 24), which.plan->output(),
                                       runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = 1U << 28U;
    static_cast<void>(cache);
    const auto result = evaluator.evaluate(which.plan, request, {});
    if (result.frame() == nullptr)
        throw std::logic_error("the codec plan evaluates frame " + std::to_string(frame));
    const auto view = result.frame()->processImage().pixels();
    return std::vector<render::Rgba32f>(view.begin(), view.end());
}

double mean(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t c = 0; c < 3; ++c)
            sum += std::abs(static_cast<double>(a[i].components()[c]) -
                            static_cast<double>(b[i].components()[c]));
    return sum / (3.0 * static_cast<double>(a.size()));
}

void testChain() {
    const runtime::CpuCompositionEvaluator evaluator;
    CodecValues values;
    values.gopFrames = 4;
    const auto grouped = codec_test::codecPlan(values);
    values.gopFrames = 1;
    const auto intra = codec_test::codecPlan(values, 56300);
    for (const auto& which : {grouped, intra})
        for (std::size_t i = 0; i < which.plan->operations().size(); ++i)
            static_cast<void>(which.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(i)));

    // A keyframe forgets what came before: frame 4 of a group of four is the group of one's frame 4.
    expect(frameOf(evaluator, grouped, 4) == frameOf(evaluator, intra, 4),
           "a keyframe is coded from its own frame alone");
    expect(frameOf(evaluator, grouped, 8) == frameOf(evaluator, intra, 8),
           "and so is every group's first");
    // The frames after it are predicted from it and differ from a keyframe's.
    expect(frameOf(evaluator, grouped, 6) != frameOf(evaluator, intra, 6),
           "a predicted frame is not a keyframe");
    // A frame renders on its own: the same after every frame before it as with none of them.
    {
        const runtime::CpuCompositionEvaluator fresh;
        const auto alone = frameOf(fresh, grouped, 6);
        const runtime::CpuCompositionEvaluator walked;
        for (std::int64_t frame = 0; frame <= 6; ++frame)
            static_cast<void>(frameOf(walked, grouped, frame));
        expect(frameOf(walked, grouped, 6) == alone, "a frame is the same scrubbed to as played to");
        const runtime::CpuCompositionEvaluator backwards;
        for (std::int64_t frame = 11; frame >= 5; --frame)
            static_cast<void>(frameOf(backwards, grouped, frame));
        expect(frameOf(backwards, grouped, 6) == alone, "and the same reached backwards");
    }
    // A predicted frame depends on the one before it in its group: moving the previous frame's
    // picture (here: asking a plan whose picture stands still) changes it.
    {
        CodecValues still = values;
        still.gopFrames = 4;
        still.still = true;
        const auto steady = codec_test::codecPlan(still, 56500);
        const auto a = frameOf(evaluator, steady, 5);
        const auto b = frameOf(evaluator, steady, 6);
        // Nothing moves, so no block moves; each frame is the keyframe refined by the same residual.
        expect(mean(a, b) < 0.02, "a still picture settles");
    }
    // Persistence 100% freezes the group; 0% refreshes it.
    {
        CodecValues freeze;
        freeze.gopFrames = 6;
        freeze.persistence = 1.0;
        freeze.still = true;
        const auto frozen = codec_test::codecPlan(freeze, 56600);
        // Nothing moves, so no block does; the picture is copied from frame to frame, through the
        // colour conversion's own rounding.
        expect(mean(frameOf(evaluator, frozen, 6), frameOf(evaluator, frozen, 9)) < 1e-5 &&
                   mean(frameOf(evaluator, frozen, 6), frameOf(evaluator, frozen, 11)) < 1e-5,
               "full persistence of a still picture holds the keyframe until the next");
        CodecValues refresh = freeze;
        refresh.persistence = 0.0;
        refresh.quality = 100.0;
        const auto fresh = codec_test::codecPlan(refresh, 56700);
        const auto one = codec_test::codecPlan(CodecValues{100.0, 0.0, 8, 1, true, true}, 56800);
        expect(mean(frameOf(evaluator, fresh, 9), frameOf(evaluator, one, 9)) < 0.03,
               "no persistence at the best quality is the picture of the frame");
    }
    // Blocks follow the picture's motion: full persistence moves the keyframe's picture along with
    // the flow, so a moving frame is nearer its own keyframe-coded picture than the unmoved
    // keyframe is.
    {
        CodecValues move;
        move.gopFrames = 4;
        move.persistence = 1.0;
        move.quality = 90.0;
        const auto followed = codec_test::codecPlan(move, 56900);
        CodecValues single = move;
        single.gopFrames = 1;
        const auto own = codec_test::codecPlan(single, 57000);
        const auto chained = frameOf(evaluator, followed, 5);
        const auto fifth = frameOf(evaluator, own, 5);
        const auto fourth = frameOf(evaluator, own, 4);
        const double moved = mean(chained, fifth);
        const double unmoved = mean(fourth, fifth);
        expect(moved < 0.8 * unmoved, "a predicted frame's blocks follow the flow, not against it");
    }
    // A parameter changes the frame it is drawn on.
    {
        CodecValues sharper = values;
        sharper.quality = 80.0;
        const auto fine = codec_test::codecPlan(sharper, 56300);
        expect(frameOf(evaluator, grouped, 5) != frameOf(evaluator, fine, 5),
               "the quality changes the frame");
    }
}

} // namespace

int main() {
    try {
        testColourRoundTrip();
        testKeyframe();
        testChroma();
        testPredicted();
        testWindows();
        testStages();
        testProgram();
        testBudgetCancellationAndBands();
        testChain();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "codec degrade tests passed\n";
    return 0;
}
