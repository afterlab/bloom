// #569 audio effects, through the engine: the same mixer the playback and the export mix run.
#include <bloom/media/audio/effects.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using bloom::core::RationalTime;
using bloom::media::audio::AudioBuffer;
using bloom::media::audio::effects::EffectKind;
using bloom::media::audio::effects::EffectSettings;
using bloom::media::audio::effects::makeEffectSettings;
using bloom::media::audio::playback::AudioClip;
using bloom::media::audio::playback::AudioEngine;
using bloom::media::audio::playback::AudioEffectRamp;
using bloom::media::audio::playback::NullBackend;

constexpr std::uint32_t kRate = 48'000;
constexpr double kPi = 3.14159265358979323846;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        ++failures_;
        std::cerr << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] RationalTime time(const std::int64_t numerator, const std::int64_t denominator = 1) {
    const auto result = RationalTime::create(numerator, denominator);
    if (!result.has_value())
        std::abort();
    return *result;
}

[[nodiscard]] std::vector<float> sine(const double frequency, const double amplitude,
                                      const std::size_t frames) {
    std::vector<float> samples(frames);
    for (std::size_t index = 0; index < frames; ++index)
        samples[index] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(index) / kRate));
    return samples;
}

[[nodiscard]] std::shared_ptr<const AudioBuffer> mono(const std::vector<float>& samples) {
    return std::make_shared<const AudioBuffer>(AudioBuffer{kRate, 1, samples.size(), {samples}});
}

[[nodiscard]] std::shared_ptr<const AudioBuffer> stereo(const std::vector<float>& left,
                                                        const std::vector<float>& right) {
    return std::make_shared<const AudioBuffer>(
        AudioBuffer{kRate, 2, left.size(), {left, right}});
}

struct Render final {
    std::vector<float> samples;
};

// Plays one clip with `effects` from `from` for `frames` frames, in pulls of `pull` frames.
[[nodiscard]] std::vector<float> render(const std::shared_ptr<const AudioBuffer>& buffer,
                                        std::vector<EffectSettings> effects,
                                        const std::size_t frames, const std::uint32_t channels = 1,
                                        const std::int64_t from = 0, const std::size_t pull = 0,
                                        const std::optional<AudioEffectRamp> ramp = {}) {
    auto backend = std::make_unique<NullBackend>();
    auto* captured = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{kRate, channels, 16'384});
    AudioClip clip{buffer, time(0), 1.0F, false, false};
    clip.effectKey = 7;
    clip.effects = std::move(effects);
    clip.effectRamp = ramp;
    static_cast<void>(engine.addClip(std::move(clip)));
    static_cast<void>(engine.play(time(from, kRate)));
    if (pull == 0) {
        static_cast<void>(engine.renderForTesting(frames));
    } else {
        for (std::size_t done = 0; done < frames; done += pull)
            static_cast<void>(engine.renderForTesting(std::min(pull, frames - done)));
    }
    return {captured->captured().begin(), captured->captured().end()};
}

[[nodiscard]] double rms(const std::vector<float>& samples, const std::size_t begin,
                         const std::size_t end) {
    double sum = 0.0;
    for (std::size_t index = begin; index < end; ++index)
        sum += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
    return std::sqrt(sum / static_cast<double>(end - begin));
}

[[nodiscard]] double db(const double linear) { return 20.0 * std::log10(std::max(linear, 1e-12)); }

[[nodiscard]] EffectSettings equalizer(const std::vector<double>& values) {
    return makeEffectSettings(EffectKind::Equalizer, values);
}

// highPass, lowPass, lowGain, lowFrequency, midGain, midFrequency, midQ, highGain, highFrequency
[[nodiscard]] std::vector<double> flatEqualizer() {
    return {20.0, 20000.0, 0.0, 120.0, 0.0, 1000.0, 1.0, 0.0, 6000.0};
}

void testRoleListsMatchTheSettingsWidth(Expectations& e) {
    using bloom::media::audio::effects::effectRoles;
    e.expect(effectRoles(EffectKind::Equalizer).size() == 9, "the equalizer has nine parameters");
    e.expect(effectRoles(EffectKind::Compressor).size() == 6, "the compressor has six");
    e.expect(effectRoles(EffectKind::AutoLevel).size() == 4, "the auto level has four");
    e.expect(effectRoles(EffectKind::Mono).size() == 1, "the mono fold has one");
    e.expect(effectRoles(EffectKind::Hiss).size() == 3, "the hiss has three");
}

void testFlatEqualizerIsExact(Expectations& e) {
    const auto source = sine(440.0, 0.5, 4'800);
    const auto played = render(mono(source), {equalizer(flatEqualizer())}, 4'800);
    bool exact = played.size() == source.size();
    for (std::size_t index = 0; exact && index < source.size(); ++index)
        exact = played[index] == source[index];
    e.expect(exact, "an equalizer at its defaults passes the audio unchanged, bit for bit");
}

void testEqualizerFilters(Expectations& e) {
    constexpr std::size_t frames = 24'000;
    // High-pass at 1 kHz: a 100 Hz tone falls well below, a 4 kHz tone passes.
    auto values = flatEqualizer();
    values[0] = 1000.0;
    const auto low = render(mono(sine(100.0, 0.5, frames)), {equalizer(values)}, frames);
    const auto high = render(mono(sine(4000.0, 0.5, frames)), {equalizer(values)}, frames);
    e.expect(db(rms(low, 12'000, frames)) - db(0.5 / std::sqrt(2.0)) < -30.0,
             "a 1 kHz high-pass takes at least 30 dB off a 100 Hz tone (24 dB per octave)");
    e.expect(std::abs(db(rms(high, 12'000, frames)) - db(0.5 / std::sqrt(2.0))) < 0.5,
             "a 1 kHz high-pass passes 4 kHz within half a decibel");
    // Low-pass at 1 kHz: the opposite.
    values = flatEqualizer();
    values[1] = 1000.0;
    const auto passed = render(mono(sine(200.0, 0.5, frames)), {equalizer(values)}, frames);
    const auto cut = render(mono(sine(8000.0, 0.5, frames)), {equalizer(values)}, frames);
    e.expect(std::abs(db(rms(passed, 12'000, frames)) - db(0.5 / std::sqrt(2.0))) < 0.5,
             "a 1 kHz low-pass passes 200 Hz within half a decibel");
    e.expect(db(rms(cut, 12'000, frames)) - db(0.5 / std::sqrt(2.0)) < -40.0,
             "a 1 kHz low-pass takes at least 40 dB off 8 kHz");
    // A +12 dB peak at 1 kHz.
    values = flatEqualizer();
    values[4] = 12.0;
    const auto boosted = render(mono(sine(1000.0, 0.1, frames)), {equalizer(values)}, frames);
    e.expect(std::abs(db(rms(boosted, 12'000, frames)) - db(0.1 / std::sqrt(2.0)) - 12.0) < 0.5,
             "a +12 dB peak at 1 kHz lifts a 1 kHz tone by 12 dB");
    // A +9 dB high shelf at 4 kHz lifts 10 kHz by about 9 dB and leaves 200 Hz alone.
    values = flatEqualizer();
    values[7] = 9.0;
    values[8] = 4000.0;
    const auto shelf = render(mono(sine(12000.0, 0.1, frames)), {equalizer(values)}, frames);
    const auto under = render(mono(sine(200.0, 0.1, frames)), {equalizer(values)}, frames);
    e.expect(std::abs(db(rms(shelf, 12'000, frames)) - db(0.1 / std::sqrt(2.0)) - 9.0) < 1.0,
             "a +9 dB high shelf lifts the top by about 9 dB");
    e.expect(std::abs(db(rms(under, 12'000, frames)) - db(0.1 / std::sqrt(2.0))) < 0.5,
             "a high shelf leaves the lows alone");
}

void testCompressorReachesItsTransferCurve(Expectations& e) {
    constexpr std::size_t frames = 48'000;
    // threshold -24 dB, ratio 4:1, 5 ms attack, 50 ms release, hard knee, no makeup.
    const auto settings = makeEffectSettings(EffectKind::Compressor,
                                             std::vector<double>{-24.0, 4.0, 5.0, 50.0, 0.0, 0.0});
    const auto source = sine(1000.0, std::pow(10.0, -6.0 / 20.0), frames);
    const auto played = render(mono(source), {settings}, frames);
    // The peak of the output settles at -24 + (-6 - -24) / 4 = -19.5 dBFS.
    float peak = 0.0F;
    for (std::size_t index = 36'000; index < frames; ++index)
        peak = std::max(peak, std::abs(played[index]));
    e.expect(std::abs(db(static_cast<double>(peak)) + 19.5) < 0.6,
             "a 4:1 compressor settles at the level its transfer curve gives");
    // A quiet tone under the threshold is left alone.
    const auto quiet = render(mono(sine(1000.0, std::pow(10.0, -40.0 / 20.0), frames)), {settings},
                              frames);
    e.expect(std::abs(db(rms(quiet, 24'000, frames)) -
                      db(std::pow(10.0, -40.0 / 20.0) / std::sqrt(2.0))) < 0.05,
             "audio under the threshold passes unchanged");
}

void testAutoLevelFollowsItsTarget(Expectations& e) {
    constexpr std::size_t frames = 48'000 * 12;
    // target -20 dB, up to 24 dB of gain, 1 s response, gate -55 dB.
    const auto settings = makeEffectSettings(EffectKind::AutoLevel,
                                             std::vector<double>{-20.0, 24.0, 1.0, -55.0});
    const auto source = sine(500.0, std::pow(10.0, -40.0 / 20.0) * std::sqrt(2.0), frames);
    const auto played = render(mono(source), {settings}, frames);
    const double levelDb = db(rms(played, frames - 48'000, frames));
    e.expect(std::abs(levelDb + 20.0) < 1.0, "the auto level brings a quiet voice to its target");
    // Below the gate the gain holds at unity instead of lifting the room tone.
    const auto floor = render(mono(sine(500.0, std::pow(10.0, -70.0 / 20.0), 96'000)), {settings},
                              96'000);
    e.expect(std::abs(db(rms(floor, 48'000, 96'000)) -
                      db(std::pow(10.0, -70.0 / 20.0) / std::sqrt(2.0))) < 0.1,
             "audio under the gate is not lifted");
}

void testMonoFold(Expectations& e) {
    const std::vector<float> left(256, 1.0F);
    const std::vector<float> right(256, 0.0F);
    const auto folded = render(stereo(left, right),
                               {makeEffectSettings(EffectKind::Mono, std::vector<double>{1.0})},
                               256, 2);
    bool ok = folded.size() == 512;
    for (std::size_t index = 0; ok && index < 256; ++index)
        ok = folded[index * 2] == 0.5F && folded[index * 2 + 1] == 0.5F;
    e.expect(ok, "a full fold puts the average of the channels on every channel");
    const auto half = render(stereo(left, right),
                             {makeEffectSettings(EffectKind::Mono, std::vector<double>{0.5})}, 256,
                             2);
    e.expect(half.size() == 512 && half[0] == 0.75F && half[1] == 0.25F,
             "half a fold moves each channel halfway to the average");
    const auto none = render(stereo(left, right),
                             {makeEffectSettings(EffectKind::Mono, std::vector<double>{0.0})}, 256,
                             2);
    e.expect(none.size() == 512 && none[0] == 1.0F && none[1] == 0.0F,
             "no fold leaves the channels apart");
}

void testHissIsDeterministicAndAtItsLevel(Expectations& e) {
    constexpr std::size_t frames = 96'000;
    const std::vector<float> silence(frames, 0.0F);
    // level -40 dB RMS, no tone filter, seed 3.
    const auto settings =
        makeEffectSettings(EffectKind::Hiss, std::vector<double>{-40.0, 20.0, 3.0});
    const auto whole = render(mono(silence), {settings}, frames);
    e.expect(std::abs(db(rms(whole, 0, frames)) + 40.0) < 0.5, "the hiss is at its level in RMS");
    // Cut into different pulls, and started later: the noise at a sample depends on its position
    // only.
    const auto pieces = render(mono(silence), {settings}, frames, 1, 0, 301);
    e.expect(whole == pieces, "the hiss does not depend on how the mix was cut into blocks");
    const auto later = render(mono(silence), {settings}, 4'000, 1, 50'000);
    bool same = later.size() == 4'000;
    for (std::size_t index = 0; same && index < 4'000; ++index)
        same = later[index] == whole[50'000 + index];
    e.expect(same, "the hiss from a later start equals the same samples of a full run");
    const auto other = render(mono(silence),
                              {makeEffectSettings(EffectKind::Hiss,
                                                  std::vector<double>{-40.0, 20.0, 4.0})},
                              1'000);
    e.expect(other != std::vector<float>(whole.begin(), whole.begin() + 1'000),
             "another seed is another noise");
    // The tone filter takes the lows out.
    const auto toned = render(mono(silence),
                              {makeEffectSettings(EffectKind::Hiss,
                                                  std::vector<double>{-40.0, 8000.0, 3.0})},
                              frames);
    // White noise high-passed at a third of the band keeps two thirds of its power.
    const double kept = rms(toned, 24'000, frames) / rms(whole, 24'000, frames);
    e.expect(kept > 0.7 && kept < 0.9, "a tone of 8 kHz takes the lows out of the noise");
}

void testBlockSplitsDoNotChangeTheResult(Expectations& e) {
    constexpr std::size_t frames = 24'000;
    auto values = flatEqualizer();
    values[0] = 300.0;
    values[1] = 5000.0;
    values[4] = 6.0;
    const auto compressor = makeEffectSettings(EffectKind::Compressor,
                                               std::vector<double>{-20.0, 3.0, 5.0, 80.0, 6.0, 2.0});
    const auto source = sine(700.0, 0.8, frames);
    const std::vector<EffectSettings> chain{equalizer(values), compressor};
    const auto whole = render(mono(source), chain, frames);
    const auto pieces = render(mono(source), chain, frames, 1, 0, 97);
    e.expect(whole == pieces, "a chain gives the same samples however the mix is cut into pulls");
}

void testSeekingWarmsTheDynamics(Expectations& e) {
    constexpr std::size_t frames = 48'000 * 3;
    const auto compressor = makeEffectSettings(EffectKind::Compressor,
                                               std::vector<double>{-24.0, 6.0, 5.0, 120.0, 0.0, 0.0});
    // Loud bursts and quiet gaps: the gain is moving at the seek point.
    std::vector<float> source(frames);
    for (std::size_t index = 0; index < frames; ++index) {
        const bool loud = (index / 12'000) % 2 == 0;
        source[index] = static_cast<float>((loud ? 0.9 : 0.05) *
                                           std::sin(2.0 * kPi * 440.0 * static_cast<double>(index) /
                                                    kRate));
    }
    const auto whole = render(mono(source), {compressor}, frames);
    // A run that begins 1.2 s in, mid-gap after a burst, hears the same compression.
    constexpr std::int64_t from = 48'000 + 9'600;
    const auto sought = render(mono(source), {compressor}, 6'000, 1, from);
    double worst = 0.0;
    for (std::size_t index = 0; index < sought.size(); ++index)
        worst = std::max(worst, static_cast<double>(std::abs(
                                    sought[index] - whole[static_cast<std::size_t>(from) + index])));
    e.expect(sought.size() == 6'000 && worst < 0.003,
             "playback started mid-clip compresses like a run from the start");
}

void testParametersMoveSmoothly(Expectations& e) {
    constexpr std::size_t frames = 48'000;
    // A mid peak that rises from 0 to +12 dB over the second.
    auto start = flatEqualizer();
    auto end = flatEqualizer();
    end[4] = 12.0;
    const auto settings = makeEffectSettings(EffectKind::Equalizer, start, end);
    const auto played = render(mono(sine(1000.0, 0.1, frames)), {settings}, frames, 1, 0, 0,
                               AudioEffectRamp{time(0), time(1)});
    // The envelope per 10 ms rises steadily: no step.
    double previous = 0.0;
    double biggestStep = 0.0;
    bool rising = true;
    for (std::size_t window = 0; window + 480 <= frames; window += 480) {
        const double level = rms(played, window, window + 480);
        if (window > 0) {
            rising = rising && level >= previous * 0.999;
            biggestStep = std::max(biggestStep, db(level) - db(previous));
        }
        previous = level;
    }
    e.expect(rising, "an animated gain rises without falling back");
    e.expect(biggestStep < 0.4, "an animated gain moves in small steps, with no jump");
    e.expect(std::abs(db(rms(played, 47'000, frames)) - db(0.1 / std::sqrt(2.0)) - 12.0) < 0.6,
             "the animated gain reaches its end value");
}

void testChainKeepsItsStateAcrossReplacements(Expectations& e) {
    // The export replaces its clips every block: the filter memory must survive that.
    constexpr std::size_t frames = 4'800;
    auto values = flatEqualizer();
    values[0] = 500.0;
    const auto source = sine(300.0, 0.5, frames);
    const auto whole = render(mono(source), {equalizer(values)}, frames);

    auto backend = std::make_unique<NullBackend>();
    auto* captured = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{kRate, 1, 16'384});
    static_cast<void>(engine.play(time(0)));
    for (std::size_t done = 0; done < frames; done += 512) {
        AudioClip clip{mono(source), time(0), 1.0F, false, false};
        clip.effectKey = 9;
        clip.effects = {equalizer(values)};
        std::vector<AudioClip> clips;
        clips.push_back(std::move(clip));
        engine.replaceClips(std::move(clips));
        captured->clearCaptured();
        static_cast<void>(engine.renderForTesting(std::min<std::size_t>(512, frames - done)));
        for (std::size_t index = 0; index < captured->captured().size(); ++index)
            e.expect(captured->captured()[index] == whole[done + index],
                     "replacing the clips each block keeps the filter state");
    }
}

} // namespace

int main() {
    Expectations expectations;
    testRoleListsMatchTheSettingsWidth(expectations);
    testFlatEqualizerIsExact(expectations);
    testEqualizerFilters(expectations);
    testCompressorReachesItsTransferCurve(expectations);
    testAutoLevelFollowsItsTarget(expectations);
    testMonoFold(expectations);
    testHissIsDeterministicAndAtItsLevel(expectations);
    testBlockSplitsDoNotChangeTheResult(expectations);
    testSeekingWarmsTheDynamics(expectations);
    testParametersMoveSmoothly(expectations);
    testChainKeepsItsStateAcrossReplacements(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
