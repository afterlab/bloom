#pragma once
#include "footage_interpretation_pass.hpp"
#include <bloom/media/image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bloom::runtime {
class ImageSequenceReadAhead;
} // namespace bloom::runtime

namespace bloom::runtime::detail {
struct ImageSourceSelection {
    std::filesystem::path path;
    // The source's identity: the asset or member content digest, or for a sequence member import
    // stamped without reading it (#473) its provisional identity
    // (document::sequenceMemberIdentity). Every cache key below is made from it.
    core::Sha256Digest digest;
    // #473: the stamp a stamped member's file must still carry; its bytes are verified by it
    // rather than by `digest`.
    std::optional<media::ImageSourceStamp> expectedStamp;
    // The digest the project records for the bytes: the asset's, or the member's (absent for a
    // member pending its background digest). A capture compares it only when the process memo or
    // a changed identity supplies a full hash (ADR 0025, amended 2026-11-04).
    std::optional<core::Sha256Digest> recordedDigest;
    media::ImageInterpretation interpretation;
    std::shared_ptr<const color::CpuColorSpaceProcessor> inputProcessor;
    std::string resolvedInputColorSpaceName;
    std::string inputColorSpaceWarning;
    std::string workingColorSpaceId;
    core::Sha256Digest configRevision;
    bool inputColorSpaceAutomatic = false;
    std::string cacheKey;
    // Decode-only identity: validated asset/sequence-member content digest + the requested alpha
    // association. It deliberately excludes the colour interpretation, input/working colour space,
    // configuration revision and the proxy, because none of them change the RAW decoded bytes. The
    // GPU media colour split keys its raw upload cache on this identity, so a changed working space
    // or display reuses the decoded upload while a changed frame is a miss.
    std::string decodeKey;
    std::string warning;
    bool available = false;
    bool cancelled = false;
    // The source capture was refused by the host byte budget, not by missing/changed media.
    bool budgetExceeded = false;
    std::shared_ptr<const media::ImageSourceRevision> sourceRevision;
    bool retainedRevision = false;
    // #381: the selected sequence frame is missing and the asset shows nothing there. The
    // selection is available, reads no file and decodes to a transparent image whose frame bounds
    // are the sequence's frame size.
    bool missingFrame = false;
    // #381: the footage interpretation pass the converted image runs (alpha, deinterlace).
    std::optional<FootagePass> footagePass;
    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;
};
// The association an image source decodes with: an explicit asset alpha interpretation, else the
// node's Premultiply switch.
[[nodiscard]] media::ImageAlphaAssociation
imageSourceAlphaAssociation(const CompiledImageSource& source);
// The source revisions one evaluation (one scene build) has already captured, by revision key.
// Several layers reading the same file at the same time share the revision the first verified
// instead of reading and hashing the file again. The revision cache cannot serve that within a
// linear pass, which stores a key only when it is offered a second time. Thread-safe.
class ImageSourceRevisionMemo final {
  public:
    [[nodiscard]] std::shared_ptr<const media::ImageSourceRevision>
    find(const std::string& key) const;
    void share(const std::string& key, std::shared_ptr<const media::ImageSourceRevision> revision);

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const media::ImageSourceRevision>> revisions_;
};
// The revision-cache key of one immutable image file (selectImageSource stores captures under it).
[[nodiscard]] std::string imageSourceRevisionKey(const core::Sha256Digest& digest,
                                                 const document::AssetLocator& locator,
                                                 const std::filesystem::path& base);
// One sequence member file to capture ahead of need.
struct ImageSourceReadAheadTarget final {
    std::filesystem::path path;
    core::Sha256Digest digest;
    // #473: a stamped member is captured against its stamp instead of `digest`.
    std::optional<media::ImageSourceStamp> stamp;
    // The digest the project records for the member (absent while it is pending).
    std::optional<core::Sha256Digest> recordedDigest;
    std::string revisionKey;
    // #718: the asset's override of local copies, for the capture's mirror lookup.
    platform::LocalCopyOverride localCopies = platform::LocalCopyOverride::Inherit;
};
// The asset's "Keep local copies" choice as the source store's override.
[[nodiscard]] platform::LocalCopyOverride
localCopyOverride(const document::AssetInterpretation& interpretation) noexcept;
// The decode-only identity of a selected image: its content digest, the stored samples (a footage
// pass) or the alpha association it decodes with, and its part and resolved colour layer. Colour
// interpretation, proxy and path fields are deliberately absent.
[[nodiscard]] std::string imageDecodeKey(const core::Sha256Digest& digest, bool storedSamples,
                                         media::ImageAlphaAssociation alphaAssociation,
                                         const std::string& part, const std::string& layer);
// #386: the operation-cache key of an image's decoded native samples (decodeRawImageSamples()),
// which the sequence read-ahead decodes ahead of the frame that uses them.
[[nodiscard]] std::string decodedImageSamplesKey(const std::string& decodeKey);
// #549: the identity of one source part (content digest and part), and the operation-cache key of
// named channels decoded from it, `channels` in the order they were decoded (sorted for a build's
// shared read).
[[nodiscard]] std::string sourcePartKey(const core::Sha256Digest& digest, const std::string& part);
[[nodiscard]] std::string decodedImageChannelsKey(const std::string& partKey,
                                                  const std::vector<std::string>& channels);
// The distinct sequence members the next `frames` composition frames after `time` select (start
// frame, loop mode and gap hold included), excluding the member `time` itself selects.
[[nodiscard]] std::vector<ImageSourceReadAheadTarget>
imageSequenceReadAheadTargets(const CompiledImageSource& source, core::RationalTime time,
                              document::FrameRate rate, const std::filesystem::path& base,
                              std::size_t frames);
// `allowRevisionInsertion` lets a source read land in `revisionCache` as a source revision (the
// file's encoded bytes under its own key). The bytes of a file do not depend on any parameter, so
// an interactive override keeps them too (#718): a drag reads each source once, not once per
// step. Decoded frames keep their own gesture rule (ImageSourceMemoryCacheAccess).
[[nodiscard]] ImageSourceSelection selectImageSource(
    const CompiledImageSource& source, core::RationalTime time, document::FrameRate rate,
    const std::filesystem::path& base, const CancellationToken& cancel,
    const EvaluationColorIntent& colorIntent = EvaluationColorIntent::LinearRec709Scene,
    OperationCache* revisionCache = nullptr,
    MediaSourceRevisionPolicy revisionPolicy = MediaSourceRevisionPolicy::Revalidate,
    std::size_t hostByteBudget = media::kMaxImageStorageBytes, bool allowRevisionInsertion = true,
    LinearPassRetention* linearPass = nullptr, ImageSourceRevisionMemo* buildRevisions = nullptr,
    ImageSequenceReadAhead* readAhead = nullptr);
// CACHE-1: how evaluateImageSource() may use the evaluator's shared memory cache for an
// already-verified, immutable native decoded still-image. This governs only the source decode
// entry; derived operation memoization is gated separately by the evaluator's own `cache` pointer.
enum class ImageSourceMemoryCacheAccess : std::uint8_t {
    // Explicit evaluation bypass (`request.bypassOperationCache`): the still-image memory and disk
    // entries are neither read nor written, so the call is an uncached re-decode. This flag governs
    // derived operation memoization and the still-image source caches only; it does not touch the
    // video decoded cache or colour-processor caches.
    Disabled,
    // Interactive/overridden plan (`plan->bypassOperationCache()`): read a warmed native decoded
    // still-image entry, but never insert one. A gesture miss decodes directly and is not stored,
    // so gesture data cannot enter the memory cache under the source key.
    ReadOnly,
    // Ordinary request: memory -> disk -> decode, inserting the decoded entry on a miss.
    ReadWrite,
};
// `memoryCache` is the evaluator's shared memory cache and `memoryAccess` says whether this call
// may read it, read-and-write it, or ignore it entirely.
// A memory miss decodes the selection's source revision; decoded frames have no disk tier (#718:
// the disk tier holds the encoded source bytes behind the revision).
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
evaluateImageSource(const ImageSourceSelection& selection,
                    render::Rgba32fImageDescriptor composition, double horizontalScale,
                    double verticalScale, std::size_t budget, OperationCache* memoryCache,
                    ImageSourceMemoryCacheAccess memoryAccess, const CancellationToken& cancel,
                    media::ImageDecodeStatistics* decodeStatistics = nullptr,
                    LinearPassRetention* linearPass = nullptr);

// Decode-only factoring for the GPU media colour split. The connected evaluateImageSource() path
// is unchanged; these two functions expose its halves so the GPU path can decode RAW (no OCIO
// processor, no implicit conversion, no proxy resample hidden in host code), upload the full source
// dimensions/metadata, and let the accepted GPU colour command run the input->working transform.

// Decode the selected source with no input colour processor and no resample. The returned image
// carries the native decoded pixels and the source's own descriptor. Selection, digest validation
// and diagnostics are exactly media::decodeImage()'s; the interpretation is forced to a Raw,
// processor-free decode so no OCIO pass runs.
[[nodiscard]] media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeRawImageSource(const ImageSourceSelection& selection, std::size_t budget,
                     const CancellationToken& cancel,
                     media::ImageDecodeStatistics* decodeStatistics = nullptr,
                     media::ImageChannelsAlongside* alongside = nullptr);

// #422: the same raw decode in its native sample layout (a PNG, JPEG, TIFF or DPX's 16-bit RGBA, an
// all-half OpenEXR layer's binary16 RGBA) for SampleConvertV1 to widen on the device;
// render::convertNativeSamples() of the result equals decodeRawImageSource(). A missing sequence
// frame or a FLOAT/UINT OpenEXR layer returns NativeSamplesUnavailable.
[[nodiscard]] media::ImageResult<std::shared_ptr<const render::NativeSampleImage>>
decodeRawImageSamples(const ImageSourceSelection& selection, std::size_t budget,
                      const CancellationToken& cancel,
                      media::ImageDecodeStatistics* decodeStatistics = nullptr,
                      media::ImageChannelsAlongside* alongside = nullptr);

// Rebase a decoded image into the evaluator's composition descriptor: its data window keeps its
// offset inside the source frame (OpenEXR display window), scaled by the shared PointResampleV1
// window rule, with the composition display window and pixel aspect. At unit scale this is the
// evaluator's exact rebase copy; at a fractional proxy scale it is the evaluator's exact
// nearest-neighbour copy. One implementation shared by evaluateImageSource() and the GPU split.
[[nodiscard]] std::optional<render::ImageWindow>
frameRelativeDataWindow(const render::Rgba32fImageDescriptor& decoded) noexcept;
// Local content bounds of an image source whose data window differs from its frame (an OpenEXR
// crop or overscan): the frame in full-resolution source pixels, so passes with different data
// windows of one frame stay registered under the layer transform. nullopt when data and frame
// coincide; the bounds then follow the published data window exactly as before.
[[nodiscard]] std::optional<ContentBounds>
imageSourceFrameBounds(const ImageSourceSelection& selection) noexcept;
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
resampleDecodedImage(const render::Rgba32fImage& decoded,
                     render::Rgba32fImageDescriptor composition, double horizontalScale,
                     double verticalScale, std::size_t budget, const CancellationToken& cancel);
} // namespace bloom::runtime::detail
