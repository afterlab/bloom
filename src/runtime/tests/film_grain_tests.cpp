// #486 Film Grain on the CPU reference: the noise's statistics, its pinned values and its streams;
// the formula against an independent double-precision reading over an HDR, partial-alpha image; the
// promises the node makes (alpha kept, premultiplication, no clipping, a channel's own amount, the
// luma response, the size and the proxy scale, the data window and region of interest); frame and
// seed identity through the evaluator's memoization; identity, domains, the one fixed program, the
// pixel budget and cancellation.

#include "color_grade_image_effect.hpp"
#include "film_grain_image_effect.hpp"
#include "film_grain_test_support.hpp"
#include "image_effect.hpp"

#include <bloom/core/film_grain_noise.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
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

namespace {

using namespace bloom;
using film_grain_test::GrainValues;
using film_grain_test::grainKernel;
using film_grain_test::grainOperands;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Frame final {
    std::int64_t dataX = 0;
    std::int64_t dataY = 0;
    std::uint32_t width = 96;
    std::uint32_t height = 64;
    core::PixelAspectRatio aspect = core::PixelAspectRatio::square();
};

// A straight colour and its alpha at a pixel, premultiplied when stored.
struct Straight final {
    float red = 0.5F;
    float green = 0.5F;
    float blue = 0.5F;
    float alpha = 1.0F;
};

render::Rgba32fImage image(const Frame& frame, const Straight color) {
    const auto data =
        render::ImageWindow::create(frame.dataX, frame.dataY, frame.width, frame.height);
    auto descriptor =
        render::Rgba32fImageDescriptor::create(*data.value(), *data.value(), frame.aspect);
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (auto y = frame.dataY; y < frame.dataY + static_cast<std::int64_t>(frame.height); ++y)
        for (auto x = frame.dataX; x < frame.dataX + static_cast<std::int64_t>(frame.width); ++x) {
            const auto pixel = render::Rgba32f::fromPremultiplied(
                color.red * color.alpha, color.green * color.alpha, color.blue * color.alpha,
                color.alpha);
            if (!pixel || builder.value()->write(x, y, *pixel.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

std::optional<runtime::detail::PreparedColorGrade> prepare(const GrainValues& values,
                                                           const double frame = 0.0) {
    return runtime::detail::prepareColorGrade(grainKernel(values, 100),
                                              grainOperands(values, frame));
}

std::shared_ptr<const render::Rgba32fImage> apply(const GrainValues& values,
                                                  const render::Rgba32fImage& input,
                                                  const double frame = 0.0, const double scaleX = 1.0,
                                                  const double scaleY = 1.0) {
    const auto grade = prepare(values, frame);
    if (!grade)
        throw std::logic_error("the grain prepares");
    const auto result = runtime::detail::applyFilmGrain(*grade, input, scaleX, scaleY, 1U << 26U, {},
                                                        runtime::OperationIndex::fromRaw(0), {},
                                                        nullptr);
    if (!result.image)
        throw std::logic_error("the grain applies");
    return result.image;
}

std::array<float, 4> at(const render::Rgba32fImage& picture, const std::int64_t x,
                        const std::int64_t y) {
    std::array<float, 4> value{};
    if (const auto pixel = picture.read(x, y))
        std::ranges::copy(pixel.value()->components(), value.begin());
    return value;
}

struct Stats final {
    double mean = 0.0;
    double deviation = 0.0;
};
Stats statsOf(const std::vector<double>& values) {
    double sum = 0.0;
    for (const auto v : values)
        sum += v;
    const double mean = sum / static_cast<double>(values.size());
    double squares = 0.0;
    for (const auto v : values)
        squares += (v - mean) * (v - mean);
    return {mean, std::sqrt(squares / static_cast<double>(values.size()))};
}

void testNoise() {
    using namespace core::film_grain;
    // The values are part of every saved look: a change here changes the grain of every project.
    expect(stream(1, 0) != 0U, "a stream is drawn");
    expect(stream(1, 0) == 3139425692U && stream(1, 1) == 791299464U && stream(2, 0) == 3997897555U,
           "a seed and frame keep their stream from release to release");
    expect(noise(0.5F, 0.5F, stream(1, 0)) ==
                   std::array{0x1.2babd6p-2F, 0x1.1fd114p-2F, -0x1.59e104p-4F} &&
               noise(-3.25F, 7.75F, stream(1, 0)) ==
                   std::array{0x1.782386p-6F, -0x1.bda114p-3F, 0x1.e5ae84p-1F} &&
               noise(1000.125F, 250.5F, stream(1, 0)) ==
                   std::array{-0x1.2b8bacp-3F, 0x1.4d96a6p-3F, 0x1.25553cp-2F},
           "a point and stream keep their grain from release to release");

    // Over many points: zero mean and, normalised, unit deviation, at every spacing and in every
    // channel, and the channels do not follow each other.
    for (const float spacing : {4.0F, 1.0F, 0.37F, 0.11F}) {
        std::array<std::vector<double>, 3> samples;
        for (int y = -400; y < 400; ++y)
            for (int x = -400; x < 400; ++x) {
                const auto grain = noise(static_cast<float>(x) * spacing + 0.25F,
                                         static_cast<float>(y) * spacing + 0.25F, 12345U);
                for (std::size_t c = 0; c < 3; ++c)
                    samples[c].push_back(kNormalise * static_cast<double>(grain[c]));
            }
        for (std::size_t c = 0; c < 3; ++c) {
            const auto stats = statsOf(samples[c]);
            expect(std::abs(stats.mean) < 0.03,
                   "the grain has zero mean, got " + std::to_string(stats.mean));
            expect(std::abs(stats.deviation - 1.0) < 0.05,
                   "the grain's deviation is 1, got " + std::to_string(stats.deviation));
        }
        double together = 0.0;
        for (std::size_t i = 0; i < samples[0].size(); ++i)
            together += samples[0][i] * samples[2][i];
        together /= static_cast<double>(samples[0].size());
        expect(std::abs(together) < 0.05,
               "red and blue grain are independent, got " + std::to_string(together));
        // Their grains lie apart: where one channel is strong says nothing of the other.
        double strongTogether = 0.0;
        double strongRed = 0.0;
        double strongBlue = 0.0;
        for (std::size_t i = 0; i < samples[0].size(); ++i) {
            const double red = samples[0][i] * samples[0][i];
            const double blue = samples[2][i] * samples[2][i];
            strongTogether += red * blue;
            strongRed += red;
            strongBlue += blue;
        }
        const auto count = static_cast<double>(samples[0].size());
        expect(std::abs(strongTogether / count - (strongRed / count) * (strongBlue / count)) < 0.1,
               "red and blue grain are strong in different places");
    }

    // No lattice: the deviation is the same at a cell's centre and at its corner.
    std::vector<double> centres;
    std::vector<double> corners;
    for (int y = -300; y < 300; ++y)
        for (int x = -300; x < 300; ++x) {
            centres.push_back(
                static_cast<double>(noise(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, 7U)[0]));
            corners.push_back(
                static_cast<double>(noise(static_cast<float>(x), static_cast<float>(y), 7U)[0]));
        }
    const auto centre = statsOf(centres).deviation;
    const auto corner = statsOf(corners).deviation;
    expect(std::abs(centre - corner) < 0.03 * centre, "the grain's strength has no lattice");

    // A stream's seeds and frames are unrelated, and repeat.
    expect(stream(1, 10) == stream(1, 10), "a seed and frame always give the same stream");
    expect(stream(1, 10) != stream(1, 11) && stream(1, 10) != stream(2, 10) &&
               stream(1, -1) != stream(1, 1),
           "another frame or seed gives another stream");
}

void testPixels() {
    using document::filmGrainResponse;
    const Frame frame;
    GrainValues values = film_grain_test::everyPath();
    const std::array<Straight, 4> colors{Straight{0.05F, 0.02F, 0.01F, 0.5F},
                                         Straight{0.3F, 0.25F, 0.2F, 0.75F},
                                         Straight{0.8F, 0.6F, 0.5F, 1.0F},
                                         Straight{6.0F, 3.0F, 1.5F, 0.6F}};
    const auto scalar = [&](const std::string& role) { return values.scalars.at(role); };
    for (const auto color : colors) {
        const auto input = image(frame, color);
        const auto output = apply(values, input, 3.0);
        const auto stream = core::film_grain::stream(values.seed, 3);
        const double luma = 0.2126 * static_cast<double>(color.red) +
                            0.7152 * static_cast<double>(color.green) +
                            0.0722 * static_cast<double>(color.blue);
        const double response =
            filmGrainResponse(luma, scalar("shadows"), scalar("midtones"), scalar("highlights"));
        bool matches = true;
        bool alphaKept = true;
        for (std::int64_t y = 0; y < frame.height; ++y)
            for (std::int64_t x = 0; x < frame.width; ++x) {
                const auto in = at(input, x, y);
                const auto out = at(*output, x, y);
                const auto grain = core::film_grain::noise(
                    (static_cast<float>(x) + 0.5F) * static_cast<float>(1.0 / scalar("size")),
                    (static_cast<float>(y) + 0.5F) * static_cast<float>(1.0 / scalar("size")), stream);
                const std::array amounts{scalar("redAmount"), scalar("greenAmount"),
                                         scalar("blueAmount")};
                for (std::size_t c = 0; c < 3; ++c) {
                    const double expected =
                        static_cast<double>(in[c]) +
                        static_cast<double>(in[3]) * response * scalar("intensity") * amounts[c] *
                            core::film_grain::kNormalise * static_cast<double>(grain[c]);
                    matches = matches && std::abs(static_cast<double>(out[c]) - expected) <
                                             2e-5 * std::max(1.0, std::abs(expected));
                }
                alphaKept = alphaKept && out[3] == in[3];
            }
        expect(matches, "the grain matches its formula at luma " + std::to_string(luma));
        expect(alphaKept, "the grain keeps alpha");
    }

    // Premultiplied: the same straight colour at half coverage takes half the grain.
    {
        const auto opaque = image(frame, {0.4F, 0.3F, 0.2F, 1.0F});
        const auto half = image(frame, {0.4F, 0.3F, 0.2F, 0.5F});
        const auto a = apply(values, opaque);
        const auto b = apply(values, half);
        bool same = true;
        for (std::int64_t x = 0; x < frame.width; ++x)
            for (std::size_t c = 0; c < 3; ++c)
                same = same && std::abs(at(*b, x, 5)[c] - 0.5F * at(*a, x, 5)[c]) < 1e-5F;
        expect(same, "half coverage takes half the premultiplied grain");
        const auto clear = image(frame, {0.0F, 0.0F, 0.0F, 0.0F});
        const auto cleared = apply(values, clear);
        expect(at(*cleared, 10, 10) == std::array{0.0F, 0.0F, 0.0F, 0.0F},
               "a transparent pixel stays transparent black");
    }

    // HDR-safe: nothing clips, and above 1.0 the grain stays the same share of the highlight.
    {
        const auto twice = image(frame, {2.0F, 2.0F, 2.0F, 1.0F});
        const auto eight = image(frame, {8.0F, 8.0F, 8.0F, 1.0F});
        const auto a = apply(values, twice);
        const auto b = apply(values, eight);
        bool above = false;
        bool proportional = true;
        for (std::int64_t x = 0; x < frame.width; ++x) {
            above = above || at(*b, x, 7)[0] > 8.0F;
            proportional = proportional &&
                           std::abs(at(*b, x, 7)[1] / 8.0F - at(*a, x, 7)[1] / 2.0F) < 1e-5F;
        }
        expect(above, "the grain reaches above the highlight, unclipped");
        expect(proportional, "an HDR highlight keeps its share of grain");
    }

    // Each channel's amount is its own.
    {
        GrainValues red;
        red.scalars = {{"intensity", 0.1}, {"redAmount", 0.0}, {"blueAmount", 2.0}};
        const auto input = image(frame, {0.4F, 0.4F, 0.4F, 1.0F});
        const auto output = apply(red, input);
        bool redSame = true;
        std::vector<double> green;
        std::vector<double> blue;
        for (std::int64_t y = 0; y < frame.height; ++y)
            for (std::int64_t x = 0; x < frame.width; ++x) {
                const auto out = at(*output, x, y);
                redSame = redSame && out[0] == 0.4F;
                green.push_back(static_cast<double>(out[1]));
                blue.push_back(static_cast<double>(out[2]));
            }
        expect(redSame, "a channel at amount 0 keeps its value exactly");
        const double ratio = statsOf(blue).deviation / statsOf(green).deviation;
        expect(ratio > 1.7 && ratio < 2.3,
               "a channel at twice the amount has twice the grain, got " + std::to_string(ratio));
    }

    // The response is the node's: no grain at a zero, more at a higher share.
    {
        GrainValues shadows;
        shadows.scalars = {{"shadows", 0.0}};
        const auto black = image(frame, {0.0F, 0.0F, 0.0F, 1.0F});
        const auto output = apply(shadows, black);
        expect(at(*output, 3, 3) == std::array{0.0F, 0.0F, 0.0F, 1.0F},
               "a shadows response of zero leaves black alone");
        GrainValues lifted;
        lifted.scalars = {{"shadows", 1.0}};
        const auto stronger = apply(lifted, black);
        expect(at(*stronger, 3, 3) != std::array{0.0F, 0.0F, 0.0F, 1.0F},
               "a shadows response adds grain to black");
        expect(filmGrainResponse(0.09, 0.0, 1.0, 1.0) == 0.5 &&
                   filmGrainResponse(-1.0, 0.25, 1.0, 1.0) == 0.25 &&
                   std::abs(filmGrainResponse(0.59, 1.0, 1.0, 2.0) - 1.5) < 1e-12 &&
                   filmGrainResponse(4.0, 1.0, 1.0, 0.5) == 2.0,
               "the response runs through its knots and grows above 1.0");
    }

    // The same frame always draws the same grain; another frame or seed another; Held one pattern.
    {
        const auto input = image(frame, {0.4F, 0.4F, 0.4F, 1.0F});
        GrainValues base;
        base.scalars = {{"intensity", 0.1}};
        const auto a = apply(base, input, 5.0);
        const auto again = apply(base, input, 5.0);
        const auto next = apply(base, input, 6.0);
        GrainValues seeded = base;
        seeded.seed = 2;
        const auto other = apply(seeded, input, 5.0);
        std::size_t againSame = 0;
        std::size_t nextSame = 0;
        std::size_t otherSame = 0;
        for (std::int64_t y = 0; y < frame.height; ++y)
            for (std::int64_t x = 0; x < frame.width; ++x) {
                againSame += at(*a, x, y) == at(*again, x, y) ? 1U : 0U;
                nextSame += at(*a, x, y) == at(*next, x, y) ? 1U : 0U;
                otherSame += at(*a, x, y) == at(*other, x, y) ? 1U : 0U;
            }
        const std::size_t total = static_cast<std::size_t>(frame.width) * frame.height;
        expect(againSame == total, "a frame's grain repeats exactly");
        expect(nextSame < total / 20, "the next frame's grain is new");
        expect(otherSame < total / 20, "another seed's grain is new");
    }

    // The size is in full-resolution pixels: neighbouring pixels share a grain only as far as it
    // reaches, and a proxy reads the same grain at the same place, whatever its scale. The window
    // only places the pixels.
    {
        const Frame large{0, 0, 256, 256};
        const auto input = image(large, {0.4F, 0.4F, 0.4F, 1.0F});
        const auto correlation = [&](const double size) {
            GrainValues v;
            v.scalars = {{"intensity", 0.1}, {"size", size}};
            const auto output = apply(v, input);
            double together = 0.0;
            double alone = 0.0;
            for (std::int64_t y = 0; y < large.height; ++y)
                for (std::int64_t x = 0; x + 1 < large.width; ++x) {
                    const double p = static_cast<double>(at(*output, x, y)[0]) - 0.4;
                    const double q = static_cast<double>(at(*output, x + 1, y)[0]) - 0.4;
                    together += p * q;
                    alone += p * p;
                }
            return together / alone;
        };
        const double fine = correlation(0.25);
        const double medium = correlation(1.0);
        const double coarse = correlation(4.0);
        expect(std::abs(fine) < 0.03 && medium > 0.03 && coarse > medium + 0.5,
               "a larger size reaches further: " + std::to_string(fine) + ", " +
                   std::to_string(medium) + ", " + std::to_string(coarse));
        GrainValues v;
        v.scalars = {{"intensity", 0.1}, {"size", 2.0}};
        const auto grade = prepare(v);
        const auto window = render::ImageWindow::create(0, 0, 8, 8);
        const auto proxy = runtime::detail::filmGrainProgramData(*grade, *window.value(), 1.0, 0.5, 0.5);
        const auto full = runtime::detail::filmGrainProgramData(*grade, *window.value(), 1.0, 1.0, 1.0);
        expect((*proxy)[18] == 2.0F * (*full)[18] && (*proxy)[19] == 2.0F * (*full)[19],
               "a half-resolution proxy reads the grain twice as far per pixel");
        const auto wide = runtime::detail::filmGrainProgramData(*grade, *window.value(), 2.0, 1.0, 1.0);
        expect((*wide)[18] == 2.0F * (*full)[18] && (*wide)[19] == (*full)[19],
               "a wide pixel reads the grain further across, so the grain stays round");

        const Frame shifted{-13, 21, 40, 30};
        const auto shiftedInput = image(shifted, {0.4F, 0.4F, 0.4F, 1.0F});
        const auto wholeInput = image(Frame{-13, 21, 64, 64}, {0.4F, 0.4F, 0.4F, 1.0F});
        const auto a = apply(v, shiftedInput);
        const auto b = apply(v, wholeInput);
        bool same = true;
        for (std::int64_t y = 21; y < 51; ++y)
            for (std::int64_t x = -13; x < 27; ++x)
                same = same && at(*a, x, y) == at(*b, x, y);
        expect(same, "a pixel's grain does not depend on the size of its data window");
    }
}

void testEvaluator() {
    const runtime::CpuCompositionEvaluator evaluator;
    GrainValues values = film_grain_test::everyPath();
    const auto plan = film_grain_test::grainPlan(values);
    GrainValues held = values;
    held.animation = document::kFilmGrainHeld;
    const auto heldPlan = film_grain_test::grainPlan(held);
    const auto effectIndex = runtime::OperationIndex::fromRaw(1);
    expect(plan->operationTimeDependent(effectIndex),
           "grain drawn for every frame varies with time");
    expect(!heldPlan->operationTimeDependent(effectIndex), "held grain does not");

    const auto request = [&](const auto& which, const std::int64_t frame) {
        runtime::EvaluationRequest r{*core::RationalTime::create(frame, 24), which->output(),
                                     runtime::CompositionFormatResolution{}};
        r.pixelStorageByteLimit = 1U << 26U;
        return r;
    };
    const auto pixels = [&](const auto& which, const std::int64_t frame) {
        const auto result = evaluator.evaluate(which, request(which, frame), {});
        if (result.frame() == nullptr)
            throw std::logic_error("the plan evaluates");
        const auto view = result.frame()->processImage().pixels();
        return std::vector<render::Rgba32f>(view.begin(), view.end());
    };
    // Through the evaluator's memoization, whatever order the frames are asked in.
    const auto first = pixels(plan, 1);
    const auto second = pixels(plan, 2);
    const auto firstAgain = pixels(plan, 1);
    expect(first != second, "a new frame is new grain, through the operation cache");
    expect(first == firstAgain, "returning to a frame draws its grain again");
    const auto heldFirst = pixels(heldPlan, 1);
    const auto heldSecond = pixels(heldPlan, 2);
    expect(heldFirst == heldSecond, "held grain is one pattern on every frame");

    // A region of interest reads the same grain at the same pixels.
    auto regional = request(plan, 1);
    const auto region = render::ImageWindow::create(20, 12, 16, 12);
    regional.roi = *region.value();
    const auto part = evaluator.evaluate(plan, regional, {});
    const auto whole = evaluator.evaluate(plan, request(plan, 1), {});
    expect(part.frame() != nullptr && whole.frame() != nullptr, "a region of interest evaluates");
    if (part.frame() != nullptr && whole.frame() != nullptr) {
        const auto& crop = part.frame()->processImage();
        const auto& frame = whole.frame()->processImage();
        bool same = true;
        const auto window = crop.descriptor()->dataWindow();
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
            for (auto x = window.originX(); x < window.maxXExclusive(); ++x)
                same = same && at(crop, x, y) == at(frame, x, y);
        expect(same, "a region's grain is the whole frame's");
    }

    // A proxy has the same strength of grain as the full frame.
    auto proxied = request(plan, 1);
    const auto extent = render::ImageExtent::create(32, 24);
    proxied.resolution = runtime::ProxyResolution{*extent.value()};
    expect(evaluator.evaluate(plan, proxied, {}).frame() != nullptr, "a proxy evaluates");
}

void testPrepared() {
    using runtime::detail::buildColorGradeProgram;
    using runtime::detail::ColorGradeKind;
    GrainValues base;
    auto grade = prepare(base);
    expect(grade && grade->kind == ColorGradeKind::FilmGrain && !grade->identity,
           "default grain is a grade kind with work to do");
    GrainValues off;
    off.scalars = {{"intensity", 0.0}};
    expect(prepare(off)->identity, "no intensity is an identity");
    GrainValues noChannels;
    noChannels.scalars = {{"redAmount", 0.0}, {"greenAmount", 0.0}, {"blueAmount", 0.0}};
    expect(prepare(noChannels)->identity, "no channel is an identity");
    GrainValues noResponse;
    noResponse.scalars = {{"shadows", 0.0}, {"midtones", 0.0}, {"highlights", 0.0}};
    expect(prepare(noResponse)->identity, "no response is an identity");

    for (const auto& [role, value] : {std::pair{std::string("intensity"), 1.5},
                                      std::pair{std::string("intensity"), -0.1},
                                      std::pair{std::string("size"), 0.1},
                                      std::pair{std::string("size"), 64.0},
                                      std::pair{std::string("redAmount"), -1.0},
                                      std::pair{std::string("shadows"), 20.0}}) {
        GrainValues bad;
        bad.scalars = {{role, value}};
        expect(!prepare(bad).has_value(), "a value outside its domain is refused: " + role);
    }
    GrainValues nan;
    nan.scalars = {{"intensity", std::numeric_limits<double>::quiet_NaN()}};
    expect(!prepare(nan).has_value(), "a non-finite value is refused");
    GrainValues seed;
    seed.seed = -1;
    expect(!prepare(seed).has_value(), "a seed outside its domain is refused");
    GrainValues animation;
    animation.animation = 2;
    expect(!prepare(animation).has_value(), "an animation outside its domain is refused");
    expect(!runtime::detail::prepareColorGrade(
                grainKernel(base, 100), std::vector<double>(document::kFilmGrainScalarCount, 1.0))
                .has_value(),
           "operands without the frame are refused");

    const auto window = render::ImageWindow::create(-4, -3, 32, 24);
    const auto a = runtime::detail::filmGrainProgramData(*grade, *window.value(), 1.0, 1.0, 1.0);
    expect(a && runtime::detail::validFilmGrainProgramData(*a), "the program data is valid");
    const auto otherGrade = prepare(base, 9.0);
    const auto b = runtime::detail::filmGrainProgramData(*otherGrade, *window.value(), 1.0, 1.0, 1.0);
    if (a && b) {
        const auto first = buildColorGradeProgram(ColorGradeKind::FilmGrain, *a);
        const auto second = buildColorGradeProgram(ColorGradeKind::FilmGrain, *b);
        expect(first.succeeded() && second.succeeded() &&
                   first.program()->shaderText == second.program()->shaderText &&
                   first.program()->uniformBufferData != second.program()->uniformBufferData,
               "one fixed program reads every frame's grain as data");
        expect(!buildColorGradeProgram(ColorGradeKind::FilmGrain, grade->data).succeeded(),
               "a table without its geometry is refused by the program");
        auto broken = *a;
        broken[18] = 0.0F;
        expect(!buildColorGradeProgram(ColorGradeKind::FilmGrain, broken).succeeded(),
               "a degenerate geometry is refused by the program");
    }
    expect(runtime::detail::colorGradeIdentity(*grade) !=
               runtime::detail::colorGradeIdentity(*otherGrade),
           "another frame has another cache identity");
    expect(!runtime::detail::filmGrainProgramData(*grade, *window.value(), 1.0, 0.0, 1.0) &&
               !runtime::detail::filmGrainProgramData(*grade, *window.value(), 0.0, 1.0, 1.0),
           "a non-positive scale or pixel aspect is refused");
    const auto far = render::ImageWindow::create(std::int64_t{1} << 40, 0, 8, 8);
    expect(far && !runtime::detail::filmGrainProgramData(*grade, *far.value(), 1.0, 1.0, 1.0),
           "a position beyond the grain's coordinates is refused");
    std::array<float, 4> pixel{0.5F, 0.5F, 0.5F, 1.0F};
    expect(!runtime::detail::colorGradePixel(*grade, pixel),
           "grain is never applied without its position");
}

void testBudgetAndCancellation() {
    const auto input = image({}, {0.4F, 0.4F, 0.4F, 1.0F});
    const auto grade = prepare({});
    if (!grade)
        throw std::logic_error("the grain prepares");
    auto refused = runtime::detail::applyFilmGrain(*grade, input, 1.0, 1.0, 16U, {},
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
        runtime::TaskRequest("Film Grain cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result = runtime::detail::applyFilmGrain(
                *grade, input, 1.0, 1.0, 1U << 26U, context.cancellation(),
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
           "cancelled grain publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(scheduler.isQuiescent(), "the cancelled grain shuts down safely");
}

} // namespace

int main() {
    try {
        testNoise();
        testPixels();
        testEvaluator();
        testPrepared();
        testBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "film grain tests passed\n";
    return 0;
}
