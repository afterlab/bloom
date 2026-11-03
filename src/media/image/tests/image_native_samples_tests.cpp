// #422: a raw still decode's native samples (PNG/JPEG 16-bit RGBA, all-half OpenEXR binary16
// RGBA) convert through render::convertNativeSamples, the CPU reference of SampleConvertV1, to the
// exact image the raw float decode produces, with the same repaired-pixel count. Any source
// without a native form (a FLOAT OpenEXR layer, a converted interpretation) is refused with
// NativeSamplesUnavailable so the caller decodes to float instead.

#include "generated_jpeg.hpp"
#include "png_test_support.hpp"

#include <Imath/half.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>
#include <ImfTileDescription.h>
#include <ImfTiledOutputFile.h>
#include <bloom/media/image.hpp>
#include <bloom/render/native_samples.hpp>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <zlib.h>

namespace {

using namespace bloom_output_png_test_support;
namespace media = bloom::media;
namespace render = bloom::render;

// A PNG of `width` x `height` with every row filtered None, encoded by the independent chunk
// writer and zlib.
void writePng(const std::filesystem::path& path, const std::uint32_t width,
              const std::uint32_t height, const unsigned depth, const unsigned colourType,
              const unsigned channels) {
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
        file.write(reinterpret_cast<const char*>(signature.data()), 8);
    }
    PngChunkBytes chunks(path);
    const auto be32 = [](std::vector<unsigned char>& out, const std::uint32_t value) {
        for (const unsigned shift : {24U, 16U, 8U, 0U})
            out.push_back(static_cast<unsigned char>((value >> shift) & 0xFFU));
    };
    std::vector<unsigned char> header;
    be32(header, width);
    be32(header, height);
    header.insert(header.end(), {static_cast<unsigned char>(depth),
                                 static_cast<unsigned char>(colourType), 0, 0, 0});
    chunks.insertChunk(chunks.endOffset(), "IHDR", header);
    std::vector<unsigned char> raw;
    std::uint32_t state = 0x9E3779B9U ^ (width * 31U + height * 7U + depth + colourType);
    const auto next = [&state] {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < width; ++x)
            for (unsigned channel = 0; channel < channels; ++channel) {
                auto value = next();
                // Every few pixels are fully transparent or opaque so the alpha endpoints and a
                // zero-alpha pixel's dropped colour are exercised.
                const bool alphaLane = channel + 1 == channels && (colourType == 4 || colourType == 6);
                if (alphaLane && x % 5 == 0)
                    value = 0;
                if (alphaLane && x % 7 == 1)
                    value = 0xFFFFFFFFU;
                if (depth == 16) {
                    raw.push_back(static_cast<unsigned char>(value >> 24U));
                    raw.push_back(static_cast<unsigned char>(value >> 16U));
                } else {
                    raw.push_back(static_cast<unsigned char>(value >> 24U));
                }
            }
    }
    auto size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(size);
    if (compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        std::abort();
    compressed.resize(size);
    chunks.insertChunk(chunks.endOffset(), "IDAT", compressed);
    chunks.insertChunk(chunks.endOffset(), "IEND", {});
    chunks.save();
}

struct ExrSpec final {
    Imf::PixelType type = Imf::HALF;
    bool luminance = false;
    bool alpha = true;
    bool tiled = false;
    const char* association = "premultiplied";
    // Plant NaN, infinities, out-of-range alpha and a zero-alpha emission for the repair policy.
    bool repairs = false;
};

void writeExr(const std::filesystem::path& path, const ExrSpec& spec) {
    constexpr int width = 37;
    constexpr int height = 11;
    const Imath::Box2i display(Imath::V2i(-3, 1), Imath::V2i(40, 14));
    const Imath::Box2i data(Imath::V2i(-1, 2), Imath::V2i(-1 + width - 1, 2 + height - 1));
    Imf::Header header(display, data);
    if (spec.tiled)
        header.setTileDescription(Imf::TileDescription(8, 4, Imf::ONE_LEVEL));
    if (spec.association != nullptr)
        header.insert("alphaAssociation", Imf::StringAttribute(spec.association));
    std::vector<const char*> names;
    if (spec.luminance)
        names = {"Y"};
    else
        names = {"R", "G", "B"};
    if (spec.alpha)
        names.push_back("A");
    for (const auto* name : names)
        header.channels().insert(name, Imf::Channel(spec.type));
    constexpr auto count = static_cast<std::size_t>(width) * height;
    std::vector<std::vector<Imath::half>> halves(names.size(), std::vector<Imath::half>(count));
    std::vector<std::vector<float>> floats(names.size(), std::vector<float>(count));
    std::uint32_t state = 0x1234567U;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8U) / static_cast<float>(1U << 24U);
    };
    for (std::size_t index = 0; index < count; ++index) {
        for (std::size_t channel = 0; channel < names.size(); ++channel) {
            const bool isAlpha = spec.alpha && channel + 1 == names.size();
            float value = isAlpha ? next() : next() * 6.0F - 2.0F;
            if (isAlpha && index % 9 == 0)
                value = 0.0F;
            if (isAlpha && index % 11 == 3)
                value = 1.0F;
            if (spec.repairs) {
                if (index % 13 == 5)
                    value = std::numeric_limits<float>::quiet_NaN();
                if (index % 17 == 6)
                    value = channel % 2 == 0 ? std::numeric_limits<float>::infinity()
                                             : -std::numeric_limits<float>::infinity();
                if (isAlpha && index % 19 == 7)
                    value = 1.75F;
                if (isAlpha && index % 23 == 8)
                    value = -0.25F;
            }
            // A subnormal half survives the conversion exactly.
            if (index == 2 && channel == 0)
                value = 3.0e-6F;
            halves[channel][index] = value;
            floats[channel][index] = value;
        }
    }
    Imf::FrameBuffer buffer;
    for (std::size_t channel = 0; channel < names.size(); ++channel) {
        if (spec.type == Imf::HALF)
            buffer.insert(names[channel],
                          Imf::Slice::Make(spec.type, halves[channel].data(), data,
                                           sizeof(Imath::half), sizeof(Imath::half) * width));
        else
            buffer.insert(names[channel], Imf::Slice::Make(spec.type, floats[channel].data(), data,
                                                           sizeof(float), sizeof(float) * width));
    }
    if (spec.tiled) {
        Imf::TiledOutputFile output(path.string().c_str(), header, 1);
        output.setFrameBuffer(buffer);
        output.writeTiles(0, output.numXTiles() - 1, 0, output.numYTiles() - 1, 0);
    } else {
        Imf::OutputFile output(path.string().c_str(), header, 1);
        output.setFrameBuffer(buffer);
        output.writePixels(height);
    }
}

[[nodiscard]] bool sameBits(const float a, const float b) noexcept {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

// Every pixel and the descriptor of the reference conversion equal the float decode bit for bit.
[[nodiscard]] bool identical(const render::Rgba32fImage& expected,
                             const render::Rgba32fImage& actual) {
    const auto* left = expected.descriptor();
    const auto* right = actual.descriptor();
    if (left == nullptr || right == nullptr || left->dataWindow() != right->dataWindow() ||
        left->displayWindow() != right->displayWindow() ||
        left->pixelAspect() != right->pixelAspect())
        return false;
    const auto a = expected.pixels();
    const auto b = actual.pixels();
    if (a.size() != b.size())
        return false;
    for (std::size_t index = 0; index < a.size(); ++index)
        if (!sameBits(a[index].red(), b[index].red()) ||
            !sameBits(a[index].green(), b[index].green()) ||
            !sameBits(a[index].blue(), b[index].blue()) ||
            !sameBits(a[index].alpha(), b[index].alpha()))
            return false;
    return true;
}

std::size_t comparedVariants = 0;

void expectParity(Expectations& check, const std::filesystem::path& path,
                  const render::NativeSampleLayout layout, const std::string& label) {
    const auto revision = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes);
    check.expect(revision.value.has_value(), label + ": capture");
    if (!revision.value)
        return;
    for (const auto association :
         {media::ImageAlphaAssociation::Auto, media::ImageAlphaAssociation::Straight,
          media::ImageAlphaAssociation::Premultiplied}) {
        for (const bool stored : {false, true}) {
            media::ImageInterpretation interpretation;
            interpretation.colorSpace = media::ImageColorSpace::Raw;
            interpretation.alphaAssociation = association;
            interpretation.storedSamples = stored;
            const auto variant = label + " association " +
                                 std::to_string(static_cast<int>(association)) +
                                 (stored ? " stored" : "");
            media::ImageDecodeStatistics floatStatistics;
            const auto expected = media::decodeImage(**revision.value, interpretation, {}, {}, {},
                                                     media::kMaxImageStorageBytes,
                                                     &floatStatistics);
            media::ImageDecodeStatistics sampleStatistics;
            const auto samples = media::decodeImageSamples(
                **revision.value, interpretation, {}, media::kMaxImageStorageBytes,
                &sampleStatistics);
            check.expect(expected.value.has_value() && samples.value.has_value(),
                         variant + ": both decodes succeed (" + samples.diagnostic + ")");
            if (!expected.value || !samples.value)
                continue;
            const auto& native = **samples.value;
            check.expect(native.conversion().layout == layout &&
                             native.conversion().storedSamples == stored,
                         variant + ": native layout and stored flag");
            const auto converted = render::convertNativeSamples(native, media::kMaxImageStorageBytes);
            check.expect(converted.hasValue() && identical(**expected.value, *converted.value()),
                         variant + ": the reference conversion equals the float decode exactly");
            ++comparedVariants;
            check.expect(samples.repairedPixels == expected.repairedPixels &&
                             sampleStatistics.repairedPixels == floatStatistics.repairedPixels,
                         variant + ": the same repaired-pixel count");
            check.expect(native.byteCount() * 2 == (*expected.value)->pixels().size_bytes(),
                         variant + ": half the bytes of the float decode cross to the device");
        }
    }
}

} // namespace

int main() {
    ScratchDirectory scratch("media-image-native-samples");
    Expectations check;
    // PNG grey, grey+alpha, RGB and RGBA at 8 and 16 bits (colour types 0, 4, 2, 6).
    for (const unsigned depth : {8U, 16U}) {
        for (const auto& [colourType, channels] :
             std::array<std::pair<unsigned, unsigned>, 4>{{{0, 1}, {4, 2}, {2, 3}, {6, 4}}}) {
            const auto name = "png-" + std::to_string(depth) + "-" + std::to_string(colourType);
            const auto path = scratch.file(name + ".png");
            writePng(path, 29, 13, depth, colourType, channels);
            expectParity(check, path, render::NativeSampleLayout::Rgba16, name);
        }
    }
    {
        const auto path = scratch.file("generated.jpg");
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()),
                   static_cast<std::streamsize>(kGeneratedJpeg.size()));
        file.close();
        expectParity(check, path, render::NativeSampleLayout::Rgba16, "jpeg");
    }
    const std::array<std::pair<const char*, ExrSpec>, 7> halfCases{{
        {"exr-rgba-premultiplied", ExrSpec{}},
        {"exr-rgba-straight", ExrSpec{.association = "straight"}},
        {"exr-rgba-unlabelled", ExrSpec{.association = nullptr}},
        {"exr-rgb", ExrSpec{.alpha = false}},
        {"exr-ya", ExrSpec{.luminance = true}},
        {"exr-tiled", ExrSpec{.tiled = true}},
        {"exr-repairs", ExrSpec{.repairs = true}},
    }};
    for (const auto& [name, spec] : halfCases) {
        const auto path = scratch.file(std::string(name) + ".exr");
        writeExr(path, spec);
        expectParity(check, path, render::NativeSampleLayout::RgbaHalf, name);
    }
    {
        const auto path = scratch.file("exr-repairs-straight.exr");
        writeExr(path, ExrSpec{.association = "straight", .repairs = true});
        expectParity(check, path, render::NativeSampleLayout::RgbaHalf, "exr-repairs-straight");
    }
    // 8 PNG kinds, one JPEG and 8 half OpenEXR kinds, each under three associations, raw and
    // stored.
    check.expect(comparedVariants == (8 + 1 + 8) * 6, "every native-sample variant was compared");
    // No native form: a FLOAT layer and any converted interpretation decode to float instead.
    {
        const auto path = scratch.file("exr-float.exr");
        writeExr(path, ExrSpec{.type = Imf::FLOAT});
        const auto revision = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes);
        media::ImageInterpretation raw;
        raw.colorSpace = media::ImageColorSpace::Raw;
        check.expect(revision.value.has_value(), "capture the FLOAT OpenEXR layer");
        if (revision.value) {
            const auto refused = media::decodeImageSamples(**revision.value, raw);
            check.expect(!refused.value &&
                             refused.code == media::ImageDiagnosticCode::NativeSamplesUnavailable,
                         "a FLOAT OpenEXR layer has no native samples");
        }
    }
    {
        const auto path = scratch.file("png-16-6.png");
        const auto revision = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes);
        media::ImageInterpretation converted;
        converted.colorSpace = media::ImageColorSpace::Srgb;
        const auto refused = media::decodeImageSamples(**revision.value, converted);
        check.expect(!refused.value &&
                         refused.code == media::ImageDiagnosticCode::NativeSamplesUnavailable,
                     "a converting interpretation has no native samples");
        media::ImageInterpretation raw;
        raw.colorSpace = media::ImageColorSpace::Raw;
        const auto cancelled = media::decodeImageSamples(**revision.value, raw, [] { return true; });
        check.expect(!cancelled.value && cancelled.cancelled, "a cancelled decode publishes nothing");
        const auto tight = media::decodeImageSamples(**revision.value, raw, {},
                                                     revision.value->get()->retainedBytes() + 64);
        check.expect(!tight.value && !tight.cancelled, "the host byte budget bounds the samples");
    }
    return check.failures() == 0 ? 0 : 1;
}
