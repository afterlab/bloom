#pragma once

// Decoded media samples in their native layout (#422).
//
// A decoder hands its samples over as they came out of the codec: 8-bit 4:2:0 or 16-bit 4:4:4:4
// YUV planes, packed 16-bit RGBA, or packed binary16 RGBA. The GPU route uploads those bytes as
// they are and SampleConvertV1 widens them to the premultiplied RGBA32F working image on the
// device: the YUV matrix and range, bit-depth expansion, alpha association and the stored-samples
// encoding of the footage pass. Media decode (container I/O and decompression) stays host
// preparation; this conversion is GPU pixel work.
//
// `convertNativeSamples` is the CPU reference of SampleConvertV1. It reproduces the decoders' own
// CPU conversions (media::video::videoToInputColorSpace and videoToStoredSamples, and the raw
// PNG/JPEG/OpenEXR decodes) value for value, and the kernel reproduces it: every division by a
// sample range or by alpha is evaluated as a correctly rounded float division on both sides, and
// every other step is one correctly rounded add, subtract or multiply. Only the legacy BT.709 and
// sRGB inverse transfer curves (transfer 1, 6 and 13, which the config-managed GPU route never
// selects) use pow() on the device and are held to the documented 2e-6 tolerance instead.

#include <bloom/render/image.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace bloom::render {

enum class NativeSampleLayout : std::uint8_t {
    // Planes Y, Cb, Cr of 8-bit codes. Cb and Cr are ceil(w/2) x ceil(h/2), left-sited
    // horizontally and centred vertically, and are reconstructed with the decoders' bilinear
    // filter.
    Yuv420p8 = 1,
    // Planes Y, Cb, Cr, A of 16-bit little-endian codes at full resolution.
    Yuva444p16 = 2,
    // One plane of interleaved R, G, B, A 16-bit little-endian codes (unorm).
    Rgba16 = 3,
    // One plane of interleaved binary16 R, G, B, A. With `luminance` the first lane is Y and is
    // replicated to R, G and B; without `hasAlpha` alpha is 1.
    RgbaHalf = 4,
};

// Every layout, in enumerator order. The GPU coverage contract requires one native fixture per
// entry (feature.media.native_samples.<name>), and the static_assert below fails the build when an
// enumerator is added without extending this list.
inline constexpr std::array<NativeSampleLayout, 4> kNativeSampleLayouts{
    NativeSampleLayout::Yuv420p8, NativeSampleLayout::Yuva444p16, NativeSampleLayout::Rgba16,
    NativeSampleLayout::RgbaHalf};
[[nodiscard]] constexpr std::string_view nativeSampleLayoutName(NativeSampleLayout layout) noexcept {
    switch (layout) {
    case NativeSampleLayout::Yuv420p8:
        return "yuv420p8";
    case NativeSampleLayout::Yuva444p16:
        return "yuva444p16";
    case NativeSampleLayout::Rgba16:
        return "rgba16";
    case NativeSampleLayout::RgbaHalf:
        return "rgba_half";
    }
    return {};
}
static_assert([] {
    for (std::size_t index = 0; index < kNativeSampleLayouts.size(); ++index)
        if (static_cast<std::size_t>(kNativeSampleLayouts[index]) != index + 1 ||
            nativeSampleLayoutName(kNativeSampleLayouts[index]).empty())
            return false;
    // The enumerator after the last listed one must not exist.
    return nativeSampleLayoutName(static_cast<NativeSampleLayout>(kNativeSampleLayouts.size() + 1))
        .empty();
}());

// How alpha relates to the colour lanes of the decoded samples.
enum class NativeSampleAlpha : std::uint8_t {
    // Straight: the output is (rgb * a, a).
    Straight = 1,
    // Already premultiplied: the output keeps the samples (OpenEXR premultiplied stills).
    Premultiplied = 2,
    // Stored premultiplied but decoded through straight RGB (a PNG or JPEG declared premultiplied):
    // rgb = a > 0 ? rgb / a : 0, then (rgb * a, a).
    Unpremultiply = 3,
};

struct NativeSampleConversion final {
    NativeSampleLayout layout = NativeSampleLayout::Rgba16;
    NativeSampleAlpha alpha = NativeSampleAlpha::Straight;
    // #381 stored samples: (r, g, b, fma(a, 0.5, 0.5)), never premultiplied or unpremultiplied.
    bool storedSamples = false;
    // YUV layouts. `limitedRange` selects 16-235/16-240 (scaled by 256 for 16-bit codes);
    // otherwise full range. `transfer` is the inverse transfer applied to R'G'B': 8 none (the
    // config-managed route), 13 sRGB, anything else the BT.709 curve. The four coefficients are the
    // YUV matrix exactly as color::yuvToSceneLinear evaluates it:
    //   R = yy + redFromCr * v;  G = (yy - greenFromCb * u) - greenFromCr * v;  B = yy + blueFromCb * u.
    bool limitedRange = true;
    std::int32_t transfer = 8;
    float redFromCr = 0.0F;
    float greenFromCb = 0.0F;
    float greenFromCr = 0.0F;
    float blueFromCb = 0.0F;
    // RgbaHalf only.
    bool luminance = false;
    bool hasAlpha = true;

    friend bool operator==(const NativeSampleConversion&, const NativeSampleConversion&) = default;
};

// One plane: `height` rows of `stride` bytes; a row holds `width` samples of the layout's size.
struct NativeSamplePlane final {
    std::span<const std::byte> bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
};

inline constexpr std::size_t kMaxNativeSamplePlanes = 4;

// Immutable. The planes are borrowed from `owner` (a decoded video frame, or the image decoder's
// sample buffer), which this object keeps alive.
class NativeSampleImage final {
  public:
    // Validates the plane count, extents and strides of `conversion.layout` against the data
    // window of `descriptor` (the converted image's descriptor) and that every plane fits its
    // bytes. Returns InvalidParameter otherwise.
    [[nodiscard]] static ImageResult<std::shared_ptr<const NativeSampleImage>>
    create(Rgba32fImageDescriptor descriptor, NativeSampleConversion conversion,
           std::span<const NativeSamplePlane> planes, std::shared_ptr<const void> owner) noexcept;

    [[nodiscard]] const Rgba32fImageDescriptor& descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] const NativeSampleConversion& conversion() const noexcept { return conversion_; }
    [[nodiscard]] std::span<const NativeSamplePlane> planes() const noexcept {
        return {planes_.data(), planeCount_};
    }
    // The host bytes the planes occupy (stride * height summed), what an upload copies.
    [[nodiscard]] std::uint64_t byteCount() const noexcept { return byteCount_; }
    // A stable identity of the borrowed bytes for once-per-backing accounting.
    [[nodiscard]] const void* backingIdentity() const noexcept {
        return planeCount_ == 0 ? nullptr : static_cast<const void*>(planes_[0].bytes.data());
    }

    NativeSampleImage(Rgba32fImageDescriptor descriptor, NativeSampleConversion conversion,
                      std::array<NativeSamplePlane, kMaxNativeSamplePlanes> planes,
                      std::size_t planeCount, std::uint64_t byteCount,
                      std::shared_ptr<const void> owner) noexcept
        : descriptor_(descriptor), conversion_(conversion), planes_(planes),
          planeCount_(planeCount), byteCount_(byteCount), owner_(std::move(owner)) {}

  private:
    Rgba32fImageDescriptor descriptor_;
    NativeSampleConversion conversion_;
    std::array<NativeSamplePlane, kMaxNativeSamplePlanes> planes_{};
    std::size_t planeCount_ = 0;
    std::uint64_t byteCount_ = 0;
    std::shared_ptr<const void> owner_;
};

// The CPU reference of SampleConvertV1: the premultiplied RGBA32F image `samples` converts to.
// `cancel` is polled once per row. Fails with PixelStorageBudgetExceeded over the budget and with
// InvalidPixel if a converted pixel is not a valid premultiplied pixel (which the decoders' own
// conversions would refuse the same way).
[[nodiscard]] ImageResult<Rgba32fImage>
convertNativeSamples(const NativeSampleImage& samples, std::size_t pixelStorageByteLimit,
                     const std::function<bool()>& cancel = {}) noexcept;

// The correctly rounded binary32 quotient a / b for finite a and a positive normal b whose
// quotient is normal or zero, computed exactly as SampleConvertV1 does on the device (an integer
// long division of the significands). Equal to the IEEE a / b; exposed for the kernel parity
// tests.
[[nodiscard]] float nativeSampleExactDivide(float a, float b) noexcept;

// The SHA-256 of the embedded SampleConvertV1 SPIR-V. Cache identities of converted samples fold
// it, so a changed kernel never serves an image the previous one produced.
[[nodiscard]] std::string_view sampleConvertKernelDigest() noexcept;

} // namespace bloom::render
