#include <bloom/media/audio/channel_layout.hpp>

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace bloom::media::audio {
namespace {

using enum AudioChannel;

struct NamedChannel final {
    std::string_view name;
    AudioChannel channel;
};

// FFmpeg's av_channel_name spellings, plus the bare L/R/C some providers emit.
constexpr std::array kNames{
    NamedChannel{"FL", FrontLeft},
    NamedChannel{"FR", FrontRight},
    NamedChannel{"FC", FrontCentre},
    NamedChannel{"LFE", LowFrequency},
    NamedChannel{"BL", BackLeft},
    NamedChannel{"BR", BackRight},
    NamedChannel{"FLC", FrontLeftOfCentre},
    NamedChannel{"FRC", FrontRightOfCentre},
    NamedChannel{"BC", BackCentre},
    NamedChannel{"SL", SideLeft},
    NamedChannel{"SR", SideRight},
    NamedChannel{"TC", TopCentre},
    NamedChannel{"TFL", TopFrontLeft},
    NamedChannel{"TFC", TopFrontCentre},
    NamedChannel{"TFR", TopFrontRight},
    NamedChannel{"TBL", TopBackLeft},
    NamedChannel{"TBC", TopBackCentre},
    NamedChannel{"TBR", TopBackRight},
    NamedChannel{"DL", DownmixLeft},
    NamedChannel{"DR", DownmixRight},
    NamedChannel{"WL", WideLeft},
    NamedChannel{"WR", WideRight},
    NamedChannel{"SDL", SurroundDirectLeft},
    NamedChannel{"SDR", SurroundDirectRight},
    NamedChannel{"LFE2", LowFrequency2},
    NamedChannel{"TSL", TopSideLeft},
    NamedChannel{"TSR", TopSideRight},
    NamedChannel{"BFC", BottomFrontCentre},
    NamedChannel{"BFL", BottomFrontLeft},
    NamedChannel{"BFR", BottomFrontRight},
    NamedChannel{"SSL", SideSurroundLeft},
    NamedChannel{"SSR", SideSurroundRight},
    NamedChannel{"TTL", TopSurroundLeft},
    NamedChannel{"TTR", TopSurroundRight},
    NamedChannel{"BIL", BinauralLeft},
    NamedChannel{"BIR", BinauralRight},
    NamedChannel{"L", FrontLeft},
    NamedChannel{"R", FrontRight},
    NamedChannel{"C", FrontCentre},
};

// -3 dB, FFmpeg's default centre and surround mix level (M_SQRT1_2).
constexpr double kMinus3dB = 0.70710678118654752440;

struct StereoGain final {
    double left = 0.0;
    double right = 0.0;
};

// ITU-R BS.775 Lo/Ro: L + 0.707 C + 0.707 Ls and R + 0.707 C + 0.707 Rs, LFE omitted. Channels
// the standard does not name fold into their own side at -3 dB like the surrounds, and
// centre-line channels other than front centre split at -3 dB and then fold at -3 dB (-6 dB),
// matching FFmpeg's treatment of back centre.
[[nodiscard]] constexpr StereoGain stereoGain(const AudioChannel channel) noexcept {
    switch (channel) {
    case FrontLeft:
    case FrontLeftOfCentre:
    case DownmixLeft:
    case BinauralLeft:
        return {1.0, 0.0};
    case FrontRight:
    case FrontRightOfCentre:
    case DownmixRight:
    case BinauralRight:
        return {0.0, 1.0};
    case FrontCentre:
        return {kMinus3dB, kMinus3dB};
    case BackLeft:
    case SideLeft:
    case WideLeft:
    case SurroundDirectLeft:
    case TopFrontLeft:
    case TopBackLeft:
    case TopSideLeft:
    case BottomFrontLeft:
    case SideSurroundLeft:
    case TopSurroundLeft:
        return {kMinus3dB, 0.0};
    case BackRight:
    case SideRight:
    case WideRight:
    case SurroundDirectRight:
    case TopFrontRight:
    case TopBackRight:
    case TopSideRight:
    case BottomFrontRight:
    case SideSurroundRight:
    case TopSurroundRight:
        return {0.0, kMinus3dB};
    case BackCentre:
    case TopCentre:
    case TopFrontCentre:
    case TopBackCentre:
    case BottomFrontCentre:
        return {0.5, 0.5};
    case LowFrequency:
    case LowFrequency2:
    case Unknown:
        return {};
    }
    return {};
}

[[nodiscard]] AudioChannelLayout layoutOf(std::initializer_list<AudioChannel> channels) noexcept {
    AudioChannelLayout layout{};
    std::copy(channels.begin(), channels.end(), layout.begin());
    return layout;
}

} // namespace

AudioChannel audioChannelFromName(const std::string_view name) noexcept {
    for (const auto& entry : kNames)
        if (entry.name == name)
            return entry.channel;
    return Unknown;
}

AudioChannelLayout audioChannelLayout(const std::span<const std::string> names) noexcept {
    AudioChannelLayout layout{};
    if (names.size() > layout.size())
        return layout;
    for (std::size_t index = 0; index < names.size(); ++index)
        layout[index] = audioChannelFromName(names[index]);
    return layout;
}

AudioChannelLayout waveChannelLayout(const std::uint32_t mask,
                                     const std::uint32_t channels) noexcept {
    // Bits 0-17 are FL FR FC LFE BL BR FLC FRC BC SL SR TC TFL TFC TFR TBL TBC TBR, the same
    // order as AudioChannel after Unknown. Reserved bits declare nothing.
    constexpr std::uint32_t kKnownSpeakers = (1U << 18U) - 1U;
    AudioChannelLayout layout{};
    if (channels == 0 || channels > layout.size() || (mask & ~kKnownSpeakers) != 0)
        return layout;
    std::uint32_t placed = 0;
    for (std::uint32_t bit = 0; bit < 18U; ++bit) {
        if ((mask & (1U << bit)) == 0)
            continue;
        if (placed == channels)
            return AudioChannelLayout{};
        layout[placed++] = static_cast<AudioChannel>(bit + 1U);
    }
    return placed == channels ? layout : AudioChannelLayout{};
}

AudioChannelLayout defaultAudioChannelLayout(const std::uint32_t channels) noexcept {
    switch (channels) {
    case 0:
        return {};
    case 1:
        return layoutOf({FrontCentre});
    case 2:
        return layoutOf({FrontLeft, FrontRight});
    case 3:
        return layoutOf({FrontLeft, FrontRight, FrontCentre});
    case 4:
        return layoutOf({FrontLeft, FrontRight, BackLeft, BackRight});
    case 5:
        return layoutOf({FrontLeft, FrontRight, FrontCentre, BackLeft, BackRight});
    case 6:
        return layoutOf({FrontLeft, FrontRight, FrontCentre, LowFrequency, BackLeft, BackRight});
    case 7:
        return layoutOf(
            {FrontLeft, FrontRight, FrontCentre, LowFrequency, BackCentre, SideLeft, SideRight});
    case 8:
        return layoutOf({FrontLeft, FrontRight, FrontCentre, LowFrequency, BackLeft, BackRight,
                         SideLeft, SideRight});
    default:
        return layoutOf({FrontLeft, FrontRight});
    }
}

AudioChannelLayout resolvedAudioChannelLayout(const AudioChannelLayout& declared,
                                              const std::uint32_t channels) noexcept {
    if (channels == 0 || channels > declared.size())
        return defaultAudioChannelLayout(channels);
    const auto end = declared.begin() + static_cast<std::ptrdiff_t>(channels);
    return std::find(declared.begin(), end, Unknown) == end ? declared
                                                            : defaultAudioChannelLayout(channels);
}

std::vector<float> channelMixMatrix(const AudioChannelLayout& declared,
                                    const std::uint32_t sourceChannels,
                                    const std::uint32_t outputChannels) {
    std::vector<float> gains(static_cast<std::size_t>(sourceChannels) * outputChannels, 0.0F);
    if (sourceChannels == 0 || outputChannels == 0)
        return gains;
    const auto at = [&](const std::uint32_t output, const std::uint32_t source) -> float& {
        return gains[static_cast<std::size_t>(output) * sourceChannels + source];
    };
    if (sourceChannels == 1) {
        for (std::uint32_t output = 0; output < outputChannels; ++output)
            at(output, 0) = 1.0F;
        return gains;
    }
    if (outputChannels > 2) {
        for (std::uint32_t channel = 0; channel < std::min(sourceChannels, outputChannels);
             ++channel)
            at(channel, channel) = 1.0F;
        return gains;
    }
    const auto layout = resolvedAudioChannelLayout(declared, sourceChannels);
    for (std::uint32_t source = 0; source < sourceChannels; ++source) {
        const auto gain = source < layout.size() ? stereoGain(layout[source]) : StereoGain{};
        if (outputChannels == 2) {
            at(0, source) = static_cast<float>(gain.left);
            at(1, source) = static_cast<float>(gain.right);
        } else {
            at(0, source) = static_cast<float>(kMinus3dB * (gain.left + gain.right));
        }
    }
    return gains;
}

} // namespace bloom::media::audio
