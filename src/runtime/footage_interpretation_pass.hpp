#pragma once

// Interpret Footage (#381): the footage interpretation pass. Alpha (Ignore, Invert, a
// premultiplied matte color) and deinterlacing are pixel transformations of the file's stored
// samples, so they run as one pass before the input colour transform: on the CPU reference here,
// and on the GPU as a ProcessNeighborhood program with the same binary32 arithmetic.
//
// Its input is a stored-samples image (media::ImageInterpretation::storedSamples): stored R, G, B
// and fma(A, 0.5, 0.5). Its output is premultiplied pre-colour-transform RGBA, the same state an
// ordinary decode publishes, so proxy resampling and the colour transform follow unchanged.

#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bloom::color {
class CpuColorSpaceProcessor;
} // namespace bloom::color

namespace bloom::runtime::detail {

struct FootagePass final {
    enum class Alpha : std::uint8_t { Ignore, Straight, Premultiplied };
    Alpha alpha = Alpha::Straight;
    bool invert = false;
    // Premultiplied only: the colour the alpha was matted against, in the file's encoding.
    std::array<float, 3> matte{};
    // 0: no deinterlace; 1: keep the upper field (even frame rows) and interpolate the others; 2:
    // keep the lower field.
    std::uint8_t keepField = 0;
    // The frame row of the image's first data row, mod 2 (a cropped OpenEXR data window).
    std::uint8_t rowParity = 0;
    friend bool operator==(const FootagePass&, const FootagePass&) = default;
    // Stable identity bytes for cache keys and the GPU preparer.
    [[nodiscard]] std::vector<float> parameters() const;
    [[nodiscard]] std::string identity() const;
};

// The pass `asset` needs when decoded with `association`, or nullopt when the ordinary decode
// already is the interpretation (Guess, Straight or Premultiplied over black, progressive).
[[nodiscard]] std::optional<FootagePass> footagePassFor(const document::AssetRecord& asset,
                                                        media::ImageAlphaAssociation association);

// The CPU reference: the pass over a stored-samples image, at its own resolution.
[[nodiscard]] std::variant<render::Rgba32fImage, std::string>
applyFootagePass(const FootagePass& pass, const render::Rgba32fImage& stored, std::size_t budget,
                 const std::function<bool()>& cancelled);

// An input colour processor over a premultiplied image: un-premultiply, transform the straight
// RGB, re-premultiply (an alpha-zero pixel stays transparent). The CPU twin of the GPU OCIO CST
// that follows the pass.
[[nodiscard]] std::variant<render::Rgba32fImage, std::string>
applyInputProcessorPremultiplied(const render::Rgba32fImage& image,
                                 const color::CpuColorSpaceProcessor& processor, std::size_t budget,
                                 const std::function<bool()>& cancelled);

// The GPU ProcessNeighborhood program of `pass` (parameters are baked into the shader text, so the
// shader digest is the pass identity).
[[nodiscard]] render::OcioGpuProgramResult
buildFootagePassProgram(std::span<const float> parameters);

} // namespace bloom::runtime::detail
