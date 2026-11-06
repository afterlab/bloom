#include "image_source.hpp"
#include "footage_interpretation_pass.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"
#include <algorithm>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/media/cache/media_disk_cache_decode.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/render/gpu_point_resample.hpp>
#include <bloom/runtime/footage_time.hpp>
#include <bloom/runtime/image_sequence_read_ahead.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

namespace {
struct SequenceMemberChoice final {
    const document::AssetSequenceMember* member = nullptr;
    // #381: the frame is missing from the manifest and the asset shows nothing there.
    bool missing = false;
    std::int64_t frame = 0;
    std::string warning;
};
// The sequence member a composition time selects: the node's start frame and loop mode, the
// asset's rate, Loop count and missing-frame interpretation (footageFrameIndex), and gap hold.
SequenceMemberChoice selectSequenceMember(const CompiledImageSource& source,
                                          const core::RationalTime time,
                                          const document::FrameRate rate) {
    const auto& asset = *source.asset;
    SequenceMemberChoice choice;
    if (asset.manifest.members.empty()) {
        choice.warning = "Sequence time is outside the supported range";
        return choice;
    }
    const auto offset = footageFrameIndex(asset, time, source.startFrame, source.loopMode, rate);
    if (!offset) {
        choice.warning = "Sequence time exceeds the frame range";
        return choice;
    }
    // The member, the one a gap holds, none for a gap that shows nothing, or the members back to
    // back when the asset skips missing frames.
    const auto frame = document::footageSequenceFrame(asset, *offset);
    choice.frame = frame.frame;
    choice.member = frame.member;
    choice.missing = frame.member == nullptr;
    return choice;
}
// A transparent 1x1 image at the frame origin: what a missing sequence frame decodes to when the
// asset shows nothing there. Its frame bounds (imageSourceFrameBounds) keep the layer geometry.
media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>
missingFrameImage(const std::size_t budget) {
    const auto window = render::ImageWindow::create(0, 0, 1, 1);
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return {{},
                "Image source exceeds the pixel budget",
                false,
                media::ImageDiagnosticCode::PixelBudgetExceeded};
    auto row = builder.value()->row(0);
    (*row.value())[0] = render::Rgba32f::transparent();
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return {{}, "Image source pixel validation failed"};
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}};
}
// Whether a capture failed because the bytes are not an image Bloom reads (#473), as opposed to
// a missing, changed, unreadable-for-now or over-budget file, or a cancellation.
bool unreadableImage(const media::ImageDiagnosticCode code) noexcept {
    using Code = media::ImageDiagnosticCode;
    switch (code) {
    case Code::InvalidMagic:
    case Code::InvalidHeader:
    case Code::UnsupportedVersion:
    case Code::UnsupportedDeepImage:
    case Code::UnsupportedMultipart:
    case Code::UnsupportedTiledLevels:
    case Code::UnsupportedChannels:
    case Code::UnsupportedChannelType:
    case Code::InvalidDataWindow:
    case Code::DimensionsExceeded:
    case Code::Truncated:
    case Code::DecodeFailed:
    case Code::ProviderMissing:
    case Code::ProviderFailed:
        return true;
    default:
        return false;
    }
}
} // namespace

media::ImageAlphaAssociation imageSourceAlphaAssociation(const CompiledImageSource& source) {
    // #381: an explicit asset alpha interpretation applies to every use of the asset; Guess leaves
    // each node's Premultiply switch (made from the file's declared association) in charge.
    if (source.asset) {
        switch (source.asset->interpretation.alphaMode) {
        case document::AssetAlphaMode::Straight:
        case document::AssetAlphaMode::Ignore:
            return media::ImageAlphaAssociation::Straight;
        case document::AssetAlphaMode::Premultiplied:
            return media::ImageAlphaAssociation::Premultiplied;
        case document::AssetAlphaMode::Guess:
            break;
        }
    }
    return source.premultiply ? media::ImageAlphaAssociation::Straight
                              : media::ImageAlphaAssociation::Premultiplied;
}

std::string imageSourceRevisionKey(const core::Sha256Digest& digest,
                                   const document::AssetLocator& locator,
                                   const std::filesystem::path& base) {
    OperationKey revisionKey;
    revisionKey.add(std::string{"immutable-image-source-v1"});
    const auto digestHex = digest.toLowercaseHex();
    revisionKey.add(std::string(digestHex.begin(), digestHex.end()));
    revisionKey.add(locator.path);
    revisionKey.add(locator.relinkHint);
    const auto baseUtf8 = base.generic_u8string();
    revisionKey.add(std::string(reinterpret_cast<const char*>(baseUtf8.data()), baseUtf8.size()));
    return revisionKey.bytes();
}

std::string imageDecodeKey(const core::Sha256Digest& digest, const bool storedSamples,
                           const media::ImageAlphaAssociation alphaAssociation,
                           const std::string& part, const std::string& layer) {
    OperationKey decodeKey;
    decodeKey.add(std::string{"gpu-raw-decode-v2"});
    const auto hex = digest.toLowercaseHex();
    decodeKey.add(std::string(hex.begin(), hex.end()));
    // A footage pass decodes the stored samples, which no association changes.
    if (storedSamples)
        decodeKey.add(std::string{"stored-samples-v1"});
    else
        decodeKey.add(alphaAssociation);
    decodeKey.add(part);
    decodeKey.add(layer);
    return decodeKey.digest();
}

std::string decodedImageSamplesKey(const std::string& decodeKey) {
    OperationKey key;
    key.add(std::string{"decoded-image-samples-v1"});
    key.add(decodeKey);
    return key.bytes();
}

std::string sourcePartKey(const core::Sha256Digest& digest, const std::string& part) {
    OperationKey key;
    key.add(std::string{"source-part-v1"});
    const auto hex = digest.toLowercaseHex();
    key.add(std::string(hex.begin(), hex.end()));
    key.add(part);
    return key.digest();
}

std::string decodedImageChannelsKey(const std::string& partKey,
                                    const std::vector<std::string>& channels) {
    OperationKey key;
    key.add(std::string{"decoded-image-channels-v1"});
    key.add(partKey);
    key.add(static_cast<std::uint64_t>(channels.size()));
    for (const auto& channel : channels)
        key.add(channel);
    return key.bytes();
}

std::vector<ImageSourceReadAheadTarget>
imageSequenceReadAheadTargets(const CompiledImageSource& source, const core::RationalTime time,
                              const document::FrameRate rate, const std::filesystem::path& base,
                              const std::size_t frames) {
    std::vector<ImageSourceReadAheadTarget> targets;
    if (!source.asset || source.asset->kind != document::AssetKind::Sequence || frames == 0)
        return targets;
    const auto current = selectSequenceMember(source, time, rate).member;
    const auto frame = valueGraphFrameIndex(time, rate);
    if (!frame || *frame > std::numeric_limits<std::int64_t>::max() / 2)
        return targets;
    for (std::size_t index = 1; index <= frames; ++index) {
        const auto next =
            core::RationalTime::create((*frame + static_cast<std::int64_t>(index)) *
                                           static_cast<std::int64_t>(rate.denominator()),
                                       static_cast<std::int64_t>(rate.numerator()));
        if (!next)
            break;
        const auto* member = selectSequenceMember(source, *next, rate).member;
        if (!member || member == current)
            continue;
        const auto identity = document::sequenceMemberIdentity(*member);
        auto key = imageSourceRevisionKey(identity, member->locator, base);
        if (std::ranges::any_of(targets,
                                [&key](const auto& target) { return target.revisionKey == key; }))
            continue;
        targets.push_back(
            {.path =
                 media::resolveImagePath(member->locator.path, member->locator.relinkHint, base),
             .digest = identity,
             .stamp = member->stamp ? std::optional(media::ImageSourceStamp{
                                          member->stamp->size, member->stamp->modifiedNanoseconds})
                                    : std::nullopt,
             .recordedDigest = member->contentDigest,
             .revisionKey = std::move(key)});
    }
    return targets;
}

std::shared_ptr<const media::ImageSourceRevision>
ImageSourceRevisionMemo::find(const std::string& key) const {
    const std::lock_guard lock(mutex_);
    const auto found = revisions_.find(key);
    return found == revisions_.end() ? nullptr : found->second;
}

void ImageSourceRevisionMemo::share(const std::string& key,
                                    std::shared_ptr<const media::ImageSourceRevision> revision) {
    if (!revision)
        return;
    try {
        const std::lock_guard lock(mutex_);
        revisions_.try_emplace(key, std::move(revision));
    } catch (const std::bad_alloc&) {
        // Sharing is an optimization inside one evaluation; a later layer captures its own.
    }
}

ImageSourceSelection selectImageSource(
    const CompiledImageSource& source, core::RationalTime time, document::FrameRate rate,
    const std::filesystem::path& base, const CancellationToken& cancel,
    const EvaluationColorIntent& colorIntent, OperationCache* revisionCache,
    const MediaSourceRevisionPolicy revisionPolicy, const std::size_t hostByteBudget,
    const bool allowRevisionInsertion, LinearPassRetention* const linearPass,
    ImageSourceRevisionMemo* const buildRevisions, ImageSequenceReadAhead* const readAhead) {
    ImageSourceSelection selected;
    if (!source.asset) {
        selected.warning = "Image asset is missing; choose an asset or relink in Assets";
        selected.cacheKey = "missing-asset";
        return selected;
    }
    const auto& asset = *source.asset;
    const document::AssetLocator* locator = &asset.locator;
    selected.digest = asset.contentDigest;
    selected.recordedDigest = asset.contentDigest;
    std::int64_t memberFrame = 0;
    // #381: a missing frame shows nothing; no file is read.
    const auto missingFrame = [&](const std::int64_t frame) {
        selected.missingFrame = true;
        selected.available = true;
        selected.frameWidth = asset.width;
        selected.frameHeight = asset.height;
        OperationKey key;
        key.add(std::string{"missing-sequence-frame-v1"});
        const auto digest = asset.contentDigest.toLowercaseHex();
        key.add(std::string(digest.begin(), digest.end()));
        key.add(frame);
        key.add(asset.width);
        key.add(asset.height);
        selected.cacheKey = key.bytes();
        selected.decodeKey = key.digest();
    };
    const document::AssetSequenceMember* chosen = nullptr;
    // A member's identity and, for a member import stamped without reading it (#473), the stamp
    // its bytes must still carry.
    const auto choose = [&](const document::AssetSequenceMember& member) {
        chosen = &member;
        locator = &member.locator;
        selected.digest = document::sequenceMemberIdentity(member);
        selected.recordedDigest = member.contentDigest;
        selected.expectedStamp =
            member.stamp ? std::optional(media::ImageSourceStamp{member.stamp->size,
                                                                 member.stamp->modifiedNanoseconds})
                         : std::nullopt;
        memberFrame = member.frame;
    };
    if (asset.kind == document::AssetKind::Sequence) {
        const auto member = selectSequenceMember(source, time, rate);
        if (member.missing) {
            missingFrame(member.frame);
            return selected;
        }
        if (!member.member) {
            selected.warning = member.warning;
            return selected;
        }
        choose(*member.member);
        if (asset.manifest.hasGaps() &&
            asset.interpretation.missingFrames == document::AssetMissingFrames::HoldPrevious)
            selected.warning = "Sequence gaps hold the preceding frame";
    }
    media::ImageResult<std::shared_ptr<const media::ImageSourceRevision>> capture;
    // The verified revision of the chosen file: a retained one, or one captured now. A digest
    // member's bytes must hash to its digest; a stamped member's file must still carry its stamp,
    // and the digest the first read records (ADR 0025 memo) then stands for its bytes.
    const auto captureChosen = [&] {
        selected.sourceRevision = nullptr;
        selected.retainedRevision = false;
        selected.budgetExceeded = false;
        capture = {};
        const auto revisionKeyText = imageSourceRevisionKey(selected.digest, *locator, base);
        const auto revisionGeneration =
            revisionCache ? std::optional{revisionCache->decodedMediaGeneration()} : std::nullopt;
        if (cancel.isCancellationRequested()) {
            selected.cancelled = true;
            return false;
        }
        // Captured earlier in this same evaluation: the file was read and verified a moment ago.
        if (buildRevisions) {
            if (auto shared = buildRevisions->find(revisionKeyText);
                shared && shared->retainedBytes() < hostByteBudget) {
                selected.path = media::resolveImagePath(locator->path, locator->relinkHint, base);
                selected.sourceRevision = std::move(shared);
                selected.retainedRevision = true;
                return true;
            }
        }
        const auto findRetained = [&] {
            if (revisionCache) {
                if (const auto cached = revisionCache->find(revisionKeyText, document::Revision{},
                                                            OperationCacheEntryKind::DecodedMedia);
                    cached && cached->sourceRevision &&
                    cached->sourceRevision->retainedBytes() < hostByteBudget) {
                    if (revisionPolicy == MediaSourceRevisionPolicy::Retained) {
                        selected.sourceRevision = cached->sourceRevision;
                        selected.retainedRevision = true;
                    } else {
                        // Revalidate: the retained bytes stand for the original while no full hash
                        // in the process memo refutes the recorded digest and either a cheap
                        // identity observation still matches the one that bracketed their capture
                        // (ADR 0025: the identity check, no hash) or the memo verified the file's
                        // current identity to hold the retained bytes' digest (a renamed or touched
                        // file whose content did not change).
                        selected.path =
                            media::resolveImagePath(locator->path, locator->relinkHint, base);
                        const auto observed = platform::observeSource(selected.path);
                        const auto& retained = *cached->sourceRevision;
                        const auto& retainedDigest = retained.probe().contentDigest;
                        const bool refuted = observed.digest && selected.recordedDigest &&
                                             *observed.digest != *selected.recordedDigest;
                        const bool sameIdentity =
                            observed.identity && retained.sourceIdentity() &&
                            *observed.identity == *retained.sourceIdentity() &&
                            (selected.expectedStamp || retainedDigest == selected.digest);
                        const bool verifiedContent =
                            observed.digest && *observed.digest == retainedDigest &&
                            (selected.expectedStamp
                                 ? observed.identity &&
                                       media::ImageSourceStamp{
                                           observed.identity->size,
                                           observed.identity->modifiedNanoseconds} ==
                                           *selected.expectedStamp
                                 : retainedDigest == selected.digest);
                        const bool current = !refuted && (sameIdentity || verifiedContent);
                        if (current) {
                            selected.sourceRevision = cached->sourceRevision;
                            selected.retainedRevision = true;
                        }
                    }
                }
            }
        };
        findRetained();
        // #477: a file the sequence read-ahead is capturing is read once: a queued capture is
        // withdrawn and this frame reads it now; one in flight is waited for and then found in
        // the cache.
        if (!selected.sourceRevision && readAhead != nullptr &&
            readAhead->claim(revisionKeyText, [&] { return cancel.isCancellationRequested(); }))
            findRetained();
        if (!selected.sourceRevision) {
            selected.path = media::resolveImagePath(locator->path, locator->relinkHint, base);
            // #477: this frame's own read goes first; the read-ahead starts no capture meanwhile.
            const auto foreground = readAhead != nullptr ? readAhead->foregroundRead()
                                                         : ImageSequenceReadAhead::ForegroundRead{};
            capture = media::captureImageSourceRevision(
                selected.path, hostByteBudget, [&] { return cancel.isCancellationRequested(); },
                selected.recordedDigest, selected.expectedStamp);
            selected.sourceRevision = capture.value ? *capture.value : nullptr;
            selected.cancelled = capture.cancelled;
            selected.budgetExceeded =
                !selected.sourceRevision &&
                capture.code == media::ImageDiagnosticCode::PixelBudgetExceeded;
            if (selected.sourceRevision && buildRevisions)
                buildRevisions->share(revisionKeyText, selected.sourceRevision);
            if (selected.sourceRevision && revisionCache && allowRevisionInsertion)
                revisionCache->store(revisionKeyText, document::Revision{},
                                     {.image = {},
                                      .values = {},
                                      .bounds = {},
                                      .sourceRevision = selected.sourceRevision},
                                     OperationCacheEntryKind::DecodedMedia, revisionGeneration,
                                     linearPass);
        }
        return true;
    };
    // Cancelled before anything was read: nothing more to describe.
    if (!captureChosen())
        return selected;
    // ADR 0026 / #373 (#473): a stamped member whose bytes turn out not to be a readable image is
    // reported and becomes a gap, found at its first read rather than at import. It shows what a
    // gap shows: nothing when the asset shows nothing for missing frames, else the preceding
    // readable member (a bounded walk back; none left shows nothing).
    constexpr std::size_t kMaxHeldUnreadable = 8;
    std::string unreadable;
    for (std::size_t held = 0; chosen != nullptr && selected.expectedStamp &&
                               !selected.sourceRevision && unreadableImage(capture.code);
         ++held) {
        if (unreadable.empty())
            unreadable = "Frame " + std::to_string(chosen->frame) +
                         " is unreadable and is treated as a gap: " + capture.diagnostic;
        const auto index = static_cast<std::size_t>(chosen - asset.manifest.members.data());
        if (asset.interpretation.missingFrames == document::AssetMissingFrames::Nothing ||
            index == 0 || held >= kMaxHeldUnreadable) {
            const auto frame = chosen->frame;
            selected = {};
            missingFrame(frame);
            selected.warning = std::move(unreadable);
            return selected;
        }
        choose(asset.manifest.members[index - 1]);
        if (!captureChosen())
            return selected;
    }
    if (!unreadable.empty())
        selected.warning = std::move(unreadable);
    const auto input = document::effectiveAssetInputColor(asset.interpretation, source.colorSpace,
                                                          source.inputColorSpaceId);
    selected.interpretation.colorSpace = static_cast<media::ImageColorSpace>(input.colorSpace);
    selected.interpretation.inputColorSpaceId = input.inputColorSpaceId;
    selected.interpretation.alphaAssociation = imageSourceAlphaAssociation(source);
    selected.interpretation.part = source.part;
    selected.interpretation.layer = source.layer;
    // #478: a default layer on a part without a root colour layer is the part's first colour layer,
    // so every route decodes and keys the same layer.
    if (selected.sourceRevision != nullptr)
        if (const auto* part = selected.sourceRevision->probe().findPart(source.part);
            part != nullptr)
            selected.interpretation.layer = part->resolveColourLayer(source.layer);
    selected.available = selected.sourceRevision != nullptr &&
                         (selected.expectedStamp ||
                          selected.sourceRevision->probe().contentDigest == selected.digest);
    // #473: a stamped frame whose file changed (or went missing) since import is never served
    // from the old bytes; the warning names the frame.
    if (!selected.available && chosen != nullptr && selected.expectedStamp &&
        !selected.budgetExceeded)
        selected.warning = "Frame " + std::to_string(chosen->frame) +
                           (capture.code == media::ImageDiagnosticCode::FileUnavailable
                                ? " is missing; relink the sequence in Assets"
                            : capture.code == media::ImageDiagnosticCode::DigestMismatch
                                ? " changed since it was imported; reload the footage in Assets"
                                : ": " + capture.diagnostic);
    if (selected.available) {
        auto config = resolveInputColorConfig(colorIntent);
        if (!config) {
            selected.available = false;
            selected.warning = "The selected OCIO input configuration is unavailable";
        } else {
            // ADR 0026: an Auto interpretation reads the selected part's own chromaticities.
            const auto* probe = &selected.sourceRevision->probe();
            std::optional<media::ImageProbe> partProbe;
            if (const auto* part = probe->findPart(source.part);
                part != nullptr && part->colorSpaceTag != probe->colorSpaceTag) {
                partProbe = *probe;
                partProbe->colorSpaceTag = part->colorSpaceTag;
                probe = &*partProbe;
            }
            const auto resolution =
                resolveImageInputColorSpace(*config, *probe, selected.interpretation.colorSpace,
                                            selected.interpretation.inputColorSpaceId);
            selected.inputColorSpaceAutomatic = resolution.automatic;
            selected.resolvedInputColorSpaceName = resolution.name;
            selected.inputColorSpaceWarning = resolution.warning;
            selected.workingColorSpaceId = std::string(config->processColorSpaceId());
            selected.configRevision = config->expectedRevision();
            if (!resolution.warning.empty() && resolution.id.empty())
                selected.warning = resolution.warning;
            if (!resolution.noConversion && resolution.id.empty())
                selected.available = false;
            if (selected.available) {
                selected.interpretation.inputColorSpaceId = resolution.id;
                std::string diagnostic;
                selected.inputProcessor =
                    prepareInputColorProcessor(*config, resolution, diagnostic);
                if (!resolution.noConversion && !selected.inputProcessor) {
                    selected.available = false;
                    selected.warning = diagnostic;
                }
            }
        }
    }
    if (!selected.available && selected.warning.empty())
        selected.warning = capture.diagnostic.empty() ? "Image changed; relink the asset in Assets"
                                                      : capture.diagnostic;
    OperationKey key;
    const auto digest = selected.digest.toLowercaseHex();
    const auto config = selected.configRevision.toLowercaseHex();
    key.add(std::string(digest.begin(), digest.end()));
    key.add(memberFrame);
    key.add(selected.interpretation.colorSpace);
    key.add(selected.interpretation.inputColorSpaceId);
    // A Bloom-synthesized input space (Rec.2100 HLG) is defined by OCIO built-ins, so its
    // definition and the linked OCIO version are part of the source identity (as for video).
    const auto synthesized =
        color::synthesizedColorSpaceIdentity(selected.interpretation.inputColorSpaceId);
    key.add(synthesized);
    key.add(selected.workingColorSpaceId);
    const auto revision = selected.configRevision.toLowercaseHex();
    key.add(std::string(revision.begin(), revision.end()));
    key.add(selected.interpretation.alphaAssociation);
    key.add(selected.interpretation.part);
    key.add(selected.interpretation.layer);
    key.add(std::string(config.begin(), config.end()));
    key.add(selected.available);
    key.add(locator->path);
    key.add(locator->relinkHint);
    // #381: the footage interpretation pass (alpha, deinterlace) is part of the converted image.
    if (selected.available && selected.sourceRevision) {
        selected.footagePass = footagePassFor(asset, selected.interpretation.alphaAssociation);
        if (selected.footagePass) {
            const auto* part = selected.sourceRevision->probe().findPart(source.part);
            const auto originY = part != nullptr ? part->dataY - part->displayY
                                                 : selected.sourceRevision->probe().dataOffsetY;
            selected.footagePass->rowParity = static_cast<std::uint8_t>(originY & 1);
        }
    }
    key.add(selected.footagePass ? selected.footagePass->identity() : std::string{});
    selected.cacheKey = key.bytes();
    // Decode-only identity: the selected member's content digest plus the requested alpha
    // association. Colour interpretation, input/working colour space, config revision, proxy and
    // path/relink fields are deliberately absent, so a changed working space or display keeps the
    // same decoded upload while a changed frame or alpha association is a miss.
    selected.decodeKey = imageDecodeKey(selected.digest, selected.footagePass.has_value(),
                                        selected.interpretation.alphaAssociation,
                                        selected.interpretation.part,
                                        selected.interpretation.layer);
    // Content-addressed and restart-stable (docs/architecture/media-io.md "Disk cache"): asset
    // digest + member frame + interpretation + Bloom Neutral config digest + decoder identity, so
    // it deliberately omits `path`/`relinkHint`/`available` -- a relink of the same content should
    // still hit the disk cache, and an unavailable selection never reaches evaluateImageSource().
    media::cache::ImageCacheKeyInputs diskInputs;
    diskInputs.contentDigest = selected.digest;
    diskInputs.memberFrame = memberFrame;
    diskInputs.colorSpace = selected.interpretation.colorSpace;
    diskInputs.inputColorSpaceId = selected.interpretation.inputColorSpaceId;
    diskInputs.workingColorSpaceId = selected.workingColorSpaceId;
    diskInputs.alphaAssociation = selected.interpretation.alphaAssociation;
    diskInputs.configDigest = selected.configRevision;
    diskInputs.synthesizedColorSpaceIdentity = synthesized;
    diskInputs.part = selected.interpretation.part;
    diskInputs.layer = selected.interpretation.layer;
    selected.diskCacheKey =
        media::cache::buildImageCacheKey(diskInputs, media::cache::kImageDecoderIdentity);
    return selected;
}
media::ImageResult<render::Rgba32fImage> evaluateImageSource(
    const ImageSourceSelection& selected, render::Rgba32fImageDescriptor composition,
    double horizontalScale, double verticalScale, std::size_t budget, OperationCache* memoryCache,
    ImageSourceMemoryCacheAccess memoryAccess, const CancellationToken& cancel,
    media::cache::MediaDiskCache* diskCache, media::ImageDecodeStatistics* decodeStatistics,
    LinearPassRetention* const linearPass) {
    if (decodeStatistics)
        *decodeStatistics = {};
    if (!selected.available)
        return {{}, selected.warning};
    if (selected.missingFrame) {
        auto missing = missingFrameImage(budget);
        if (!missing.value)
            return {{}, missing.diagnostic};
        return resampleDecodedImage(**missing.value, composition, horizontalScale, verticalScale,
                                    budget, cancel);
    }
    std::shared_ptr<const render::Rgba32fImage> image;
    std::uint64_t repairedPixels = 0;
    if (selected.footagePass) {
        // #381: stored samples -> footage pass -> input color, then the proxy resample. The disk
        // cache holds ordinary decodes only; the memory cache keeps the interpreted image.
        if (memoryAccess != ImageSourceMemoryCacheAccess::Disabled && memoryCache != nullptr)
            if (auto cached = memoryCache->find(selected.cacheKey, document::Revision{},
                                                OperationCacheEntryKind::DecodedMedia))
                image = cached->image;
        if (!image) {
            auto stored = decodeRawImageSource(selected, budget, cancel, decodeStatistics);
            if (!stored.value.has_value())
                return {{}, stored.diagnostic, stored.cancelled, stored.code};
            const auto cancelled = [&cancel] { return cancel.isCancellationRequested(); };
            auto interpreted =
                applyFootagePass(*selected.footagePass, **stored.value, budget, cancelled);
            if (auto* error = std::get_if<std::string>(&interpreted))
                return {{}, *error, cancel.isCancellationRequested()};
            if (selected.inputProcessor && !selected.inputProcessor->isIdentity()) {
                auto converted =
                    applyInputProcessorPremultiplied(std::get<render::Rgba32fImage>(interpreted),
                                                     *selected.inputProcessor, budget, cancelled);
                if (auto* error = std::get_if<std::string>(&converted))
                    return {{}, *error, cancel.isCancellationRequested()};
                interpreted = std::move(converted);
            }
            image = std::make_shared<const render::Rgba32fImage>(
                std::move(std::get<render::Rgba32fImage>(interpreted)));
            if (memoryAccess == ImageSourceMemoryCacheAccess::ReadWrite && memoryCache != nullptr)
                memoryCache->store(selected.cacheKey, document::Revision{},
                                   {.image = image, .values = {}, .bounds = {}},
                                   OperationCacheEntryKind::DecodedMedia, {}, linearPass);
        }
        return resampleDecodedImage(*image, composition, horizontalScale, verticalScale, budget,
                                    cancel);
    }
    if (memoryAccess != ImageSourceMemoryCacheAccess::Disabled && memoryCache != nullptr) {
        auto cached = memoryCache->find(selected.cacheKey, document::Revision{},
                                        OperationCacheEntryKind::DecodedMedia);
        if (cached)
            image = cached->image;
    }
    if (!image) {
        // Memory -> disk -> decode, but only an ordinary ReadWrite request may touch the disk
        // cache. A ReadOnly interactive miss decodes directly (null disk cache) and stores nothing,
        // and an explicit Disabled bypass decodes without consulting either cache.
        auto* const decodeDiskCache =
            memoryAccess == ImageSourceMemoryCacheAccess::ReadWrite ? diskCache : nullptr;
        // `budget` is the already-validated caller request allowance (the evaluator and the media
        // split both pass a budget derived from `request.pixelStorageByteLimit`). It is honored as
        // given so a large real source can use the caller's real budget instead of being clamped to
        // the 256 MiB default; the decoder still enforces its own structural ceilings. No
        // artificial min() is applied here.
        auto decoded = media::cache::decodeThroughDiskCache(
            selected.path, selected.interpretation,
            selected.expectedStamp ? std::nullopt : std::optional(selected.digest),
            selected.diskCacheKey, decodeDiskCache, /*writeAsync=*/true,
            [&] { return cancel.isCancellationRequested(); }, {}, budget, selected.inputProcessor,
            selected.sourceRevision, decodeStatistics);
        if (!decoded.value.has_value())
            return {{}, decoded.diagnostic, decoded.cancelled, decoded.code};
        image = std::move(*decoded.value);
        repairedPixels = decoded.repairedPixels;
        if (memoryAccess == ImageSourceMemoryCacheAccess::ReadWrite && memoryCache != nullptr)
            memoryCache->store(selected.cacheKey, document::Revision{},
                               {.image = image, .values = {}, .bounds = {}},
                               OperationCacheEntryKind::DecodedMedia, {}, linearPass);
    }
    auto resampled =
        resampleDecodedImage(*image, composition, horizontalScale, verticalScale, budget, cancel);
    resampled.repairedPixels = repairedPixels;
    return resampled;
}

namespace {

media::ImageInterpretation rawImageInterpretation(const ImageSourceSelection& selected) {
    media::ImageInterpretation rawInterpretation;
    rawInterpretation.colorSpace = media::ImageColorSpace::Raw;
    rawInterpretation.inputColorSpaceId = {};
    rawInterpretation.alphaAssociation = selected.interpretation.alphaAssociation;
    // #381: the footage pass reads the file's stored samples.
    rawInterpretation.storedSamples = selected.footagePass.has_value();
    rawInterpretation.part = selected.interpretation.part;
    rawInterpretation.layer = selected.interpretation.layer;
    return rawInterpretation;
}

} // namespace

media::ImageResult<std::shared_ptr<const render::Rgba32fImage>> decodeRawImageSource(
    const ImageSourceSelection& selected, const std::size_t budget, const CancellationToken& cancel,
    media::ImageDecodeStatistics* decodeStatistics, media::ImageChannelsAlongside* alongside) {
    if (decodeStatistics)
        *decodeStatistics = {};
    if (!selected.available)
        return {{}, selected.warning};
    if (selected.missingFrame)
        return missingFrameImage(budget);
    if (!selected.sourceRevision)
        return {{}, "Image selection has no immutable source revision"};
    return media::decodeImage(
        *selected.sourceRevision, rawImageInterpretation(selected), {},
        [&cancel] { return cancel.isCancellationRequested(); }, {}, budget, decodeStatistics,
        alongside);
}

media::ImageResult<std::shared_ptr<const render::NativeSampleImage>> decodeRawImageSamples(
    const ImageSourceSelection& selected, const std::size_t budget, const CancellationToken& cancel,
    media::ImageDecodeStatistics* decodeStatistics, media::ImageChannelsAlongside* alongside) {
    if (decodeStatistics)
        *decodeStatistics = {};
    if (!selected.available)
        return {{}, selected.warning};
    if (selected.missingFrame || !selected.sourceRevision)
        return {{},
                "The selection has no decoded native samples",
                false,
                media::ImageDiagnosticCode::NativeSamplesUnavailable};
    return media::decodeImageSamples(
        *selected.sourceRevision, rawImageInterpretation(selected),
        [&cancel] { return cancel.isCancellationRequested(); }, budget, decodeStatistics,
        alongside);
}

std::optional<render::ImageWindow>
frameRelativeDataWindow(const render::Rgba32fImageDescriptor& decoded) noexcept {
    const auto data = decoded.dataWindow();
    const auto frame = decoded.displayWindow();
    const auto window = render::ImageWindow::create(data.originX() - frame.originX(),
                                                    data.originY() - frame.originY(),
                                                    data.extent().width(), data.extent().height());
    if (!window)
        return std::nullopt;
    return *window.value();
}

std::optional<ContentBounds>
imageSourceFrameBounds(const ImageSourceSelection& selection) noexcept {
    if (selection.missingFrame)
        return ContentBounds{0.0, 0.0, static_cast<double>(selection.frameWidth),
                             static_cast<double>(selection.frameHeight)};
    if (!selection.sourceRevision)
        return std::nullopt;
    const auto& probe = selection.sourceRevision->probe();
    // ADR 0026: a selected OpenEXR part carries its own data and display windows.
    if (const auto* part = probe.findPart(selection.interpretation.part)) {
        if (part->dataX == part->displayX && part->dataY == part->displayY &&
            part->dataWidth == part->displayWidth && part->dataHeight == part->displayHeight)
            return std::nullopt;
        return ContentBounds{0.0, 0.0, static_cast<double>(part->displayWidth),
                             static_cast<double>(part->displayHeight)};
    }
    if (probe.dataOffsetX == 0 && probe.dataOffsetY == 0 &&
        probe.effectiveFrameWidth() == probe.width && probe.effectiveFrameHeight() == probe.height)
        return std::nullopt;
    return ContentBounds{0.0, 0.0, static_cast<double>(probe.effectiveFrameWidth()),
                         static_cast<double>(probe.effectiveFrameHeight())};
}

media::ImageResult<render::Rgba32fImage>
resampleDecodedImage(const render::Rgba32fImage& decoded,
                     const render::Rgba32fImageDescriptor composition, const double horizontalScale,
                     const double verticalScale, const std::size_t budget,
                     const CancellationToken& cancel) {
    if (decoded.descriptor() == nullptr)
        return {{}, "Decoded image has no descriptor"};
    const auto source = frameRelativeDataWindow(*decoded.descriptor());
    const auto window =
        source ? render::pointResampleProxyWindow(*source, horizontalScale, verticalScale)
               : std::nullopt;
    if (!window)
        return {{}, "Image proxy dimensions are invalid"};
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window, composition.displayWindow(), composition.pixelAspect());
    if (!descriptor)
        return {{}, "Image source descriptor is invalid"};
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return {{},
                "Image source exceeds the pixel budget",
                false,
                media::ImageDiagnosticCode::PixelBudgetExceeded};
    const auto sourceWidth = source->extent().width();
    const auto sourceHeight = source->extent().height();
    std::vector<std::uint32_t> columns(window->extent().width());
    for (std::uint32_t x = 0; x < window->extent().width(); ++x)
        columns[x] = render::pointResampleSourceIndex(window->originX() + x, source->originX(),
                                                      sourceWidth, horizontalScale);
    for (std::uint32_t y = 0; y < window->extent().height(); ++y) {
        if (cancel.isCancellationRequested())
            return {{}, {}, true};
        const auto outputY = window->originY() + y;
        auto row = builder.value()->row(outputY);
        const auto sourceY = render::pointResampleSourceIndex(outputY, source->originY(),
                                                              sourceHeight, verticalScale);
        for (std::uint32_t x = 0; x < window->extent().width(); ++x)
            (*row.value())[x] =
                decoded.pixels()[static_cast<std::size_t>(sourceY) * sourceWidth + columns[x]];
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return {{}, "Image source pixel validation failed"};
    return {std::move(*frozen.value()), {}};
}
} // namespace bloom::runtime::detail
