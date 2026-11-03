#include "image_effect.hpp"
#include <bloom/runtime/gpu_ocio_wrapper.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace {
using namespace bloom;
void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
runtime::SeparableBlurKernel kernel(const runtime::SeparableBlurKind kind,
                                    const runtime::BlurEdgeMode edge) {
    return {kind, {document::ParameterId::fromRaw(1), 1.0}, edge};
}
render::Rgba32fImage source() {
    const auto data = render::ImageWindow::create(-3, 5, 3, 2);
    const auto display = render::ImageWindow::create(0, 0, 8, 8);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    const auto p = render::Rgba32f::fromPremultiplied(-0.25F, 2.0F, 0.5F, 0.5F);
    require(!builder.value()->write(-2, 5, *p.value()), "impulse writes");
    auto image = std::move(*builder.value()).freeze();
    return std::move(*image.value());
}
void referenceTests() {
    const auto input = source();
    for (const auto kind :
         {runtime::SeparableBlurKind::Gaussian, runtime::SeparableBlurKind::Box}) {
        for (const auto edge : {runtime::BlurEdgeMode::Transparent, runtime::BlurEdgeMode::Clamp}) {
            const auto blur =
                runtime::detail::prepareSeparableBlur(kernel(kind, edge), 1.0, 1.0, 1.0);
            require(blur.has_value(), "blur prepares");
            const auto result = runtime::detail::applySeparableBlur(
                *blur, input, 65536, {}, runtime::OperationIndex::fromRaw(0), {});
            require(result.image && !result.diagnostic, "filter publishes finite RGBA");
            const auto output = result.image->descriptor()->dataWindow();
            require(result.image->descriptor()->displayWindow() ==
                        input.descriptor()->displayWindow(),
                    "filter preserves display window independently of data window");
            require(output.originX() == (edge == runtime::BlurEdgeMode::Transparent ? -4 : -3) &&
                        output.originY() == (edge == runtime::BlurEdgeMode::Transparent ? 4 : 5),
                    "transparent halo expands both axes; clamp retains extent");
            const auto original = input.descriptor()->dataWindow();
            // Independent direct 2-D convolution; the implementation is two one-dimensional passes.
            for (auto y = output.originY(); y < output.maxYExclusive(); ++y) {
                for (auto x = output.originX(); x < output.maxXExclusive(); ++x) {
                    std::array<double, 4> expected{};
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            auto sx = x + dx, sy = y + dy;
                            if (edge == runtime::BlurEdgeMode::Clamp) {
                                sx = std::clamp(sx, original.originX(),
                                                original.maxXExclusive() - 1);
                                sy = std::clamp(sy, original.originY(),
                                                original.maxYExclusive() - 1);
                            }
                            const auto sample = input.read(sx, sy);
                            if (!sample)
                                continue;
                            const double weight =
                                static_cast<double>(
                                    blur->horizontal[static_cast<std::size_t>(dx + 1)]) *
                                static_cast<double>(
                                    blur->vertical[static_cast<std::size_t>(dy + 1)]);
                            const auto components = sample.value()->components();
                            for (std::size_t c = 0; c < 4; ++c)
                                expected[c] += static_cast<double>(components[c]) * weight;
                        }
                    const auto actual = result.image->read(x, y);
                    require(actual.hasValue(), "expanded-window sample exists");
                    for (std::size_t c = 0; c < 4; ++c)
                        require(std::abs(static_cast<double>(actual.value()->components()[c]) -
                                         expected[c]) < 2e-6,
                                "separable output matches independent 2-D convolution on signed "
                                "HDR and coverage");
                }
            }
            require(!runtime::detail::applySeparableBlur(*blur, input, 1, {},
                                                         runtime::OperationIndex::fromRaw(0), {})
                         .image,
                    "intermediate budget refuses before publication");
            for (const bool vertical : {false, true}) {
                auto program = runtime::detail::buildSeparableBlurProgram(
                    vertical ? blur->vertical : blur->horizontal, vertical, edge);
                require(program.succeeded(),
                        "normalized host coefficients prepare a device program");
                require(render::validateOcioGpuProgram(*program.program(), {}) ==
                            render::OcioGpuProgramError::None,
                        "program descriptor validates");
                const auto wrapper = runtime::buildGpuOcioWrapperGlsl(*program.program());
                require(
                    wrapper.succeeded() &&
                        wrapper.source.find("imageLoad(bloom_ocio_input, p)") !=
                            std::string::npos &&
                        wrapper.source.find("vec4 p = bloom_separable_blur(c)") !=
                            std::string::npos,
                    "GPU wrapper performs resident neighborhood reads and associated-RGBA writes");
                const auto validation = wrapper.source.find("if (any(isnan(p))");
                const auto canonicalization =
                    wrapper.source.find("if (p.a == 0.0) { p = vec4(0.0); }");
                const auto store = wrapper.source.find("imageStore(bloom_ocio_output, c, p)");
                require(validation < canonicalization && canonicalization < store,
                        "GPU neighborhood passes validate then canonicalize zero coverage "
                        "before storing, matching the CPU intermediate");
            }
        }
    }
    const auto k = kernel(runtime::SeparableBlurKind::Gaussian, runtime::BlurEdgeMode::Transparent);
    require(runtime::detail::prepareSeparableBlur(k, 0.0, 1.0, 1.0)->identity(),
            "radius zero is identity");
    require(!runtime::detail::prepareSeparableBlur(k, -1.0, 1.0, 1.0) &&
                !runtime::detail::prepareSeparableBlur(k, 257.0, 1.0, 1.0) &&
                !runtime::detail::prepareSeparableBlur(k, std::numeric_limits<double>::quiet_NaN(),
                                                       1.0, 1.0),
            "nonfinite and out-of-domain radius refuses");
    // #460: Box covers [-radius, radius] continuously; the outermost taps of a fractional radius
    // weigh its fraction, so radius 0.01 is nearly the identity and the kernel never jumps at an
    // integer radius.
    const auto box = kernel(runtime::SeparableBlurKind::Box, runtime::BlurEdgeMode::Transparent);
    const auto boxAt = [&](const double radius) {
        const auto prepared = runtime::detail::prepareSeparableBlur(box, radius, 1.0, 1.0);
        require(prepared.has_value(), "box blur prepares");
        return prepared->horizontal;
    };
    const auto near = [](const float actual, const double expected) {
        return std::abs(static_cast<double>(actual) - expected) < 1e-7;
    };
    const auto quarter = boxAt(2.25);
    require(quarter.size() == 7 && near(quarter[0], 0.25 / 5.5) && near(quarter[6], 0.25 / 5.5) &&
                std::all_of(quarter.begin() + 1, quarter.end() - 1,
                            [&](const float w) { return near(w, 1.0 / 5.5); }),
            "fractional box radius weights its outermost taps by the fraction");
    const auto tiny = boxAt(0.01);
    require(tiny.size() == 3 && near(tiny[1], 1.0 / 1.02) && near(tiny[0], 0.01 / 1.02),
            "box radius 0.01 is nearly the identity, not a full three-tap box");
    const auto whole = boxAt(2.0);
    const auto below = boxAt(1.9999);
    const auto above = boxAt(2.0001);
    require(
        whole.size() == 5 && below.size() == 5 && above.size() == 7 &&
            std::all_of(whole.begin(), whole.end(), [&](const float w) { return near(w, 0.2); }),
        "whole box radius keeps uniform weights");
    for (std::size_t i = 0; i < 5; ++i)
        require(std::abs(below[i] - whole[i]) < 1e-4F && std::abs(above[i + 1] - whole[i]) < 1e-4F,
                "box weights are continuous across an integer radius");
    require(above[0] < 1e-4F && above[6] < 1e-4F, "a just-grown box tap starts near zero");
    const auto proxy = runtime::detail::prepareSeparableBlur(k, 3.0, 0.5, 2.0);
    require(proxy->horizontal.size() == 5 && proxy->vertical.size() == 13,
            "proxy radii and halos scale independently per axis");
}
void subnormalCoverageTest() {
    const auto window = render::ImageWindow::create(0, 0, 1, 1);
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    const auto pixel = render::Rgba32f::fromPremultiplied(
        1.0F, 0.0F, 0.0F, std::numeric_limits<float>::denorm_min());
    require(pixel.hasValue(), "subnormal coverage is valid input");
    require(!builder.value()->write(0, 0, *pixel.value()), "subnormal input writes");
    auto image = std::move(*builder.value()).freeze();
    for (const auto kind :
         {runtime::SeparableBlurKind::Gaussian, runtime::SeparableBlurKind::Box}) {
        const auto blur = runtime::detail::prepareSeparableBlur(
            kernel(kind, runtime::BlurEdgeMode::Transparent), 1.0, 1.0, 1.0);
        const auto result = runtime::detail::applySeparableBlur(
            *blur, *image.value(), 65536, {}, runtime::OperationIndex::fromRaw(0), {});
        require(result.image != nullptr, "subnormal blur evaluates");
        const auto side = result.image->read(-1, 0);
        require(side && side.value()->alpha() == 0.0F && side.value()->red() == 0.0F,
                "underflowed coverage canonicalizes RGB after each blur pass");
    }
}
// #337 Matte Blur: Coverage filters alpha alone (an independent 2-D convolution of coverage) and
// writes it to every channel, so even a non-white input leaves premultiplied white. RGB never
// contributes, the windows follow the Rgba blur, and the device program is a distinct identity.
void coverageTests() {
    const auto data = render::ImageWindow::create(-2, -1, 4, 3);
    const auto display = render::ImageWindow::create(0, 0, 6, 6);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    const std::array<float, 12> coverage{0.0F,  1.0F, 0.25F,  0.5F, 1.0F, 0.0F,
                                         0.75F, 1.0F, 0.125F, 1.0F, 0.5F, 0.0F};
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        const auto a = coverage[i];
        // Signed/HDR RGB that a Mask never carries: Coverage must ignore it entirely.
        const auto p = render::Rgba32f::fromPremultiplied(a == 0.0F ? 0.0F : -3.0F * a,
                                                          a == 0.0F ? 0.0F : 7.0F * a, a, a);
        require(p.hasValue() &&
                    !builder.value()->write(-2 + static_cast<std::int64_t>(i % 4),
                                            -1 + static_cast<std::int64_t>(i / 4), *p.value()),
                "coverage input writes");
    }
    auto frozen = std::move(*builder.value()).freeze();
    require(frozen.hasValue(), "coverage input freezes");
    const auto input = std::move(*frozen.value());
    for (const auto edge : {runtime::BlurEdgeMode::Transparent, runtime::BlurEdgeMode::Clamp}) {
        auto matte = kernel(runtime::SeparableBlurKind::Gaussian, edge);
        matte.channels = runtime::BlurChannels::Coverage;
        const auto blur = runtime::detail::prepareSeparableBlur(matte, 1.5, 1.0, 1.0);
        require(blur && blur->channels == runtime::BlurChannels::Coverage,
                "coverage blur prepares");
        const auto rgba = runtime::detail::prepareSeparableBlur(
            kernel(runtime::SeparableBlurKind::Gaussian, edge), 1.5, 1.0, 1.0);
        require(rgba && rgba->horizontal == blur->horizontal && rgba->vertical == blur->vertical,
                "coverage shares the Gaussian coefficients exactly");
        const auto result = runtime::detail::applySeparableBlur(
            *blur, input, 65536, {}, runtime::OperationIndex::fromRaw(0), {});
        require(result.image && !result.diagnostic, "coverage filter publishes");
        const auto output = result.image->descriptor()->dataWindow();
        const auto rgbaResult = runtime::detail::applySeparableBlur(
            *rgba, input, 65536, {}, runtime::OperationIndex::fromRaw(0), {});
        require(rgbaResult.image && rgbaResult.image->descriptor()->dataWindow() == output &&
                    result.image->descriptor()->displayWindow() ==
                        input.descriptor()->displayWindow(),
                "coverage windows equal the Rgba blur's; display window is preserved");
        const auto support = static_cast<std::int64_t>(blur->horizontal.size() / 2);
        const auto original = input.descriptor()->dataWindow();
        for (auto y = output.originY(); y < output.maxYExclusive(); ++y)
            for (auto x = output.originX(); x < output.maxXExclusive(); ++x) {
                double expected = 0.0;
                for (auto dy = -support; dy <= support; ++dy)
                    for (auto dx = -support; dx <= support; ++dx) {
                        auto sx = x + dx, sy = y + dy;
                        if (edge == runtime::BlurEdgeMode::Clamp) {
                            sx = std::clamp(sx, original.originX(), original.maxXExclusive() - 1);
                            sy = std::clamp(sy, original.originY(), original.maxYExclusive() - 1);
                        }
                        const auto sample = input.read(sx, sy);
                        if (!sample)
                            continue;
                        expected += static_cast<double>(sample.value()->alpha()) *
                                    static_cast<double>(
                                        blur->horizontal[static_cast<std::size_t>(dx + support)]) *
                                    static_cast<double>(
                                        blur->vertical[static_cast<std::size_t>(dy + support)]);
                    }
                const auto actual = result.image->read(x, y);
                require(actual.hasValue(), "coverage sample exists");
                const auto p = *actual.value();
                require(std::abs(static_cast<double>(p.alpha()) - expected) < 2e-6 &&
                            p.alpha() >= 0.0F && p.alpha() <= 1.0F,
                        "coverage equals an independent 2-D convolution of alpha");
                require(p.red() == p.alpha() && p.green() == p.alpha() && p.blue() == p.alpha(),
                        "coverage writes premultiplied white: every channel holds the coverage");
            }
        require(!runtime::detail::applySeparableBlur(*blur, input, 1, {},
                                                     runtime::OperationIndex::fromRaw(0), {})
                     .image,
                "coverage intermediate budget refuses before publication");
        for (const bool vertical : {false, true}) {
            const auto& weights = vertical ? blur->vertical : blur->horizontal;
            auto program = runtime::detail::buildSeparableBlurProgram(
                weights, vertical, edge, runtime::BlurChannels::Coverage);
            auto rgbaProgram = runtime::detail::buildSeparableBlurProgram(weights, vertical, edge);
            require(program.succeeded() && rgbaProgram.succeeded() &&
                        render::validateOcioGpuProgram(*program.program(), {}) ==
                            render::OcioGpuProgramError::None,
                    "coverage device program prepares and validates");
            require(
                program.program()->semanticsId == "bloom.render.separable-blur.coverage.v1" &&
                    program.program()->contentIdentity != rgbaProgram.program()->contentIdentity &&
                    program.program()->shaderText.find("imageLoad(bloom_ocio_input, p).a") !=
                        std::string::npos &&
                    program.program()->shaderText.find("return vec4(clamp(sum.a, 0.0, 1.0));") !=
                        std::string::npos,
                "coverage program reads alpha only, writes it to every channel and has its "
                "own content identity");
            const auto wrapper = runtime::buildGpuOcioWrapperGlsl(*program.program());
            require(wrapper.succeeded() &&
                        wrapper.source.find("vec4 p = bloom_separable_blur(c)") !=
                            std::string::npos,
                    "coverage program runs in the resident neighborhood wrapper");
        }
    }
    auto identity = kernel(runtime::SeparableBlurKind::Gaussian, runtime::BlurEdgeMode::Clamp);
    identity.channels = runtime::BlurChannels::Coverage;
    require(runtime::detail::prepareSeparableBlur(identity, 0.0, 1.0, 1.0)->identity(),
            "coverage radius zero is an exact identity");
    auto invalid = identity;
    invalid.channels = static_cast<runtime::BlurChannels>(2);
    require(!runtime::detail::prepareSeparableBlur(invalid, 1.0, 1.0, 1.0) &&
                !runtime::detail::buildSeparableBlurProgram(std::array{1.0F}, false,
                                                            runtime::BlurEdgeMode::Clamp,
                                                            static_cast<runtime::BlurChannels>(2))
                     .succeeded(),
            "an unknown channel policy refuses on both routes");
}
// #338 Matte Erode/Dilate: the separable square minimum/maximum of coverage, compared with an
// independent two-dimensional reduction. Whole taps weigh 1 and a fractional radius's outer taps
// weigh the fraction (Dilate: max of w*a; Erode: min of 1 - w*(1 - a), a = 0 outside under
// Transparent). RGB never contributes, every channel holds the coverage, Erode never grows the
// window, and the device programs are distinct identities that reduce instead of summing.
void morphologyTests() {
    const auto data = render::ImageWindow::create(-2, -1, 5, 4);
    const auto display = render::ImageWindow::create(0, 0, 6, 6);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    const std::array<float, 20> coverage{1.0F,  1.0F, 0.25F, 1.0F,  1.0F, 0.5F,  1.0F,
                                         1.0F,  0.0F, 1.0F,  1.0F,  1.0F, 0.75F, 1.0F,
                                         0.125F, 1.0F, 1.0F, 0.0F,  1.0F, 0.625F};
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        const auto a = coverage[i];
        const auto p = render::Rgba32f::fromPremultiplied(a == 0.0F ? 0.0F : -3.0F * a,
                                                          a == 0.0F ? 0.0F : 7.0F * a, a, a);
        require(p.hasValue() &&
                    !builder.value()->write(-2 + static_cast<std::int64_t>(i % 5),
                                            -1 + static_cast<std::int64_t>(i / 5), *p.value()),
                "morphology input writes");
    }
    auto frozen = std::move(*builder.value()).freeze();
    require(frozen.hasValue(), "morphology input freezes");
    const auto input = std::move(*frozen.value());
    const auto original = input.descriptor()->dataWindow();
    for (const auto kind : {runtime::SeparableBlurKind::Erode, runtime::SeparableBlurKind::Dilate}) {
        const bool erode = kind == runtime::SeparableBlurKind::Erode;
        for (const auto edge : {runtime::BlurEdgeMode::Transparent, runtime::BlurEdgeMode::Clamp}) {
            for (const double radius : {1.0, 1.5, 0.25}) {
                auto matte = kernel(kind, edge);
                matte.channels = runtime::BlurChannels::Coverage;
                const auto filter = runtime::detail::prepareSeparableBlur(matte, radius, 1.0, 1.0);
                require(filter && filter->kind == kind && !filter->identity(),
                        "morphology prepares");
                const auto support = static_cast<std::int64_t>(std::ceil(radius));
                const float fraction = static_cast<float>(radius - std::floor(radius));
                require(filter->horizontal.size() == static_cast<std::size_t>(2 * support + 1) &&
                            filter->horizontal == filter->vertical,
                        "morphology support is ceil(radius) per side");
                for (std::int64_t tap = -support; tap <= support; ++tap)
                    require(filter->horizontal[static_cast<std::size_t>(tap + support)] ==
                                (std::abs(tap) == support && fraction > 0.0F ? fraction : 1.0F),
                            "whole taps weigh 1 and fractional outer taps weigh the fraction");
                const auto result = runtime::detail::applySeparableBlur(
                    *filter, input, 65536, {}, runtime::OperationIndex::fromRaw(0), {});
                require(result.image && !result.diagnostic, "morphology publishes");
                const auto output = result.image->descriptor()->dataWindow();
                const bool grows = !erode && edge == runtime::BlurEdgeMode::Transparent;
                require(output.originX() == original.originX() - (grows ? support : 0) &&
                            output.originY() == original.originY() - (grows ? support : 0) &&
                            output.extent().width() ==
                                original.extent().width() +
                                    static_cast<std::uint64_t>(grows ? 2 * support : 0) &&
                            result.image->descriptor()->displayWindow() ==
                                input.descriptor()->displayWindow(),
                        "only a transparent Dilate grows the window; display window is kept");
                for (auto y = output.originY(); y < output.maxYExclusive(); ++y)
                    for (auto x = output.originX(); x < output.maxXExclusive(); ++x) {
                        double expected = erode ? 1.0 : 0.0;
                        for (auto dy = -support; dy <= support; ++dy)
                            for (auto dx = -support; dx <= support; ++dx) {
                                auto sx = x + dx, sy = y + dy;
                                if (edge == runtime::BlurEdgeMode::Clamp) {
                                    sx = std::clamp(sx, original.originX(),
                                                    original.maxXExclusive() - 1);
                                    sy = std::clamp(sy, original.originY(),
                                                    original.maxYExclusive() - 1);
                                }
                                const auto sample = input.read(sx, sy);
                                const double a =
                                    sample ? static_cast<double>(sample.value()->alpha()) : 0.0;
                                const double w =
                                    static_cast<double>(
                                        filter->horizontal[static_cast<std::size_t>(dx + support)]) *
                                    static_cast<double>(
                                        filter->vertical[static_cast<std::size_t>(dy + support)]);
                                expected = erode ? std::min(expected, 1.0 - w * (1.0 - a))
                                                 : std::max(expected, w * a);
                            }
                        const auto actual = result.image->read(x, y);
                        require(actual.hasValue(), "morphology sample exists");
                        const auto p = *actual.value();
                        require(radius == 1.0
                                    ? static_cast<double>(p.alpha()) == expected
                                    : std::abs(static_cast<double>(p.alpha()) - expected) < 2e-6,
                                "morphology equals an independent 2-D square min/max of coverage "
                                "(exactly for a whole radius)");
                        require(p.red() == p.alpha() && p.green() == p.alpha() &&
                                    p.blue() == p.alpha(),
                                "morphology writes premultiplied white");
                    }
                require(!runtime::detail::applySeparableBlur(*filter, input, 1, {},
                                                             runtime::OperationIndex::fromRaw(0),
                                                             {})
                             .image,
                        "morphology budget refuses before publication");
                for (const bool vertical : {false, true}) {
                    const auto& weights = vertical ? filter->vertical : filter->horizontal;
                    auto program = runtime::detail::buildSeparableBlurProgram(
                        weights, vertical, edge, runtime::BlurChannels::Coverage, kind);
                    auto blurProgram = runtime::detail::buildSeparableBlurProgram(
                        weights, vertical, edge, runtime::BlurChannels::Coverage);
                    require(program.succeeded() && blurProgram.succeeded() &&
                                render::validateOcioGpuProgram(*program.program(), {}) ==
                                    render::OcioGpuProgramError::None,
                            "morphology device program prepares and validates");
                    const auto& text = program.program()->shaderText;
                    require(program.program()->semanticsId ==
                                    (erode ? "bloom.render.separable-erode.coverage.v1"
                                           : "bloom.render.separable-dilate.coverage.v1") &&
                                program.program()->contentIdentity !=
                                    blurProgram.program()->contentIdentity &&
                                text.find(erode ? "m = min(m, t);" : "m = max(m, t);") !=
                                    std::string::npos &&
                                text.find(erode ? "fma(w, a, 1.0 - w)" : "w * imageLoad") !=
                                    std::string::npos &&
                                text.find("return vec4(clamp(m, 0.0, 1.0));") != std::string::npos,
                            "morphology program reduces alpha by min/max with its own identity");
                    const auto wrapper = runtime::buildGpuOcioWrapperGlsl(*program.program());
                    require(wrapper.succeeded() && wrapper.source.find(
                                                       "vec4 p = bloom_separable_blur(c)") !=
                                                       std::string::npos,
                            "morphology program runs in the resident neighborhood wrapper");
                }
            }
        }
        auto identity = kernel(kind, runtime::BlurEdgeMode::Clamp);
        identity.channels = runtime::BlurChannels::Coverage;
        require(runtime::detail::prepareSeparableBlur(identity, 0.0, 1.0, 1.0)->identity(),
                "morphology radius zero is an exact identity");
        const auto rgba = kernel(kind, runtime::BlurEdgeMode::Clamp);
        require(!runtime::detail::prepareSeparableBlur(rgba, 1.0, 1.0, 1.0) &&
                    !runtime::detail::buildSeparableBlurProgram(std::array{1.0F, 1.0F, 1.0F},
                                                                false, runtime::BlurEdgeMode::Clamp,
                                                                runtime::BlurChannels::Rgba, kind)
                         .succeeded(),
                "morphology refuses the Rgba channel policy on both routes");
    }
    auto invalid = kernel(static_cast<runtime::SeparableBlurKind>(4), runtime::BlurEdgeMode::Clamp);
    invalid.channels = runtime::BlurChannels::Coverage;
    require(!runtime::detail::prepareSeparableBlur(invalid, 1.0, 1.0, 1.0),
            "an unknown neighborhood kind refuses");
    // Proxy scaling follows the blur: support is ceil(radius * scale) per axis.
    auto proxy = kernel(runtime::SeparableBlurKind::Dilate, runtime::BlurEdgeMode::Transparent);
    proxy.channels = runtime::BlurChannels::Coverage;
    const auto scaled = runtime::detail::prepareSeparableBlur(proxy, 3.0, 0.5, 0.25);
    require(scaled && scaled->horizontal.size() == 5 && scaled->vertical.size() == 3 &&
                scaled->horizontal.front() == 0.5F && scaled->vertical.front() == 0.75F,
            "morphology support and fractions scale with the proxy resolution");
}
void cancellationTest(const runtime::BlurChannels channels,
                      const runtime::SeparableBlurKind kind = runtime::SeparableBlurKind::Box) {
    const auto input = source();
    auto cancelled = kernel(kind, runtime::BlurEdgeMode::Transparent);
    cancelled.channels = channels;
    const auto blur = runtime::detail::prepareSeparableBlur(cancelled, 2.0, 1.0, 1.0);
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(83)};
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Blur cancellation", owner), [&](runtime::TaskContext& context) {
            const auto result = runtime::detail::applySeparableBlur(
                *blur, input, 65536, context.cancellation(), runtime::OperationIndex::fromRaw(0),
                [&](const runtime::EvaluationProgress& progress) {
                    if (progress.total && progress.completed == *progress.total / 2)
                        static_cast<void>(scheduler.cancelOwner(owner));
                });
            require(result.cancelled && !result.image,
                    "cancellation between passes publishes no image");
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
            "pass-boundary cancellation observed");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(scheduler.isQuiescent(), "blur cancellation shuts down safely");
}
} // namespace
int main() {
    try {
        referenceTests();
        subnormalCoverageTest();
        coverageTests();
        cancellationTest(runtime::BlurChannels::Rgba);
        cancellationTest(runtime::BlurChannels::Coverage);
        morphologyTests();
        cancellationTest(runtime::BlurChannels::Coverage, runtime::SeparableBlurKind::Erode);
        cancellationTest(runtime::BlurChannels::Coverage, runtime::SeparableBlurKind::Dilate);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: Gaussian/Box/coverage reference, HDR/alpha/windows, proxy halo, budget, "
                 "resident shader and cancellation\n";
}
