#include <bloom/render/native_samples.hpp>

#include "vulkan/shaders/sample_convert_spirv.inc"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace bloom::render {
namespace {

[[nodiscard]] constexpr std::uint32_t ceilHalf(const std::uint32_t value) noexcept {
    return value / 2U + (value & 1U);
}

[[nodiscard]] std::uint32_t sampleBytes(const NativeSampleLayout layout) noexcept {
    switch (layout) {
    case NativeSampleLayout::Yuv420p8:
        return 1;
    case NativeSampleLayout::Yuva444p16:
        return 2;
    case NativeSampleLayout::Rgba16:
    case NativeSampleLayout::RgbaHalf:
        return 8;
    }
    return 0;
}

[[nodiscard]] std::size_t planeCount(const NativeSampleLayout layout) noexcept {
    switch (layout) {
    case NativeSampleLayout::Yuv420p8:
        return 3;
    case NativeSampleLayout::Yuva444p16:
        return 4;
    case NativeSampleLayout::Rgba16:
    case NativeSampleLayout::RgbaHalf:
        return 1;
    }
    return 0;
}

[[nodiscard]] float code8(const NativeSamplePlane& plane, const std::uint32_t x,
                          const std::uint32_t y) noexcept {
    const auto offset = static_cast<std::size_t>(y) * plane.stride + x;
    return static_cast<float>(std::to_integer<unsigned>(plane.bytes[offset]));
}

[[nodiscard]] std::uint32_t word16(const NativeSamplePlane& plane, const std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(plane.bytes[offset]) |
           (std::to_integer<std::uint32_t>(plane.bytes[offset + 1]) << 8U);
}

[[nodiscard]] float code16(const NativeSamplePlane& plane, const std::uint32_t x,
                           const std::uint32_t y, const std::uint32_t lane = 0,
                           const std::uint32_t lanes = 1) noexcept {
    const auto offset = static_cast<std::size_t>(y) * plane.stride +
                        (static_cast<std::size_t>(x) * lanes + lane) * 2U;
    return static_cast<float>(word16(plane, offset));
}

// The decoders' 4:2:0 reconstruction (media::video colour.cpp): horizontally co-sited with even
// luma columns, vertically between luma rows, bilinear.
[[nodiscard]] float chroma(const NativeSamplePlane& plane, const std::uint32_t x,
                           const std::uint32_t y) noexcept {
    const auto at = [&](std::int64_t cx, std::int64_t cy) {
        cx = std::clamp<std::int64_t>(cx, 0, static_cast<std::int64_t>(plane.width) - 1);
        cy = std::clamp<std::int64_t>(cy, 0, static_cast<std::int64_t>(plane.height) - 1);
        return code8(plane, static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cy));
    };
    const auto cx = static_cast<std::int64_t>(x / 2U);
    const float fx = (x & 1U) != 0 ? 0.5F : 0.0F;
    const auto vy = static_cast<std::int64_t>(4 * static_cast<std::int64_t>(y) - 2);
    const auto cy = vy < 0 ? std::int64_t{-1} : vy / 8;
    const float fy = static_cast<float>((vy - cy * 8)) / 8.0F;
    const auto top = at(cx, cy) * (1.0F - fx) + at(cx + 1, cy) * fx;
    const auto bottom = at(cx, cy + 1) * (1.0F - fx) + at(cx + 1, cy + 1) * fx;
    return top * (1.0F - fy) + bottom * fy;
}

// color::video_colour.cpp's inverse transfer.
[[nodiscard]] float inverseTransfer(const float value, const std::int32_t transfer) noexcept {
    if (transfer == 8)
        return value;
    if (transfer == 13)
        return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
    return value < 0.081F ? value / 4.5F : std::pow((value + 0.099F) / 1.099F, 1.0F / 0.45F);
}

[[nodiscard]] float halfToFloat(const std::uint32_t half) noexcept {
    const std::uint32_t sign = (half & 0x8000U) << 16U;
    const std::uint32_t exponent = (half >> 10U) & 0x1FU;
    std::uint32_t mantissa = half & 0x3FFU;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal half: normalise into a binary32 normal.
            std::int32_t shift = 0;
            while ((mantissa & 0x400U) == 0) {
                mantissa <<= 1U;
                ++shift;
            }
            mantissa &= 0x3FFU;
            bits = sign | (static_cast<std::uint32_t>(127 - 15 + 1 - shift) << 23U) |
                   (mantissa << 13U);
        }
    } else if (exponent == 0x1FU) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 127U - 15U) << 23U) | (mantissa << 13U);
    }
    return std::bit_cast<float>(bits);
}

// One converted sample before alpha association: straight RGB and alpha.
[[nodiscard]] std::array<float, 4> sampleAt(const NativeSampleImage& image, const std::uint32_t x,
                                            const std::uint32_t y) noexcept {
    const auto& conversion = image.conversion();
    const auto planes = image.planes();
    switch (conversion.layout) {
    case NativeSampleLayout::Yuv420p8:
    case NativeSampleLayout::Yuva444p16: {
        const bool wide = conversion.layout == NativeSampleLayout::Yuva444p16;
        const auto yCode = wide ? code16(planes[0], x, y) : code8(planes[0], x, y);
        const auto uCode = wide ? code16(planes[1], x, y) : chroma(planes[1], x, y);
        const auto vCode = wide ? code16(planes[2], x, y) : chroma(planes[2], x, y);
        const bool limited = conversion.limitedRange;
        const bool fullWide = wide && !limited;
        const auto divisor = wide ? 256.0F : 1.0F;
        constexpr float kWideToEight = 255.0F / 65535.0F;
        const auto luma = fullWide ? yCode * kWideToEight : yCode / divisor;
        const auto cb = fullWide ? 128.0F + (uCode - 32768.0F) * kWideToEight : uCode / divisor;
        const auto cr = fullWide ? 128.0F + (vCode - 32768.0F) * kWideToEight : vCode / divisor;
        const auto yy = limited ? (luma - 16.0F) / 219.0F : luma / 255.0F;
        const auto u = (cb - 128.0F) / (limited ? 224.0F : 255.0F);
        const auto v = (cr - 128.0F) / (limited ? 224.0F : 255.0F);
        const auto red = inverseTransfer(yy + conversion.redFromCr * v, conversion.transfer);
        const auto green = inverseTransfer(
            yy - conversion.greenFromCb * u - conversion.greenFromCr * v, conversion.transfer);
        const auto blue = inverseTransfer(yy + conversion.blueFromCb * u, conversion.transfer);
        const auto alpha = wide ? code16(planes[3], x, y) / 65535.0F : 1.0F;
        return {red, green, blue, alpha};
    }
    case NativeSampleLayout::Rgba16: {
        std::array<float, 4> sample{};
        for (std::uint32_t c = 0; c < 4; ++c)
            sample[c] = code16(planes[0], x, y, c, 4) / 65535.0F;
        return sample;
    }
    case NativeSampleLayout::RgbaHalf: {
        const auto base = static_cast<std::size_t>(y) * planes[0].stride +
                          static_cast<std::size_t>(x) * 8U;
        std::array<std::uint32_t, 4> bits{};
        for (std::size_t c = 0; c < 4; ++c)
            bits[c] = word16(planes[0], base + c * 2U);
        if (conversion.luminance)
            bits[1] = bits[2] = bits[0];
        std::array<float, 4> sample{};
        for (std::size_t c = 0; c < 4; ++c)
            sample[c] = halfToFloat(bits[c]);
        if (!conversion.hasAlpha)
            sample[3] = 1.0F;
        // The OpenEXR ingest policy: a NaN/Inf component becomes zero, alpha is clamped to [0, 1].
        for (auto& component : sample)
            if (!std::isfinite(component))
                component = 0.0F;
        if (sample[3] < 0.0F || sample[3] > 1.0F)
            sample[3] = std::clamp(sample[3], 0.0F, 1.0F);
        return sample;
    }
    }
    return {};
}

} // namespace

ImageResult<std::shared_ptr<const NativeSampleImage>>
NativeSampleImage::create(const Rgba32fImageDescriptor descriptor,
                          const NativeSampleConversion conversion,
                          const std::span<const NativeSamplePlane> planes,
                          std::shared_ptr<const void> owner) noexcept {
    using Result = ImageResult<std::shared_ptr<const NativeSampleImage>>;
    const auto invalid = [] {
        return Result::failure(ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    };
    const auto expected = planeCount(conversion.layout);
    if (expected == 0 || planes.size() != expected || owner == nullptr)
        return invalid();
    const auto width = descriptor.dataWindow().extent().width();
    const auto height = descriptor.dataWindow().extent().height();
    std::array<NativeSamplePlane, kMaxNativeSamplePlanes> stored{};
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < planes.size(); ++index) {
        const auto& plane = planes[index];
        const bool chromaPlane = conversion.layout == NativeSampleLayout::Yuv420p8 && index != 0;
        const auto planeWidth = chromaPlane ? ceilHalf(width) : width;
        const auto planeHeight = chromaPlane ? ceilHalf(height) : height;
        const std::uint64_t rowBytes =
            static_cast<std::uint64_t>(plane.width) * sampleBytes(conversion.layout);
        const std::uint64_t bytes = static_cast<std::uint64_t>(plane.stride) * plane.height;
        if (plane.width != planeWidth || plane.height != planeHeight || plane.stride < rowBytes ||
            plane.bytes.size() < bytes || plane.bytes.data() == nullptr)
            return invalid();
        stored[index] = plane;
        stored[index].bytes = plane.bytes.first(static_cast<std::size_t>(bytes));
        total += bytes;
    }
    try {
        return Result::success(std::make_shared<const NativeSampleImage>(
            descriptor, conversion, stored, planes.size(), total, std::move(owner)));
    } catch (...) {
        return Result::failure(ImageError::codeOnly(ImageErrorCode::AllocationFailure));
    }
}

ImageResult<Rgba32fImage> convertNativeSamples(const NativeSampleImage& samples,
                                               const std::size_t pixelStorageByteLimit,
                                               const std::function<bool()>& cancel) noexcept {
    auto builder = Rgba32fImageBuilder::create(samples.descriptor(), pixelStorageByteLimit);
    if (!builder)
        return ImageResult<Rgba32fImage>::failure(*builder.error());
    const auto& conversion = samples.conversion();
    const auto window = samples.descriptor().dataWindow();
    const auto width = window.extent().width();
    const auto height = window.extent().height();
    for (std::uint32_t y = 0; y < height; ++y) {
        if (cancel && cancel())
            return ImageResult<Rgba32fImage>::failure(
                ImageError::codeOnly(ImageErrorCode::InvalidState));
        auto row = builder.value()->row(window.originY() + static_cast<std::int64_t>(y));
        if (!row)
            return ImageResult<Rgba32fImage>::failure(*row.error());
        for (std::uint32_t x = 0; x < width; ++x) {
            auto sample = sampleAt(samples, x, y);
            if (!conversion.storedSamples && conversion.alpha == NativeSampleAlpha::Unpremultiply)
                for (std::size_t c = 0; c < 3; ++c)
                    sample[c] = sample[3] > 0.0F ? sample[c] / sample[3] : 0.0F;
            const auto pixel =
                conversion.storedSamples
                    ? Rgba32f::fromPremultiplied(sample[0], sample[1], sample[2],
                                                 std::fma(sample[3], 0.5F, 0.5F))
                : conversion.alpha == NativeSampleAlpha::Premultiplied
                    ? Rgba32f::fromPremultiplied(sample[0], sample[1], sample[2], sample[3])
                    : Rgba32f::fromPremultiplied(sample[0] * sample[3], sample[1] * sample[3],
                                                 sample[2] * sample[3], sample[3]);
            if (!pixel)
                return ImageResult<Rgba32fImage>::failure(*pixel.error());
            (*row.value())[x] = *pixel.value();
        }
    }
    return std::move(*builder.value()).freeze();
}

float nativeSampleExactDivide(const float a, const float b) noexcept {
    // Mirrors bloomExactDivide in sample_convert.comp line for line.
    const auto sa = std::bit_cast<std::uint32_t>(a);
    const auto sb = std::bit_cast<std::uint32_t>(b);
    if ((sa & 0x7FFFFFFFU) == 0U)
        return a;
    const std::uint32_t sign = sa & 0x80000000U;
    std::int32_t ea = static_cast<std::int32_t>((sa >> 23U) & 0xFFU);
    const auto eb = static_cast<std::int32_t>((sb >> 23U) & 0xFFU);
    std::uint32_t ma = sa & 0x7FFFFFU;
    const std::uint32_t mb = (sb & 0x7FFFFFU) | 0x800000U;
    if (ea == 0) {
        ea = 1;
        while ((ma & 0x800000U) == 0U) {
            ma <<= 1U;
            --ea;
        }
    } else {
        ma |= 0x800000U;
    }
    std::int32_t exponent = ea - eb;
    std::uint32_t remainder = ma;
    if (remainder < mb) {
        remainder <<= 1U;
        --exponent;
    }
    std::uint32_t quotient = 0;
    for (int bit = 0; bit < 25; ++bit) {
        quotient <<= 1U;
        if (remainder >= mb) {
            remainder -= mb;
            quotient |= 1U;
        }
        remainder <<= 1U;
    }
    std::uint32_t mantissa = quotient >> 1U;
    const bool guard = (quotient & 1U) != 0U;
    if (guard && (remainder != 0U || (mantissa & 1U) != 0U))
        ++mantissa;
    if (mantissa == 0x1000000U) {
        mantissa >>= 1U;
        ++exponent;
    }
    const std::int32_t biased = exponent + 127;
    if (biased <= 0 || biased >= 255)
        return a / b;
    return std::bit_cast<float>(sign | (static_cast<std::uint32_t>(biased) << 23U) |
                                (mantissa & 0x7FFFFFU));
}

std::string_view sampleConvertKernelDigest() noexcept {
    return vulkan_detail::kSampleConvertSpirvDigest;
}

} // namespace bloom::render
