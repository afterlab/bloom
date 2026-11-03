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
    context.mediaDiskCache = evaluator.mediaDiskCache();
    context.sequenceReadAhead = evaluator.sequenceReadAhead();
    return context;
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

// Composition frames of image-sequence files captured ahead of the frame being prepared: the
// source working set's read-ahead width (ADR 0037).
constexpr auto kSequenceReadAheadFrames =
    static_cast<std::size_t>(SourceWorkingSet::kReadAheadFrames);

// Queues the next sequence members after a verified selection, so their reads overlap this frame.
void requestSequenceReadAhead(const CompiledImageSource& source, const EvaluationRequest& request,
                              const CompiledCompositionPlan& plan,
                              const GpuSceneMediaContext& context,
                              const ImageSourceSelection& selection, const bool explicitBypass,
                              const bool planBypass) {
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
    // Target i is the composition frame i + 1 after this request's.
    auto use = SourceWorkingSet::current();
    for (auto& target : targets) {
        if (use.tracked)
            ++use.frame;
        requests.push_back({.path = std::move(target.path),
                            .digest = target.digest,
                            .stamp = target.stamp,
                            .revisionKey = std::move(target.revisionKey),
                            .byteBudget = byteBudget(request.pixelStorageByteLimit),
                            .use = use});
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
    const auto window = framed ? frameRelativeDataWindow(source)
                               : [&]() -> std::optional<render::ImageWindow> {
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
    auto* const diskCache = (explicitBypass || planBypass) ? nullptr : context.mediaDiskCache.get();
    media::ImageDecodeStatistics decodeStatistics;
    auto image =
        evaluateImageSource(selection, resolved.imageDescriptor, resolved.horizontalScale,
                            resolved.verticalScale, byteBudget(pixelBudget), memoryCache,
                            memoryAccess, cancellation, diskCache, &decodeStatistics, linearPass);
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
        byteBudget(request.pixelStorageByteLimit), !planBypass, request.linearPass.get());
    recordSourceRevisionWork(selection, statistics);
    requestSequenceReadAhead(source, request, plan, context, selection, explicitBypass, planBypass);
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
    const auto selection = selectImageSource(
        source, request.time, plan.format().frameRate(), context.assetBaseDirectory, cancellation,
        request.colorIntent, explicitBypass ? nullptr : context.operationCache.get(),
        (explicitBypass || request.requireVerifiedMediaSources)
            ? MediaSourceRevisionPolicy::Revalidate
            : request.mediaSourceRevisionPolicy,
        byteBudget(request.pixelStorageByteLimit), !planBypass, request.linearPass.get());
    recordSourceRevisionWork(selection, statistics);
    requestSequenceReadAhead(source, request, plan, context, selection, explicitBypass, planBypass);
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
    // #422: a PNG, JPEG or all-half OpenEXR still crosses as its native samples and
    // SampleConvertV1 widens it on the device; any other still (TIFF, a FLOAT or UINT OpenEXR
    // layer, a missing sequence frame) uploads its raw RGBA32F decode.
    const auto samplesKey = rawDecodeUploadKey("image-samples", selection.decodeKey);
    ++statistics.uploadKeyConstructions;
    outcome.samples = findSharedSamples(context, samplesKey);
    if (outcome.samples == nullptr && !selection.missingFrame) {
        if (pixelBudget == 0) {
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
            outcome.failure = "A new raw image backing exceeds the remaining source allowance";
            return outcome;
        }
        media::ImageDecodeStatistics decodeStatistics;
        auto native = decodeRawImageSamples(selection, byteBudget(pixelBudget), cancellation,
                                            &decodeStatistics);
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
            outcome.failure = native.diagnostic.empty() ? "Image source could not be decoded"
                                                        : native.diagnostic;
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
            auto decoded = decodeRawImageSource(selection, byteBudget(pixelBudget), cancellation,
                                                &decodeStatistics);
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
    const auto selection = selectChannelShuffle(
        shuffle, request.time, plan.format().frameRate(), context.assetBaseDirectory, cancellation,
        request.colorIntent, explicitBypass ? nullptr : context.operationCache.get(),
        (explicitBypass || request.requireVerifiedMediaSources)
            ? MediaSourceRevisionPolicy::Revalidate
            : request.mediaSourceRevisionPolicy,
        byteBudget(request.pixelStorageByteLimit), !planBypass, request.linearPass.get());
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
        auto gathered =
            gatherChannelShuffle(selection, false, byteBudget(pixelBudget), cancellation);
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
    const auto selection = selectCryptomatte(
        cryptomatte, request.time, plan.format().frameRate(), context.assetBaseDirectory,
        cancellation, request.colorIntent, explicitBypass ? nullptr : context.operationCache.get(),
        (explicitBypass || request.requireVerifiedMediaSources)
            ? MediaSourceRevisionPolicy::Revalidate
            : request.mediaSourceRevisionPolicy,
        byteBudget(request.pixelStorageByteLimit), !planBypass, request.linearPass.get());
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
    // The stacked ranks depend only on the sources and rank channels; the selected IDs (resolved
    // through the manifest the decode carries) are kept beside them under the selection identity,
    // which covers the sources' content, the layer and the matte list.
    const auto backingKey = rawDecodeUploadKey("cryptomatte-raw", selection.gatherKey);
    ++statistics.uploadKeyConstructions;
    std::optional<std::vector<std::uint32_t>> ids;
    if (pixelBudget == 0) {
        outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure = "A new Cryptomatte backing exceeds the remaining source allowance";
        return outcome;
    }
    const auto gather = gatherCryptomatte(selection, byteBudget(pixelBudget), cancellation);
    addMediaWork(statistics.imageRepairedPixels, gather.repairedPixels);
    ++statistics.imageConversions;
    if (gather.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (gather.ranks.empty()) {
        if (gather.code == media::ImageDiagnosticCode::PixelBudgetExceeded)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure = gather.diagnostic.empty() ? "Cryptomatte ranks could not be decoded"
                                                    : gather.diagnostic;
        return outcome;
    }
    auto stacked = stackCryptomatteRanks(gather, byteBudget(pixelBudget), cancellation);
    if (stacked.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!stacked.value.has_value()) {
        if (stacked.code == media::ImageDiagnosticCode::PixelBudgetExceeded)
            outcome.failureCode = PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded;
        outcome.failure = stacked.diagnostic.empty() ? "Cryptomatte ranks could not be stacked"
                                                     : stacked.diagnostic;
        return outcome;
    }
    outcome.image = stagingImage(std::move(*stacked.value));
    outcome.converted = true;
    ids = gather.ids;
    const auto rankCount = static_cast<std::uint32_t>(selection.ranks.size());
    const auto* stackedDescriptor = outcome.image->descriptor();
    if (stackedDescriptor == nullptr || rankCount == 0 ||
        stackedDescriptor->dataWindow().extent().height() % rankCount != 0) {
        outcome.failure = "Cryptomatte rank upload has an invalid descriptor";
        outcome.image.reset();
        return outcome;
    }
    const auto uploadDescriptor =
        rawUploadDescriptor(*outcome.image, resolved.imageDescriptor, true);
    if (!uploadDescriptor) {
        outcome.failure = "Cryptomatte rank upload descriptor is invalid";
        outcome.image.reset();
        return outcome;
    }
    const auto uploadWindow = uploadDescriptor->dataWindow();
    const auto matteWindow = render::ImageWindow::create(
        uploadWindow.originX(), uploadWindow.originY(), uploadWindow.extent().width(),
        uploadWindow.extent().height() / rankCount);
    if (!matteWindow) {
        outcome.failure = "Cryptomatte matte window is invalid";
        outcome.image.reset();
        return outcome;
    }
    const auto matteDescriptor = render::Rgba32fImageDescriptor::create(
        *matteWindow.value(), uploadDescriptor->displayWindow(), uploadDescriptor->pixelAspect());
    if (!matteDescriptor) {
        outcome.failure = "Cryptomatte matte descriptor is invalid";
        outcome.image.reset();
        return outcome;
    }
    outcome.uploadDescriptor = *uploadDescriptor;
    outcome.uploadSemanticKey = rawUploadCommandKey(backingKey, *uploadDescriptor);
    outcome.semanticKey = outcome.uploadSemanticKey;
    outcome.cryptomatte = MediaCryptomattePlan{*matteDescriptor.value(), rankCount, std::move(*ids),
                                               cryptomatte.preview};
    if (!resolvedProxyIsUnit(resolved)) {
        const auto proxyDescriptor = proxyOutputDescriptor(*matteDescriptor.value(), resolved);
        if (!proxyDescriptor.has_value()) {
            outcome.failure = "Cryptomatte proxy output descriptor is invalid";
            outcome.image.reset();
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

} // namespace bloom::runtime::detail
