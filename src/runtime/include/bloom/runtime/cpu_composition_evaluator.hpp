#pragma once

#include <bloom/runtime/audio_stream_key.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/image_sequence_read_ahead.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

namespace bloom::runtime {
// Pixel-free preflight result. This is the live-image minimum checked before evaluation, including
// nested plans. Transforms and media may need more storage when their actual bounds are resolved
// during execution; the caller's original limit remains the upper bound for those allocations.
struct EvaluationPixelStoragePlan final {
    std::optional<std::size_t> requiredPreflightBytes = {};
    std::optional<std::size_t> outputImageBytes = {};
    std::optional<EvaluationDiagnostic> diagnostic = {};
    bool cancelled = false;
};

namespace detail {
class VideoSourceContext;
class ImageEffectContext;
} // namespace detail

// A device-free audio projection of one compiled composition. It carries only the source identity
// and time-varying controls; decoding and device ownership stay at the media/audio boundary.
struct AudioCompositionTimeMapping final {
    double offset = 0.0;
    double scale = 1.0;
    std::int64_t loopMode = 0;
    core::RationalTime duration{};
    document::FrameRate frameRate = document::FrameRate::framesPerSecond24();
    core::RationalTime inPoint{}, outPoint{};
    // ADR 0024: a layer-time entry instead of a composition-source one. `plan` keeps the compiled
    // Time Remap curve alive so each output sample samples it at its own layer time.
    std::optional<CompiledLayerTime> layerTime{};
    std::shared_ptr<const CompiledCompositionPlan> plan{};
    friend bool operator==(const AudioCompositionTimeMapping&,
                           const AudioCompositionTimeMapping&) = default;
};
// #471: where an automated level is heading. The clip's `level` is the level at `start`; the level
// at `end` is `endLevel`, and playback ramps linearly between them (in the playing transport's
// time) so a keyed fade is continuous, as in export, instead of stepping once per mix.
struct AudioClipLevelRamp final {
    core::RationalTime start{};
    core::RationalTime end{};
    double endLevel = 1.0;

    friend bool operator==(const AudioClipLevelRamp&, const AudioClipLevelRamp&) = default;
};
// #569: one audio effect of a clip at a time: its kind and its parameter values in the node's
// definition order.
struct AudioEffectDescription final {
    AudioEffectKind kind = AudioEffectKind::Mono;
    std::vector<double> values;
    // The values at the end of the clip's effect ramp (empty: they hold `values`).
    std::vector<double> endValues{};
    friend bool operator==(const AudioEffectDescription&, const AudioEffectDescription&) = default;
};

struct AudioClipDescription final {
    document::NodeId sourceNodeId;
    document::AssetId assetId;
    core::RationalTime startTime{};
    core::RationalTime endTime{};
    double level = 1.0;
    bool muted = false;
    bool solo = false;
    std::vector<AudioCompositionTimeMapping> timeMappings{};
    // #354: the source's own loop (a video's Loop/PingPong over its picture extent), applied
    // after every mapping above to the clip's content time; see wrapSourceAudioTime.
    std::int64_t sourceLoopMode = 0;
    std::optional<TimeExtent> sourceLoopExtent{};
    // The asset audio stream the clip plays (0-based among the asset's audio streams).
    std::uint32_t audioStream = 0;
    // #381: a conformed or looped video's audio timing, applied last (see FootageAudioTiming).
    std::optional<FootageAudioTiming> footage{};
    // #471: set by the interactive playback mix on a clip whose level is automated.
    std::optional<AudioClipLevelRamp> levelRamp{};
    // #569: the effect chain the clip's sound runs through after its level, nearest the source
    // first.
    std::vector<AudioEffectDescription> effects{};
    // #569: set by the interactive playback mix on a clip whose effect parameters move: the effects'
    // `endValues` are those at `end` (the ramp's endLevel is not used).
    std::optional<AudioClipLevelRamp> effectRamp{};

    friend bool operator==(const AudioClipDescription&, const AudioClipDescription&) = default;
};

struct AudioMixDescription final {
    document::NodeId outputNodeId;
    std::vector<AudioClipDescription> clips;

    friend bool operator==(const AudioMixDescription&, const AudioMixDescription&) = default;
};

// Resolve an enclosing composition's transport time into this clip's leaf composition.
// A time outside any enclosing Layer range contributes silence. Every layer clip carries at least
// its own layer's range entry, so a trimmed head is silent before the in point. Allocation-free:
// it runs per output sample on the mixer worker.
[[nodiscard]] std::optional<core::RationalTime> mapAudioClipTime(const AudioClipDescription& clip,
                                                                 core::RationalTime time);

class CpuCompositionEvaluator final {
  public:
    [[nodiscard]] EvaluationPixelStoragePlan
    planPreflightPixelStorage(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                              const EvaluationRequest& request,
                              const CancellationToken& cancellation = {}) const;
    [[nodiscard]] std::shared_ptr<detail::ImageEffectContext> imageEffectContext() const;
    void setVideoCacheByteBudget(std::size_t budget) const;
    // "Purge media cache": drops the evaluator's decoded-video memory cache. Only the cache's own
    // references are released, so a frame already handed to a task stays valid. Safe to call from
    // any thread.
    void clearDecodedVideoCache() const;
    [[nodiscard]] std::shared_ptr<detail::VideoSourceContext> videoContext() const;
    void setAssetBaseDirectory(std::filesystem::path directory) const {
        std::lock_guard lock(assetContext_->mutex);
        assetContext_->directory = std::move(directory);
    }
    [[nodiscard]] std::filesystem::path assetBaseDirectory() const {
        std::lock_guard lock(assetContext_->mutex);
        return assetContext_->directory;
    }
    // #718: the host's local mirror of source media on slow storage (media-io.md "Disk cache").
    // It sits behind the operation cache's source revisions: memory, then the mirror, then the
    // source. Null (the default) reads sources directly.
    void setSourceStore(std::shared_ptr<platform::SourceStore> store) const {
        cache_->setSourceStore(std::move(store));
    }
    [[nodiscard]] const std::shared_ptr<OperationCache>& operationCache() const { return cache_; }
    // The image-sequence read-ahead feeding this evaluator's operation cache; preview, playback and
    // export requests sharing the evaluator share it.
    [[nodiscard]] const std::shared_ptr<ImageSequenceReadAhead>& sequenceReadAhead() const {
        return readAhead_;
    }
    // `rowBands` is the bounded pool the per-row kernels are spread across (the scheduler owns one;
    // TaskContext::rowBandExecutor() is where a task body gets it). Null evaluates every row band
    // on the calling thread, in band order.
    //
    // The pixels are the same either way, to the bit: banding partitions rows, and every row kernel
    // writes only its own row. The evaluator therefore does NOT enter the pool into
    // ProcessFrameIdentity -- a frame evaluated in parallel and the same frame evaluated serially
    // are the same frame, and a cache that holds one may serve the other.
    [[nodiscard]] EvaluationResult evaluate(std::shared_ptr<const CompiledCompositionPlan> plan,
                                            const EvaluationRequest& request,
                                            const CancellationToken& cancellation,
                                            EvaluationProgressCallback progress = {},
                                            CpuRowBandExecutor* rowBands = nullptr,
                                            OperationCacheStatistics* statistics = nullptr) const;

    // Resolves the compiled audio graph at `time` without touching pixels, a device, or the
    // filesystem. Curve-backed and value-graph-backed levels use the same exact evaluators as the
    // image path. A malformed audio operand returns nullopt instead of silently changing the mix.
    // `mix` is the plan's own mix when null, or one of the plan's analysis sounds (#630), which
    // read the same curves and value graph.
    [[nodiscard]] std::optional<AudioMixDescription>
    evaluateAudioMix(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                     core::RationalTime time, const CancellationToken& cancellation = {},
                     const CompositionAudioMix* mix = nullptr) const;

  private:
    struct AssetContext {
        std::mutex mutex;
        std::filesystem::path directory;
        std::shared_ptr<detail::VideoSourceContext> video;
        std::shared_ptr<detail::ImageEffectContext> effects;
    };
    std::shared_ptr<AssetContext> assetContext_ = std::make_shared<AssetContext>();
    std::shared_ptr<OperationCache> cache_ = std::make_shared<OperationCache>();
    std::shared_ptr<ImageSequenceReadAhead> readAhead_ = std::make_shared<ImageSequenceReadAhead>();
};

} // namespace bloom::runtime
