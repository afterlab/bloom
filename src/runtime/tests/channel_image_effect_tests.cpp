// #479 Separate Channels and Combine Channels: the Channel Select and Channel Combine CPU reference
// over HDR, signed and partial-alpha pixels and Raw data passes (depth, normals, IDs) -- stored
// values move bit for bit with no colour transform and no clamp except alpha's; Luma; the alpha
// modes; windows, an absent or smaller second image, domain refusals, budget, cancellation, and
// the resident Channel Select program the GPU route prepares.

#include "image_effect.hpp"
#include <bloom/runtime/gpu_ocio_wrapper.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using runtime::ImageChannelSource;
void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

// Premultiplied HDR, signed, partial-alpha and transparent pixels, and Raw data values (a 1e4
// depth, signed unit normals, an integer ID), in a 4x3 data window at (-2, 3).
const std::vector<std::array<float, 4>> kPixels{
    {0.25F, 0.5F, 0.75F, 1.0F},  {-0.3F, 1.75F, 6.0F, 0.625F},  {4.0F, -2.0F, 0.125F, 0.25F},
    {0.0F, 0.0F, 0.0F, 0.0F},    {12000.0F, 3.5F, -7.0F, 1.0F}, {-0.6F, 0.8F, 0.0F, 1.0F},
    {42.0F, 7.0F, 1.0F, 1.0F},   {0.02F, 0.03F, -0.01F, 0.05F}, {1.0e-3F, 2.0F, 3.0F, 0.5F},
    {-1.0F, -1.0F, -1.0F, 1.0F}, {65504.0F, 0.0F, 1.0F, 0.75F}, {0.5F, 0.5F, 0.5F, 0.5F}};

render::Rgba32fImage image(const std::vector<std::array<float, 4>>& pixels, const std::int64_t x,
                           const std::int64_t y, const std::uint32_t width) {
    const auto height = static_cast<std::uint32_t>(pixels.size() / width);
    const auto data = render::ImageWindow::create(x, y, width, height);
    const auto display = render::ImageWindow::create(0, 0, 8, 8);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const auto& p = pixels[i];
        const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
        require(pixel.hasValue() && !builder.value()->write(
                                        x + static_cast<std::int64_t>(i % width),
                                        y + static_cast<std::int64_t>(i / width), *pixel.value()),
                "channel input writes");
    }
    auto frozen = std::move(*builder.value()).freeze();
    require(frozen.hasValue(), "channel input freezes");
    return std::move(*frozen.value());
}
render::Rgba32fImage input() { return image(kPixels, -2, 3, 4); }

std::array<float, 4> components(const render::Rgba32f p) {
    return {p.red(), p.green(), p.blue(), p.alpha()};
}
double luma(const render::Rgba32f p) {
    return 0.2126 * static_cast<double>(p.red()) + 0.7152 * static_cast<double>(p.green()) +
           0.0722 * static_cast<double>(p.blue());
}

runtime::detail::PreparedChannelSelect select(const std::array<ImageChannelSource, 4> sources,
                                              const bool premultiply = false) {
    const auto prepared =
        runtime::detail::prepareChannelSelect(runtime::ChannelSelectKernel{sources, premultiply});
    require(prepared.has_value(), "channel select prepares");
    return *prepared;
}
std::shared_ptr<const render::Rgba32fImage> apply(const runtime::detail::PreparedChannelSelect& s,
                                                  const render::Rgba32fImage& source) {
    const auto result = runtime::detail::applyChannelSelect(
        s, source, 65536, {}, runtime::OperationIndex::fromRaw(0), {});
    require(result.image && !result.diagnostic, "channel select publishes");
    require(result.image->descriptor()->dataWindow() == source.descriptor()->dataWindow() &&
                result.image->descriptor()->displayWindow() ==
                    source.descriptor()->displayWindow() &&
                result.image->descriptor()->pixelAspect() == source.descriptor()->pixelAspect(),
            "channel select keeps the input windows and pixel aspect");
    return result.image;
}
std::shared_ptr<const render::Rgba32fImage>
combine(const runtime::detail::PreparedChannelCombine& c, const render::Rgba32fImage& base,
        const render::Rgba32fImage* second) {
    const auto result = runtime::detail::applyChannelCombine(
        c, base, second, 65536, {}, runtime::OperationIndex::fromRaw(0), {});
    require(result.image && !result.diagnostic, "channel combine publishes");
    require(result.image->descriptor()->dataWindow() == base.descriptor()->dataWindow() &&
                result.image->descriptor()->displayWindow() == base.descriptor()->displayWindow(),
            "channel combine keeps the base windows");
    return result.image;
}
runtime::detail::PreparedChannelCombine combineKernel(const std::uint8_t slot,
                                                      const ImageChannelSource source,
                                                      const bool premultiply = false) {
    const auto prepared = runtime::detail::prepareChannelCombine(
        runtime::ChannelCombineKernel{slot, source, premultiply});
    require(prepared.has_value(), "channel combine prepares");
    return *prepared;
}
render::Rgba32f at(const render::Rgba32fImage& source, const std::int64_t x, const std::int64_t y) {
    const auto sample = source.read(x, y);
    require(sample.hasValue(), "channel sample reads");
    return *sample.value();
}

// Separate Channels: each output is the grey stored channel over the whole window, alpha 1.
void separateTests() {
    const auto source = input();
    const std::array channels{ImageChannelSource::Red, ImageChannelSource::Green,
                              ImageChannelSource::Blue, ImageChannelSource::Alpha};
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        const auto c = channels[channel];
        const auto grey = apply(select({c, c, c, ImageChannelSource::One}), source);
        for (std::size_t i = 0; i < kPixels.size(); ++i) {
            const auto x = -2 + static_cast<std::int64_t>(i % 4);
            const auto y = 3 + static_cast<std::int64_t>(i / 4);
            const float stored = components(at(source, x, y))[channel];
            const auto out = at(*grey, x, y);
            require(out.red() == stored && out.green() == stored && out.blue() == stored &&
                        out.alpha() == 1.0F,
                    "a Separate Channels output is the exact stored channel as grey, alpha 1");
        }
    }
    // The HDR depth and the signed normals arrive exactly as stored: no clamp, no transform.
    const auto red = apply(select({ImageChannelSource::Red, ImageChannelSource::Red,
                                   ImageChannelSource::Red, ImageChannelSource::One}),
                           source);
    require(at(*red, -2, 4).red() == 12000.0F && at(*red, -1, 4).red() == -0.6F &&
                at(*red, 0, 4).red() == 42.0F && at(*red, 0, 5).red() == 65504.0F,
            "Raw depth, signed normals and IDs keep their exact values");
    // A transparent input pixel separates to opaque zero, so its channel reads as data.
    require(components(at(*red, 1, 3)) == std::array{0.0F, 0.0F, 0.0F, 1.0F},
            "a transparent pixel separates to opaque zero");
    const auto lumaGrey = apply(select({ImageChannelSource::Luma, ImageChannelSource::Luma,
                                        ImageChannelSource::Luma, ImageChannelSource::One}),
                                source);
    for (std::size_t i = 0; i < kPixels.size(); ++i) {
        const auto x = -2 + static_cast<std::int64_t>(i % 4);
        const auto y = 3 + static_cast<std::int64_t>(i / 4);
        require(at(*lumaGrey, x, y).red() == static_cast<float>(luma(at(source, x, y))),
                "Luma is the Rec. 709 luma of the stored colour, rounded once");
    }
}

// Combine Channels: Separate into Combine reproduces the image bit for bit; the alpha modes,
// alpha's [0, 1] limit, windows and an absent second image.
void combineTests() {
    const auto source = input();
    const auto grey = [&](const ImageChannelSource c) {
        return apply(select({c, c, c, ImageChannelSource::One}), source);
    };
    const auto r = grey(ImageChannelSource::Red);
    const auto g = grey(ImageChannelSource::Green);
    const auto b = grey(ImageChannelSource::Blue);
    const auto a = grey(ImageChannelSource::Alpha);
    // The compiler's chain: a select of the base (red) then one combine per further input; alpha
    // reads red, the channel a grey image carries its value in.
    const auto base = apply(select({ImageChannelSource::Red, ImageChannelSource::Zero,
                                    ImageChannelSource::Zero, ImageChannelSource::One}),
                            *r);
    const auto withGreen = combine(combineKernel(1, ImageChannelSource::Green), *base, g.get());
    const auto withBlue = combine(combineKernel(2, ImageChannelSource::Blue), *withGreen, b.get());
    const auto rebuilt = combine(combineKernel(3, ImageChannelSource::Red), *withBlue, a.get());
    for (std::size_t i = 0; i < kPixels.size(); ++i) {
        const auto x = -2 + static_cast<std::int64_t>(i % 4);
        const auto y = 3 + static_cast<std::int64_t>(i / 4);
        require(at(*rebuilt, x, y) == at(source, x, y),
                "Separate Channels into Combine Channels reproduces the image exactly");
    }
    // Straight: red, green and blue are multiplied by the combined alpha.
    const auto straight =
        combine(combineKernel(3, ImageChannelSource::Red, true), *withBlue, a.get());
    for (std::size_t i = 0; i < kPixels.size(); ++i) {
        const auto x = -2 + static_cast<std::int64_t>(i % 4);
        const auto y = 3 + static_cast<std::int64_t>(i / 4);
        const auto s = at(source, x, y);
        const auto out = at(*straight, x, y);
        if (s.alpha() == 0.0F) {
            require(out == render::Rgba32f::transparent(), "zero alpha is transparent black");
            continue;
        }
        require(out.alpha() == s.alpha() && out.red() == s.red() * s.alpha() &&
                    out.green() == s.green() * s.alpha() && out.blue() == s.blue() * s.alpha(),
                "Straight premultiplies red, green and blue by the alpha");
    }
    // Alpha from an HDR or signed value is limited to [0, 1]; colour is never clamped.
    const auto hdrAlpha =
        combine(combineKernel(3, ImageChannelSource::Blue),
                *apply(select({ImageChannelSource::Red, ImageChannelSource::Green,
                               ImageChannelSource::Blue, ImageChannelSource::One}),
                       source),
                &source);
    require(at(*hdrAlpha, -1, 3).alpha() == 1.0F && at(*hdrAlpha, -1, 3).blue() == 6.0F &&
                at(*hdrAlpha, -2, 4) == render::Rgba32f::transparent() &&
                at(*hdrAlpha, 0, 3).alpha() == 0.125F,
            "alpha is limited to [0, 1] while colour stays HDR");
    // A smaller second image reads zero outside its data window; an absent one reads zero.
    const std::vector<std::array<float, 4>> small{{9.0F, 8.0F, 7.0F, 1.0F},
                                                  {-5.0F, 4.0F, 3.0F, 1.0F}};
    const auto second = image(small, -1, 4, 2);
    const auto partial = combine(combineKernel(0, ImageChannelSource::Red), source, &second);
    for (std::size_t i = 0; i < kPixels.size(); ++i) {
        const auto x = -2 + static_cast<std::int64_t>(i % 4);
        const auto y = 3 + static_cast<std::int64_t>(i / 4);
        const auto s = at(source, x, y);
        const float expected = y == 4 && x == -1 ? 9.0F : y == 4 && x == 0 ? -5.0F : 0.0F;
        const auto out = at(*partial, x, y);
        require((s.alpha() == 0.0F ? out == render::Rgba32f::transparent()
                                   : out.red() == expected && out.green() == s.green() &&
                                         out.blue() == s.blue() && out.alpha() == s.alpha()),
                "the second image is read at the same global pixel and is zero outside it");
    }
    const auto absent = combine(combineKernel(3, ImageChannelSource::Alpha), source, nullptr);
    for (std::size_t i = 0; i < kPixels.size(); ++i)
        require(at(*absent, -2 + static_cast<std::int64_t>(i % 4),
                   3 + static_cast<std::int64_t>(i / 4)) == render::Rgba32f::transparent(),
                "an absent alpha input is zero alpha");
    // Luma as a source.
    const auto lumaRed = combine(combineKernel(0, ImageChannelSource::Luma), source, &source);
    require(at(*lumaRed, -1, 3).red() == static_cast<float>(luma(at(source, -1, 3))),
            "a Luma source reads the stored colour's luma");
}

void domainTests() {
    require(runtime::detail::prepareChannelSelect(runtime::ChannelSelectKernel{})->identity,
            "the red, green, blue, alpha mapping is an identity");
    require(
        !runtime::detail::prepareChannelSelect(runtime::ChannelSelectKernel{{}, true})->identity,
        "premultiply is never an identity");
    runtime::ChannelSelectKernel bad;
    bad.sources[2] = static_cast<ImageChannelSource>(9);
    require(!runtime::detail::prepareChannelSelect(bad), "an unknown select source refuses");
    require(
        !runtime::detail::prepareChannelCombine({4, ImageChannelSource::Red, false}) &&
            !runtime::detail::prepareChannelCombine({0, ImageChannelSource::Zero, false}) &&
            !runtime::detail::prepareChannelCombine({0, ImageChannelSource::One, false}) &&
            !runtime::detail::prepareChannelCombine({0, static_cast<ImageChannelSource>(9), false}),
        "a combine slot above alpha or a constant source refuses");
    const auto source = input();
    const auto grey = select({ImageChannelSource::Red, ImageChannelSource::Red,
                              ImageChannelSource::Red, ImageChannelSource::One});
    require(!runtime::detail::applyChannelSelect(grey, source, 1, {},
                                                 runtime::OperationIndex::fromRaw(0), {})
                    .image &&
                !runtime::detail::applyChannelCombine(combineKernel(0, ImageChannelSource::Red),
                                                      source, &source, 1, {},
                                                      runtime::OperationIndex::fromRaw(0), {})
                     .image,
            "the budget refuses before publication");
    const auto s1 = runtime::detail::channelSelectIdentity(grey);
    const auto s2 = runtime::detail::channelSelectIdentity(
        select({ImageChannelSource::Green, ImageChannelSource::Green, ImageChannelSource::Green,
                ImageChannelSource::One}));
    const auto c1 =
        runtime::detail::channelCombineIdentity(combineKernel(1, ImageChannelSource::Red));
    const auto c2 =
        runtime::detail::channelCombineIdentity(combineKernel(1, ImageChannelSource::Red, true));
    require(s1 != s2 && c1 != c2 && s1 != c1, "every mapping has its own cache identity");
}

void programTests() {
    const auto grey = select({ImageChannelSource::Alpha, ImageChannelSource::Alpha,
                              ImageChannelSource::Alpha, ImageChannelSource::One});
    const auto data = runtime::detail::channelSelectProgramData(grey);
    require(data == std::array{3.0F, 3.0F, 3.0F, 6.0F, 0.0F, 0.0F, 0.0F, 0.0F},
            "the program data is the prepared mapping");
    const auto program = runtime::detail::buildChannelSelectProgram(data);
    require(program.succeeded() && render::validateOcioGpuProgram(*program.program(), {}) ==
                                       render::OcioGpuProgramError::None,
            "the Channel Select program prepares and validates");
    const auto& desc = *program.program();
    require(desc.semanticsId == "bloom.render.channel-select.v1" &&
                desc.functionName == "bloom_channel_select" &&
                desc.stage == render::OcioGpuProgramStage::ProcessNeighborhood &&
                desc.uniformBufferStorage &&
                desc.uniformBufferSize == data.size() * sizeof(float) &&
                std::memcmp(desc.uniformBufferData.data(), data.data(),
                            data.size() * sizeof(float)) == 0,
            "the program carries the mapping as its storage data");
    const auto other =
        runtime::detail::buildChannelSelectProgram(runtime::detail::channelSelectProgramData(
            select({ImageChannelSource::Luma, ImageChannelSource::Zero, ImageChannelSource::Red,
                    ImageChannelSource::Blue},
                   true)));
    require(other.succeeded() && other.program()->shaderText == desc.shaderText &&
                other.program()->uniformBufferData != desc.uniformBufferData,
            "a changed mapping changes only the program data, never the shader");
    const auto wrapper = runtime::buildGpuOcioWrapperGlsl(desc);
    require(wrapper.succeeded() &&
                wrapper.source.find("vec4 p = bloom_channel_select(c)") != std::string::npos,
            "the select runs in the resident neighborhood wrapper");
    auto bad = data;
    bad[1] = 7.0F;
    auto fraction = data;
    fraction[0] = 0.5F;
    auto flag = data;
    flag[4] = 2.0F;
    require(!runtime::detail::buildChannelSelectProgram(bad).succeeded() &&
                !runtime::detail::buildChannelSelectProgram(fraction).succeeded() &&
                !runtime::detail::buildChannelSelectProgram(flag).succeeded(),
            "out-of-domain program data refuses");
}

void cancellationTest() {
    const auto source = input();
    const auto grey = select({ImageChannelSource::Red, ImageChannelSource::Red,
                              ImageChannelSource::Red, ImageChannelSource::One});
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(86)};
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Channel cancellation", owner), [&](runtime::TaskContext& context) {
            const auto progress = [&](const runtime::EvaluationProgress& value) {
                if (value.completed == 1)
                    static_cast<void>(scheduler.cancelOwner(owner));
            };
            const auto selected =
                runtime::detail::applyChannelSelect(grey, source, 65536, context.cancellation(),
                                                    runtime::OperationIndex::fromRaw(0), progress);
            const auto combined = runtime::detail::applyChannelCombine(
                combineKernel(0, ImageChannelSource::Red), source, &source, 65536,
                context.cancellation(), runtime::OperationIndex::fromRaw(0), progress);
            require(selected.cancelled && !selected.image && combined.cancelled && !combined.image,
                    "row cancellation publishes no image");
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
    require(scheduler.isQuiescent(), "channel cancellation shuts down safely");
}
} // namespace

int main() {
    try {
        separateTests();
        combineTests();
        domainTests();
        programTests();
        cancellationTest();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: Separate/Combine Channels reference, exact stored values, Raw passes, "
                 "alpha modes, windows, budget, resident program and cancellation\n";
}
