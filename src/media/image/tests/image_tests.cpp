#include "generated_jpeg.hpp"
#include "png_test_support.hpp"
#include "stb_image_adapter.hpp"
#include <Imath/half.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>
#include <ImfTileDescription.h>
#include <ImfTiledOutputFile.h>
#include <OpenColorIO/OpenColorIO.h>
#include <algorithm>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/media/image.hpp>
#include <bloom/platform/source_identity.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OCIOAlias = OCIO_NAMESPACE;

namespace {
using namespace bloom_output_png_test_support;
void png(const std::filesystem::path& path, const bool sixteen = false) {
    // The existing independent chunk encoder supplies CRCs and framing. zlib supplies only
    // compression, and the production image decoder shares neither implementation.
    {
        std::ofstream file(path, std::ios::binary);
        const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
        file.write(reinterpret_cast<const char*>(signature.data()), 8);
    }
    PngChunkBytes chunks(path);
    const std::vector<unsigned char> header{
        0, 0, 0, 1, 0, 0, 0, 1, static_cast<unsigned char>(sixteen ? 16 : 8), 6, 0, 0, 0};
    chunks.insertChunk(chunks.endOffset(), "IHDR", header);
    const std::vector<unsigned char> raw =
        sixteen ? std::vector<unsigned char>{0, 128, 1, 64, 2, 32, 3, 128, 0}
                : std::vector<unsigned char>{0, 128, 64, 32, 128};
    auto size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(size);
    if (compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        std::abort();
    compressed.resize(size);
    chunks.insertChunk(chunks.endOffset(), "IDAT", compressed);
    chunks.insertChunk(chunks.endOffset(), "IEND", {});
    chunks.save();
}

void exr(const std::filesystem::path& path, const bool tiled, const Imf::PixelType type,
         const bool luminance = false,
         const std::optional<Imf::Chromaticities>& chromaticities = std::nullopt) {
    constexpr int width = 3;
    constexpr int height = 2;
    const Imath::Box2i display(Imath::V2i(-2, 1), Imath::V2i(2, 4));
    const Imath::Box2i data(Imath::V2i(-1, 2), Imath::V2i(1, 3));
    Imf::Header header(display, data);
    if (tiled)
        header.setTileDescription(Imf::TileDescription(2, 2, Imf::ONE_LEVEL));
    header.insert("chromaticities",
                  Imf::ChromaticitiesAttribute(chromaticities.value_or(Imf::Chromaticities())));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    if (luminance) {
        header.channels().insert("Y", Imf::Channel(type));
    } else {
        header.channels().insert("R", Imf::Channel(type));
        header.channels().insert("G", Imf::Channel(type));
        header.channels().insert("B", Imf::Channel(type));
    }
    header.channels().insert("A", Imf::Channel(type));

    constexpr auto sampleCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> red(sampleCount);
    std::vector<float> green(sampleCount);
    std::vector<float> blue(sampleCount);
    std::vector<float> alpha(sampleCount);
    std::vector<Imath::half> redHalf(sampleCount);
    std::vector<Imath::half> greenHalf(sampleCount);
    std::vector<Imath::half> blueHalf(sampleCount);
    std::vector<Imath::half> alphaHalf(sampleCount);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
            red[index] = 0.25F + static_cast<float>(x) * 0.05F;
            green[index] = 0.125F + static_cast<float>(y) * 0.05F;
            blue[index] = 0.0625F;
            alpha[index] = 0.5F;
            redHalf[index] = red[index];
            greenHalf[index] = green[index];
            blueHalf[index] = blue[index];
            alphaHalf[index] = alpha[index];
        }
    }
    Imf::FrameBuffer buffer;
    const auto insert = [&](const char* name, const std::vector<float>& samples,
                            const std::vector<Imath::half>& halfSamples) {
        if (type == Imf::HALF) {
            buffer.insert(name, Imf::Slice::Make(type, halfSamples.data(), data,
                                                 sizeof(Imath::half), sizeof(Imath::half) * width));
        } else {
            buffer.insert(name, Imf::Slice::Make(type, samples.data(), data, sizeof(float),
                                                 sizeof(float) * width));
        }
    };
    if (luminance)
        insert("Y", red, redHalf);
    else {
        insert("R", red, redHalf);
        insert("G", green, greenHalf);
        insert("B", blue, blueHalf);
    }
    insert("A", alpha, alphaHalf);
    if (tiled) {
        Imf::TiledOutputFile output(path.string().c_str(), header, 1);
        output.setFrameBuffer(buffer);
        output.writeTiles(0, output.numXTiles() - 1, 0, output.numYTiles() - 1, 0);
    } else {
        Imf::OutputFile output(path.string().c_str(), header, 1);
        output.setFrameBuffer(buffer);
        output.writePixels(height);
    }
}

void patchExrVersionFlag(const std::filesystem::path& path, const std::uint32_t flag) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(4);
    std::array<unsigned char, 4> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    auto version = static_cast<std::uint32_t>(bytes[0]) |
                   (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                   (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[3]) << 24U);
    version |= flag;
    bytes = {static_cast<unsigned char>(version), static_cast<unsigned char>(version >> 8U),
             static_cast<unsigned char>(version >> 16U),
             static_cast<unsigned char>(version >> 24U)};
    file.clear();
    file.seekp(4);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void acesAp0Read(const std::filesystem::path& path, Expectations& check) {
    Imf::Chromaticities ap0;
    ap0.red = {0.7347F, 0.2653F};
    ap0.green = {0.0F, 1.0F};
    ap0.blue = {0.0001F, -0.0770F};
    ap0.white = {0.32168F, 0.33767F};
    exr(path, false, Imf::FLOAT, false, ap0);

    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    check.expect(revision.has_value(), "ACES config revision is available for the AP0 fixture");
    if (!revision)
        return;
    auto resolved = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
        *revision, bloom::color::kAcesCgV1SceneLinearColorSpaceId);
    check.expect(resolved.ready(), "ACES config resolves for the AP0 fixture");
    if (!resolved)
        return;
    auto processor = bloom::color::CpuColorSpaceProcessor::prepare(
        *resolved.resolved(), "ACES2065-1", bloom::color::kAcesCgV1SceneLinearColorSpaceId);
    check.expect(processor.succeeded(), "the AP0-to-ACEScg input processor prepares");
    if (!processor)
        return;

    const auto decoded = bloom::media::decodeImage(
        path,
        bloom::media::ImageInterpretation{.colorSpace = bloom::media::ImageColorSpace::Auto,
                                          .inputColorSpaceId = "ACES2065-1",
                                          .alphaAssociation =
                                              bloom::media::ImageAlphaAssociation::Auto},
        std::move(processor).takeProcessor());
    check.expect(decoded.value.has_value(), "an AP0 EXR reads through the ACEScg processor");
    if (!decoded.value.has_value())
        return;

    auto oracleConfig = OCIOAlias::Config::CreateFromBuiltinConfig(
        std::string(bloom::color::kAcesCgV1BuiltinConfigName).c_str());
    const auto oracleProcessor =
        oracleConfig
            ->getProcessor(OCIOAlias::Context::Create(), "ACES2065-1",
                           std::string(bloom::color::kAcesCgV1SceneLinearColorSpaceId).c_str())
            ->getDefaultCPUProcessor();
    std::array<float, 3> oraclePixel{0.5F, 0.25F, 0.125F};
    oracleProcessor->applyRGB(oraclePixel.data());
    const auto pixel = (*decoded.value)->read(-1, 2);
    check.expect(pixel && std::abs(pixel.value()->red() - oraclePixel[0] * 0.5F) < 0.00001F &&
                     std::abs(pixel.value()->green() - oraclePixel[1] * 0.5F) < 0.00001F &&
                     std::abs(pixel.value()->blue() - oraclePixel[2] * 0.5F) < 0.00001F &&
                     pixel.value()->alpha() == 0.5F,
                 "AP0 EXR reads to ACEScg within the OCIO oracle tolerance");
}
// In-memory PNG with every row filter (rows cycle None, Sub, Up, Average, Paeth), so the zlib fast
// path is compared against the stb decoder over every filter and colour type.
void appendWord(std::vector<std::byte>& out, const std::uint32_t value) {
    for (const auto shift : {24U, 16U, 8U, 0U})
        out.push_back(static_cast<std::byte>((value >> shift) & 255U));
}
void appendChunk(std::vector<std::byte>& out, const char* type,
                 const std::vector<unsigned char>& data) {
    appendWord(out, static_cast<std::uint32_t>(data.size()));
    const auto start = out.size();
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>(type[i]));
    for (const auto value : data)
        out.push_back(static_cast<std::byte>(value));
    const auto crc = crc32(0L, reinterpret_cast<const Bytef*>(out.data() + start),
                           static_cast<uInt>(out.size() - start));
    appendWord(out, static_cast<std::uint32_t>(crc));
}
std::vector<std::byte> filteredPng(const unsigned width, const unsigned height,
                                   const unsigned depth, const unsigned colourType,
                                   const bool interlaced = false, const bool transparency = false,
                                   const bool adlerChunk = false) {
    const unsigned channels = colourType == 0 ? 1 : colourType == 2 ? 3 : colourType == 4 ? 2 : 4;
    const unsigned pixelBytes = channels * depth / 8;
    const unsigned rowBytes = width * pixelBytes;
    std::vector<unsigned char> image(static_cast<std::size_t>(rowBytes) * height);
    std::uint32_t seed = 0x2545F491U ^ (depth * 31U + colourType);
    for (auto& value : image) {
        seed = seed * 1103515245U + 12345U;
        value = static_cast<unsigned char>(seed >> 23U);
    }
    std::vector<unsigned char> raw;
    for (unsigned row = 0; row < height; ++row) {
        const unsigned filter = row % 5;
        raw.push_back(static_cast<unsigned char>(filter));
        const auto* current = image.data() + static_cast<std::size_t>(row) * rowBytes;
        const auto* previous = row == 0 ? nullptr : current - rowBytes;
        for (unsigned i = 0; i < rowBytes; ++i) {
            const int left = i >= pixelBytes ? current[i - pixelBytes] : 0;
            const int up = previous ? previous[i] : 0;
            const int corner = i >= pixelBytes && previous ? previous[i - pixelBytes] : 0;
            int predictor = 0;
            if (filter == 1)
                predictor = left;
            else if (filter == 2)
                predictor = up;
            else if (filter == 3)
                predictor = (left + up) / 2;
            else if (filter == 4) {
                const int p = left + up - corner;
                const int pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - corner);
                predictor = pa <= pb && pa <= pc ? left : (pb <= pc ? up : corner);
            }
            raw.push_back(static_cast<unsigned char>(current[i] - predictor));
        }
    }
    auto size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(size);
    if (compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        std::abort();
    compressed.resize(size);
    std::vector<std::byte> out;
    for (const int value : {137, 80, 78, 71, 13, 10, 26, 10})
        out.push_back(static_cast<std::byte>(value));
    std::vector<unsigned char> header;
    for (const auto value : {width, height})
        for (const auto shift : {24U, 16U, 8U, 0U})
            header.push_back(static_cast<unsigned char>((value >> shift) & 255U));
    header.insert(header.end(),
                  {static_cast<unsigned char>(depth), static_cast<unsigned char>(colourType), 0, 0,
                   static_cast<unsigned char>(interlaced ? 1 : 0)});
    appendChunk(out, "IHDR", header);
    if (transparency)
        appendChunk(out, "tRNS", {0, 0, 0, 0, 0, 0});
    // Split IDAT so the fast path streams more than one chunk into inflate.
    // adlerChunk puts only the 4-byte zlib checksum in the last IDAT, so the output is already
    // full when the stream end arrives.
    const auto half = adlerChunk ? compressed.size() - 4 : compressed.size() / 2;
    appendChunk(out, "IDAT", {compressed.begin(), compressed.begin() + static_cast<long>(half)});
    appendChunk(out, "IDAT", {compressed.begin() + static_cast<long>(half), compressed.end()});
    appendChunk(out, "IEND", {});
    return out;
}

void testFastPngMatchesStb(Expectations& check) {
    namespace detail = bloom::media::detail;
    constexpr std::size_t budget = std::size_t{64} * 1024U * 1024U;
    // Rows are unfiltered as inflate completes them: a one-pixel column, a small image and one
    // whose rows straddle the IDAT split many times over all match stb.
    for (const auto& [width, height] :
         {std::pair{37U, 11U}, std::pair{1U, 7U}, std::pair{613U, 97U}}) {
        for (const unsigned depth : {8U, 16U}) {
            for (const unsigned colourType : {0U, 2U, 4U, 6U}) {
                const auto bytes = filteredPng(width, height, depth, colourType);
                detail::ImageInfo info;
                check.expect(detail::imageInfo(bytes, info), "generated PNG header reads");
                const auto fast = detail::fastPngSamples(bytes, info, budget);
                const auto reference = detail::stbImageSamples(bytes, info, budget);
                check.expect(fast.has_value() && !reference.empty() && *fast == reference,
                             "zlib PNG samples equal the stb decoder for every filter and type");
            }
        }
    }
    {
        const auto bytes = filteredPng(37, 11, 8, 6, false, false, true);
        detail::ImageInfo adlerInfo;
        check.expect(detail::imageInfo(bytes, adlerInfo), "adler-chunk PNG header reads");
        const auto fast = detail::fastPngSamples(bytes, adlerInfo, budget);
        check.expect(fast.has_value() && *fast == detail::stbImageSamples(bytes, adlerInfo, budget),
                     "a PNG whose output fills before the zlib end still takes the fast path");
    }
    const auto transparent = filteredPng(5, 3, 8, 2, false, true);
    const auto interlaced = filteredPng(5, 3, 8, 6, true);
    detail::ImageInfo info;
    check.expect(detail::imageInfo(transparent, info) &&
                     !detail::fastPngSamples(transparent, info, budget).has_value(),
                 "tRNS stays on the stb decoder");
    check.expect(detail::imageInfo(interlaced, info) &&
                     !detail::fastPngSamples(interlaced, info, budget).has_value(),
                 "interlaced PNG stays on the stb decoder");
    const auto small = filteredPng(37, 11, 16, 6);
    check.expect(detail::imageInfo(small, info) &&
                     !detail::fastPngSamples(small, info, 37 * 11 * 24).has_value(),
                 "the fast path never exceeds the caller's peak admission");
}

} // namespace

// ADR 0025 (amended 2026-11-04): a first use admits the bytes by the file identity without a full
// SHA-256, and decodes pixels identical to a digest-first capture. A changed identity still forces
// the hash, and a memo digest that refutes the recorded digest reports the relink diagnostic.
void testDeferredDigests(bloom_output_png_test_support::Expectations& check,
                         bloom_output_png_test_support::ScratchDirectory& scratch) {
    namespace media = bloom::media;
    namespace platform = bloom::platform;
    for (const auto* extension : {"png", "exr"}) {
        const auto path = scratch.file(std::string("deferred.") + extension);
        if (std::string_view(extension) == "png")
            png(path);
        else
            exr(path, false, Imf::HALF);
        std::vector<std::byte> bytes;
        {
            std::ifstream input(path, std::ios::binary);
            const std::string text(std::istreambuf_iterator<char>(input), {});
            bytes.resize(text.size());
            std::memcpy(bytes.data(), text.data(), text.size());
        }
        const auto recorded = bloom::core::Sha256Hasher::hash(bytes);
        check.expect(recorded.has_value(), "fixture digest");
        if (!recorded)
            continue;
        platform::verifiedSourceDigests().clear();
        const auto readsBefore = platform::sourceReadWork();
        const auto deferred =
            media::captureImageSourceRevision(path, media::kMaxImageStorageBytes, {}, *recorded);
        check.expect(deferred && !(*deferred.value)->contentVerified() &&
                         (*deferred.value)->captureStatistics().bytesHashed == 0 &&
                         (*deferred.value)->probe().contentDigest == *recorded &&
                         (*deferred.value)->sourceIdentity().has_value() &&
                         platform::sourceReadWork().fullReads == readsBefore.fullReads + 1,
                     "a first use reads the file once and admits it by its identity, no hash");
        check.expect(
            !platform::verifiedSourceDigests().find(path, *platform::observeFileIdentity(path)),
            "an admitted identity vouches for no digest");
        // Digest-first: the hash is in the memo before the capture (a background pass).
        platform::verifiedSourceDigests().clear();
        const auto hashed = platform::hashSourceFile(path);
        const auto first =
            media::captureImageSourceRevision(path, media::kMaxImageStorageBytes, {}, *recorded);
        check.expect(hashed.error == platform::SourceHashError::None && first &&
                         (*first.value)->contentVerified() &&
                         (*first.value)->probe().contentDigest == *recorded,
                     "a digest-first capture is verified by the memo");
        if (!deferred || !first)
            continue;
        const auto a = media::decodeImage(**deferred.value);
        const auto b = media::decodeImage(**first.value);
        check.expect(a && b && *(*a.value)->descriptor() == *(*b.value)->descriptor() &&
                         (*a.value)->pixels().size() == (*b.value)->pixels().size() &&
                         std::memcmp((*a.value)->pixels().data(), (*b.value)->pixels().data(),
                                     (*a.value)->pixels().size_bytes()) == 0,
                     "digest-deferred and digest-first captures decode identical pixels");
        // A memo digest that differs from the recorded digest refutes it.
        auto refutingBytes = bloom::core::Sha256Digest::Bytes{};
        std::ranges::copy(recorded->bytes(), refutingBytes.begin());
        refutingBytes[0] ^= 1U;
        const auto refuting = bloom::core::Sha256Digest::fromBytes(refutingBytes);
        platform::verifiedSourceDigests().record(path, *platform::observeFileIdentity(path),
                                                 refuting);
        const auto refuted =
            media::captureImageSourceRevision(path, media::kMaxImageStorageBytes, {}, *recorded);
        check.expect(!refuted && refuted.code == media::ImageDiagnosticCode::DigestMismatch,
                     "a full hash that refutes the recorded digest reports the relink diagnostic");
        // A changed identity (here a rewrite with the same bytes) forces a rehash, which admits
        // unchanged content; changed content is refused.
        platform::verifiedSourceDigests().clear();
        (void)media::captureImageSourceRevision(path, media::kMaxImageStorageBytes, {}, *recorded);
        const auto stampBefore = media::observeImageSourceStamp(path);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::ofstream rewrite(path, std::ios::binary | std::ios::trunc);
            rewrite.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
        }
        const auto rehashed =
            media::captureImageSourceRevision(path, media::kMaxImageStorageBytes, {}, *recorded);
        check.expect(rehashed && (*rehashed.value)->contentVerified() &&
                         (*rehashed.value)->captureStatistics().bytesHashed == bytes.size(),
                     "a changed identity forces the hash, which admits the same bytes");
        // A verified member (stamp and digest, document 1.36) whose stamp changed is rehashed.
        const auto verifiedMember = media::captureImageSourceRevision(
            path, media::kMaxImageStorageBytes, {}, *recorded, stampBefore);
        check.expect(stampBefore && verifiedMember && (*verifiedMember.value)->contentVerified(),
                     "a verified member with a changed stamp is admitted by its digest");
        {
            std::ofstream rewrite(path, std::ios::binary | std::ios::trunc);
            const std::string changed(bytes.size(), 'x');
            rewrite.write(changed.data(), static_cast<std::streamsize>(changed.size()));
        }
        const auto changed =
            media::captureImageSourceRevision(path, media::kMaxImageStorageBytes, {}, *recorded);
        check.expect(!changed && changed.code == media::ImageDiagnosticCode::DigestMismatch,
                     "changed content under a changed identity reports the relink diagnostic");
    }
    // A pending member (stamp only) is admitted by its stamp without a hash.
    const auto pending = scratch.file("pending.0001.png");
    png(pending);
    bloom::platform::verifiedSourceDigests().clear();
    const auto stamp = media::observeImageSourceStamp(pending);
    const auto admitted =
        media::captureImageSourceRevision(pending, media::kMaxImageStorageBytes, {}, {}, stamp);
    check.expect(stamp && admitted && !(*admitted.value)->contentVerified() &&
                     (*admitted.value)->captureStatistics().bytesHashed == 0,
                 "a pending stamped member is admitted by its stamp without a hash");
}

int main() {
    using namespace bloom_output_png_test_support;
    namespace media = bloom::media;
    ScratchDirectory scratch("media-image");
    Expectations check;
    testFastPngMatchesStb(check);
    // Every codec reads a frozen revision after its original pathname disappears. Comparing
    // independent path decode pixels also qualifies the custom EXR/stb stream adapters.
    for (const auto* extension : {"png", "jpg", "exr", "tiled.exr"}) {
        const auto path = scratch.file(std::string("revision.") + extension);
        if (std::string_view(extension) == "png")
            png(path);
        else if (std::string_view(extension) == "jpg") {
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()), kGeneratedJpeg.size());
        } else
            exr(path, std::string_view(extension) == "tiled.exr", Imf::FLOAT);
        const auto oracle = media::decodeImage(path);
        const auto captured = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes);
        check.expect(oracle && captured, "capture revision for every supported in-process codec");
        if (!oracle || !captured)
            continue;
        const auto revision = *captured.value;
        const auto& measured = revision->captureStatistics();
        // The path decode above hashed the file, so the process memo vouches for this identity
        // and the capture reads it without hashing again (ADR 0025).
        check.expect(measured.bytesRead == revision->bytes().size() && measured.bytesHashed == 0 &&
                         measured.ioNanoseconds > 0 && measured.probeNanoseconds > 0 &&
                         revision->contentVerified(),
                     "a memo-verified revision separates measured I/O and metadata work, no hash");
        const auto ownDigest = bloom::core::Sha256Hasher::hash(revision->bytes());
        check.expect(ownDigest && *ownDigest == revision->probe().contentDigest,
                     "source revision SHA256 describes its actual decoder bytes");
        const auto timestamp = std::filesystem::last_write_time(path);
        // Same size and timestamp do not authorize reuse or final revalidation.
        {
            std::ofstream replacement(path, std::ios::binary | std::ios::trunc);
            const std::string changed(revision->bytes().size(), 'x');
            replacement.write(changed.data(), static_cast<std::streamsize>(changed.size()));
        }
        std::filesystem::last_write_time(path, timestamp);
        const auto replaced = media::captureImageSourceRevision(
            path, media::kMaxImageStorageBytes, {}, revision->probe().contentDigest);
        check.expect(!replaced && replaced.code == media::ImageDiagnosticCode::DigestMismatch,
                     "same-size same-timestamp replacement fails strong revision validation");
        std::filesystem::remove(path);

        media::ImageDecodeStatistics decodeWork;
        const auto decoded = media::decodeImage(*revision, {}, {}, {}, {},
                                                media::kMaxImageStorageBytes, &decodeWork);
        check.expect(decodeWork.decodeNanoseconds > 0,
                     "decoder duration is measured independently of source I/O/hash/probe");
        auto constrained = std::async(std::launch::async, [revision] {
            return media::decodeImage(*revision, {}, {}, {}, {}, revision->retainedBytes());
        });
        auto concurrent =
            std::async(std::launch::async, [revision] { return media::decodeImage(*revision); });
        check.expect(!constrained.get() && concurrent.get().value.has_value(),
                     "concurrent revision decoders have independent streams and request budgets");
        check.expect(decoded && (*decoded.value)->descriptor() &&
                         *(*decoded.value)->descriptor() == *(*oracle.value)->descriptor() &&
                         (*decoded.value)->pixels().size() == (*oracle.value)->pixels().size() &&
                         std::memcmp((*decoded.value)->pixels().data(),
                                     (*oracle.value)->pixels().data(),
                                     (*oracle.value)->pixels().size_bytes()) == 0,
                     "decoding retained immutable bytes stays pixel-identical after path "
                     "replacement/removal");
        const auto missing = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes);
        check.expect(!missing && missing.code == media::ImageDiagnosticCode::FileUnavailable,
                     "fresh validation reports missing source rather than trusting a revision");
        const auto refused =
            media::decodeImage(*revision, {}, {}, {}, {}, revision->retainedBytes());
        check.expect(!refused && refused.code == media::ImageDiagnosticCode::PixelBudgetExceeded,
                     "encoded revision is charged before decoder working-set admission");
    }
    testDeferredDigests(check, scratch);
    {
        const auto path = scratch.file("capture-budget.png");
        png(path);
        const auto refused = media::captureImageSourceRevision(path, 1);
        check.expect(!refused && refused.code == media::ImageDiagnosticCode::PixelBudgetExceeded,
                     "capture refuses encoded storage before allocation");
        const auto cancelled = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes,
                                                                 [] { return true; });
        check.expect(!cancelled && cancelled.cancelled,
                     "pre-cancelled capture publishes no source revision");
        {
            std::ofstream append(path, std::ios::binary | std::ios::app);
            const std::string tail(std::size_t{3} * 1024 * 1024, 'x');
            append.write(tail.data(), static_cast<std::streamsize>(tail.size()));
        }
        unsigned polls = 0;
        const auto interrupted = media::captureImageSourceRevision(
            path, media::kMaxImageStorageBytes, [&] { return ++polls == 3; });
        check.expect(!interrupted && interrupted.cancelled && polls == 3,
                     "large source capture cancels between bounded hashing chunks");
    }

    for (bool sixteen : {false, true}) {
        const auto path = scratch.file(sixteen ? "image16.png" : "image.png");
        png(path, sixteen);
        const auto probe = media::probeImage(path);
        check.expect(probe.value.has_value() && probe.value->width == 1 &&
                         probe.value->bitDepth == (sixteen ? 16 : 8),
                     "PNG dimensions and precision");
        const auto result = media::decodeImage(path);
        check.expect(result.value.has_value(), "PNG decode succeeds");
        if (result.value.has_value()) {
            const auto pixel = (*result.value)->read(0, 0);
            check.expect(pixel && std::abs(pixel.value()->red() - 0.108F) < 0.002F,
                         "inverse sRGB then premultiply");
        }
        const auto neutral = bloom::color::resolveBloomNeutralV1BuiltIn(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
            bloom::color::kBloomNeutralV1ConfigUri, bloom::color::kBloomNeutralV1ConfigDigest);
        check.expect(neutral.ready(), "Bloom Neutral resolves for byte-identity coverage");
        if (neutral) {
            auto managedProcessor = bloom::color::CpuColorSpaceProcessor::prepare(
                *neutral.resolved(), neutral.resolved()->sRgbTextureColorSpaceId(),
                neutral.resolved()->processColorSpaceId());
            check.expect(managedProcessor.succeeded(),
                         "Bloom Neutral config processor prepares for byte-identity coverage");
            if (managedProcessor) {
                const auto managed =
                    media::decodeImage(path,
                                       media::ImageInterpretation{
                                           .colorSpace = media::ImageColorSpace::Auto,
                                           .inputColorSpaceId = std::string(
                                               neutral.resolved()->sRgbTextureColorSpaceId()),
                                           .alphaAssociation = media::ImageAlphaAssociation::Auto},
                                       std::move(managedProcessor).takeProcessor());
                const auto oldPixels = result.value ? (*result.value)->pixels()
                                                    : std::span<const bloom::render::Rgba32f>{};
                const auto newPixels = managed.value ? (*managed.value)->pixels()
                                                     : std::span<const bloom::render::Rgba32f>{};
                check.expect(managed.value.has_value() && oldPixels.size() == newPixels.size() &&
                                 std::memcmp(oldPixels.data(), newPixels.data(),
                                             oldPixels.size_bytes()) == 0,
                             "Neutral-project sRGB PNG decode remains byte-identical");
            }
        }
        const auto raw = media::decodeImage(
            path,
            media::ImageInterpretation{.colorSpace = media::ImageColorSpace::Raw,
                                       .inputColorSpaceId = {},
                                       .alphaAssociation = media::ImageAlphaAssociation::Auto});
        check.expect(raw.value.has_value() &&
                         std::abs((*raw.value)->pixels()[0].red() - 0.252F) < 0.002F,
                     "Raw preserves encoded RGB then premultiplies");
        check.expect(!media::decodeImage(path, {}, {}, {}, {}, 1),
                     "pixel budget rejects before decode");
        check.expect(media::decodeImage(path, {}, {}, [] { return true; }).cancelled,
                     "cancel before read");
        PngChunkBytes broken(path);
        broken.truncateTo(30);
        broken.save();
        check.expect(!media::decodeImage(path), "truncated PNG rejected");
        png(path, sixteen);
        PngChunkBytes huge(path);
        huge.patchDataByteWithValidCrc("IHDR", 0, 0, 127);
        huge.save();
        check.expect(!media::probeImage(path) && !media::decodeImage(path),
                     "huge dimensions rejected");
    }
    const auto jpeg = scratch.file("tiny.jpg");
    {
        std::ofstream file(jpeg, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()),
                   static_cast<std::streamsize>(kGeneratedJpeg.size()));
    }
    const auto jpg = media::decodeImage(jpeg);
    check.expect(jpg.value.has_value() &&
                     (*jpg.value)->descriptor()->dataWindow().extent().width() == 2,
                 "generated JPEG decodes");
    {
        const auto tiff = scratch.file("provider-missing.tiff");
        std::ofstream file(tiff, std::ios::binary);
        file.write("II*\0", 4);
        file.close();
        const auto probe = media::probeImage(tiff);
        const auto decoded = media::decodeImage(tiff);
        check.expect(!probe && probe.code == media::ImageDiagnosticCode::ProviderMissing &&
                         !decoded && decoded.code == media::ImageDiagnosticCode::ProviderMissing,
                     "TIFF magic reaches the provider seam and refuses without a provider");
    }
    for (const auto fixture :
         {std::pair{"scanline-half.exr", false}, std::pair{"tiled-float.exr", true}}) {
        const auto path = scratch.file(fixture.first);
        exr(path, fixture.second, fixture.second ? Imf::FLOAT : Imf::HALF);
        const auto probe = media::probeImage(path);
        check.expect(probe.value.has_value() && probe.value->format == media::ImageFormat::Exr &&
                         probe.value->width == 3 && probe.value->height == 2 &&
                         probe.value->bitDepth == (fixture.second ? 32 : 16) &&
                         probe.value->colorSpace == media::ImageColorSpace::Linear &&
                         probe.value->alphaAssociation ==
                             media::ImageAlphaAssociation::Premultiplied,
                     "EXR probe reports bounded dimensions, precision, and interpretation");
        if (probe.value.has_value()) {
            const auto decoded = media::decodeImage(
                path, {}, {}, {}, {}, media::kMaxImageStorageBytes, probe.value->contentDigest);
            check.expect(decoded.value.has_value(), "EXR decode accepts its pinned content digest");
            if (decoded.value.has_value()) {
                const auto pixel = (*decoded.value)->read(-1, 2);
                check.expect(pixel && std::abs(pixel.value()->red() - 0.25F) < 0.01F &&
                                 std::abs(pixel.value()->alpha() - 0.5F) < 0.01F,
                             "EXR premultiplied samples are not multiplied twice");
                check.expect((*decoded.value)->descriptor()->displayWindow().originX() == -2 &&
                                 (*decoded.value)->descriptor()->displayWindow().originY() == 1,
                             "EXR data and display windows retain their origins");
            }
            auto wrongBytes = bloom::core::Sha256Digest::Bytes{};
            std::copy(probe.value->contentDigest.bytes().begin(),
                      probe.value->contentDigest.bytes().end(), wrongBytes.begin());
            wrongBytes[0] ^= 1U;
            const auto wrongDigest = bloom::core::Sha256Digest::fromBytes(wrongBytes);
            const auto rejected =
                media::decodeImage(path, {}, {}, {}, {}, media::kMaxImageStorageBytes, wrongDigest);
            check.expect(!rejected && rejected.code == media::ImageDiagnosticCode::DigestMismatch,
                         "EXR digest mismatch is typed and clean");
        }
    }
    {
        acesAp0Read(scratch.file("aces-ap0.exr"), check);
    }
    {
        const auto path = scratch.file("luminance.exr");
        exr(path, false, Imf::FLOAT, true);
        const auto decoded = media::decodeImage(path);
        check.expect(decoded.value.has_value(), "EXR Y plus alpha decodes");
        if (decoded.value.has_value()) {
            const auto pixel = (*decoded.value)->read(-1, 2);
            check.expect(pixel &&
                             std::abs(pixel.value()->red() - pixel.value()->green()) < 0.001F &&
                             std::abs(pixel.value()->green() - pixel.value()->blue()) < 0.001F,
                         "EXR luminance is replicated to RGB");
        }
    }
    {
        const auto path = scratch.file("truncated.exr");
        exr(path, false, Imf::HALF);
        std::ifstream source(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(source)), {});
        bytes.resize(8);
        std::ofstream target(path, std::ios::binary | std::ios::trunc);
        target.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        target.close();
        const auto result = media::probeImage(path);
        check.expect(!result && result.code != media::ImageDiagnosticCode::None,
                     "truncated EXR is refused with a typed diagnostic");
    }
    for (const auto fixture :
         {std::pair{"deep.exr", 0x00000800U}, std::pair{"multipart.exr", 0x00001000U}}) {
        const auto path = scratch.file(fixture.first);
        exr(path, false, Imf::HALF);
        patchExrVersionFlag(path, fixture.second);
        const auto result = media::probeImage(path);
        // Multipart files are supported (ADR 0026); a single-part file whose version claims
        // multipart has no part headers and is refused as malformed, never decoded.
        const auto typed = fixture.second == 0x00000800U
                               ? result.code == media::ImageDiagnosticCode::UnsupportedDeepImage
                               : result.code != media::ImageDiagnosticCode::None;
        check.expect(!result && typed,
                     "deep and malformed multipart EXR fixtures are refused with typed "
                     "diagnostics");
    }
    // "" with an empty base name is the render-farm form: 0000.png, 0001.png, ... (owner,
    // 2026-09-15: such a folder imported as individual images).
    // Any name shape: the last digit run is the frame; prefix, suffix and padding are free.
    for (const auto* prefix : {"take.", "take_", "take", ""}) {
        png(scratch.file(std::string(prefix) + "0001.png"));
        png(scratch.file(std::string(prefix) + "0002.png"));
        png(scratch.file(std::string(prefix) + "0004.png"));
        const auto sequence = media::scanSequence(scratch.file(std::string(prefix) + "0001.png"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 3 &&
                         sequence.value->gaps == std::vector<std::int64_t>{3} &&
                         sequence.value->padding == 4,
                     "sequence form detects members, padding and gap");
    }
    {
        png(scratch.file("sparse.0.png"));
        png(scratch.file("sparse.1.png"));
        png(scratch.file("sparse.9223372036854775807.png"));
        const auto sequence = media::scanSequence(scratch.file("sparse.0.png"));
        check.expect(
            sequence.value.has_value() && sequence.value->members.size() == 3 &&
                sequence.value->last == std::numeric_limits<std::int64_t>::max() &&
                sequence.value->gaps.empty() && sequence.value->hasGaps(),
            "sparse sequence span is represented by adjacent members without gap allocation");
    }
    {
        exr(scratch.file("plates.0001.exr"), false, Imf::HALF);
        exr(scratch.file("plates.0002.exr"), false, Imf::HALF);
        const auto sequence = media::scanSequence(scratch.file("plates.0001.exr"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 2 &&
                         sequence.value->pattern == "plates.####.exr",
                     "EXR files scan as numbered image sequences");
    }
    {
        exr(scratch.file("broken.0001.exr"), false, Imf::HALF);
        {
            std::ofstream bad(scratch.file("broken.0002.exr"), std::ios::binary);
            bad.write("v/1\x01garbage", 12);
        }
        exr(scratch.file("broken.0003.exr"), false, Imf::HALF);
        // #473: the scan stamps members without reading them, so a member whose bytes are not an
        // image is found when it is first read (and becomes a gap then), not here.
        const auto before = bloom::platform::sourceReadWork();
        const auto sequence = media::scanSequence(scratch.file("broken.0001.exr"));
        check.expect(bloom::platform::sourceReadWork().fullReads == before.fullReads,
                     "a sequence scan reads no member");
        const auto stamp = media::observeImageSourceStamp(scratch.file("broken.0002.exr"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 3 &&
                         sequence.value->gaps.empty() && stamp &&
                         sequence.value->members[1].stamp == *stamp,
                     "every listed member carries its file's stamp, readable or not");
        // A member that cannot even be stamped (here a directory) is a reported gap.
        std::filesystem::create_directory(scratch.file("broken.0004.exr"));
        exr(scratch.file("broken.0005.exr"), false, Imf::HALF);
        const auto gapped = media::scanSequence(scratch.file("broken.0001.exr"));
        check.expect(gapped.value.has_value() && gapped.value->members.size() == 4 &&
                         gapped.value->gaps == std::vector<std::int64_t>{4} &&
                         std::ranges::any_of(gapped.value->diagnostics,
                                             [](const std::string& line) {
                                                 return line.starts_with("Frame 4 is unreadable");
                                             }),
                     "an unstampable sequence member is a reported gap, not a failed scan");
        // A capture against a stamp admits the file only while it keeps that stamp.
        const auto path = scratch.file("broken.0003.exr");
        const auto current = media::observeImageSourceStamp(path);
        check.expect(current && media::captureImageSourceRevision(
                                    path, media::kMaxImageStorageBytes, {}, {}, *current)
                                    .value.has_value(),
                     "a stamped capture reads the file that carries the stamp");
        auto stale = *current;
        stale.modifiedNanoseconds -= 1000000000;
        const auto refused =
            media::captureImageSourceRevision(path, media::kMaxImageStorageBytes, {}, {}, stale);
        check.expect(!refused.value && refused.code == media::ImageDiagnosticCode::DigestMismatch,
                     "a stamped capture refuses a file whose stamp changed");
    }
    {
        png(scratch.file("stereo0001_left.png"));
        png(scratch.file("stereo0002_left.png"));
        png(scratch.file("stereo0001_right.png"));
        const auto sequence = media::scanSequence(scratch.file("stereo0001_left.png"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 2 &&
                         sequence.value->pattern == "stereo####_left.png",
                     "a suffix after the frame number is part of the member identity");
    }
    {
        png(scratch.file("mixed7.png"));
        png(scratch.file("mixed08.png"));
        png(scratch.file("mixed009.PNG"));
        const auto sequence = media::scanSequence(scratch.file("mixed7.png"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 3 &&
                         sequence.value->first == 7 && sequence.value->last == 9 &&
                         !sequence.value->diagnostics.empty(),
                     "mixed padding and extension case still form one sequence, with a note");
    }
    {
        // #458: numbered files without two consecutive frame numbers (camera files) are not a
        // sequence; the picked file is a still and nothing else is listed or read for it.
        for (const auto* name : {"IMG_2041.png", "IMG_2077.png", "IMG_3105.png"})
            png(scratch.file(name));
        const auto picked = scratch.file("IMG_2077.png");
        const auto sequence = media::scanSequence(picked);
        check.expect(sequence.value.has_value() && sequence.value->members.empty() &&
                         media::formsSequence(picked) == false &&
                         media::countSequenceMembers(picked) == 1U,
                     "numbered files without consecutive frame numbers are stills");
        // A frame range does not change what the group is.
        const auto ranged = media::scanSequence(picked, {}, {}, media::SequenceFrameRange{0, 1});
        check.expect(ranged.value.has_value() && ranged.value->members.empty(),
                     "a frame range leaves a non-sequence a still");
        png(scratch.file("IMG_2042.png"));
        check.expect(media::formsSequence(picked) == true &&
                         media::countSequenceMembers(picked) == 4U,
                     "one consecutive pair makes the group a sequence");
    }
    {
        // Sequences rendered on twos or tens: three frames at one constant step are a sequence,
        // two files or uneven numbers are not.
        for (const auto* name : {"twos0001.png", "twos0003.png"})
            png(scratch.file(name));
        check.expect(media::formsSequence(scratch.file("twos0003.png")) == false,
                     "two frames a step apart are not yet a sequence");
        png(scratch.file("twos0005.png"));
        check.expect(media::formsSequence(scratch.file("twos0003.png")) == true &&
                         media::scanSequence(scratch.file("twos0001.png")).value->members.size() ==
                             3U,
                     "frames on twos form a sequence");
        for (const auto* name : {"tens0010.png", "tens0020.png", "tens0030.png"})
            png(scratch.file(name));
        check.expect(media::formsSequence(scratch.file("tens0020.png")) == true,
                     "frames 10, 20, 30 form a sequence");
        for (const auto* name : {"cam_2041.png", "cam_2077.png", "cam_3105.png", "cam_4000.png"})
            png(scratch.file(name));
        check.expect(media::formsSequence(scratch.file("cam_2077.png")) == false,
                     "uneven frame numbers stay stills");
    }
    {
        // #458: several files numbered with one frame keep one of them and say so; the scan
        // never fails. The picked file wins its frame, then the file written like it.
        png(scratch.file("dup.1.png"));
        png(scratch.file("dup.01.png"));
        png(scratch.file("dup.2.png"));
        const auto sequence = media::scanSequence(scratch.file("dup.2.png"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 2 &&
                         sequence.value->members.front().path.filename() == "dup.1.png" &&
                         std::ranges::any_of(sequence.value->diagnostics,
                                             [](const std::string& line) {
                                                 return line == "Frame 1 has 2 files; using "
                                                                "dup.1.png and leaving out "
                                                                "dup.01.png";
                                             }),
                     "duplicate frame numbers keep the file written like the picked one");
        const auto picked = media::scanSequence(scratch.file("dup.01.png"));
        check.expect(picked.value.has_value() && picked.value->members.size() == 2 &&
                         picked.value->members.front().path.filename() == "dup.01.png",
                     "duplicate frame numbers keep the picked file");
        check.expect(media::countSequenceMembers(scratch.file("dup.2.png")) == 2U,
                     "duplicate frame numbers count once");
    }
    for (std::size_t size = 1; size < 64; ++size) {
        const auto path = scratch.file("hostile.png");
        {
            std::ofstream file(path, std::ios::binary);
            const std::string bytes(size, static_cast<char>(size));
            file.write(bytes.data(), static_cast<std::streamsize>(size));
        }
        check.expect(!media::decodeImage(path), "hostile random short payload rejected");
    }
    return check.failures() == 0 ? 0 : 1;
}
