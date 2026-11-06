// #484 Motion blur on the CPU reference. The shutter model (intervals, strata, curve weights, the
// sample counts and Final's adaptive count); the accumulator's operators against their formulas;
// the path sampler's promises (a flat image stays flat, energy is kept, a zero extent is the
// identity, jitter is deterministic); the motion-vector stages (zero vectors are the identity, a
// constant vector blurs along itself only, the outside-in fill takes the colour outside the matte,
// the forward-backward check); the optical flow solvers recovering a known shift and the cut
// diagnostic; every production chain through the evaluator (cache, ROI, finite output, the
// sub-frame accumulation against the Transform 2D shutter, Echo's weights, flow blur of a still
// image); the one program whose values are data; the pixel budget and cancellation.

#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"
#include "motion_blur_image_effect.hpp"
#include "motion_blur_test_support.hpp"
#include "motion_shutter.hpp"

#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using runtime::AccumulateOperator;
using runtime::MotionPathBlurKernel;
using runtime::MotionPathMode;
using runtime::OpticalFlowKernel;
using runtime::TemporalAccumulateKernel;
using runtime::VectorBlurKernel;
using runtime::VectorBlurStage;
using Pixel = std::array<float, 4>;
using Image = render::Rgba32fImage;

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

Image image(const std::int64_t width, const std::int64_t height,
            const std::function<Pixel(std::int64_t, std::int64_t)>& value,
            const std::int64_t originX = 0, const std::int64_t originY = 0,
            const std::int64_t displayWidth = 0, const std::int64_t displayHeight = 0) {
    const auto data = render::ImageWindow::create(originX, originY, static_cast<std::uint64_t>(width),
                                                  static_cast<std::uint64_t>(height));
    const auto display = render::ImageWindow::create(
        0, 0, static_cast<std::uint64_t>(displayWidth > 0 ? displayWidth : width),
        static_cast<std::uint64_t>(displayHeight > 0 ? displayHeight : height));
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

using Sample = std::array<double, 4>;
Sample at(const Image& source, const std::int64_t x, const std::int64_t y) {
    const auto window = source.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() ||
        x >= window.originX() + static_cast<std::int64_t>(window.extent().width()) ||
        y >= window.originY() + static_cast<std::int64_t>(window.extent().height()))
        return {0.0, 0.0, 0.0, 0.0};
    const auto view = source.view();
    const auto row = view.value()->row(y);
    const auto& p = (*row.value())[static_cast<std::size_t>(x - window.originX())];
    return {static_cast<double>(p.red()), static_cast<double>(p.green()),
            static_cast<double>(p.blue()), static_cast<double>(p.alpha())};
}

// A smooth texture with structure in both directions (no shift of a few pixels repeats).
Pixel texture(const double x, const double y) {
    const double r = 0.5 + 0.25 * std::sin(0.7 * x + 0.3 * y) + 0.2 * std::cos(0.45 * y - 0.2 * x);
    const double g = 0.4 + 0.3 * std::sin(0.9 * x) * std::cos(0.8 * y);
    const double b = 0.3 + 0.2 * std::cos(0.33 * x + 0.61 * y);
    return {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), 1.0F};
}

runtime::detail::MotionGeometry geometryOf(const std::int64_t width, const std::int64_t height) {
    runtime::detail::MotionGeometry geometry;
    geometry.display = {0, 0, width, height};
    geometry.centreX = static_cast<double>(width) / 2.0;
    geometry.centreY = static_cast<double>(height) / 2.0;
    return geometry;
}

std::vector<double> pathValues(const double angle, const double length, const double cx,
                               const double cy, const double amount) {
    std::vector<double> values{0.0, 0.0, angle, length, cx, cy, amount};
    for (int i = 0; i < 8; ++i)
        values.push_back(0.0);
    return values;
}

runtime::detail::ImageEffectResult run(const runtime::ImageEffectKernel& kernel,
                                       const std::vector<double>& values,
                                       const std::vector<const Image*>& inputs,
                                       const runtime::detail::MotionGeometry& geometry,
                                       const std::size_t budget = 1U << 28U,
                                       const runtime::CancellationToken& token = {}) {
    const auto grade = runtime::detail::prepareMotionGrade(kernel, values);
    if (!grade)
        throw std::logic_error("the motion kernel prepares");
    return runtime::detail::applyMotion(kernel, *grade, inputs, geometry, budget, token,
                                        runtime::OperationIndex::fromRaw(0), {}, nullptr);
}

std::vector<const Image*> slotsOf(const runtime::ImageEffectKernel& kernel, const Image& input) {
    std::vector<const Image*> inputs(runtime::detail::motionInputCount(kernel), nullptr);
    inputs[0] = &input;
    return inputs;
}

bool finiteImage(const Image& source) {
    const auto window = source.descriptor()->dataWindow();
    for (std::int64_t y = window.originY();
         y < window.originY() + static_cast<std::int64_t>(window.extent().height()); ++y)
        for (std::int64_t x = window.originX();
             x < window.originX() + static_cast<std::int64_t>(window.extent().width()); ++x) {
            const auto p = at(source, x, y);
            for (const auto c : p)
                if (!std::isfinite(c))
                    return false;
            if (p[3] < 0.0 || p[3] > 1.0)
                return false;
        }
    return true;
}

// --- The shutter ---------------------------------------------------------------------------------

void testShutter() {
    using runtime::detail::shutterInterval;
    using runtime::detail::shutterSampleCount;
    using runtime::detail::shutterSamples;
    runtime::CompiledShutter shutter;
    shutter.shutter = 0.5;
    const auto centred = shutterInterval(shutter);
    expect(centred[0] == -0.25 && centred[1] == 0.25, "a centred 180 degree shutter is [-1/4, 1/4]");
    shutter.offset = document::kShutterOffsetStart;
    expect(shutterInterval(shutter) == std::array<double, 2>{0.0, 0.5}, "a start shutter opens on the frame");
    shutter.offset = document::kShutterOffsetEnd;
    expect(shutterInterval(shutter) == std::array<double, 2>{-0.5, 0.0}, "an end shutter closes on the frame");
    shutter.offset = document::kShutterOffsetCustom;
    shutter.phase = -0.125;
    expect(shutterInterval(shutter) == std::array<double, 2>{-0.125, 0.375},
           "a custom shutter opens at its phase");
    shutter.offset = document::kShutterOffsetCentred;
    for (const auto curve : {document::kShutterCurveBox, document::kShutterCurveTriangle,
                             document::kShutterCurveGaussian, document::kShutterCurveCustom}) {
        auto s = motion_test::shutterOf({.curve = curve});
        const auto samples = shutterSamples(s, 8);
        const double sum = std::accumulate(samples.weight.begin(), samples.weight.end(), 0.0);
        expect(samples.weight.size() == 8 && near(sum, 1.0, 1e-12),
               "the curve's weights sum to one (" + std::to_string(curve) + ")");
        expect(near(samples.centre[0], -0.25 + 0.5 / 16.0, 1e-12) &&
                   near(samples.centre[7], 0.25 - 0.5 / 16.0, 1e-12),
               "the strata are evenly spaced over the open time");
        if (curve == document::kShutterCurveBox)
            expect(std::ranges::all_of(samples.weight,
                                       [](const double w) { return near(w, 0.125, 1e-12); }),
                   "a box shutter weighs every stratum equally");
        else
            expect(samples.weight[3] > samples.weight[0] && near(samples.weight[3], samples.weight[4], 1e-9),
                   "a peaked shutter weighs the middle most, symmetrically");
    }
    for (const auto [quality, count] : std::array<std::pair<std::int64_t, std::int64_t>, 3>{
             {{document::kMotionQualityDraft, 4},
              {document::kMotionQualityStandard, 16},
              {document::kMotionQualityHigh, 64}}}) {
        runtime::CompiledShutter q;
        q.quality = quality;
        expect(shutterSampleCount(q, 1000.0) == count, "a fixed quality takes its sample count");
    }
    runtime::CompiledShutter final;
    final.quality = document::kMotionQualityFinal;
    final.minSamples = 8;
    final.maxSamples = 64;
    expect(shutterSampleCount(final, 0.0) == 8, "Final takes its minimum for a still image");
    expect(shutterSampleCount(final, 20.0) == 32, "Final takes a power of two per pixel moved");
    expect(shutterSampleCount(final, 1000.0) == 64, "Final is limited by its maximum");
    expect(document::shutterFrames(180.0) == 0.5 && document::shutterFrames(360.0) == 1.0,
           "a shutter angle is its fraction of a frame");
}

// --- The accumulator -------------------------------------------------------------------------------

void testAccumulate() {
    const auto a = image(6, 4, [](auto x, auto y) -> Pixel {
        return {0.2F * static_cast<float>(x), 0.5F, 3.0F, 0.25F * static_cast<float>(y + 1) - 0.25F};
    });
    // The sample is offset: the output is the union of both windows.
    const auto s = image(6, 4, [](auto x, auto) -> Pixel {
        return {1.5F, 0.125F * static_cast<float>(x), 0.25F, 0.5F};
    }, 2, 1, 6, 4);
    const auto geometry = geometryOf(6, 4);
    for (std::uint8_t op = 0; op <= 5; ++op) {
        TemporalAccumulateKernel kernel{static_cast<AccumulateOperator>(op), 0.75, 0.5};
        const auto result = run(kernel, {0.0, 0.0}, {&a, &s}, geometry);
        expect(result.image != nullptr, "the accumulator evaluates (" + std::to_string(op) + ")");
        if (!result.image)
            continue;
        const auto window = result.image->descriptor()->dataWindow();
        expect(window.originX() == 0 && window.originY() == 0 && window.extent().width() == 8 &&
                   window.extent().height() == 5,
               "the accumulator covers the union of its windows");
        bool exact = true;
        for (std::int64_t y = 0; y < 5; ++y)
            for (std::int64_t x = 0; x < 8; ++x) {
                const auto pa = at(a, x, y);
                const auto ps = at(s, x, y);
                Pixel e{};
                std::array<float, 4> wa{};
                std::array<float, 4> ws{};
                for (std::size_t c = 0; c < 4; ++c) {
                    wa[c] = static_cast<float>(pa[c]) * 0.75F;
                    ws[c] = static_cast<float>(ps[c]) * 0.5F;
                }
                for (std::size_t c = 0; c < 4; ++c) {
                    switch (op) {
                    case 0: e[c] = wa[c] + ws[c]; break;
                    case 1: e[c] = std::max(wa[c], ws[c]); break;
                    case 2: e[c] = std::min(wa[c], ws[c]); break;
                    case 3: e[c] = (wa[c] + ws[c]) - wa[c] * ws[c]; break;
                    case 4: e[c] = ws[c] + wa[c] * (1.0F - ws[3]); break;
                    default: e[c] = wa[c] + ws[c] * (1.0F - wa[3]); break;
                    }
                }
                e[3] = std::clamp(e[3], 0.0F, 1.0F);
                const auto o = at(*result.image, x, y);
                for (std::size_t c = 0; c < 4; ++c)
                    exact = exact && near(o[c], static_cast<double>(e[c]), 1e-6);
            }
        expect(exact, "the accumulator's operator matches its formula (" + std::to_string(op) + ")");
    }
}

// --- The path sampler ------------------------------------------------------------------------------

void testPath() {
    const auto geometry = geometryOf(32, 24);
    const auto flat = image(32, 24, [](auto, auto) -> Pixel { return {2.5F, 1.0F, 0.5F, 0.75F}; });
    const auto impulse = image(32, 24, [](auto x, auto y) -> Pixel {
        return x == 10 && y == 12 ? Pixel{8.0F, 4.0F, 2.0F, 1.0F} : Pixel{0.0F, 0.0F, 0.0F, 0.0F};
    });
    MotionPathBlurKernel kernel;
    kernel.shutter.shutter = 1.0;
    kernel.shutter.quality = document::kMotionQualityHigh;
    kernel.shutter.maxBlur = 64.0;
    const auto zero = runtime::detail::prepareMotionGrade(kernel, pathValues(0.0, 0.0, 0, 0, 0));
    expect(zero && zero->identity, "a directional blur of no length is the identity");
    for (const auto mode : {MotionPathMode::Directional, MotionPathMode::Spin, MotionPathMode::Zoom}) {
        kernel.mode = mode;
        const auto values = pathValues(0.0, 8.0, 0.0, 0.0, mode == MotionPathMode::Spin ? 30.0 : 0.4);
        const auto result = run(kernel, values, {&flat}, geometry);
        bool stays = result.image != nullptr;
        for (std::int64_t y = 10; y < 14 && stays; ++y)
            for (std::int64_t x = 14; x < 18 && stays; ++x) {
                const auto p = at(*result.image, x, y);
                stays = near(p[0], 2.5, 1e-5) && near(p[3], 0.75, 1e-5);
            }
        expect(stays, "a flat image stays flat under the path sampler (" +
                          std::to_string(static_cast<int>(mode)) + ")");
    }
    // Directional along screen right: the impulse spreads along its row only, its energy kept.
    kernel.mode = MotionPathMode::Directional;
    const auto streak = run(kernel, pathValues(0.0, 8.0, 0.0, 0.0, 0.0), {&impulse}, geometry);
    expect(streak.image != nullptr, "the directional blur evaluates");
    if (streak.image) {
        double sum = 0.0;
        double off = 0.0;
        int lit = 0;
        for (std::int64_t y = 0; y < 24; ++y)
            for (std::int64_t x = 0; x < 32; ++x) {
                const auto p = at(*streak.image, x, y);
                if (y == 12) {
                    sum += p[0];
                    lit += p[0] > 0.0 ? 1 : 0;
                } else {
                    off += std::abs(p[0]);
                }
            }
        expect(near(sum, 8.0, 1e-4), "the directional blur keeps the impulse's energy");
        expect(off == 0.0, "a horizontal blur leaves the other rows untouched");
        expect(lit >= 7 && lit <= 10, "the streak is the blur's length");
    }
    // Spin and Zoom leave their centre fixed.
    for (const auto mode : {MotionPathMode::Spin, MotionPathMode::Zoom}) {
        kernel.mode = mode;
        const auto centred = image(32, 24, [](auto x, auto y) -> Pixel {
            return x == 16 && y == 12 ? Pixel{4.0F, 4.0F, 4.0F, 1.0F} : Pixel{0.0F, 0.0F, 0.0F, 0.0F};
        });
        // The centre is an offset from the display centre (16, 12); the pixel's centre is +0.5.
        const auto result = run(kernel, pathValues(0.0, 0.0, 0.5, 0.5, mode == MotionPathMode::Spin ? 90.0 : 0.5),
                                {&centred}, geometry);
        expect(result.image && near(at(*result.image, 16, 12)[0], 4.0, 1e-4),
               "spin and zoom keep the pixel at their centre");
    }
    // Jitter is deterministic per node, frame and pixel, and changes the samples.
    kernel.mode = MotionPathMode::Directional;
    kernel.shutter.quality = document::kMotionQualityDraft;
    const auto textured = image(32, 24, [](auto x, auto y) {
        return texture(static_cast<double>(x), static_cast<double>(y));
    });
    const auto plain = run(kernel, pathValues(30.0, 12.0, 0, 0, 0), {&textured}, geometry);
    kernel.shutter.jitter = true;
    kernel.seed = 77;
    const auto first = run(kernel, pathValues(30.0, 12.0, 0, 0, 0), {&textured}, geometry);
    const auto second = run(kernel, pathValues(30.0, 12.0, 0, 0, 0), {&textured}, geometry);
    expect(first.image && second.image && plain.image &&
               std::ranges::equal(first.image->pixels(), second.image->pixels()),
           "jitter is deterministic");
    expect(first.image && plain.image &&
               !std::ranges::equal(first.image->pixels(), plain.image->pixels()),
           "jitter moves the samples within their strata");
    // Spin beyond its range is refused.
    kernel.mode = MotionPathMode::Spin;
    expect(!runtime::detail::prepareMotionGrade(kernel, pathValues(0, 0, 0, 0, 7200.0)),
           "a spin beyond ten turns is refused");
}

// --- The motion-vector stages ---------------------------------------------------------------------

Image constantVectors(const float u, const float v, const std::int64_t w = 32,
                      const std::int64_t h = 24) {
    return image(w, h, [=](auto, auto) -> Pixel { return {u, v, 0.0F, 1.0F}; });
}

void testVectors() {
    const auto geometry = geometryOf(32, 24);
    const auto colour = image(32, 24, [](auto x, auto y) {
        auto p = texture(static_cast<double>(x), static_cast<double>(y));
        p[3] = 0.8F;
        for (std::size_t c = 0; c < 3; ++c)
            p[c] *= 0.8F * 3.0F;
        return p;
    });
    const auto still = constantVectors(0.0F, 0.0F);
    for (const auto stage : {VectorBlurStage::Gather, VectorBlurStage::Splat}) {
        VectorBlurKernel kernel;
        kernel.stage = stage;
        auto inputs = slotsOf(kernel, colour);
        inputs[1 + runtime::kVectorForwardSlot] = &still;
        kernel.slotImages[runtime::kVectorForwardSlot] = 0;
        const auto result = run(kernel, {0.0, 0.0}, inputs, geometry);
        bool same = result.image != nullptr;
        for (std::int64_t y = 0; y < 24 && same; ++y)
            for (std::int64_t x = 0; x < 32 && same; ++x) {
                const auto o = at(*result.image, x, y);
                const auto i = at(colour, x, y);
                for (std::size_t c = 0; c < 4; ++c)
                    same = same && near(o[c], i[c], 2e-6);
            }
        expect(same, std::string("zero vectors are the identity (") +
                         (stage == VectorBlurStage::Gather ? "gather" : "splat") + ")");
    }
    // A constant horizontal vector over vertical stripes: every column stays constant down the
    // image, the stripes soften, a flat image stays flat.
    const auto stripes = image(32, 24, [](auto x, auto) -> Pixel {
        return (x / 4) % 2 == 0 ? Pixel{3.0F, 1.0F, 0.5F, 1.0F} : Pixel{0.2F, 0.4F, 0.1F, 1.0F};
    });
    const auto across = constantVectors(6.0F, 0.0F);
    for (const auto stage : {VectorBlurStage::Gather, VectorBlurStage::Splat}) {
        VectorBlurKernel kernel;
        kernel.stage = stage;
        kernel.shutter.quality = document::kMotionQualityHigh;
        kernel.shutter.shutter = 1.0;
        auto inputs = slotsOf(kernel, stripes);
        inputs[1 + runtime::kVectorForwardSlot] = &across;
        kernel.slotImages[runtime::kVectorForwardSlot] = 0;
        const auto result = run(kernel, {0.0, 0.0}, inputs, geometry);
        expect(result.image != nullptr && finiteImage(*result.image), "a vector blur evaluates");
        if (!result.image)
            continue;
        bool columns = true;
        for (std::int64_t x = 8; x < 24; ++x)
            for (std::int64_t y = 6; y < 18; ++y)
                columns = columns && near(at(*result.image, x, y)[0], at(*result.image, x, 6)[0], 1e-5);
        expect(columns, "a horizontal vector blurs along rows only");
        const auto edge = at(*result.image, 16, 12)[0];
        expect(edge > 0.25 && edge < 2.95, "the stripes soften along the vector");
    }
    // NeighborMax keeps only what reaches a tile: a wide Maximum blur changes nothing where the
    // vectors are short and a fast strip is out of reach.
    {
        const auto wideStripes = image(128, 24, [](auto x, auto) -> Pixel {
            const float v = (x / 3) % 2 == 0 ? 3.0F : 0.5F;
            return {v, v, v, 1.0F};
        });
        const auto mixed = image(128, 24, [](auto x, auto) -> Pixel {
            return {x < 8 ? 12.0F : 2.0F, 0.0F, 0.0F, 1.0F};
        });
        const auto wide = geometryOf(128, 24);
        const auto blur = [&](const double limit) {
            VectorBlurKernel kernel;
            kernel.stage = VectorBlurStage::Gather;
            kernel.shutter.shutter = 1.0;
            kernel.shutter.maxBlur = limit;
            kernel.shutter.quality = document::kMotionQualityHigh;
            auto inputs = slotsOf(kernel, wideStripes);
            inputs[1 + runtime::kVectorForwardSlot] = &mixed;
            kernel.slotImages[runtime::kVectorForwardSlot] = 0;
            return run(kernel, {0.0, 0.0}, inputs, wide);
        };
        const auto tight = blur(32.0);
        const auto open = blur(512.0);
        expect(tight.image != nullptr && open.image != nullptr, "the mixed vectors evaluate");
        if (tight.image && open.image) {
            bool same = true;
            for (std::int64_t x = 64; x < 120; ++x)
                same = same && near(at(*tight.image, x, 12)[0], at(*open.image, x, 12)[0], 1e-5);
            expect(same, "a wide limit leaves short vectors out of a fast strip's reach unchanged");
            expect(!near(at(*open.image, 80, 12)[0], at(wideStripes, 80, 12)[0], 1e-5),
                   "the short vectors still blur");
        }
    }
    // RE:Vision's validity: where the coverage in the vector image's third channel is not full on,
    // the vector is undefined and the pixel keeps its colour.
    {
        const auto partly = image(32, 24, [](auto x, auto) -> Pixel {
            return {6.0F, 0.0F, x < 16 ? 1.0F : 0.5F, 1.0F};
        });
        VectorBlurKernel kernel;
        kernel.stage = VectorBlurStage::Gather;
        kernel.decode.alphaValid = true;
        kernel.shutter.shutter = 1.0;
        kernel.shutter.quality = document::kMotionQualityHigh;
        auto inputs = slotsOf(kernel, stripes);
        inputs[1 + runtime::kVectorForwardSlot] = &partly;
        kernel.slotImages[runtime::kVectorForwardSlot] = 0;
        const auto result = run(kernel, {0.0, 0.0}, inputs, geometry);
        expect(result.image != nullptr, "an alpha-valid gather evaluates");
        if (result.image) {
            // Far from the boundary on the invalid side nothing moves there or reaches it.
            expect(near(at(*result.image, 28, 12)[0], at(stripes, 28, 12)[0], 1e-5),
                   "an invalid vector leaves its pixel sharp");
            expect(!near(at(*result.image, 6, 12)[0], at(stripes, 6, 12)[0], 1e-5),
                   "a valid vector blurs its pixel");
        }
    }
    // The outside-in fill: the pixels under the matte take the colour around it.
    {
        const auto halves = image(32, 24, [](auto x, auto) -> Pixel {
            return x < 16 ? Pixel{2.0F, 0.0F, 0.0F, 1.0F} : Pixel{0.0F, 0.0F, 3.0F, 1.0F};
        });
        const auto matte = image(8, 8, [](auto, auto) -> Pixel { return {1.0F, 1.0F, 1.0F, 1.0F}; },
                                 12, 8, 32, 24);
        VectorBlurKernel fill;
        fill.stage = VectorBlurStage::Fill;
        fill.threshold = 0.5;
        auto inputs = slotsOf(fill, halves);
        inputs[1 + runtime::kVectorMatteSlot] = &matte;
        fill.slotImages[runtime::kVectorMatteSlot] = 0;
        const auto result = run(fill, {0.0, 0.0}, inputs, geometry);
        expect(result.image != nullptr, "the outside-in fill evaluates");
        if (result.image) {
            const auto left = at(*result.image, 13, 12);
            const auto right = at(*result.image, 18, 12);
            expect(left[0] > 1.5 && left[2] < 0.5, "the fill takes the nearest colour on the left");
            expect(right[2] > 2.5 && right[0] < 0.5, "the fill takes the nearest colour on the right");
            expect(at(*result.image, 2, 2) == at(halves, 2, 2), "the fill leaves the unmatted pixels");
        }
    }
    // Sundaram et al.'s forward-backward check: consistent flows are confident, inconsistent ones
    // are occluded.
    {
        const auto forward = constantVectors(2.0F, 1.0F);
        const auto back = constantVectors(-2.0F, -1.0F);
        const auto wrong = constantVectors(3.0F, -2.0F);
        for (const bool consistent : {true, false}) {
            VectorBlurKernel check;
            check.stage = VectorBlurStage::Consistency;
            check.border = 1;
            auto inputs = slotsOf(check, forward);
            inputs[1 + runtime::kVectorForwardSlot] = consistent ? &back : &wrong;
            check.slotImages[runtime::kVectorForwardSlot] = 0;
            const auto result = run(check, {0.0, 0.0}, inputs, geometry);
            expect(result.image != nullptr, "the consistency check evaluates");
            if (result.image) {
                const auto p = at(*result.image, 16, 12);
                expect(consistent ? p[3] > 0.99 : p[3] < 0.01,
                       consistent ? "a consistent flow is confident" : "an inconsistent flow is occluded");
            }
            check.layer = 1;
            const auto occlusion = run(check, {0.0, 0.0}, inputs, geometry);
            if (occlusion.image) {
                const auto p = at(*occlusion.image, 16, 12);
                expect(consistent ? p[3] < 0.01 : p[3] > 0.99, "occlusion is the complement");
            }
        }
    }
}

// --- Optical flow ------------------------------------------------------------------------------------

std::array<double, 2> meanFlow(const Image& flow, const std::int64_t margin, const std::int64_t w,
                               const std::int64_t h) {
    double u = 0.0;
    double v = 0.0;
    double n = 0.0;
    for (std::int64_t y = margin; y < h - margin; ++y)
        for (std::int64_t x = margin; x < w - margin; ++x) {
            const auto p = at(flow, x, y);
            u += p[0];
            v += p[1];
            n += 1.0;
        }
    return {u / n, v / n};
}

void testFlow() {
    constexpr std::int64_t w = 48;
    constexpr std::int64_t h = 40;
    const auto geometry = geometryOf(w, h);
    // The texture at half its frequency, so every pyramid level the solvers use still resolves it.
    const auto from = image(w, h, [](auto x, auto y) {
        return texture(0.5 * static_cast<double>(x), 0.5 * static_cast<double>(y));
    });
    // The texture moved 2 right and 1 down: the flow from `from` to `to` is (2, 1).
    const auto to = image(w, h, [](auto x, auto y) {
        return texture(0.5 * (static_cast<double>(x) - 2.0), 0.5 * (static_cast<double>(y) - 1.0));
    });
    struct Case final {
        std::int64_t method;
        std::int64_t quality;
        double tolerance;
        const char* name;
    };
    for (const auto& c : {Case{0, document::kMotionQualityDraft, 0.5, "DIS draft"},
                          Case{0, document::kMotionQualityStandard, 0.35, "DIS standard"},
                          Case{0, document::kMotionQualityFinal, 0.3, "DIS final"},
                          Case{1, document::kMotionQualityDraft, 0.5, "TV-L1 draft"},
                          Case{1, document::kMotionQualityStandard, 0.3, "TV-L1 standard"},
                          Case{1, document::kMotionQualityFinal, 0.2, "TV-L1 final"}}) {
        OpticalFlowKernel kernel;
        kernel.method = c.method;
        kernel.quality = c.quality;
        kernel.slotImages = {0, -1, -1};
        auto inputs = slotsOf(kernel, from);
        inputs[1] = &to;
        const auto result = run(kernel, {0.0, 0.0}, inputs, geometry);
        expect(result.image != nullptr && finiteImage(*result.image),
               std::string(c.name) + ": the flow evaluates");
        if (!result.image)
            continue;
        const auto mean = meanFlow(*result.image, 8, w, h);
        expect(std::abs(mean[0] - 2.0) < c.tolerance && std::abs(mean[1] - 1.0) < c.tolerance,
               std::string(c.name) + ": the flow recovers the shift (" + std::to_string(mean[0]) +
                   ", " + std::to_string(mean[1]) + ")");
        // The same frames have no motion.
        inputs[1] = &from;
        const auto none = run(kernel, {0.0, 0.0}, inputs, geometry);
        if (none.image) {
            const auto zero = meanFlow(*none.image, 4, w, h);
            expect(std::abs(zero[0]) < 0.05 && std::abs(zero[1]) < 0.05,
                   std::string(c.name) + ": identical frames have no flow");
        }
    }
    // The cut diagnostic: the residual of a good flow stays below the threshold, an unrelated
    // frame's exceeds it.
    {
        OpticalFlowKernel solve;
        solve.slotImages = {0, -1, -1};
        auto inputs = slotsOf(solve, from);
        inputs[1] = &to;
        const auto flow = run(solve, {0.0, 0.0}, inputs, geometry);
        const auto other = image(w, h, [](auto x, auto y) -> Pixel {
            return (x + y) % 2 == 0 ? Pixel{4.0F, 0.0F, 4.0F, 1.0F} : Pixel{0.0F, 3.0F, 0.0F, 1.0F};
        });
        OpticalFlowKernel cut;
        cut.stage = runtime::OpticalFlowStage::CutDetect;
        cut.cutThreshold = 24.0;
        cut.slotImages = {0, -1, 1};
        for (const bool unrelated : {false, true}) {
            auto cutInputs = slotsOf(cut, from);
            cutInputs[1] = unrelated ? &other : &to;
            cutInputs[3] = flow.image.get();
            const auto result = run(cut, {0.0, 0.0}, cutInputs, geometry);
            expect(result.image != nullptr, "the cut diagnostic evaluates");
            if (result.image)
                expect(unrelated ? at(*result.image, 5, 5)[3] == 1.0
                                 : at(*result.image, 5, 5)[3] == 0.0,
                       unrelated ? "an unrelated frame is a cut" : "a moved frame is no cut");
        }
    }
}

// --- The production chains through the evaluator ------------------------------------------------------

runtime::EvaluationRequest request(const runtime::OperationIndex output,
                                   const std::int64_t frame = 4) {
    runtime::EvaluationRequest value{*core::RationalTime::create(frame, 24), output,
                                     runtime::CompositionFormatResolution{}};
    value.pixelStorageByteLimit = 1U << 27U;
    return value;
}

const Image* frameOf(const runtime::EvaluationResult& result) {
    return result.frame() ? &result.frame()->processImage() : nullptr;
}

double maxDifference(const Image& a, const Image& b, const std::int64_t x0, const std::int64_t y0,
                     const std::int64_t x1, const std::int64_t y1) {
    double worst = 0.0;
    for (std::int64_t y = y0; y < y1; ++y)
        for (std::int64_t x = x0; x < x1; ++x) {
            const auto p = at(a, x, y);
            const auto q = at(b, x, y);
            for (std::size_t c = 0; c < 4; ++c)
                worst = std::max(worst, std::abs(static_cast<double>(p[c]) - q[c]));
        }
    return worst;
}

void testChains() {
    const runtime::CpuCompositionEvaluator evaluator;
    using motion_test::MotionValues;
    using motion_test::Scene;
    const std::vector<std::pair<std::string, MotionValues>> cases{
        {"directional", {.scene = Scene::Directional}},
        {"spin", {.scene = Scene::Spin}},
        {"zoom", {.scene = Scene::Zoom, .jitter = true}},
        {"transform blur", {.scene = Scene::TransformBlur}},
        {"transform blur gaussian", {.scene = Scene::TransformBlur, .curve = document::kShutterCurveGaussian}},
        {"sub-frame", {.scene = Scene::SubFrame, .quality = document::kMotionQualityDraft}},
        {"sub-frame custom curve", {.scene = Scene::SubFrame, .quality = document::kMotionQualityDraft, .curve = document::kShutterCurveCustom, .offset = document::kShutterOffsetEnd}},
        {"echo add", {.scene = Scene::Echo}},
        {"echo over posterized", {.scene = Scene::Echo, .op = AccumulateOperator::Over, .posterize = true}},
        {"echo screen", {.scene = Scene::Echo, .op = AccumulateOperator::Screen}},
        {"echo, early samples absent", {.scene = Scene::Echo, .layerIn = 3}},
        {"echo, the frame's sample absent", {.scene = Scene::Echo, .layerOut = 2}},
        {"vector gather", {.scene = Scene::VectorGather}},
        {"vector gather curved depth", {.scene = Scene::VectorGather, .jitter = true, .curved = true, .depth = true}},
        {"vector splat", {.scene = Scene::VectorSplat, .depth = true}},
        {"vector layered", {.scene = Scene::VectorLayered}},
        {"flow DIS", {.scene = Scene::FlowDis, .quality = document::kMotionQualityDraft}},
        {"flow TV-L1", {.scene = Scene::FlowTvl1}},
        {"flow consistency", {.scene = Scene::FlowConsistency}},
        {"flow occlusion", {.scene = Scene::FlowConsistency, .depth = true}},
        {"flow cut", {.scene = Scene::FlowCut}},
        {"flow cut unrelated", {.scene = Scene::FlowCut, .depth = true}},
        {"flow blur", {.scene = Scene::FlowBlur, .quality = document::kMotionQualityDraft}},
        {"flow blur matte", {.scene = Scene::FlowBlurMatte, .quality = document::kMotionQualityDraft}},
        {"flow blur temporal", {.scene = Scene::FlowBlurTemporal, .quality = document::kMotionQualityDraft}},
        {"flow blur wrap", {.scene = Scene::FlowBlur, .quality = document::kMotionQualityDraft, .border = document::kFlowBorderWrap}},
    };
    for (const auto& [name, values] : cases) {
        const auto plan = motion_test::motionPlan(values).plan;
        const auto cold = evaluator.evaluate(plan, request(plan->output()), {});
        expect(frameOf(cold) != nullptr, name + ": the production chain evaluates");
        if (!frameOf(cold)) {
            for (const auto& diagnostic : cold.diagnostics())
                std::cerr << "  " << diagnostic.summary << ": " << diagnostic.detail << '\n';
            continue;
        }
        expect(finiteImage(*frameOf(cold)), name + ": every pixel is finite with alpha in [0, 1]");
        const auto warm = evaluator.evaluate(plan, request(plan->output()), {});
        expect(frameOf(warm) && std::ranges::equal(frameOf(warm)->pixels(), frameOf(cold)->pixels()),
               name + ": a warm evaluation is identical");
        auto roiRequest = request(plan->output());
        const auto roiWindow = render::ImageWindow::create(6, 5, 12, 9);
        roiRequest.roi = *roiWindow.value();
        const auto roi = evaluator.evaluate(plan, roiRequest, {});
        expect(frameOf(roi) && maxDifference(*frameOf(roi), *frameOf(cold), 6, 5, 18, 14) == 0.0,
               name + ": an ROI matches the full frame inside it");
    }

    // Every sample absent: no image at all, and no failure.
    {
        const auto plan = motion_test::motionPlan({.scene = Scene::Echo, .layerIn = 100}).plan;
        const auto result = evaluator.evaluate(plan, request(plan->output()), {});
        expect(frameOf(result) != nullptr, "an Echo reading no image at any time evaluates");
    }
    // The sub-frame accumulation of the moving texture and Transform 2D's own shutter sample the
    // same times: they agree.
    {
        const auto sub = motion_test::motionPlan({.scene = Scene::SubFrame}).plan;
        const auto shutter = motion_test::motionPlan({.scene = Scene::TransformBlur}).plan;
        const auto a = evaluator.evaluate(sub, request(sub->output()), {});
        const auto b = evaluator.evaluate(shutter, request(shutter->output()), {});
        expect(frameOf(a) && frameOf(b) &&
                   maxDifference(*frameOf(a), *frameOf(b), 4, 4, 28, 20) < 2e-4,
               "sub-frame accumulation matches the Transform 2D shutter");
        // And both differ from the frame itself: the texture is blurred.
        const auto sharp = motion_test::motionPlan({.scene = Scene::SubFrame, .shutter = 0.0}).plan;
        const auto c = evaluator.evaluate(sharp, request(sharp->output()), {});
        expect(frameOf(a) && frameOf(c) && maxDifference(*frameOf(a), *frameOf(c), 4, 4, 28, 20) > 0.01,
               "a moving texture is blurred by its shutter");
    }
    // A still texture: the sub-frame samples sum to it, Echo's weights to their total, and the
    // optical-flow blur finds no motion.
    {
        const auto base = motion_test::motionPlan({.scene = Scene::SubFrame, .shutter = 0.0, .still = true}).plan;
        const auto reference = evaluator.evaluate(base, request(base->output()), {});
        const auto sub = motion_test::motionPlan({.scene = Scene::SubFrame, .quality = document::kMotionQualityDraft, .still = true}).plan;
        const auto summed = evaluator.evaluate(sub, request(sub->output()), {});
        expect(frameOf(reference) && frameOf(summed) &&
                   maxDifference(*frameOf(reference), *frameOf(summed), 0, 0, 32, 24) < 4e-6,
               "the sub-frame samples of a still image sum to it");
        const auto echo = motion_test::motionPlan({.scene = Scene::Echo, .still = true}).plan;
        const auto echoed = evaluator.evaluate(echo, request(echo->output()), {});
        bool weighted = frameOf(reference) && frameOf(echoed);
        for (std::int64_t y = 0; y < 24 && weighted; ++y)
            for (std::int64_t x = 0; x < 32 && weighted; ++x) {
                const auto r = at(*frameOf(reference), x, y);
                const auto e = at(*frameOf(echoed), x, y);
                weighted = near(e[0], 1.875 * r[0], 1e-5) && near(e[3], 1.0, 1e-6);
            }
        expect(weighted, "Echo's samples of a still image add by their weights");
        const auto flow = motion_test::motionPlan({.scene = Scene::FlowBlur, .quality = document::kMotionQualityDraft, .still = true}).plan;
        const auto flowed = evaluator.evaluate(flow, request(flow->output()), {});
        expect(frameOf(reference) && frameOf(flowed) &&
                   maxDifference(*frameOf(reference), *frameOf(flowed), 4, 4, 28, 20) < 1e-3,
               "the optical-flow blur of a still image is the image");
    }
    // The flow through the plan recovers the texture's shift.
    {
        const auto plan = motion_test::motionPlan(
                              {.scene = Scene::FlowTvl1, .quality = document::kMotionQualityFinal})
                              .plan;
        const auto result = evaluator.evaluate(plan, request(plan->output()), {});
        expect(frameOf(result) != nullptr, "the plan's TV-L1 flow evaluates");
        if (frameOf(result)) {
            const auto mean = meanFlow(*frameOf(result), 7, 32, 24);
            expect(std::abs(mean[0] - 2.0) < 0.35 && std::abs(mean[1] - 1.0) < 0.35,
                   "the plan's TV-L1 flow recovers the shift (" + std::to_string(mean[0]) + ", " +
                       std::to_string(mean[1]) + ")");
        }
    }
    // A time-sampled plan differs at another frame; a still one is the same.
    {
        const auto plan = motion_test::motionPlan({.scene = Scene::Echo}).plan;
        const auto a = evaluator.evaluate(plan, request(plan->output(), 4), {});
        const auto b = evaluator.evaluate(plan, request(plan->output(), 5), {});
        expect(frameOf(a) && frameOf(b) && !std::ranges::equal(frameOf(a)->pixels(), frameOf(b)->pixels()),
               "a time-sampled chain follows the animation");
    }
}

// --- The program, budget and cancellation --------------------------------------------------------------

void testPrograms() {
    const auto colour = image(16, 8, [](auto x, auto y) {
        return texture(static_cast<double>(x), static_cast<double>(y));
    });
    MotionPathBlurKernel kernel;
    kernel.shutter.quality = document::kMotionQualityStandard;
    const auto grade = runtime::detail::prepareMotionGrade(kernel, pathValues(10.0, 6.0, 0, 0, 0));
    const std::array rects{runtime::detail::MotionRect{0, 0, 16, 8}};
    const auto plan = runtime::detail::planMotion(kernel, *grade, rects, geometryOf(16, 8));
    expect(plan && !plan->steps.empty(), "the path sampler plans");
    bool built = plan.has_value();
    if (plan)
        for (const auto& step : plan->steps)
            if (step.kind == runtime::detail::MotionStepKind::Run)
                built = built && runtime::detail::validMotionProgramData(step.table) &&
                        runtime::detail::buildColorGradeProgram(
                            runtime::detail::ColorGradeKind::Motion, step.table)
                            .succeeded();
    expect(built, "every pass is the one motion program over its data");
    expect(!runtime::detail::validMotionProgramData(std::vector<float>(8, 0.0F)),
           "a table without its header is refused");
    expect(runtime::detail::motionProgramText(1).find("bmk_main") != std::string::npos,
           "the program text carries the shared kernel");
    const auto program = runtime::detail::buildColorGradeProgram(
        runtime::detail::ColorGradeKind::Motion, std::vector<float>(80, 0.0F));
    expect(!program.succeeded(), "a table of pass zero is refused");
}

void testBudgetAndCancellation() {
    const auto colour = image(32, 32, [](auto, auto) -> Pixel { return {0.5F, 0.5F, 0.5F, 1.0F}; });
    MotionPathBlurKernel kernel;
    const auto refused = run(kernel, pathValues(0.0, 6.0, 0, 0, 0), {&colour}, geometryOf(32, 32), 64);
    expect(!refused.image && refused.diagnostic &&
               refused.diagnostic->code == runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "a motion blur beyond the pixel budget is refused");
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(484)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Motion blur cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result = run(kernel, pathValues(0.0, 6.0, 0, 0, 0), {&colour},
                                    geometryOf(32, 32), 1U << 24U, context.cancellation());
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
           "a cancelled motion blur publishes no image");
}

} // namespace

int main() {
    try {
        testShutter();
        testAccumulate();
        testPath();
        testVectors();
        testFlow();
        testChains();
        testPrograms();
        testBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " motion blur expectation(s) failed\n";
        return 1;
    }
    std::cout << "motion blur tests passed\n";
    return 0;
}
