#include "gpu_media_preparation.hpp"
#include "channel_shuffle.hpp"
#include "cryptomatte.hpp"
#include <bloom/runtime/image_sequence_read_ahead.hpp>

#include "image_source.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"
#include "video_source.hpp"

#include "cpu_composition_evaluator_support.hpp"

#include <bloom/core/telemetry_counters.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/media/video/colour.hpp>
#include <bloom/render/gpu_point_resample.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/native_samples.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <variant>

namespace bloom::runtime {

GpuSceneMediaContext GpuSceneMediaContext::fromEvaluator(const CpuCompositionEvaluator& evaluator) {
    GpuSceneMediaContext context;
    context.operationCache = evaluator.operationCache();
    context.videoContext = evaluator.videoContext();
    context.assetBaseDirectory = evaluator.assetBaseDirectory();
    context.sequenceReadAhead = evaluator.sequenceReadAhead();
    return context;
}

std::shared_ptr<detail::VideoSourceContext> GpuSceneMediaContext::makeVideoSources() {
    return std::make_shared<detail::VideoSourceContext>();
}

std::shared_ptr<const render::Rgba32fImage>
GpuSceneBuildUploads::find(const std::string& key) const {
    const std::lock_guard lock(mutex_);
    const auto found = images_.find(key);
    return found == images_.end() ? nullptr : found->second;
}

void GpuSceneBuildUploads::share(const std::string& key,
                                 std::shared_ptr<const render::Rgba32fImage> image) {
    if (!image)
        return;
    try {
        const std::lock_guard lock(mutex_);
        images_.try_emplace(key, std::move(image));
    } catch (const std::bad_alloc&) {
        // Sharing is an optimization inside one build; a later leaf converts its own copy.
    }
}

std::shared_ptr<const detail::CryptomatteGather>
GpuSceneBuildUploads::findCryptomatte(const std::string& key) const {
    const std::lock_guard lock(mutex_);
    const auto found = cryptomattes_.find(key);
    return found == cryptomattes_.end() ? nullptr : found->second;
}

void GpuSceneBuildUploads::shareCryptomatte(
    const std::string& key, std::shared_ptr<const detail::CryptomatteGather> gather) {
    if (!gather)
        return;
    try {
        const std::lock_guard lock(mutex_);
        cryptomattes_.try_emplace(key, std::move(gather));
    } catch (const std::bad_alloc&) {
        // Sharing is an optimization inside one build; a later matte gathers its own layers.
    }
}

std::shared_ptr<const render::NativeSampleImage>
GpuSceneBuildUploads::findSamples(const std::string& key) const {
    const std::lock_guard lock(mutex_);
    const auto found = samples_.find(key);
    return found == samples_.end() ? nullptr : found->second;
}

void GpuSceneBuildUploads::shareSamples(const std::string& key,
                                        std::shared_ptr<const render::NativeSampleImage> samples) {
    if (!samples)
        return;
    try {
        const std::lock_guard lock(mutex_);
        samples_.try_emplace(key, std::move(samples));
    } catch (const std::bad_alloc&) {
        // Sharing is an optimization inside one build; a later leaf decodes its own samples.
    }
}

void GpuSceneBuildUploads::wantChannels(const std::string& part,
                                        const std::vector<std::string>& names) {
    try {
        const std::lock_guard lock(mutex_);
        auto& wanted = wantedChannels_[part];
        for (const auto& name : names)
            if (const auto at = std::ranges::lower_bound(wanted, name);
                at == wanted.end() || *at != name)
                wanted.insert(at, name);
    } catch (const std::bad_alloc&) {
        // Registration is an optimization; each channel read then decodes what it reads.
    }
}

std::vector<std::string> GpuSceneBuildUploads::wantedChannels(const std::string& part) const {
    const std::lock_guard lock(mutex_);
    const auto found = wantedChannels_.find(part);
    return found == wantedChannels_.end() ? std::vector<std::string>{} : found->second;
}

std::shared_ptr<const render::ChannelSetImage>
GpuSceneBuildUploads::findChannels(const std::string& part) const {
    const std::lock_guard lock(mutex_);
    const auto found = channels_.find(part);
    return found == channels_.end() ? nullptr : found->second;
}

void GpuSceneBuildUploads::shareChannels(const std::string& part,
                                         std::shared_ptr<const render::ChannelSetImage> channels) {
    if (!channels)
        return;
    try {
        const std::lock_guard lock(mutex_);
        channels_.insert_or_assign(part, std::move(channels));
    } catch (const std::bad_alloc&) {
        // Sharing is an optimization inside one build; a later read decodes its own channels.
    }
}

void GpuSceneBuildUploads::planColour(
    const std::string& part, std::shared_ptr<const detail::ImageSourceSelection> selection) {
    if (!selection)
        return;
    try {
        const std::lock_guard lock(mutex_);
        colour_.try_emplace(part, std::move(selection));
    } catch (const std::bad_alloc&) {
        // Planning is an optimization; the Image leaf then decodes its own colour.
    }
}

std::shared_ptr<const detail::ImageSourceSelection>
GpuSceneBuildUploads::findColour(const std::string& part) const {
    const std::lock_guard lock(mutex_);
    const auto found = colour_.find(part);
    return found == colour_.end() ? nullptr : found->second;
}

} // namespace bloom::runtime

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] std::shared_ptr<const render::NativeSampleImage>
findSharedSamples(const GpuSceneMediaContext& context, const std::string& key) {
    return context.buildUploads ? context.buildUploads->findSamples(key) : nullptr;
}

void shareSamples(const GpuSceneMediaContext& context, const std::string& key,
                  const std::shared_ptr<const render::NativeSampleImage>& samples) {
    if (context.buildUploads)
        context.buildUploads->shareSamples(key, samples);
}

[[nodiscard]] std::shared_ptr<const render::Rgba32fImage>
findSharedUpload(const GpuSceneMediaContext& context, const std::string& key) {
    return context.buildUploads ? context.buildUploads->find(key) : nullptr;
}

void shareUpload(const GpuSceneMediaContext& context, const std::string& key,
                 const std::shared_ptr<const render::Rgba32fImage>& image) {
    if (context.buildUploads)
        context.buildUploads->share(key, image);
}

// Host upload staging (scratch, ADR 0037): the converted pixels live only while the scene's upload
// command, or this build's sharing, holds them. The Profiler's staging line counts them from here
// to their release (#423).
[[nodiscard]] std::shared_ptr<const render::Rgba32fImage>
stagingImage(render::Rgba32fImage pixels) {
    const auto bytes = static_cast<std::uint64_t>(pixels.pixelStorageByteCost());
    auto* image = new render::Rgba32fImage(std::move(pixels));
    core::telemetry::stagingAllocated(bytes);
    // A throwing control-block allocation runs the deleter, which balances the count.
    return std::shared_ptr<const render::Rgba32fImage>(
        image, [bytes](const render::Rgba32fImage* released) {
            delete released;
            core::telemetry::stagingReleased(bytes);
        });
}

void addMediaWork(std::uint64_t& total, const std::uint64_t value) noexcept {
    total += std::min(value, std::numeric_limits<std::uint64_t>::max() - total);
}

void recordSourceRevisionWork(const ImageSourceSelection& selection,
                              GpuSceneMediaStatistics& statistics) noexcept {
    if (selection.retainedRevision) {
        addMediaWork(statistics.sourceRevisionCacheHits, 1);
    } else if (selection.sourceRevision) {
        addMediaWork(statistics.sourceRevisionCaptures, 1);
        const auto& work = selection.sourceRevision->captureStatistics();
        addMediaWork(statistics.sourceRevisionBytesHashed, work.bytesHashed);
        addMediaWork(statistics.sourceBytesRead, work.bytesRead);
        addMediaWork(statistics.sourceIoNanoseconds, work.ioNanoseconds);
        addMediaWork(statistics.sourceHashNanoseconds, work.hashNanoseconds);
        addMediaWork(statistics.sourceProbeNanoseconds, work.probeNanoseconds);
    }
}

void recordSourceRevisionWork(const VideoSourceSelection& selection,
                              GpuSceneMediaStatistics& statistics) noexcept {
    if (selection.retainedRevision)
        addMediaWork(statistics.sourceRevisionCacheHits, 1);
    else if (selection.sourceWork.bytesRead != 0)
        addMediaWork(statistics.sourceRevisionCaptures, 1);
    addMediaWork(statistics.sourceRevisionBytesHashed, selection.sourceWork.bytesHashed);
    addMediaWork(statistics.sourceBytesRead, selection.sourceWork.bytesRead);
    addMediaWork(statistics.sourceIoNanoseconds, selection.sourceWork.ioNanoseconds);
    addMediaWork(statistics.sourceHashNanoseconds, selection.sourceWork.hashNanoseconds);
}
void bindVideoSourceOwnership(const GpuSceneMediaContext& context) {
    if (context.operationCache && context.videoContext) {
        context.videoContext->bindRetirement(context.operationCache->sourceRetirementState());
        context.operationCache->bindVideoSources(context.videoContext);
    }
}

void addWindow(OperationKey& key, const render::ImageWindow window) {
    key.add(window.originX());
    key.add(window.originY());
    key.add(window.extent().width());
    key.add(window.extent().height());
}

void addPixelAspect(OperationKey& key, const core::PixelAspectRatio ratio) {
    key.add(ratio.numerator());
    key.add(ratio.denominator());
}

// The source upload identity used by the connected path. It carries only what makes the CONVERTED
// pixels differ: the validated selection identity (asset/sequence/frame digest, interpretation,
// input/working colour space and configuration/processor revision), the proxy scales the conversion
// ran at, and the composition display window and pixel aspect its descriptor was built for. A layer
// transform, node id, plan index or frame time never enters it, so an unchanged source behind a
// changed transform is a hit.
[[nodiscard]] std::string uploadSemanticKey(const std::string_view kind,
                                            const std::string& selectionKey,
                                            const ResolvedEvaluation& resolved) {
    OperationKey key;
    key.add(std::string{"gpu-upload-v1"});
    key.add(std::string{kind});
    key.add(selectionKey);
    key.add(resolved.horizontalScale);
    key.add(resolved.verticalScale);
    addWindow(key, resolved.imageDescriptor.displayWindow());
    addPixelAspect(key, resolved.imageDescriptor.pixelAspect());
    return key.digest();
}

// The RAW backing identity: the kind tag plus the decode-only identity. It deliberately does NOT
// fold proxy scales or display geometry, so those request changes never re-decode. The resident
// command key below does include geometry because a GPU image retains it.
[[nodiscard]] std::string rawDecodeUploadKey(const std::string_view kind,
                                             const std::string& decodeKey) {
    OperationKey key;
    key.add(std::string{"gpu-raw-upload-v1"});
    key.add(std::string{kind});
    key.add(decodeKey);
    return key.digest();
}

// GPU residents also carry display geometry. A raw backing can be shared across requests, but a
// resident made for one display/PAR cannot be returned from the scene cache for another request.
[[nodiscard]] std::string rawUploadCommandKey(const std::string& backingKey,
                                              const render::Rgba32fImageDescriptor& descriptor) {
    OperationKey key;
    key.add(std::string{"gpu-raw-upload-descriptor-v1"});
    key.add(backingKey);
    addWindow(key, descriptor.dataWindow());
    addWindow(key, descriptor.displayWindow());
    addPixelAspect(key, descriptor.pixelAspect());
    return key.digest();
}

[[nodiscard]] std::size_t byteBudget(const std::uint64_t pixels) {
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(pixels, std::numeric_limits<std::size_t>::max()));
}

// A frame's own decode, counted so the sequence read-ahead decodes only on the cores it leaves.
[[nodiscard]] ImageSequenceReadAhead::ForegroundDecode
foregroundDecode(const GpuSceneMediaContext& context) {
    return context.sequenceReadAhead ? context.sequenceReadAhead->foregroundDecode()
                                     : ImageSequenceReadAhead::ForegroundDecode{};
}

// #549: a frame's channel reads (Cryptomatte ranks, Shuffle and depth channels) share one decode of
// each source part. A build registers what its channel reads take from each part before any leaf
// decodes (registerChannelReads); the part's colour decode reads those channels beside its layer
// when it runs first, otherwise the first channel read decodes them all, and every later read of
// the build takes that set. The operation cache keeps it under the source sample and channel set,
// so a later build of the frame (and the sequence read-ahead, which decodes it with the member's
// samples) serves it without a decode.

[[nodiscard]] bool channelSetHolds(const render::ChannelSetImage& set,
                                   const std::vector<std::string>& names) {
    return std::ranges::all_of(
        names, [&set](const std::string& name) { return set.find(name).has_value(); });
}

[[nodiscard]] std::shared_ptr<const render::ChannelSetImage>
cachedChannels(const GpuSceneMediaContext& context, const std::string& part,
               const std::vector<std::string>& channels, const bool explicitBypass) {
    if (explicitBypass || !context.operationCache || channels.empty())
        return nullptr;
    auto cached =
        context.operationCache->find(decodedImageChannelsKey(part, channels), document::Revision{},
                                     OperationCacheEntryKind::DecodedMedia);
    return cached ? std::move(cached->channels) : nullptr;
}

void keepChannels(const GpuSceneMediaContext& context, const EvaluationRequest& request,
                  const std::string& part, const std::vector<std::string>& channels,
                  const std::shared_ptr<const render::ChannelSetImage>& set,
                  const bool explicitBypass, const bool planBypass) {
    if (context.buildUploads)
        context.buildUploads->shareChannels(part, set);
    // An interactive (overridden) plan reads the cache but never fills it, as for decoded images.
    if (explicitBypass || planBypass || !context.operationCache)
        return;
    context.operationCache->store(decodedImageChannelsKey(part, channels), document::Revision{},
                                  {.image = {},
                                   .values = {},
                                   .bounds = {},
                                   .sourceRevision = {},
                                   .samples = {},
                                   .channels = set},
                                  OperationCacheEntryKind::DecodedMedia, {},
                                  request.linearPass.get());
}

// The channels the colour decode of `part` should read beside its layer: the build's registered
// channels of the part, unless this build or the operation cache already holds them.
[[nodiscard]] std::vector<std::string> channelsForColourDecode(const GpuSceneMediaContext& context,
                                                               const std::string& part,
                                                               const bool explicitBypass) {
    if (!context.buildUploads || context.buildUploads->findChannels(part))
        return {};
    auto wanted = context.buildUploads->wantedChannels(part);
    if (wanted.empty())
        return {};
    if (auto cached = cachedChannels(context, part, wanted, explicitBypass)) {
        context.buildUploads->shareChannels(part, std::move(cached));
        return {};
    }
    return wanted;
}

// The channels of `part` decoded with the colour layer its planned Image leaf uploads, the
// decoded colour shared under the keys that leaf looks for; null when no leaf is planned, its
// colour is already shared, or the channels could not be read with it.
[[nodiscard]] std::shared_ptr<const render::ChannelSetImage>
readColourWithChannels(const GpuSceneMediaContext& context, const std::string& part,
                       const std::vector<std::string>& channels, const std::size_t budget,
                       const CancellationToken& cancel, GpuSceneMediaStatistics& statistics) {
    const auto colour = context.buildUploads ? context.buildUploads->findColour(part) : nullptr;
    if (!colour)
        return nullptr;
    const auto samplesKey = rawDecodeUploadKey("image-samples", colour->decodeKey);
    if (findSharedSamples(context, samplesKey) ||
        findSharedUpload(context, rawDecodeUploadKey("image-raw", colour->decodeKey)))
        return nullptr;
    // A sequence member the read-ahead decoded: the leaf uploads those samples.
    if (const auto cached = context.operationCache->find(decodedImageSamplesKey(colour->decodeKey),
                                                         document::Revision{},
                                                         OperationCacheEntryKind::DecodedMedia);
        cached && cached->samples)
        return nullptr;
    media::ImageChannelsAlongside alongside;
    alongside.channels = channels;
    media::ImageDecodeStatistics decodeStatistics;
    const auto foreground = foregroundDecode(context);
    auto samples = decodeRawImageSamples(*colour, budget, cancel, &decodeStatistics, &alongside);
    addMediaWork(statistics.imageDecodeNanoseconds, decodeStatistics.decodeNanoseconds);
    if (samples.value && *samples.value) {
        shareSamples(context, samplesKey, *samples.value);
    } else if (samples.code == media::ImageDiagnosticCode::NativeSamplesUnavailable) {
        auto image = decodeRawImageSource(*colour, budget, cancel, &decodeStatistics, &alongside);
        addMediaWork(statistics.imageDecodeNanoseconds, decodeStatistics.decodeNanoseconds);
        if (image.value && *image.value && (*image.value)->descriptor() != nullptr)
            shareUpload(context, rawDecodeUploadKey("image-raw", colour->decodeKey), *image.value);
    }
    if (alongside.decoded)
        ++statistics.channelSetsReadWithColour;
    return std::move(alongside.decoded);
}

// The reader a leaf's gather reads its sources' channels through.
[[nodiscard]] ChannelSetReader sharedChannelReader(const GpuSceneMediaContext& context,
                                                   const EvaluationRequest& request,
                                                   const bool explicitBypass, const bool planBypass,
                                                   GpuSceneMediaStatistics& statistics) {
    return [&context, &request, explicitBypass, planBypass, &statistics](
               const ImageSourceSelection& source, const std::vector<std::string>& names,
               const std::size_t budget, const CancellationToken& cancel) -> ChannelSetRead {
        if (!source.sourceRevision)
            return readSourceChannels(source, names, budget, cancel);
        const auto part = sourcePartKey(source.digest, source.interpretation.part);
        if (context.buildUploads)
            if (auto shared = context.buildUploads->findChannels(part);
                shared && channelSetHolds(*shared, names)) {
                ++statistics.channelSetsReused;
                return {std::move(shared), {}};
            }
        // The build's whole read of the part (it covers every registered leaf's names); a read
        // the build did not register decodes its own names.
        auto channels = context.buildUploads ? context.buildUploads->wantedChannels(part)
                                             : std::vector<std::string>{};
        if (!std::ranges::all_of(names, [&channels](const std::string& name) {
                return std::ranges::binary_search(channels, name);
            })) {
            channels = names;
            std::ranges::sort(channels);
            channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
        }
        if (auto cached = cachedChannels(context, part, channels, explicitBypass)) {
            ++statistics.channelSetsReused;
            if (context.buildUploads)
                context.buildUploads->shareChannels(part, cached);
            return {std::move(cached), {}};
        }
        // A channel read prepared before the part's Image leaf decodes the leaf's colour layer in
        // the same read and shares it with the leaf.
        if (auto colour =
                readColourWithChannels(context, part, channels, budget, cancel, statistics)) {
            keepChannels(context, request, part, channels, colour, explicitBypass, planBypass);
            return {std::move(colour), {}};
        }
        ChannelSetRead decoded;
        {
            const auto foreground = foregroundDecode(context);
            decoded = readSourceChannels(source, channels, budget, cancel);
        }
        if (decoded.value && *decoded.value) {
            ++statistics.channelSetDecodes;
            keepChannels(context, request, part, channels, *decoded.value, explicitBypass,
                         planBypass);
        }
        return decoded;
    };
}

// The selection the GPU colour split's Image leaf makes, shared with registerColourRead.
[[nodiscard]] ImageSourceSelection
selectImageColorLeaf(const CompiledImageSource& source, const EvaluationRequest& request,
                     const CompiledCompositionPlan& plan, const GpuSceneMediaContext& context,
                     const bool explicitBypass, const CancellationToken& cancellation) {
    return selectImageSource(
        source, request.time, plan.format().frameRate(), context.assetBaseDirectory, cancellation,
        request.colorIntent, explicitBypass ? nullptr : context.operationCache.get(),
        (explicitBypass || request.requireVerifiedMediaSources)
            ? MediaSourceRevisionPolicy::Revalidate
            : request.mediaSourceRevisionPolicy,
        byteBudget(request.pixelStorageByteLimit), /*allowRevisionInsertion=*/true,
        request.linearPass.get(),
        context.buildUploads ? &context.buildUploads->revisions() : nullptr,
        explicitBypass || request.linearPass ? nullptr : context.sequenceReadAhead.get());
}

// The selections a Shuffle and a Cryptomatte leaf make, shared with registerChannelReads.
[[nodiscard]] ChannelShuffleSelection
selectShuffleLeaf(const CompiledChannelShuffle& shuffle, const EvaluationRequest& request,
                  const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                  const GpuSceneMediaContext& context, const bool explicitBypass,
                  const CancellationToken& cancellation) {
    return selectChannelShuffle(shuffle, request.time, plan.format().frameRate(),
                                context.assetBaseDirectory, cancellation, request.colorIntent,
                                explicitBypass ? nullptr : context.operationCache.get(),
                                (explicitBypass || request.requireVerifiedMediaSources)
                                    ? MediaSourceRevisionPolicy::Revalidate
                                    : request.mediaSourceRevisionPolicy,
                                byteBudget(request.pixelStorageByteLimit),
                                /*allowRevisionInsertion=*/true,
                                request.linearPass.get(), ChannelSourceClock{&plan, &resolved});
}

[[nodiscard]] CryptomatteSelection
selectCryptomatteLeaf(const CompiledCryptomatte& cryptomatte, const EvaluationRequest& request,
                      const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                      const GpuSceneMediaContext& context, const bool explicitBypass,
                      const CancellationToken& cancellation) {
    return selectCryptomatte(cryptomatte, request.time, plan.format().frameRate(),
                             context.assetBaseDirectory, cancellation, request.colorIntent,
                             explicitBypass ? nullptr : context.operationCache.get(),
                             (explicitBypass || request.requireVerifiedMediaSources)
                                 ? MediaSourceRevisionPolicy::Revalidate
                                 : request.mediaSourceRevisionPolicy,
                             byteBudget(request.pixelStorageByteLimit),
                                /*allowRevisionInsertion=*/true,
                             request.linearPass.get(), ChannelSourceClock{&plan, &resolved});
}

// Composition frames of image-sequence files captured ahead of the frame being prepared: the
// source working set's read-ahead width (ADR 0037).
constexpr auto kSequenceReadAheadFrames =
    static_cast<std::size_t>(SourceWorkingSet::kReadAheadFrames);

// Queues the next sequence members after a verified selection, so their reads overlap this frame.
void requestSequenceReadAhead(const CompiledImageSource& source, const EvaluationRequest& request,
                              const CompiledCompositionPlan& plan,
                              const GpuSceneMediaContext& context,
                              const ImageSourceSelection& selection, const bool explicitBypass,
                              const bool planBypass, const bool decodeSamples,
                              const std::vector<std::string>& channels) {
    // A linear pass does not read ahead into the cache: its frames are used once, and its own
    // frames in flight already overlap decode with evaluation.
    if (request.linearPass || !context.sequenceReadAhead || !context.operationCache ||
        explicitBypass || planBypass || !selection.available || !source.asset ||
        source.asset->kind != document::AssetKind::Sequence)
        return;
    auto targets =
        imageSequenceReadAheadTargets(source, request.time, plan.format().frameRate(),
                                      context.assetBaseDirectory, kSequenceReadAheadFrames);
    std::vector<ImageSequenceReadAhead::Request> requests;
    requests.reserve(targets.size());
    // #718: this frame's own member was just observed, so its volume id is known and the
    // read-ahead's storage classification asks the volume nothing.
    std::optional<std::uint64_t> volume;
    if (selection.sourceRevision && selection.sourceRevision->sourceIdentity())
        volume = selection.sourceRevision->sourceIdentity()->volume;
    // #386: a GPU colour leaf uploads native samples, so the members are decoded ahead too, with
    // the raw interpretation decodeRawImageSamples() uses (the requested layer is resolved per
    // member).
    std::optional<media::ImageInterpretation> decode;
    if (decodeSamples) {
        decode.emplace();
        decode->colorSpace = media::ImageColorSpace::Raw;
        decode->alphaAssociation = selection.interpretation.alphaAssociation;
        decode->storedSamples = selection.footagePass.has_value();
        decode->part = selection.interpretation.part;
        decode->layer = source.layer;
    }
    // Target i is the composition frame i + 1 after this request's.
    auto use = SourceWorkingSet::current();
    for (auto& target : targets) {
        if (use.tracked)
            ++use.frame;
        requests.push_back({.path = std::move(target.path),
                            .digest = target.digest,
                            .stamp = target.stamp,
                            .recordedDigest = target.recordedDigest,
                            .revisionKey = std::move(target.revisionKey),
                            .byteBudget = byteBudget(request.pixelStorageByteLimit),
                            .localCopies = target.localCopies,
                            .volume = volume,
                            .use = use,
                            .decode = decode,
                            .channels = decode ? channels : std::vector<std::string>{}});
    }
    context.sequenceReadAhead->request(context.operationCache, std::move(requests));
}

[[nodiscard]] bool resolvedProxyIsUnit(const ResolvedEvaluation& resolved) {
    return resolved.horizontalScale == 1.0 && resolved.verticalScale == 1.0;
}

// The descriptor for the raw upload: the source's own data window extent at (0,0) with the
// composition display window and pixel aspect. Both the unit-scale and proxy cases keep this
// full-resolution data window.
[[nodiscard]] std::optional<render::Rgba32fImageDescriptor>
rawUploadDescriptor(const render::Rgba32fImageDescriptor& source,
                    const render::Rgba32fImageDescriptor& composition, const bool framed) {
    // A still image keeps its data window's offset inside the source frame
    // (frameRelativeDataWindow); a video frame is always a (0,0)-origin frame.
    const auto data = source.dataWindow();
    const auto window =
        framed ? frameRelativeDataWindow(source) : [&]() -> std::optional<render::ImageWindow> {
        auto created =
            render::ImageWindow::create(0, 0, data.extent().width(), data.extent().height());
        if (!created)
            return std::nullopt;
        return *created.value();
    }();
    if (!window) {
        return std::nullopt;
    }
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window, composition.displayWindow(), composition.pixelAspect());
    if (!descriptor) {
        return std::nullopt;
    }
    return *descriptor.value();
}

[[nodiscard]] std::optional<render::Rgba32fImageDescriptor>
rawUploadDescriptor(const render::Rgba32fImage& source,
                    const render::Rgba32fImageDescriptor& composition, const bool framed) {
    if (source.descriptor() == nullptr) {
        return std::nullopt;
    }
    return rawUploadDescriptor(*source.descriptor(), composition, framed);
}

// #422: the backing identity of native samples. The decode-only identity plus every
// SampleConvertV1 parameter and the kernel digest, so two interpretations of one decoded frame (or
// a changed kernel) never share a converted resident.
[[nodiscard]] std::string nativeSampleUploadKey(const std::string_view kind,
                                                const std::string& decodeKey,
                                                const render::NativeSampleImage& samples) {
    const auto& conversion = samples.conversion();
    OperationKey key;
    key.add(std::string{"gpu-native-sample-upload-v1"});
    key.add(std::string{kind});
    key.add(decodeKey);
    key.add(std::string{render::sampleConvertKernelDigest()});
    key.add(conversion.layout);
    key.add(conversion.alpha);
    key.add(conversion.storedSamples);
    key.add(conversion.limitedRange);
    key.add(conversion.transfer);
    for (const float coefficient : {conversion.redFromCr, conversion.greenFromCb,
                                    conversion.greenFromCr, conversion.blueFromCb})
        key.add(std::bit_cast<std::uint32_t>(coefficient));
    key.add(conversion.luminance);
    key.add(conversion.hasAlpha);
    return key.digest();
}

// The proxy output descriptor: the shared PointResampleV1 proxy window of the positioned raw
// upload window, with the composition display window and pixel aspect, exactly the CPU
// evaluateImageSource() oracle.
[[nodiscard]] std::optional<render::Rgba32fImageDescriptor>
proxyOutputDescriptor(const render::Rgba32fImageDescriptor& source,
                      const ResolvedEvaluation& resolved) {
    const auto window = render::pointResampleProxyWindow(
        source.dataWindow(), resolved.horizontalScale, resolved.verticalScale);
    if (!window) {
        return std::nullopt;
    }
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window, resolved.imageDescriptor.displayWindow(), resolved.imageDescriptor.pixelAspect());
    if (!descriptor) {
        return std::nullopt;
    }
    return *descriptor.value();
}

// The decoded/converted upload the connected path and every identity colour leaf publish. Exactly
// the evaluator's bypass matrix: only the explicit request bypass disables the still image memory
// cache; an interactive (overridden) plan gets it read-only; the disk cache is never consulted or
// written for either bypass.
[[nodiscard]] MediaUploadOutcome evaluateConvertedImageUpload(
    const ImageSourceSelection& selection, const ResolvedEvaluation& resolved,
    const GpuSceneMediaContext& context, const std::uint64_t pixelBudget, const bool explicitBypass,
    const bool planBypass, const CancellationToken& cancellation,
    GpuSceneMediaStatistics& statistics, LinearPassRetention* const linearPass) {
    MediaUploadOutcome outcome;
    outcome.frameBounds = imageSourceFrameBounds(selection);
    outcome.semanticKey = uploadSemanticKey("image", selection.cacheKey, resolved);
    outcome.uploadSemanticKey = outcome.semanticKey;
    ++statistics.uploadKeyConstructions;
    if (auto shared = findSharedUpload(context, outcome.uploadSemanticKey)) {
        outcome.image = std::move(shared);
        return outcome;
    }
    if (pixelBudget == 0) {
        outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure = "A new image backing exceeds the remaining source allowance";
        return outcome;
    }
    auto* const memoryCache = explicitBypass ? nullptr : context.operationCache.get();
    const auto memoryAccess = explicitBypass
                                  ? ImageSourceMemoryCacheAccess::Disabled
                                  : (planBypass ? ImageSourceMemoryCacheAccess::ReadOnly
                                                : ImageSourceMemoryCacheAccess::ReadWrite);
    media::ImageDecodeStatistics decodeStatistics;
    auto image =
        evaluateImageSource(selection, resolved.imageDescriptor, resolved.horizontalScale,
                            resolved.verticalScale, byteBudget(pixelBudget), memoryCache,
                            memoryAccess, cancellation, &decodeStatistics, linearPass);
    addMediaWork(statistics.imageDecodeNanoseconds, decodeStatistics.decodeNanoseconds);
    addMediaWork(statistics.imageRepairedPixels, image.repairedPixels);
    ++statistics.imageConversions;
    if (image.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!image.value.has_value() && image.code == media::ImageDiagnosticCode::NoColourChannels) {
        outcome.noColourLayer = true;
        ++statistics.imageNoColourLayerSources;
        return outcome;
    }
    if (!image.value.has_value()) {
        if (image.code == media::ImageDiagnosticCode::PixelBudgetExceeded)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure =
            image.diagnostic.empty() ? "Image source could not be evaluated" : image.diagnostic;
        return outcome;
    }
    outcome.image = stagingImage(std::move(*image.value));
    outcome.converted = true;
    shareUpload(context, outcome.uploadSemanticKey, outcome.image);
    return outcome;
}

// The converted frame lives only until its upload completes: the scene's command holds it (and,
// during the build, the build's upload scratch), and nothing keeps a host copy afterwards.
[[nodiscard]] MediaUploadOutcome evaluateConvertedVideoUpload(const VideoSourceSelection& selection,
                                                              const ResolvedEvaluation& resolved,
                                                              const GpuSceneMediaContext& context,
                                                              const std::uint64_t pixelBudget,
                                                              const CancellationToken& cancellation,
                                                              GpuSceneMediaStatistics& statistics) {
    MediaUploadOutcome outcome;
    outcome.semanticKey = uploadSemanticKey("video", selection.cacheKey, resolved);
    outcome.uploadSemanticKey = outcome.semanticKey;
    ++statistics.uploadKeyConstructions;
    if (auto shared = findSharedUpload(context, outcome.uploadSemanticKey)) {
        outcome.image = std::move(shared);
        return outcome;
    }
    core::telemetry::StageTimer convertTime(core::telemetry::Stage::ColourConvert);
    auto converted =
        convertSelectedVideo(selection, resolved.imageDescriptor, resolved.horizontalScale,
                             resolved.verticalScale, byteBudget(pixelBudget),
                             [&cancellation] { return cancellation.isCancellationRequested(); });
    ++statistics.videoConversions;
    if (std::holds_alternative<media::provider::Unavailable>(converted))
        convertTime.cancel();
    else
        convertTime.end();
    if (const auto* error = std::get_if<media::provider::Unavailable>(&converted)) {
        outcome.cancelled = error->reason == media::provider::Error::Cancelled;
        if (!outcome.cancelled) {
            if (error->reason == media::provider::Error::Oversized)
                outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure =
                error->detail.empty() ? "Video frame could not be converted" : error->detail;
        }
        return outcome;
    }
    auto& pixels = std::get<render::Rgba32fImage>(converted);
    outcome.image = stagingImage(std::move(pixels));
    outcome.converted = true;
    shareUpload(context, outcome.uploadSemanticKey, outcome.image);
    return outcome;
}

// Whether the resolved input colour space is (exactly) the working colour space, so the CPU
// processor is identity and there is no colour pass to move to the GPU.
[[nodiscard]] bool colourTransformIsIdentity(const std::string& fromId, const std::string_view toId,
                                             const bool noConversion) {
    return noConversion || fromId.empty() || fromId == toId;
}

// Prepare the real input->working OCIO ProcessEffect for a non-identity media transform. Returns
// nullopt on success; on failure fills `outcome` (cancelled or a fail-closed diagnostic) and
// returns false.
[[nodiscard]] bool prepareMediaOcioProgram(const color::ResolvedBloomNeutralConfig& config,
                                           const std::string& fromId, const std::string_view toId,
                                           const render::ImageWindow outputWindow,
                                           const core::PixelAspectRatio pixelAspect,
                                           const GpuSceneOcioContext& ocioContext,
                                           const CancellationToken& cancellation,
                                           MediaUploadOutcome& outcome,
                                           GpuSceneMediaStatistics& statistics) {
    if (ocioContext.preparer == nullptr) {
        outcome.failure = "No GPU OCIO preparer is configured for media color";
        return false;
    }
    const GpuOcioCommandGeometry geometry{
        .width = static_cast<std::uint32_t>(outputWindow.extent().width()),
        .height = static_cast<std::uint32_t>(outputWindow.extent().height())};
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = fromId;
    spec.toId = std::string{toId};
    auto prepared = ocioContext.preparer->prepare(
        config, spec, geometry, ocioContext.compileOptions,
        [&cancellation] { return cancellation.isCancellationRequested(); });
    if (!prepared) {
        if (prepared.error == GpuOcioPreparationError::CompileCancelled ||
            cancellation.isCancellationRequested()) {
            outcome.cancelled = true;
            return false;
        }
        if (prepared.error == GpuOcioPreparationError::IdentityTransform) {
            // OCIO itself considers the pair equivalent: the raw decode already carries working
            // space, so no effect command is emitted.
            return true;
        }
        // A busy or failed compiler is a wait, not a media failure (#539).
        if (prepared.error == GpuOcioPreparationError::CompileUnavailable)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::AwaitingCapacity;
        outcome.failure = "GPU media color preparation failed: " +
                          std::string{gpuOcioPreparationErrorName(prepared.error)};
        return false;
    }
    ++statistics.ocioCommandPreparations;
    outcome.program = prepared.command;
    outcome.semanticKey = makeGpuSceneOcioEffectSemanticKey(
        outcome.uploadSemanticKey, prepared.command->identity(), outputWindow, pixelAspect);
    return true;
}

// #381: prepare the footage interpretation pass over the full-resolution stored-samples upload.
[[nodiscard]] bool prepareFootagePass(const color::ResolvedBloomNeutralConfig& config,
                                      const FootagePass& pass, const render::ImageWindow window,
                                      const GpuSceneOcioContext& ocioContext,
                                      const CancellationToken& cancellation,
                                      MediaUploadOutcome& outcome,
                                      GpuSceneMediaStatistics& statistics) {
    if (ocioContext.preparer == nullptr) {
        outcome.failure = "No GPU program preparer is configured for footage interpretation";
        return false;
    }
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::FootageInterpretation;
    spec.footagePass = pass.parameters();
    auto prepared = ocioContext.preparer->prepare(
        config, spec,
        GpuOcioCommandGeometry{.width = static_cast<std::uint32_t>(window.extent().width()),
                               .height = static_cast<std::uint32_t>(window.extent().height())},
        ocioContext.compileOptions,
        [&cancellation] { return cancellation.isCancellationRequested(); });
    if (!prepared) {
        if (prepared.error == GpuOcioPreparationError::CompileCancelled ||
            cancellation.isCancellationRequested()) {
            outcome.cancelled = true;
            return false;
        }
        // A busy or failed compiler is a wait, not a media failure (#539).
        if (prepared.error == GpuOcioPreparationError::CompileUnavailable)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::AwaitingCapacity;
        outcome.failure = "GPU footage interpretation preparation failed: " +
                          std::string{gpuOcioPreparationErrorName(prepared.error)};
        return false;
    }
    ++statistics.ocioCommandPreparations;
    outcome.interpretation = prepared.command;
    return true;
}

} // namespace

MediaUploadOutcome
prepareImageUpload(const CompiledImageSource& source, const EvaluationRequest& request,
                   const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                   const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                   const bool explicitBypass, const bool planBypass,
                   const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics) {
    ++statistics.imageSources;
    // The default empty context has no evaluator caches: fail closed instead of decoding with
    // semantics that differ from the evaluator (prepared_gpu_scene.hpp).
    if (context.operationCache == nullptr) {
        MediaUploadOutcome outcome;
        outcome.failure = "Image source context is unavailable";
        return outcome;
    }
    const auto selection = selectImageSource(
        source, request.time, plan.format().frameRate(), context.assetBaseDirectory, cancellation,
        request.colorIntent, explicitBypass ? nullptr : context.operationCache.get(),
        (explicitBypass || request.requireVerifiedMediaSources)
            ? MediaSourceRevisionPolicy::Revalidate
            : request.mediaSourceRevisionPolicy,
        byteBudget(request.pixelStorageByteLimit), /*allowRevisionInsertion=*/true,
        request.linearPass.get(),
        context.buildUploads ? &context.buildUploads->revisions() : nullptr,
        explicitBypass || request.linearPass ? nullptr : context.sequenceReadAhead.get());
    recordSourceRevisionWork(selection, statistics);
    requestSequenceReadAhead(source, request, plan, context, selection, explicitBypass, planBypass,
                             false, {});
    if (selection.cancelled) {
        MediaUploadOutcome outcome;
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.available) {
        MediaUploadOutcome outcome;
        if (pixelBudget == 0 || selection.budgetExceeded)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure =
            selection.warning.empty() ? "Image source is unavailable" : selection.warning;
        return outcome;
    }
    return evaluateConvertedImageUpload(selection, resolved, context, pixelBudget, explicitBypass,
                                        planBypass, cancellation, statistics,
                                        request.linearPass.get());
}

MediaUploadOutcome
prepareVideoUpload(const CompiledVideoSource& source, const EvaluationRequest& request,
                   const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                   const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                   const bool explicitBypass, const bool planBypass,
                   const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics) {
    ++statistics.videoSources;
    MediaUploadOutcome outcome;
    if (context.videoContext == nullptr) {
        outcome.failure = "Video decode context is unavailable";
        return outcome;
    }
    bindVideoSourceOwnership(context);
    const auto selection = selectVideoSource(
        source, request.time, plan.format().frameRate(), context.assetBaseDirectory,
        *context.videoContext, !explicitBypass, cancellation, request.colorIntent,
        (explicitBypass || request.requireVerifiedMediaSources)
            ? MediaSourceRevisionPolicy::Revalidate
            : request.mediaSourceRevisionPolicy,
        !planBypass && !explicitBypass, byteBudget(pixelBudget));
    recordSourceRevisionWork(selection, statistics);
    if (selection.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.frame) {
        outcome.failure =
            selection.warning.empty() ? "Video source is unavailable" : selection.warning;
        return outcome;
    }
    return evaluateConvertedVideoUpload(selection, resolved, context, pixelBudget, cancellation,
                                        statistics);
}

MediaUploadOutcome
prepareImageColorLeaf(const CompiledImageSource& source, const EvaluationRequest& request,
                      const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                      const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                      const std::uint64_t pixelBudget, const bool explicitBypass,
                      const bool planBypass, const CancellationToken& cancellation,
                      GpuSceneMediaStatistics& statistics) {
    if (context.operationCache == nullptr) {
        ++statistics.imageSources;
        MediaUploadOutcome outcome;
        outcome.failure = "Image source context is unavailable";
        return outcome;
    }
    const auto selection = selectImageColorLeaf(source, request, plan, context, explicitBypass, cancellation);
    recordSourceRevisionWork(selection, statistics);
    // #549: the channels this build's channel reads take from the selected part; the read-ahead
    // decodes them with each later member's samples.
    const auto part = sourcePartKey(selection.digest, selection.interpretation.part);
    const auto partChannels = context.buildUploads && selection.available
                                  ? context.buildUploads->wantedChannels(part)
                                  : std::vector<std::string>{};
    requestSequenceReadAhead(source, request, plan, context, selection, explicitBypass, planBypass,
                             ocioContext.preparer != nullptr, partChannels);
    if (selection.cancelled) {
        ++statistics.imageSources;
        MediaUploadOutcome outcome;
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.available) {
        ++statistics.imageSources;
        MediaUploadOutcome outcome;
        if (pixelBudget == 0 || selection.budgetExceeded)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure =
            selection.warning.empty() ? "Image source is unavailable" : selection.warning;
        return outcome;
    }
    const std::string fromId = selection.interpretation.inputColorSpaceId;
    const std::string_view toId = selection.workingColorSpaceId;
    const bool noConversion = selection.inputProcessor == nullptr;
    const bool identityTransform = colourTransformIsIdentity(fromId, toId, noConversion);
    // Without a GPU OCIO preparer there is no way to run a non-identity transform on the device, so
    // this leaf falls back to the connected CPU path (the exact evaluator conversion, including its
    // CPU proxy resample). A GPU colour context is the caller's opt-in to the raw-upload +
    // native-PointResample (+ optional OCIO) split.
    if (ocioContext.preparer == nullptr) {
        ++statistics.imageSources;
        return evaluateConvertedImageUpload(selection, resolved, context, pixelBudget,
                                            explicitBypass, planBypass, cancellation, statistics,
                                            request.linearPass.get());
    }
    std::optional<color::ResolvedBloomNeutralConfig> config;
    if (!identityTransform || selection.footagePass) {
        config = resolveInputColorConfig(request.colorIntent);
        if (!config) {
            MediaUploadOutcome outcome;
            outcome.failure = "The selected OCIO input configuration is unavailable";
            return outcome;
        }
    }

    ++statistics.imageSources;
    // A real OR identity input->working transform. The raw upload always stays at the FULL source
    // dimensions with request geometry on its command. Under a fractional proxy the accepted
    // PointResampleV1 command gathers the full-resolution upload to the proxy output window on the
    // GPU; a non-identity transform then runs the OCIO CST over that proxy geometry. The CPU oracle
    // applies exact nearest resampling before the per-pixel colour transform, and an exact nearest
    // sample commutes with a per-pixel transform, so nearest-then-(optional OCIO) is
    // pixel-identical with no host per-pixel resampling.
    MediaUploadOutcome outcome;
    outcome.frameBounds = imageSourceFrameBounds(selection);
    // #422: a PNG, JPEG, TIFF, DPX or all-half OpenEXR still crosses as its native samples and
    // SampleConvertV1 widens it on the device; any other still (a FLOAT or UINT OpenEXR
    // layer, a missing sequence frame) uploads its raw RGBA32F decode.
    const auto samplesKey = rawDecodeUploadKey("image-samples", selection.decodeKey);
    ++statistics.uploadKeyConstructions;
    outcome.samples = findSharedSamples(context, samplesKey);
    // #386: a sequence member the read-ahead decoded before this frame needed it.
    if (outcome.samples == nullptr && !selection.missingFrame && !explicitBypass && !planBypass &&
        !request.linearPass && context.sequenceReadAhead && selection.sourceRevision != nullptr &&
        source.asset && source.asset->kind == document::AssetKind::Sequence)
        if (auto cached = context.operationCache->find(decodedImageSamplesKey(selection.decodeKey),
                                                       document::Revision{},
                                                       OperationCacheEntryKind::DecodedMedia);
            cached && cached->samples) {
            ++statistics.imageDecodedAhead;
            outcome.samples = std::move(cached->samples);
            shareSamples(context, samplesKey, outcome.samples);
        }
    media::ImageChannelsAlongside alongside;
    const auto keepAlongside = [&] {
        if (!alongside.decoded)
            return;
        ++statistics.channelSetsReadWithColour;
        keepChannels(context, request, part, alongside.channels, alongside.decoded, explicitBypass,
                     planBypass);
        alongside.channels.clear();
        alongside.decoded.reset();
    };
    if (outcome.samples == nullptr && !selection.missingFrame) {
        if (pixelBudget == 0) {
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure = "A new raw image backing exceeds the remaining source allowance";
            return outcome;
        }
        media::ImageDecodeStatistics decodeStatistics;
        // #549: the build's channel reads of this part, read from the same chunks.
        if (selection.sourceRevision != nullptr && !partChannels.empty())
            alongside.channels = channelsForColourDecode(context, part, explicitBypass);
        auto native = [&] {
            // #386: the read-ahead's decodes give this frame's decode the cores.
            const auto foreground = foregroundDecode(context);
            return decodeRawImageSamples(selection, byteBudget(pixelBudget), cancellation,
                                         &decodeStatistics,
                                         alongside.channels.empty() ? nullptr : &alongside);
        }();
        keepAlongside();
        addMediaWork(statistics.imageDecodeNanoseconds, decodeStatistics.decodeNanoseconds);
        addMediaWork(statistics.imageRepairedPixels, decodeStatistics.repairedPixels);
        if (native.cancelled) {
            ++statistics.imageConversions;
            outcome.cancelled = true;
            return outcome;
        }
        if (native.value.has_value()) {
            ++statistics.imageConversions;
            outcome.samples = std::move(*native.value);
            outcome.converted = true;
            shareSamples(context, samplesKey, outcome.samples);
        } else if (native.code == media::ImageDiagnosticCode::NoColourChannels) {
            ++statistics.imageConversions;
            outcome.noColourLayer = true;
            ++statistics.imageNoColourLayerSources;
            return outcome;
        } else if (native.code != media::ImageDiagnosticCode::NativeSamplesUnavailable) {
            ++statistics.imageConversions;
            if (native.code == media::ImageDiagnosticCode::PixelBudgetExceeded)
                outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure =
                native.diagnostic.empty() ? "Image source could not be decoded" : native.diagnostic;
            return outcome;
        }
    }
    std::string backingKey;
    std::optional<render::Rgba32fImageDescriptor> uploadDescriptor;
    if (outcome.samples != nullptr) {
        backingKey = nativeSampleUploadKey("image", selection.decodeKey, *outcome.samples);
        uploadDescriptor =
            rawUploadDescriptor(outcome.samples->descriptor(), resolved.imageDescriptor, true);
    } else {
        backingKey = rawDecodeUploadKey("image-raw", selection.decodeKey);
        ++statistics.uploadKeyConstructions;
        outcome.image = findSharedUpload(context, backingKey);
        if (outcome.image == nullptr) {
            if (pixelBudget == 0) {
                outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
                outcome.failure = "A new raw image backing exceeds the remaining source allowance";
                return outcome;
            }
            // RAW decode: no processor, no implicit conversion, no resample. File decode stays
            // host preparation; the OCIO colour pass is the GPU command prepared below.
            media::ImageDecodeStatistics decodeStatistics;
            // #549: a FLOAT or UINT layer reads the build's channels of the part in its decode.
            if (selection.sourceRevision != nullptr && !partChannels.empty() &&
                alongside.channels.empty())
                alongside.channels = channelsForColourDecode(context, part, explicitBypass);
            auto decoded = [&] {
                const auto foreground = foregroundDecode(context);
                return decodeRawImageSource(selection, byteBudget(pixelBudget), cancellation,
                                            &decodeStatistics,
                                            alongside.channels.empty() ? nullptr : &alongside);
            }();
            keepAlongside();
            addMediaWork(statistics.imageDecodeNanoseconds, decodeStatistics.decodeNanoseconds);
            addMediaWork(statistics.imageRepairedPixels, decodeStatistics.repairedPixels);
            ++statistics.imageConversions;
            if (decoded.cancelled) {
                outcome.cancelled = true;
                return outcome;
            }
            if (!decoded.value.has_value() &&
                decoded.code == media::ImageDiagnosticCode::NoColourChannels) {
                outcome.noColourLayer = true;
                ++statistics.imageNoColourLayerSources;
                return outcome;
            }
            if (!decoded.value.has_value()) {
                if (decoded.code == media::ImageDiagnosticCode::PixelBudgetExceeded)
                    outcome.failureCode =
                        PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
                outcome.failure = decoded.diagnostic.empty() ? "Image source could not be decoded"
                                                             : decoded.diagnostic;
                return outcome;
            }
            // Keep the decoded allocation itself. The request descriptor is attached to the
            // upload command below; rebasing it here would copy every source pixel only to change
            // metadata.
            outcome.image = std::move(*decoded.value);
            outcome.converted = true;
            shareUpload(context, backingKey, outcome.image);
        }
        if (outcome.image->descriptor() == nullptr) {
            outcome.failure = "Raw image upload has no descriptor";
            outcome.image.reset();
            return outcome;
        }
        uploadDescriptor = rawUploadDescriptor(*outcome.image, resolved.imageDescriptor, true);
    }
    if (!uploadDescriptor) {
        outcome.failure = "Raw image upload descriptor is invalid";
        outcome.image.reset();
        outcome.samples.reset();
        return outcome;
    }
    outcome.uploadDescriptor = *uploadDescriptor;
    outcome.uploadSemanticKey = rawUploadCommandKey(backingKey, *uploadDescriptor);
    outcome.semanticKey = outcome.uploadSemanticKey;
    const auto& descriptor = *uploadDescriptor;
    if (selection.footagePass &&
        !prepareFootagePass(*config, *selection.footagePass, descriptor.dataWindow(), ocioContext,
                            cancellation, outcome, statistics)) {
        if (!outcome.cancelled)
            outcome.image.reset();
        return outcome;
    }
    if (!resolvedProxyIsUnit(resolved)) {
        const auto proxyDescriptor = proxyOutputDescriptor(descriptor, resolved);
        if (!proxyDescriptor.has_value()) {
            outcome.failure = "Image proxy output descriptor is invalid";
            outcome.image.reset();
            return outcome;
        }
        outcome.resample =
            MediaResamplePlan{*proxyDescriptor, resolved.horizontalScale, resolved.verticalScale};
    }
    if (identityTransform) {
        // No OCIO pass: the raw (or native-point-resampled) upload is already in working space.
        return outcome;
    }
    const auto ocioWindow = outcome.resample.has_value() ? outcome.resample->output.dataWindow()
                                                         : descriptor.dataWindow();
    const auto ocioAspect = outcome.resample.has_value() ? outcome.resample->output.pixelAspect()
                                                         : descriptor.pixelAspect();
    if (!prepareMediaOcioProgram(*config, fromId, toId, ocioWindow, ocioAspect, ocioContext,
                                 cancellation, outcome, statistics)) {
        if (!outcome.cancelled) {
            outcome.image.reset();
        }
        return outcome;
    }
    return outcome;
}

MediaUploadOutcome prepareChannelShuffleColorLeaf(
    const CompiledChannelShuffle& shuffle, const EvaluationRequest& request,
    const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
    const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
    const std::uint64_t pixelBudget, const bool explicitBypass, const bool planBypass,
    const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics) {
    ++statistics.imageSources;
    MediaUploadOutcome outcome;
    if (context.operationCache == nullptr) {
        outcome.failure = "Channel source context is unavailable";
        return outcome;
    }
    const auto selection = selectShuffleLeaf(shuffle, request, plan, resolved, context,
                                             explicitBypass, cancellation);
    for (const auto& source : selection.sources)
        recordSourceRevisionWork(source, statistics);
    if (selection.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.available) {
        if (pixelBudget == 0 || selection.budgetExceeded)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure =
            selection.warning.empty() ? "Channel source is unavailable" : selection.warning;
        return outcome;
    }
    outcome.frameBounds = channelShuffleFrameBounds(selection);
    const auto* colour =
        selection.colourSource ? &selection.sources[*selection.colourSource] : nullptr;
    const std::string fromId = colour ? colour->interpretation.inputColorSpaceId : std::string{};
    const std::string toId = colour ? colour->workingColorSpaceId : std::string{};
    const bool noConversion = colour == nullptr || colour->inputProcessor == nullptr;
    const bool identityTransform = colourTransformIsIdentity(fromId, toId, noConversion);
    // Without a GPU OCIO preparer a non-identity Colour mapping uses the CPU reference upload.
    if (!identityTransform && ocioContext.preparer == nullptr) {
        outcome.semanticKey = uploadSemanticKey("channel-shuffle", selection.cacheKey, resolved);
        outcome.uploadSemanticKey = outcome.semanticKey;
        ++statistics.uploadKeyConstructions;
        if (pixelBudget == 0) {
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure = "A new shuffle backing exceeds the remaining source allowance";
            return outcome;
        }
        auto image =
            evaluateChannelShuffle(selection, resolved.imageDescriptor, resolved.horizontalScale,
                                   resolved.verticalScale, byteBudget(pixelBudget), cancellation);
        addMediaWork(statistics.imageRepairedPixels, image.repairedPixels);
        ++statistics.imageConversions;
        if (image.cancelled) {
            outcome.cancelled = true;
            return outcome;
        }
        if (!image.value.has_value()) {
            if (image.code == media::ImageDiagnosticCode::PixelBudgetExceeded)
                outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure =
                image.diagnostic.empty() ? "Shuffle could not be evaluated" : image.diagnostic;
            return outcome;
        }
        outcome.image = stagingImage(std::move(*image.value));
        outcome.converted = true;
        return outcome;
    }
    std::optional<color::ResolvedBloomNeutralConfig> config;
    if (!identityTransform) {
        config = resolveInputColorConfig(request.colorIntent);
        if (!config) {
            outcome.failure = "The selected OCIO input configuration is unavailable";
            return outcome;
        }
    }
    const auto backingKey = rawDecodeUploadKey("channel-shuffle-raw", selection.decodeKey);
    ++statistics.uploadKeyConstructions;
    outcome.image = findSharedUpload(context, backingKey);
    if (outcome.image == nullptr) {
        if (pixelBudget == 0) {
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure = "A new raw shuffle backing exceeds the remaining source allowance";
            return outcome;
        }
        auto gathered = gatherChannelShuffle(
            selection, false, byteBudget(pixelBudget), cancellation,
            sharedChannelReader(context, request, explicitBypass, planBypass, statistics));
        addMediaWork(statistics.imageRepairedPixels, gathered.repairedPixels);
        ++statistics.imageConversions;
        if (gathered.cancelled) {
            outcome.cancelled = true;
            return outcome;
        }
        if (!gathered.value.has_value()) {
            if (gathered.code == media::ImageDiagnosticCode::PixelBudgetExceeded)
                outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure = gathered.diagnostic.empty() ? "Shuffle channels could not be decoded"
                                                          : gathered.diagnostic;
            return outcome;
        }
        outcome.image = std::move(*gathered.value);
        outcome.converted = true;
        shareUpload(context, backingKey, outcome.image);
    }
    if (outcome.image->descriptor() == nullptr) {
        outcome.failure = "Raw shuffle upload has no descriptor";
        outcome.image.reset();
        return outcome;
    }
    const auto uploadDescriptor =
        rawUploadDescriptor(*outcome.image, resolved.imageDescriptor, true);
    if (!uploadDescriptor) {
        outcome.failure = "Raw shuffle upload descriptor is invalid";
        outcome.image.reset();
        return outcome;
    }
    outcome.uploadDescriptor = *uploadDescriptor;
    outcome.uploadSemanticKey = rawUploadCommandKey(backingKey, *uploadDescriptor);
    outcome.semanticKey = outcome.uploadSemanticKey;
    const auto& descriptor = *uploadDescriptor;
    if (!resolvedProxyIsUnit(resolved)) {
        const auto proxyDescriptor = proxyOutputDescriptor(descriptor, resolved);
        if (!proxyDescriptor.has_value()) {
            outcome.failure = "Shuffle proxy output descriptor is invalid";
            outcome.image.reset();
            return outcome;
        }
        outcome.resample =
            MediaResamplePlan{*proxyDescriptor, resolved.horizontalScale, resolved.verticalScale};
    }
    if (identityTransform)
        return outcome;
    const auto ocioWindow = outcome.resample.has_value() ? outcome.resample->output.dataWindow()
                                                         : descriptor.dataWindow();
    const auto ocioAspect = outcome.resample.has_value() ? outcome.resample->output.pixelAspect()
                                                         : descriptor.pixelAspect();
    if (!prepareMediaOcioProgram(*config, fromId, toId, ocioWindow, ocioAspect, ocioContext,
                                 cancellation, outcome, statistics)) {
        if (!outcome.cancelled)
            outcome.image.reset();
        return outcome;
    }
    return outcome;
}

MediaUploadOutcome
prepareCryptomatteLeaf(const CompiledCryptomatte& cryptomatte, const EvaluationRequest& request,
                       const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                       const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                       const bool explicitBypass, const bool planBypass,
                       const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics) {
    ++statistics.imageSources;
    MediaUploadOutcome outcome;
    if (context.operationCache == nullptr) {
        outcome.failure = "Channel source context is unavailable";
        return outcome;
    }
    const auto selection = selectCryptomatteLeaf(cryptomatte, request, plan, resolved, context,
                                                 explicitBypass, cancellation);
    for (const auto& source : selection.channels.sources)
        recordSourceRevisionWork(source, statistics);
    if (selection.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.available) {
        if (pixelBudget == 0 || selection.budgetExceeded)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure =
            selection.warning.empty() ? "Cryptomatte source is unavailable" : selection.warning;
        return outcome;
    }
    outcome.frameBounds = cryptomatteFrameBounds(selection);
    // #549: the rank layers depend only on the sources and rank channels, so every matte of the
    // type in this build reads the one gather (each resolves its own IDs through the manifest the
    // decode carries) and the layers' upload identities ignore the selection.
    std::shared_ptr<const CryptomatteGather> gather =
        context.buildUploads ? context.buildUploads->findCryptomatte(selection.gatherKey) : nullptr;
    if (gather == nullptr) {
        if (pixelBudget == 0) {
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure = "A new Cryptomatte backing exceeds the remaining source allowance";
            return outcome;
        }
        auto gathered = gatherCryptomatte(
            selection, byteBudget(pixelBudget), cancellation,
            sharedChannelReader(context, request, explicitBypass, planBypass, statistics));
        addMediaWork(statistics.imageRepairedPixels, gathered.repairedPixels);
        ++statistics.imageConversions;
        if (gathered.cancelled) {
            outcome.cancelled = true;
            return outcome;
        }
        if (gathered.layers.empty()) {
            if (gathered.code == media::ImageDiagnosticCode::PixelBudgetExceeded)
                outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure = gathered.diagnostic.empty() ? "Cryptomatte ranks could not be decoded"
                                                          : gathered.diagnostic;
            return outcome;
        }
        gather = std::make_shared<const CryptomatteGather>(std::move(gathered));
        if (context.buildUploads)
            context.buildUploads->shareCryptomatte(selection.gatherKey, gather);
    }
    const auto& first = *gather->layers.front();
    const auto layerDescriptor = render::Rgba32fImageDescriptor::create(
        first.dataWindow(), first.displayWindow(), first.pixelAspect());
    const auto uploadDescriptor =
        layerDescriptor
            ? rawUploadDescriptor(*layerDescriptor.value(), resolved.imageDescriptor, true)
            : std::nullopt;
    if (!uploadDescriptor || gather->rankCount == 0) {
        outcome.failure = "Cryptomatte rank upload descriptor is invalid";
        return outcome;
    }
    std::vector<MediaCryptomatteLayer> layers;
    for (std::size_t index = 0; index < gather->layers.size(); ++index) {
        OperationKey layerKey;
        layerKey.add(selection.gatherKey);
        layerKey.add(static_cast<std::uint64_t>(index));
        const auto backingKey = rawDecodeUploadKey("cryptomatte-rank-layer", layerKey.digest());
        ++statistics.uploadKeyConstructions;
        layers.push_back(
            {gather->layers[index], rawUploadCommandKey(backingKey, *uploadDescriptor)});
    }
    outcome.uploadDescriptor = *uploadDescriptor;
    outcome.uploadSemanticKey = layers.front().uploadSemanticKey;
    outcome.semanticKey = outcome.uploadSemanticKey;
    outcome.converted = true;
    outcome.cryptomatte =
        MediaCryptomattePlan{*uploadDescriptor, gather->rankCount,
                             resolveCryptomatteIds(selection.mattes, gather->manifest),
                             cryptomatte.preview, std::move(layers)};
    if (!resolvedProxyIsUnit(resolved)) {
        const auto proxyDescriptor = proxyOutputDescriptor(*uploadDescriptor, resolved);
        if (!proxyDescriptor.has_value()) {
            outcome.failure = "Cryptomatte proxy output descriptor is invalid";
            return outcome;
        }
        outcome.resample =
            MediaResamplePlan{*proxyDescriptor, resolved.horizontalScale, resolved.verticalScale};
    }
    return outcome;
}

MediaUploadOutcome
prepareVideoColorLeaf(const CompiledVideoSource& source, const EvaluationRequest& request,
                      const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                      const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                      const std::uint64_t pixelBudget, const bool explicitBypass,
                      const bool planBypass, const CancellationToken& cancellation,
                      GpuSceneMediaStatistics& statistics) {
    ++statistics.videoSources;
    MediaUploadOutcome outcome;
    if (context.videoContext == nullptr) {
        outcome.failure = "Video decode context is unavailable";
        return outcome;
    }
    bindVideoSourceOwnership(context);
    const auto selection = selectVideoSource(
        source, request.time, plan.format().frameRate(), context.assetBaseDirectory,
        *context.videoContext, !explicitBypass, cancellation, request.colorIntent,
        (explicitBypass || request.requireVerifiedMediaSources)
            ? MediaSourceRevisionPolicy::Revalidate
            : request.mediaSourceRevisionPolicy,
        !planBypass && !explicitBypass, byteBudget(pixelBudget));
    recordSourceRevisionWork(selection, statistics);
    if (selection.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.frame) {
        outcome.failure =
            selection.warning.empty() ? "Video source is unavailable" : selection.warning;
        return outcome;
    }
    const std::string& fromId = selection.inputColorSpaceId;
    const std::string_view toId = selection.workingColorSpaceId;
    const bool noConversion = selection.inputProcessor == nullptr;
    const bool identityTransform = colourTransformIsIdentity(fromId, toId, noConversion);
    // Without a GPU OCIO preparer a non-identity transform cannot run on the device, so fall back
    // to the connected CPU path (the exact evaluator conversion). A GPU colour context is the
    // caller's opt-in to the raw-upload + native-PointResample (+ optional OCIO) split.
    if (ocioContext.preparer == nullptr) {
        return evaluateConvertedVideoUpload(selection, resolved, context, pixelBudget, cancellation,
                                            statistics);
    }
    std::optional<color::ResolvedBloomNeutralConfig> config;
    if (!identityTransform || selection.footagePass) {
        config = resolveInputColorConfig(request.colorIntent);
        if (!config) {
            outcome.failure = "The selected OCIO input configuration is unavailable";
            return outcome;
        }
    }
    // #422: the host does no pixel work. The decoded frame's planes cross in their native layout
    // (8-bit 4:2:0, 16-bit 4:4:4:4 YUVA or 16-bit RGBA) and SampleConvertV1 applies the codec-side
    // YUV matrix/range, bit-depth expansion and the config-managed transfer-8 path (no curve) on
    // the device, producing the source's premultiplied pre-OCIO state (or, for a footage pass, its
    // stored samples) exactly as the CPU reference does. A non-identity transform then runs the
    // OCIO CST on the GPU. The frame stays at source resolution; under a fractional proxy the
    // PointResampleV1 command gathers it on the GPU before the (optional) OCIO transform.
    if (selection.frame->planes.empty()) {
        outcome.failure = "Raw video frame has no source plane";
        return outcome;
    }
    const auto sourceWindow = render::ImageWindow::create(0, 0, selection.frame->planes[0].width,
                                                          selection.frame->planes[0].height);
    if (!sourceWindow) {
        outcome.failure = "Raw video source descriptor is invalid";
        return outcome;
    }
    const auto sourceDescriptor = render::Rgba32fImageDescriptor::create(
        *sourceWindow.value(), *sourceWindow.value(), core::PixelAspectRatio::square());
    if (!sourceDescriptor) {
        outcome.failure = "Raw video source descriptor is invalid";
        return outcome;
    }
    // The converted working image is the same full-resolution RGBA32F image the host conversion
    // produced, so it is admitted against the same allowance.
    const auto convertedBytes = static_cast<std::uint64_t>(sourceWindow.value()->extent().width()) *
                                sourceWindow.value()->extent().height() * sizeof(render::Rgba32f);
    if (convertedBytes > byteBudget(pixelBudget)) {
        outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure = "The converted video frame exceeds the remaining source allowance";
        return outcome;
    }
    const auto samplesKey = rawDecodeUploadKey("video-samples", selection.decodeKey);
    ++statistics.uploadKeyConstructions;
    outcome.samples = findSharedSamples(context, samplesKey);
    if (outcome.samples == nullptr) {
        if (cancellation.isCancellationRequested()) {
            outcome.cancelled = true;
            return outcome;
        }
        // #381: a footage pass starts from the stored samples (not premultiplied).
        auto prepared = media::video::videoNativeSamples(
            selection.frame, selection.interpretation,
            !selection.footagePass || selection.inputProcessor
                ? std::string_view(selection.inputColorSpaceId)
                : std::string_view{},
            *sourceDescriptor.value(), selection.footagePass.has_value());
        ++statistics.videoConversions;
        if (const auto* error = std::get_if<media::provider::Unavailable>(&prepared)) {
            outcome.cancelled = error->reason == media::provider::Error::Cancelled;
            if (!outcome.cancelled) {
                if (error->reason == media::provider::Error::Oversized)
                    outcome.failureCode =
                        PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
                outcome.failure =
                    error->detail.empty() ? "Video frame could not be prepared" : error->detail;
            }
            return outcome;
        }
        outcome.samples =
            std::move(std::get<std::shared_ptr<const render::NativeSampleImage>>(prepared));
        outcome.converted = true;
        shareSamples(context, samplesKey, outcome.samples);
    }
    const auto backingKey = nativeSampleUploadKey("video", selection.decodeKey, *outcome.samples);
    const auto uploadDescriptor =
        rawUploadDescriptor(outcome.samples->descriptor(), resolved.imageDescriptor, false);
    if (!uploadDescriptor) {
        outcome.failure = "Raw video upload descriptor is invalid";
        outcome.samples.reset();
        return outcome;
    }
    outcome.uploadDescriptor = *uploadDescriptor;
    outcome.uploadSemanticKey = rawUploadCommandKey(backingKey, *uploadDescriptor);
    outcome.semanticKey = outcome.uploadSemanticKey;
    const auto& descriptor = *uploadDescriptor;
    if (selection.footagePass &&
        !prepareFootagePass(*config, *selection.footagePass, descriptor.dataWindow(), ocioContext,
                            cancellation, outcome, statistics)) {
        if (!outcome.cancelled)
            outcome.image.reset();
        return outcome;
    }
    if (!resolvedProxyIsUnit(resolved)) {
        const auto proxyDescriptor = proxyOutputDescriptor(descriptor, resolved);
        if (!proxyDescriptor.has_value()) {
            outcome.failure = "Video proxy output descriptor is invalid";
            outcome.image.reset();
            return outcome;
        }
        outcome.resample =
            MediaResamplePlan{*proxyDescriptor, resolved.horizontalScale, resolved.verticalScale};
    }
    if (identityTransform) {
        // No OCIO pass: the codec-prepared (or native-point-resampled) frame is already in working
        // space.
        return outcome;
    }
    const auto ocioWindow = outcome.resample.has_value() ? outcome.resample->output.dataWindow()
                                                         : descriptor.dataWindow();
    const auto ocioAspect = outcome.resample.has_value() ? outcome.resample->output.pixelAspect()
                                                         : descriptor.pixelAspect();
    if (!prepareMediaOcioProgram(*config, fromId, toId, ocioWindow, ocioAspect, ocioContext,
                                 cancellation, outcome, statistics)) {
        if (!outcome.cancelled) {
            outcome.image.reset();
        }
        return outcome;
    }
    return outcome;
}

void registerChannelReads(const CompiledOperation& operation, const EvaluationRequest& request,
                          const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                          const GpuSceneMediaContext& context, const bool explicitBypass,
                          const bool /*planBypass*/, const CancellationToken& cancellation) {
    if (!context.buildUploads || !context.operationCache)
        return;
    const auto want = [&context](const std::vector<ImageSourceSelection>& sources,
                                 const auto& channels) {
        std::vector<std::vector<std::string>> names(sources.size());
        for (const auto& channel : channels)
            if (channel.source && *channel.source < sources.size())
                names[*channel.source].push_back(channel.sourceChannel);
        for (std::size_t index = 0; index < sources.size(); ++index)
            if (!names[index].empty() && sources[index].sourceRevision)
                context.buildUploads->wantChannels(
                    sourcePartKey(sources[index].digest, sources[index].interpretation.part),
                    names[index]);
    };
    if (const auto* cryptomatte = std::get_if<CompiledCryptomatte>(&operation)) {
        const auto selection = selectCryptomatteLeaf(*cryptomatte, request, plan, resolved, context,
                                                     explicitBypass, cancellation);
        if (selection.available)
            want(selection.channels.sources, selection.rankChannels);
    } else if (const auto* shuffle = std::get_if<CompiledChannelShuffle>(&operation)) {
        const auto selection = selectShuffleLeaf(*shuffle, request, plan, resolved, context,
                                                 explicitBypass, cancellation);
        if (selection.available)
            want(selection.sources, selection.mapping);
    }
}

void registerColourRead(const CompiledImageSource& source, const EvaluationRequest& request,
                        const CompiledCompositionPlan& plan, const GpuSceneMediaContext& context,
                        const bool explicitBypass, const bool /*planBypass*/,
                        const CancellationToken& cancellation) {
    if (!context.buildUploads || !context.operationCache)
        return;
    auto selection = selectImageColorLeaf(source, request, plan, context, explicitBypass, cancellation);
    if (!selection.available || selection.missingFrame || !selection.sourceRevision)
        return;
    const auto part = sourcePartKey(selection.digest, selection.interpretation.part);
    if (context.buildUploads->wantedChannels(part).empty())
        return;
    try {
        context.buildUploads->planColour(
            part, std::make_shared<const ImageSourceSelection>(std::move(selection)));
    } catch (const std::bad_alloc&) {
        // Planning is an optimization; the Image leaf then decodes its own colour.
    }
}

} // namespace bloom::runtime::detail
