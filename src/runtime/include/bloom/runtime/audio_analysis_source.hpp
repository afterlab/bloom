#pragma once

// #347: the audio an analysis node reads, as the compiled plan carries it. It names the asset (its
// content digest is the source revision), the stream, and the timing the audio mixer would give the
// same source: the node's start frame, a video's loop and its conformed rate. It carries no samples
// and no file state; docs/architecture/media-io.md, "Audio analysis", is the specification.

#include <bloom/document/asset.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/composition_time.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

// #630: where the digest of a sound is kept once a worker has computed it (audio_sound.hpp). It
// belongs to one compiled sound, so an evaluation that only reads (the interface thread) finds the
// prepared sound of its own plan without evaluating the mix.
class AudioSoundIdentity final {
  public:
    [[nodiscard]] std::string digest() const {
        const std::scoped_lock lock(mutex_);
        return digest_;
    }
    void setDigest(std::string digest) {
        const std::scoped_lock lock(mutex_);
        digest_ = std::move(digest);
    }

  private:
    mutable std::mutex mutex_;
    std::string digest_;
};

// #630: the sound an analysis node reads when its `audio` input carries more than a bare source:
// a source through its effects, a layer, a Merge's mix or a nested composition. `mix` is lowered
// exactly as the composition's own mix is and is evaluated against the plan that holds it;
// `assets` are the project's audio and video assets, so a clip's asset id resolves to a file.
struct AudioAnalysisSound final {
    CompositionAudioMix mix;
    core::RationalTime duration{};
    document::FrameRate rate = document::FrameRate::framesPerSecond24();
    std::shared_ptr<const std::vector<document::AssetRecord>> assets;
    std::shared_ptr<AudioSoundIdentity> identity = std::make_shared<AudioSoundIdentity>();

    friend bool operator==(const AudioAnalysisSound& left, const AudioAnalysisSound& right);
};

struct AudioAnalysisSource final {
    document::AssetRecord asset;
    // 0-based among the asset's audio streams.
    std::uint32_t audioStream = 0;
    std::int64_t startFrame = 0;
    document::FrameRate compositionRate = document::FrameRate::framesPerSecond24();
    // A video source's loop (0 hold, 1 loop, 2 ping-pong) over its picture extent.
    std::int64_t loopMode = 0;
    std::optional<TimeExtent> loopExtent{};
    std::optional<FootageAudioTiming> footage{};
    // #630: true when the input is a source wired straight in at a constant level of 1: the
    // source's own samples are read from its file (the fields above). Otherwise they are unused
    // and the window analyses read `sound` as it is rendered. `sound` is always set.
    bool bare = true;
    std::shared_ptr<const AudioAnalysisSound> sound{};

    friend bool operator==(const AudioAnalysisSource& left, const AudioAnalysisSource& right);
};

[[nodiscard]] bool sameAudioAnalysisSource(const std::shared_ptr<const AudioAnalysisSource>& left,
                                           const std::shared_ptr<const AudioAnalysisSource>& right) noexcept;

} // namespace bloom::runtime
