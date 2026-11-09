// DPX stills decode natively into the same 16-bit RGBA samples as PNG, JPEG and TIFF. The fixtures
// are written by an independent encoder in this file (every sample size and packing, byte order,
// channel layout and element arrangement the decoder accepts) and compared sample for sample
// through the public revision, path and channel-set entry points.

#include "png_test_support.hpp"

#include <bloom/media/image.hpp>
#include <bloom/render/native_samples.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <utility>
#include <string>
#include <vector>

namespace {
using namespace bloom_output_png_test_support;
namespace media = bloom::media;
namespace render = bloom::render;

using Bytes = std::vector<unsigned char>;

struct Element {
    unsigned descriptor = 50; // 1 R, 2 G, 3 B, 4 A, 6 luma, 50 RGB, 51 RGBA, 52 ABGR
    unsigned depth = 10;
    unsigned packing = 1;     // 0 packed, 1 method A, 2 method B
    unsigned encoding = 0;
    unsigned sign = 0;
    std::uint32_t eolPadding = 0;
};

struct Spec {
    std::uint32_t width = 7;
    std::uint32_t height = 5;
    bool big = false;
    std::vector<Element> elements{Element{}};
    unsigned transfer = 3;
    unsigned colorimetric = 1;
    std::uint32_t orientation = 0;
    float filmRate = 24.0F;
    float tvRate = 0.0F;
    std::uint32_t timecode = 0x01020304U; // 01:02:03:04
    bool shortHeader = false;             // no film or television header
};

unsigned channelCount(const Element& element) {
    switch (element.descriptor) {
    case 50: return 3;
    case 51:
    case 52: return 4;
    default: return 1;
    }
}

// The destination channel (0 R, 1 G, 2 B, 3 A, 4 luma) of an element's `index`th sample.
unsigned channelOf(const Element& element, const unsigned index) {
    switch (element.descriptor) {
    case 1: return 0;
    case 2: return 1;
    case 3: return 2;
    case 4: return 3;
    case 6: return 4;
    case 52: return 3 - index;
    default: return index;
    }
}

std::uint32_t codeAt(const Element& element, const std::uint32_t x, const std::uint32_t y,
                     const unsigned channel) {
    const auto mixed = (x * 2654435761U) ^ (y * 40503U) ^ (channel * 977U + 29U);
    return (mixed >> 5U) & ((1U << element.depth) - 1U);
}

std::uint16_t scaled(const std::uint32_t code, const unsigned depth) {
    const std::uint32_t maximum = (1U << depth) - 1U;
    return static_cast<std::uint16_t>((code * 65535U + maximum / 2U) / maximum);
}

void put(Bytes& out, const std::uint64_t number, const unsigned size, const bool big) {
    for (unsigned index = 0; index < size; ++index)
        out.push_back(static_cast<unsigned char>(
            (number >> (big ? 8U * (size - 1U - index) : 8U * index)) & 0xffU));
}

void putAt(Bytes& out, const std::size_t at, const std::uint64_t number, const unsigned size,
           const bool big) {
    Bytes bytes;
    put(bytes, number, size, big);
    std::copy(bytes.begin(), bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
}

Bytes elementData(const Spec& spec, const Element& element) {
    const auto count = channelCount(element);
    const std::size_t samples = static_cast<std::size_t>(spec.width) * count;
    Bytes data;
    for (std::uint32_t y = 0; y < spec.height; ++y) {
        Bytes line;
        if (element.depth == 8) {
            for (std::size_t i = 0; i < samples; ++i)
                line.push_back(static_cast<unsigned char>(
                    codeAt(element, static_cast<std::uint32_t>(i / count), y,
                           channelOf(element, static_cast<unsigned>(i % count)))));
        } else if (element.depth == 10) {
            for (std::size_t word = 0; word < (samples + 2) / 3; ++word) {
                std::uint32_t value = 0;
                for (unsigned slot = 0; slot < 3; ++slot) {
                    const auto i = word * 3 + slot;
                    if (i >= samples)
                        break;
                    const auto code = codeAt(element, static_cast<std::uint32_t>(i / count), y,
                                             channelOf(element, static_cast<unsigned>(i % count)));
                    const unsigned shift =
                        element.packing == 1 ? 22U - 10U * slot : 20U - 10U * slot;
                    value |= code << shift;
                }
                put(line, value, 4, spec.big);
            }
        } else {
            for (std::size_t i = 0; i < samples; ++i) {
                auto code = codeAt(element, static_cast<std::uint32_t>(i / count), y,
                                   channelOf(element, static_cast<unsigned>(i % count)));
                if (element.depth == 12 && element.packing == 1)
                    code <<= 4U;
                put(line, code, 2, spec.big);
            }
        }
        while (line.size() % 4 != 0)
            line.push_back(0);
        line.insert(line.end(), element.eolPadding, 0);
        data.insert(data.end(), line.begin(), line.end());
    }
    return data;
}

Bytes makeDpx(const Spec& spec) {
    Bytes file(spec.shortHeader ? 1664U : 2048U, 0);
    const char* magic = spec.big ? "SDPX" : "XPDS";
    std::copy_n(magic, 4, file.begin());
    std::memcpy(file.data() + 8, "V2.0", 4);
    putAt(file, 24, 1664, 4, spec.big);
    putAt(file, 768, spec.orientation, 2, spec.big);
    putAt(file, 770, spec.elements.size(), 2, spec.big);
    putAt(file, 772, spec.width, 4, spec.big);
    putAt(file, 776, spec.height, 4, spec.big);
    std::uint64_t offset = file.size();
    std::vector<Bytes> data;
    for (std::size_t index = 0; index < spec.elements.size(); ++index) {
        const auto& element = spec.elements[index];
        data.push_back(elementData(spec, element));
        const std::size_t at = 780 + index * 72;
        putAt(file, at + 0, element.sign, 4, spec.big);
        putAt(file, at + 4, 0, 4, spec.big);
        putAt(file, at + 12, (1U << element.depth) - 1U, 4, spec.big);
        file[at + 20] = static_cast<unsigned char>(element.descriptor);
        file[at + 21] = static_cast<unsigned char>(spec.transfer);
        file[at + 22] = static_cast<unsigned char>(spec.colorimetric);
        file[at + 23] = static_cast<unsigned char>(element.depth);
        putAt(file, at + 24, element.packing, 2, spec.big);
        putAt(file, at + 26, element.encoding, 2, spec.big);
        putAt(file, at + 28, offset, 4, spec.big);
        putAt(file, at + 32, element.eolPadding, 4, spec.big);
        offset += data.back().size();
    }
    putAt(file, 4, file.size(), 4, spec.big); // offset of the first element's data
    if (!spec.shortHeader) {
        const auto real = [&](const std::size_t at, const float value) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, 4);
            putAt(file, at, bits, 4, spec.big);
        };
        real(1724, spec.filmRate);
        putAt(file, 1920, spec.timecode, 4, spec.big);
        real(1940, spec.tvRate);
    }
    for (const auto& element : data)
        file.insert(file.end(), element.begin(), element.end());
    putAt(file, 16, file.size(), 4, spec.big);
    return file;
}

media::ImageInterpretation rawInterpretation() {
    media::ImageInterpretation interpretation;
    interpretation.colorSpace = media::ImageColorSpace::Raw;
    return interpretation;
}

void write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::string describe(const Spec& spec) {
    std::string text = spec.big ? "big" : "little";
    for (const auto& element : spec.elements)
        text += " d" + std::to_string(element.descriptor) + "/" + std::to_string(element.depth) +
                "/p" + std::to_string(element.packing) + "/e" +
                std::to_string(element.eolPadding);
    return text + " " + std::to_string(spec.width) + "x" + std::to_string(spec.height);
}

// The expected R, G, B, A of a pixel (all four, A defaults to opaque).
std::array<std::uint16_t, 4> expected(const Spec& spec, const std::uint32_t x,
                                      const std::uint32_t y) {
    std::array<std::uint16_t, 4> rgba{0, 0, 0, 65535};
    for (const auto& element : spec.elements)
        for (unsigned index = 0; index < channelCount(element); ++index) {
            const auto channel = channelOf(element, index);
            const auto value = scaled(codeAt(element, x, y, channel), element.depth);
            if (channel == 4)
                rgba[0] = rgba[1] = rgba[2] = value;
            else
                rgba[channel] = value;
        }
    return rgba;
}

bool matches(const Spec& spec, const media::ImageSourceRevision& revision) {
    const auto channels = media::decodeImageChannels(revision, {});
    if (!channels.value)
        return false;
    const auto& image = **channels.value;
    const bool alpha = std::ranges::any_of(spec.elements, [](const Element& element) {
        return element.descriptor == 4 || element.descriptor == 51 || element.descriptor == 52;
    });
    if (image.channels().size() != (alpha ? 4U : 3U))
        return false;
    for (std::uint32_t y = 0; y < spec.height; ++y)
        for (std::uint32_t x = 0; x < spec.width; ++x) {
            const auto want = expected(spec, x, y);
            const auto at = static_cast<std::size_t>(y) * spec.width + x;
            for (unsigned c = 0; c < image.channels().size(); ++c)
                if (image.plane(c)[at] != static_cast<float>(want[c]) / 65535.0F)
                    return false;
        }
    return true;
}
} // namespace

int main() {
    ScratchDirectory scratch("media-dpx");
    Expectations check;
    const auto path = scratch.file("fixture.dpx");

    std::vector<Spec> specs;
    // Every sample size and packing in both byte orders, with widths that exercise line padding.
    for (const bool big : {false, true})
        for (const auto layout : {std::pair{8U, 1U}, std::pair{10U, 1U}, std::pair{10U, 2U},
                                  std::pair{12U, 1U}, std::pair{12U, 2U}, std::pair{16U, 0U}})
            for (const unsigned descriptor : {50U, 51U, 52U, 6U})
                for (const std::uint32_t width : {7U, 8U, 9U}) {
                    Spec spec;
                    spec.big = big;
                    spec.width = width;
                    spec.elements = {Element{descriptor, layout.first, layout.second}};
                    specs.push_back(spec);
                }
    {
        Spec spec; // end-of-line padding
        spec.elements = {Element{50, 10, 1, 0, 0, 8}};
        specs.push_back(spec);
        spec.elements = {Element{51, 16, 0, 0, 0, 6}};
        specs.push_back(spec);
        spec = Spec{}; // separate elements, in any order, and RGB with a separate alpha element
        spec.elements = {Element{3, 10, 1}, Element{1, 10, 2}, Element{2, 12, 1}};
        specs.push_back(spec);
        spec.elements = {Element{1, 16, 0}, Element{2, 16, 0}, Element{3, 16, 0}, Element{4, 8, 0}};
        specs.push_back(spec);
        spec.elements = {Element{50, 10, 1}, Element{4, 10, 1}};
        specs.push_back(spec);
        spec.elements = {Element{6, 16, 0}, Element{4, 16, 0}};
        specs.push_back(spec);
        spec = Spec{};
        spec.shortHeader = true;
        specs.push_back(spec);
        spec = Spec{};
        spec.orientation = 0xffff; // undefined orientation is read as the default
        specs.push_back(spec);
    }
    for (const auto& spec : specs) {
        write(path, makeDpx(spec));
        const auto probe = media::probeImage(path);
        check.expect(probe.value && probe.value->format == media::ImageFormat::Dpx &&
                         probe.value->width == spec.width && probe.value->height == spec.height &&
                         probe.value->dpx.has_value() &&
                         probe.value->bitDepth == probe.value->dpx->bitDepth &&
                         probe.value->colorSpace == media::ImageColorSpace::Auto,
                     "probe reports a DPX still: " + describe(spec));
        const auto captured = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes);
        check.expect(captured.value.has_value(), "DPX revision captured: " + describe(spec));
        if (!captured.value)
            continue;
        check.expect(matches(spec, **captured.value),
                     "DPX samples are exact through the revision: " + describe(spec));
        const auto decoded = media::decodeImage(path, rawInterpretation());
        const auto fromRevision = media::decodeImage(**captured.value, rawInterpretation());
        bool same = decoded.value && fromRevision.value;
        if (same) {
            const auto a = (*decoded.value)->pixels();
            const auto b = (*fromRevision.value)->pixels();
            same = a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
        }
        check.expect(same, "DPX path and revision decodes agree: " + describe(spec));
        const auto native = media::decodeImageSamples(**captured.value, rawInterpretation());
        check.expect(native.value.has_value(), "DPX has native 16-bit samples: " + describe(spec));
        if (native.value && decoded.value) {
            // The device route's reference conversion equals the float decode exactly.
            const auto converted =
                render::convertNativeSamples(**native.value, media::kMaxImageStorageBytes);
            bool equal = converted.hasValue();
            if (equal) {
                const auto a = (*decoded.value)->pixels();
                const auto b = converted.value()->pixels();
                equal = a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
            }
            check.expect(equal, "DPX native samples convert to the float decode: " +
                                    describe(spec));
        }
    }

    // Header metadata that drives interpretation.
    for (const bool big : {false, true}) {
        Spec spec;
        spec.big = big;
        spec.transfer = 13;
        spec.colorimetric = 13;
        spec.filmRate = 23.976F;
        write(path, makeDpx(spec));
        const auto probe = media::probeImage(path);
        check.expect(probe.value && probe.value->dpx && probe.value->dpx->transfer == 13 &&
                         probe.value->dpx->colorimetric == 13 && probe.value->dpx->bitDepth == 10 &&
                         probe.value->dpx->packing == 1 && probe.value->dpx->bigEndian == big &&
                         probe.value->dpx->dataOffset == 2048 &&
                         std::abs(probe.value->dpx->frameRate - 23.976) < 1e-3 &&
                         probe.value->dpx->timecode == "01:02:03:04",
                     "DPX header metadata is read");
    }
    {
        Spec spec;
        spec.filmRate = 0.0F;
        spec.tvRate = 25.0F;
        spec.timecode = 0xffffffffU;
        write(path, makeDpx(spec));
        const auto probe = media::probeImage(path);
        check.expect(probe.value && probe.value->dpx && probe.value->dpx->frameRate == 25.0 &&
                         probe.value->dpx->timecode.empty(),
                     "the television frame rate stands in and an unset time code stays empty");
        spec.shortHeader = true;
        write(path, makeDpx(spec));
        const auto bare = media::probeImage(path);
        check.expect(bare.value && bare.value->dpx && bare.value->dpx->frameRate == 0.0 &&
                         bare.value->dpx->timecode.empty(),
                     "a header without film or television blocks records no rate or time code");
    }

    // A converted decode reaches the working space through the config like any still.
    {
        Spec spec;
        write(path, makeDpx(spec));
        check.expect(media::decodeImage(path).value.has_value(),
                     "DPX decodes through the automatic colour rule");
    }

    // Numbered DPX files form a sequence.
    for (const auto* name : {"plate.0001.dpx", "plate.0002.dpx", "plate.0004.dpx"}) {
        Spec spec;
        write(scratch.file(name), makeDpx(spec));
    }
    {
        const auto sequence = media::scanSequence(scratch.file("plate.0002.dpx"));
        check.expect(sequence.value && sequence.value->members.size() == 3 &&
                         sequence.value->hasGaps(),
                     "numbered DPX files form a sequence with a gap");
    }

    // Plain-language refusals.
    struct Refusal {
        const char* label;
        Spec spec;
        const char* expect;
    };
    std::vector<Refusal> refusals;
    {
        Spec s;
        s.elements = {Element{100, 10, 1}};
        refusals.push_back({"ycbcr", s, "element layout"});
        s = {};
        s.elements = {Element{50, 10, 1, 1}};
        refusals.push_back({"run-length", s, "Run-length"});
        s = {};
        s.elements = {Element{50, 10, 1, 0, 1}};
        refusals.push_back({"signed", s, "Signed"});
        s = {};
        s.elements = {Element{50, 10, 0}};
        refusals.push_back({"packed ten bit", s, "Packed 10-bit"});
        s = {};
        s.elements = {Element{50, 12, 0}};
        refusals.push_back({"packed twelve bit", s, "Packed 10-bit"});
        s = {};
        s.elements = {Element{50, 1, 1}};
        refusals.push_back({"one bit", s, "8-, 10-, 12- and 16-bit"});
        s = {};
        s.orientation = 3;
        refusals.push_back({"rotated", s, "Rotated"});
        s = {};
        s.elements = {Element{1, 16, 0}, Element{1, 16, 0}, Element{2, 16, 0}};
        refusals.push_back({"repeated channel", s, "more than one"});
        s = {};
        s.elements = {Element{1, 16, 0}, Element{2, 16, 0}};
        refusals.push_back({"incomplete colour", s, "complete color image"});
    }
    for (const auto& refusal : refusals) {
        write(path, makeDpx(refusal.spec));
        const auto probe = media::probeImage(path);
        const auto decoded = media::decodeImage(path);
        check.expect(!probe && !decoded &&
                         probe.diagnostic.find(refusal.expect) != std::string::npos &&
                         decoded.diagnostic.find(refusal.expect) != std::string::npos,
                     std::string("plain DPX refusal: ") + refusal.label + " -> " +
                         probe.diagnostic);
    }

    // Malformed and truncated files are refused without reading outside the file.
    {
        Spec spec;
        const auto good = makeDpx(spec);
        for (const auto cut : {std::size_t{3}, std::size_t{100}, std::size_t{800}, std::size_t{1700},
                               good.size() - 100, good.size() - 1}) {
            write(path, Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut)));
            const auto decoded = media::decodeImage(path);
            check.expect(!decoded && !decoded.diagnostic.empty(),
                         "truncated DPX refused at " + std::to_string(cut));
        }
        auto huge = good; // data offset past the end of the file
        putAt(huge, 4, 0xfffffff0U, 4, false);
        putAt(huge, 780 + 28, 0xfffffff0U, 4, false);
        write(path, huge);
        check.expect(!media::decodeImage(path), "DPX with a wild data offset refused");
        auto wide = good; // dimensions far larger than the data
        putAt(wide, 772, 1U << 24U, 4, false);
        write(path, wide);
        check.expect(!media::decodeImage(path), "DPX with oversized dimensions refused");
        for (std::uint32_t seed = 1; seed < 400; ++seed) {
            auto fuzz = good;
            fuzz[(seed * 7919U) % 1500U] ^= static_cast<unsigned char>(seed);
            write(path, fuzz);
            (void)media::decodeImage(path); // must neither crash nor hang
        }
        Spec tiny;
        tiny.width = 4;
        tiny.height = 4;
        write(path, makeDpx(tiny));
        const auto small = media::decodeImage(path, {}, {}, {}, {}, 4 * 4 * 24 - 1);
        check.expect(!small && small.code == media::ImageDiagnosticCode::PixelBudgetExceeded,
                     "DPX over the pixel budget refused before decode");
    }
    return check.failures() == 0 ? 0 : 1;
}
