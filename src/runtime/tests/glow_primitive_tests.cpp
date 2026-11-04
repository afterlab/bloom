// #506 Glow's two primitives on the CPU reference. Directional Blur: the prepared taps (bilinear
// texels of four exponentially weighted ray taps), the window growth along the ray, an independent
// direct bilinear ray sum over an HDR, signed, partial-alpha image, identity at zero spacing, domain
// refusals, the fixed program and its data, cancellation and budget refusal. Highlight Roll-off: the
// associated-colour curve against its formula, alpha untouched, identity at zero, domains and the
// fixed program.

#include "image_effect.hpp"
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using runtime::DirectionalBlurKernel;
using runtime::HighlightRollOffKernel;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << message << '\n';
        ++failures;
    }
}

// A 5 x 4 HDR, signed, partial-alpha image at (-2, 3) on an 8 x 8 display window.
render::Rgba32fImage source() {
    const auto data = render::ImageWindow::create(-2, 3, 5, 4);
    const auto display = render::ImageWindow::create(0, 0, 8, 8);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 20U);
    for (std::int64_t y = 3; y < 7; ++y)
        for (std::int64_t x = -2; x < 3; ++x) {
            const float a = 0.25F + 0.15F * static_cast<float>((x + 2 + y) % 5);
            const float r = static_cast<float>(x) * 0.7F + 3.0F;
            const auto pixel = render::Rgba32f::fromPremultiplied(
                r * a, -0.25F * a, static_cast<float>(y) * 0.5F * a, a);
            if (builder.value()->write(x, y, *pixel.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto image = std::move(*builder.value()).freeze();
    return std::move(*image.value());
}

DirectionalBlurKernel directional(const double decay) {
    return {{document::ParameterId::fromRaw(1), 0.0}, {document::ParameterId::fromRaw(2), 1.0},
            decay};
}

std::array<double, 4> pixelAt(const render::Rgba32fImage& image, const std::int64_t x,
                              const std::int64_t y) {
    const auto pixel = image.read(x, y);
    if (!pixel)
        return {};
    const auto c = pixel.value()->components();
    return {static_cast<double>(c[0]), static_cast<double>(c[1]), static_cast<double>(c[2]),
            static_cast<double>(c[3])};
}

// The ray written directly: tap i at c - i * spacing * (cos a, sin a), bilinear, weight decay^i.
std::array<double, 4> rayReference(const render::Rgba32fImage& input, const std::int64_t x,
                                   const std::int64_t y, const double angle, const double spacing,
                                   const double decay) {
    const double radians = angle * std::numbers::pi / 180.0;
    double total = 0.0;
    for (int i = 0; i < 4; ++i)
        total += std::pow(decay, i);
    std::array<double, 4> sum{};
    for (int i = 0; i < 4; ++i) {
        const double px = static_cast<double>(x) - i * spacing * std::cos(radians);
        const double py = static_cast<double>(y) - i * spacing * std::sin(radians);
        const double fx = std::floor(px);
        const double fy = std::floor(py);
        const double tx = px - fx;
        const double ty = py - fy;
        const auto ix = static_cast<std::int64_t>(fx);
        const auto iy = static_cast<std::int64_t>(fy);
        const std::array<std::array<double, 3>, 4> texels{{{0, 0, (1 - tx) * (1 - ty)},
                                                           {1, 0, tx * (1 - ty)},
                                                           {0, 1, (1 - tx) * ty},
                                                           {1, 1, tx * ty}}};
        for (const auto& texel : texels) {
            const auto value = pixelAt(input, ix + static_cast<std::int64_t>(texel[0]),
                                       iy + static_cast<std::int64_t>(texel[1]));
            for (std::size_t c = 0; c < 4; ++c)
                sum[c] += std::pow(decay, i) / total * texel[2] * value[c];
        }
    }
    return sum;
}

void testDirectionalReference() {
    const auto input = source();
    for (const auto& [angle, spacing, decay] :
         std::vector<std::array<double, 3>>{{0.0, 1.0, 0.8},
                                            {90.0, 2.0, 1.0},
                                            {200.0, 1.37, 0.6},
                                            {-35.0, 0.4, 0.95}}) {
        const auto blur = runtime::detail::prepareDirectionalBlur(directional(decay), angle,
                                                                  spacing, 1.0, 1.0);
        expect(blur.has_value() && !blur->identity, "a directional pass prepares");
        if (!blur)
            continue;
        const auto result = runtime::detail::applyDirectionalBlur(
            *blur, input, 1U << 22U, {}, runtime::OperationIndex::fromRaw(0), {});
        expect(result.image && !result.diagnostic, "a directional pass publishes an image");
        if (!result.image)
            continue;
        const auto window = result.image->descriptor()->dataWindow();
        expect(result.image->descriptor()->displayWindow() == input.descriptor()->displayWindow(),
               "the display window is kept");
        // The window grows by the reach in the ray's direction only.
        const double radians = angle * std::numbers::pi / 180.0;
        const double reachX = 3.0 * spacing * std::cos(radians);
        const double reachY = 3.0 * spacing * std::sin(radians);
        const auto original = input.descriptor()->dataWindow();
        expect(window.originX() == original.originX() + std::min<std::int64_t>(
                                                            0, static_cast<std::int64_t>(
                                                                   std::floor(reachX + 1e-9))) &&
                   window.maxXExclusive() ==
                       original.maxXExclusive() +
                           std::max<std::int64_t>(
                               0, static_cast<std::int64_t>(std::ceil(reachX - 1e-9))) &&
                   window.originY() ==
                       original.originY() +
                           std::min<std::int64_t>(
                               0, static_cast<std::int64_t>(std::floor(reachY + 1e-9))) &&
                   window.maxYExclusive() ==
                       original.maxYExclusive() +
                           std::max<std::int64_t>(
                               0, static_cast<std::int64_t>(std::ceil(reachY - 1e-9))),
               "the window grows along the ray by its reach, angle " + std::to_string(angle));
        bool matches = true;
        for (auto y = window.originY() - 1; y <= window.maxYExclusive(); ++y)
            for (auto x = window.originX() - 1; x <= window.maxXExclusive(); ++x) {
                const auto expected = rayReference(input, x, y, angle, spacing, decay);
                const auto actual = pixelAt(*result.image, x, y);
                for (std::size_t c = 0; c < 4; ++c)
                    matches = matches && std::abs(actual[c] - expected[c]) <=
                                             2e-6 * std::max(1.0, std::abs(expected[c]));
            }
        expect(matches, "a directional pass equals the direct bilinear ray sum, angle " +
                            std::to_string(angle));
    }
}

void testDirectionalDomainsAndProgram() {
    const auto zero = runtime::detail::prepareDirectionalBlur(directional(0.5), 45.0, 0.0, 1.0,
                                                              1.0);
    expect(zero && zero->identity, "a zero spacing is the exact identity");
    runtime::CompiledImageEffect effect{document::NodeId::fromRaw(1),
                                        runtime::OperationIndex::fromRaw(0), directional(0.5)};
    runtime::detail::ImageEffectContext context;
    expect(context
               .prepare(effect, {}, {}, {}, {}, {}, 1.0, 1.0, {}, {}, {}, {}, {},
                        std::array{45.0, 0.0})
               .identity(),
           "an evaluator pass at zero spacing aliases its input");
    expect(!runtime::detail::prepareDirectionalBlur(directional(0.5), 0.0, -1.0, 1.0, 1.0) &&
               !runtime::detail::prepareDirectionalBlur(directional(0.5), 0.0, 1025.0, 1.0, 1.0) &&
               !runtime::detail::prepareDirectionalBlur(directional(0.0), 0.0, 1.0, 1.0, 1.0) &&
               !runtime::detail::prepareDirectionalBlur(directional(1.5), 0.0, 1.0, 1.0, 1.0) &&
               !runtime::detail::prepareDirectionalBlur(directional(0.5), std::nan(""), 1.0, 1.0,
                                                        1.0) &&
               !runtime::detail::prepareDirectionalBlur(directional(0.5), 0.0, 1.0, 0.0, 1.0),
           "out-of-domain spacing, decay, angle or scale is refused");
    // 90 degrees: the cosine's rounding error snaps away, so the window grows vertically only.
    const auto vertical =
        runtime::detail::prepareDirectionalBlur(directional(1.0), 90.0, 2.0, 1.0, 1.0);
    const auto square = render::ImageWindow::create(0, 0, 4, 4);
    const auto grown =
        vertical ? runtime::detail::directionalBlurOutputWindow(*vertical, *square.value())
                 : std::nullopt;
    expect(grown && grown->originX() == 0 && grown->extent().width() == 4 &&
               grown->originY() == 0 && grown->extent().height() == 10,
           "a vertical ray grows the window by exactly its reach, and only vertically");
    // A proxy scale shortens the ray in device pixels.
    const auto proxy =
        runtime::detail::prepareDirectionalBlur(directional(1.0), 0.0, 2.0, 0.5, 0.5);
    expect(proxy && proxy->dx[12] == -3 && proxy->weights[12] > 0.0F,
           "the evaluation scale shortens the ray");
    const auto data = runtime::detail::directionalBlurProgramData(*vertical);
    const auto program = runtime::detail::buildDirectionalBlurProgram(data);
    expect(data.size() == 64 && program.succeeded() &&
               render::validateOcioGpuProgram(*program.program(), {}) ==
                   render::OcioGpuProgramError::None &&
               program.program()->stage == render::OcioGpuProgramStage::ProcessNeighborhood &&
               program.program()->uniformBufferStorage &&
               program.program()->uniformBufferData.size() == 64 * sizeof(float),
           "the pass is one fixed program whose taps are storage-buffer data");
    const auto other = runtime::detail::buildDirectionalBlurProgram(
        runtime::detail::directionalBlurProgramData(*proxy));
    expect(other.succeeded() && other.program()->shaderText == program.program()->shaderText &&
               other.program()->uniformBufferData != program.program()->uniformBufferData,
           "another angle or spacing changes only the program's data");
    auto bad = data;
    bad[0] = 0.5F;
    expect(!runtime::detail::buildDirectionalBlurProgram(bad).succeeded() &&
               !runtime::detail::buildDirectionalBlurProgram({}).succeeded(),
           "a fractional offset or malformed data is refused");
}

// Cancellation arriving after the first row stops the pass; it publishes nothing.
void testDirectionalCancellationAndBudget() {
    const auto input = source();
    const auto blur =
        runtime::detail::prepareDirectionalBlur(directional(0.7), 30.0, 3.0, 1.0, 1.0);
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(506)};
    std::atomic_bool published{true};
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Directional blur cancellation", owner),
        [&](runtime::TaskContext& context) {
            const auto result = runtime::detail::applyDirectionalBlur(
                *blur, input, 1U << 22U, context.cancellation(),
                runtime::OperationIndex::fromRaw(0),
                [&](const runtime::EvaluationProgress& progress) {
                    if (progress.completed == 1)
                        static_cast<void>(scheduler.cancelOwner(owner));
                });
            published = !result.cancelled || result.image != nullptr;
            return runtime::TaskResult<void>::cancelled();
        });
    expect(submission.accepted(), "the cancellation task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> outcome;
    while (!outcome && std::chrono::steady_clock::now() < deadline) {
        outcome = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(outcome && outcome->state() == runtime::TaskState::Cancelled && !published,
           "a pass cancelled after its first row publishes nothing");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(scheduler.isQuiescent(), "the cancelled pass shuts down safely");
    const auto refused = runtime::detail::applyDirectionalBlur(
        *blur, input, 64, {}, runtime::OperationIndex::fromRaw(0), {});
    expect(!refused.image && refused.diagnostic &&
               refused.diagnostic->code ==
                   runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "a pass beyond the pixel budget is refused");
}

void testRollOff() {
    const auto input = source();
    const auto prepare = [](const double amount) {
        return runtime::detail::prepareColorGrade(
            HighlightRollOffKernel{{document::ParameterId::fromRaw(3), amount}},
            std::vector<double>{amount});
    };
    const auto zero = prepare(0.0);
    expect(zero && zero->identity, "a zero roll-off is the identity");
    expect(!prepare(-0.1) && !prepare(100.5) && !prepare(std::nan("")),
           "an out-of-domain roll-off is refused");
    const auto grade = prepare(0.75);
    const auto result = runtime::detail::applyColorGrade(*grade, input, 1U << 22U, {},
                                                         runtime::OperationIndex::fromRaw(0), {});
    expect(result.image && result.image->descriptor()->dataWindow() ==
                               input.descriptor()->dataWindow(),
           "the roll-off keeps the windows");
    bool matches = static_cast<bool>(result.image);
    const auto window = input.descriptor()->dataWindow();
    for (auto y = window.originY(); matches && y < window.maxYExclusive(); ++y)
        for (auto x = window.originX(); x < window.maxXExclusive(); ++x) {
            const auto in = pixelAt(input, x, y);
            const auto out = pixelAt(*result.image, x, y);
            const double luma = 0.2126 * in[0] + 0.7152 * in[1] + 0.0722 * in[2];
            const double divisor = 1.0 + 0.75 * std::max(luma, 0.0);
            for (std::size_t c = 0; c < 3; ++c)
                matches = matches && std::abs(out[c] - in[c] / divisor) <=
                                         2e-6 * std::max(1.0, std::abs(in[c]));
            matches = matches && out[3] == in[3];
        }
    expect(matches, "the roll-off is c / (1 + amount * max(luma, 0)) with alpha untouched");
    const auto program = runtime::detail::buildColorGradeProgram(grade->kind, grade->data);
    const auto other = runtime::detail::buildColorGradeProgram(grade->kind, prepare(2.0)->data);
    expect(program.succeeded() && other.succeeded() &&
               render::validateOcioGpuProgram(*program.program(), {}) ==
                   render::OcioGpuProgramError::None &&
               program.program()->shaderText == other.program()->shaderText &&
               program.program()->semanticsId == "bloom.render.highlight-roll-off.v1",
           "the roll-off is one fixed grade program whose amount is data");
    expect(!runtime::detail::buildColorGradeProgram(grade->kind, {0.5F, 0.2F}).succeeded(),
           "a malformed roll-off table is refused");
}

} // namespace

int main() {
    try {
        testDirectionalReference();
        testDirectionalDomainsAndProgram();
        testDirectionalCancellationAndBudget();
        testRollOff();
    } catch (const std::exception& error) {
        std::cerr << "glow primitive tests threw: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " glow primitive expectation(s) failed\n";
        return 1;
    }
    std::cout << "glow primitive tests passed\n";
    return 0;
}
