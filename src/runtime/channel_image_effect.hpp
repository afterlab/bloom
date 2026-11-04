#pragma once

// #479 Separate Channels and Combine Channels: the one CPU reference of the Channel Select and
// Channel Combine kernels. Channel Select's resident GPU program reads the identical prepared
// mapping from its storage buffer; Channel Combine runs on the resident ImageCombineV1 Set Channel
// mode over the same render::storedImageChannel and render::setImageChannel arithmetic. Neither
// performs a colour transform or divides by alpha, so a Raw data pass moves between channels as
// stored and HDR and signed values stay unclamped; only alpha is limited to [0, 1].

#include <bloom/render/image_coverage.hpp>
#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace bloom::runtime::detail {

struct PreparedChannelSelect final {
    std::array<ImageChannelSource, 4> sources{};
    bool premultiply = false;
    // The mapping is red, green, blue, alpha without premultiply: the input passes unchanged.
    bool identity = false;
    friend bool operator==(const PreparedChannelSelect&, const PreparedChannelSelect&) = default;
};

struct PreparedChannelCombine final {
    std::uint8_t slot = 0;
    render::ImageCoverageChannel source = render::ImageCoverageChannel::Red;
    bool premultiply = false;
    friend bool operator==(const PreparedChannelCombine&, const PreparedChannelCombine&) = default;
};

// The stored channel a source reads; null for the Zero and One constants or an unknown value.
[[nodiscard]] std::optional<render::ImageCoverageChannel>
storedChannelOf(ImageChannelSource source) noexcept;

// Null for a source outside the closed enumeration (and, for Combine, a constant or a slot > 3).
[[nodiscard]] std::optional<PreparedChannelSelect>
prepareChannelSelect(const ChannelSelectKernel& kernel) noexcept;
[[nodiscard]] std::optional<PreparedChannelCombine>
prepareChannelCombine(const ChannelCombineKernel& kernel) noexcept;

// One premultiplied pixel. Fails when a Luma read is not representable.
[[nodiscard]] render::ImageResult<render::Rgba32f>
channelSelectPixel(const PreparedChannelSelect& select, render::Rgba32f pixel) noexcept;
// `second` is transparent outside the second image's data window or when it has no image.
[[nodiscard]] render::ImageResult<render::Rgba32f>
channelCombinePixel(const PreparedChannelCombine& combine, render::Rgba32f base,
                    render::Rgba32f second) noexcept;

// The prepared kernels as cache identity bytes.
[[nodiscard]] std::string channelSelectIdentity(const PreparedChannelSelect& select);
[[nodiscard]] std::string channelCombineIdentity(const PreparedChannelCombine& combine);

struct ImageEffectResult;
// Every pixel of the input's data window; the windows and pixel aspect are preserved.
[[nodiscard]] ImageEffectResult applyChannelSelect(const PreparedChannelSelect&,
                                                   const render::Rgba32fImage& input,
                                                   std::size_t budget, const CancellationToken&,
                                                   OperationIndex,
                                                   const EvaluationProgressCallback&);
// Every pixel of the base's data window; `second` (null: no image) is read at the same global
// pixel. The base's windows and pixel aspect are preserved.
[[nodiscard]] ImageEffectResult
applyChannelCombine(const PreparedChannelCombine&, const render::Rgba32fImage& base,
                    const render::Rgba32fImage* second, std::size_t budget,
                    const CancellationToken&, OperationIndex, const EvaluationProgressCallback&);

// The fixed Channel Select program's storage-buffer data: [the four slot sources], [premultiply,
// 0, 0, 0].
[[nodiscard]] std::array<float, 8> channelSelectProgramData(const PreparedChannelSelect& select);
[[nodiscard]] render::OcioGpuProgramResult buildChannelSelectProgram(const std::array<float, 8>&);

} // namespace bloom::runtime::detail
