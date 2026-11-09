#include <bloom/media/audio/audio.hpp>
#include <bloom/media/audio/channel_layout.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef BLOOM_AUDIO_FIXTURE_DIR
#error "BLOOM_AUDIO_FIXTURE_DIR must name the checked-in audio fixtures"
#endif

namespace {

using bloom::core::RationalTime;
using bloom::media::audio::AudioChannel;
using bloom::media::audio::AudioChannelLayout;
using bloom::media::audio::AudioContainer;
using bloom::media::audio::AudioDecodeLimits;
using bloom::media::audio::AudioErrorCode;
using bloom::media::audio::AudioProbe;
using bloom::media::audio::AudioResult;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TemporaryFile final {
  public:
    explicit TemporaryFile(const std::string_view suffix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("bloom-audio-" + std::to_string(stamp) + std::string{suffix});
    }

    ~TemporaryFile() {
        std::error_code status;
        std::filesystem::remove(path_, status);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void writeU16(std::ofstream& output, const std::uint16_t value) {
    const std::array<std::byte, 2> bytes{
        std::byte{static_cast<unsigned char>(value & 0xFFU)},
        std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)},
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void writeU32(std::ofstream& output, const std::uint32_t value) {
    const std::array<std::byte, 4> bytes{
        std::byte{static_cast<unsigned char>(value & 0xFFU)},
        std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)},
        std::byte{static_cast<unsigned char>((value >> 16U) & 0xFFU)},
        std::byte{static_cast<unsigned char>((value >> 24U) & 0xFFU)},
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct GeneratedWav final {
    std::vector<float> left;
    std::vector<float> right;
};

GeneratedWav writeSineWav(const std::filesystem::path& path) {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::uint32_t frames = rate;
    GeneratedWav samples;
    samples.left.reserve(frames);
    samples.right.reserve(frames);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto phase =
            2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / static_cast<double>(rate);
        const auto value = static_cast<float>(std::sin(phase) * 0.5);
        samples.left.push_back(value);
        samples.right.push_back(-value);
    }

    const auto dataBytes = frames * 2U * static_cast<std::uint32_t>(sizeof(float));
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    writeU32(output, 36U + dataBytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    writeU32(output, 16);
    writeU16(output, 3); // IEEE float
    writeU16(output, 2);
    writeU32(output, rate);
    writeU32(output, rate * 2U * static_cast<std::uint32_t>(sizeof(float)));
    writeU16(output, 2U * static_cast<std::uint16_t>(sizeof(float)));
    writeU16(output, 32);
    output.write("data", 4);
    writeU32(output, dataBytes);
    for (std::size_t frame = 0; frame < samples.left.size(); ++frame) {
        const auto left = std::bit_cast<std::uint32_t>(samples.left[frame]);
        const auto right = std::bit_cast<std::uint32_t>(samples.right[frame]);
        writeU32(output, left);
        writeU32(output, right);
    }
    output.close();
    return samples;
}

// A WAVE_FORMAT_EXTENSIBLE float file whose channel c holds the constant 0.1 * (c + 1).
void writeExtensibleWav(const std::filesystem::path& path, const std::uint16_t channels,
                        const std::uint32_t mask) {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::uint32_t frames = 64;
    const auto blockAlign = static_cast<std::uint16_t>(channels * sizeof(float));
    const auto dataBytes = frames * blockAlign;
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    writeU32(output, 4U + 8U + 40U + 8U + dataBytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    writeU32(output, 40);
    writeU16(output, 0xFFFE); // WAVE_FORMAT_EXTENSIBLE
    writeU16(output, channels);
    writeU32(output, rate);
    writeU32(output, rate * blockAlign);
    writeU16(output, blockAlign);
    writeU16(output, 32);
    writeU16(output, 22);
    writeU16(output, 32);
    writeU32(output, mask);
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    constexpr std::array<unsigned char, 16> subFormat{0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                                      0x00, 0x38, 0x9B, 0x71};
    output.write(reinterpret_cast<const char*>(subFormat.data()), subFormat.size());
    output.write("data", 4);
    writeU32(output, dataBytes);
    for (std::uint32_t frame = 0; frame < frames; ++frame)
        for (std::uint16_t channel = 0; channel < channels; ++channel)
            writeU32(output, std::bit_cast<std::uint32_t>(0.1F * static_cast<float>(channel + 1)));
}

[[nodiscard]] bool near(const float actual, const double expected) {
    return std::abs(static_cast<double>(actual) - expected) < 1e-6;
}

[[nodiscard]] AudioChannelLayout layoutOf(std::initializer_list<AudioChannel> channels) {
    AudioChannelLayout layout{};
    std::copy(channels.begin(), channels.end(), layout.begin());
    return layout;
}

// #450: a source layout that differs from the output is downmixed (ITU-R BS.775 with FFmpeg's
// default levels) instead of mapping channel i to output i and dropping the rest.
void testChannelLayoutsAndDownmix(Expectations& expectations) {
    using enum AudioChannel;
    namespace audio = bloom::media::audio;
    constexpr double h = 0.70710678118654752440;
    const std::vector<std::string> ffmpegSide{"FL", "FR", "FC", "LFE", "SL", "SR"};
    const auto side = audio::audioChannelLayout(ffmpegSide);
    expectations.expect(
        side == layoutOf({FrontLeft, FrontRight, FrontCentre, LowFrequency, SideLeft, SideRight}),
        "FFmpeg channel names parse into speaker roles");
    expectations.expect(audio::audioChannelFromName("L") == FrontLeft &&
                            audio::audioChannelFromName("R") == FrontRight &&
                            audio::audioChannelFromName("channel-3") == Unknown,
                        "bare L/R are recognised and placeholders stay unknown");

    const auto stereo = audio::channelMixMatrix(side, 6, 2);
    const std::array<double, 12> expectedStereo{1, 0, h, 0, h, 0, 0, 1, h, 0, 0, h};
    bool stereoOk = stereo.size() == expectedStereo.size();
    for (std::size_t index = 0; stereoOk && index < stereo.size(); ++index)
        stereoOk = near(stereo[index], expectedStereo[index]);
    expectations.expect(stereoOk, "5.1 downmixes to stereo with centre and surrounds at -3 dB, "
                                  "LFE dropped");

    const auto mono = audio::channelMixMatrix(side, 6, 1);
    const std::array<double, 6> expectedMono{h, h, 1.0, 0.0, 0.5, 0.5};
    bool monoOk = mono.size() == expectedMono.size();
    for (std::size_t index = 0; monoOk && index < mono.size(); ++index)
        monoOk = near(mono[index], expectedMono[index]);
    expectations.expect(monoOk, "mono output sums the stereo downmix at -3 dB");

    const auto identity = audio::channelMixMatrix(AudioChannelLayout{}, 2, 2);
    expectations.expect(identity == std::vector<float>{1.0F, 0.0F, 0.0F, 1.0F},
                        "undeclared stereo into stereo is the identity");
    expectations.expect(audio::channelMixMatrix(AudioChannelLayout{}, 1, 2) ==
                            std::vector<float>{1.0F, 1.0F},
                        "a mono source feeds both outputs at unity");

    const std::vector<std::string> unnamed{"channel-0", "channel-1", "channel-2",
                                           "channel-3", "channel-4", "channel-5"};
    expectations.expect(
        audio::resolvedAudioChannelLayout(audio::audioChannelLayout(unnamed), 6) ==
            layoutOf({FrontLeft, FrontRight, FrontCentre, LowFrequency, BackLeft, BackRight}),
        "an undeclared six-channel source takes the default 5.1 layout");
    expectations.expect(audio::resolvedAudioChannelLayout(AudioChannelLayout{}, 4) ==
                            layoutOf({FrontLeft, FrontRight, BackLeft, BackRight}),
                        "an undeclared four-channel source is quad");

    expectations.expect(
        audio::waveChannelLayout(0x60FU, 6) ==
            layoutOf({FrontLeft, FrontRight, FrontCentre, LowFrequency, SideLeft, SideRight}),
        "a WAVE channel mask declares speakers in bit order");
    expectations.expect(audio::waveChannelLayout(0x3FU, 4) == AudioChannelLayout{} &&
                            audio::waveChannelLayout(0U, 2) == AudioChannelLayout{},
                        "a mask that disagrees with the channel count declares nothing");

    TemporaryFile file{".wav"};
    writeExtensibleWav(file.path(), 4, 0x0FU); // FL FR FC LFE, not the quad default
    const auto expected = layoutOf({FrontLeft, FrontRight, FrontCentre, LowFrequency});
    const auto probe = audio::probeAudio(file.path());
    expectations.expect(probe && probe.value()->channelLayout == expected,
                        "WAV probe reports the extensible channel mask");
    const auto decoded = audio::decodeAudio(file.path(), AudioDecodeLimits{.sampleBudget = 256});
    expectations.expect(decoded && decoded.value()->channelLayout == expected,
                        "decoded WAV carries the extensible channel mask");
    const auto windowed =
        audio::openWindowedAudio(file.path(), AudioDecodeLimits{.sampleBudget = 4096U * 4U});
    expectations.expect(windowed && windowed.value()->channelLayout == expected,
                        "windowed WAV playback carries the extensible channel mask");
}

void testWavRoundTripAndWaveform(Expectations& expectations) {
    TemporaryFile file{".wav"};
    const auto expected = writeSineWav(file.path());
    const auto probe = bloom::media::audio::probeAudio(file.path());
    const auto expectedDuration = RationalTime::create(1, 1);
    expectations.expect(probe && probe.value()->container == AudioContainer::Wav &&
                            probe.value()->rate == 48'000 && probe.value()->channels == 2 &&
                            probe.value()->frames == 48'000 && expectedDuration.has_value() &&
                            probe.value()->duration == *expectedDuration,
                        "generated WAV probe reports exact stereo 48 kHz one-second metadata");

    const auto decoded = bloom::media::audio::decodeAudio(
        file.path(), AudioDecodeLimits{.sampleBudget = 48'000U * 2U});
    expectations.expect(decoded && decoded.value()->planes.size() == 2 &&
                            decoded.value()->frames == expected.left.size(),
                        "generated WAV decodes into two complete planar channels");
    if (decoded) {
        const auto windowed = bloom::media::audio::openWindowedAudio(
            file.path(), AudioDecodeLimits{.sampleBudget = 4096U * 2U});
        expectations.expect(windowed && windowed.value()->stream &&
                                windowed.value()->planes.empty() &&
                                windowed.value()->frames == decoded.value()->frames,
                            "WAV playback retains a window instead of full planar PCM");
        if (windowed) {
            for (const auto frame :
                 {std::uint64_t{0}, std::uint64_t{4095}, std::uint64_t{4096}, std::uint64_t{8192},
                  std::uint64_t{47'999}, std::uint64_t{2}}) {
                for (std::uint32_t channel = 0; channel < 2; ++channel) {
                    const auto sample = windowed.value()->stream->sample(frame, channel);
                    expectations.expect(sample &&
                                            *sample == decoded.value()->planes[channel][frame],
                                        "window boundary and backward seek match full WAV decode");
                }
            }
            const auto streamingSummary =
                bloom::media::audio::waveformSummary(*windowed.value()->stream, 8);
            const auto referenceSummary = bloom::media::audio::waveformSummary(*decoded.value(), 8);
            expectations.expect(streamingSummary && referenceSummary &&
                                    streamingSummary.value()->buckets ==
                                        referenceSummary.value()->buckets,
                                "bounded WAV waveform matches whole-buffer reference");
            const auto cancelledSummary = bloom::media::audio::waveformSummary(
                *windowed.value()->stream, 8, [] { return true; });
            expectations.expect(!cancelledSummary &&
                                    cancelledSummary.error()->code == AudioErrorCode::Cancelled,
                                "streaming waveform observes cancellation");
        }
        for (std::size_t frame = 0; frame < expected.left.size(); ++frame) {
            expectations.expect(
                std::bit_cast<std::uint32_t>(decoded.value()->planes[0][frame]) ==
                        std::bit_cast<std::uint32_t>(expected.left[frame]) &&
                    std::bit_cast<std::uint32_t>(decoded.value()->planes[1][frame]) ==
                        std::bit_cast<std::uint32_t>(expected.right[frame]),
                "WAV float samples round-trip bit-exactly");
            if (expectations.failures() != 0 && frame > 4) {
                break;
            }
        }

        const auto waveform = bloom::media::audio::waveformSummary(*decoded.value(), 8);
        expectations.expect(waveform && waveform.value()->buckets.size() == 8 &&
                                waveform.value()->buckets.front().size() == 2,
                            "waveform summary has one min/max pair per channel and bucket");
        if (waveform) {
            expectations.expect(waveform.value()->buckets[0][0].minimum < 0.0F &&
                                    waveform.value()->buckets[0][0].maximum > 0.0F &&
                                    waveform.value()->buckets[0][0].minimum <
                                        waveform.value()->buckets[0][0].maximum,
                                "waveform bucket captures the sine range");
        }
    }
}

void testMp3Fixture(Expectations& expectations) {
    const auto path = std::filesystem::path{BLOOM_AUDIO_FIXTURE_DIR} / "sine-stereo.mp3";
    const auto probe = bloom::media::audio::probeAudio(path);
    expectations.expect(probe && probe.value()->container == AudioContainer::Mp3 &&
                            probe.value()->rate == 48'000 && probe.value()->channels == 2 &&
                            probe.value()->frames > 0,
                        "checked-in MP3 fixture probes as a non-empty stereo 48 kHz stream");
    const auto decoded = bloom::media::audio::decodeAudio(
        path, AudioDecodeLimits{.sampleBudget =
                                    probe ? probe.value()->frames * probe.value()->channels : 0});
    expectations.expect(decoded && decoded.value()->frames > 0 &&
                            decoded.value()->planes.size() == 2,
                        "checked-in MP3 fixture decodes into planar samples");
    const auto windowed =
        bloom::media::audio::openWindowedAudio(path, AudioDecodeLimits{.sampleBudget = 4096U * 2U});
    expectations.expect(windowed && windowed.value()->stream && windowed.value()->planes.empty(),
                        "MP3 opens with persistent bounded playback storage");
    if (decoded && windowed) {
        for (const auto frame : {std::uint64_t{0}, std::uint64_t{4095}, std::uint64_t{4096},
                                 decoded.value()->frames - 1, std::uint64_t{100}}) {
            for (std::uint32_t channel = 0; channel < 2; ++channel) {
                const auto sample = windowed.value()->stream->sample(frame, channel);
                expectations.expect(
                    sample &&
                        std::abs(*sample - decoded.value()->planes[channel][frame]) < 0.00001F,
                    "MP3 window and seek agree with full decoder");
            }
        }
        auto backend = std::make_unique<bloom::media::audio::playback::NullBackend>();
        auto* captured = backend.get();
        bloom::media::audio::playback::AudioEngine engine(std::move(backend), {48'000, 2, 1024});
        const auto buffer =
            std::make_shared<const bloom::media::audio::AudioBuffer>(std::move(*windowed.value()));
        static_cast<void>(engine.addClip({.buffer = buffer}));
        const auto start = RationalTime::create(4094, 48'000).value();
        expectations.expect(!engine.play(start).has_value() &&
                                !engine.renderForTesting(5).has_value(),
                            "mixer plays across MP3 window boundary");
        if (captured->captured().size() == 10)
            for (std::size_t frame = 0; frame < 5; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    expectations.expect(std::abs(captured->captured()[frame * 2 + channel] -
                                                 decoded.value()->planes[channel][4094 + frame]) <
                                            0.00001F,
                                        "mixer PCM matches reference across window boundary");
        captured->clearCaptured();
        expectations.expect(!engine.seek(RationalTime::create(100, 48'000).value()).has_value() &&
                                !engine.renderForTesting(2).has_value(),
                            "mixer seeks backward through persistent MP3 decoder");
        if (captured->captured().size() == 4)
            expectations.expect(
                std::abs(captured->captured()[0] - decoded.value()->planes[0][100]) < 0.00001F,
                "seeked mixer PCM matches reference");
    }
}

// Seeks land on the exact requested frame, backward and forward, without decoding the span in
// between: every probe of a windowed stream equals the sequential full decode.
void testMp3Seeks(Expectations& expectations) {
    for (const auto* name : {"sweep-stereo.mp3", "sweep-gapless.mp3"}) {
        const auto path = std::filesystem::path{BLOOM_AUDIO_FIXTURE_DIR} / name;
        const auto probe = bloom::media::audio::probeAudio(path);
        expectations.expect(probe && probe.value()->frames > 100'000,
                            std::string(name) + " probes as a multi-second stream");
        if (!probe)
            continue;
        const auto decoded = bloom::media::audio::decodeAudio(
            path, AudioDecodeLimits{.sampleBudget = probe.value()->frames * probe.value()->channels});
        const auto windowed = bloom::media::audio::openWindowedAudio(
            path, AudioDecodeLimits{.sampleBudget = 4096U * 2U});
        expectations.expect(decoded && windowed && windowed.value()->stream,
                            std::string(name) + " decodes and opens windowed");
        if (!decoded || !windowed)
            continue;
        const auto frames = decoded.value()->frames;
        expectations.expect(frames == probe.value()->frames, std::string(name) + " frame counts agree");
        // Forward, then far backward, then to the very end, then back near the start, then a
        // pseudo-random walk: every jump is a seek, none continues the previous window.
        std::vector<std::uint64_t> probes{0,         frames / 2, frames - 1, 5000,     frames - 4097,
                                          1,         frames / 3, 4096,       frames / 5, 90'000};
        std::uint64_t state = 12345;
        for (int step = 0; step < 40; ++step) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            probes.push_back((state >> 33U) % frames);
        }
        bool exact = true;
        for (const auto frame : probes)
            for (std::uint32_t channel = 0; channel < 2; ++channel) {
                const auto sample = windowed.value()->stream->sample(frame, channel);
                exact = exact && sample &&
                        std::abs(*sample - decoded.value()->planes[channel][frame]) < 0.00001F;
            }
        expectations.expect(exact, std::string(name) + " seeks agree with the sequential decode");
        // A read that spans a window and continues into the next one still equals the decode.
        std::vector<float> chunk(4096U * 2U);
        const auto read = windowed.value()->stream->readInterleaved(frames / 2 - 1000, chunk);
        bool contiguous = read && *read.value() == 4096;
        for (std::size_t i = 0; contiguous && i < 4096; ++i)
            for (std::uint32_t channel = 0; channel < 2; ++channel)
                contiguous = std::abs(chunk[i * 2 + channel] -
                                      decoded.value()->planes[channel][frames / 2 - 1000 + i]) <
                             0.00001F;
        expectations.expect(contiguous, std::string(name) + " window read after a seek is exact");
        // A cancelled open (the frame index is built there) fails instead of leaving a partial one.
        const auto cancelled = bloom::media::audio::openWindowedAudio(
            path, AudioDecodeLimits{.sampleBudget = 4096U * 2U}, bloom::media::audio::AudioStream::kWindowFrames,
            [] { return true; });
        expectations.expect(!cancelled && cancelled.error()->code == AudioErrorCode::Cancelled,
                            std::string(name) + " cancelled open is a typed cancellation");
    }
}

void testHostileInputsAndLimits(Expectations& expectations) {
    TemporaryFile wav{".wav"};
    const auto expected = writeSineWav(wav.path());
    std::ifstream input(wav.path(), std::ios::binary);
    const std::vector<char> source((std::istreambuf_iterator<char>(input)), {});

    TemporaryFile truncated{".wav"};
    std::ofstream truncatedOutput(truncated.path(), std::ios::binary);
    truncatedOutput.write(source.data(), 24);
    truncatedOutput.close();
    const auto truncatedResult = bloom::media::audio::decodeAudio(
        truncated.path(), AudioDecodeLimits{.sampleBudget = 48'000U * 2U});
    expectations.expect(!truncatedResult &&
                            truncatedResult.error()->code == AudioErrorCode::MalformedFile,
                        "truncated WAV fails cleanly without a partial buffer");

    TemporaryFile wrongMagic{".bin"};
    std::ofstream wrongMagicOutput(wrongMagic.path(), std::ios::binary);
    wrongMagicOutput.write("not audio", 9);
    wrongMagicOutput.close();
    const auto wrongMagicResult = bloom::media::audio::probeAudio(wrongMagic.path());
    expectations.expect(!wrongMagicResult &&
                            wrongMagicResult.error()->code == AudioErrorCode::InvalidMagic,
                        "wrong magic fails before any decoder call");

    AudioDecodeLimits sampleLimit;
    sampleLimit.sampleBudget = expected.left.size();
    const auto budgetResult = bloom::media::audio::decodeAudio(wav.path(), sampleLimit);
    expectations.expect(!budgetResult &&
                            budgetResult.error()->code == AudioErrorCode::SampleBudgetExceeded,
                        "sample budget rejects an interleaved sample count above the limit");

    const auto unadmitted = bloom::media::audio::decodeAudio(wav.path(), {});
    expectations.expect(!unadmitted &&
                            unadmitted.error()->code == AudioErrorCode::InvalidAudioFormat,
                        "whole-buffer decode requires caller-owned sample admission");
    const auto cancelled = bloom::media::audio::decodeAudio(
        wav.path(), AudioDecodeLimits{.sampleBudget = expected.left.size() * 2U},
        [] { return true; });
    expectations.expect(!cancelled && cancelled.error()->code == AudioErrorCode::Cancelled,
                        "cancelled audio work returns a typed cancellation result");

    AudioDecodeLimits fileLimit;
    fileLimit.fileSizeCap = 16;
    fileLimit.sampleBudget = expected.left.size() * 2U;
    const auto fileResult = bloom::media::audio::decodeAudio(wav.path(), fileLimit);
    expectations.expect(!fileResult && fileResult.error()->code == AudioErrorCode::FileTooLarge,
                        "file-size cap rejects input before it is read");

    TemporaryFile large{".wav"};
    constexpr std::uint32_t largeDataBytes = 65U * 1024U * 1024U;
    {
        std::ofstream output(large.path(), std::ios::binary);
        output.write("RIFF", 4);
        writeU32(output, 36U + largeDataBytes);
        output.write("WAVE", 4);
        output.write("fmt ", 4);
        writeU32(output, 16);
        writeU16(output, 3);
        writeU16(output, 2);
        writeU32(output, 48'000);
        writeU32(output, 48'000U * 8U);
        writeU16(output, 8);
        writeU16(output, 32);
        output.write("data", 4);
        writeU32(output, largeDataBytes);
        output.seekp(44 + largeDataBytes - 1);
        output.put('\0');
    }
    const auto largeProbe = bloom::media::audio::probeAudio(large.path());
    expectations.expect(largeProbe && largeProbe.value()->frames == largeDataBytes / 8,
                        "WAV metadata probe streams a file larger than the former 64 MiB cap");
    const auto longWindow = bloom::media::audio::openWindowedAudio(
        large.path(), AudioDecodeLimits{.sampleBudget = 4096U * 2U});
    expectations.expect(longWindow && longWindow.value()->stream &&
                            longWindow.value()->stream->retainedBytes() < 512U * 1024U &&
                            longWindow.value()->stream->sample(largeDataBytes / 8U - 1U, 0) == 0.0F,
                        "long WAV playback budget and retained PCM are independent of duration");
    const auto largeDecode = bloom::media::audio::decodeAudio(
        large.path(), AudioDecodeLimits{.sampleBudget = largeDataBytes / 4U});
    expectations.expect(largeDecode && largeDecode.value()->frames == largeDataBytes / 8,
                        "WAV decode streams a file larger than the former 64 MiB cap");
    std::size_t cancellationChecks = 0;
    const auto interrupted = bloom::media::audio::decodeAudio(
        large.path(), AudioDecodeLimits{.sampleBudget = largeDataBytes / 4U},
        [&] { return ++cancellationChecks > 100; });
    expectations.expect(!interrupted && interrupted.error()->code == AudioErrorCode::Cancelled &&
                            cancellationChecks > 100,
                        "chunked decode stops after cancellation without publishing audio");
    AudioDecodeLimits explicitFileLimit;
    explicitFileLimit.fileSizeCap = 64U * 1024U * 1024U;
    explicitFileLimit.sampleBudget = largeDataBytes / 4U;
    const auto explicitRefusal = bloom::media::audio::decodeAudio(large.path(), explicitFileLimit);
    expectations.expect(!explicitRefusal &&
                            explicitRefusal.error()->code == AudioErrorCode::FileTooLarge &&
                            bloom::media::audio::describeAudioError(*explicitRefusal.error())
                                    .find("configured encoded-file limit") != std::string::npos,
                        "explicit file limit reports an exact typed refusal");
}

} // namespace

int main() {
    Expectations expectations;
    testWavRoundTripAndWaveform(expectations);
    testChannelLayoutsAndDownmix(expectations);
    testMp3Fixture(expectations);
    testMp3Seeks(expectations);
    testHostileInputsAndLimits(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
