#include "dpx_decoder.hpp"
#include "exr_backend.hpp"
#include "image_budget.hpp"
#include "image_work_statistics.hpp"
#include "stb_image_adapter.hpp"
#include "tiff_decoder.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_input_processor.hpp>
#include <bloom/core/file_uri.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/media/image.hpp>
#include <bloom/platform/source_identity.hpp>
#include <cmath>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

namespace bloom::media {
namespace {
[[nodiscard]] bool cancelled(const CancelImageWork& cancel) { return cancel && cancel(); }

constexpr std::size_t kImageHeaderBytes = 16;
constexpr std::size_t kHashChunkBytes = std::size_t{1024} * 1024U;
// PNG/JPEG peak admission phases: the decoded RGBA32F image (16 bytes/px), the codec's RGBA16
// staging buffer (4 channels * 2 bytes = 8 bytes/px), and the one-row float conversion scratch
// (16 bytes/px). Every phase that is live at once must fit the caller's explicit budget before the
// parser runs.
constexpr std::uint64_t kRgba32fBytesPerPixel = sizeof(render::Rgba32f);
constexpr std::uint64_t kPngJpegStagingBytesPerPixel = 8U;

// Streams the whole file through SHA-256 in bounded chunks. There is no whole-file cap: the hash is
// the content identity, the working set is one chunk, and cancellation is polled per chunk.
// Identity-verified digest: a full hash only when the file's identity is unknown or changed.
[[nodiscard]] ImageResult<core::Sha256Digest> hashFile(const std::filesystem::path& path,
                                                       const CancelImageWork& cancel) {
    if (cancelled(cancel))
        return {{}, {}, true};
    const auto hashed = platform::digestSourceFile(path, cancel);
    switch (hashed.error) {
    case platform::SourceHashError::None:
        return {hashed.digest, {}};
    case platform::SourceHashError::Cancelled:
        return {{}, {}, true};
    case platform::SourceHashError::Missing:
        return {
            {}, "Image file is missing or unreadable", false, ImageDiagnosticCode::FileUnavailable};
    case platform::SourceHashError::Changed:
        return {
            {}, "Image source changed while hashing", false, ImageDiagnosticCode::FileReadFailed};
    case platform::SourceHashError::Oversized:
    case platform::SourceHashError::Io:
        break;
    }
    return {{}, "Image read failed", false, ImageDiagnosticCode::FileReadFailed};
}

[[nodiscard]] bool readHeader(const std::filesystem::path& path, const std::span<std::byte> head,
                              std::size_t& bytesRead) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    input.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    bytesRead = static_cast<std::size_t>(input.gcount());
    return !input.bad();
}

// A codec-level geometry check only: dimensions must be positive and their RGBA32F byte count must
// be representable. The caller's pixelBudget decides how large a decode is admitted.
[[nodiscard]] bool validInfo(const detail::ImageInfo& info) {
    if (info.width <= 0 || info.height <= 0)
        return false;
    const auto pixels =
        static_cast<std::uint64_t>(info.width) * static_cast<std::uint64_t>(info.height);
    return pixels <= std::numeric_limits<std::size_t>::max() / kRgba32fBytesPerPixel;
}

[[nodiscard]] bool isExrMagic(const std::span<const std::byte> bytes) {
    return bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 0x76U &&
           static_cast<unsigned char>(bytes[1]) == 0x2fU &&
           static_cast<unsigned char>(bytes[2]) == 0x31U &&
           static_cast<unsigned char>(bytes[3]) == 0x01U;
}

[[nodiscard]] bool isPngMagic(const std::span<const std::byte> bytes) {
    return bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 137U &&
           static_cast<unsigned char>(bytes[1]) == 'P' &&
           static_cast<unsigned char>(bytes[2]) == 'N' &&
           static_cast<unsigned char>(bytes[3]) == 'G';
}

[[nodiscard]] ImageFormat stillFormat(const std::span<const std::byte> bytes) {
    if (detail::isTiffMagic(bytes))
        return ImageFormat::Tiff;
    if (detail::isDpxMagic(bytes))
        return ImageFormat::Dpx;
    return isPngMagic(bytes) ? ImageFormat::Png : ImageFormat::Jpeg;
}

// The plain-language reason a still header was refused: a TIFF's or DPX's own reason, else the
// codec's.
[[nodiscard]] std::string invalidStillMessage(const detail::ImageInfo& info) {
    if (!info.refusal.empty())
        return info.refusal;
    if (info.tiff || info.dpx)
        return "The image file is damaged or truncated";
    return "Invalid PNG/JPEG header or image dimensions";
}

[[nodiscard]] std::string decodeFailedMessage(const detail::ImageInfo& info) {
    return info.tiff || info.dpx
               ? "The image data is damaged, truncated or exceeds the pixel budget"
               : "PNG/JPEG decode failed or exceeded the pixel budget";
}

// ADR 0026: a still has one unnamed part whose channels are its decoded R, G, B and, when the file
// carries alpha, A (see decodeImageChannels).
[[nodiscard]] ImagePartInfo stillPart(const detail::ImageInfo& info) {
    ImagePartInfo part;
    part.dataWidth = part.displayWidth = static_cast<std::uint32_t>(info.width);
    part.dataHeight = part.displayHeight = static_cast<std::uint32_t>(info.height);
    part.alphaAssociation = ImageAlphaAssociation::Straight;
    part.colorSpaceTag = "srgb_rec709_display";
    for (const auto* name : {"R", "G", "B", "A"}) {
        if (std::string_view(name) == "A" && info.channels != 2 && info.channels != 4)
            continue;
        part.channels.push_back({name, ImageChannelSampleKind::Float});
    }
    part.layers = {std::string{}};
    part.colourLayers = {std::string{}};
    return part;
}

[[nodiscard]] ImageProbe stillProbe(const detail::ImageInfo& info, const core::Sha256Digest digest,
                                    const ImageFormat format) {
    ImageProbe probe{static_cast<std::uint32_t>(info.width),
                     static_cast<std::uint32_t>(info.height),
                     static_cast<std::uint8_t>(info.sixteenBit ? 16 : 8),
                     digest,
                     format,
                     ImageColorSpace::Srgb,
                     ImageAlphaAssociation::Straight,
                     "srgb_rec709_display",
                     {}};
    probe.parts.push_back(stillPart(info));
    if (info.dpx) {
        // The header's transfer and colorimetric codes decide the colour space (runtime Auto rule);
        // the part tag stays honest about it rather than claiming sRGB.
        probe.bitDepth = info.dpx->bitDepth;
        probe.colorSpace = ImageColorSpace::Auto;
        probe.colorSpaceTag = "dpx-transfer-" + std::to_string(info.dpx->transfer) +
                              "-colorimetric-" + std::to_string(info.dpx->colorimetric);
        probe.parts.front().colorSpaceTag = probe.colorSpaceTag;
        probe.dpx = info.dpx;
    }
    return probe;
}

template <typename T> [[nodiscard]] ImageResult<T> encoderMissing() {
    return {{},
            "TIFF export is not available in this build",
            false,
            ImageDiagnosticCode::ProviderMissing};
}

} // namespace
std::filesystem::path resolveImagePath(std::string_view relativePath, std::string_view relinkHint,
                                       const std::filesystem::path& projectDirectory) {
    // A project-relative path cannot contain ':'. An external locator stores its absolute URI
    // directly in path, so it does not depend on the project directory or a fallback hint.
    if (relativePath.find(':') != std::string_view::npos)
        return core::absolutePathFromFileUri(relativePath).value_or(std::filesystem::path{});
    if (relativePath.empty() || relativePath.front() == '/' ||
        relativePath.find('\\') != std::string_view::npos ||
        relativePath.find('\0') != std::string_view::npos)
        return {};
    const auto relative = std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(relativePath.data()), relativePath.size()));
    if (!projectDirectory.empty()) {
        const auto candidate = projectDirectory / relative;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return core::absolutePathFromFileUri(relinkHint).value_or(std::filesystem::path{});
}
namespace {
// A capture admitted by its identity alone, with no recorded digest to stand for (a stamped member
// still pending its background digest): an identity for these bytes in this process, never a
// content digest. Path and the full file identity, so two files never share it.
core::Sha256Digest unverifiedCaptureIdentity(const std::filesystem::path& path,
                                             const platform::FileIdentity& identity) {
    core::Sha256Hasher hasher;
    const auto text = path.generic_u8string();
    std::string record = "BloomUnverifiedCapture";
    record.push_back('\0');
    record.append(reinterpret_cast<const char*>(text.data()), text.size());
    for (const auto value :
         {identity.size, static_cast<std::uint64_t>(identity.modifiedNanoseconds),
          static_cast<std::uint64_t>(identity.changedNanoseconds), identity.volume,
          identity.fileIdHigh, identity.fileIdLow}) {
        record.push_back('\0');
        record += std::to_string(value);
    }
    (void)hasher.update(std::as_bytes(std::span(record.data(), record.size())));
    return hasher.finalize();
}
} // namespace
std::optional<ImageSourceStamp> observeImageSourceStamp(const std::filesystem::path& path) {
    const auto identity = platform::observeFileIdentity(path);
    if (!identity)
        return std::nullopt;
    return ImageSourceStamp{identity->size, identity->modifiedNanoseconds};
}
ImageResult<std::shared_ptr<const ImageSourceRevision>> captureImageSourceRevision(
    const std::filesystem::path& path, const std::size_t hostByteBudget,
    const CancelImageWork& cancel, const std::optional<core::Sha256Digest> expectedDigest,
    const std::optional<ImageSourceStamp> expectedStamp, const platform::SourceStoreAccess& store) {
    try {
        if (cancelled(cancel))
            return {{}, {}, true, ImageDiagnosticCode::Cancelled};
        ImageSourceCaptureStatistics statistics;
        auto ioStart = detail::ImageWorkClock::now();
        const auto observedBefore = platform::observeFileIdentity(path);
        const auto stampOf = [](const platform::FileIdentity& identity) {
            return ImageSourceStamp{identity.size, identity.modifiedNanoseconds};
        };
        const auto changedSinceImport = [] {
            return ImageResult<std::shared_ptr<const ImageSourceRevision>>{
                {},
                "Image file changed since it was imported; reload the footage",
                false,
                ImageDiagnosticCode::DigestMismatch};
        };
        // ADR 0025 (amended 2026-11-04): a changed identity forces a full hash. A stamped member
        // without a digest has nothing to compare, so no byte is read from a file that is no
        // longer the one import stamped (#473); a verified member (stamp and digest, document
        // 1.36) is rehashed and admitted only when its bytes still hash to that digest.
        const bool stampChanged =
            expectedStamp && observedBefore && stampOf(*observedBefore) != *expectedStamp;
        if (stampChanged && !expectedDigest)
            return changedSinceImport();
        // #718: the local mirror of slow sources is consulted before the source is read. One
        // identity observation of the original (above) stands against the mirror's entry; a match
        // yields the very bytes the source held under that identity, with the digest the mirror
        // just verified them against, and the source is not read at all.
        std::vector<std::byte> bytes;
        std::optional<platform::FileIdentity> observedAfter;
        std::optional<core::Sha256Digest> storedDigest;
        const bool mirrored = store.store != nullptr && observedBefore &&
                              store.store->keeps(path, *observedBefore, store.asset);
        if (mirrored && !stampChanged) {
            if (auto stored = store.store->find(path, *observedBefore, cancel)) {
                constexpr auto storedOverhead = sizeof(ImageSourceRevision) + std::size_t{512};
                if (hostByteBudget < storedOverhead ||
                    stored->bytes.size() > hostByteBudget - storedOverhead)
                    return {{},
                            "Image source capture exceeds the host byte budget",
                            false,
                            ImageDiagnosticCode::PixelBudgetExceeded};
                bytes = std::move(stored->bytes);
                storedDigest = stored->digest;
                observedAfter = observedBefore;
            } else if (cancelled(cancel)) {
                return {{}, {}, true, ImageDiagnosticCode::Cancelled};
            }
        }
        if (!storedDigest) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input)
                return {{},
                        "Image file is missing or unreadable",
                        false,
                        ImageDiagnosticCode::FileUnavailable};
            const auto length = input.tellg();
            detail::addImageWork(statistics.ioNanoseconds, detail::imageWorkNanoseconds(ioStart));
            if (length < 0)
                return {
                    {}, "Image size could not be read", false, ImageDiagnosticCode::FileReadFailed};
            // File size admits a capture, never proves an unchanged revision. Decode reads only
            // these owned bytes, even if the path is concurrently replaced: old and new bytes never
            // mix.
            constexpr auto overhead = sizeof(ImageSourceRevision) + std::size_t{512};
            const auto lengthBytes = static_cast<std::uint64_t>(length);
            if (hostByteBudget < overhead || lengthBytes > hostByteBudget - overhead)
                return {{},
                        "Image source capture exceeds the host byte budget",
                        false,
                        ImageDiagnosticCode::PixelBudgetExceeded};
            bytes.resize(static_cast<std::size_t>(lengthBytes));
            ioStart = detail::ImageWorkClock::now();
            input.seekg(0);
            detail::addImageWork(statistics.ioNanoseconds, detail::imageWorkNanoseconds(ioStart));
            for (std::size_t offset = 0; offset < bytes.size();) {
                if (cancelled(cancel))
                    return {{}, {}, true, ImageDiagnosticCode::Cancelled};
                const auto count = std::min(kHashChunkBytes, bytes.size() - offset);
                ioStart = detail::ImageWorkClock::now();
                input.read(reinterpret_cast<char*>(bytes.data() + offset),
                           static_cast<std::streamsize>(count));
                detail::addImageWork(statistics.ioNanoseconds,
                                     detail::imageWorkNanoseconds(ioStart));
                if (input.gcount() != static_cast<std::streamsize>(count))
                    return {{},
                            "Image source changed or failed during capture",
                            false,
                            ImageDiagnosticCode::FileReadFailed};
                detail::addImageWork(statistics.bytesRead, count);
                offset += count;
            }
            ioStart = detail::ImageWorkClock::now();
            const auto trailing = input.peek();
            detail::addImageWork(statistics.ioNanoseconds, detail::imageWorkNanoseconds(ioStart));
            if (trailing != std::char_traits<char>::eof() || input.bad())
                return {{},
                        "Image source changed during capture",
                        false,
                        ImageDiagnosticCode::FileReadFailed};
            if (cancelled(cancel))
                return {{}, {}, true, ImageDiagnosticCode::Cancelled};
            platform::recordFullSourceRead(bytes.size());
            observedAfter = platform::observeFileIdentity(path);
        }
        const bool unchanged = observedBefore && observedAfter &&
                               *observedBefore == *observedAfter &&
                               observedAfter->size == bytes.size();
        const auto hashBytes = [&] {
            const auto hashStart = detail::ImageWorkClock::now();
            core::Sha256Hasher hasher;
            if (!hasher.update(std::span<const std::byte>(bytes)))
                throw std::bad_alloc();
            const auto value = hasher.finalize();
            detail::addImageWork(statistics.hashNanoseconds,
                                 detail::imageWorkNanoseconds(hashStart));
            detail::addImageWork(statistics.bytesHashed, bytes.size());
            if (unchanged)
                platform::verifiedSourceDigests().record(path, *observedAfter, value);
            return value;
        };
        // What admits these bytes, without hashing them when nothing changed:
        // - a full hash the process memo recorded for this exact identity (a refuting memo digest
        //   reports the relink diagnostic);
        // - a stamped member whose file carried its stamp across the whole read;
        // - otherwise the identity bracket alone (the recorded digest is verified by the
        //   background "Verify footage" pass, which refutes a changed file).
        // A changed identity, during the read or against the stamp, forces the hash.
        std::optional<core::Sha256Digest> digest;
        bool verified = false;
        // The memo knowing this path under another identity is a changed identity too.
        const auto known =
            unchanged ? platform::verifiedSourceDigests().knownIdentity(path, &*observedAfter)
                      : std::nullopt;
        const bool identityChanged = known && *known != *observedAfter;
        if (storedDigest) {
            // The mirror hashed these bytes on this read: they are verified content, and the
            // identity that found them now vouches for that digest in the process memo.
            digest = storedDigest;
            verified = true;
            platform::verifiedSourceDigests().record(path, *observedAfter, *storedDigest);
        }
        if (!digest && unchanged && !stampChanged)
            if (const auto memo = platform::verifiedSourceDigests().find(path, *observedAfter)) {
                digest = *memo;
                verified = true;
            }
        if (!digest && (stampChanged || !unchanged || identityChanged)) {
            if (expectedStamp && !expectedDigest && !unchanged)
                return changedSinceImport();
            digest = hashBytes();
            verified = true;
        }
        if (expectedStamp && !expectedDigest && stampOf(*observedAfter) != *expectedStamp)
            return changedSinceImport();
        if (expectedDigest && digest && *digest != *expectedDigest)
            return {{},
                    "Image content changed; relink the asset",
                    false,
                    ImageDiagnosticCode::DigestMismatch};
        if (!digest) {
            platform::verifiedSourceDigests().admit(path, *observedAfter);
            digest =
                expectedDigest ? *expectedDigest : unverifiedCaptureIdentity(path, *observedAfter);
        }
        const auto probeStart = detail::ImageWorkClock::now();
        ImageResult<ImageProbe> probe;
        const auto view = std::span<const std::byte>(bytes);
        if (isExrMagic(view)) {
            probe = detail::probeExr(view, *digest, cancel);
        } else {
            detail::ImageInfo info;
            if (!detail::imageInfo(view, info) || !validInfo(info))
                return {{}, invalidStillMessage(info), false, ImageDiagnosticCode::InvalidHeader};
            probe = {stillProbe(info, *digest, stillFormat(view)), {}};
        }
        detail::addImageWork(statistics.probeNanoseconds, detail::imageWorkNanoseconds(probeStart));
        if (!probe.value.has_value())
            return {{}, probe.diagnostic, probe.cancelled, probe.code};
        auto revision = std::shared_ptr<const ImageSourceRevision>(
            new ImageSourceRevision(std::move(bytes), std::move(*probe.value), statistics,
                                    unchanged ? observedAfter : std::nullopt, verified));
        if (revision->retainedBytes() > hostByteBudget)
            return {{},
                    "Image source metadata exceeds the host byte budget",
                    false,
                    ImageDiagnosticCode::PixelBudgetExceeded};
        // Write-through (#718): the bytes this read brought from the source are already on
        // their way to the decoder; the mirror keeps the same bytes in the background. Only a
        // read whose identity held across it is kept, so the copy is the file under that
        // identity.
        if (mirrored && !storedDigest && unchanged)
            store.store->keep(path, *observedAfter, revision, revision->bytes());
        return {std::move(revision), {}};
    } catch (const std::bad_alloc&) {
        return {{},
                "Image source capture allocation failed",
                false,
                ImageDiagnosticCode::AllocationFailure};
    } catch (...) {
        return {{}, "Image source capture I/O failed", false, ImageDiagnosticCode::FileReadFailed};
    }
}

ImageResult<ImageProbe> probeImage(const std::filesystem::path& path, const CancelImageWork& cancel,
                                   const ImageProvider* /*unused: TIFF is decoded natively*/) {
    try {
        auto digest = hashFile(path, cancel);
        if (!digest.value.has_value())
            return {{}, digest.diagnostic, digest.cancelled};
        std::array<std::byte, kImageHeaderBytes> head{};
        std::size_t headBytes = 0;
        if (!readHeader(path, head, headBytes))
            return {{},
                    "Image file is missing or unreadable",
                    false,
                    ImageDiagnosticCode::FileUnavailable};
        const auto view = std::span<const std::byte>(head.data(), headBytes);
        if (isExrMagic(view))
            return detail::probeExr(path, *digest.value, cancel);
        detail::ImageInfo info;
        if (!detail::imageInfo(path, info) || !validInfo(info))
            return {{}, invalidStillMessage(info), false, ImageDiagnosticCode::InvalidHeader};
        return {stillProbe(info, *digest.value, stillFormat(view)), {}};
    } catch (const std::exception&) {
        return {{}, "Image probe allocation or I/O failed"};
    }
}
float storedSampleAlpha(const float alpha) noexcept { return std::fma(alpha, 0.5F, 0.5F); }

static ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImageImpl(const std::filesystem::path& path, const ImageSourceRevision* revision,
                const ImageInterpretation& interpretation,
                std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
                const CancelImageWork& cancel, const ImageProgress& progress,
                std::size_t pixelBudget, std::optional<core::Sha256Digest> expectedDigest,
                ImageChannelsAlongside* alongside = nullptr) {
    try {
        if (progress)
            progress(0, 0);
        auto digest = revision
                          ? ImageResult<core::Sha256Digest>{revision->probe().contentDigest, {}}
                          : hashFile(path, cancel);
        if (!digest.value.has_value())
            return {{}, digest.diagnostic, digest.cancelled};
        if (expectedDigest && *digest.value != *expectedDigest)
            return {{},
                    "Image content changed; relink the asset",
                    false,
                    ImageDiagnosticCode::DigestMismatch};
        std::array<std::byte, kImageHeaderBytes> head{};
        std::size_t headBytes = 0;
        if (revision) {
            headBytes = std::min(head.size(), revision->bytes().size());
            std::copy_n(revision->bytes().begin(), headBytes, head.begin());
        }
        if (!revision && !readHeader(path, head, headBytes))
            return {{},
                    "Image file is missing or unreadable",
                    false,
                    ImageDiagnosticCode::FileUnavailable};
        const auto view = std::span<const std::byte>(head.data(), headBytes);
        if (isExrMagic(view)) {
            if (revision)
                return detail::decodeExr(revision->bytes(), interpretation, std::move(processor),
                                         cancel, progress, pixelBudget, alongside);
            return detail::decodeExr(path, interpretation, std::move(processor), cancel, progress,
                                     pixelBudget);
        }
        detail::ImageInfo info;
        if (!(revision ? detail::imageInfo(revision->bytes(), info)
                       : detail::imageInfo(path, info)) ||
            !validInfo(info))
            return {{}, invalidStillMessage(info), false, ImageDiagnosticCode::InvalidHeader};
        const auto width = static_cast<std::uint64_t>(info.width);
        const auto height = static_cast<std::uint64_t>(info.height);
        std::uint64_t pixels = 0;
        std::uint64_t finalBytes = 0;
        std::uint64_t stagingBytes = 0;
        std::uint64_t rowBytes = 0;
        const bool sized =
            detail::checkedSizeProduct(width, height, pixels) &&
            detail::checkedSizeProduct(pixels, kRgba32fBytesPerPixel, finalBytes) &&
            detail::checkedSizeProduct(pixels, kPngJpegStagingBytesPerPixel, stagingBytes) &&
            detail::checkedSizeProduct(width, sizeof(std::array<float, 4>), rowBytes);
        if (!sized ||
            !detail::decodeWorkingSetFits(finalBytes, stagingBytes, rowBytes, pixelBudget))
            return {{},
                    "Decoded image exceeds the pixel storage budget",
                    false,
                    ImageDiagnosticCode::PixelBudgetExceeded};
        if (cancelled(cancel))
            return {{}, {}, true};
        auto samples = revision ? detail::imageSamples(revision->bytes(), info, pixelBudget)
                                : detail::imageSamples(path, info, pixelBudget);
        if (samples.empty())
            return {{}, decodeFailedMessage(info), false, ImageDiagnosticCode::DecodeFailed};
        if (cancelled(cancel))
            return {{}, {}, true};
        const bool stored = interpretation.storedSamples;
        const bool convert =
            !stored && (!interpretation.inputColorSpaceId.empty() ||
                        interpretation.colorSpace == ImageColorSpace::Auto ||
                        interpretation.colorSpace == ImageColorSpace::Srgb || processor != nullptr);
        std::shared_ptr<const color::CpuInputProcessor> compatibilityProcessor;
        if (convert && !processor) {
            const auto resolved = color::resolveBloomNeutralV1BuiltIn(
                color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                color::kBloomNeutralV1ConfigDigest);
            if (!resolved)
                return {{}, "Bloom Neutral input config could not be resolved"};
            if (interpretation.inputColorSpaceId.empty()) {
                // Direct media callers from the 1.19 API do not carry a project config. Preserve
                // their exact Bloom Neutral default while all project-managed paths supply the
                // general config processor explicitly.
                compatibilityProcessor = color::CpuInputProcessor::prepare(*resolved.resolved());
            } else {
                auto prepared = color::CpuColorSpaceProcessor::prepare(
                    *resolved.resolved(), interpretation.inputColorSpaceId,
                    resolved.resolved()->processColorSpaceId());
                if (!prepared)
                    return {{},
                            "Input color space is unavailable in the selected config",
                            false,
                            ImageDiagnosticCode::ColorSpaceUnavailable};
                processor = std::move(prepared).takeProcessor();
            }
        }
        if (convert && !processor && !compatibilityProcessor)
            return {{},
                    "Input color processor could not be prepared",
                    false,
                    ImageDiagnosticCode::ColorSpaceUnavailable};
        const auto window = render::ImageWindow::create(0, 0, width, height);
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *window.value(), *window.value(), core::PixelAspectRatio::square());
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), pixelBudget);
        if (!builder)
            return {{}, "Process image allocation failed"};
        std::vector<std::array<float, 4>> row(static_cast<std::size_t>(width));
        for (std::uint64_t y = 0; y < height; ++y) {
            if (cancelled(cancel))
                return {{}, {}, true};
            for (std::uint64_t x = 0; x < width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)) *
                                    4;
                for (std::size_t c = 0; c < 4; ++c)
                    row[static_cast<std::size_t>(x)][c] =
                        static_cast<float>(samples[offset + c]) / 65535.0F;
                if (!stored &&
                    interpretation.alphaAssociation == ImageAlphaAssociation::Premultiplied) {
                    for (std::size_t c = 0; c < 3; ++c)
                        row[static_cast<std::size_t>(x)][c] =
                            row[static_cast<std::size_t>(x)][3] > 0.0F
                                ? row[static_cast<std::size_t>(x)][c] /
                                      row[static_cast<std::size_t>(x)][3]
                                : 0.0F;
                }
            }
            if (convert && ((processor && !processor->apply(row)) ||
                            (compatibilityProcessor && !compatibilityProcessor->apply(row))))
                return {{},
                        "Input color conversion failed",
                        false,
                        ImageDiagnosticCode::ColorSpaceUnavailable};
            auto outputRow = builder.value()->row(static_cast<std::int64_t>(y));
            for (std::uint64_t x = 0; x < width; ++x) {
                const auto& sample = row[static_cast<std::size_t>(x)];
                auto pixel =
                    stored ? render::Rgba32f::fromPremultiplied(sample[0], sample[1], sample[2],
                                                                storedSampleAlpha(sample[3]))
                           : render::Rgba32f::fromPremultiplied(sample[0] * sample[3],
                                                                sample[1] * sample[3],
                                                                sample[2] * sample[3], sample[3]);
                if (!pixel)
                    return {{}, "Input color conversion produced an invalid pixel"};
                (*outputRow.value())[static_cast<std::size_t>(x)] = *pixel.value();
            }
            if (progress)
                progress(y + 1, height);
        }
        auto image = std::move(*builder.value()).freeze();
        if (!image)
            return {{}, "Process image validation failed"};
        return {std::make_shared<const render::Rgba32fImage>(std::move(*image.value())), {}};
    } catch (const std::exception&) {
        return {{}, "Image decode allocation or I/O failed"};
    }
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const std::filesystem::path& path, const ImageInterpretation& interpretation,
            std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
            const CancelImageWork& cancel, const ImageProgress& progress,
            const std::size_t pixelBudget, const std::optional<core::Sha256Digest> expectedDigest,
            const ImageProvider* /*unused: TIFF is decoded natively*/) {
    // The Profiler's decode stage (#423); a failed or cancelled decode records nothing.
    core::telemetry::StageTimer decodeTime(core::telemetry::Stage::Decode);
    auto decoded = decodeImageImpl(path, nullptr, interpretation, std::move(processor), cancel,
                                   progress, pixelBudget, expectedDigest);
    if (!decoded.value)
        decodeTime.cancel();
    return decoded;
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const ImageSourceRevision& revision, const ImageInterpretation& interpretation,
            std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
            const CancelImageWork& cancel, const ImageProgress& progress,
            const std::size_t hostByteBudget, ImageDecodeStatistics* statistics,
            ImageChannelsAlongside* alongside) {
    if (statistics)
        *statistics = {};
    if (alongside)
        alongside->decoded.reset();
    if (cancelled(cancel))
        return {{}, {}, true, ImageDiagnosticCode::Cancelled};
    if (revision.retainedBytes() >= hostByteBudget)
        return {{},
                "Image revision and decode exceed the host byte budget",
                false,
                ImageDiagnosticCode::PixelBudgetExceeded};
    core::telemetry::StageTimer decodeTime(core::telemetry::Stage::Decode);
    auto decoded = [&] {
        const detail::ImageWorkTimer timer(statistics ? &statistics->decodeNanoseconds : nullptr);
        return decodeImageImpl({}, &revision, interpretation, std::move(processor), cancel,
                               progress, hostByteBudget - revision.retainedBytes(),
                               revision.probe().contentDigest, alongside);
    }();
    if (!decoded.value)
        decodeTime.cancel();
    else
        decodeTime.end();
    if (statistics)
        statistics->repairedPixels = decoded.repairedPixels;
    return decoded;
}

// #422: a still's decoded samples in their native layout for the GPU route. PNG and JPEG keep the
// decoder's 16-bit RGBA samples and an all-half OpenEXR colour layer its binary16 samples; the
// SampleConvertV1 parameters reproduce decodeImage's raw conversion of the same interpretation.
static ImageResult<std::shared_ptr<const render::NativeSampleImage>>
decodeImageSamplesImpl(const ImageSourceRevision& revision,
                       const ImageInterpretation& interpretation, const CancelImageWork& cancel,
                       const std::size_t pixelBudget, ImageChannelsAlongside* alongside) {
    using Decoded = std::shared_ptr<const render::NativeSampleImage>;
    static_assert(std::endian::native == std::endian::little,
                  "native samples are uploaded as little-endian words");
    const auto unavailable = [](std::string detail) {
        return ImageResult<Decoded>{
            {}, std::move(detail), false, ImageDiagnosticCode::NativeSamplesUnavailable};
    };
    // Only the raw decode (no colour conversion) has a native-sample form.
    if (interpretation.colorSpace != ImageColorSpace::Raw ||
        !interpretation.inputColorSpaceId.empty())
        return unavailable("Only a raw decode has native samples");
    const auto bytes = revision.bytes();
    const auto view = bytes.first(std::min(bytes.size(), kImageHeaderBytes));
    render::NativeSampleConversion conversion;
    conversion.storedSamples = interpretation.storedSamples;
    if (isExrMagic(view)) {
        auto decoded =
            detail::decodeExrHalfSamples(bytes, interpretation, cancel, pixelBudget, alongside);
        if (!decoded.value)
            return {{}, decoded.diagnostic, decoded.cancelled, decoded.code};
        const auto& layout = decoded.value->layout;
        const auto data =
            render::ImageWindow::create(layout.dataX, layout.dataY, layout.width, layout.height);
        const auto display = render::ImageWindow::create(layout.displayX, layout.displayY,
                                                         layout.displayWidth, layout.displayHeight);
        if (!data || !display)
            return {{},
                    "OpenEXR image window is invalid",
                    false,
                    ImageDiagnosticCode::InvalidDataWindow};
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *data.value(), *display.value(), core::PixelAspectRatio::square());
        if (!descriptor)
            return {{},
                    "OpenEXR image descriptor is invalid",
                    false,
                    ImageDiagnosticCode::InvalidDataWindow};
        conversion.layout = render::NativeSampleLayout::RgbaHalf;
        conversion.alpha = layout.premultiplied ? render::NativeSampleAlpha::Premultiplied
                                                : render::NativeSampleAlpha::Straight;
        conversion.luminance = layout.luminance;
        conversion.hasAlpha = layout.hasAlpha;
        const auto& storage = decoded.value->bytes;
        const render::NativeSamplePlane plane{std::span<const std::byte>(*storage),
                                              static_cast<std::uint32_t>(layout.width),
                                              static_cast<std::uint32_t>(layout.height),
                                              static_cast<std::uint32_t>(layout.width * 8U)};
        auto samples = render::NativeSampleImage::create(*descriptor.value(), conversion,
                                                         std::span(&plane, 1), storage);
        if (!samples)
            return {{},
                    "OpenEXR samples do not match their layout",
                    false,
                    ImageDiagnosticCode::DecodeFailed};
        ImageResult<Decoded> result{*samples.value(), {}};
        result.repairedPixels = decoded.value->repairedPixels;
        return result;
    }
    detail::ImageInfo info;
    if (!detail::imageInfo(bytes, info) || !validInfo(info))
        return {{}, invalidStillMessage(info), false, ImageDiagnosticCode::InvalidHeader};
    if (cancelled(cancel))
        return {{}, {}, true, ImageDiagnosticCode::Cancelled};
    auto decodedSamples = detail::imageSamples(bytes, info, pixelBudget);
    if (decodedSamples.empty())
        return {{}, decodeFailedMessage(info), false, ImageDiagnosticCode::DecodeFailed};
    if (cancelled(cancel))
        return {{}, {}, true, ImageDiagnosticCode::Cancelled};
    const auto width = static_cast<std::uint32_t>(info.width);
    const auto height = static_cast<std::uint32_t>(info.height);
    const auto window = render::ImageWindow::create(0, 0, width, height);
    if (!window)
        return {{}, "Invalid PNG/JPEG image dimensions", false, ImageDiagnosticCode::InvalidHeader};
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), core::PixelAspectRatio::square());
    if (!descriptor)
        return {{}, "Invalid PNG/JPEG image dimensions", false, ImageDiagnosticCode::InvalidHeader};
    // The raw decode unpremultiplies a still declared premultiplied before associating alpha.
    conversion.layout = render::NativeSampleLayout::Rgba16;
    conversion.alpha = interpretation.alphaAssociation == ImageAlphaAssociation::Premultiplied
                           ? render::NativeSampleAlpha::Unpremultiply
                           : render::NativeSampleAlpha::Straight;
    auto storage = std::make_shared<const std::vector<std::uint16_t>>(std::move(decodedSamples));
    const render::NativeSamplePlane plane{std::as_bytes(std::span<const std::uint16_t>(*storage)),
                                          width, height, width * 8U};
    auto samples = render::NativeSampleImage::create(*descriptor.value(), conversion,
                                                     std::span(&plane, 1), storage);
    if (!samples)
        return {{},
                "PNG/JPEG samples do not match their layout",
                false,
                ImageDiagnosticCode::DecodeFailed};
    return {*samples.value(), {}};
}

ImageResult<std::shared_ptr<const render::NativeSampleImage>>
decodeImageSamples(const ImageSourceRevision& revision, const ImageInterpretation& interpretation,
                   const CancelImageWork& cancel, const std::size_t hostByteBudget,
                   ImageDecodeStatistics* statistics, ImageChannelsAlongside* alongside) {
    if (statistics)
        *statistics = {};
    if (alongside)
        alongside->decoded.reset();
    if (cancelled(cancel))
        return {{}, {}, true, ImageDiagnosticCode::Cancelled};
    if (revision.retainedBytes() >= hostByteBudget)
        return {{},
                "Image revision and decode exceed the host byte budget",
                false,
                ImageDiagnosticCode::PixelBudgetExceeded};
    core::telemetry::StageTimer decodeTime(core::telemetry::Stage::Decode);
    auto decoded = [&] {
        const detail::ImageWorkTimer timer(statistics ? &statistics->decodeNanoseconds : nullptr);
        try {
            return decodeImageSamplesImpl(revision, interpretation, cancel,
                                          hostByteBudget - revision.retainedBytes(), alongside);
        } catch (const std::exception&) {
            return ImageResult<std::shared_ptr<const render::NativeSampleImage>>{
                {},
                "Image decode allocation or I/O failed",
                false,
                ImageDiagnosticCode::AllocationFailure};
        }
    }();
    if (!decoded.value)
        decodeTime.cancel();
    else
        decodeTime.end();
    if (statistics)
        statistics->repairedPixels = decoded.repairedPixels;
    return decoded;
}

// PNG/JPEG channel sets (owner decision for ADR 0026): the decoded R, G, B and, when the file
// carries alpha, A as straight Colour channels in [0, 1], recorded with the asset's input colour
// space. Grey files expose R = G = B.
static ImageResult<std::shared_ptr<const render::ChannelSetImage>>
decodeStillChannels(const ImageSourceRevision& revision, const ImageChannelSelection& selection,
                    const CancelImageWork& cancel, const ImageProgress& progress,
                    const std::size_t budget) {
    try {
        if (!selection.part.empty())
            return {{}, "Still images have one part", false, ImageDiagnosticCode::PartNotFound};
        detail::ImageInfo info;
        if (!detail::imageInfo(revision.bytes(), info) || !validInfo(info))
            return {{}, invalidStillMessage(info), false, ImageDiagnosticCode::InvalidHeader};
        const bool hasAlpha = info.channels == 2 || info.channels == 4;
        std::vector<std::string> available{"R", "G", "B"};
        if (hasAlpha)
            available.emplace_back("A");
        std::vector<std::size_t> picked;
        if (selection.channels.empty()) {
            for (std::size_t index = 0; index < available.size(); ++index)
                picked.push_back(index);
        } else {
            for (const auto& name : selection.channels) {
                const auto found = std::ranges::find(available, name);
                if (found == available.end())
                    return {{},
                            "A selected channel is not present",
                            false,
                            ImageDiagnosticCode::ChannelNotFound};
                const auto index = static_cast<std::size_t>(found - available.begin());
                if (std::ranges::find(picked, index) != picked.end())
                    return {{},
                            "A channel is selected more than once",
                            false,
                            ImageDiagnosticCode::UnsupportedChannels};
                picked.push_back(index);
            }
        }
        const auto width = static_cast<std::uint64_t>(info.width);
        const auto height = static_cast<std::uint64_t>(info.height);
        std::uint64_t pixels = 0;
        std::uint64_t planeBytes = 0;
        std::uint64_t stagingBytes = 0;
        if (!detail::checkedSizeProduct(width, height, pixels) ||
            !detail::checkedSizeProduct(pixels, picked.size() * sizeof(float), planeBytes) ||
            !detail::checkedSizeProduct(pixels, kPngJpegStagingBytesPerPixel, stagingBytes) ||
            !detail::decodeWorkingSetFits(planeBytes, stagingBytes, 0U, budget))
            return {{},
                    "Decoded channel set exceeds the pixel working-set budget",
                    false,
                    ImageDiagnosticCode::PixelBudgetExceeded};
        if (cancelled(cancel))
            return {{}, {}, true, ImageDiagnosticCode::Cancelled};
        if (progress)
            progress(0, height);
        // The parser's own admission covers final RGBA32F plus staging; hand it this request's
        // whole remaining budget so a small selection is not refused by a larger ceiling.
        const auto samples = detail::imageSamples(revision.bytes(), info, budget);
        if (samples.empty())
            return {{}, decodeFailedMessage(info), false, ImageDiagnosticCode::DecodeFailed};
        if (cancelled(cancel))
            return {{}, {}, true, ImageDiagnosticCode::Cancelled};
        std::vector<std::vector<float>> planes(picked.size());
        std::vector<render::ChannelDescriptor> descriptors;
        for (std::size_t plane = 0; plane < picked.size(); ++plane) {
            const auto component = picked[plane];
            planes[plane].resize(static_cast<std::size_t>(pixels));
            for (std::size_t pixel = 0; pixel < planes[plane].size(); ++pixel)
                planes[plane][pixel] =
                    static_cast<float>(samples[pixel * 4U + component]) / 65535.0F;
            auto descriptor =
                render::describeChannel(available[component], render::ChannelSampleKind::Float);
            descriptors.push_back(std::move(descriptor));
        }
        if (progress)
            progress(height, height);
        const auto window = render::ImageWindow::create(0, 0, width, height);
        if (!window)
            return {{}, "Image window is invalid", false, ImageDiagnosticCode::InvalidDataWindow};
        auto created = render::ChannelSetImage::create(
            *window.value(), *window.value(), core::PixelAspectRatio::square(),
            std::move(descriptors), std::move(planes), {}, std::numeric_limits<std::size_t>::max(),
            selection.inputColorSpaceId.empty() ? revision.probe().colorSpaceTag
                                                : selection.inputColorSpaceId);
        if (!created)
            return {{}, "Channel set validation failed", false, ImageDiagnosticCode::DecodeFailed};
        return {std::make_shared<const render::ChannelSetImage>(std::move(*created.value())), {}};
    } catch (const std::bad_alloc&) {
        return {{}, "Channel set allocation failed", false, ImageDiagnosticCode::AllocationFailure};
    } catch (const std::exception&) {
        return {{}, "Channel set decode failed", false, ImageDiagnosticCode::DecodeFailed};
    }
}

ImageResult<std::shared_ptr<const render::ChannelSetImage>>
decodeImageChannels(const ImageSourceRevision& revision, const ImageChannelSelection& selection,
                    const CancelImageWork& cancel, const ImageProgress& progress,
                    const std::size_t hostByteBudget, ImageDecodeStatistics* statistics) {
    if (statistics)
        *statistics = {};
    if (cancelled(cancel))
        return {{}, {}, true, ImageDiagnosticCode::Cancelled};
    const auto format = revision.probe().format;
    if (format != ImageFormat::Exr && format != ImageFormat::Png && format != ImageFormat::Jpeg &&
        format != ImageFormat::Tiff && format != ImageFormat::Dpx)
        return {{},
                "This image format has no channel-set decoder",
                false,
                ImageDiagnosticCode::UnsupportedChannelSetFormat};
    if (revision.retainedBytes() >= hostByteBudget)
        return {{},
                "Image revision and decode exceed the host byte budget",
                false,
                ImageDiagnosticCode::PixelBudgetExceeded};
    const detail::ImageWorkTimer timer(statistics ? &statistics->decodeNanoseconds : nullptr);
    const auto budget = hostByteBudget - revision.retainedBytes();
    if (format == ImageFormat::Exr)
        return detail::decodeExrChannels(revision.bytes(), selection, cancel, progress, budget);
    return decodeStillChannels(revision, selection, cancel, progress, budget);
}

ImageResult<ImageProviderEncodeResponse> encodeImage(const ImageProviderEncodeRequest& request,
                                                     const ImageProvider* provider) {
    if (provider == nullptr || !provider->encode)
        return encoderMissing<ImageProviderEncodeResponse>();
    try {
        const auto response = provider->encode(request);
        if (!response.value.has_value())
            return {{},
                    response.diagnostic,
                    response.cancelled,
                    response.code == ImageDiagnosticCode::None ? ImageDiagnosticCode::ProviderFailed
                                                               : response.code};
        if (!response.value->written)
            return {{},
                    "Image provider did not write the requested artifact",
                    false,
                    ImageDiagnosticCode::ProviderFailed};
        return {response.value, {}};
    } catch (...) {
        return {{}, "Image provider failed", false, ImageDiagnosticCode::ProviderFailed};
    }
}
} // namespace bloom::media
