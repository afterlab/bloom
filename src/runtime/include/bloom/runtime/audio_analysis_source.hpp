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
#include <optional>

namespace bloom::runtime {

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

    friend bool operator==(const AudioAnalysisSource&, const AudioAnalysisSource&) = default;
};

[[nodiscard]] bool sameAudioAnalysisSource(const std::shared_ptr<const AudioAnalysisSource>& left,
                                           const std::shared_ptr<const AudioAnalysisSource>& right) noexcept;

} // namespace bloom::runtime
