#pragma once

#include <algorithm>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/render/channel_set_image.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/native_samples.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::media {
// Default decode request budget. This is a benign default for callers that do not pass their own
// budget. Decoded storage and the decoder's peak working set are admitted against the explicit
// caller-supplied pixelBudget; there is no separate absolute byte, pixel, or dimension ceiling.
// Geometry is bounded only by the codec's own representable window and by overflow/index checks.
inline constexpr std::size_t kMaxImageStorageBytes = 268435456;
// Bounds retained member metadata, not unrelated files in the directory or frame-number span.
inline constexpr std::size_t kMaxSequenceEntries = 100000;
using CancelImageWork = std::function<bool()>;
using ImageProgress = std::function<void(std::uint64_t, std::uint64_t)>;
template <typename T> struct ImageResult;
// #473: a file's length and modification time (nanoseconds since the Unix epoch), as
// platform::observeFileIdentity() reports them. A sequence member whose bytes import did not read
// is identified by its stamp until a read proves its bytes; a different stamp is a changed file.
struct ImageSourceStamp {
    std::uint64_t size = 0;
    std::int64_t modifiedNanoseconds = 0;
    friend bool operator==(const ImageSourceStamp&, const ImageSourceStamp&) = default;
};
// The stamp of `path` now (one stat, no read), or nullopt when it is missing or not a regular file.
[[nodiscard]] std::optional<ImageSourceStamp>
observeImageSourceStamp(const std::filesystem::path& path);
enum class ImageColorSpace : std::uint8_t { Auto, Srgb, Linear, Raw };
enum class ImageAlphaAssociation : std::uint8_t { Auto, Straight, Premultiplied };
struct ImageInterpretation {
    ImageColorSpace colorSpace = ImageColorSpace::Auto;
    std::string inputColorSpaceId;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Auto;
    // ADR 0026 selection. `part` names an OpenEXR part (empty: the first part); `layer` names the
    // layer whose R/G/B[/A] or Y[/A] channels form the colour image (empty: the root layer).
    std::string part{};
    std::string layer{};
    // Interpret Footage (#381): decode the file's stored channel values for the runtime's footage
    // interpretation pass instead of a process image. No color conversion and no alpha
    // association run; each pixel is (stored R, G, B, 0.5 + 0.5 * stored A), so a transparent
    // pixel keeps its stored RGB through the premultiplied image type (the pass decodes the
    // biased alpha). Only valid with the Raw color space. A provider-decoded file (TIFF) is
    // decoded premultiplied, so its stored RGB under zero alpha is not available.
    bool storedSamples = false;
    friend bool operator==(const ImageInterpretation&, const ImageInterpretation&) = default;
};
// The stored-samples alpha encoding (above): fma(alpha, 0.5, 0.5).
[[nodiscard]] float storedSampleAlpha(float alpha) noexcept;

enum class ImageFormat : std::uint8_t { Png, Jpeg, Exr, Tiff };

enum class ImageDiagnosticCode : std::uint8_t {
    None,
    FileUnavailable,
    FileTooLarge,
    FileReadFailed,
    InvalidMagic,
    InvalidHeader,
    UnsupportedVersion,
    UnsupportedDeepImage,
    UnsupportedMultipart,
    UnsupportedTiledLevels,
    UnsupportedChannels,
    UnsupportedChannelType,
    InvalidDataWindow,
    DimensionsExceeded,
    PixelBudgetExceeded,
    Truncated,
    DecodeFailed,
    AllocationFailure,
    DigestMismatch,
    ProviderMissing,
    ProviderFailed,
    ColorSpaceUnavailable,
    Cancelled,
    // ADR 0026 selection outcomes.
    PartNotFound,
    LayerNotFound,
    ChannelNotFound,
    NoColourChannels,
    UnsupportedChannelSetFormat,
    // #422: the source has no native-sample decode (TIFF, a FLOAT or UINT OpenEXR colour layer, a
    // converted interpretation); the float decode applies.
    NativeSamplesUnavailable,
};

using ImageChannelSampleKind = render::ChannelSampleKind;

struct ImageChannelInfo final {
    std::string name;
    ImageChannelSampleKind sampleKind = ImageChannelSampleKind::Half;
    friend bool operator==(const ImageChannelInfo&, const ImageChannelInfo&) = default;
};

// One OpenEXR part as enumerated by the probe (ADR 0026). Windows are in source pixel coordinates.
struct ImagePartInfo final {
    std::string name;
    std::string view;
    bool tiled = false;
    std::int64_t dataX = 0;
    std::int64_t dataY = 0;
    std::uint32_t dataWidth = 0;
    std::uint32_t dataHeight = 0;
    std::int64_t displayX = 0;
    std::int64_t displayY = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Premultiplied;
    std::string colorSpaceTag;
    // Header channel order (OpenEXR sorts names).
    std::vector<ImageChannelInfo> channels;
    // Distinct layer names in first-appearance order; "" is the root layer.
    std::vector<std::string> layers;
    // The subset of `layers` that carries R/G/B or Y colour channels.
    std::vector<std::string> colourLayers;
    // #478: an empty (default) layer selects the root colour layer; a part without one defaults to
    // its first colour layer. An explicit name is returned unchanged, listed or not.
    [[nodiscard]] std::string resolveColourLayer(const std::string_view selected) const {
        if (selected.empty() && !colourLayers.empty() &&
            std::ranges::find(colourLayers, std::string{}) == colourLayers.end())
            return colourLayers.front();
        return std::string(selected);
    }
    friend bool operator==(const ImagePartInfo&, const ImagePartInfo&) = default;
    [[nodiscard]] std::size_t retainedBytes() const noexcept {
        std::size_t total =
            sizeof(ImagePartInfo) + name.capacity() + view.capacity() + colorSpaceTag.capacity();
        for (const auto& channel : channels)
            total += sizeof(ImageChannelInfo) + channel.name.capacity();
        for (const auto& layer : layers)
            total += sizeof(std::string) + layer.capacity();
        for (const auto& layer : colourLayers)
            total += sizeof(std::string) + layer.capacity();
        return total;
    }
};

struct ImageProbe {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    core::Sha256Digest contentDigest;
    ImageFormat format = ImageFormat::Png;
    ImageColorSpace colorSpace = ImageColorSpace::Auto;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Straight;
    // The tag is intentionally a Bloom vocabulary value rather than an OpenEXR type. For a
    // chromaticities-bearing EXR it records the recognized interpretation; when metadata is absent,
    // interpretationAssumption explains the convention used by the bounded reader.
    std::string colorSpaceTag;
    std::string interpretationAssumption;
    // The image frame (OpenEXR display window) and the data window's offset inside it, in source
    // pixels. width/height above are the data window. A zero frame means the frame is the data
    // window at offset (0, 0), as for PNG/JPEG.
    std::int64_t dataOffsetX = 0;
    std::int64_t dataOffsetY = 0;
    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;
    // Parts (ADR 0026): every OpenEXR part, or the one unnamed part of a PNG or JPEG still (its
    // R, G, B and optional A); empty for provider formats. The scalar fields above describe the
    // first part and its root colour layer.
    std::vector<ImagePartInfo> parts{};
    [[nodiscard]] const ImagePartInfo* findPart(const std::string_view name) const noexcept {
        if (parts.empty())
            return nullptr;
        if (name.empty())
            return &parts.front();
        for (const auto& part : parts)
            if (part.name == name)
                return &part;
        return nullptr;
    }
    [[nodiscard]] std::uint32_t effectiveFrameWidth() const noexcept {
        return frameWidth != 0 ? frameWidth : width;
    }
    [[nodiscard]] std::uint32_t effectiveFrameHeight() const noexcept {
        return frameHeight != 0 ? frameHeight : height;
    }
};

// A session-owned, immutable source revision. Its digest, header and every decoder read refer to
// these exact captured bytes, never to a reopened pathname or a timestamp. Capture is worker-only;
// callers bound both transient capture and retention through their current host allowances.
struct ImageSourceCaptureStatistics final {
    std::uint64_t ioNanoseconds = 0;
    std::uint64_t hashNanoseconds = 0;
    std::uint64_t probeNanoseconds = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesHashed = 0;
};
struct ImageDecodeStatistics final {
    // Codec, image construction and any requested input colour processing. Source capture I/O,
    // SHA256, metadata probe, proxy resampling and cache lookup are excluded.
    std::uint64_t decodeNanoseconds = 0;
    // Pixels repaired by the non-finite/out-of-range policy during this decode.
    std::uint64_t repairedPixels = 0;
};
class ImageSourceRevision final {
  public:
    [[nodiscard]] const ImageProbe& probe() const noexcept { return probe_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] const ImageSourceCaptureStatistics& captureStatistics() const noexcept {
        return captureStatistics_;
    }
    // The file identity observed before and after the read when it held across it. While a later
    // observation is identical, the original still holds these bytes (ADR 0025).
    [[nodiscard]] const std::optional<platform::FileIdentity>& sourceIdentity() const noexcept {
        return sourceIdentity_;
    }
    // True when a full SHA-256 of these bytes (now or recorded in the process memo for this
    // identity) produced probe().contentDigest. False when the identity check admitted them and
    // the digest is the recorded one (or, for a pending member, a capture identity) awaiting the
    // background verification (ADR 0025, amended 2026-11-04).
    [[nodiscard]] bool contentVerified() const noexcept { return contentVerified_; }
    [[nodiscard]] std::size_t retainedBytes() const noexcept {
        std::size_t total = sizeof(ImageSourceRevision) + bytes_.capacity() +
                            probe_.colorSpaceTag.capacity() +
                            probe_.interpretationAssumption.capacity();
        for (const auto& part : probe_.parts)
            total += part.retainedBytes();
        return total;
    }

  private:
    ImageSourceRevision(std::vector<std::byte> bytes, ImageProbe probe,
                        ImageSourceCaptureStatistics statistics,
                        std::optional<platform::FileIdentity> identity, bool verified)
        : bytes_(std::move(bytes)), probe_(std::move(probe)), captureStatistics_(statistics),
          sourceIdentity_(identity), contentVerified_(verified) {}
    friend ImageResult<std::shared_ptr<const ImageSourceRevision>>
    captureImageSourceRevision(const std::filesystem::path&, std::size_t, const CancelImageWork&,
                               std::optional<core::Sha256Digest>, std::optional<ImageSourceStamp>);
    std::vector<std::byte> bytes_;
    ImageProbe probe_;
    ImageSourceCaptureStatistics captureStatistics_;
    std::optional<platform::FileIdentity> sourceIdentity_;
    bool contentVerified_ = false;
};

// Reads the file into an immutable revision without hashing it when its identity held across the
// read (ADR 0025, amended 2026-11-04). `expectedDigest` is the recorded digest: a process-memo
// digest for this identity must equal it, and a file that changed during the read or against
// `expectedStamp` is hashed and admitted only when its bytes hash to it. `expectedStamp` (#473)
// alone admits the bytes only when the file's stamp before and after the read is that stamp
// (DigestMismatch otherwise): a sequence member pending its digest.
[[nodiscard]] ImageResult<std::shared_ptr<const ImageSourceRevision>>
captureImageSourceRevision(const std::filesystem::path& path, std::size_t hostByteBudget,
                           const CancelImageWork& cancel = {},
                           std::optional<core::Sha256Digest> expectedDigest = {},
                           std::optional<ImageSourceStamp> expectedStamp = {});

// The revision remains live throughout decode. Its encoded storage is subtracted from the total
// host allowance before admitting codec staging and output. No path is reopened and no hash
// repeats.
[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const ImageSourceRevision& revision, const ImageInterpretation& interpretation = {},
            std::shared_ptr<const color::CpuColorSpaceProcessor> processor = {},
            const CancelImageWork& cancel = {}, const ImageProgress& progress = {},
            std::size_t hostByteBudget = kMaxImageStorageBytes,
            ImageDecodeStatistics* statistics = nullptr);

// #422: the raw decode's samples in their native layout, for the GPU route: a PNG or JPEG's 16-bit
// RGBA samples, or an all-half OpenEXR colour layer's binary16 samples, with the SampleConvertV1
// parameters that make render::convertNativeSamples(result) equal decodeImage(revision,
// interpretation) value for value (and the same repaired-pixel count). `interpretation` must be a
// raw one (ImageColorSpace::Raw without an input colour space). Any other source or interpretation
// (TIFF, a FLOAT or UINT OpenEXR layer) returns NativeSamplesUnavailable and the caller decodes to
// float instead.
[[nodiscard]] ImageResult<std::shared_ptr<const render::NativeSampleImage>>
decodeImageSamples(const ImageSourceRevision& revision, const ImageInterpretation& interpretation,
                   const CancelImageWork& cancel = {},
                   std::size_t hostByteBudget = kMaxImageStorageBytes,
                   ImageDecodeStatistics* statistics = nullptr);

// ADR 0026 channel-set decode. `part` selects an OpenEXR part (empty: the first); `channels` names
// the channels to decode in order (empty: every channel of the part). Values are source values:
// never converted, premultiplied, repaired or clamped. PNG and JPEG stills expose their decoded
// R, G, B (and A when present) as straight Colour channels in [0, 1]; formats without an immutable
// revision decoder report UnsupportedChannelSetFormat.
struct ImageChannelSelection final {
    std::string part{};
    std::vector<std::string> channels{};
    // The input colour space recorded on the channel set for its Colour channels (the asset's
    // interpretation); empty records the source's declared tag.
    std::string inputColorSpaceId{};
    friend bool operator==(const ImageChannelSelection&, const ImageChannelSelection&) = default;
};
[[nodiscard]] ImageResult<std::shared_ptr<const render::ChannelSetImage>>
decodeImageChannels(const ImageSourceRevision& revision, const ImageChannelSelection& selection,
                    const CancelImageWork& cancel = {}, const ImageProgress& progress = {},
                    std::size_t hostByteBudget = kMaxImageStorageBytes,
                    ImageDecodeStatistics* statistics = nullptr);

enum class ImageProviderRole : std::uint8_t { Decode, Encode };

struct ImageProviderDecodeRequest final {
    std::filesystem::path path;
    ImageInterpretation interpretation;
    std::size_t pixelBudget = kMaxImageStorageBytes;
    std::optional<core::Sha256Digest> expectedDigest;
};

struct ImageProviderDecodeResponse final {
    ImageProbe probe;
    std::shared_ptr<const render::Rgba32fImage> image;
};

struct ImageProviderEncodeRequest final {
    std::filesystem::path destination;
    std::shared_ptr<const render::Rgba32fImage> image;
};

struct ImageProviderEncodeResponse final {
    bool written = false;
};

using ImageProviderDecode =
    std::function<ImageResult<ImageProviderDecodeResponse>(const ImageProviderDecodeRequest&)>;
using ImageProviderEncode =
    std::function<ImageResult<ImageProviderEncodeResponse>(const ImageProviderEncodeRequest&)>;

// A provider is supplied explicitly by the worker composition root. The in-process image library
// never discovers a provider globally: an absent callback is the typed ProviderMissing outcome
// required for TIFF until MEDIA-3 wires the worker.
struct ImageProvider final {
    ImageProviderDecode decode;
    ImageProviderEncode encode;
};

template <typename T> struct ImageResult {
    std::optional<T> value;
    std::string diagnostic;
    bool cancelled = false;
    ImageDiagnosticCode code = ImageDiagnosticCode::None;
    // Pixels a successful decode repaired under the documented non-finite policy (media-io.md
    // "OpenEXR ingest"): non-finite components become zero and alpha is clamped to [0, 1].
    std::uint64_t repairedPixels = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return value.has_value(); }
};
struct SequenceMember {
    std::int64_t frame = 0;
    std::filesystem::path path;
    // #473: what the listing observed; the bytes are not read.
    ImageSourceStamp stamp;
};
struct SequenceManifest {
    std::string pattern;
    std::uint32_t padding = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::vector<SequenceMember> members;
    std::vector<std::int64_t> gaps;
    std::vector<std::string> diagnostics;
    // Missing intervals are implicit between adjacent sorted members. `gaps` remains for
    // compatibility with older manifests that enumerated every missing frame.
    [[nodiscard]] bool hasGaps() const noexcept {
        if (!gaps.empty())
            return true;
        for (std::size_t index = 1; index < members.size(); ++index)
            if (members[index].frame > members[index - 1].frame &&
                static_cast<std::uint64_t>(members[index].frame) -
                        static_cast<std::uint64_t>(members[index - 1].frame) >
                    1)
                return true;
        return false;
    }
};
// Worker-only locator resolution: prefer the project-relative file; use the absolute relink
// hint when that file is unavailable (including an unsaved project's first Save As).
[[nodiscard]] std::filesystem::path resolveImagePath(std::string_view relativePath,
                                                     std::string_view relinkHint,
                                                     const std::filesystem::path& projectDirectory);
[[nodiscard]] ImageResult<ImageProbe> probeImage(const std::filesystem::path& path,
                                                 const CancelImageWork& cancel = {},
                                                 const ImageProvider* provider = nullptr);
[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const std::filesystem::path& path, const ImageInterpretation& interpretation = {},
            std::shared_ptr<const color::CpuColorSpaceProcessor> processor = {},
            const CancelImageWork& cancel = {}, const ImageProgress& progress = {},
            std::size_t pixelBudget = kMaxImageStorageBytes,
            std::optional<core::Sha256Digest> expectedDigest = {},
            const ImageProvider* provider = nullptr);
[[nodiscard]] ImageResult<ImageProviderEncodeResponse>
encodeImage(const ImageProviderEncodeRequest& request, const ImageProvider* provider = nullptr);
// The numbered frames, inclusive, a scan keeps (an import's frame range).
struct SequenceFrameRange {
    std::int64_t first = 0;
    std::int64_t last = 0;
};
// Lists the members of `path`'s sequence, then stamps each one (#473: one stat per member; no
// member is opened, read or hashed). `progress` reports the listing as (found, 0) and the stamping
// as (stamped, members). With `range`, only the members numbered within it are kept; none there is
// "No frames in the frame range". A member that cannot be stamped is reported and becomes a gap
// (ADR 0026 / #373); one whose bytes turn out unreadable is found when it is first read.
// Numbered files form a sequence only when two of them (ignoring `range`) have consecutive frame
// numbers or three share one constant step (1, 3, 5); otherwise the manifest has no members and
// `path` is a still. Several files numbered with one frame keep one (the picked file, then one with
// its padding and extension spelling, then the first file name) with a diagnostic naming the files
// left out.
[[nodiscard]] ImageResult<SequenceManifest>
scanSequence(const std::filesystem::path& path, const CancelImageWork& cancel = {},
             const ImageProgress& progress = {},
             const std::optional<SequenceFrameRange>& range = std::nullopt);
// What every numbered member of one image sequence shares: its directory and the name around the
// frame number. When formsSequence() is true, two paths with the same key are members of one
// sequence, so scanSequence() of either finds the other. Nullopt for a name without a frame number
// or a format that never sequences. Names only; no file system access beyond absolute().
[[nodiscard]] std::optional<std::string> sequenceKey(const std::filesystem::path& path);
// Whether `path`'s numbered group is an image sequence: two of its files, ignoring any frame
// range, have consecutive frame numbers or three share one
// constant step. Lists the directory only, stopping at the first such pair or triple. False for a
// name without a frame number. True for an unreadable directory, which scanSequence() reports.
// Nullopt when cancelled.
[[nodiscard]] std::optional<bool> formsSequence(const std::filesystem::path& path,
                                                const CancelImageWork& cancel = {});
// How many members scanSequence() would read for `path`, by listing its directory only: 1 for a
// name without a frame number. `progress` reports (found, 0). Nullopt when cancelled.
[[nodiscard]] std::optional<std::uint64_t>
countSequenceMembers(const std::filesystem::path& path, const CancelImageWork& cancel = {},
                     const ImageProgress& progress = {},
                     const std::optional<SequenceFrameRange>& range = std::nullopt);
} // namespace bloom::media
