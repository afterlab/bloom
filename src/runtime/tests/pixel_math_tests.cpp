// #481 Image Math, Image Map Range and Image Clamp on the CPU reference: known answers for every
// operation read from real pixels, premultiplication and channel choices, the failure rules
// (division by zero, a negative base, non-finite input), a second image through differing windows,
// domain refusal, the pixel budget, cancellation and the table's cache identity.

#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"
#include "pixel_math_image_effect.hpp"
#include "pixel_math_test_support.hpp"

#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
namespace detail = runtime::detail;
using runtime::PixelMathOperation;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using Pixel = std::array<float, 4>;

bool near(const float a, const float b, const float tolerance = 1e-5F) {
    return std::fabs(a - b) <= tolerance * std::max(1.0F, std::fabs(b));
}
bool same(const Pixel& a, const Pixel& b, const float tolerance = 1e-5F) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) &&
           near(a[2], b[2], tolerance) && near(a[3], b[3], tolerance);
}

std::optional<detail::PreparedColorGrade> prepare(const runtime::PixelMathKernel& kernel) {
    std::vector<double> values;
    std::size_t count = 0;
    if (kernel.operation == PixelMathOperation::Clamp)
        count = 8;
    else if (kernel.operation == PixelMathOperation::MapRange)
        count = 4;
    else if (!kernel.second && kernel.operation != PixelMathOperation::Absolute)
        count = 1;
    for (std::size_t index = 0; index < count; ++index)
        values.push_back(std::get<double>(kernel.values[index].source));
    return detail::preparePixelMathGrade(kernel, values);
}

// A premultiplied pixel from straight color and alpha.
Pixel premultiplied(const float r, const float g, const float b, const float a) {
    return {r * a, g * a, b * a, a};
}

Pixel run(const runtime::PixelMathKernel& kernel, Pixel pixel, const Pixel* second = nullptr) {
    const auto grade = prepare(kernel);
    if (!grade)
        throw std::logic_error("the kernel prepares");
    const bool ok = detail::pixelMathPixel(grade->data, pixel, second);
    expect(ok, "a finite pixel always succeeds");
    return pixel;
}

constexpr auto kColor = document::kImagePixelColor;
constexpr auto kAll = document::kImagePixelColorAndAlpha;

void testArithmetic() {
    using namespace pixel_math_test;
    const Pixel opaque{0.25F, 0.5F, 2.0F, 1.0F};
    expect(same(run(math(1, PixelMathOperation::Add, 0.5, kColor, false), opaque),
                {0.75F, 1.0F, 2.5F, 1.0F}),
           "Add 0.5");
    expect(same(run(math(1, PixelMathOperation::Subtract, 0.5, kColor, false), opaque),
                {-0.25F, 0.0F, 1.5F, 1.0F}),
           "Subtract 0.5 may go negative");
    expect(same(run(math(1, PixelMathOperation::Multiply, 2.0, kColor, false), opaque),
                {0.5F, 1.0F, 4.0F, 1.0F}),
           "Multiply 2");
    expect(same(run(math(1, PixelMathOperation::Divide, 4.0, kColor, false), opaque),
                {0.0625F, 0.125F, 0.5F, 1.0F}),
           "Divide 4");
    expect(same(run(math(1, PixelMathOperation::Power, 2.0, kColor, false), opaque),
                {0.0625F, 0.25F, 4.0F, 1.0F}),
           "Power 2");
    expect(same(run(math(1, PixelMathOperation::Minimum, 0.4, kColor, false), opaque),
                {0.25F, 0.4F, 0.4F, 1.0F}),
           "Minimum 0.4");
    expect(same(run(math(1, PixelMathOperation::Maximum, 0.4, kColor, false), opaque),
                {0.4F, 0.5F, 2.0F, 1.0F}),
           "Maximum 0.4");
    expect(same(run(math(1, PixelMathOperation::Absolute, 0.0, kColor, false),
                    {-0.25F, 0.5F, -2.0F, 1.0F}),
                {0.25F, 0.5F, 2.0F, 1.0F}),
           "Absolute");

    // Failure rules: each yields 0.
    expect(same(run(math(1, PixelMathOperation::Divide, 0.0, kColor, false), opaque),
                {0.0F, 0.0F, 0.0F, 1.0F}),
           "divide by zero is 0");
    expect(same(run(math(1, PixelMathOperation::Power, 0.5, kColor, false),
                    {-0.25F, 0.25F, 4.0F, 1.0F}),
                {0.0F, 0.5F, 2.0F, 1.0F}),
           "a negative base to a fractional power is 0");
    expect(same(run(math(1, PixelMathOperation::Power, 0.0, kColor, false), {0.0F, 0.5F, 2.0F, 1.0F}),
                {1.0F, 1.0F, 1.0F, 1.0F}),
           "zero to the zero is one");
    expect(same(run(math(1, PixelMathOperation::Add, 1.0, kColor, false),
                    {std::numeric_limits<float>::quiet_NaN(),
                     std::numeric_limits<float>::infinity(), 0.5F, 1.0F}),
                {1.0F, 1.0F, 1.5F, 1.0F}),
           "NaN and infinity input channels are read as 0");
}

void testChannelsAndPremultiplication() {
    using namespace pixel_math_test;
    const auto half = premultiplied(0.8F, 0.4F, 0.2F, 0.5F);
    // Straight: color is multiplied in straight terms, alpha untouched.
    auto got = run(math(1, PixelMathOperation::Multiply, 0.5, kColor, true), half);
    expect(same(got, premultiplied(0.4F, 0.2F, 0.1F, 0.5F)), "straight Multiply keeps alpha");
    // Stored: the stored values are multiplied.
    got = run(math(1, PixelMathOperation::Multiply, 0.5, kColor, false), half);
    expect(same(got, {0.2F, 0.1F, 0.05F, 0.5F}), "stored Multiply scales the stored values");
    // One channel only.
    got = run(math(1, PixelMathOperation::Add, 1.0, document::kImagePixelGreen, false),
              {0.1F, 0.2F, 0.3F, 1.0F});
    expect(same(got, {0.1F, 1.2F, 0.3F, 1.0F}), "Green alone changes only green");
    // Alpha alone, straight: color re-weights with the new alpha, alpha limited to [0, 1].
    got = run(math(1, PixelMathOperation::Multiply, 3.0, document::kImagePixelAlpha, true), half);
    expect(same(got, premultiplied(0.8F, 0.4F, 0.2F, 1.0F)), "alpha is limited to one");
    got = run(math(1, PixelMathOperation::Subtract, 1.0, document::kImagePixelAlpha, false),
              {0.2F, 0.2F, 0.2F, 0.5F});
    expect(near(got[3], 0.0F), "alpha is limited to zero");
    got = run(math(1, PixelMathOperation::Add, 1.0, kAll, false), {0.0F, 0.0F, 0.0F, 0.25F});
    expect(near(got[0], 1.0F) && near(got[3], 1.0F), "Color and alpha changes all four");
}

void testMapRangeAndClamp() {
    using namespace pixel_math_test;
    // A depth pass: 2..12 onto 0..1.
    auto got = run(mapRange(1, 2.0, 12.0, 0.0, 1.0, false, kColor, false), {2.0F, 7.0F, 12.0F, 1.0F});
    expect(same(got, {0.0F, 0.5F, 1.0F, 1.0F}), "Map Range normalizes 2..12 to 0..1");
    got = run(mapRange(1, 2.0, 12.0, 0.0, 1.0, false, kColor, false), {-8.0F, 22.0F, 0.0F, 1.0F});
    expect(same(got, {-1.0F, 2.0F, -0.2F, 1.0F}), "without Clamp it extrapolates");
    got = run(mapRange(1, 2.0, 12.0, 0.0, 1.0, true, kColor, false), {-8.0F, 22.0F, 0.0F, 1.0F});
    expect(same(got, {0.0F, 1.0F, 0.0F, 1.0F}), "with Clamp it stays in the output range");
    got = run(mapRange(1, 0.0, 1.0, 1.0, -1.0, false, kColor, false), {0.25F, 0.5F, 1.0F, 1.0F});
    expect(same(got, {0.5F, 0.0F, -1.0F, 1.0F}), "a reversed output range inverts");
    got = run(mapRange(1, 0.0, 1.0, 1.0, -1.0, true, kColor, false), {2.0F, -2.0F, 0.0F, 1.0F});
    expect(same(got, {-1.0F, 1.0F, 1.0F, 1.0F}), "Clamp uses the ordered output range");
    got = run(mapRange(1, 3.0, 3.0, 0.25, 9.0, false, kColor, false), {1.0F, 3.0F, 5.0F, 1.0F});
    expect(same(got, {0.25F, 0.25F, 0.25F, 1.0F}), "an empty input range maps to the output minimum");

    // Clamp, with its own limits per channel.
    const auto clamp = clampKernel(1, {0.1, 0.9, 0.2, 0.8, 0.3, 0.7, 0.0, 0.5}, false);
    got = run(clamp, {0.0F, 1.0F, 0.5F, 0.75F});
    expect(same(got, {0.1F, 0.8F, 0.5F, 0.5F}), "Clamp limits each channel to its own range");
    const auto inverted = clampKernel(1, {0.9, 0.1, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0}, false);
    got = run(inverted, {0.5F, 0.5F, 0.5F, 1.0F});
    expect(near(got[0], 0.1F), "a minimum above the maximum is the maximum");
}

render::Rgba32fImage image(const std::int64_t ox, const std::int64_t oy, const std::uint32_t w,
                           const std::uint32_t h, const Pixel& fill) {
    const auto window = render::ImageWindow::create(ox, oy, w, h);
    auto descriptor = render::Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (auto y = oy; y < oy + static_cast<std::int64_t>(h); ++y)
        for (auto x = ox; x < ox + static_cast<std::int64_t>(w); ++x) {
            const auto stored = render::Rgba32f::fromPremultiplied(fill[0], fill[1], fill[2], fill[3]);
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

void testSecondImageAndBudget() {
    using namespace pixel_math_test;
    const auto input = image(0, 0, 6, 4, {0.5F, 0.5F, 0.5F, 1.0F});
    const auto second = image(2, 1, 3, 2, {0.25F, 1.0F, 2.0F, 1.0F});
    auto kernel = math(1, PixelMathOperation::Multiply, 0.0, kColor, false, true);
    const auto grade = prepare(kernel);
    expect(grade && detail::pixelMathReadsSecond(grade->data), "a second image is read");
    const auto result = detail::applyPixelMath(*grade, input, &second, 1U << 26U, {},
                                               runtime::OperationIndex::fromRaw(0), {});
    expect(result.image != nullptr, "the pass applies");
    if (!result.image)
        return;
    expect(same(at(*result.image, 3, 2), {0.125F, 0.5F, 1.0F, 1.0F}),
           "inside the second image the pixels multiply");
    expect(same(at(*result.image, 0, 0), {0.0F, 0.0F, 0.0F, 1.0F}),
           "outside it the operand is transparent black");
    expect(result.image->descriptor()->dataWindow().extent().width() == 6, "the input's window");
    // Without a second image connected, the operand is transparent black too.
    const auto none = detail::applyPixelMath(*grade, input, nullptr, 1U << 26U, {},
                                             runtime::OperationIndex::fromRaw(0), {});
    expect(none.image && same(at(*none.image, 3, 2), {0.0F, 0.0F, 0.0F, 1.0F}),
           "an absent second image is transparent black");

    const auto refused = detail::applyPixelMath(*grade, input, &second, 4, {},
                                                runtime::OperationIndex::fromRaw(0), {});
    expect(!refused.image && refused.diagnostic.has_value(), "a refused budget is an explicit error");
}

void testDomainAndIdentity() {
    using namespace pixel_math_test;
    auto kernel = math(1, PixelMathOperation::Add, 0.0, kColor, false);
    const std::array tooBig{2.0e6};
    expect(!detail::preparePixelMathGrade(kernel, tooBig), "a value beyond the domain is refused");
    const std::array nan{std::numeric_limits<double>::quiet_NaN()};
    expect(!detail::preparePixelMathGrade(kernel, nan), "a NaN value is refused");
    const auto a = prepare(math(1, PixelMathOperation::Add, 0.5, kColor, false));
    const auto b = prepare(math(1, PixelMathOperation::Add, 0.5, kColor, false));
    const auto c = prepare(math(1, PixelMathOperation::Add, 0.75, kColor, false));
    const auto d = prepare(math(1, PixelMathOperation::Add, 0.5, kColor, true));
    expect(a && b && c && d, "the tables prepare");
    expect(a->data == b->data, "equal settings give an equal table");
    expect(a->data != c->data && a->data != d->data, "a changed value or mode changes the table");
    expect(!a->identity, "the pass is never an identity alias");
    // Add of zero is still a pass: it replaces non-finite values.
    const auto zero = prepare(math(1, PixelMathOperation::Add, 0.0, kColor, false));
    expect(zero && !zero->identity, "Add zero is still a pass");
}

void testCancellation() {
    using namespace pixel_math_test;
    const auto input = image(0, 0, 64, 64, {0.5F, 0.5F, 0.5F, 1.0F});
    const auto grade = prepare(math(1, PixelMathOperation::Add, 1.0, kColor, false));
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(481)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Image Math cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result = detail::applyPixelMath(*grade, input, nullptr, 1U << 26U,
                                                       context.cancellation(),
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
           "a cancelled pass publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

// BLOOM_PIXEL_MATH_ARTIFACT_DIR: a sheet of a depth ramp (2 to 12 across 256 columns) and what each
// node makes of it, one row each (display-clamped sRGB), and the values read at three columns.
void writeCapture() {
    const char* directory = std::getenv("BLOOM_PIXEL_MATH_ARTIFACT_DIR");
    if (directory == nullptr || *directory == '\0')
        return;
    using namespace pixel_math_test;
    constexpr std::uint32_t width = 256;
    constexpr std::uint32_t height = 8;
    const auto window = render::ImageWindow::create(0, 0, width, height);
    auto descriptor = render::Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (std::int64_t y = 0; y < height; ++y)
        for (std::int64_t x = 0; x < width; ++x) {
            const float depth = 2.0F + 10.0F * static_cast<float>(x) / 255.0F;
            const auto stored = render::Rgba32f::fromPremultiplied(depth, depth, depth, 1.0F);
            if (!stored || builder.value()->write(x, y, *stored.value()))
                throw std::logic_error("capture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    const render::Rgba32fImage ramp = std::move(*frozen.value());
    const auto apply = [](const runtime::PixelMathKernel& kernel, const render::Rgba32fImage& in) {
        const auto grade = prepare(kernel);
        const auto result = detail::applyPixelMath(*grade, in, nullptr, 1U << 26U, {},
                                                   runtime::OperationIndex::fromRaw(0), {});
        if (!result.image)
            throw std::logic_error("the capture pass applies");
        return result.image;
    };
    const auto normalized = apply(mapRange(1, 2.0, 12.0, 0.0, 1.0, false, kColor, false), ramp);
    const auto halved = apply(math(1, PixelMathOperation::Multiply, 0.5, kColor, false), *normalized);
    const auto clamped =
        apply(clampKernel(1, {0.25, 0.75, 0.25, 0.75, 0.25, 0.75, 0.0, 1.0}, false), *normalized);
    const auto squared = apply(math(1, PixelMathOperation::Power, 2.0, kColor, false), *normalized);
    const std::array<std::pair<const char*, const render::Rgba32fImage*>, 5> rows{
        {{"depth 2..12 (raw)", &ramp},
         {"Map Range 2..12 -> 0..1", normalized.get()},
         {"then Multiply 0.5", halved.get()},
         {"Map Range then Clamp 0.25..0.75", clamped.get()},
         {"Map Range then Power 2", squared.get()}}};
    const auto encode = [](const double linear) {
        const double v = std::clamp(linear, 0.0, 1.0);
        const double e = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        return static_cast<unsigned char>(std::lround(e * 255.0));
    };
    constexpr int rowHeight = 24;
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * rowHeight * rows.size() * 3);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (int y = 0; y < rowHeight; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto p = at(*rows[row].second, x, 0);
                const auto offset =
                    ((row * rowHeight + static_cast<std::size_t>(y)) * width + x) * 3;
                for (std::size_t c = 0; c < 3; ++c)
                    rgb[offset + c] = encode(static_cast<double>(p[c]));
            }
        const auto a = at(*rows[row].second, 0, 0);
        const auto b = at(*rows[row].second, 128, 0);
        const auto c = at(*rows[row].second, 255, 0);
        std::cout << rows[row].first << ": x0=" << a[0] << " x128=" << b[0] << " x255=" << c[0]
                  << '\n';
    }
    std::ofstream out(std::string(directory) + "/image-math-ramp.ppm", std::ios::binary);
    out << "P6\n" << width << ' ' << rowHeight * rows.size() << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

} // namespace

int main() {
    try {
        testArithmetic();
        testChannelsAndPremultiplication();
        testMapRangeAndClamp();
        testSecondImageAndBudget();
        testDomainAndIdentity();
        testCancellation();
        writeCapture();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "pixel math tests passed\n";
    return 0;
}
