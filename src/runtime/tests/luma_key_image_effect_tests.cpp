// #338 Luma Key: the CPU reference against an independent double-precision straight-colour luma
// over HDR, negative and partial-alpha pixels; ramp, inverted and hard keys; windows, budget,
// cancellation; and the resident program the GPU route prepares.

#include "image_effect.hpp"
#include <bloom/runtime/gpu_ocio_wrapper.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace bloom;
void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
runtime::LumaKeyKernel kernel(const double low, const double high, const bool invert) {
    return {{document::ParameterId::fromRaw(1), low},
            {document::ParameterId::fromRaw(2), high},
            invert};
}
// Straight RGBA authoring values premultiplied into the resident representation.
constexpr std::array<std::array<float, 4>, 12> kPixels{{{0.0F, 0.0F, 0.0F, 1.0F},
                                                        {1.0F, 1.0F, 1.0F, 1.0F},
                                                        {0.5F, 0.5F, 0.5F, 1.0F},
                                                        {0.25F, 0.75F, 0.1F, 0.5F},
                                                        {4.0F, 3.0F, 6.0F, 1.0F},
                                                        {-0.5F, 0.2F, -1.0F, 1.0F},
                                                        {0.8F, 0.1F, 0.9F, 0.25F},
                                                        {0.0F, 0.0F, 0.0F, 0.0F},
                                                        {2.0F, 2.0F, 2.0F, 0.125F},
                                                        {0.3F, 0.6F, 0.2F, 0.75F},
                                                        {0.6F, 0.4F, 0.5F, 1.0F},
                                                        {0.1F, 0.1F, 0.1F, 0.0625F}}};
render::Rgba32fImage source() {
    const auto data = render::ImageWindow::create(-2, 3, 4, 3);
    const auto display = render::ImageWindow::create(0, 0, 8, 8);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    for (std::size_t i = 0; i < kPixels.size(); ++i) {
        const auto& s = kPixels[i];
        const auto p = render::Rgba32f::fromPremultiplied(s[0] * s[3], s[1] * s[3], s[2] * s[3],
                                                          s[3]);
        require(p.hasValue() &&
                    !builder.value()->write(-2 + static_cast<std::int64_t>(i % 4),
                                            3 + static_cast<std::int64_t>(i / 4), *p.value()),
                "luma key input writes");
    }
    auto image = std::move(*builder.value()).freeze();
    require(image.hasValue(), "luma key input freezes");
    return std::move(*image.value());
}
// Independent reference: alpha times the ramp of the straight Rec. 709 luma, in double.
double expectedCoverage(const render::Rgba32f p, const double low, const double high,
                        const bool invert) {
    const double a = static_cast<double>(p.alpha());
    if (a == 0.0)
        return 0.0;
    const double luma = (0.2126 * static_cast<double>(p.red()) +
                         0.7152 * static_cast<double>(p.green()) +
                         0.0722 * static_cast<double>(p.blue())) /
                        a;
    double key = 0.0;
    if (high > low)
        key = std::clamp((luma - low) / (high - low), 0.0, 1.0);
    else
        key = luma >= low ? 1.0 : 0.0;
    if (invert)
        key = 1.0 - key;
    return a * key;
}
void referenceTests() {
    const auto input = source();
    struct Case final {
        double low;
        double high;
        bool invert;
    };
    for (const auto [low, high, invert] :
         {Case{0.0, 1.0, false}, Case{0.0, 1.0, true}, Case{0.2, 0.6, false},
          Case{0.2, 0.6, true}, Case{1.0, 5.0, false}, Case{-0.5, 0.25, true},
          Case{0.4, 0.4, false}, Case{0.4, 0.4, true}, Case{0.7, 0.3, false}}) {
        const auto key = runtime::detail::prepareLumaKey(kernel(low, high, invert), low, high);
        require(key.has_value(), "luma key prepares");
        require((high > low) == (key->mode == runtime::detail::LumaKeyMode::Ramp),
                "high <= low prepares a hard key");
        const auto result = runtime::detail::applyLumaKey(*key, input, 65536, {},
                                                          runtime::OperationIndex::fromRaw(0), {});
        require(result.image && !result.diagnostic, "luma key publishes");
        require(result.image->descriptor()->dataWindow() == input.descriptor()->dataWindow() &&
                    result.image->descriptor()->displayWindow() ==
                        input.descriptor()->displayWindow(),
                "luma key keeps the input data and display windows");
        const auto window = input.descriptor()->dataWindow();
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
            for (auto x = window.originX(); x < window.maxXExclusive(); ++x) {
                const auto sourceSample = input.read(x, y);
                const auto actualSample = result.image->read(x, y);
                require(sourceSample.hasValue() && actualSample.hasValue(), "luma key samples");
                const auto source = *sourceSample.value();
                const auto actual = *actualSample.value();
                const double expected = expectedCoverage(source, low, high, invert);
                require(std::abs(static_cast<double>(actual.alpha()) - expected) < 2e-6,
                        "luma key coverage equals alpha times the straight-luma ramp");
                require(actual.red() == actual.alpha() && actual.green() == actual.alpha() &&
                            actual.blue() == actual.alpha(),
                        "luma key writes premultiplied white");
                require(actual.alpha() >= 0.0F && actual.alpha() <= source.alpha(),
                        "luma key coverage never exceeds the pixel's alpha");
                if (source.alpha() == 0.0F)
                    require(actual.alpha() == 0.0F && !std::signbit(actual.alpha()),
                            "a transparent pixel has canonical zero coverage either way");
            }
        require(!runtime::detail::applyLumaKey(*key, input, 1, {},
                                               runtime::OperationIndex::fromRaw(0), {})
                     .image,
                "luma key budget refuses before publication");
    }
    // The ramp and its inversion are complements within the pixel's alpha.
    const auto ramp = runtime::detail::prepareLumaKey(kernel(0.1, 0.9, false), 0.1, 0.9);
    const auto inverted = runtime::detail::prepareLumaKey(kernel(0.1, 0.9, true), 0.1, 0.9);
    for (const auto& s : kPixels) {
        const float a = s[3];
        const float sum = runtime::detail::lumaKeyCoverage(*ramp, s[0] * a, s[1] * a, s[2] * a, a) +
                          runtime::detail::lumaKeyCoverage(*inverted, s[0] * a, s[1] * a,
                                                           s[2] * a, a);
        require(std::abs(sum - a) < 2e-6F, "an inverted key is the complement within alpha");
    }
    // Domain: non-finite or out-of-range thresholds refuse; a ramp too steep for binary32 is hard.
    require(!runtime::detail::prepareLumaKey(kernel(0, 1, false), std::nullopt, 1.0) &&
                !runtime::detail::prepareLumaKey(kernel(0, 1, false), 0.0,
                                                 std::numeric_limits<double>::quiet_NaN()) &&
                !runtime::detail::prepareLumaKey(kernel(0, 1, false), -2.0e6, 1.0),
            "luma key refuses missing, non-finite and out-of-domain thresholds");
    const auto steep = runtime::detail::prepareLumaKey(kernel(0, 1, false), 0.5, 0.5 + 1e-300);
    require(steep && steep->mode == runtime::detail::LumaKeyMode::Hard,
            "a ramp narrower than binary32 can express is a hard key");
}
void programTests() {
    const auto key = runtime::detail::prepareLumaKey(kernel(0.2, 0.7, true), 0.2, 0.7);
    const auto data = runtime::detail::lumaKeyProgramData(*key);
    require(data[0] == 0.7F && data[1] == -static_cast<float>(1.0 / 0.5) && data[2] == 0.0F,
            "the program data carries the inverted pivot and slope");
    auto program = runtime::detail::buildLumaKeyProgram(data);
    require(program.succeeded() && render::validateOcioGpuProgram(*program.program(), {}) ==
                                       render::OcioGpuProgramError::None,
            "luma key program prepares and validates");
    const auto& desc = *program.program();
    require(desc.semanticsId == "bloom.render.luma-key.v1" &&
                desc.stage == render::OcioGpuProgramStage::ProcessNeighborhood &&
                desc.uniformBufferStorage && desc.uniformBufferSize == sizeof(data) &&
                std::memcmp(desc.uniformBufferData.data(), data.data(), sizeof(data)) == 0 &&
                desc.shaderText.find("precise float d = fma(-k.x, p.a, luma);") !=
                    std::string::npos &&
                desc.shaderText.find("coverage = min(max(v, 0.0), p.a);") != std::string::npos,
            "luma key program runs the reference operations over its storage data");
    const auto other = runtime::detail::buildLumaKeyProgram(runtime::detail::lumaKeyProgramData(
        *runtime::detail::prepareLumaKey(kernel(0.0, 1.0, false), 0.0, 1.0)));
    require(other.succeeded() && other.program()->shaderText == desc.shaderText,
            "changed thresholds change only the program data, never the shader");
    const auto wrapper = runtime::buildGpuOcioWrapperGlsl(desc);
    require(wrapper.succeeded() &&
                wrapper.source.find("vec4 p = bloom_luma_key(c)") != std::string::npos,
            "luma key runs in the resident neighborhood wrapper");
    require(!runtime::detail::buildLumaKeyProgram({0.0F, 1.0F, 3.0F, 0.0F}).succeeded() &&
                !runtime::detail::buildLumaKeyProgram(
                     {std::numeric_limits<float>::infinity(), 1.0F, 0.0F, 0.0F})
                     .succeeded(),
            "an unknown mode or non-finite data refuses");
}
void cancellationTest() {
    const auto input = source();
    const auto key = runtime::detail::prepareLumaKey(kernel(0.0, 1.0, false), 0.0, 1.0);
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(84)};
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Luma Key cancellation", owner), [&](runtime::TaskContext& context) {
            const auto result = runtime::detail::applyLumaKey(
                *key, input, 65536, context.cancellation(), runtime::OperationIndex::fromRaw(0),
                [&](const runtime::EvaluationProgress& progress) {
                    if (progress.completed == 1)
                        static_cast<void>(scheduler.cancelOwner(owner));
                });
            require(result.cancelled && !result.image, "row cancellation publishes no image");
            return runtime::TaskResult<void>::cancelled();
        });
    require(submission.accepted(), "cancellation task admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> result;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    require(result && result->state() == runtime::TaskState::Cancelled,
            "row cancellation observed");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(scheduler.isQuiescent(), "luma key cancellation shuts down safely");
}
} // namespace
int main() {
    try {
        referenceTests();
        programTests();
        cancellationTest();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: Luma Key ramp/inverted/hard reference, HDR/alpha/windows, budget, "
                 "resident program and cancellation\n";
}
