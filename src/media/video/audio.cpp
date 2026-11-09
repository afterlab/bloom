#include <algorithm>
#include <bloom/media/video/audio.hpp>
#include <new>
#include <cmath>
#include <limits>
namespace bloom::media::video {
std::optional<std::uint64_t>
audioClipFrameCount(const provider::StreamDescriptor& stream) noexcept {
    if (stream.duration.numerator <= 0 || stream.duration.denominator <= 0 ||
        stream.sampleRate == 0 || stream.channelLayout.empty() ||
        stream.channelLayout.size() > provider::Limits::channels)
        return {};
    const auto samples = static_cast<long double>(stream.duration.numerator) /
                         static_cast<long double>(stream.duration.denominator) * stream.sampleRate;
    const auto ceiling = static_cast<long double>(std::min<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::size_t>::max()));
    if (!std::isfinite(samples) || samples <= 0.0L || samples >= ceiling)
        return {};
    // A positive fractional tail is still an audible frame in range/export playback.
    return static_cast<std::uint64_t>(std::ceil(samples));
}
const provider::StreamDescriptor* audioStreamAt(const provider::ProbeResult& probe,
                                                const std::uint32_t ordinal) noexcept {
    std::uint32_t seen = 0;
    for (const auto& stream : probe.streams)
        if (stream.kind == provider::MediaKind::Audio && seen++ == ordinal)
            return &stream;
    return nullptr;
}
namespace {
[[nodiscard]] audio::AudioError audioErrorOf(const provider::Unavailable& error) noexcept {
    switch (error.reason) {
    case provider::Error::Cancelled:
        return audio::AudioError::codeOnly(audio::AudioErrorCode::Cancelled);
    case provider::Error::Oversized:
        return audio::AudioError::codeOnly(audio::AudioErrorCode::SampleBudgetExceeded);
    case provider::Error::SourceChanged:
    case provider::Error::IdentityMismatch:
        return audio::AudioError::codeOnly(audio::AudioErrorCode::FileReadFailed);
    default:
        return audio::AudioError::codeOnly(audio::AudioErrorCode::DecoderFailure);
    }
}

// Decodes forward through one session; a request that does not continue the previous block
// repositions the session's retained worker (the worker seeks the container, it does not decode
// the span in between).
class VideoAudioStream final : public audio::AudioStream {
  public:
    VideoAudioStream(audio::AudioProbe audioProbe, const std::size_t windowFrames,
                     std::shared_ptr<VideoDecodeSession> session, provider::ProbeResult source,
                     const std::uint32_t streamId)
        : AudioStream(std::move(audioProbe), windowFrames), session_(std::move(session)),
          source_(std::move(source)), streamId_(streamId) {}

  protected:
    [[nodiscard]] audio::AudioResult<std::size_t>
    fill(const std::uint64_t firstFrame, const std::span<float> output,
         const audio::CancelAudioWork& cancel) noexcept override {
        try {
            const auto channels = static_cast<std::size_t>(probe().channels);
            const auto total = output.size() / channels;
            const Cancel stop = [&cancel] { return cancel && cancel(); };
            for (std::size_t done = 0; done < total;) {
                const auto count = static_cast<std::uint32_t>(
                    std::min<std::size_t>(provider::Limits::audioSamples, total - done));
                auto decoded = session_->audio(source_, streamId_, firstFrame + done, count, stop);
                if (const auto* error = std::get_if<provider::Unavailable>(&decoded))
                    return audio::AudioResult<std::size_t>::failure(audioErrorOf(*error));
                const auto& block = std::get<provider::AudioBlock>(decoded);
                if (block.channels.size() != channels || block.sampleRate != probe().rate)
                    return audio::AudioResult<std::size_t>::failure(
                        audio::AudioError::codeOnly(audio::AudioErrorCode::DecoderFailure));
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    if (block.channels[channel].size() != count)
                        return audio::AudioResult<std::size_t>::failure(
                            audio::AudioError::codeOnly(audio::AudioErrorCode::DecoderFailure));
                    for (std::size_t frame = 0; frame < count; ++frame)
                        output[(done + frame) * channels + channel] = block.channels[channel][frame];
                }
                done += count;
            }
            return audio::AudioResult<std::size_t>::success(total);
        } catch (const std::bad_alloc&) {
            return audio::AudioResult<std::size_t>::failure(
                audio::AudioError::codeOnly(audio::AudioErrorCode::AllocationFailure));
        } catch (...) {
            return audio::AudioResult<std::size_t>::failure(
                audio::AudioError::codeOnly(audio::AudioErrorCode::DecoderFailure));
        }
    }

  private:
    std::shared_ptr<VideoDecodeSession> session_;
    provider::ProbeResult source_;
    std::uint32_t streamId_;
};
} // namespace

std::optional<audio::AudioProbe> videoAudioProbe(const provider::StreamDescriptor& stream) noexcept {
    const auto frames = audioClipFrameCount(stream);
    if (!frames)
        return {};
    audio::AudioProbe probe;
    probe.container = audio::AudioContainer::Video;
    probe.rate = stream.sampleRate;
    probe.channels = static_cast<std::uint32_t>(stream.channelLayout.size());
    probe.frames = *frames;
    probe.duration = core::RationalTime::create(static_cast<std::int64_t>(probe.frames),
                                                static_cast<std::int64_t>(probe.rate))
                         .value_or(core::RationalTime{});
    probe.channelLayout = audio::audioChannelLayout(stream.channelLayout);
    return probe;
}

provider::Result<audio::AudioBuffer>
openWindowedVideoAudio(std::shared_ptr<VideoDecodeSession> session, provider::ProbeResult probe,
                       const std::uint32_t audioStream, const std::size_t windowFrames) {
    const auto* stream = audioStreamAt(probe, audioStream);
    if (stream == nullptr || !session)
        return provider::Unavailable{provider::Error::Unavailable,
                                     audioStream == 0 ? "Video has no audio stream"
                                                      : "The selected audio stream does not exist"};
    const auto audioProbe = videoAudioProbe(*stream);
    if (!audioProbe || windowFrames == 0)
        return provider::Unavailable{provider::Error::Oversized,
                                     "Video audio sample count exceeds addressable storage"};
    audio::AudioBuffer buffer;
    buffer.rate = audioProbe->rate;
    buffer.channels = audioProbe->channels;
    buffer.frames = audioProbe->frames;
    buffer.channelLayout = audioProbe->channelLayout;
    try {
        buffer.stream = std::make_shared<VideoAudioStream>(*audioProbe, windowFrames,
                                                           std::move(session), std::move(probe),
                                                           stream->id);
    } catch (const std::bad_alloc&) {
        return provider::Unavailable{provider::Error::Oversized, "Video audio allocation failed"};
    }
    return buffer;
}

provider::Result<audio::AudioBuffer> decodeAudioClip(VideoDecodeSession& session,
                                                     const provider::ProbeResult& probe,
                                                     const std::uint64_t admittedSamples,
                                                     const Cancel& cancel,
                                                     const std::uint32_t audioStream) {
    const auto* stream = audioStreamAt(probe, audioStream);
    if (stream == nullptr)
        return provider::Unavailable{provider::Error::Unavailable,
                                     audioStream == 0 ? "Video has no audio stream"
                                                      : "The selected audio stream does not exist"};
    const auto frames = audioClipFrameCount(*stream);
    if (!frames || *frames > admittedSamples / stream->channelLayout.size() ||
        *frames > std::numeric_limits<std::size_t>::max() /
                      (stream->channelLayout.size() * sizeof(float)))
        return provider::Unavailable{provider::Error::Oversized,
                                     "Video audio exceeds its admitted sample storage"};
    audio::AudioBuffer buffer;
    buffer.rate = stream->sampleRate;
    buffer.channels = static_cast<std::uint32_t>(stream->channelLayout.size());
    buffer.frames = *frames;
    buffer.channelLayout = audio::audioChannelLayout(stream->channelLayout);
    buffer.planes.resize(buffer.channels);
    for (auto& channel : buffer.planes)
        channel.reserve(static_cast<std::size_t>(buffer.frames));
    for (std::uint64_t start = 0; start < buffer.frames;) {
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(provider::Limits::audioSamples, buffer.frames - start));
        auto decoded = session.audio(probe, stream->id, start, count, cancel);
        if (const auto* error = std::get_if<provider::Unavailable>(&decoded))
            return *error;
        auto& block = std::get<provider::AudioBlock>(decoded);
        if (block.sampleRate != buffer.rate || block.channels.size() != buffer.channels ||
            block.channels.front().size() != count)
            return provider::Unavailable{provider::Error::IdentityMismatch,
                                         "Video audio descriptor changed"};
        for (std::size_t channel = 0; channel < block.channels.size(); ++channel)
            buffer.planes[channel].insert(buffer.planes[channel].end(),
                                          block.channels[channel].begin(),
                                          block.channels[channel].end());
        start += count;
    }
    return buffer;
}
} // namespace bloom::media::video
