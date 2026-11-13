#pragma once

// Opens one audio stream of an asset as a windowed buffer (only a small window of samples is
// resident, however long the audio is), after checking that the file is the one the asset
// records. Shared by the window analyses (audio_analysis.cpp) and the rendered sound
// (audio_sound.cpp). Blocking file work: call it on a worker.

#include <bloom/document/asset.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/video/session.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace bloom::runtime::detail {

struct OpenedAssetAudio final {
    std::shared_ptr<const media::audio::AudioBuffer> buffer;
    // A video container's audio decodes through its session's retained worker.
    std::shared_ptr<media::video::VideoDecodeSession> session;
    // Why there is no buffer, in words the artist can read; empty when cancelled.
    std::string error;
    bool cancelled = false;
};

[[nodiscard]] OpenedAssetAudio openAssetAudio(const document::AssetRecord& asset,
                                              std::uint32_t audioStream,
                                              const std::filesystem::path& base,
                                              const std::function<bool()>& cancelled);

} // namespace bloom::runtime::detail
