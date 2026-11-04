#include "gpu_blend_native_support.hpp"

#include <bloom/render/image_coverage.hpp>

#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render::blend_proof {
namespace {
using namespace bloom::render::composite_proof;

[[nodiscard]] Rgba32f oracle(const Rgba32f base, const Rgba32f second,
                            const GpuImageCombineParameters mode) {
    if (mode.kind == GpuImageCombineKind::SetChannel) {
        // #479 Combine Channels: the production CPU reference's own channel read and write.
        const auto result = setImageChannel(base, storedImageChannel(second, mode.channel),
                                            mode.slot, mode.premultiply);
        if (!result)
            throw std::logic_error("invalid Set Channel oracle");
        return *result.value();
    }
    if (mode.kind == GpuImageCombineKind::SetAlpha) {
        const auto result =
            setImageAlpha(base, matteCoverage(second, {mode.channel, mode.invert}));
        if (!result)
            throw std::logic_error("invalid Set Alpha oracle");
        return *result.value();
    }
    // Match the production CPU evaluator: double products and accumulation, then
    // one Float32 conversion. Never use the shader expression as its own oracle. Multiply Alpha's
    // amount is the shared Float32 coverage x factor (#532).
    const double weight =
        mode.kind == GpuImageCombineKind::Mix
            ? static_cast<double>(mode.factor)
            : static_cast<double>(multiplyAlphaAmount(
                  matteCoverage(second, {mode.channel, mode.invert}), mode.factor));
    const double baseWeight = mode.kind == GpuImageCombineKind::Mix ? 1.0 - weight : weight;
    const double secondWeight = mode.kind == GpuImageCombineKind::Mix ? weight : 0.0;
    return pixel(static_cast<float>(static_cast<double>(base.red()) * baseWeight + static_cast<double>(second.red()) * secondWeight),
                 static_cast<float>(static_cast<double>(base.green()) * baseWeight + static_cast<double>(second.green()) * secondWeight),
                 static_cast<float>(static_cast<double>(base.blue()) * baseWeight + static_cast<double>(second.blue()) * secondWeight),
                 static_cast<float>(static_cast<double>(base.alpha()) * baseWeight + static_cast<double>(second.alpha()) * secondWeight));
}
[[nodiscard]] GpuImageCombineParameters setChannel(const ImageCoverageChannel channel,
                                                   const std::uint8_t slot, const bool premultiply,
                                                   const bool absent) {
    GpuImageCombineParameters mode{GpuImageCombineKind::SetChannel, 1.0F, channel, false, absent};
    mode.slot = slot;
    mode.premultiply = premultiply;
    return mode;
}
} // namespace

void runImageCombineTests(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!blend || !uploader) {
        expectations.expect(false, "image combine hosts created");
        return;
    }
    // The base image owns the output extent. The smaller second window starts one pixel to the
    // right and one pixel below; every tap outside it is transparent black.
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 3;
    constexpr std::uint32_t secondWidth = 2;
    constexpr std::uint32_t secondHeight = 2;
    const auto baseWindow = window(-2, 1, width, height);
    const auto secondWindow = window(-1, 2, secondWidth, secondHeight);
    std::vector<Rgba32f> basePixels(width * height, Rgba32f::transparent());
    for (std::size_t index = 0; index < basePixels.size(); ++index) {
        const float alpha = index % 5 == 0 ? 0.0F : (index % 2 == 0 ? 0.5F : 1.0F);
        basePixels[index] = pixel(-0.75F * alpha, 2.5F * alpha, 0.125F * alpha, alpha);
    }
    basePixels[6] = pixel(10000.0F, -10000.0F, 10000.0F, 1.0F);
    basePixels[7] = pixel(1.0F, -2.0F, 3.0F, std::numeric_limits<float>::min());
    basePixels[12] = pixel(-0.75F, 2.5F, 0.125F, 1.0F);
    const std::vector<Rgba32f> secondPixels{
        pixel(-10000.0F, 10000.0F, -10000.0F, 1.0F), pixel(7152.0F, -2126.0F, 0.5F, 1.0F),
        pixel(1.0F, -0.29725947976112366F, 0.0F, 1.0e-7F),
        // #460: a feathered matte edge (straight 1, 0.75, 2 at half alpha). Luma reads the
        // premultiplied colour, so its coverage is about 0.4467, not the straight 0.8934.
        pixel(0.5F, 0.375F, 1.0F, 0.5F)};
    auto baseImage = makeImage(baseWindow, baseWindow, PixelAspectRatio::square(), basePixels);
    auto secondImage =
        makeImage(secondWindow, secondWindow, PixelAspectRatio::square(), secondPixels);
    if (!baseImage || !secondImage) {
        expectations.expect(false, "image combine fixtures build");
        return;
    }
    auto baseResident = upload(*uploader.upload,
                               std::make_shared<const Rgba32fImage>(std::move(*baseImage)));
    auto secondResident = upload(*uploader.upload,
                                 std::make_shared<const Rgba32fImage>(std::move(*secondImage)));
    if (!baseResident || !secondResident) {
        expectations.expect(false, "image combine fixtures upload");
        return;
    }
    auto base = std::make_shared<const GpuImage>(std::move(*baseResident));
    auto second = std::make_shared<const GpuImage>(std::move(*secondResident));
    const std::array modes{
        GpuImageCombineParameters{GpuImageCombineKind::Mix, 0.375F, ImageCoverageChannel::Alpha},
        GpuImageCombineParameters{GpuImageCombineKind::Mix, std::nextafter(0.5F, 1.0F),
                                  ImageCoverageChannel::Alpha},
        GpuImageCombineParameters{GpuImageCombineKind::Mix, std::numeric_limits<float>::min(),
                                  ImageCoverageChannel::Alpha},
        GpuImageCombineParameters{GpuImageCombineKind::SetAlpha, 1.0F, ImageCoverageChannel::Luma},
        GpuImageCombineParameters{GpuImageCombineKind::MultiplyAlpha,
                                  std::numeric_limits<float>::min(), ImageCoverageChannel::Red},
        GpuImageCombineParameters{GpuImageCombineKind::MultiplyAlpha, 0.5F,
                                  ImageCoverageChannel::Red},
        // Inverted mattes: the exact Float32 complement of each channel's unit coverage.
        GpuImageCombineParameters{GpuImageCombineKind::SetAlpha, 1.0F, ImageCoverageChannel::Luma,
                                  true},
        GpuImageCombineParameters{GpuImageCombineKind::SetAlpha, 1.0F, ImageCoverageChannel::Alpha,
                                  true},
        // Unit factor: base pixel 7's FLT_MIN alpha stays normal under full inverted coverage.
        GpuImageCombineParameters{GpuImageCombineKind::MultiplyAlpha, 1.0F,
                                  ImageCoverageChannel::Green, true},
        // Absent mattes: every tap is transparent black, plain and inverted.
        GpuImageCombineParameters{GpuImageCombineKind::SetAlpha, 1.0F, ImageCoverageChannel::Alpha,
                                  false, true},
        GpuImageCombineParameters{GpuImageCombineKind::SetAlpha, 1.0F, ImageCoverageChannel::Red,
                                  true, true},
        GpuImageCombineParameters{GpuImageCombineKind::MultiplyAlpha, 0.375F,
                                  ImageCoverageChannel::Alpha, false, true},
        GpuImageCombineParameters{GpuImageCombineKind::MultiplyAlpha, 1.0F,
                                  ImageCoverageChannel::Alpha, true, true},
        GpuImageCombineParameters{GpuImageCombineKind::Mix, 1.0F, ImageCoverageChannel::Alpha,
                                  false, true},
        // #479 Set Channel: one stored channel of the second image (HDR, signed, partial alpha)
        // written into one base component, alpha clamped to [0, 1], optionally premultiplied.
        setChannel(ImageCoverageChannel::Red, 0, false, false),
        setChannel(ImageCoverageChannel::Luma, 1, false, false),
        setChannel(ImageCoverageChannel::Blue, 2, false, true),
        setChannel(ImageCoverageChannel::Red, 3, false, false),
        setChannel(ImageCoverageChannel::Alpha, 3, true, false)};
    // #479: Set Channel writes one of four components and reads its channel uninverted; a slot
    // or a selector outside that is refused before dispatch, and no other kind takes a slot.
    {
        auto slotFour = setChannel(ImageCoverageChannel::Red, 4, false, false);
        auto inverted = setChannel(ImageCoverageChannel::Red, 0, false, false);
        inverted.invert = true;
        GpuImageCombineParameters mixWithSlot{GpuImageCombineKind::Mix, 0.5F,
                                              ImageCoverageChannel::Alpha};
        mixWithSlot.slot = 1;
        GpuImageCombineParameters mixPremultiplied{GpuImageCombineKind::Mix, 0.5F,
                                                   ImageCoverageChannel::Alpha};
        mixPremultiplied.premultiply = true;
        for (const auto& [invalid, what] :
             {std::pair{slotFour, "a Set Channel slot past alpha"},
              std::pair{inverted, "an inverted Set Channel selector"},
              std::pair{mixWithSlot, "a slot on another combine kind"},
              std::pair{mixPremultiplied, "premultiply on another combine kind"}}) {
            const auto refused = blend.blend->beginBlend(
                GpuBlendParameters{second, base, core::BlendMode::Normal, invalid}, kBudget);
            expectations.expect(refused.code == GpuBlendDiagnosticCode::InvalidArgument &&
                                    blend.blend->state() == GpuBlendJobState::Idle,
                                std::string(what) + " is refused before dispatch");
        }
    }
    for (const auto mode : modes) {
        const auto label =
            "image-combine-" + std::to_string(static_cast<int>(mode.kind)) + "-" +
            std::to_string(static_cast<int>(mode.channel)) + (mode.invert ? "-inverted" : "") +
            (mode.matteAbsent ? "-absent" : "") +
            (mode.kind == GpuImageCombineKind::SetChannel
                 ? "-slot" + std::to_string(mode.slot) + (mode.premultiply ? "-premultiply" : "")
                 : std::string{});
        // An absent matte binds the base image in the matte slot; no matte texel is read.
        const auto begin = blend.blend->beginBlend(
            GpuBlendParameters{mode.matteAbsent ? base : second, base, core::BlendMode::Normal,
                               mode},
            kBudget);
        expectations.expect(begin.code == GpuBlendDiagnosticCode::None,
                            label + ": begin accepted: " + begin.message);
        if (begin.code != GpuBlendDiagnosticCode::None)
            continue;
        GpuBlendPollResult poll = GpuBlendPollResult::Pending;
        while (poll == GpuBlendPollResult::Pending)
            poll = blend.blend->poll();
        // FLT_MIN alpha times 0.5 is nonzero subnormal on the CPU. Refuse it
        // explicitly instead of silently flushing alpha and discarding visible RGB.
        if (mode.kind == GpuImageCombineKind::MultiplyAlpha && mode.factor == 0.5F) {
            expectations.expect(poll == GpuBlendPollResult::Failure &&
                                    blend.blend->diagnostic().code ==
                                        GpuBlendDiagnosticCode::StatusFlagRejected,
                                label + ": nonzero subnormal alpha explicitly refused");
            continue;
        }
        expectations.expect(poll == GpuBlendPollResult::Ready,
                            label + ": native execution completes");
        if (poll != GpuBlendPollResult::Ready)
            continue;
        const auto result = blend.blend->takeImage();
        expectations.expect(result.dataWindow() == baseWindow &&
                                result.displayWindow() == baseWindow,
                            label + ": base owns data and display windows");
        std::vector<Rgba32f> expected(basePixels);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto globalX = baseWindow.originX() + static_cast<std::int64_t>(x);
                const auto globalY = baseWindow.originY() + static_cast<std::int64_t>(y);
                Rgba32f tap = Rgba32f::transparent();
                if (!mode.matteAbsent && globalX >= secondWindow.originX() &&
                    globalX < secondWindow.maxXExclusive() &&
                    globalY >= secondWindow.originY() &&
                    globalY < secondWindow.maxYExclusive()) {
                    const auto localX = static_cast<std::size_t>(globalX - secondWindow.originX());
                    const auto localY = static_cast<std::size_t>(globalY - secondWindow.originY());
                    tap = secondPixels[localY * secondWidth + localX];
                }
                const auto index = static_cast<std::size_t>(y) * width + x;
                expected[index] = oracle(basePixels[index], tap, mode);
            }
        }
        // The feathered matte pixel lands on opaque base pixel 12; its Luma coverage must follow
        // its alpha on both routes instead of saturating the edge (Set Alpha publishes it as the
        // output alpha, so the readback below compares it directly).
        if (mode.kind != GpuImageCombineKind::SetChannel &&
            mode.channel == ImageCoverageChannel::Luma && !mode.matteAbsent) {
            const float edge = expected[12].alpha();
            const float straight = mode.invert ? 1.0F - edge : edge;
            expectations.expect(straight > 0.44F && straight < 0.45F,
                                label + ": feathered Luma matte coverage follows matte alpha");
        }
        const auto readback = readbackResidentImage(result, kBudget);
        expectations.expect(readback.hasValue(), label + ": oracle readback succeeds");
        if (readback) {
            static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, label));
            if (mode.kind == GpuImageCombineKind::MultiplyAlpha &&
                mode.factor == std::numeric_limits<float>::min())
                expectations.expect(readback.pixels[7] == Rgba32f::transparent(),
                                    label + ": rounded-zero alpha is exactly transparent black");
        }
    }
}
} // namespace bloom::render::blend_proof
