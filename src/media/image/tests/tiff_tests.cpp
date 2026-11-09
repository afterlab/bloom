// TIFF stills decode natively into the same 16-bit RGBA samples as PNG and JPEG. The fixtures are
// written by an independent encoder in this file (every compression, predictor, byte order, bit
// depth, channel layout and strip or tile organization the decoder accepts) and compared sample
// for sample through the public revision, path and channel-set entry points.

#include "png_test_support.hpp"

#include <bloom/media/image.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <zlib.h>

namespace {
using namespace bloom_output_png_test_support;
namespace media = bloom::media;

using Bytes = std::vector<unsigned char>;

enum class Compress { None, PackBits, Deflate, Lzw };

struct Spec {
    std::uint32_t width = 5;
    std::uint32_t height = 7;
    unsigned depth = 16;        // 8 or 16
    unsigned samples = 4;       // samples per pixel
    unsigned photometric = 2;   // 0/1 grey, 2 RGB
    bool little = true;
    Compress compress = Compress::None;
    bool predictor = false;
    std::uint32_t rowsPerStrip = 3;
    std::uint32_t tile = 0;     // 0: strips, else square tile size
    int extra = 2;              // ExtraSamples value, -1: omit the tag
    std::uint32_t magic = 42;
    unsigned compressionTag = 0; // overrides when nonzero
    unsigned sampleFormat = 1;
};

std::uint16_t value(const Spec& spec, const std::uint32_t x, const std::uint32_t y,
                    const unsigned channel) {
    const auto mixed = (x * 2654435761U) ^ (y * 40503U) ^ (channel * 977U + 13U);
    const auto sample = static_cast<std::uint16_t>((mixed >> 3U) & 0xffffU);
    return spec.depth == 8 ? static_cast<std::uint16_t>((sample & 0xffU) * 257U) : sample;
}

void put(Bytes& out, const std::uint64_t number, const unsigned size, const bool little) {
    for (unsigned index = 0; index < size; ++index)
        out.push_back(static_cast<unsigned char>(
            (number >> (little ? 8U * index : 8U * (size - 1U - index))) & 0xffU));
}

Bytes lzwEncode(const Bytes& input) {
    Bytes out;
    std::uint64_t buffer = 0;
    unsigned buffered = 0;
    unsigned bits = 9;
    const auto emit = [&](const unsigned code) {
        buffer = (buffer << bits) | code;
        buffered += bits;
        while (buffered >= 8) {
            out.push_back(static_cast<unsigned char>((buffer >> (buffered - 8U)) & 0xffU));
            buffered -= 8;
        }
    };
    std::map<std::pair<unsigned, unsigned char>, unsigned> table;
    unsigned next = 258;
    emit(256);
    if (input.empty()) {
        emit(257);
    } else {
        unsigned current = input[0];
        for (std::size_t index = 1; index < input.size(); ++index) {
            const auto byte = input[index];
            const auto found = table.find({current, byte});
            if (found != table.end()) {
                current = found->second;
                continue;
            }
            emit(current);
            table[{current, byte}] = next++;
            // Early change: the width grows one code before the table needs it.
            if (next + 1U > (1U << bits) && bits < 12)
                ++bits;
            if (next >= 4094) {
                emit(256);
                table.clear();
                next = 258;
                bits = 9;
            }
            current = byte;
        }
        emit(current);
        // The decoder adds an entry after this code too, which can widen its next read.
        ++next;
        if (next + 1U > (1U << bits) && bits < 12)
            ++bits;
        emit(257);
    }
    if (buffered > 0)
        out.push_back(static_cast<unsigned char>((buffer << (8U - buffered)) & 0xffU));
    return out;
}

Bytes packBitsEncode(const Bytes& input) {
    Bytes out;
    std::size_t index = 0;
    while (index < input.size()) {
        std::size_t run = 1;
        while (index + run < input.size() && input[index + run] == input[index] && run < 128)
            ++run;
        if (run >= 2) {
            out.push_back(static_cast<unsigned char>(257U - run));
            out.push_back(input[index]);
            index += run;
            continue;
        }
        std::size_t literal = 1;
        while (index + literal < input.size() && literal < 128 &&
               !(index + literal + 1 < input.size() &&
                 input[index + literal] == input[index + literal + 1]))
            ++literal;
        out.push_back(static_cast<unsigned char>(literal - 1U));
        out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(index),
                   input.begin() + static_cast<std::ptrdiff_t>(index + literal));
        index += literal;
    }
    return out;
}

Bytes compressUnit(const Spec& spec, const Bytes& raw) {
    switch (spec.compress) {
    case Compress::None:
        return raw;
    case Compress::PackBits:
        return packBitsEncode(raw);
    case Compress::Deflate: {
        auto size = compressBound(static_cast<uLong>(raw.size()));
        Bytes out(size);
        compress2(out.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6);
        out.resize(size);
        return out;
    }
    case Compress::Lzw:
        return lzwEncode(raw);
    }
    return raw;
}

// One strip or tile of raw rows (padding tiles with zeros past the image edge).
Bytes unitBytes(const Spec& spec, const std::uint32_t x0, const std::uint32_t y0,
                const std::uint32_t unitWidth, const std::uint32_t rows) {
    const unsigned bytes = spec.depth / 8U;
    Bytes raw;
    for (std::uint32_t y = 0; y < rows; ++y) {
        Bytes line;
        std::vector<std::uint16_t> previous(spec.samples, 0);
        for (std::uint32_t x = 0; x < unitWidth; ++x) {
            const bool inside = x0 + x < spec.width && y0 + y < spec.height;
            for (unsigned c = 0; c < spec.samples; ++c) {
                auto sample = inside ? value(spec, x0 + x, y0 + y, c) : std::uint16_t{0};
                if (spec.depth == 8)
                    sample = static_cast<std::uint16_t>(sample / 257U);
                auto stored = sample;
                if (spec.predictor) {
                    stored = static_cast<std::uint16_t>(sample - previous[c]);
                    if (spec.depth == 8)
                        stored &= 0xffU;
                    previous[c] = sample;
                }
                put(line, stored, bytes, spec.little);
            }
        }
        raw.insert(raw.end(), line.begin(), line.end());
    }
    return raw;
}

Bytes makeTiff(const Spec& spec) {
    std::vector<Bytes> units;
    if (spec.tile == 0) {
        for (std::uint32_t y = 0; y < spec.height; y += spec.rowsPerStrip)
            units.push_back(compressUnit(
                spec, unitBytes(spec, 0, y, spec.width,
                                std::min(spec.rowsPerStrip, spec.height - y))));
    } else {
        for (std::uint32_t y = 0; y < spec.height; y += spec.tile)
            for (std::uint32_t x = 0; x < spec.width; x += spec.tile)
                units.push_back(
                    compressUnit(spec, unitBytes(spec, x, y, spec.tile, spec.tile)));
    }
    Bytes file;
    file.push_back(spec.little ? 'I' : 'M');
    file.push_back(spec.little ? 'I' : 'M');
    put(file, spec.magic, 2, spec.little);
    put(file, 8, 4, spec.little);
    struct Entry {
        std::uint16_t tag, type;
        std::uint32_t count;
        std::vector<std::uint32_t> items;
    };
    std::vector<Entry> entries;
    const auto compression = spec.compressionTag != 0
                                 ? spec.compressionTag
                                 : (spec.compress == Compress::None       ? 1U
                                    : spec.compress == Compress::PackBits ? 32773U
                                    : spec.compress == Compress::Deflate  ? 8U
                                                                          : 5U);
    entries.push_back({256, 4, 1, {spec.width}});
    entries.push_back({257, 4, 1, {spec.height}});
    entries.push_back({258, 3, spec.samples, std::vector<std::uint32_t>(spec.samples, spec.depth)});
    entries.push_back({259, 3, 1, {compression}});
    entries.push_back({262, 3, 1, {spec.photometric}});
    entries.push_back({277, 3, 1, {spec.samples}});
    if (spec.predictor)
        entries.push_back({317, 3, 1, {2}});
    const unsigned colour = spec.photometric == 2 ? 3U : 1U;
    if (spec.samples > colour && spec.extra >= 0)
        entries.push_back({338, 3, 1, {static_cast<std::uint32_t>(spec.extra)}});
    if (spec.sampleFormat != 1)
        entries.push_back({339, 3, spec.samples,
                           std::vector<std::uint32_t>(spec.samples, spec.sampleFormat)});
    const auto offsetsTag = spec.tile ? 324 : 273;
    const auto countsTag = spec.tile ? 325 : 279;
    if (spec.tile) {
        entries.push_back({322, 4, 1, {spec.tile}});
        entries.push_back({323, 4, 1, {spec.tile}});
    } else {
        entries.push_back({278, 4, 1, {spec.rowsPerStrip}});
    }
    entries.push_back({static_cast<std::uint16_t>(offsetsTag), 4,
                       static_cast<std::uint32_t>(units.size()),
                       std::vector<std::uint32_t>(units.size(), 0)});
    entries.push_back({static_cast<std::uint16_t>(countsTag), 4,
                       static_cast<std::uint32_t>(units.size()),
                       std::vector<std::uint32_t>(units.size(), 0)});
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
    // Layout: header(8) | IFD | out-of-line values | unit data.
    const auto ifdSize = 2U + static_cast<std::uint32_t>(entries.size()) * 12U + 4U;
    std::uint32_t cursor = 8U + ifdSize;
    std::vector<std::uint32_t> valueOffset(entries.size(), 0);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto size = entries[index].type == 3 ? 2U : 4U;
        if (size * entries[index].count > 4) {
            valueOffset[index] = cursor;
            cursor += size * entries[index].count;
        }
    }
    std::vector<std::uint32_t> offsets;
    for (const auto& unit : units) {
        offsets.push_back(cursor);
        cursor += static_cast<std::uint32_t>(unit.size());
    }
    for (auto& entry : entries) {
        if (entry.tag == offsetsTag)
            entry.items = offsets;
        if (entry.tag == countsTag)
            for (std::size_t index = 0; index < units.size(); ++index)
                entry.items[index] = static_cast<std::uint32_t>(units[index].size());
    }
    put(file, entries.size(), 2, spec.little);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        put(file, entry.tag, 2, spec.little);
        put(file, entry.type, 2, spec.little);
        put(file, entry.count, 4, spec.little);
        if (valueOffset[index] != 0) {
            put(file, valueOffset[index], 4, spec.little);
        } else {
            Bytes inline_;
            for (const auto item : entry.items)
                put(inline_, item, entry.type == 3 ? 2 : 4, spec.little);
            inline_.resize(4, 0);
            file.insert(file.end(), inline_.begin(), inline_.end());
        }
    }
    put(file, 0, 4, spec.little);
    for (std::size_t index = 0; index < entries.size(); ++index)
        if (valueOffset[index] != 0)
            for (const auto item : entries[index].items)
                put(file, item, entries[index].type == 3 ? 2 : 4, spec.little);
    for (const auto& unit : units)
        file.insert(file.end(), unit.begin(), unit.end());
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
    return std::to_string(spec.depth) + "-bit x" + std::to_string(spec.samples) +
           (spec.photometric == 2 ? " rgb" : " grey") + (spec.little ? " le" : " be") +
           " comp" + std::to_string(static_cast<int>(spec.compress)) +
           (spec.predictor ? " pred" : "") + (spec.tile ? " tiled" : " strips");
}

// Compares every decoded channel sample with the independent expectation.
bool matches(const Spec& spec, const media::ImageSourceRevision& revision) {
    const auto channels = media::decodeImageChannels(revision, {});
    if (!channels.value)
        return false;
    const bool alpha = spec.samples > (spec.photometric == 2 ? 3U : 1U);
    const auto& image = **channels.value;
    for (std::uint32_t y = 0; y < spec.height; ++y)
        for (std::uint32_t x = 0; x < spec.width; ++x) {
            const auto at = static_cast<std::size_t>(y) * spec.width + x;
            for (unsigned c = 0; c < 3; ++c) {
                const auto source = spec.photometric == 2 ? c : 0U;
                auto sample = value(spec, x, y, source);
                if (spec.photometric == 0)
                    sample = static_cast<std::uint16_t>(65535U - sample);
                const std::uint32_t a = alpha ? value(spec, x, y, spec.photometric == 2 ? 3U : 1U) : 65535U;
                if (alpha && spec.extra == 1) {
                    sample = a == 0 ? 0
                                    : static_cast<std::uint16_t>(std::min<std::uint32_t>(
                                          65535U, (sample * 65535U + a / 2U) / a));
                }
                if (image.plane(c)[at] != static_cast<float>(sample) / 65535.0F)
                    return false;
            }
            if (alpha) {
                const auto a = value(spec, x, y, spec.photometric == 2 ? 3U : 1U);
                if (image.plane(3)[at] != static_cast<float>(a) / 65535.0F)
                    return false;
            }
        }
    return true;
}
} // namespace

int main() {
    ScratchDirectory scratch("media-tiff");
    Expectations check;
    const auto path = scratch.file("fixture.tif");

    // Every supported layout round-trips exactly through a frozen revision and the path decode.
    std::vector<Spec> specs;
    for (const bool little : {true, false})
        for (const unsigned depth : {8U, 16U})
            for (const auto compress :
                 {Compress::None, Compress::PackBits, Compress::Deflate, Compress::Lzw})
                for (const bool predictor : {false, true}) {
                    Spec spec;
                    spec.little = little;
                    spec.depth = depth;
                    spec.compress = compress;
                    spec.predictor = predictor;
                    specs.push_back(spec);
                }
    for (const bool tiled : {false, true})
        for (const auto layout : {std::pair{3U, 2U}, std::pair{4U, 2U}, std::pair{1U, 1U},
                                  std::pair{2U, 1U}, std::pair{5U, 2U}}) {
            Spec spec;
            spec.samples = layout.first;
            spec.photometric = layout.second;
            spec.compress = Compress::Deflate;
            spec.tile = tiled ? 4 : 0;
            spec.width = 9;
            spec.height = 11;
            specs.push_back(spec);
        }
    {
        Spec spec;
        spec.photometric = 0;
        spec.samples = 1;
        specs.push_back(spec); // WhiteIsZero grey
        spec.photometric = 2;
        spec.samples = 4;
        spec.extra = 1; // associated alpha is returned as straight samples
        specs.push_back(spec);
        spec.extra = -1; // no ExtraSamples tag: the fourth sample is still alpha
        specs.push_back(spec);
        spec = Spec{};
        spec.compress = Compress::Lzw;
        spec.width = 300; // long enough to fill the code table and restart it
        spec.height = 40;
        spec.rowsPerStrip = 40;
        specs.push_back(spec);
    }
    for (const auto& spec : specs) {
        write(path, makeTiff(spec));
        const auto probe = media::probeImage(path);
        check.expect(probe.value && probe.value->format == media::ImageFormat::Tiff &&
                         probe.value->width == spec.width && probe.value->height == spec.height &&
                         probe.value->bitDepth == spec.depth &&
                         probe.value->colorSpace == media::ImageColorSpace::Srgb,
                     "probe reports a TIFF still: " + describe(spec));
        const auto captured = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes);
        check.expect(captured.value.has_value(), "TIFF revision captured: " + describe(spec));
        if (!captured.value)
            continue;
        check.expect(matches(spec, **captured.value),
                     "TIFF samples are exact through the revision: " + describe(spec));
        const auto decoded = media::decodeImage(path, rawInterpretation());
        const auto fromRevision =
            media::decodeImage(**captured.value, rawInterpretation());
        bool same = decoded.value && fromRevision.value;
        if (same) {
            const auto a = (*decoded.value)->pixels();
            const auto b = (*fromRevision.value)->pixels();
            same = a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
        }
        check.expect(same, "TIFF path and revision decodes agree: " + describe(spec));
        const auto native = media::decodeImageSamples(
            **captured.value, rawInterpretation());
        check.expect(native.value.has_value(), "TIFF has native 16-bit samples: " + describe(spec));
    }

    // A converted decode reaches the working space like PNG does.
    {
        Spec spec;
        write(path, makeTiff(spec));
        const auto decoded = media::decodeImage(path);
        check.expect(decoded.value.has_value(), "TIFF decodes through the automatic colour rule");
    }

    // Refusals are plain language and never crash.
    struct Refusal {
        const char* label;
        Spec spec;
        const char* expect;
    };
    std::vector<Refusal> refusals;
    {
        Spec s;
        s.magic = 43;
        refusals.push_back({"bigtiff", s, "64-bit"});
        s = {};
        s.photometric = 5;
        s.samples = 4;
        refusals.push_back({"cmyk", s, "grey and RGB"});
        s = {};
        s.depth = 32;
        refusals.push_back({"32-bit", s, "8-bit and 16-bit"});
        s = {};
        s.sampleFormat = 3;
        refusals.push_back({"float", s, "Floating-point"});
        s = {};
        s.compressionTag = 7;
        refusals.push_back({"jpeg compression", s, "compression type"});
    }
    for (const auto& refusal : refusals) {
        write(path, makeTiff(refusal.spec));
        const auto probe = media::probeImage(path);
        const auto decoded = media::decodeImage(path);
        check.expect(!probe && !decoded &&
                         probe.diagnostic.find(refusal.expect) != std::string::npos &&
                         decoded.diagnostic.find(refusal.expect) != std::string::npos &&
                         probe.diagnostic.find("provider") == std::string::npos,
                     std::string("plain TIFF refusal: ") + refusal.label + " -> " +
                         probe.diagnostic);
    }

    // Malformed and truncated files are refused without reading outside the file.
    {
        Spec spec;
        spec.compress = Compress::Deflate;
        const auto good = makeTiff(spec);
        for (const auto cut : {std::size_t{3}, std::size_t{9}, std::size_t{40}, good.size() / 2,
                               good.size() - 1}) {
            write(path, Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut)));
            const auto decoded = media::decodeImage(path);
            check.expect(!decoded && !decoded.diagnostic.empty() &&
                             decoded.diagnostic.find("provider") == std::string::npos,
                         "truncated TIFF refused at " + std::to_string(cut));
        }
        // A directory pointing past the end of the file.
        auto bad = good;
        for (std::size_t at = 4; at < 8; ++at)
            bad[at] = 0xff;
        write(path, bad);
        check.expect(!media::decodeImage(path), "TIFF with a wild directory offset refused");
        // Strip counts larger than the file.
        for (std::uint32_t seed = 1; seed < 400; ++seed) {
            auto fuzz = good;
            fuzz[8U + (seed * 7919U) % (fuzz.size() - 8U)] ^= static_cast<unsigned char>(seed);
            write(path, fuzz);
            (void)media::decodeImage(path); // must neither crash nor hang
        }
        // Dimensions larger than the pixel budget.
        Spec tiny;
        tiny.width = 4;
        tiny.height = 4;
        tiny.rowsPerStrip = 4;
        write(path, makeTiff(tiny));
        const auto small = media::decodeImage(path, {}, {}, {}, {}, 4 * 4 * 24 - 1);
        check.expect(!small && small.code == media::ImageDiagnosticCode::PixelBudgetExceeded,
                     "TIFF over the pixel budget refused before decode");
    }

    // Numbered TIFF files are a sequence like any other still format.
    for (const auto* name : {"plate.0001.tif", "plate.0002.tif", "plate.0003.tiff"}) {
        Spec spec;
        spec.width = 3;
        spec.height = 2;
        write(scratch.file(name), makeTiff(spec));
    }
    {
        const auto sequence = media::scanSequence(scratch.file("plate.0002.tif"));
        check.expect(sequence.value && sequence.value->members.size() == 2,
                     "numbered TIFF files form a sequence per extension");
    }

    // A file whose compressed data ends early is refused, not padded.
    {
        Spec spec;
        spec.compress = Compress::Lzw;
        auto bytes = makeTiff(spec);
        bytes.resize(bytes.size() - 6);
        write(path, bytes);
        check.expect(!media::decodeImage(path), "short compressed data refused");
    }
    return check.failures() == 0 ? 0 : 1;
}
