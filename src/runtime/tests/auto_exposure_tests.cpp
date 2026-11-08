// #566 Auto Exposure on the CPU reference: the mean reduction by tile sums against an independent
// mean, on every grid shape; the recursive filter against an independent double-precision reading of
// the specification (steady, attack, release, limits, amounts, no lag, absent means); the gain pass;
// domains, the one fixed program, the pixel budget, cancellation and band independence; and the
// look-back window through the evaluator -- that a frame renders the same on its own, after the frames
// before it and reached backwards, that it follows the footage with the lag asked for, that its
// reductions are shared between frames by cache identity, and that a refused budget is an explicit
// error.

#include "auto_exposure_image_effect.hpp"
#include "auto_exposure_recipes.hpp"
#include "auto_exposure_test_support.hpp"
#include "color_grade_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"
#include "look_back_recipes.hpp"

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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
using exposure_test::ExposureValues;

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

// A scene with structure in every channel and a varying alpha.
Pixel scene(const std::int64_t x, const std::int64_t y) {
    const auto u = static_cast<float>(x);
    const auto v = static_cast<float>(y);
    return {0.05F + 0.9F * std::fabs(std::sin(0.31F * u + 0.17F * v)),
            0.02F + 2.5F * std::fabs(std::cos(0.13F * u - 0.29F * v)),
            0.4F + 0.3F * std::sin(0.07F * u * v), (x + y) % 5 == 0 ? 0.5F : 1.0F};
}

runtime::AutoExposureKernel kernelOf(const runtime::AutoExposureRole role,
                                     const std::int64_t grid = 1) {
    runtime::AutoExposureKernel kernel;
    kernel.role = role;
    kernel.grid = grid;
    return kernel;
}

std::vector<double> filterValues(const ExposureValues& v, const double frame = 1.0 / 24.0) {
    return {v.exposure, v.target,   v.attack,       v.release,     v.minGain,
            v.maxGain,  v.whiteBalance, v.balanceLag, v.balanceLimit, frame};
}

std::optional<detail::PreparedColorGrade> prepareFilter(const ExposureValues& v) {
    return detail::prepareColorGrade(kernelOf(runtime::AutoExposureRole::Filter), filterValues(v));
}
std::optional<detail::PreparedColorGrade> prepareRole(const runtime::AutoExposureRole role,
                                                      const std::int64_t grid = 1) {
    return detail::prepareColorGrade(kernelOf(role, grid), std::span<const double>{});
}

std::shared_ptr<const render::Rgba32fImage>
apply(const detail::PreparedColorGrade& grade, const render::Rgba32fImage* input,
      std::vector<const render::Rgba32fImage*> images = {},
      runtime::CpuRowBandExecutor* bands = nullptr) {
    const auto result = detail::applyAutoExposure(
        grade, input, std::span<const render::Rgba32fImage* const>(images), 1U << 26U, {},
        runtime::OperationIndex::fromRaw(0), {}, bands);
    if (!result.image)
        throw std::logic_error("the pass applies");
    return result.image;
}

// The mean of the premultiplied colour of `source`, in double precision.
std::array<double, 3> meanOf(const render::Rgba32fImage& source) {
    std::array<double, 3> sum{};
    const auto window = source.descriptor()->dataWindow();
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
        for (auto x = window.originX(); x < window.maxXExclusive(); ++x) {
            const auto p = at(source, x, y);
            for (std::size_t c = 0; c < 3; ++c)
                sum[c] += static_cast<double>(p[c]);
        }
    const auto count = static_cast<double>(window.extent().width()) *
                       static_cast<double>(window.extent().height());
    for (auto& value : sum)
        value /= count;
    return sum;
}

std::shared_ptr<const render::Rgba32fImage> reduced(const render::Rgba32fImage& source) {
    const auto tiles = prepareRole(runtime::AutoExposureRole::Reduce, 64);
    const auto last = prepareRole(runtime::AutoExposureRole::Reduce, 1);
    if (!tiles || !last)
        throw std::logic_error("the reductions prepare");
    const auto first = apply(*tiles, &source);
    return apply(*last, first.get());
}

// --- The reduction -----------------------------------------------------------------------------

void testReduce() {
    struct Shape final {
        std::int64_t ox, oy;
        std::uint32_t w, h;
    };
    for (const auto shape : {Shape{0, 0, 1, 1}, Shape{3, -2, 7, 5}, Shape{0, 0, 64, 64},
                             Shape{-5, 9, 65, 3}, Shape{10, 10, 130, 77}, Shape{0, 0, 1, 200}}) {
        const auto source = image(shape.ox, shape.oy, shape.w, shape.h, scene);
        const auto tiles = prepareRole(runtime::AutoExposureRole::Reduce, 64);
        const auto first = apply(*tiles, &source);
        const auto geometry = detail::autoExposureGrid(shape.w, shape.h, 64);
        const auto window = first->descriptor()->dataWindow();
        expect(static_cast<std::int64_t>(window.extent().width()) == geometry.width &&
                   static_cast<std::int64_t>(window.extent().height()) == geometry.height &&
                   window.originX() == shape.ox && window.originY() == shape.oy,
               "the tile sums are a grid at the frame's origin, " + std::to_string(shape.w) + "x" +
                   std::to_string(shape.h));
        expect(geometry.width <= 64 && geometry.height <= 64 && geometry.tileWidth >= 1 &&
                   (geometry.width - 1) * geometry.tileWidth < static_cast<std::int64_t>(shape.w) &&
                   geometry.width * geometry.tileWidth >= static_cast<std::int64_t>(shape.w),
               "the grid covers the frame in whole tiles, never more than 64 across");
        const auto last = reduced(source);
        const auto lastWindow = last->descriptor()->dataWindow();
        expect(lastWindow.extent().width() == 1 && lastWindow.extent().height() == 1 &&
                   lastWindow.originX() == shape.ox && lastWindow.originY() == shape.oy,
               "the frame's mean is one pixel at its origin");
        const auto expected = meanOf(source);
        const auto got = at(*last, shape.ox, shape.oy);
        bool close = got[3] == 1.0F;
        for (std::size_t c = 0; c < 3; ++c)
            close = close && std::abs(static_cast<double>(got[c]) - expected[c]) <=
                                 2e-5 * std::max(expected[c], 1.0);
        expect(close, "the reduction is the mean of the premultiplied colour, " +
                          std::to_string(shape.w) + "x" + std::to_string(shape.h));
    }
    // A mean does not depend on where the frame lies.
    {
        const auto a = image(0, 0, 40, 30, scene);
        const auto b = image(-17, 23, 40, 30, [](auto x, auto y) { return scene(x + 17, y - 23); });
        expect(at(*reduced(a), 0, 0) == at(*reduced(b), -17, 23),
               "a frame's mean is the same wherever its window lies");
    }
    // The grid is bounded however large the frame is.
    {
        const auto big = detail::autoExposureGrid(7680, 4320, 64);
        expect(big.width <= 64 && big.height <= 64, "an 8K frame still reduces to at most 64 by 64");
    }
}

// --- The filter --------------------------------------------------------------------------------

std::shared_ptr<const render::Rgba32fImage> meanImage(const std::array<float, 3>& rgb,
                                                      const std::int64_t x = 0,
                                                      const std::int64_t y = 0) {
    return std::make_shared<const render::Rgba32fImage>(
        image(x, y, 1, 1, [&](auto, auto) { return Pixel{rgb[0], rgb[1], rgb[2], 1.0F}; }));
}

// The Filter over `means` oldest first through the pass, and the independent reading's gains.
std::array<float, 3> filtered(const ExposureValues& v,
                              const std::vector<std::optional<std::array<float, 3>>>& means) {
    const auto grade = prepareFilter(v);
    if (!grade)
        throw std::logic_error("the filter prepares");
    std::vector<std::shared_ptr<const render::Rgba32fImage>> owned;
    std::vector<const render::Rgba32fImage*> older;
    for (std::size_t i = 0; i + 1 < means.size(); ++i) {
        owned.push_back(means[i] ? meanImage(*means[i]) : nullptr);
        older.push_back(owned.back().get());
    }
    const auto newest = meanImage(*means.back());
    const auto out = apply(*grade, newest.get(), older);
    const auto p = at(*out, 0, 0);
    return {p[0], p[1], p[2]};
}

std::array<double, 3> reference(const ExposureValues& v,
                                const std::vector<std::array<double, 3>>& means) {
    // exposure_test::expectedGain reads two flat colours; this is the same recursion over any means.
    const double dt = 1.0 / 24.0;
    const auto coefficient = [dt](const double seconds) {
        return seconds > 0.0 ? 1.0 - std::exp(-dt / seconds) : 1.0;
    };
    std::array<double, 3> state{};
    std::array<double, 3> seed{};
    std::array<double, 3> weight{1.0, 1.0, 1.0};
    bool have = false;
    for (const auto& m : means) {
        const double r = std::max(m[0], 1e-6);
        const double g = std::max(m[1], 1e-6);
        const double b = std::max(m[2], 1e-6);
        const double wr = std::clamp(std::log2(g / r), -v.balanceLimit, v.balanceLimit) * v.whiteBalance;
        const double wb = std::clamp(std::log2(g / b), -v.balanceLimit, v.balanceLimit) * v.whiteBalance;
        const double luma = 0.2126 * r * std::exp2(wr) + 0.7152 * g + 0.0722 * b * std::exp2(wb);
        const double e =
            std::clamp(std::log2(v.target) - std::log2(std::max(luma, 1e-6)), v.minGain, v.maxGain) *
            v.exposure;
        const std::array<double, 3> desired{e, wr, wb};
        if (!have) {
            state = desired;
            seed = desired;
            have = true;
            continue;
        }
        const double ke = coefficient(desired[0] < state[0] ? v.attack : v.release);
        const double kb = coefficient(v.balanceLag);
        const std::array<double, 3> k{ke, kb, kb};
        for (std::size_t i = 0; i < 3; ++i) {
            state[i] += k[i] * (desired[i] - state[i]);
            weight[i] *= 1.0 - k[i];
        }
    }
    for (std::size_t i = 0; i < 3; ++i)
        if (weight[i] < 0.99609375)
            state[i] = (state[i] - weight[i] * seed[i]) / (1.0 - weight[i]);
    return state;
}

bool near(const std::array<float, 3>& got, const std::array<double, 3>& want, const double tolerance) {
    for (std::size_t c = 0; c < 3; ++c)
        if (std::abs(static_cast<double>(got[c]) - want[c]) > tolerance)
            return false;
    return true;
}

void testFilter() {
    const ExposureValues defaults;
    const auto grey = [](const float v) { return std::array<float, 3>{v, v, v}; };
    const auto greyd = [](const double v) { return std::array<double, 3>{v, v, v}; };

    // A steady mean asks for its gain at once, whatever the lag: 0.09 is one stop under the target.
    {
        const auto gains = filtered(defaults, {grey(0.09F), grey(0.09F), grey(0.09F)});
        expect(near(gains, {1.0, 0.0, 0.0}, 1e-5), "a steady dark frame is brought up one stop");
        expect(near(filtered(defaults, {grey(0.18F), grey(0.18F)}), {0.0, 0.0, 0.0}, 1e-5),
               "a frame at the target is left alone");
    }
    // A step to a brighter frame: the gain falls with the attack; to a darker one it rises with the
    // release. Against the independent recursion.
    {
        std::vector<std::optional<std::array<float, 3>>> bright;
        std::vector<std::array<double, 3>> brightD;
        for (int i = 0; i < 12; ++i) {
            bright.push_back(grey(i < 6 ? 0.18F : 0.72F));
            brightD.push_back(greyd(i < 6 ? 0.18 : 0.72));
        }
        expect(near(filtered(defaults, bright), reference(defaults, brightD), 2e-5),
               "a brightening step follows the attack");
        std::vector<std::optional<std::array<float, 3>>> dark;
        std::vector<std::array<double, 3>> darkD;
        for (int i = 0; i < 12; ++i) {
            dark.push_back(grey(i < 6 ? 0.72F : 0.18F));
            darkD.push_back(greyd(i < 6 ? 0.72 : 0.18));
        }
        expect(near(filtered(defaults, dark), reference(defaults, darkD), 2e-5),
               "a darkening step follows the release");
        // A short attack and a long release: the new gain is reached in the frames given, or not.
        ExposureValues quick = defaults;
        quick.attack = 0.05;
        quick.release = 5.0;
        const auto fallen = filtered(quick, bright)[0];
        const auto risen = filtered(quick, dark)[0];
        expect(fallen < -1.9F && risen < 0.2F,
               "a short attack reaches the new gain in the frames given and a long release does not");
    }
    // No lag follows the newest frame at once.
    {
        ExposureValues instant = defaults;
        instant.attack = 0.0;
        instant.release = 0.0;
        instant.balanceLag = 0.0;
        const auto gains = filtered(instant, {grey(0.18F), grey(0.72F), std::array<float, 3>{0.3F, 0.2F, 0.1F}});
        expect(near(gains, reference(instant, {greyd(0.72), {0.3, 0.2, 0.1}}), 2e-5),
               "no lag is the newest frame's own gain");
    }
    // Limits: black is lifted to the maximum, a blazing frame cut to the minimum.
    {
        expect(near(filtered(defaults, {grey(0.0F), grey(0.0F)}), {3.0, 0.0, 0.0}, 1e-5),
               "a black frame is lifted by the most the gain may rise");
        expect(near(filtered(defaults, {grey(500.0F), grey(500.0F)}), {-3.0, 0.0, 0.0}, 1e-5),
               "a blazing frame is cut by the most the gain may fall");
        ExposureValues narrow = defaults;
        narrow.maxGain = 0.5;
        narrow.minGain = -0.25;
        expect(near(filtered(narrow, {grey(0.01F), grey(0.01F)}), {0.5, 0.0, 0.0}, 1e-5),
               "the limits are the node's");
    }
    // White balance: a warm frame is pulled to neutral, within the limit, and 0% leaves it alone.
    {
        const std::array<float, 3> warm{0.3F, 0.2F, 0.1F};
        const auto gains = filtered(defaults, {warm, warm});
        expect(near(gains, reference(defaults, {{0.3, 0.2, 0.1}}), 2e-5) && gains[1] < -0.5F &&
                   std::abs(gains[2] - 1.0F) < 1e-5F,
               "a warm frame loses red and gains blue, the blue by the limit");
        ExposureValues off = defaults;
        off.whiteBalance = 0.0;
        const auto flat = filtered(off, {warm, warm});
        expect(flat[1] == 0.0F && flat[2] == 0.0F, "no white balance moves no channel");
        ExposureValues half = defaults;
        half.whiteBalance = 0.5;
        expect(near(filtered(half, {warm, warm}), reference(half, {{0.3, 0.2, 0.1}}), 2e-5),
               "half the white balance is half the correction");
        ExposureValues none = defaults;
        none.exposure = 0.0;
        none.whiteBalance = 0.0;
        expect(near(filtered(none, {warm, warm}), {0.0, 0.0, 0.0}, 0.0),
               "no exposure and no white balance is a gain of exactly nothing");
    }
    // An absent mean holds the filter: it is skipped, not taken for black.
    {
        const auto a = filtered(defaults, {grey(0.09F), std::nullopt, std::nullopt, grey(0.09F)});
        const auto b = filtered(defaults, {grey(0.09F), grey(0.09F)});
        expect(a == b, "absent means hold the filter");
        const auto first = filtered(defaults, {std::nullopt, grey(0.09F), grey(0.09F)});
        expect(near(first, {1.0, 0.0, 0.0}, 1e-5), "a window that begins absent starts at its first mean");
    }
    // The oldest mean's remaining share is taken out of the state, so a constant input is returned as
    // it is, whatever the lags, and the lag of one frame still follows the newest frame exactly.
    {
        const std::array<float, 3> warmDim{0.12F, 0.06F, 0.03F};
        std::vector<std::optional<std::array<float, 3>>> constant(24, warmDim);
        expect(near(filtered(defaults, constant), reference(defaults, std::vector<std::array<double, 3>>(
                                                                         24, {0.12, 0.06, 0.03})),
                    2e-5),
               "a constant input returns its own gain");
        ExposureValues slow = defaults;
        slow.attack = 2.0;
        slow.release = 2.0;
        slow.balanceLag = 3.0;
        expect(near(filtered(slow, constant), reference(slow, std::vector<std::array<double, 3>>(
                                                                  24, {0.12, 0.06, 0.03})),
                    2e-5),
               "a constant input returns its own gain under a long lag");
        // The oldest mean leaving the window no longer moves the result by its whole remaining share:
        // a window of a change and one that has just lost the sample before it differ by little.
        std::vector<std::optional<std::array<float, 3>>> held(24, grey(0.18F));
        for (std::size_t i = 12; i < held.size(); ++i)
            held[i] = grey(0.72F);
        const auto whole = filtered(defaults, held);
        auto shorter = held;
        shorter.erase(shorter.begin());
        shorter.push_back(grey(0.72F));
        std::vector<std::array<double, 3>> heldD(12, greyd(0.18));
        heldD.insert(heldD.end(), 12, greyd(0.72));
        expect(near(whole, reference(defaults, heldD), 2e-5),
               "a window holding a change follows the independent recursion");
        expect(std::abs(filtered(defaults, shorter)[0] - whole[0]) < 0.25F,
               "sliding the window by one frame moves the gain by a small step");
    }
    // A lag far longer than the window leaves the seed nearly all of the state: the share cannot be
    // separated, so the state itself (no division by the vanishing remainder) is the result. The same
    // for a step of no length, whose coefficient is zero.
    {
        ExposureValues forever = defaults;
        forever.attack = 30.0;
        forever.release = 30.0;
        forever.balanceLag = 30.0;
        const auto got = filtered(forever, {grey(0.09F), grey(0.72F), grey(0.72F)});
        expect(std::isfinite(got[0]) && std::isfinite(got[1]) && std::isfinite(got[2]) &&
                   std::abs(got[0] - 1.0F) < 0.05F,
               "a lag far beyond the window stays on the oldest mean, finite and defined");
        const auto grade = detail::prepareColorGrade(kernelOf(runtime::AutoExposureRole::Filter),
                                                     filterValues(defaults, 1e-12));
        expect(grade.has_value(), "a frame of almost no length prepares");
        if (grade) {
            const auto newest = meanImage(grey(0.72F));
            const auto oldest = meanImage(grey(0.09F));
            const auto out = apply(*grade, newest.get(), {oldest.get()});
            const auto p = at(*out, 0, 0);
            expect(std::isfinite(p[0]) && std::abs(p[0] - 1.0F) < 1e-3F && p[1] == 0.0F && p[2] == 0.0F,
                   "a step of no length leaves the state on the oldest mean");
        }
    }
    // Domains.
    {
        auto bad = [&](const std::function<void(ExposureValues&)>& edit, const char* what) {
            ExposureValues v;
            edit(v);
            expect(!prepareFilter(v), what);
        };
        bad([](ExposureValues& v) { v.target = 0.0; }, "a target of zero is refused");
        bad([](ExposureValues& v) { v.maxGain = -1.0; }, "a negative maximum gain is refused");
        bad([](ExposureValues& v) { v.minGain = 1.0; }, "a positive minimum gain is refused");
        bad([](ExposureValues& v) { v.attack = -0.1; }, "a negative attack is refused");
        bad([](ExposureValues& v) { v.exposure = 1.5; }, "an exposure over 100% is refused");
        bad([](ExposureValues& v) { v.target = std::nan(""); }, "a non-finite target is refused");
        expect(!detail::prepareColorGrade(kernelOf(runtime::AutoExposureRole::Filter),
                                          std::vector<double>{1.0}),
               "a filter without its values is refused");
        expect(!detail::prepareColorGrade(kernelOf(runtime::AutoExposureRole::Apply),
                                          std::vector<double>{1.0}),
               "an application given values is refused");
        expect(prepareFilter(ExposureValues{}).has_value(), "the defaults prepare");
        expect(!detail::prepareColorGrade(kernelOf(runtime::AutoExposureRole::Filter),
                                          filterValues(defaults, 0.0)),
               "a frame of no length is refused");
    }
}

// --- The gain ----------------------------------------------------------------------------------

void testApply() {
    const auto source = image(4, 2, 9, 7, scene);
    const auto gain = std::make_shared<const render::Rgba32fImage>(
        image(0, 0, 1, 1, [](auto, auto) { return Pixel{1.0F, 0.5F, -0.5F, 1.0F}; }));
    const auto grade = prepareRole(runtime::AutoExposureRole::Apply);
    const auto out = apply(*grade, &source, {gain.get()});
    expect(out->descriptor()->dataWindow() == source.descriptor()->dataWindow(),
           "the gain keeps the frame's window");
    bool exact = true;
    for (std::int64_t y = 2; y < 9; ++y)
        for (std::int64_t x = 4; x < 13; ++x) {
            const auto in = at(source, x, y);
            const auto got = at(*out, x, y);
            const std::array<float, 3> gains{std::exp2(1.5F), std::exp2(1.0F), std::exp2(0.5F)};
            for (std::size_t c = 0; c < 3; ++c)
                exact = exact && std::abs(got[c] - in[c] * gains[c]) <= 1e-6F * std::max(1.0F, got[c]);
            exact = exact && got[3] == in[3];
        }
    expect(exact, "the gain multiplies the premultiplied colour by its stops and keeps the alpha");
    // No gain pixel: the frame as it is, bit for bit.
    const auto same = apply(*grade, &source, std::vector<const render::Rgba32fImage*>{nullptr});
    bool identical = true;
    for (std::int64_t y = 2; y < 9; ++y)
        for (std::int64_t x = 4; x < 13; ++x)
            identical = identical && at(*same, x, y) == at(source, x, y);
    expect(identical, "no gain pixel leaves the frame exactly as it is");
    // No gain at all (zero stops) is exact.
    const auto zero = std::make_shared<const render::Rgba32fImage>(
        image(0, 0, 1, 1, [](auto, auto) { return Pixel{0.0F, 0.0F, 0.0F, 1.0F}; }));
    const auto unchanged = apply(*grade, &source, {zero.get()});
    identical = true;
    for (std::int64_t y = 2; y < 9; ++y)
        for (std::int64_t x = 4; x < 13; ++x)
            identical = identical && at(*unchanged, x, y) == at(source, x, y);
    expect(identical, "a gain of zero stops is exact");
    // No frame, no image.
    const auto none = detail::applyAutoExposure(*grade, nullptr, {}, 1U << 26U, {},
                                                runtime::OperationIndex::fromRaw(0), {}, nullptr);
    expect(none.empty && !none.image, "no frame has no gain");
}

// --- The program -------------------------------------------------------------------------------

void testProgram() {
    const auto reduce = prepareRole(runtime::AutoExposureRole::Reduce, 64);
    detail::AutoExposurePlacement placement;
    placement.sourceWidth = 130;
    placement.sourceHeight = 77;
    const auto data = detail::autoExposureProgramData(*reduce, placement);
    expect(data && data->size() == detail::kAutoExposureProgramFloats &&
               detail::validAutoExposureProgramData(*data),
           "a reduction's table is the fixed size and valid");
    if (!data)
        return;
    const auto geometry = detail::autoExposureGrid(130, 77, 64);
    expect((*data)[14] == static_cast<float>(geometry.tileWidth) &&
               (*data)[15] == static_cast<float>(geometry.tileHeight) &&
               (*data)[16] == static_cast<float>(geometry.width) &&
               (*data)[17] == static_cast<float>(geometry.height),
           "the table carries the tile and the grid");
    const auto program = buildColorGradeProgram(detail::ColorGradeKind::AutoExposure, *data);
    expect(program.succeeded(), "the Auto Exposure program builds");
    // One fixed program: every setting and pass is data.
    const auto filter = prepareFilter(ExposureValues{});
    ExposureValues other;
    other.target = 0.5;
    other.attack = 0.0;
    const auto otherFilter = prepareFilter(other);
    detail::AutoExposurePlacement cells;
    cells.cells = {{1, 0}, {-1, -1}, {0, 0}};
    const auto filterData = detail::autoExposureProgramData(*filter, cells);
    const auto otherData = detail::autoExposureProgramData(*otherFilter, cells);
    expect(filterData && otherData && detail::validAutoExposureProgramData(*filterData) &&
               detail::validAutoExposureProgramData(*otherData),
           "a filter's table is valid");
    if (filterData && otherData) {
        const auto a = buildColorGradeProgram(detail::ColorGradeKind::AutoExposure, *filterData);
        const auto b = buildColorGradeProgram(detail::ColorGradeKind::AutoExposure, *otherData);
        expect(a.succeeded() && b.succeeded() && program.succeeded() &&
                   a.program()->shaderText == b.program()->shaderText &&
                   a.program()->shaderText == program.program()->shaderText &&
                   a.program()->uniformBufferData != b.program()->uniformBufferData,
               "every setting and pass is data of one program");
        expect((*filterData)[11] == 3.0F && (*filterData)[32] == 1.0F && (*filterData)[34] == -1.0F,
               "the cells of the window are in the table, oldest first");
    }
    auto broken = *data;
    broken.pop_back();
    expect(!detail::validAutoExposureProgramData(broken) &&
               !buildColorGradeProgram(detail::ColorGradeKind::AutoExposure, broken).succeeded(),
           "a table of the wrong size is refused");
    broken = *data;
    broken[0] = 3.0F;
    expect(!detail::validAutoExposureProgramData(broken), "a pass that is not 0..2 is refused");
    broken = *data;
    broken[40] = std::numeric_limits<float>::infinity();
    expect(!detail::validAutoExposureProgramData(broken), "a non-finite table is refused");
    detail::AutoExposurePlacement empty;
    expect(!detail::autoExposureProgramData(*reduce, empty), "no source has no table");
    detail::AutoExposurePlacement tooMany;
    tooMany.cells.assign(detail::kAutoExposureMaxCells + 1, {0, 0});
    expect(!detail::autoExposureProgramData(*filter, tooMany), "a window over the bound has no table");
    expect(detail::kAutoExposureMaxCells >= static_cast<std::size_t>(document::kMaximumAutoExposureLookBack),
           "the table holds the node's longest look-back");
}

void testBudgetCancellationAndBands() {
    const auto source = image(0, 0, 150, 90, scene);
    const auto tiles = prepareRole(runtime::AutoExposureRole::Reduce, 64);
    const auto gainGrade = prepareRole(runtime::AutoExposureRole::Apply);
    const auto gain = std::make_shared<const render::Rgba32fImage>(
        image(0, 0, 1, 1, [](auto, auto) { return Pixel{0.5F, 0.0F, 0.0F, 1.0F}; }));
    const auto refused = detail::applyAutoExposure(*gainGrade, &source, std::array{gain.get()},
                                                   16U, {}, runtime::OperationIndex::fromRaw(0), {},
                                                   nullptr);
    expect(refused.diagnostic &&
               refused.diagnostic->code == runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "the pixel budget refuses the gain's output");
    const auto refusedTiles = detail::applyAutoExposure(*tiles, &source, {}, 16U, {},
                                                        runtime::OperationIndex::fromRaw(0), {},
                                                        nullptr);
    expect(refusedTiles.diagnostic &&
               refusedTiles.diagnostic->code ==
                   runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "and the reduction's");
    // Bands of any width give the same pixels.
    runtime::CpuRowBandExecutor narrow(1);
    runtime::CpuRowBandExecutor wide(6);
    const auto serial = apply(*tiles, &source);
    const auto narrowed = apply(*tiles, &source, {}, &narrow);
    const auto widened = apply(*tiles, &source, {}, &wide);
    bool same = true;
    const auto window = serial->descriptor()->dataWindow();
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
        for (auto x = window.originX(); x < window.maxXExclusive(); ++x)
            same = same && at(*serial, x, y) == at(*narrowed, x, y) &&
                   at(*serial, x, y) == at(*widened, x, y);
    expect(same, "the row bands do not change the sums");
    const auto gained = apply(*gainGrade, &source, {gain.get()});
    const auto gainedWide = apply(*gainGrade, &source, {gain.get()}, &wide);
    same = true;
    for (std::int64_t y = 0; y < 90; ++y)
        for (std::int64_t x = 0; x < 150; ++x)
            same = same && at(*gained, x, y) == at(*gainedWide, x, y);
    expect(same, "nor the gain's pixels");

    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(566)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Auto Exposure cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result = detail::applyAutoExposure(
                *tiles, &source, {}, 1U << 26U, context.cancellation(),
                runtime::OperationIndex::fromRaw(0), {}, nullptr);
            const auto gained = detail::applyAutoExposure(
                *gainGrade, &source, std::array{gain.get()}, 1U << 26U, context.cancellation(),
                runtime::OperationIndex::fromRaw(0), {}, nullptr);
            cancelled = result.cancelled && !result.image && gained.cancelled && !gained.image;
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

// --- The look-back window ----------------------------------------------------------------------

struct Rendered final {
    std::array<float, 4> pixel{};
    runtime::OperationCacheStatistics statistics;
    bool evaluated = false;
    std::vector<runtime::EvaluationDiagnostic> diagnostics;
};

Rendered frameOf(const runtime::CpuCompositionEvaluator& evaluator,
                 const exposure_test::ExposurePlan& which, const std::int64_t frame,
                 const std::size_t limit = 1U << 28U) {
    runtime::EvaluationRequest request{*core::RationalTime::create(frame, 24), which.plan->output(),
                                       runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = limit;
    Rendered rendered;
    const auto result = evaluator.evaluate(which.plan, request, {}, {}, nullptr, &rendered.statistics);
    rendered.diagnostics = result.diagnostics();
    if (result.frame() == nullptr)
        return rendered;
    rendered.evaluated = true;
    rendered.pixel = at(result.frame()->processImage(), 6, 5);
    return rendered;
}

bool closeTo(const std::array<float, 4>& got, const std::array<double, 3>& want, const double relative) {
    for (std::size_t c = 0; c < 3; ++c)
        if (std::abs(static_cast<double>(got[c]) - want[c]) > relative * std::max(want[c], 0.05))
            return false;
    return true;
}

void testWindow() {
    // The window is bounded, oldest first, ends on the input and shares nothing it is not given.
    {
        runtime::CompiledCompositionPlanDefinition definition{
            document::Revision::fromRaw(1), document::ProjectId::fromRaw(1),
            document::CompositionId::fromRaw(1), *document::CompositionFormat::create(4, 4), {},
            runtime::OperationIndex::fromRaw(0)};
        detail::RecipeEmitter emitter(document::NodeId::fromRaw(2), definition.operations,
                                      detail::definitionValueSink(definition));
        definition.operations.emplace_back(runtime::CompiledSolid{});
        const auto input = runtime::OperationIndex::fromRaw(0);
        const auto samples = detail::appendLookBackSamples(emitter, input, 5);
        expect(samples.size() == 5 && samples.back() == input,
               "a look-back window of five frames is four samples and the input");
        bool lags = true;
        for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
            const auto* effect = std::get_if<runtime::CompiledImageEffect>(
                &definition.operations[samples[i].value()]);
            const auto* sample = effect ? std::get_if<runtime::TimeSampleKernel>(&effect->kernel) : nullptr;
            lags = lags && sample && sample->lookBack &&
                   sample->lagFrames == static_cast<std::int64_t>(samples.size() - 1 - i);
        }
        expect(lags, "each sample lags by its distance from the current frame, oldest first");
        expect(detail::appendLookBackSamples(emitter, input, 0).empty() &&
                   detail::appendLookBackSamples(emitter, input, detail::kMaximumLookBackFrames + 1)
                       .empty() &&
                   detail::appendLookBackSamples(emitter, input, detail::kMaximumLookBackFrames).size() ==
                       static_cast<std::size_t>(detail::kMaximumLookBackFrames),
               "a window over the bound is refused, and the longest is allowed");
        detail::AutoExposureOperands operands;
        operands.lookBackFrames = 1;
        expect(!detail::appendAutoExposure(emitter, input, operands),
               "a window with no earlier frame has nothing to filter");
        operands.lookBackFrames = document::kMaximumAutoExposureLookBack + 1;
        expect(!detail::appendAutoExposure(emitter, input, operands),
               "a look-back over the node's bound does not lower");
    }
    // The sample of a frame never reads before the first.
    {
        runtime::TimeSampleKernel sample;
        sample.lookBack = true;
        sample.lagFrames = 10;
        const auto rate = document::FrameRate::framesPerSecond24();
        expect(detail::offsetTime(*core::RationalTime::create(4, 24), sample, rate) ==
                   core::RationalTime::create(0, 1),
               "a frame within the look-back of the start repeats the first");
        expect(detail::offsetTime(*core::RationalTime::create(30, 24), sample, rate) ==
                   core::RationalTime::create(20 * 1, 24),
               "otherwise it is the frame lag frames before");
    }

    const exposure_test::ExposurePlan rising = exposure_test::exposurePlan();
    // A frame renders the same on its own, after the frames before it and reached backwards.
    {
        const runtime::CpuCompositionEvaluator fresh;
        const auto alone = frameOf(fresh, rising, 20);
        expect(alone.evaluated, "the plan evaluates");
        const runtime::CpuCompositionEvaluator walked;
        for (std::int64_t frame = 0; frame <= 20; ++frame)
            static_cast<void>(frameOf(walked, rising, frame));
        expect(frameOf(walked, rising, 20).pixel == alone.pixel, "a frame is the same scrubbed to as played to");
        const runtime::CpuCompositionEvaluator backwards;
        for (std::int64_t frame = 40; frame >= 15; --frame)
            static_cast<void>(frameOf(backwards, rising, frame));
        expect(frameOf(backwards, rising, 20).pixel == alone.pixel, "and the same reached backwards");
        runtime::CpuRowBandExecutor wide(6);
        runtime::EvaluationRequest request{*core::RationalTime::create(20, 24), rising.plan->output(),
                                           runtime::CompositionFormatResolution{}};
        request.pixelStorageByteLimit = 1U << 28U;
        request.bypassOperationCache = true;
        const runtime::CpuCompositionEvaluator banded;
        const auto result = banded.evaluate(rising.plan, request, {}, {}, &wide);
        expect(result.frame() != nullptr && at(result.frame()->processImage(), 6, 5) == alone.pixel,
               "and the same with the bands of a pool, and with no cache");
    }
    // It follows the footage with the lag asked for, against the independent reading.
    {
        const runtime::CpuCompositionEvaluator evaluator;
        const ExposureValues values;
        for (const std::int64_t frame : {0, 5, 11, 12, 13, 14, 18, 30, 60}) {
            const auto got = frameOf(evaluator, rising, frame);
            expect(got.evaluated &&
                       closeTo(got.pixel, exposure_test::expectedColour(values, frame), 2e-4),
                   "frame " + std::to_string(frame) + " is the footage times the specified gain");
        }
        const auto before = frameOf(evaluator, rising, 11).pixel[0];
        const auto first = frameOf(evaluator, rising, 12).pixel[0];
        const auto later = frameOf(evaluator, rising, 18).pixel[0];
        const auto settled = frameOf(evaluator, rising, 60).pixel[0];
        expect(std::abs(before - 0.18F) < 1e-3F, "footage at the target is left alone before the change");
        expect(first > 0.45F && first < 0.72F, "the flare shows at first, only partly corrected");
        expect(later < first && settled < later && std::abs(settled - 0.18F) < 0.01F,
               "and is pulled back to the target with the attack");
        // A dimming light rises with the release, more slowly than the flare fell.
        ExposureValues dimmer;
        dimmer.before = {0.72, 0.72, 0.72};
        dimmer.after = {0.18, 0.18, 0.18};
        const auto dimming = exposure_test::exposurePlan(dimmer, 57000);
        const auto dimFirst = frameOf(evaluator, dimming, 12).pixel[0];
        const auto dimLater = frameOf(evaluator, dimming, 18).pixel[0];
        expect(closeTo(frameOf(evaluator, dimming, 18).pixel, exposure_test::expectedColour(dimmer, 18),
                       2e-4) &&
                   dimFirst < 0.18F && dimLater > dimFirst && dimLater < 0.18F * 1.6F,
               "a dimming light is brought back up with the release");
    }
    // Parameters change the frame; no correction is the footage exactly.
    {
        const runtime::CpuCompositionEvaluator evaluator;
        ExposureValues bright;
        bright.target = 0.3;
        const auto other = exposure_test::exposurePlan(bright, 56600);
        expect(frameOf(evaluator, rising, 30).pixel != frameOf(evaluator, other, 30).pixel,
               "the target changes the frame");
        ExposureValues none;
        none.exposure = 0.0;
        none.whiteBalance = 0.0;
        const auto off = exposure_test::exposurePlan(none, 57100);
        const auto got = frameOf(evaluator, off, 30).pixel;
        expect(got[0] == 0.72F && got[1] == 0.72F && got[2] == 0.72F,
               "no exposure and no white balance leave the footage exactly as it is");
    }
    // A warm shot is made neutral and brought to the target.
    {
        const runtime::CpuCompositionEvaluator evaluator;
        ExposureValues warm;
        warm.steady = true;
        warm.before = {0.3, 0.2, 0.1};
        const auto plan = exposure_test::exposurePlan(warm, 57200);
        const auto got = frameOf(evaluator, plan, 5).pixel;
        expect(closeTo(got, exposure_test::expectedColour(warm, 5), 2e-4) &&
                   std::abs(got[0] - got[1]) < 2e-3F && std::abs(got[2] - got[1]) < 2e-3F &&
                   std::abs(got[1] - 0.18F) < 5e-3F,
               "a steady warm shot is neutral and at the target");
    }
    // Cache identity: a source time's reductions are shared by every frame whose window reaches it.
    {
        const runtime::CpuCompositionEvaluator evaluator;
        const auto node = document::NodeId::fromRaw(56600 + 30);
        const auto count = [&](const Rendered& rendered) {
            return std::ranges::count(rendered.statistics.evaluatedNodes, node);
        };
        const auto cold = frameOf(evaluator, rising, 40);
        const auto next = frameOf(evaluator, rising, 41);
        const auto again = frameOf(evaluator, rising, 41);
        expect(count(cold) > 0 && count(next) > 0 && count(next) * 3 < count(cold),
               "the next frame computes the one new frame's reductions, not the window again");
        expect(count(again) == 0 && again.statistics.hits > 0 && again.pixel == next.pixel,
               "the same frame again is served from the cache");
        ExposureValues retuned;
        retuned.release = 0.1;
        retuned.attack = 0.05;
        const auto changed = exposure_test::exposurePlan(retuned, 56600);
        const auto warmed = frameOf(evaluator, rising, 14);
        const auto afterChange = frameOf(evaluator, changed, 14);
        expect(count(warmed) > 0 && count(afterChange) > 0 && count(afterChange) * 3 < count(warmed) &&
                   afterChange.pixel != warmed.pixel,
               "a changed lag recomputes the filter only, never the measurements");
    }
    // A change leaving the look-back window must not pop the correction. Every frame across the change
    // and well past the window is rendered, for a brighter, a darker and a color-cast step, at the
    // node's default look-back and a short one, and the frame-to-frame change of the corrected picture
    // (sRGB codes, 0 to 255) after the first reaction is bounded by the step before it.
    {
        const auto code = [](const float linear) {
            const double v = std::clamp(static_cast<double>(linear), 0.0, 1.0);
            const double e = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
            return e * 255.0;
        };
        struct Step final {
            const char* name;
            std::array<double, 3> before;
            std::array<double, 3> after;
        };
        const Step steps[] = {{"brighter", {0.18, 0.18, 0.18}, {0.72, 0.72, 0.72}},
                              {"darker", {0.72, 0.72, 0.72}, {0.09, 0.09, 0.09}},
                              {"color cast", {0.18, 0.18, 0.18}, {0.40, 0.18, 0.06}}};
        std::uint64_t base = 58000;
        for (const std::int64_t lookBack : {std::int64_t{48}, std::int64_t{8}}) {
            for (const auto& step : steps) {
                ExposureValues values;
                values.lookBack = lookBack;
                values.changeFrame = 14;
                values.before = step.before;
                values.after = step.after;
                const auto plan = exposure_test::exposurePlan(values, base);
                base += 200;
                const runtime::CpuCompositionEvaluator evaluator;
                const std::int64_t last = values.changeFrame + lookBack + 12;
                std::array<double, 3> previous{};
                std::array<double, 3> lastStep{};
                // How far a step exceeds the previous one (1.5 times it and one code are allowed): a
                // correction that eases in or out never does, a weight vanishing at once always does.
                double worst = 0.0;
                for (std::int64_t frame = values.changeFrame - 2; frame <= last; ++frame) {
                    const auto got = frameOf(evaluator, plan, frame);
                    expect(got.evaluated, "every frame across the change renders");
                    const std::array<double, 3> now{code(got.pixel[0]), code(got.pixel[1]),
                                                    code(got.pixel[2])};
                    // The first reaction is the footage's own change at the change frame and the step
                    // after it; from the second step on the correction alone moves the picture.
                    for (std::size_t c = 0; c < 3; ++c) {
                        const double moved = std::abs(now[c] - previous[c]);
                        if (frame >= values.changeFrame + 2)
                            worst = std::max(worst, moved - (1.5 * lastStep[c] + 1.0));
                        lastStep[c] = moved;
                    }
                    previous = now;
                }
                std::cerr << "no-pop " << step.name << " lookBack " << lookBack << " excess " << worst << '\n';
                expect(worst <= 0.0,
                       std::string("the correction has no jump when a ") + step.name +
                           " change leaves a look-back of " + std::to_string(lookBack));
            }
        }
    }
    // A refused budget is an explicit error, never a wrong frame.
    {
        const runtime::CpuCompositionEvaluator evaluator;
        const auto starved = frameOf(evaluator, rising, 30, 4096);
        expect(!starved.evaluated && !starved.diagnostics.empty(),
               "a pixel budget too small for the window is refused with a diagnostic");
        const auto sufficient = frameOf(evaluator, rising, 30);
        expect(sufficient.evaluated, "and the same frame renders with the budget it needs");
    }
}

} // namespace

// With BLOOM_AUTO_EXPOSURE_ARTIFACT_DIR set, the frames around a brightness change are written as a
// contact sheet (a binary PPM, sRGB encoded): one column per frame, the footage above and the
// corrected frame below, so the lag can be seen reacting. Two sheets: a brightness change, and a
// brightness change with a colour cast.
void writeCapture() {
    const char* directory = std::getenv("BLOOM_AUTO_EXPOSURE_ARTIFACT_DIR");
    if (directory == nullptr || *directory == '\0')
        return;
    const auto encode = [](const double linear) {
        const double v = std::clamp(linear, 0.0, 1.0);
        const double e = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        return static_cast<unsigned char>(std::lround(e * 255.0));
    };
    const auto sheet = [&](const char* name, const ExposureValues& values) {
        constexpr std::int64_t first = 4;
        constexpr std::int64_t last = 60;
        constexpr int column = 16;
        constexpr int half = 40;
        const int columns = static_cast<int>(last - first + 1);
        const int width = columns * column;
        std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * half * 2 * 3, 24);
        const exposure_test::ExposurePlan plan = exposure_test::exposurePlan(values);
        const runtime::CpuCompositionEvaluator evaluator;
        for (std::int64_t frame = first; frame <= last; ++frame) {
            const auto got = frameOf(evaluator, plan, frame);
            const auto raw = !values.steady && frame >= values.changeFrame ? values.after : values.before;
            for (int y = 0; y < half * 2; ++y)
                for (int x = 0; x < column - 2; ++x) {
                    const auto offset =
                        (static_cast<std::size_t>(y) * width +
                         static_cast<std::size_t>((frame - first) * column + x)) * 3;
                    for (std::size_t c = 0; c < 3; ++c)
                        rgb[offset + c] = y < half - 2 ? encode(raw[c])
                                          : y >= half + 2 ? encode(static_cast<double>(got.pixel[c]))
                                                          : 24;
                }
        }
        std::ofstream out(std::string(directory) + "/" + name, std::ios::binary);
        out << "P6\n" << width << ' ' << half * 2 << "\n255\n";
        out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    };
    ExposureValues brighter;
    brighter.changeFrame = 14;
    sheet("auto-exposure-lag-brighter.ppm", brighter);
    ExposureValues dimmer;
    dimmer.changeFrame = 14;
    dimmer.before = {0.72, 0.72, 0.72};
    dimmer.after = {0.09, 0.09, 0.09};
    sheet("auto-exposure-lag-darker.ppm", dimmer);
    ExposureValues cast;
    cast.changeFrame = 14;
    cast.before = {0.18, 0.18, 0.18};
    cast.after = {0.40, 0.18, 0.06};
    sheet("auto-exposure-lag-color-cast.ppm", cast);
}

int main() {
    try {
        testReduce();
        testFilter();
        testApply();
        testProgram();
        testBudgetCancellationAndBands();
        testWindow();
        writeCapture();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "auto exposure tests passed\n";
    return 0;
}
