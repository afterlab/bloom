#include "stb_image_adapter.hpp"
#include "dpx_decoder.hpp"
#include "tiff_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <zlib.h>

namespace {
// Thread-local so concurrent decodes with distinct request budgets cannot interfere. A default
// only covers header inspection; imageSamples() installs the caller's peak budget for its parse.
thread_local std::size_t allocationLimit = 268435456;
thread_local std::size_t allocatedBytes = 0;
struct alignas(std::max_align_t) Allocation {
    std::size_t size;
};
[[nodiscard]] bool exceedsLimit(const std::size_t size, const std::size_t liveBytes) {
    return allocationLimit <= liveBytes || size > allocationLimit - liveBytes;
}
[[nodiscard]] bool allocationSizeFits(const std::size_t size) {
    return size <= std::numeric_limits<std::size_t>::max() - sizeof(Allocation);
}
void* imageAllocate(std::size_t size) {
    if (!allocationSizeFits(size) || exceedsLimit(size, allocatedBytes))
        return nullptr;
    auto* block = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
    if (!block)
        return nullptr;
    block->size = size;
    allocatedBytes += size;
    return block + 1;
}
void imageFree(void* pointer) {
    if (!pointer)
        return;
    auto* block = static_cast<Allocation*>(pointer) - 1;
    allocatedBytes -= block->size;
    std::free(block);
}
void* imageReallocate(void* pointer, std::size_t size) {
    if (!pointer)
        return imageAllocate(size);
    auto* block = static_cast<Allocation*>(pointer) - 1;
    if (!allocationSizeFits(size) || exceedsLimit(size, allocatedBytes - block->size))
        return nullptr;
    const auto oldSize = block->size;
    auto* replacement = static_cast<Allocation*>(std::realloc(block, sizeof(Allocation) + size));
    if (!replacement)
        return nullptr;
    replacement->size = size;
    allocatedBytes = allocatedBytes - oldSize + size;
    return replacement + 1;
}
} // namespace

namespace bloom::media::detail {
ImageAllocationBudgetScope::ImageAllocationBudgetScope(const std::size_t limitBytes) noexcept
    : previous_(allocationLimit) {
    allocationLimit = limitBytes;
}
ImageAllocationBudgetScope::~ImageAllocationBudgetScope() noexcept { allocationLimit = previous_; }
} // namespace bloom::media::detail

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
// The upstream per-dimension ceiling, not a total-pixel cap: the codec's own integer overflow
// guards (and this adapter's explicit peak budget) bound the decoded samples.
#define STBI_MAX_DIMENSIONS (1 << 24)
#define STBI_MALLOC(size) imageAllocate(size)
#define STBI_REALLOC(pointer, size) imageReallocate(pointer, size)
#define STBI_FREE(pointer) imageFree(pointer)
#include "stb_image.h"

namespace {
int readStream(void* user, char* data, int size) {
    auto* stream = static_cast<std::ifstream*>(user);
    stream->read(data, size);
    return static_cast<int>(stream->gcount());
}
void skipStream(void* user, int count) {
    static_cast<std::ifstream*>(user)->seekg(count, std::ios::cur);
}
int streamEof(void* user) { return static_cast<std::ifstream*>(user)->eof() ? 1 : 0; }
const stbi_io_callbacks kFileCallbacks{readStream, skipStream, streamEof};
struct ByteStream {
    std::span<const std::byte> bytes;
    std::size_t position = 0;
};
int readBytes(void* user, char* data, const int size) {
    auto& stream = *static_cast<ByteStream*>(user);
    if (size <= 0)
        return 0;
    const auto count =
        std::min(static_cast<std::size_t>(size), stream.bytes.size() - stream.position);
    if (count == 0)
        return 0;
    std::memcpy(data, stream.bytes.data() + stream.position, count);
    stream.position += count;
    return static_cast<int>(count);
}
void skipBytes(void* user, const int count) {
    auto& stream = *static_cast<ByteStream*>(user);
    if (count < 0) {
        const auto distance = static_cast<std::uint64_t>(-static_cast<std::int64_t>(count));
        stream.position -= std::min<std::uint64_t>(stream.position, distance);
    } else {
        stream.position +=
            std::min(static_cast<std::size_t>(count), stream.bytes.size() - stream.position);
    }
}
int bytesEof(void* user) {
    const auto& stream = *static_cast<ByteStream*>(user);
    return stream.position == stream.bytes.size() ? 1 : 0;
}
const stbi_io_callbacks kByteCallbacks{readBytes, skipBytes, bytesEof};
} // namespace

namespace bloom::media::detail {
namespace {
// A TIFF or DPX is random access, so it is read whole, as its source revision is.
std::vector<std::byte> readWholeFile(std::ifstream& stream) {
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0)
        return {};
    stream.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(stream.gcount()) != bytes.size())
        return {};
    return bytes;
}
bool streamStartsWithWholeFileFormat(std::ifstream& stream) {
    std::array<std::byte, 4> head{};
    stream.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    const auto count = static_cast<std::size_t>(stream.gcount());
    stream.clear();
    stream.seekg(0);
    const auto view = std::span<const std::byte>(head.data(), count);
    return isTiffMagic(view) || isDpxMagic(view);
}
} // namespace
bool imageInfo(const std::filesystem::path& path, ImageInfo& info) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    if (streamStartsWithWholeFileFormat(stream)) {
        const auto bytes = readWholeFile(stream);
        return isDpxMagic(bytes) ? dpxInfo(bytes, info) : tiffInfo(bytes, info);
    }
    if (!stbi_info_from_callbacks(&kFileCallbacks, &stream, &info.width, &info.height,
                                  &info.channels))
        return false;
    stream.clear();
    stream.seekg(0);
    info.sixteenBit = stbi_is_16_bit_from_callbacks(&kFileCallbacks, &stream) != 0;
    return true;
}
bool imageInfo(const std::span<const std::byte> bytes, ImageInfo& info) {
    if (isTiffMagic(bytes))
        return tiffInfo(bytes, info);
    if (isDpxMagic(bytes))
        return dpxInfo(bytes, info);
    ByteStream stream{bytes};
    if (!stbi_info_from_callbacks(&kByteCallbacks, &stream, &info.width, &info.height,
                                  &info.channels))
        return false;
    stream.position = 0;
    info.sixteenBit = stbi_is_16_bit_from_callbacks(&kByteCallbacks, &stream) != 0;
    return true;
}
static std::vector<std::uint16_t> decodeSamples(const stbi_io_callbacks& callbacks, void* source,
                                                const ImageInfo& info,
                                                const std::size_t pixelBudget) {
    if (info.width <= 0 || info.height <= 0)
        return {};
    const auto pixels =
        static_cast<std::uint64_t>(info.width) * static_cast<std::uint64_t>(info.height);
    // Peak admission: final RGBA32F (16 bytes/px) plus this parse's RGBA16 staging (8 bytes/px)
    // must fit the caller's explicit budget, so the parser is never asked to over-allocate.
    constexpr std::uint64_t kPeakBytesPerPixel = 24U;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / kPeakBytesPerPixel ||
        pixels * kPeakBytesPerPixel > pixelBudget)
        return {};
    const ImageAllocationBudgetScope budgetScope(pixelBudget);
    int width = 0;
    int height = 0;
    int channels = 0;
    auto* decoded = stbi_load_16_from_callbacks(&callbacks, source, &width, &height, &channels, 4);
    if (!decoded)
        return {};
    if (width != info.width || height != info.height) {
        stbi_image_free(decoded);
        return {};
    }
    try {
        std::vector<std::uint16_t> result(decoded,
                                          decoded + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(decoded);
        return result;
    } catch (...) {
        stbi_image_free(decoded);
        throw;
    }
}
namespace {
[[nodiscard]] std::uint32_t pngWord(const std::byte* bytes) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[3]);
}
[[nodiscard]] inline std::uint8_t paeth(const int a, const int b, const int c) noexcept {
    // p - a, p - b and p - c of the PNG predictor p = a + b - c, with its tie order.
    const int pa = std::abs(b - c);
    const int pb = std::abs(a - c);
    const int pc = std::abs(a + b - 2 * c);
    return static_cast<std::uint8_t>(pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
}
// Reverses one PNG row filter in place. `previous` is the unfiltered row above, or a zero row for
// the first row. The pixel size is a template argument so the per-byte loops carry no bounds or
// first-row tests and the compiler can keep each channel's dependency chain in registers.
template <std::size_t PixelBytes>
[[nodiscard]] bool unfilterRow(const std::uint8_t filter, std::uint8_t* current,
                               const std::uint8_t* previous, const std::size_t rowBytes) noexcept {
    const std::size_t head = std::min(PixelBytes, rowBytes);
    switch (filter) {
    case 0:
        return true;
    case 1:
        for (std::size_t i = PixelBytes; i < rowBytes; ++i)
            current[i] = static_cast<std::uint8_t>(current[i] + current[i - PixelBytes]);
        return true;
    case 2:
        for (std::size_t i = 0; i < rowBytes; ++i)
            current[i] = static_cast<std::uint8_t>(current[i] + previous[i]);
        return true;
    case 3:
        for (std::size_t i = 0; i < head; ++i)
            current[i] = static_cast<std::uint8_t>(current[i] + (previous[i] >> 1U));
        for (std::size_t i = PixelBytes; i < rowBytes; ++i)
            current[i] = static_cast<std::uint8_t>(
                current[i] + ((unsigned{current[i - PixelBytes]} + previous[i]) >> 1U));
        return true;
    case 4:
        // With no left or corner neighbour the predictor is the byte above.
        for (std::size_t i = 0; i < head; ++i)
            current[i] = static_cast<std::uint8_t>(current[i] + previous[i]);
        for (std::size_t i = PixelBytes; i < rowBytes; ++i)
            current[i] = static_cast<std::uint8_t>(
                current[i] + paeth(current[i - PixelBytes], previous[i], previous[i - PixelBytes]));
        return true;
    default:
        return false;
    }
}
// One unfiltered row to RGBA16: big-endian 16-bit samples, 8-bit samples widened as v * 257, grey
// replicated, opaque alpha at the maximum (the samples stbi_load_16(..., 4) returns).
template <std::size_t Channels, std::size_t SampleBytes>
void rowSamples(const std::uint8_t* line, std::uint16_t* out, const std::size_t width) noexcept {
    const auto sample = [line](const std::size_t index) noexcept {
        const auto* bytes = line + index * SampleBytes;
        if constexpr (SampleBytes == 2)
            return static_cast<std::uint16_t>((unsigned{bytes[0]} << 8U) | bytes[1]);
        else
            return static_cast<std::uint16_t>(bytes[0] * 257U);
    };
    for (std::size_t x = 0; x < width; ++x, out += 4) {
        const std::size_t base = x * Channels;
        if constexpr (Channels <= 2) {
            const auto grey = sample(base);
            out[0] = grey;
            out[1] = grey;
            out[2] = grey;
            out[3] = Channels == 2 ? sample(base + 1) : std::uint16_t{0xFFFF};
        } else {
            out[0] = sample(base);
            out[1] = sample(base + 1);
            out[2] = sample(base + 2);
            out[3] = Channels == 4 ? sample(base + 3) : std::uint16_t{0xFFFF};
        }
    }
}
// Unfilters and converts rows as inflate completes them, so each row is processed while it is
// still in cache instead of in a second pass over the whole inflated image.
class PngRowDecoder {
  public:
    PngRowDecoder(const std::size_t width, const std::size_t rowBytes, const std::size_t channels,
                  const std::size_t sampleBytes)
        : width_(width), rowBytes_(rowBytes), zeroRow_(rowBytes, 0) {
        const auto key = channels * 10 + sampleBytes;
        switch (key) {
        case 11:
            bind<1, 1>();
            break;
        case 12:
            bind<1, 2>();
            break;
        case 21:
            bind<2, 1>();
            break;
        case 22:
            bind<2, 2>();
            break;
        case 31:
            bind<3, 1>();
            break;
        case 32:
            bind<3, 2>();
            break;
        case 41:
            bind<4, 1>();
            break;
        default:
            bind<4, 2>();
            break;
        }
    }
    // Processes rows [rowsDone, rows) of `raw` (filter byte + row bytes per row).
    [[nodiscard]] bool process(std::uint8_t* raw, std::uint16_t* samples,
                               const std::size_t rows) noexcept {
        for (; rowsDone_ < rows; ++rowsDone_) {
            auto* line = raw + rowsDone_ * (rowBytes_ + 1);
            const std::uint8_t* previous = rowsDone_ == 0 ? zeroRow_.data() : line - rowBytes_;
            if (!unfilter_(line[0], line + 1, previous, rowBytes_))
                return false;
            convert_(line + 1, samples + rowsDone_ * width_ * 4U, width_);
        }
        return true;
    }
    [[nodiscard]] std::size_t rowsDone() const noexcept { return rowsDone_; }

  private:
    template <std::size_t Channels, std::size_t SampleBytes> void bind() noexcept {
        unfilter_ = &unfilterRow<Channels * SampleBytes>;
        convert_ = &rowSamples<Channels, SampleBytes>;
    }
    std::size_t width_;
    std::size_t rowBytes_;
    std::size_t rowsDone_ = 0;
    std::vector<std::uint8_t> zeroRow_;
    bool (*unfilter_)(std::uint8_t, std::uint8_t*, const std::uint8_t*,
                      std::size_t) noexcept = nullptr;
    void (*convert_)(const std::uint8_t*, std::uint16_t*, std::size_t) noexcept = nullptr;
};
} // namespace

// The common PNG subset decoded through zlib's inflate: non-interlaced 8/16-bit grey, grey+alpha,
// RGB and RGBA without tRNS. The samples equal stbi_load_16(..., 4) exactly: big-endian 16-bit
// samples, 8-bit samples widened as v * 257, grey replicated, opaque alpha at the maximum.
// Anything else, or any inconsistency, returns nullopt and the caller keeps the stb decoder.
std::optional<std::vector<std::uint16_t>> fastPngSamples(const std::span<const std::byte> bytes,
                                                         const ImageInfo& info,
                                                         const std::size_t pixelBudget) {
    constexpr std::array<unsigned char, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 33 || info.width <= 0 || info.height <= 0)
        return std::nullopt;
    for (std::size_t i = 0; i < signature.size(); ++i)
        if (std::to_integer<unsigned char>(bytes[i]) != signature[i])
            return std::nullopt;
    if (pngWord(bytes.data() + 8) != 13 || std::memcmp(bytes.data() + 12, "IHDR", 4) != 0)
        return std::nullopt;
    const auto width = static_cast<std::size_t>(pngWord(bytes.data() + 16));
    const auto height = static_cast<std::size_t>(pngWord(bytes.data() + 20));
    const auto depth = std::to_integer<unsigned>(bytes[24]);
    const auto colourType = std::to_integer<unsigned>(bytes[25]);
    if (width != static_cast<std::size_t>(info.width) ||
        height != static_cast<std::size_t>(info.height) || (depth != 8 && depth != 16) ||
        (colourType != 0 && colourType != 2 && colourType != 4 && colourType != 6) ||
        std::to_integer<unsigned>(bytes[26]) != 0 || std::to_integer<unsigned>(bytes[27]) != 0 ||
        std::to_integer<unsigned>(bytes[28]) != 0)
        return std::nullopt;
    const std::size_t channels = colourType == 0   ? 1
                                 : colourType == 2 ? 3
                                 : colourType == 4 ? 2
                                                   : 4;
    const std::size_t sampleBytes = depth / 8;
    const std::size_t pixelBytes = channels * sampleBytes;
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (width > std::numeric_limits<std::uint32_t>::max() / pixelBytes ||
        pixels > std::numeric_limits<std::uint64_t>::max() / 40U)
        return std::nullopt;
    const std::size_t rowBytes = width * pixelBytes;
    const auto inflatedBytes = static_cast<std::uint64_t>(rowBytes + 1) * height;
    // Peak: final RGBA32F + RGBA16 output + the inflated rows, within the caller's admission.
    if (pixels * 24U + inflatedBytes > pixelBudget)
        return std::nullopt;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(inflatedBytes));
    std::vector<std::uint16_t> samples(static_cast<std::size_t>(pixels) * 4U);
    PngRowDecoder rows(width, rowBytes, channels, sampleBytes);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
        return std::nullopt;
    struct End {
        z_stream& stream;
        ~End() { inflateEnd(&stream); }
    } end{stream};
    if (raw.size() > std::numeric_limits<uInt>::max())
        return std::nullopt;
    // Inflate a cache-sized window of rows at a time and unfilter/convert each completed row at
    // once, while it is still in cache.
    const std::size_t window = std::max<std::size_t>(rowBytes + 1, std::size_t{256} * 1024U);
    const auto produced = [&] { return static_cast<std::size_t>(stream.next_out - raw.data()); };
    stream.next_out = raw.data();
    stream.avail_out = 0;
    bool finished = false;
    for (std::size_t position = 8; position + 12 <= bytes.size() && !finished;) {
        const auto length = static_cast<std::size_t>(pngWord(bytes.data() + position));
        const auto* type = bytes.data() + position + 4;
        if (length > bytes.size() - position - 12)
            return std::nullopt;
        if (std::memcmp(type, "tRNS", 4) == 0 || std::memcmp(type, "CgBI", 4) == 0)
            return std::nullopt;
        if (std::memcmp(type, "IEND", 4) == 0)
            break;
        if (std::memcmp(type, "IDAT", 4) == 0 && length != 0) {
            stream.next_in =
                reinterpret_cast<Bytef*>(const_cast<std::byte*>(bytes.data()) + position + 8);
            stream.avail_in = static_cast<uInt>(length);
            while (stream.avail_in != 0) {
                stream.avail_out = static_cast<uInt>(std::min(window, raw.size() - produced()));
                const auto status = inflate(&stream, Z_NO_FLUSH);
                if (!rows.process(raw.data(), samples.data(), produced() / (rowBytes + 1)))
                    return std::nullopt;
                if (status == Z_STREAM_END) {
                    finished = true;
                    break;
                }
                // A full output buffer is fine while the stream end (checksum) is still to come;
                // more input after a full buffer makes inflate report Z_BUF_ERROR or Z_DATA_ERROR.
                if (status != Z_OK)
                    return std::nullopt;
            }
        }
        position += 12 + length;
    }
    if (!finished || produced() != raw.size() || rows.rowsDone() != height)
        return std::nullopt;
    return samples;
}

std::vector<std::uint16_t> imageSamples(const std::filesystem::path& path, const ImageInfo& info,
                                        const std::size_t pixelBudget) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    if (info.tiff || info.dpx) {
        const auto bytes = readWholeFile(stream);
        return info.dpx ? dpxSamples(bytes, info, pixelBudget) : tiffSamples(bytes, info, pixelBudget);
    }
    return decodeSamples(kFileCallbacks, &stream, info, pixelBudget);
}
std::vector<std::uint16_t> stbImageSamples(const std::span<const std::byte> bytes,
                                           const ImageInfo& info, const std::size_t pixelBudget) {
    ByteStream stream{bytes};
    return decodeSamples(kByteCallbacks, &stream, info, pixelBudget);
}
std::vector<std::uint16_t> imageSamples(const std::span<const std::byte> bytes,
                                        const ImageInfo& info, const std::size_t pixelBudget) {
    if (info.tiff)
        return tiffSamples(bytes, info, pixelBudget);
    if (info.dpx)
        return dpxSamples(bytes, info, pixelBudget);
    if (auto fast = fastPngSamples(bytes, info, pixelBudget))
        return std::move(*fast);
    return stbImageSamples(bytes, info, pixelBudget);
}
} // namespace bloom::media::detail
