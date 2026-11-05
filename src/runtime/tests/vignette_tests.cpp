// #505 Vignette on the CPU reference: both shapes and both modes, roundness both ways, a moved
// centre, a hard edge and highlight protection against an independent double-precision formula
// over an HDR, signed, partial-alpha image whose data window overhangs its display window; the
// promises the node makes (alpha kept, the centre untouched, the corners at the full strength,
// a circle on a wide frame, protected highlights, the same falloff at a proxy resolution);
// identity, domains, the one fixed program whose values are data, the cache identity, the pixel
// budget and row cancellation.

#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using runtime::VignetteKernel;
using runtime::VignetteMode;
using runtime::VignetteShape;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Frame final {
    std::int64_t dataX = -4;
    std::int64_t dataY = -3;
    std::uint32_t dataWidth = 48;
    std::uint32_t dataHeight = 26;
    std::uint32_t displayWidth = 40;
    std::uint32_t displayHeight = 20;
    core::PixelAspectRatio aspect = core::PixelAspectRatio::square();
};

// Premultiplied pixels cycling through grey, HDR, signed colour, partial alpha and transparent
// black, so every position sees every kind of value.
std::array<float, 4> sample(const std::int64_t x, const std::int64_t y) {
    static const std::vector<std::array<float, 4>> values{
        {0.18F, 0.18F, 0.18F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F},    {4.0F, 2.5F, 6.0F, 1.0F},
        {-0.25F, 0.5F, 0.125F, 0.75F}, {0.3F, 0.15F, 0.6F, 0.6F}, {0.0F, 0.0F, 0.0F, 0.0F},
        {0.05F, 0.02F, 0.01F, 0.1F}, {0.7F, 0.2F, 0.7F, 1.0F}};
    const auto index = static_cast<std::size_t>((x * 3 + y * 5) % 8 + 8) % values.size();
    return values[index];
}

render::Rgba32fImage image(const Frame& frame = {}, const bool uniform = false) {
    const auto data =
        render::ImageWindow::create(frame.dataX, frame.dataY, frame.dataWidth, frame.dataHeight);
    const auto display = render::ImageWindow::create(0, 0, frame.displayWidth, frame.displayHeight);
    auto descriptor =
        render::Rgba32fImageDescriptor::create(*data.value(), *display.value(), frame.aspect);
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 22U);
    for (std::int64_t y = frame.dataY; y < frame.dataY + frame.dataHeight; ++y)
        for (std::int64_t x = frame.dataX; x < frame.dataX + frame.dataWidth; ++x) {
            const auto p = uniform ? std::array{0.5F, 0.5F, 0.5F, 1.0F} : sample(x, y);
            const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!pixel || builder.value()->write(x, y, *pixel.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

render::ImageWindow displayWindow(const std::uint32_t width, const std::uint32_t height) {
    const auto window = render::ImageWindow::create(0, 0, width, height);
    return *window.value();
}

struct Values final {
    VignetteShape shape = VignetteShape::Elliptical;
    VignetteMode mode = VignetteMode::Multiply;
    double amount = 0.5;
    double exposure = -1.0;
    double midpoint = 0.5;
    double roundness = 0.0;
    double feather = 0.5;
    double protection = 0.0;
    double centerX = 0.0;
    double centerY = 0.0;
};

VignetteKernel kernel(const Values& v) {
    VignetteKernel item;
    item.shape = v.shape;
    item.mode = v.mode;
    item.amount = {document::ParameterId::fromRaw(1), v.amount};
    item.exposure = {document::ParameterId::fromRaw(2), v.exposure};
    item.midpoint = {document::ParameterId::fromRaw(3), v.midpoint};
    item.roundness = {document::ParameterId::fromRaw(4), v.roundness};
    item.feather = {document::ParameterId::fromRaw(5), v.feather};
    item.highlightProtection = {document::ParameterId::fromRaw(6), v.protection};
    item.center = {document::ParameterId::fromRaw(7), document::Vec2d{v.centerX, v.centerY}};
    return item;
}

std::vector<double> operands(const Values& v) {
    return {v.amount,   v.exposure,   v.midpoint, v.roundness,
            v.feather,  v.protection, v.centerX,  v.centerY};
}

std::optional<runtime::detail::PreparedColorGrade> prepare(const Values& v) {
    return runtime::detail::prepareColorGrade(kernel(v), operands(v));
}

// The specification in double precision (docs/architecture/image-filters.md, "Vignette"), written
// out again here rather than through the document's helpers.
double referenceGain(const Values& v, const Frame& frame, const double x, const double y,
                     const double straightLuma, const double scale = 1.0) {
    const double aspect = static_cast<double>(frame.aspect.numerator()) /
                          static_cast<double>(frame.aspect.denominator());
    const double width = frame.displayWidth / scale * aspect;
    const double height = frame.displayHeight / scale;
    const double mean = std::sqrt(width * height) / 2.0;
    const double circle = std::max(v.roundness, 0.0);
    const double halfX = std::pow(width / 2.0, 1.0 - circle) * std::pow(mean, circle);
    const double halfY = std::pow(height / 2.0, 1.0 - circle) * std::pow(mean, circle);
    const double centreX = frame.displayWidth / 2.0 + v.centerX * scale;
    const double centreY = frame.displayHeight / 2.0 + v.centerY * scale;
    const double u = std::min(std::abs((x + 0.5 - centreX) / scale * aspect / halfX), 4.0);
    const double w = std::min(std::abs((y + 0.5 - centreY) / scale / halfY), 4.0);
    const double base = v.shape == VignetteShape::Rectangular ? 6.0 : 2.0;
    const double n = v.roundness < 0.0 ? base * std::pow(4.0, -v.roundness) : base;
    const double d = std::pow(std::pow(u, n) + std::pow(w, n), 1.0 / n);
    const double middle = 0.4 + 1.2 * v.midpoint;
    const double inner = middle * (1.0 - v.feather);
    const double outer = middle * (1.0 + v.feather);
    double mask = 0.0;
    if (outer > inner) {
        const double t = std::clamp((d - inner) / (outer - inner), 0.0, 1.0);
        mask = t * t * (3.0 - 2.0 * t);
    } else {
        mask = d >= inner ? 1.0 : 0.0;
    }
    double gain = v.mode == VignetteMode::Multiply ? 1.0 - v.amount * mask
                                                   : std::exp2(v.exposure * mask);
    const double h = std::clamp((straightLuma - 0.18) / 0.82, 0.0, 1.0);
    gain += v.protection * h * h * (3.0 - 2.0 * h) * (1.0 - gain);
    return gain;
}

struct Applied final {
    std::shared_ptr<const render::Rgba32fImage> image;
    render::Rgba32fImage input;
};

Applied apply(const Values& v, const Frame& frame = {}, const bool uniform = false,
              const double scale = 1.0) {
    const auto grade = prepare(v);
    expect(grade.has_value(), "the vignette prepares");
    auto input = image(frame, uniform);
    if (!grade)
        return {nullptr, std::move(input)};
    const auto result = runtime::detail::applyColorGrade(
        *grade, input, 1U << 22U, {}, runtime::OperationIndex::fromRaw(0), {}, scale, scale);
    expect(result.image &&
               result.image->descriptor()->dataWindow() == input.descriptor()->dataWindow() &&
               result.image->descriptor()->displayWindow() == input.descriptor()->displayWindow(),
           "the vignette keeps the data and display windows");
    return {result.image, std::move(input)};
}

std::array<float, 4> at(const render::Rgba32fImage& image, const std::int64_t x,
                        const std::int64_t y) {
    std::array<float, 4> value{};
    if (const auto pixel = image.read(x, y))
        std::ranges::copy(pixel.value()->components(), value.begin());
    return value;
}

bool near(const double actual, const double expected, const double tolerance = 2e-5) {
    return std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected));
}

void expectReference(const Values& v, const std::string& name, const Frame& frame = {}) {
    const auto applied = apply(v, frame);
    if (!applied.image)
        return;
    bool matches = true;
    bool alphaKept = true;
    for (std::int64_t y = frame.dataY; y < frame.dataY + frame.dataHeight; ++y)
        for (std::int64_t x = frame.dataX; x < frame.dataX + frame.dataWidth; ++x) {
            const auto in = at(applied.input, x, y);
            const auto out = at(*applied.image, x, y);
            alphaKept = alphaKept && out[3] == in[3];
            if (in[3] == 0.0F) {
                matches = matches && out == in;
                continue;
            }
            const double luma = (0.2126 * static_cast<double>(in[0]) + 0.7152 * static_cast<double>(in[1]) +
                                 0.0722 * static_cast<double>(in[2])) /
                                static_cast<double>(in[3]);
            const double gain = referenceGain(v, frame, static_cast<double>(x),
                                              static_cast<double>(y), luma);
            for (std::size_t i = 0; i < 3; ++i)
                matches = matches && near(static_cast<double>(out[i]), static_cast<double>(in[i]) * gain);
        }
    expect(matches, name + " matches the double-precision formula");
    expect(alphaKept, name + " keeps every alpha");
}

void testAgainstReference() {
    expectReference({}, "the default elliptical multiply");
    expectReference({.shape = VignetteShape::Rectangular, .amount = 0.8, .midpoint = 0.3},
                    "a rectangular multiply");
    expectReference({.mode = VignetteMode::Exposure, .exposure = -2.5, .feather = 0.8},
                    "an exposure vignette");
    expectReference({.mode = VignetteMode::Exposure, .exposure = 1.5, .midpoint = 0.2},
                    "a brightening exposure vignette");
    expectReference({.amount = -0.75, .roundness = 1.0}, "a brightening circular vignette");
    expectReference({.shape = VignetteShape::Rectangular, .roundness = -0.6, .feather = 0.2},
                    "a squared-off rectangle");
    expectReference({.roundness = 0.4, .centerX = 6.5, .centerY = -3.0}, "a moved centre");
    expectReference({.amount = 1.0, .protection = 0.7}, "highlight protection");
    expectReference({.amount = 0.6, .feather = 0.0}, "a hard edge");
    Frame anamorphic;
    anamorphic.aspect = *core::PixelAspectRatio::create(2, 1);
    expectReference({.roundness = 1.0}, "a circle over anamorphic pixels", anamorphic);
}

void testPromises() {
    const Frame frame;
    // Full strength at the corners, nothing at the centre.
    Values hardCorners{.amount = 1.0, .midpoint = 0.0, .feather = 0.2};
    auto applied = apply(hardCorners, frame, true);
    if (applied.image) {
        expect(at(*applied.image, 20, 10)[0] == 0.5F, "the centre is untouched");
        expect(at(*applied.image, 0, 0)[0] == 0.0F && at(*applied.image, 39, 19)[0] == 0.0F &&
                   at(*applied.image, 0, 0)[3] == 1.0F,
               "a full multiply at full strength makes the corners black, alpha kept");
    }
    // Exposure: the corners lose exactly the stops.
    applied = apply({.mode = VignetteMode::Exposure, .exposure = -2.0, .midpoint = 0.0,
                     .feather = 0.2},
                    frame, true);
    if (applied.image)
        expect(at(*applied.image, 0, 0)[0] == 0.125F && at(*applied.image, 20, 10)[0] == 0.5F,
               "an exposure vignette takes its stops at the corners and nothing at the centre");
    // Protection keeps a pixel at 1.0 entirely and a pixel at mid grey not at all.
    Values protectedValues{.amount = 1.0, .midpoint = 0.0, .feather = 0.2, .protection = 1.0};
    auto grade = prepare(protectedValues);
    if (grade) {
        const auto data = runtime::detail::vignetteProgramData(
            *grade, displayWindow(40, 20), 0, 0, 1.0, 1.0, 1.0);
        expect(data.has_value(), "the vignette geometry is representable");
        if (data) {
            std::array<float, 4> bright{2.0F, 2.0F, 2.0F, 1.0F};
            std::array<float, 4> grey{0.09F, 0.09F, 0.09F, 0.5F};
            expect(runtime::detail::vignettePixel(*data, 0, 0, bright) && bright[0] == 2.0F &&
                       runtime::detail::vignettePixel(*data, 0, 0, grey) && grey[0] == 0.0F &&
                       grey[3] == 0.5F,
                   "full protection keeps highlights and leaves straight mid grey to the "
                   "vignette");
        }
    }
    // Roundness 1 on a wide frame: a circle, so equal distances across and down agree.
    Values circle{.roundness = 1.0};
    grade = prepare(circle);
    if (grade) {
        const auto data = runtime::detail::vignetteProgramData(
            *grade, displayWindow(400, 100), 0, 0, 1.0, 1.0, 1.0);
        std::array<float, 4> across{1.0F, 1.0F, 1.0F, 1.0F};
        std::array<float, 4> down = across;
        std::array<float, 4> ellipse = across;
        // Pixel centres 60 px right of and 60 px below the centre (200, 50).
        expect(data && runtime::detail::vignettePixel(*data, 259, 49, across) &&
                   runtime::detail::vignettePixel(*data, 199, 109, down) &&
                   std::abs(across[0] - down[0]) < 1e-6F && across[0] < 1.0F,
               "roundness 1 draws a circle on a wide frame");
        const auto frameFit = prepare({});
        const auto fitted = runtime::detail::vignetteProgramData(
            *frameFit, displayWindow(400, 100), 0, 0, 1.0, 1.0, 1.0);
        std::array<float, 4> fittedDown = ellipse;
        expect(fitted && runtime::detail::vignettePixel(*fitted, 259, 49, ellipse) &&
                   runtime::detail::vignettePixel(*fitted, 199, 109, fittedDown) &&
                   ellipse[0] > fittedDown[0],
               "roundness 0 follows the frame: a wide frame falls off sooner down than across");
    }
    // The shapes differ along the diagonal: a rectangle keeps more of its corners' neighbours.
    applied = apply({.amount = 1.0, .midpoint = 0.6}, frame, true);
    auto rectangle = apply({.shape = VignetteShape::Rectangular, .amount = 1.0, .midpoint = 0.6},
                           frame, true);
    if (applied.image && rectangle.image)
        expect(at(*applied.image, 3, 2)[0] < at(*rectangle.image, 3, 2)[0],
               "a rectangle reaches further into the corners than an ellipse");
    // The same falloff at half resolution: the display window and the centre offset scale.
    Frame half;
    half.dataX = -2;
    half.dataY = -1;
    half.dataWidth = 24;
    half.dataHeight = 13;
    half.displayWidth = 20;
    half.displayHeight = 10;
    const Values moved{.roundness = 0.5, .centerX = 4.0, .centerY = 2.0};
    const auto full = apply(moved, frame, true);
    const auto proxy = apply(moved, half, true, 0.5);
    if (full.image && proxy.image) {
        // Proxy pixel (x, y) covers full pixels (2x, 2y)..(2x + 1, 2y + 1); its centre is their
        // shared corner, which lies between the four full pixels' values.
        bool consistent = true;
        for (const auto& [x, y] : std::array<std::pair<int, int>, 4>{{{1, 1}, {9, 3}, {17, 8}, {4, 7}}}) {
            const float p = at(*proxy.image, x, y)[0];
            float low = 1.0F;
            float high = 0.0F;
            for (const int dx : {0, 1})
                for (const int dy : {0, 1}) {
                    const float f = at(*full.image, 2 * x + dx, 2 * y + dy)[0];
                    low = std::min(low, f);
                    high = std::max(high, f);
                }
            consistent = consistent && p >= low - 1e-6F && p <= high + 1e-6F;
        }
        expect(consistent, "a proxy resolution draws the same vignette");
    }
}

void testIdentityDomainsAndProgram() {
    const auto none = prepare({.amount = 0.0});
    const auto noStops = prepare({.mode = VignetteMode::Exposure, .exposure = 0.0});
    expect(none && none->identity && noStops && noStops->identity,
           "no amount, or no stops, is the identity");
    const auto vignette = prepare({});
    expect(vignette && !vignette->identity && vignette->kind == runtime::detail::ColorGradeKind::Vignette,
           "the default vignette is a vignette grade");
    const auto refused = [](const Values& v) { return !prepare(v).has_value(); };
    expect(refused({.amount = 1.5}) && refused({.amount = -1.5}) &&
               refused({.exposure = 10.5}) && refused({.midpoint = -0.1}) &&
               refused({.midpoint = 1.1}) && refused({.roundness = 1.1}) &&
               refused({.feather = 1.5}) && refused({.protection = -0.5}) &&
               refused({.centerX = 2e6}) && refused({.amount = std::nan("")}),
           "values outside their domains are refused");
    expect(!runtime::detail::prepareColorGrade(kernel({}), std::vector<double>{0.5}).has_value(),
           "a short operand list is refused");
    // The strength the mode does not read never reaches the table.
    const auto multiply = prepare({.exposure = -3.0});
    const auto exposure = prepare({.mode = VignetteMode::Exposure, .amount = 0.9});
    expect(multiply && vignette && multiply->data == vignette->data && exposure &&
               exposure->data == prepare({.mode = VignetteMode::Exposure})->data,
           "an unused strength changes neither the table nor the cache identity");
    // Every value is data: one fixed program whose shader never changes.
    const auto other = prepare({.shape = VignetteShape::Rectangular, .amount = 0.2, .midpoint = 0.9,
                                .roundness = -0.3, .feather = 0.1, .protection = 0.4,
                                .centerX = 12.0});
    const auto window = displayWindow(40, 20);
    const auto a = runtime::detail::vignetteProgramData(*vignette, window, -4, -3, 1.0, 1.0, 1.0);
    const auto b = runtime::detail::vignetteProgramData(*other, window, -4, -3, 1.0, 1.0, 1.0);
    expect(a && b && a->size() == runtime::detail::kVignetteProgramFloats, "geometry is appended");
    if (a && b) {
        const auto first =
            runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::Vignette, *a);
        const auto second =
            runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::Vignette, *b);
        expect(first.succeeded() && second.succeeded() &&
                   render::validateOcioGpuProgram(*first.program(), {}) ==
                       render::OcioGpuProgramError::None &&
                   first.program()->semanticsId == "bloom.render.vignette.v1" &&
                   first.program()->shaderText == second.program()->shaderText &&
                   first.program()->uniformBufferData != second.program()->uniformBufferData,
               "one fixed program reads every vignette's values as data");
        expect(!runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::Vignette,
                                                        vignette->data)
                    .succeeded(),
               "a table without its geometry is refused by the program");
        auto broken = *a;
        broken[22] = 0.0F;
        expect(!runtime::detail::buildColorGradeProgram(runtime::detail::ColorGradeKind::Vignette,
                                                        broken)
                    .succeeded(),
               "a degenerate geometry is refused by the program");
    }
    expect(runtime::detail::colorGradeIdentity(*vignette) !=
               runtime::detail::colorGradeIdentity(*other),
           "different vignettes have different cache identities");
    expect(!runtime::detail::vignetteProgramData(*vignette, window, -4, -3, 1.0, 0.0, 1.0) &&
               !runtime::detail::vignetteProgramData(*vignette, window, -4, -3, 0.0, 1.0, 1.0),
           "a non-positive scale or pixel aspect is refused");
    std::array<float, 4> pixel{0.5F, 0.5F, 0.5F, 1.0F};
    expect(!runtime::detail::colorGradePixel(*vignette, pixel),
           "a vignette is never applied without its position");
}

void testBudgetAndCancellation() {
    const auto input = image();
    const auto grade = prepare({});
    const auto refused = runtime::detail::applyColorGrade(*grade, input, 64, {},
                                                          runtime::OperationIndex::fromRaw(0), {});
    expect(!refused.image && refused.diagnostic &&
               refused.diagnostic->code ==
                   runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "a vignette beyond the pixel budget is refused");
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(505)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Vignette cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result =
                runtime::detail::applyColorGrade(*grade, input, 1U << 22U, context.cancellation(),
                                                 runtime::OperationIndex::fromRaw(0), {});
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
           "a cancelled vignette publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(scheduler.isQuiescent(), "the cancelled vignette shuts down safely");
}

} // namespace

int main() {
    try {
        testAgainstReference();
        testPromises();
        testIdentityDomainsAndProgram();
        testBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " vignette expectation(s) failed\n";
        return 1;
    }
    std::cout << "vignette tests passed\n";
    return 0;
}
