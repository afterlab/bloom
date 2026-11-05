#ifndef BLOOM_RENDER_VULKAN_GPU_BLEND_PLAN_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_BLEND_PLAN_PRIVATE_HPP

// Private to src/render/vulkan. The geometry, mode and push-constant plan of one blend or image
// combine dispatch, shared by GpuBlend::beginBlend and the batched blend (#386) so both submit the
// same kernel with the same values.

#include "gpu_blend_private.hpp"

#include <bloom/core/blend_mode.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace bloom::render::blend_plan_detail {

// The kernel adds a per-axis int32 source offset to an int32 destination-local coordinate. An
// int64 origin difference can be far outside int32 (even overflowing int64), and narrowing it would
// silently wrap the source position and report a false overlap. This computes either an exact
// representable offset for a genuinely overlapping axis, or a sentinel guaranteed to be out of the
// source window for a non-overlapping/unrepresentable one, and fails closed when the extents are
// too large for destLocal + offset to stay in int32.
[[nodiscard]] inline bool safeSourceOffset(const std::int64_t destinationOrigin,
                                           const std::int64_t sourceOrigin,
                                           const std::uint32_t outputExtent,
                                           const std::uint32_t sourceExtent,
                                           std::int32_t& out) noexcept {
    const auto outputExtent64 = static_cast<std::int64_t>(outputExtent);
    const auto sourceExtent64 = static_cast<std::int64_t>(sourceExtent);
    if (outputExtent64 + sourceExtent64 >
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    std::int64_t delta = 0;
    // Overlap on this axis is exactly delta in (-outputExtent, sourceExtent); a subtraction
    // overflow or anything outside that range cannot overlap and is forced transparent.
    if (__builtin_sub_overflow(destinationOrigin, sourceOrigin, &delta) ||
        delta <= -outputExtent64 || delta >= sourceExtent64) {
        out = static_cast<std::int32_t>(-(outputExtent64 + sourceExtent64));
        return true;
    }
    out = static_cast<std::int32_t>(delta);
    return true;
}

// Which of GpuBlend's kernels a dispatch uses.
enum class BlendKernel : std::uint8_t { Exact, General, ImageCombine };

struct BlendPlan final {
    BlendKernel kernel = BlendKernel::Exact;
    BlendPush push{};
    ImageCombinePush combine{};
    std::uint32_t destWidth = 0;
    std::uint32_t destHeight = 0;
    std::uint64_t imageBytes = 0;
};

// Validates the blend's mode, image combine fields and windows and computes its push constants.
// `mask` is the Mask's data window when the image combine reads a present Mask.
[[nodiscard]] inline GpuBlendDiagnostic
planBlend(const ImageWindow& sourceWindow, const ImageWindow& destinationWindow,
          const std::optional<ImageWindow>& mask, const core::BlendMode mode,
          const std::optional<GpuImageCombineParameters>& imageCombine, const bool maskSupplied,
          BlendPlan& out) {
    const std::uint32_t destWidth = destinationWindow.extent().width();
    const std::uint32_t destHeight = destinationWindow.extent().height();
    const std::uint32_t sourceWidth = sourceWindow.extent().width();
    const std::uint32_t sourceHeight = sourceWindow.extent().height();
    if (destWidth == 0 || destHeight == 0 || sourceWidth == 0 || sourceHeight == 0) {
        return blendDiagnostic(GpuBlendDiagnosticCode::InvalidArgument, "an extent is empty");
    }
    // A BlendMode that does not round-trip through the durable stored mapping is a caller bug, not
    // a request to render Normal. Reject it here: the shader's separable helper falls through to
    // the source value for an unknown mode, so it must never receive one.
    const std::int64_t storedMode = core::blendModeStoredValue(mode);
    if (!imageCombine && !core::blendModeFromStoredValue(storedMode).has_value()) {
        return blendDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                               "the blend mode is not a known core::BlendMode value");
    }
    if (imageCombine &&
        (static_cast<std::uint8_t>(imageCombine->kind) >
             static_cast<std::uint8_t>(GpuImageCombineKind::SetChannel) ||
         static_cast<std::uint8_t>(imageCombine->channel) >
             static_cast<std::uint8_t>(ImageCoverageChannel::Luma) ||
         !std::isfinite(imageCombine->factor) || imageCombine->factor < 0.0F ||
         imageCombine->factor > 1.0F)) {
        return blendDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                               "the image combine kind, channel or factor is invalid");
    }
    // #479: only Set Channel writes a slot or premultiplies, and it never inverts or masks.
    if (imageCombine &&
        (imageCombine->kind == GpuImageCombineKind::SetChannel
             ? imageCombine->slot > 3 || imageCombine->invert ||
                   imageCombine->mask != GpuImageCombineParameters::Mask::None
             : imageCombine->slot != 0 || imageCombine->premultiply)) {
        return blendDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                               "the image combine slot does not match its mode");
    }
    const bool maskPresent =
        imageCombine && imageCombine->mask == GpuImageCombineParameters::Mask::Present;
    if (imageCombine &&
        (static_cast<std::uint8_t>(imageCombine->mask) >
             static_cast<std::uint8_t>(GpuImageCombineParameters::Mask::Absent) ||
         (imageCombine->mask != GpuImageCombineParameters::Mask::None &&
          imageCombine->kind != GpuImageCombineKind::Mix) ||
         maskPresent != maskSupplied || (maskPresent && !mask.has_value()))) {
        return blendDiagnostic(GpuBlendDiagnosticCode::InvalidArgument,
                               "the image combine mask does not match its mode");
    }
    std::int32_t maskOffsetX = 0;
    std::int32_t maskOffsetY = 0;
    std::uint32_t maskWidth = 0;
    std::uint32_t maskHeight = 0;
    if (maskPresent) {
        maskWidth = mask->extent().width();
        maskHeight = mask->extent().height();
        if (maskWidth == 0 || maskHeight == 0 ||
            !safeSourceOffset(destinationWindow.originX(), mask->originX(), destWidth, maskWidth,
                              maskOffsetX) ||
            !safeSourceOffset(destinationWindow.originY(), mask->originY(), destHeight,
                              maskHeight, maskOffsetY))
            return blendDiagnostic(GpuBlendDiagnosticCode::Unsupported,
                                   "the mask extent is empty or too large for a safe offset");
    }
    // Resolve the int32 shader offsets safely BEFORE any driver resource is created. A huge int64
    // origin difference is either represented exactly (overlap) or forced transparent (no overlap),
    // never narrowed and wrapped.
    std::int32_t sourceOffsetX = 0;
    std::int32_t sourceOffsetY = 0;
    if (!safeSourceOffset(destinationWindow.originX(), sourceWindow.originX(), destWidth,
                          sourceWidth, sourceOffsetX) ||
        !safeSourceOffset(destinationWindow.originY(), sourceWindow.originY(), destHeight,
                          sourceHeight, sourceOffsetY)) {
        return blendDiagnostic(GpuBlendDiagnosticCode::Unsupported,
                               "the source/destination extents are too large for a safe int32 "
                               "source offset");
    }
    std::uint64_t imageBytes = 0;
    if (compositeMultiplyOverflows(destWidth, destHeight, imageBytes) ||
        compositeMultiplyOverflows(imageBytes, sizeof(Rgba32f), imageBytes)) {
        return blendDiagnostic(GpuBlendDiagnosticCode::OverBudget,
                               "the blend request overflows the byte arithmetic");
    }
    // Normal is the exact retained fma source-over and Add is the exact premultiplied sum, so both
    // stay on the exact Float32 kernel. The general modes (Multiply .. Mask) use the exact Float64
    // companion when it was selected, and the compensated-Float32 portable kernel otherwise.
    out.kernel = imageCombine ? BlendKernel::ImageCombine
                 : (mode != core::BlendMode::Normal && mode != core::BlendMode::Add)
                     ? BlendKernel::General
                     : BlendKernel::Exact;
    // An absent matte declares an empty source extent: the shader's window test then rejects every
    // tap, which is the CPU reference's transparent-black matte outside any data window.
    const bool matteAbsent = imageCombine && imageCombine->matteAbsent;
    out.push = BlendPush{destWidth,
                         destHeight,
                         matteAbsent ? 0U : sourceWidth,
                         matteAbsent ? 0U : sourceHeight,
                         matteAbsent ? 0 : sourceOffsetX,
                         matteAbsent ? 0 : sourceOffsetY,
                         imageCombine ? 8U + static_cast<std::uint32_t>(imageCombine->kind)
                                      : static_cast<std::uint32_t>(storedMode)};
    if (imageCombine) {
        // Bits 0-2 of the channel word select the channel and bit 3 the inverted coverage; Set
        // Channel (#479) adds its slot in bits 4-5 and its premultiply in bit 6.
        out.combine = ImageCombinePush{
            out.push,
            imageCombine->factor,
            static_cast<std::uint32_t>(imageCombine->channel) |
                (imageCombine->invert ? kImageCombineInvertBit : 0U) |
                (static_cast<std::uint32_t>(imageCombine->slot) << kImageCombineSlotShift) |
                (imageCombine->premultiply ? kImageCombinePremultiplyBit : 0U),
            maskWidth,
            maskHeight,
            maskOffsetX,
            maskOffsetY,
            static_cast<std::uint32_t>(imageCombine->mask)};
    }
    out.destWidth = destWidth;
    out.destHeight = destHeight;
    out.imageBytes = imageBytes;
    return {};
}

} // namespace bloom::render::blend_plan_detail

#endif // BLOOM_RENDER_VULKAN_GPU_BLEND_PLAN_PRIVATE_HPP
