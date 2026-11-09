#include "video_source.hpp"
#include "image_source.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"
#include <algorithm>
#include <bloom/media/image.hpp>
#include <bloom/media/video/colour.hpp>
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/image_statistics.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/footage_time.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/runtime/video_asset.hpp>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>
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
        sessionByteBudget_ = processMemoryBudgetLedger().defaultTotalByteBudget();
    std::size_t total = 0;
    // Distinct sources kept, most recent first: the parallel sessions of one intra-only source
    // share its snapshot, so the file quota and the two-source minimum count sources.
    std::vector<const std::string*> sources;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        total += it->workerBytes;
        const bool known =
            std::ranges::any_of(sources, [&](const std::string* key) { return *key == it->key; });
        const bool admitted =
            known ? total <= sessionByteBudget_
                  : sources.size() < kSnapshotFiles &&
                        (sources.size() < kMinimumRetainedSessions || total <= sessionByteBudget_);
        if (admitted) {
            if (!known)
                sources.push_back(&it->key);
            ++it;
        } else {
            total -= it->workerBytes;
            detached.splice(detached.end(), sessions_, it++);
        }
    }
}
bool videoFramesDecodeIndependently(const std::string_view codec) noexcept {
    return codec == "prores" || codec == "dnxhd" || codec == "mjpeg" || codec == "tiff";
}
std::optional<std::size_t>
chooseOrderedDecoderLane(const std::span<const media::video::DecoderReach> lanes,
                         const std::uint64_t frame, const bool mayStartLane) noexcept {
    std::optional<std::size_t> nearest, unplaced, furthestBack;
    for (std::size_t index = 0; index < lanes.size(); ++index) {
        const auto& lane = lanes[index];
        if (!lane.lowest) {
            // A decoder with no frames starts from a keyframe whatever it is given: using it sends
            // no other decoder back.
            if (!unplaced)
                unplaced = index;
            continue;
        }
        if (lane.nearest && *lane.nearest <= frame &&
            (!nearest || *lane.nearest > *lanes[*nearest].nearest))
            nearest = index;
        if (!furthestBack || *lane.lowest < *lanes[*furthestBack].lowest)
            furthestBack = index;
    }
    if (nearest)
        return nearest;
    if (unplaced)
        return unplaced;
    if (mayStartLane)
        return std::nullopt;
    return furthestBack;
}
std::list<VideoSourceContext::SessionEntry>::iterator
VideoSourceContext::chooseLocked(const std::string& key, const std::filesystem::path* path,
                                 const VideoSessionUse& use) {
    const auto matches = [&](const SessionEntry& entry) {
        return entry.key == key && (path == nullptr || entry.path == *path);
    };
    if (!use.independentFrames) {
        // #474: the decoder that reaches this frame without seeking back.
        std::vector<std::list<SessionEntry>::iterator> entries;
        std::vector<media::video::DecoderReach> lanes;
        std::size_t total = 0;
        std::size_t laneBytes = 0;
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            total += it->workerBytes;
            if (!matches(*it))
                continue;
            entries.push_back(it);
            lanes.push_back(it->session->reach(use.stream, use.frame));
            laneBytes = std::max(laneBytes, it->workerBytes);
        }
        if (lanes.empty())
            return sessions_.end();
        if (sessionByteBudget_ == 0)
            sessionByteBudget_ = processMemoryBudgetLedger().defaultTotalByteBudget();
        // Another lane only beside the others, never by evicting them.
        const bool mayStartLane = lanes.size() < kOrderedDecoderLanes &&
                                  laneBytes <= sessionByteBudget_ &&
                                  total <= sessionByteBudget_ - laneBytes;
        const auto chosen = chooseOrderedDecoderLane(lanes, use.frame, mayStartLane);
        return chosen ? entries[*chosen] : sessions_.end();
    }
    const std::pair frame{use.stream, use.frame};
    auto idle = sessions_.end();
    auto least = sessions_.end();
    std::size_t count = 0;
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (!matches(*it))
            continue;
        ++count;
        const auto users = it->users->load();
        const auto occupancy = it->session->occupancy(use.stream, use.frame);
        // The session decoding (or handed out for) this frame: join it.
        if (occupancy == media::video::VideoDecodeSession::Occupancy::Joins ||
            (users != 0 && it->leasedFrame == frame))
            return it;
        if (idle == sessions_.end() && users == 0 &&
            occupancy == media::video::VideoDecodeSession::Occupancy::Idle)
            idle = it;
        if (least == sessions_.end() || users < least->users->load())
            least = it;
    }
    if (idle != sessions_.end())
        return idle;
    // Every session is busy: another decoder while the source has fewer than one per hardware
    // thread (each decoder keeps at least one busy), else queue on the least occupied.
    if (count < std::max(1U, std::thread::hardware_concurrency()))
        return sessions_.end();
    return least;
}
std::shared_ptr<media::video::VideoDecodeSession>
VideoSourceContext::handOutLocked(const std::list<SessionEntry>::iterator entry,
                                  const VideoSessionUse& use) {
    sessions_.splice(sessions_.begin(), sessions_, entry);
    auto& front = sessions_.front();
    if (!use.independentFrames)
        return front.session;
    // The returned handle counts as a user of the entry until the caller releases it.
    struct Lease final {
        Lease(std::shared_ptr<media::video::VideoDecodeSession> held,
              std::shared_ptr<std::atomic<std::size_t>> count)
            : session(std::move(held)), users(std::move(count)) {
            ++*users;
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() { --*users; }
        std::shared_ptr<media::video::VideoDecodeSession> session;
        std::shared_ptr<std::atomic<std::size_t>> users;
    };
    front.leasedFrame = std::pair{use.stream, use.frame};
    const auto lease = std::make_shared<Lease>(front.session, front.users);
    return {lease, lease->session.get()};
}
std::shared_ptr<media::video::VideoDecodeSession>
VideoSourceContext::session(const std::string& key, const std::filesystem::path& path,
                            const bool allowInsertion, const std::size_t workerBytes,
                            const VideoSessionUse use) {
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return {};
        if (const auto found = chooseLocked(key, &path, use); found != sessions_.end())
            return handOutLocked(found, use);
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
        // A source decoded in order keeps at most kOrderedDecoderLanes sessions of its current
        // path (the most recently used); an intra-only source adds this one beside its others.
        if (!use.independentFrames) {
            std::size_t lanes = 1;
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->key == key && (it->path != path || ++lanes > kOrderedDecoderLanes))
                    detached.splice(detached.end(), sessions_, it++);
                else
                    ++it;
            }
        }
        sessions_.push_front({.key = key,
                              .path = path,
                              .session = session,
                              .workerBytes = workerBytes,
                              .users = std::make_shared<std::atomic<std::size_t>>(0),
                              .leasedFrame = std::nullopt});
        admitLocked(detached);
        if (!sessions_.empty() && sessions_.front().session == session)
            return handOutLocked(sessions_.begin(), use);
    }
    return session;
}
std::shared_ptr<media::video::VideoDecodeSession>
VideoSourceContext::retainedSession(const std::string& key, const VideoSessionUse use) {
    std::lock_guard lock(mutex_);
    if (closed_)
        return {};
    const auto found = chooseLocked(key, nullptr, use);
    if (found == sessions_.end())
        return {};
    return handOutLocked(found, use);
}
void VideoSourceContext::discardFailedSession(
    const std::string& key, const std::shared_ptr<media::video::VideoDecodeSession>& session) {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::ranges::find_if(sessions_, [&](const SessionEntry& entry) {
            return entry.key == key && entry.session.get() == session.get();
        });
        if (found != sessions_.end())
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
    // #474: an intra-only stream decodes concurrent frames on parallel sessions.
    const VideoSessionUse use{.independentFrames = videoFramesDecodeIndependently(stream->codec),
                              .stream = stream->id,
                              .frame = index};
    auto session = revisionPolicy == MediaSourceRevisionPolicy::Retained
                       ? context.retainedSession(revisionKey, use)
                       : nullptr;
    if (!session) {
        const auto path =
            media::resolveImagePath(asset.locator.path, asset.locator.relinkHint, base);
        session = context.session(
            revisionKey, path, allowInsertion,
            media::video::estimatedDecodeWorkerBytes(asset.width, asset.height), use);
    }
    if (!session) {
        selection.warning = "Video source owner is closing";
        selection.cancelled = true;
        return selection;
    }
    // Preview and final (revalidated) requests alike may use the host's hardware decoder (ADR
    // 0027, amended 2026-11-04): its first frame is proven byte-identical to the software
    // reference per source, a refusal falls back to software permanently, and the route stays in
    // every cache key below.
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
                       {true, &decodeProvenance});
    if (const auto* error = std::get_if<media::provider::Unavailable>(&product)) {
        selection.warning = error->detail;
        selection.cancelled = error->reason == media::provider::Error::Cancelled;
        // A failed cold capture must not pin an unresolved/offline locator. On reconnect the
        // next request resolves the locator again. Pointer identity protects a newer generation.
        // A cancelled or shut-down request is not a failure of the source: the session keeps its
        // decoder and position. Discarding it there made playback, which cancels requests all the
        // time, start a new decoder for almost every frame, and a long-GOP stream then decoded
        // again from its keyframe (#474).
        if (allowInsertion && error->reason != media::provider::Error::Cancelled &&
            error->reason != media::provider::Error::Shutdown)
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
    key.add(hardwareDecoded ? decodeProvenance.description : std::string{});
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
    // A hardware-decoded frame is a distinct preparation from the software reference; the route
    // ("vaapi" or "nvdec") is part of its identity.
    if (hardwareDecoded)
        decodeKey.add(decodeProvenance.description);
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
    // #381: stored samples at full resolution -> footage pass -> input color -> proxy resample.
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
    if (selection.inputProcessor && !selection.inputProcessor->isIdentity()) {
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
    // Decoded frames follow the same playback working set as the decoded-media entries.
    assetContext_->video->cache.bindSourceWindow(cache_->sourceWindow());
    return assetContext_->video;
}
void CpuCompositionEvaluator::setVideoCacheByteBudget(std::size_t budget) const {
    videoContext()->cache.setByteBudget(budget);
}
void CpuCompositionEvaluator::clearDecodedVideoCache() const {
    videoContext()->clear();
    // #347: the prepared audio analyses and their opened sources are decoded media too.
    clearAudioAnalysis();
    // #349: the prepared statistics are derived from decoded media too.
    clearImageStatistics();
}
} // namespace bloom::runtime
