#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::media::audio {

// The speaker a decoded channel feeds. The set and the spellings accepted by
// audioChannelFromName follow FFmpeg's channel names (FL, FR, FC, LFE, BL, ...).
enum class AudioChannel : std::uint8_t {
    Unknown,
    FrontLeft,
    FrontRight,
    FrontCentre,
    LowFrequency,
    BackLeft,
    BackRight,
    FrontLeftOfCentre,
    FrontRightOfCentre,
    BackCentre,
    SideLeft,
    SideRight,
    TopCentre,
    TopFrontLeft,
    TopFrontCentre,
    TopFrontRight,
    TopBackLeft,
    TopBackCentre,
    TopBackRight,
    DownmixLeft,
    DownmixRight,
    WideLeft,
    WideRight,
    SurroundDirectLeft,
    SurroundDirectRight,
    LowFrequency2,
    TopSideLeft,
    TopSideRight,
    BottomFrontCentre,
    BottomFrontLeft,
    BottomFrontRight,
    SideSurroundLeft,
    SideSurroundRight,
    TopSurroundLeft,
    TopSurroundRight,
    BinauralLeft,
    BinauralRight,
};

// One role per decoded channel, in plane order. Inline storage keeps the layout inside the owning
// buffer object, so buffer accounting needs no extra heap term. A buffer with more channels than
// this, or with any Unknown role, is treated as undeclared and gets the default layout.
inline constexpr std::size_t kMaxDeclaredAudioChannels = 64;
using AudioChannelLayout = std::array<AudioChannel, kMaxDeclaredAudioChannels>;

[[nodiscard]] AudioChannel audioChannelFromName(std::string_view name) noexcept;
[[nodiscard]] AudioChannelLayout audioChannelLayout(std::span<const std::string> names) noexcept;
// WAVE_FORMAT_EXTENSIBLE dwChannelMask. A mask whose speaker count differs from `channels`
// declares nothing.
[[nodiscard]] AudioChannelLayout waveChannelLayout(std::uint32_t mask,
                                                   std::uint32_t channels) noexcept;

// The layout a channel count implies when the source declares none: the FLAC / WAVE default
// assignment for 1-8 channels (mono, stereo, 3.0, quad, 5.0, 5.1, 6.1, 7.1). Past eight channels
// only the first two are placed (front left, front right); the rest stay Unknown.
[[nodiscard]] AudioChannelLayout defaultAudioChannelLayout(std::uint32_t channels) noexcept;

// The declared layout when every channel has a known role, otherwise the default layout.
[[nodiscard]] AudioChannelLayout resolvedAudioChannelLayout(const AudioChannelLayout& declared,
                                                            std::uint32_t channels) noexcept;

// Row-major gains, outputChannels rows by sourceChannels columns: output[o] += source[s] *
// gains[o * sourceChannels + s]. Stereo output uses the ITU-R BS.775 downmix with FFmpeg's default
// levels: centre and surrounds at -3 dB, LFE dropped, no normalization. Mono output is the
// stereo downmix summed at -3 dB. A mono source feeds every output channel at unity. Other
// output channel counts map source channel i to output channel i.
[[nodiscard]] std::vector<float> channelMixMatrix(const AudioChannelLayout& declared,
                                                  std::uint32_t sourceChannels,
                                                  std::uint32_t outputChannels);

} // namespace bloom::media::audio
