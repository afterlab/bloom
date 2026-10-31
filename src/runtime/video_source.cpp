#include "video_source.hpp"
#include "image_source.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"
#include <algorithm>
#include <bloom/media/image.hpp>
#include <bloom/media/video/colour.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/footage_time.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/runtime/video_asset.hpp>
#include <limits>
#include <numeric>
namespace bloom::runtime::detail {
namespace {
struct SessionRetirement final : render::detail::PackedHostRetirementNode {
    std::unique_ptr<media::video::VideoDecodeSession> session;
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    bool registered = false;
    ~SessionRetirement() {
        session.reset();
        if (registered)
            render::detail::releasePackedHostRegistration(retirement);
    }
};
std::shared_ptr<media::video::VideoDecodeSession>
makeSession(const std::filesystem::path& path,
            const std::shared_ptr<platform::FileSnapshotQuota>& quota,
            const std::shared_ptr<render::detail::PackedHostRetirementState>& retirement) {
    auto owner = std::make_unique<SessionRetirement>();
    owner->retirement = retirement;
    if (retirement) {
        if (!render::detail::preparePackedHostRetirement(retirement) ||
            !render::detail::registerPackedHostAllocation(retirement))
            return {};
        owner->registered = true;
    }
    owner->session = std::make_unique<media::video::VideoDecodeSession>(
        path, media::video::VideoDecodeSession::defaultWorker(), quota, std::filesystem::path{},
        retirement);
    owner->destroy = [](render::detail::PackedHostRetirementNode* node) noexcept {
        auto* entry = static_cast<SessionRetirement*>(node);
        entry->registered = false; // The queue releases its own registration after destroy.
        delete entry;
    };
    auto* entry = owner.release();
    return std::shared_ptr<media::video::VideoDecodeSession>(
        entry->session.get(), [entry](media::video::VideoDecodeSession*) noexcept {
            if (entry->registered)
                render::detail::enqueuePackedHostRetirement(entry->retirement, entry);
            else
                delete entry;
        });
}
} // namespace
void VideoSourceContext::setSessionByteBudget(const std::size_t bytes) {
    std::list<SessionEntry> detached;
    const std::lock_guard lock(mutex_);
    sessionByteBudget_ = bytes;
    admitLocked(detached);
}
std::size_t VideoSourceContext::retainedSessionCount() {
    const std::lock_guard lock(mutex_);
    return sessions_.size();
}
void VideoSourceContext::admitLocked(std::list<SessionEntry>& detached) {
    if (sessionByteBudget_ == 0)
        sessionByteBudget_ =
            std::max<std::size_t>(physicalMemoryBytes() / 4U, std::size_t{2} << 30U);
    std::size_t total = 0;
    std::size_t kept = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        total += it->workerBytes;
        const bool admitted = kept < kSnapshotFiles &&
                              (kept < kMinimumRetainedSessions || total <= sessionByteBudget_);
        if (admitted) {
            ++kept;
            ++it;
        } else {
            total -= it->workerBytes;
            detached.splice(detached.end(), sessions_, it++);
        }
    }
}
std::shared_ptr<media::video::VideoDecodeSession>
VideoSourceContext::session(const std::string& key, const std::filesystem::path& path,
                            const bool allowInsertion, const std::size_t workerBytes) {
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return {};
        const auto found = std::ranges::find(sessions_, key, &SessionEntry::key);
        if (found != sessions_.end() && found->path == path) {
            sessions_.splice(sessions_.begin(), sessions_, found);
            return sessions_.front().session;
        }
        retirement = retirement_;
        generation = generation_;
    }
    // Worker bootstrap, library verification and scheduler creation hold no mutex needed by UI
    // source close. A refresh racing construction prevents insertion into the new generation.
    auto session = makeSession(path, quota_, retirement);
    std::list<SessionEntry> detached;
    if (session && allowInsertion) {
        std::lock_guard lock(mutex_);
        if (closed_ || generation != generation_ ||
            generation == std::numeric_limits<std::uint64_t>::max())
            return session;
        const auto found = std::ranges::find(sessions_, key, &SessionEntry::key);
        if (found != sessions_.end())
            detached.splice(detached.end(), sessions_, found);
        sessions_.push_front({key, path, session, workerBytes});
        admitLocked(detached);
    }
    return session;
}
std::shared_ptr<media::video::VideoDecodeSession>
VideoSourceContext::retainedSession(const std::string& key) {
    std::lock_guard lock(mutex_);
    if (closed_)
        return {};
    const auto found = std::ranges::find(sessions_, key, &SessionEntry::key);
    if (found == sessions_.end())
        return {};
    sessions_.splice(sessions_.begin(), sessions_, found);
    return sessions_.front().session;
}
void VideoSourceContext::discardFailedSession(
    const std::string& key, const std::shared_ptr<media::video::VideoDecodeSession>& session) {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::ranges::find(sessions_, key, &SessionEntry::key);
        if (found != sessions_.end() && found->session == session)
            detached.splice(detached.end(), sessions_, found);
    }
}
void VideoSourceContext::bindRetirement(
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement) {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        if (retirement_ != retirement) {
            detached.splice(detached.end(), sessions_);
            retirement_ = std::move(retirement);
            if (generation_ != std::numeric_limits<std::uint64_t>::max())
                ++generation_;
        }
    }
}
void VideoSourceContext::report(VideoSourceReport value) {
    const std::lock_guard lock(reportMutex_);
    value.sequence = ++reportSequence_;
    std::erase_if(reports_,
                  [&](const auto& entry) { return entry.assetDigest == value.assetDigest; });
    reports_.insert(reports_.begin(), std::move(value));
    if (reports_.size() > kReportedSources)
        reports_.resize(kReportedSources);
}
std::vector<VideoSourceReport> VideoSourceContext::reports() const {
    const std::lock_guard lock(reportMutex_);
    return reports_;
}
void VideoSourceContext::clear() {
    invalidateSources();
    cache.clear(); // Explicit refresh is off-UI; signal-only shutdown does not free decoded planes.
}
void VideoSourceContext::invalidateSources() {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        detached.splice(detached.end(), sessions_);
        if (generation_ != std::numeric_limits<std::uint64_t>::max())
            ++generation_;
    }
}
void VideoSourceContext::beginShutdown() {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        detached.splice(detached.end(), sessions_);
    }
}
VideoSourceSelection selectVideoSource(const CompiledVideoSource& source, core::RationalTime time,
                                       document::FrameRate rate, const std::filesystem::path& base,
                                       VideoSourceContext& context, bool useCache,
                                       const CancellationToken& cancel,
                                       const EvaluationColorIntent& colorIntent,
                                       const MediaSourceRevisionPolicy revisionPolicy,
                                       const bool allowInsertion,
                                       const std::size_t hostByteBudget) {
    VideoSourceSelection selection;
    if (!source.asset || source.asset->kind != document::AssetKind::Video) {
        selection.warning = "Video asset is missing; choose an asset or relink in Assets";
        selection.cacheKey = "missing-video";
        return selection;
    }
    const auto& asset = *source.asset;
    const auto stream =
        std::ranges::find(asset.videoStreams, 1U, &document::AssetVideoStream::kind);
    if (stream == asset.videoStreams.end() || stream->framePeriod.numerator() <= 0 ||
        stream->framePeriod.numerator() > std::numeric_limits<std::uint32_t>::max() ||
        stream->framePeriod.denominator() > std::numeric_limits<std::uint32_t>::max() ||
        asset.frames == 0) {
        selection.warning = "Video stream timing is unavailable";
        return selection;
    }
    // #381: the conformed (or file) rate, the asset's Loop count and the node's start and loop.
    const auto frame = footageFrameIndex(asset, time, source.startFrame, source.loopMode, rate);
    if (!frame) {
        selection.warning = "Video time exceeds the supported range";
        return selection;
    }
    const auto index = *frame;
    const auto input = document::effectiveAssetInputColor(asset.interpretation, source.colorSpace,
                                                          source.inputColorSpaceId);
    selection.interpretation = static_cast<std::uint32_t>(input.colorSpace);
    selection.inputColorSpaceId = input.inputColorSpaceId;
    auto config = resolveInputColorConfig(colorIntent);
    if (!config) {
        selection.warning = "The selected OCIO input configuration is unavailable";
        selection.cacheKey = "video-colour-config-unavailable";
        return selection;
    }
    const media::provider::ColourTags tags{
        static_cast<std::int32_t>(stream->primaries), static_cast<std::int32_t>(stream->transfer),
        static_cast<std::int32_t>(stream->matrix), static_cast<std::int32_t>(stream->range)};
    // Owner decision (2026-10-24): missing SDR tags are assumed by picture size and rate; the
    // assumption is part of the transform identity below and its note is shown.
    const media::video::VideoGeometry geometry{
        stream->width, stream->height,
        stream->framePeriod.numerator() > 0
            ? static_cast<double>(stream->framePeriod.denominator()) /
                  static_cast<double>(stream->framePeriod.numerator())
            : 0.0};
    const auto assumedColour = media::video::assumeMissingVideoColour(tags, geometry);
    const auto videoResolution = media::video::resolveVideoInputColorSpace(
        *config, tags, selection.interpretation, selection.inputColorSpaceId, geometry);
    const InputColorSpaceResolution resolution{
        videoResolution.id, videoResolution.name, videoResolution.warning,
        videoResolution.noConversion, videoResolution.automatic};
    selection.inputColorSpaceAutomatic = resolution.automatic;
    selection.resolvedInputColorSpaceName = resolution.name;
    selection.inputColorSpaceWarning = resolution.warning;
    selection.workingColorSpaceId = std::string(config->processColorSpaceId());
    selection.configRevision = config->expectedRevision();
    if (!resolution.warning.empty() && resolution.id.empty()) {
        selection.warning = resolution.warning;
        selection.cacheKey = "video-colour-input-unresolved";
        return selection;
    }
    selection.inputColorSpaceId = resolution.id;
    std::string diagnostic;
    selection.inputProcessor = prepareInputColorProcessor(*config, resolution, diagnostic);
    if (!resolution.noConversion && !selection.inputProcessor) {
        selection.warning = diagnostic;
        selection.cacheKey = "video-colour-input-unavailable";
        return selection;
    }
    OperationKey sourceKey;
    const auto sourceDigest = asset.contentDigest.toLowercaseHex();
    sourceKey.add(std::string(sourceDigest.begin(), sourceDigest.end()));
    sourceKey.add(asset.locator.path);
    sourceKey.add(asset.locator.relinkHint);
    const auto baseUtf8 = base.generic_u8string();
    sourceKey.add(std::string(baseUtf8.begin(), baseUtf8.end()));
    const auto revisionKey = sourceKey.digest();
    auto session = revisionPolicy == MediaSourceRevisionPolicy::Retained
                       ? context.retainedSession(revisionKey)
                       : nullptr;
    if (!session) {
        const auto path =
            media::resolveImagePath(asset.locator.path, asset.locator.relinkHint, base);
        session =
            context.session(revisionKey, path, allowInsertion,
                            media::video::estimatedDecodeWorkerBytes(asset.width, asset.height));
    }
    if (!session) {
        selection.warning = "Video source owner is closing";
        selection.cancelled = true;
        return selection;
    }
    // Retained revisions serve interactive preview, which may use the discovered VA-API decoder;
    // revalidated (final/export) requests always decode through the software reference.
    media::video::FrameDecodeProvenance decodeProvenance;
    const auto product =
        session->frame(runtime::video::probeMetadata(asset), stream->id, index,
                       selection.interpretation, useCache ? &context.cache : nullptr,
                       selection.inputColorSpaceId, selection.workingColorSpaceId,
                       selection.configRevision, [&] { return cancel.isCancellationRequested(); },
                       {revisionPolicy == MediaSourceRevisionPolicy::Retained
                            ? media::video::SourceRevisionPolicy::Retained
                            : media::video::SourceRevisionPolicy::Revalidate,
                        allowInsertion,
                        &selection.sourceWork,
                        &selection.retainedRevision,
                        {},
                        hostByteBudget},
                       {revisionPolicy == MediaSourceRevisionPolicy::Retained, &decodeProvenance});
    if (const auto* error = std::get_if<media::provider::Unavailable>(&product)) {
        selection.warning = error->detail;
        selection.cancelled = error->reason == media::provider::Error::Cancelled;
        // A failed cold capture must not pin an unresolved/offline locator. On reconnect the
        // next request resolves the locator again. Pointer identity protects a newer generation.
        if (allowInsertion)
            context.discardFailedSession(revisionKey, session);
    } else
        selection.frame = std::get<std::shared_ptr<const media::provider::FrameProduct>>(product);
    OperationKey key;
    const auto digest = asset.contentDigest.toLowercaseHex();
    key.add(std::string(digest.begin(), digest.end()));
    key.add(stream->id);
    key.add(index);
    key.add(selection.interpretation);
    key.add(selection.inputColorSpaceId);
    key.add(selection.workingColorSpaceId);
    // A Bloom-synthesized input space (Rec.2100 HLG) is defined by OCIO built-ins, so its
    // definition and the linked OCIO version are part of the transform identity.
    key.add(color::synthesizedColorSpaceIdentity(selection.inputColorSpaceId));
    key.add(assumedColour.identity);
    const auto revision = selection.configRevision.toLowercaseHex();
    key.add(std::string(revision.begin(), revision.end()));
    key.add(selection.frame != nullptr);
    key.add(selection.warning);
    const bool hardwareDecoded =
        decodeProvenance.implementation == media::provider::Implementation::Hardware;
    key.add(hardwareDecoded);
    // #381: a video's alpha reads as straight unless Interpret Footage says premultiplied.
    selection.footagePass = footagePassFor(asset, asset.interpretation.alphaMode ==
                                                          document::AssetAlphaMode::Premultiplied
                                                      ? media::ImageAlphaAssociation::Premultiplied
                                                      : media::ImageAlphaAssociation::Straight);
    key.add(selection.footagePass ? selection.footagePass->identity() : std::string{});
    selection.decodeProvenance = decodeProvenance.description;
    selection.cacheKey = key.bytes();
    if (selection.frame)
        context.report({std::string(digest.begin(), digest.end()), decodeProvenance.description,
                        assumedColour.assumed && selection.inputColorSpaceAutomatic
                            ? selection.inputColorSpaceWarning
                            : std::string{},
                        0});
    // Decode-only identity for the raw codec-side preparation. It deliberately omits the input and
    // working colour spaces, config revision, display and proxy so a changed working space reuses
    // the decoded frame.
    OperationKey decodeKey;
    decodeKey.add(std::string{"gpu-raw-video-prep-v1"});
    decodeKey.add(std::string(digest.begin(), digest.end()));
    decodeKey.add(stream->id);
    decodeKey.add(index);
    // A hardware-decoded preview frame is a distinct preparation from the software reference.
    if (hardwareDecoded)
        decodeKey.add(std::string{"vaapi"});
    if (selection.footagePass)
        decodeKey.add(std::string{"stored-samples-v1"});
    selection.decodeKey = decodeKey.digest();
    return selection;
}

media::provider::Result<render::Rgba32fImage>
convertSelectedVideo(const VideoSourceSelection& selection,
                     const render::Rgba32fImageDescriptor composition, const double horizontalScale,
                     const double verticalScale, const std::size_t budget,
                     const std::function<bool()>& cancel) {
    if (!selection.frame)
        return media::provider::Unavailable{media::provider::Error::Unavailable,
                                            "Video frame is unavailable"};
    if (!selection.footagePass)
        return media::video::videoToSceneLinear(
            *selection.frame, selection.interpretation, selection.inputColorSpaceId,
            selection.inputProcessor, composition, horizontalScale, verticalScale, budget, cancel);
    // #381: stored samples at full resolution -> footage pass -> input colour -> proxy resample.
    auto stored = media::video::videoToStoredSamples(
        *selection.frame, selection.interpretation,
        selection.inputProcessor ? std::string_view(selection.inputColorSpaceId)
                                 : std::string_view{},
        composition, budget, cancel);
    if (auto* error = std::get_if<media::provider::Unavailable>(&stored))
        return *error;
    auto interpreted = applyFootagePass(*selection.footagePass,
                                        std::get<render::Rgba32fImage>(stored), budget, cancel);
    const auto failed =
        [&](const std::string& reason) -> media::provider::Result<render::Rgba32fImage> {
        if (cancel && cancel())
            return media::provider::Unavailable{media::provider::Error::Cancelled, "Cancelled"};
        return media::provider::Unavailable{media::provider::Error::InvalidValue, reason};
    };
    if (auto* error = std::get_if<std::string>(&interpreted))
        return failed(*error);
    if (selection.inputProcessor) {
        auto converted = applyInputProcessorPremultiplied(
            std::get<render::Rgba32fImage>(interpreted), *selection.inputProcessor, budget, cancel);
        if (auto* error = std::get_if<std::string>(&converted))
            return failed(*error);
        interpreted = std::move(converted);
    }
    CancellationToken token;
    auto resampled = resampleDecodedImage(std::get<render::Rgba32fImage>(interpreted), composition,
                                          horizontalScale, verticalScale, budget, token);
    if (!resampled.value.has_value())
        return failed(resampled.diagnostic);
    return std::move(*resampled.value);
}
} // namespace bloom::runtime::detail

namespace bloom::runtime {
std::shared_ptr<detail::VideoSourceContext> CpuCompositionEvaluator::videoContext() const {
    std::lock_guard lock(assetContext_->mutex);
    if (!assetContext_->video)
        assetContext_->video = std::make_shared<detail::VideoSourceContext>();
    assetContext_->video->bindRetirement(cache_->sourceRetirementState());
    cache_->bindVideoSources(assetContext_->video);
    return assetContext_->video;
}
void CpuCompositionEvaluator::setVideoCacheByteBudget(std::size_t budget) const {
    videoContext()->cache.setByteBudget(budget);
}
void CpuCompositionEvaluator::clearDecodedVideoCache() const { videoContext()->clear(); }
} // namespace bloom::runtime
