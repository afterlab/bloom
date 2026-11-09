// #347 audio analysis against signals whose answers are known: a sine's peak and RMS, a window
// that straddles the start of the source, the minimum and maximum of a ramp, the lane of each
// channel, the bin a bin-centred sine lands in, and cancellation.
#include <bloom/media/audio/analysis.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace bloom::media::audio;
namespace an = bloom::media::audio::analysis;

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 48000;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
bool near(const double a, const double b, const double tolerance) {
    return std::abs(a - b) <= tolerance;
}
bool near(const float a, const double b, const double tolerance) {
    return near(static_cast<double>(a), b, tolerance);
}

AudioBuffer sineBuffer(const std::vector<double>& amplitudes, const double frequency,
                       const std::uint64_t frames) {
    AudioBuffer buffer;
    buffer.rate = kRate;
    buffer.channels = static_cast<std::uint32_t>(amplitudes.size());
    buffer.frames = frames;
    for (const double amplitude : amplitudes) {
        std::vector<float> plane(frames);
        for (std::uint64_t i = 0; i < frames; ++i)
            plane[i] = static_cast<float>(amplitude *
                                          std::sin(2.0 * kPi * frequency * static_cast<double>(i) /
                                                   kRate));
        buffer.planes.push_back(std::move(plane));
    }
    return buffer;
}

AudioBuffer constantBuffer(const float value, const std::uint64_t frames) {
    AudioBuffer buffer;
    buffer.rate = kRate;
    buffer.channels = 1;
    buffer.frames = frames;
    buffer.planes.push_back(std::vector<float>(frames, value));
    return buffer;
}

// A stream that serves a sine through the playback-window interface and counts its reads.
class SineStream final : public AudioStream {
  public:
    SineStream(const std::uint64_t frames, const double frequency)
        : AudioStream(makeProbe(frames), 4096), frequency_(frequency) {}
    std::atomic<std::uint64_t> framesServed{0};

  protected:
    AudioResult<std::size_t> fill(const std::uint64_t firstFrame, const std::span<float> output,
                                  const CancelAudioWork&) noexcept override {
        const auto count = output.size();
        for (std::size_t i = 0; i < count; ++i)
            output[i] = static_cast<float>(
                0.8 * std::sin(2.0 * kPi * frequency_ * static_cast<double>(firstFrame + i) /
                               kRate));
        framesServed += count;
        return AudioResult<std::size_t>::success(count);
    }

  private:
    static AudioProbe makeProbe(const std::uint64_t frames) {
        AudioProbe probe;
        probe.rate = kRate;
        probe.channels = 1;
        probe.frames = frames;
        return probe;
    }
    double frequency_;
};

void sinePeakAndRms() {
    const auto buffer = sineBuffer({0.5}, 1000.0, kRate);
    const auto peak = an::amplitude(buffer, 0, kRate, an::Measure::Peak);
    expect(peak && near(*peak.value(), 0.5, 1e-4), "a 0.5 sine peaks at 0.5");
    const auto rms = an::amplitude(buffer, 0, kRate, an::Measure::Rms);
    expect(rms && near(*rms.value(), 0.5 / std::sqrt(2.0), 1e-4), "a 0.5 sine has an RMS of 0.3536");
    // The channels of a stereo source are measured together: the peak is the louder channel's and
    // the RMS is of every sample of both.
    const auto stereo = sineBuffer({0.8, 0.2}, 1000.0, kRate);
    const auto stereoPeak = an::amplitude(stereo, 0, kRate, an::Measure::Peak);
    expect(stereoPeak && near(*stereoPeak.value(), 0.8, 1e-4), "the stereo peak is the louder channel");
    const auto stereoRms = an::amplitude(stereo, 0, kRate, an::Measure::Rms);
    const double expectedRms = std::sqrt((0.8 * 0.8 + 0.2 * 0.2) / 4.0);
    expect(stereoRms && near(*stereoRms.value(), expectedRms, 1e-4), "the stereo RMS covers both channels");
}

void silenceOutsideTheSource() {
    const auto buffer = constantBuffer(0.5F, 1000);
    // 200 frames, the first 100 before the start: half the window is silence.
    const auto rms = an::amplitude(buffer, -100, 200, an::Measure::Rms);
    expect(rms && near(*rms.value(), std::sqrt(0.25 * 0.5), 1e-6), "frames before the start are silence");
    const auto peak = an::amplitude(buffer, -100, 200, an::Measure::Peak);
    expect(peak && near(*peak.value(), 0.5, 1e-7), "the peak of a window that straddles the start");
    const auto after = an::amplitude(buffer, 5000, 100, an::Measure::Peak);
    expect(after && *after.value() == 0.0, "a window past the end reads silence");
    const auto empty = an::amplitude(buffer, 0, 0, an::Measure::Peak);
    expect(!empty, "an empty window is refused");
    const auto huge = an::amplitude(buffer, 0, an::kMaximumWindowFrames + 1, an::Measure::Peak);
    expect(!huge, "a window beyond the admitted length is refused");
}

void waveformColumns() {
    AudioBuffer ramp;
    ramp.rate = kRate;
    ramp.channels = 2;
    ramp.frames = 8;
    ramp.planes = {{-1.0F, -0.75F, -0.5F, -0.25F, 0.0F, 0.25F, 0.5F, 0.75F},
                   {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F}};
    const auto mixed = an::waveform(ramp, 0, 8, 4, false);
    expect(mixed && mixed.value()->lanes == 1 && mixed.value()->columns == 4, "one mixed lane");
    if (mixed) {
        const auto& v = mixed.value()->values;
        // Column 0 covers frames 0 and 1: the channel mean runs -0.45, -0.275.
        expect(near(v[0], -0.45, 1e-6) && near(v[1], -0.275, 1e-6), "mixed column 0 minimum and maximum");
        expect(near(v[6], 0.6, 1e-6) && near(v[7], 0.775, 1e-6), "mixed column 3 minimum and maximum");
    }
    const auto separate = an::waveform(ramp, 0, 8, 4, true);
    expect(separate && separate.value()->lanes == 2, "one lane per channel");
    if (separate) {
        const auto& v = separate.value()->values;
        expect(near(v[0], -1.0, 1e-6) && near(v[1], -0.75, 1e-6), "left column 0");
        expect(near(v[2], 0.1, 1e-6) && near(v[3], 0.2, 1e-6), "right column 0");
        expect(near(v[12], 0.5, 1e-6) && near(v[13], 0.75, 1e-6), "left column 3");
    }
    // A window shorter than the image is wide: every column still has a value.
    const auto narrow = an::waveform(ramp, 0, 2, 8, false);
    expect(narrow && narrow.value()->columns == 8, "a window shorter than the columns");
    // A window that is partly before the start has silent columns first.
    const auto early = an::waveform(constantBuffer(0.5F, 100), -100, 200, 2, false);
    expect(early && near(early.value()->values[0], 0.0, 1e-9) && near(early.value()->values[1], 0.0, 1e-9) &&
               near(early.value()->values[2], 0.5, 1e-9) && near(early.value()->values[3], 0.5, 1e-9),
           "the part before the source is silent");
}

void spectrumPeak() {
    constexpr std::uint32_t fft = 4096;
    // A frequency at the centre of bin 100, so the Hann window does not scatter the sine.
    const double frequency = static_cast<double>(kRate) * 100.0 / fft;
    const auto buffer = sineBuffer({0.5}, frequency, kRate);
    an::SpectrumSettings settings;
    settings.fftSize = fft;
    settings.lowHz = 20.0;
    settings.highHz = 20000.0;
    settings.logarithmic = true;
    settings.floorDb = -90.0;
    settings.ceilingDb = 0.0;
    const auto result = an::spectrum(buffer, 24000, settings, 256);
    expect(result && result.value()->size() == 256, "256 columns");
    if (!result)
        return;
    const auto& levels = *result.value();
    const auto loudest = std::max_element(levels.begin(), levels.end()) - levels.begin();
    // The column that holds the sine's frequency: log2 position of the frequency in 20..20000.
    const double unit = std::log(frequency / 20.0) / std::log(1000.0);
    expect(std::abs(loudest - static_cast<long>(unit * 256.0)) <= 1,
           "the loudest column is the one that holds the frequency");
    // 0.5 is -6.02 dB, which maps to (90 - 6.0206) / 90.
    expect(near(levels[static_cast<std::size_t>(loudest)], (90.0 - 6.0206) / 90.0, 2e-3),
           "a 0.5 sine reads -6 dB");
    expect(levels[10] < 0.02F && levels[250] < 0.02F, "the columns far from the sine are at the floor");
    // Silence is the floor.
    const auto silent = an::spectrum(constantBuffer(0.0F, 8192), 4096, settings, 64);
    expect(silent && *std::max_element(silent.value()->begin(), silent.value()->end()) == 0.0F,
           "silence reads the floor");
    // A linear axis puts the same frequency at its linear place.
    settings.logarithmic = false;
    settings.highHz = 4000.0;
    const auto linear = an::spectrum(buffer, 24000, settings, 200);
    if (linear) {
        const auto& l = *linear.value();
        const auto at = std::max_element(l.begin(), l.end()) - l.begin();
        const double expected = (frequency - 20.0) / (4000.0 - 20.0) * 200.0;
        expect(std::abs(at - static_cast<long>(expected)) <= 1, "the linear axis places the frequency");
    } else {
        expect(false, "a linear spectrum is produced");
    }
    settings.fftSize = 1000;
    expect(!an::spectrum(buffer, 0, settings, 10), "a non power of two transform is refused");
}

void streamsAndCancellation() {
    auto stream = std::make_shared<SineStream>(kRate * 60, 440.0);
    AudioBuffer buffer;
    buffer.rate = kRate;
    buffer.channels = 1;
    buffer.frames = stream->probe().frames;
    buffer.stream = stream;
    // A short window of a long stream reads only its own frames (plus the playback window).
    const auto peak = an::amplitude(buffer, 48000 * 30, 4800, an::Measure::Peak);
    expect(peak && near(*peak.value(), 0.8, 1e-3), "the peak of a streamed window");
    expect(stream->framesServed.load() <= 3 * 4096, "only the window is decoded from a long stream");
    // Cancellation stops a long read.
    int calls = 0;
    const auto cancelled = an::amplitude(buffer, 0, 1 << 20, an::Measure::Rms, [&] { return ++calls > 2; });
    expect(!cancelled && cancelled.error() != nullptr && cancelled.error()->code == AudioErrorCode::Cancelled,
           "a cancelled analysis reports Cancelled");
    expect(stream->framesServed.load() < 20 * 4096, "a cancelled read stops early");
    // The same window gives the same numbers.
    const auto again = an::amplitude(buffer, 48000 * 30, 4800, an::Measure::Peak);
    expect(again && *again.value() == *peak.value(), "analysis is repeatable");
}

} // namespace

int main() {
    sinePeakAndRms();
    silenceOutsideTheSource();
    waveformColumns();
    spectrumPeak();
    streamsAndCancellation();
    if (failures == 0)
        std::cout << "audio analysis ok\n";
    return failures == 0 ? 0 : 1;
}
