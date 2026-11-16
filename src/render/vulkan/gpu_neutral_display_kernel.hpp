#ifndef BLOOM_RENDER_VULKAN_GPU_NEUTRAL_DISPLAY_KERNEL_HPP
#define BLOOM_RENDER_VULKAN_GPU_NEUTRAL_DISPLAY_KERNEL_HPP

// Private to src/render/vulkan. The push constants of the Bloom Neutral v1 display kernel
// (shaders/neutral_display.comp), shared by its two owners: GpuNeutralDisplay (host words) and
// GpuResidentDisplay (resident image). The kernel's status block is the display pack's
// (bloom/render/gpu_display_pack.hpp).

#include <bloom/render/gpu_display_pack.hpp>

#include <cstdint>

namespace bloom::render::vulkan_detail {

struct NeutralDisplayPush final {
    std::uint32_t pixelCount = 0;
    std::uint32_t pixelLayout = 0;
    float lumaRed = 0.0F;
    float lumaGreen = 0.0F;
    float lumaBlue = 0.0F;
    std::uint32_t lumaCounted = 0;
};
static_assert(sizeof(NeutralDisplayPush) == 24, "the kernel's push constant block is 24 bytes");

[[nodiscard]] constexpr NeutralDisplayPush
neutralDisplayPush(const std::uint32_t pixelCount, const GpuDisplayPackRequest& pack) noexcept {
    NeutralDisplayPush push;
    push.pixelCount = pixelCount;
    push.pixelLayout = gpuDisplayPackLayoutWord(pack.layout);
    if (pack.luma.has_value()) {
        push.lumaRed = pack.luma->red;
        push.lumaGreen = pack.luma->green;
        push.lumaBlue = pack.luma->blue;
        push.lumaCounted = 1;
    }
    return push;
}

} // namespace bloom::render::vulkan_detail

#endif // BLOOM_RENDER_VULKAN_GPU_NEUTRAL_DISPLAY_KERNEL_HPP
