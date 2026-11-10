// #525 Fields on the CPU reference: every field operation's program prepares and rasters to finite
// pixels; a raster pixel is the program's value at that pixel's centre (the Field Sample read); an
// Image field reading its own canvas window with the Nearest filter returns the stored texels
// exactly; sub-samples average; the Mask output stays coverage; a changed operand changes the
// cache identity; the pixel budget refuses; cancellation stops a raster; and a mask's signed
// distance matches a brute-force search of the nearest boundary texel inside its reach.

#include "field_raster.hpp"
#include "field_test_programs.hpp"
#include "image_effect.hpp"
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace bloom;
using core::field::FieldOp;
using runtime::FieldProgram;
using runtime::FieldRasterKernel;
namespace programs = bloom::field_test_programs;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr double kWidth = 16.0;
constexpr double kHeight = 12.0;

render::Rgba32fImage image(const std::int64_t x, const std::int64_t y, const std::uint32_t width,
                           const std::uint32_t height,
                           const std::function<std::array<float, 4>(std::int64_t, std::int64_t)>&
                               pixel) {
    const auto data = render::ImageWindow::create(x, y, width, height);
    const auto display = render::ImageWindow::create(0, 0, 16, 12);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 22U);
    for (std::int64_t row = y; row < y + height; ++row)
        for (std::int64_t column = x; column < x + width; ++column) {
            const auto p = pixel(column, row);
            const auto value = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!value || builder.value()->write(column, row, *value.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

render::Rgba32fImage canvas() {
    return image(0, 0, 16, 12, [](std::int64_t, std::int64_t) {
        return std::array{0.0F, 0.0F, 0.0F, 0.0F};
    });
}

// The program's operands as the evaluator samples them: constants, then the clock time.
std::vector<double> operands(const FieldProgram& program, const double seconds = 0.0) {
    std::vector<double> values;
    runtime::forEachFieldOperand(program, [&](const auto& operand) {
        using Operand = std::decay_t<decltype(operand)>;
        if constexpr (std::is_same_v<Operand, runtime::CompiledPathParameter>) {
            const auto& path = std::get<document::PathValue>(operand.source);
            values.push_back(static_cast<double>(path.firstContour().anchors.size()));
            for (const auto& anchor : path.firstContour().anchors) {
                values.push_back(anchor.point.x);
                values.push_back(anchor.point.y);
            }
        } else if constexpr (std::is_same_v<Operand, runtime::CompiledGradientParameter>) {
            const auto& gradient = std::get<document::GradientValue>(operand.source);
            values.push_back(static_cast<double>(gradient.stops.size()));
            for (const auto& stop : gradient.stops)
                for (const double v : {stop.position, stop.color.red, stop.color.green,
                                       stop.color.blue, stop.color.alpha})
                    values.push_back(v);
            values.push_back(static_cast<double>(gradient.modes.interpolation));
            values.push_back(static_cast<double>(gradient.modes.colorMode));
            values.push_back(static_cast<double>(gradient.modes.hueDirection));
        } else if constexpr (std::is_same_v<Operand, runtime::CompiledVec2Parameter>) {
            const auto& v = std::get<document::Vec2d>(operand.source);
            values.push_back(v.x);
            values.push_back(v.y);
        } else {
            values.push_back(std::get<double>(operand.source));
        }
    });
    values.push_back(seconds);
    return values;
}

std::optional<runtime::detail::PreparedColorGrade> prepare(const FieldProgram& program,
                                                           const std::uint32_t samples = 1,
                                                           const double seconds = 0.0) {
    return runtime::detail::prepareFieldRasterGrade(FieldRasterKernel{program, samples},
                                                    operands(program, seconds));
}

runtime::detail::ImageEffectResult raster(const runtime::detail::PreparedColorGrade& grade,
                                          std::span<const render::Rgba32fImage* const> slots = {},
                                          const std::size_t budget = 1U << 22U,
                                          const runtime::CancellationToken& cancel = {}) {
    return runtime::detail::applyFieldRaster(grade, canvas(), slots, kWidth, kHeight, 1.0, 1.0,
                                             budget, cancel, runtime::OperationIndex::fromRaw(1),
                                             {}, nullptr);
}

// #647 the vector output's one instruction holds both streams: it prepares, publishes finite pixels
// and its two components are the two decorrelated noises.
void testNoiseVectorOutput() {
    for (const int variant : {100, 101, 103, 105}) {
        const auto name = "noise vector output " + std::to_string(variant);
        const auto program = programs::operationProgram(FieldOp::Noise, 3000, variant);
        const auto grade = prepare(program);
        expect(grade.has_value(), name + ": the program prepares");
        if (!grade)
            continue;
        const auto result = raster(*grade);
        expect(result.image && !result.diagnostic, name + ": the raster publishes");
        if (!result.image)
            continue;
        std::size_t differing = 0;
        bool finite = true;
        for (const auto& pixel : result.image->pixels()) {
            finite = finite && std::isfinite(pixel.red()) && std::isfinite(pixel.green());
            differing += pixel.red() != pixel.green() ? 1U : 0U;
        }
        expect(finite, name + ": the pixels are finite");
        expect(differing * 2 > result.image->pixels().size(),
               name + ": the two streams are decorrelated");
    }
}

void testEveryOperationRasters() {
    for (const auto op : core::field::kFieldOps)
        for (int variant = 0; variant < 3; ++variant) {
            const auto name = std::string(core::field::fieldOpName(op)) + " variant " +
                              std::to_string(variant);
            const auto program = programs::operationProgram(op, 1000, variant);
            const auto grade = prepare(program);
            expect(grade.has_value(), name + ": the program prepares");
            if (!grade)
                continue;
            const auto slot = image(0, 0, 16, 12, [](const std::int64_t x, const std::int64_t y) {
                return std::array{0.1F * static_cast<float>(x), 0.2F * static_cast<float>(y),
                                  0.5F, 1.0F};
            });
            const std::array<const render::Rgba32fImage*, 1> slots{&slot};
            const auto result = raster(*grade, op == FieldOp::Image || op == FieldOp::Rays ||
                                                       op == FieldOp::Homography
                                                   ? std::span<const render::Rgba32fImage* const>(slots)
                                                   : std::span<const render::Rgba32fImage* const>{});
            expect(result.image && !result.diagnostic, name + ": the raster publishes");
            if (!result.image)
                continue;
            bool finite = true;
            bool varies = false;
            const auto first = result.image->pixels().front();
            for (const auto& p : result.image->pixels()) {
                finite = finite && std::isfinite(p.red()) && std::isfinite(p.green()) &&
                         std::isfinite(p.blue()) && p.alpha() >= 0.0F && p.alpha() <= 1.0F;
                varies = varies || !(p == first);
            }
            expect(finite, name + ": every pixel is finite with alpha in [0, 1]");
            // A Constant, and a Uniform force (variant 2: one direction everywhere), are flat.
            if (op != FieldOp::Constant && !(op == FieldOp::Force && variant == 2))
                expect(varies, name + ": the field varies across the frame");
        }
}

// A Raw scalar raster's pixel is the program's value at the pixel's centre.
void testRasterIsTheSampledField() {
    for (const auto op : {FieldOp::Noise, FieldOp::Voronoi, FieldOp::Pattern, FieldOp::Remap}) {
        const auto program = programs::operationProgram(op, 2000, 1);
        const auto grade = prepare(program);
        const auto result = grade ? raster(*grade) : runtime::detail::ImageEffectResult{};
        expect(result.image != nullptr, "sample parity: the raster publishes");
        if (!result.image)
            continue;
        const auto values = operands(program);
        double worst = 0.0;
        for (std::int64_t j = 0; j < 12; ++j)
            for (std::int64_t i = 0; i < 16; ++i) {
                const double x = static_cast<double>(i) + 0.5 - kWidth / 2.0;
                const double y = kHeight / 2.0 - (static_cast<double>(j) + 0.5);
                const auto sampled = runtime::detail::sampleFieldProgram(program, values, x, y);
                const auto pixel = result.image->pixels()[static_cast<std::size_t>(j * 16 + i)];
                if (!sampled) {
                    worst = std::numeric_limits<double>::infinity();
                    continue;
                }
                worst = std::max(worst, std::abs(static_cast<double>(pixel.red()) -
                                                 static_cast<double>((*sampled)[0])));
            }
        expect(worst <= 1.0e-5, std::string(core::field::fieldOpName(op)) +
                                    ": a raster pixel is the field sampled at its centre (" +
                                    std::to_string(worst) + ")");
    }
}

// An Image field over its own canvas window, Nearest: the stored texels come back exactly.
void testImageSlotReadsTheStoredTexels() {
    programs::Builder b(3000);
    const auto c = b.add(FieldOp::Coordinate);
    const auto read = b.add(FieldOp::Image, {c}, {1, 0, 2, 4, 0, 1});
    b.last().slot = 0;
    const auto program = b.finish(read, core::field::FieldResultKind::Color);
    const auto slot = image(0, 0, 16, 12, [](const std::int64_t x, const std::int64_t y) {
        return std::array{0.25F * static_cast<float>(x) - 1.0F, 3.0F - 0.5F * static_cast<float>(y),
                          0.125F, 1.0F};
    });
    const auto grade = prepare(program);
    const std::array<const render::Rgba32fImage*, 1> slots{&slot};
    const auto result = grade ? raster(*grade, slots) : runtime::detail::ImageEffectResult{};
    expect(result.image != nullptr, "image slot: the raster publishes");
    if (result.image)
        expect(std::ranges::equal(result.image->pixels(), slot.pixels()),
               "image slot: Nearest over its own window returns the stored texels exactly");
    // An absent image is transparent everywhere.
    const auto empty = grade ? raster(*grade) : runtime::detail::ImageEffectResult{};
    expect(empty.image && std::ranges::all_of(empty.image->pixels(), [](const auto& p) {
               return p == render::Rgba32f::transparent();
           }),
           "image slot: an absent image reads transparent");
}

void testSamplesModesIdentityBudgetAndCancellation() {
    const auto program = programs::operationProgram(FieldOp::Noise, 4000, 1);
    const auto one = prepare(program, 1);
    const auto four = prepare(program, 4);
    const auto sixteen = prepare(program, 16);
    expect(one && four && sixteen, "sub-samples: 1, 4 and 16 prepare");
    if (one && four && sixteen) {
        const auto a = raster(*one);
        const auto b = raster(*four);
        const auto c = raster(*sixteen);
        expect(a.image && b.image && c.image && !std::ranges::equal(a.image->pixels(), b.image->pixels()) &&
                   !std::ranges::equal(b.image->pixels(), c.image->pixels()),
               "sub-samples: 4 and 16 sub-samples average a different set of positions");
        expect(runtime::detail::colorGradeIdentity(*one) !=
                   runtime::detail::colorGradeIdentity(*four),
               "sub-samples: the count is part of the cache identity");
    }
    // A changed operand changes the identity; the same operands keep it.
    auto changed = program;
    changed.instructions[1].scalars[0].source = 16.0;
    const auto again = prepare(program);
    const auto moved = prepare(changed);
    expect(one && again && moved &&
               runtime::detail::colorGradeIdentity(*one) ==
                   runtime::detail::colorGradeIdentity(*again) &&
               runtime::detail::colorGradeIdentity(*one) !=
                   runtime::detail::colorGradeIdentity(*moved),
           "identity: equal operands share one identity; a changed operand has its own");
    // A looping noise reads the clock; a still one does not.
    auto looping = program;
    looping.instructions[1].choices[4] = 1;
    looping.instructions[1].scalars[7].source = 2.0;
    const auto early = prepare(looping, 1, 0.5);
    const auto late = prepare(looping, 1, 1.75);
    const auto lapped = prepare(looping, 1, 5.5);
    expect(early && late && lapped &&
               runtime::detail::colorGradeIdentity(*early) !=
                   runtime::detail::colorGradeIdentity(*late) &&
               runtime::detail::colorGradeIdentity(*early) ==
                   runtime::detail::colorGradeIdentity(*lapped),
           "loop: the evolution moves with the clock and repeats every loop length");
    // The Mask output is coverage in every channel.
    auto mask = program;
    mask.mode = static_cast<std::uint8_t>(core::field::FieldOutputMode::Mask);
    const auto masked = prepare(mask);
    const auto coverage = masked ? raster(*masked) : runtime::detail::ImageEffectResult{};
    expect(coverage.image && std::ranges::all_of(coverage.image->pixels(), [](const auto& p) {
               return p.red() == p.alpha() && p.green() == p.alpha() && p.blue() == p.alpha() &&
                      p.alpha() >= 0.0F && p.alpha() <= 1.0F;
           }),
           "mask: the Mask output is coverage in every channel");
    if (one) {
        const auto refused = raster(*one, {}, 16);
        expect(!refused.image && refused.diagnostic &&
                   refused.diagnostic->code ==
                       runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
               "budget: a raster over the pixel budget is refused");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(525)};
        bool cancelled = false;
        const auto grade = *one;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Field cancellation", owner), [&](runtime::TaskContext& context) {
                static_cast<void>(scheduler.cancelOwner(owner));
                const auto result = raster(grade, {}, 1U << 22U, context.cancellation());
                cancelled = result.cancelled && !result.image;
                return runtime::TaskResult<void>::cancelled();
            });
        expect(submission.accepted(), "cancellation: the task is admitted");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::optional<runtime::TaskResult<void>> outcome;
        while (!outcome && std::chrono::steady_clock::now() < deadline) {
            outcome = submission.handle.tryTakeResult();
            std::this_thread::yield();
        }
        expect(outcome && outcome->state() == runtime::TaskState::Cancelled && cancelled,
               "cancellation: a cancelled raster publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expect(scheduler.isQuiescent(), "cancellation: the cancelled raster shuts down safely");
    }
    // Field Sample reads no image, and refuses an operand outside its domain.
    auto bad = program;
    bad.instructions[1].vectors[0].source = document::Vec2d{0.0, 0.0};
    expect(!runtime::detail::sampleFieldProgram(bad, operands(bad), 0.0, 0.0),
           "domain: a zero noise size is refused");
}

// The signed distance in pixels to the mask's 0.5-coverage boundary: negative inside.
void testMaskDistanceMatchesBruteForce() {
    const auto mask = image(2, 1, 11, 9, [](const std::int64_t x, const std::int64_t y) {
        const double dx = static_cast<double>(x) - 7.0;
        const double dy = static_cast<double>(y) - 5.0;
        const float a = dx * dx + dy * dy <= 9.0 || (x == 11 && y == 2) ? 1.0F : 0.0F;
        return std::array{a, a, a, a};
    });
    const runtime::MaskDistanceKernel kernel{6.0};
    const auto result = runtime::detail::applyMaskDistance(
        kernel, mask, 1.0, 1.0, 1U << 22U, {}, runtime::OperationIndex::fromRaw(1), {});
    expect(result.image != nullptr, "mask distance: publishes");
    if (!result.image)
        return;
    const auto reach = runtime::detail::maskDistanceReach(kernel, 1.0, 1.0);
    const auto window = result.image->descriptor()->dataWindow();
    expect(window.originX() == 2 - reach && window.originY() == 1 - reach &&
               window.extent().width() == 11U + 2U * static_cast<std::uint64_t>(reach),
           "mask distance: the output grows by the reach on every side");
    const auto inside = [&](const std::int64_t x, const std::int64_t y) {
        if (x < 2 || y < 1 || x >= 13 || y >= 10)
            return false;
        return mask.pixels()[static_cast<std::size_t>((y - 1) * 11 + (x - 2))].alpha() >= 0.5F;
    };
    double worst = 0.0;
    for (std::int64_t y = window.originY(); y < window.maxYExclusive(); ++y)
        for (std::int64_t x = window.originX(); x < window.maxXExclusive(); ++x) {
            const bool in = inside(x, y);
            double nearest = std::numeric_limits<double>::infinity();
            for (std::int64_t v = window.originY() - 1; v <= window.maxYExclusive(); ++v)
                for (std::int64_t u = window.originX() - 1; u <= window.maxXExclusive(); ++u)
                    if (inside(u, v) != in) {
                        const double dx = static_cast<double>(u - x);
                        const double dy = static_cast<double>(v - y);
                        nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
                    }
            const double cap = static_cast<double>(reach + 1);
            const double expected = std::min(nearest, cap) - 0.5;
            const double signedExpected = in ? -expected : expected;
            const auto pixel =
                result.image->pixels()[static_cast<std::size_t>((y - window.originY()) *
                                                                    static_cast<std::int64_t>(window.extent().width()) +
                                                                (x - window.originX()))];
            worst = std::max(worst, std::abs(static_cast<double>(pixel.red()) - signedExpected));
        }
    expect(worst <= 1.0e-5, "mask distance: exact signed distances inside the reach (" +
                                std::to_string(worst) + ")");
}

} // namespace

int main() {
    try {
        testEveryOperationRasters();
        testNoiseVectorOutput();
        testRasterIsTheSampledField();
        testImageSlotReadsTheStoredTexels();
        testSamplesModesIdentityBudgetAndCancellation();
        testMaskDistanceMatchesBruteForce();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
