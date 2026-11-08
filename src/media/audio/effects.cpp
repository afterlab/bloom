#include <bloom/media/audio/effects.hpp>

#include "miniaudio_filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <variant>

namespace bloom::media::audio::effects {
namespace {

using detail::FilterType;
using detail::MiniaudioFilter;

// The filters re-derive their coefficients this often, on the absolute sample grid, while a
// parameter moves. A fixed grid keeps the result independent of how the mixer cut its blocks.
constexpr std::int64_t kCoefficientInterval = 32;

constexpr double kOff = 1e-9;

// The widest layout a chain processes (the hiss builds one frame of noise at a time).
constexpr std::uint32_t kMaxChannels = 64;

[[nodiscard]] double lerp(const double a, const double b, const double t) noexcept {
    return a + (b - a) * t;
}

[[nodiscard]] double dbToGain(const double db) noexcept { return std::pow(10.0, db / 20.0); }

// A coefficient memory for each channel of a filter that tracks its last-set parameters.
struct Band final {
    std::unique_ptr<MiniaudioFilter> filter;
    double frequency = -1.0;
    double gain = 0.0;
    double q = 0.0;
    bool active = false;
};

struct EqualizerState final {
    Band highPass;
    Band lowPass;
    Band low;
    Band mid;
    Band high;
};

struct CompressorState final {
    // The level detector's peak envelope (linear): it jumps to a louder sample and decays with the
    // release time, so it follows the signal's peaks rather than every cycle of it.
    double envelope = 0.0;
    // The smoothed gain reduction in dB (at most 0).
    double reductionDb = 0.0;
};

struct AutoLevelState final {
    double power = 0.0;
    double gainDb = 0.0;
    bool primed = false;
};

struct MonoState final {};
struct HissState final {
    Band tone;
};

using EffectState =
    std::variant<EqualizerState, CompressorState, AutoLevelState, MonoState, HissState>;

[[nodiscard]] EffectState makeState(const EffectKind kind, const std::uint32_t rate,
                                    const std::uint32_t channels) {
    switch (kind) {
    case EffectKind::Equalizer: {
        EqualizerState state;
        state.highPass.filter = std::make_unique<MiniaudioFilter>(FilterType::HighPass, channels, rate);
        state.lowPass.filter = std::make_unique<MiniaudioFilter>(FilterType::LowPass, channels, rate);
        state.low.filter = std::make_unique<MiniaudioFilter>(FilterType::LowShelf, channels, rate);
        state.mid.filter = std::make_unique<MiniaudioFilter>(FilterType::Peak, channels, rate);
        state.high.filter = std::make_unique<MiniaudioFilter>(FilterType::HighShelf, channels, rate);
        return state;
    }
    case EffectKind::Compressor:
        return CompressorState{};
    case EffectKind::AutoLevel:
        return AutoLevelState{};
    case EffectKind::Mono:
        return MonoState{};
    case EffectKind::Hiss: {
        HissState state;
        state.tone.filter = std::make_unique<MiniaudioFilter>(FilterType::HighPass, channels, rate);
        return state;
    }
    }
    return MonoState{};
}

// Filters one band at the current parameters: re-derives the coefficients when they moved (on the
// coefficient grid), and passes the frame through when the band is off.
void runBand(Band& band, const bool enabled, const double frequency, const double gain,
             const double q, const std::int64_t absoluteFrame, float* frame) noexcept {
    if (!enabled) {
        if (band.active) {
            // Leaving a band clears its memory, so re-entering it starts clean.
            band.filter->clear();
            band.active = false;
        }
        return;
    }
    const bool moved = band.frequency != frequency || band.gain != gain || band.q != q;
    if (!band.active || (moved && (absoluteFrame % kCoefficientInterval == 0))) {
        if (!band.active)
            band.filter->clear();
        if (band.filter->configure(frequency, gain, q)) {
            band.frequency = frequency;
            band.gain = gain;
            band.q = q;
        }
        band.active = true;
    }
    band.filter->process(frame);
}

void resetBand(Band& band) noexcept {
    if (band.filter)
        band.filter->clear();
    band.active = false;
    band.frequency = -1.0;
}

[[nodiscard]] double value(const EffectSettings& s, const std::size_t index, const double t) noexcept {
    return lerp(static_cast<double>(s.values[index]), static_cast<double>(s.endValues[index]), t);
}

void equalize(EqualizerState& state, const EffectSettings& s, const double t, const double nyquist,
              const std::int64_t absoluteFrame, float* frame) noexcept {
    const auto limit = [nyquist](const double hz) { return std::min(hz, nyquist * 0.45); };
    const double highPass = value(s, 0, t);
    const double lowPass = value(s, 1, t);
    // Out at the ends of the range: a high-pass at 20 Hz and a low-pass at 20 kHz are off.
    runBand(state.highPass, highPass > 20.5, limit(highPass), 0.0, 0.0, absoluteFrame, frame);
    runBand(state.low, std::abs(value(s, 2, t)) > kOff, limit(value(s, 3, t)), value(s, 2, t), 0.0,
            absoluteFrame, frame);
    runBand(state.mid, std::abs(value(s, 4, t)) > kOff, limit(value(s, 5, t)), value(s, 4, t),
            value(s, 6, t), absoluteFrame, frame);
    runBand(state.high, std::abs(value(s, 7, t)) > kOff, limit(value(s, 8, t)), value(s, 7, t), 0.0,
            absoluteFrame, frame);
    runBand(state.lowPass, lowPass < 19999.5, limit(lowPass), 0.0, 0.0, absoluteFrame, frame);
}

// A feed-forward compressor in the log domain with a soft knee (Giannoulis, Massberg and Reiss,
// "Digital Dynamic Range Compressor Design", JAES 2012). The detector is the loudest channel, so a
// stereo image is not pulled to one side. The peak detector holds the level and lets it decay with
// the release time; the gain reduction read from the static curve then falls with the attack time
// and recovers as the detector decays.
void compress(CompressorState& state, const EffectSettings& s, const double t, const double rate,
              const std::uint32_t channels, float* frame) noexcept {
    const double threshold = value(s, 0, t);
    const double ratio = std::max(1.0, value(s, 1, t));
    const double attack = std::max(0.01, value(s, 2, t)) * 0.001;
    const double release = std::max(0.1, value(s, 3, t)) * 0.001;
    const double knee = std::max(0.0, value(s, 4, t));
    const double makeup = value(s, 5, t);
    double peak = 0.0;
    for (std::uint32_t c = 0; c < channels; ++c)
        peak = std::max(peak, std::abs(static_cast<double>(frame[c])));
    state.envelope = std::max(peak, state.envelope * std::exp(-1.0 / (release * rate)));
    const double over = 20.0 * std::log10(std::max(state.envelope, 1e-9)) - threshold;
    double target = 0.0; // dB, at most 0
    if (2.0 * over < -knee) {
        target = 0.0;
    } else if (knee > 0.0 && 2.0 * std::abs(over) <= knee) {
        const double x = over + knee * 0.5;
        target = (1.0 / ratio - 1.0) * x * x / (2.0 * knee);
    } else {
        target = (1.0 / ratio - 1.0) * over;
    }
    if (target < state.reductionDb) {
        const double fall = std::exp(-1.0 / (attack * rate));
        state.reductionDb = fall * state.reductionDb + (1.0 - fall) * target;
    } else {
        state.reductionDb = target;
    }
    const float gain = static_cast<float>(dbToGain(state.reductionDb + makeup));
    for (std::uint32_t c = 0; c < channels; ++c)
        frame[c] *= gain;
}

// A slow automatic gain: it brings the running level toward a target, up to a limit either way,
// and holds still while the signal is below the noise gate so the room tone is not pumped up.
void autoLevel(AutoLevelState& state, const EffectSettings& s, const double t, const double rate,
               const std::uint32_t channels, float* frame) noexcept {
    const double target = value(s, 0, t);
    const double maxGain = std::max(0.0, value(s, 1, t));
    const double response = std::max(0.01, value(s, 2, t));
    const double gate = value(s, 3, t);
    double power = 0.0;
    for (std::uint32_t c = 0; c < channels; ++c)
        power += static_cast<double>(frame[c]) * static_cast<double>(frame[c]);
    power /= static_cast<double>(channels);
    // The level the gain tracks is the signal's level, measured over a fixed window.
    constexpr double kWindowSeconds = 0.3;
    const double detector = std::exp(-1.0 / (kWindowSeconds * rate));
    if (!state.primed) {
        state.power = power;
        state.primed = true;
    }
    state.power = detector * state.power + (1.0 - detector) * power;
    const double levelDb = 10.0 * std::log10(std::max(state.power, 1e-18));
    if (levelDb >= gate) {
        const double wanted = std::clamp(target - levelDb, -maxGain, maxGain);
        // A fall is quicker than a rise, so a loud passage is not left over-amplified.
        const double time = wanted < state.gainDb ? response * 0.25 : response;
        const double coefficient = std::exp(-1.0 / (time * rate));
        state.gainDb = coefficient * state.gainDb + (1.0 - coefficient) * wanted;
    }
    const float gain = static_cast<float>(dbToGain(std::clamp(state.gainDb, -maxGain, maxGain)));
    for (std::uint32_t c = 0; c < channels; ++c)
        frame[c] *= gain;
}

void foldMono(const EffectSettings& s, const double t, const std::uint32_t channels,
              float* frame) noexcept {
    if (channels < 2)
        return;
    const double amount = std::clamp(value(s, 0, t), 0.0, 1.0);
    if (amount <= 0.0)
        return;
    double sum = 0.0;
    for (std::uint32_t c = 0; c < channels; ++c)
        sum += static_cast<double>(frame[c]);
    const double mean = sum / static_cast<double>(channels);
    for (std::uint32_t c = 0; c < channels; ++c)
        frame[c] = static_cast<float>(lerp(static_cast<double>(frame[c]), mean, amount));
}

// A counter-based generator (splitmix64 over the seed, channel and absolute frame): the noise at a
// sample is a function of its position only, so playback from anywhere and an export of any range
// hear the same hiss.
[[nodiscard]] float whiteNoise(const std::uint64_t seed, const std::uint32_t channel,
                               const std::int64_t frame) noexcept {
    std::uint64_t z = static_cast<std::uint64_t>(frame) * 0x9E3779B97F4A7C15ULL +
                      ((seed << 8U) | channel) * 0xD1B54A32D192ED03ULL + 0x94D049BB133111EBULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    // 24 bits mapped to [-1, 1).
    return static_cast<float>(static_cast<double>(z >> 40U) * (2.0 / 16777216.0) - 1.0);
}

void addHiss(HissState& state, const EffectSettings& s, const double t, const std::uint32_t channels,
             const std::int64_t absoluteFrame, float* frame) noexcept {
    const double level = dbToGain(value(s, 0, t));
    const double tone = value(s, 1, t);
    const auto seed = static_cast<std::uint64_t>(std::max(0.0, value(s, 2, t)));
    // The generator is uniform with an RMS of 1/sqrt(3); scale it so `level` is the RMS.
    const double scale = level * 1.7320508075688772;
    std::array<float, kMaxChannels> noise{};
    const std::uint32_t used = std::min<std::uint32_t>(channels, kMaxChannels);
    for (std::uint32_t c = 0; c < used; ++c)
        noise[c] = static_cast<float>(static_cast<double>(whiteNoise(seed, c, absoluteFrame)) * scale);
    // Tone: the hiss is high-passed at `tone`; at the lowest setting it is plain white noise.
    runBand(state.tone, tone > 20.5, tone, 0.0, 0.0, absoluteFrame, noise.data());
    for (std::uint32_t c = 0; c < used; ++c)
        frame[c] += noise[c];
}

} // namespace

EffectSettings makeEffectSettings(const EffectKind kind, const std::span<const double> values,
                                  const std::span<const double> endValues) noexcept {
    EffectSettings settings;
    settings.kind = kind;
    for (std::size_t index = 0; index < kMaxEffectValues && index < values.size(); ++index) {
        settings.values[index] = static_cast<float>(values[index]);
        settings.endValues[index] = index < endValues.size()
                                        ? static_cast<float>(endValues[index])
                                        : settings.values[index];
    }
    return settings;
}

struct EffectChain::State final {
    std::vector<EffectKind> kinds;
    std::vector<EffectState> states;
};

EffectChain::EffectChain(const std::uint32_t rate, const std::uint32_t channels)
    : rate_(rate), channels_(channels), state_(std::make_unique<State>()) {}

EffectChain::~EffectChain() = default;

bool EffectChain::matches(const std::span<const EffectSettings> settings) const noexcept {
    if (settings.size() != state_->kinds.size())
        return false;
    for (std::size_t index = 0; index < settings.size(); ++index)
        if (settings[index].kind != state_->kinds[index])
            return false;
    return true;
}

void EffectChain::configure(const std::span<const EffectSettings> settings) {
    if (matches(settings))
        return;
    state_->kinds.clear();
    state_->states.clear();
    for (const auto& s : settings) {
        state_->kinds.push_back(s.kind);
        state_->states.push_back(makeState(s.kind, rate_, channels_));
    }
    last_ = kNone;
}

bool EffectChain::continuous(const std::int64_t frame) const noexcept {
    return last_ != kNone && frame > last_ && frame - last_ <= 8;
}

void EffectChain::reset() noexcept {
    for (auto& state : state_->states) {
        if (auto* eq = std::get_if<EqualizerState>(&state)) {
            resetBand(eq->highPass);
            resetBand(eq->lowPass);
            resetBand(eq->low);
            resetBand(eq->mid);
            resetBand(eq->high);
        } else if (auto* comp = std::get_if<CompressorState>(&state)) {
            *comp = {};
        } else if (auto* level = std::get_if<AutoLevelState>(&state)) {
            *level = {};
        } else if (auto* hiss = std::get_if<HissState>(&state)) {
            resetBand(hiss->tone);
        }
    }
    last_ = kNone;
}

void EffectChain::process(float* frame, const std::int64_t absoluteFrame,
                          const std::span<const EffectSettings> settings,
                          const double progress) noexcept {
    const double t = std::clamp(progress, 0.0, 1.0);
    const double rate = static_cast<double>(rate_);
    const std::size_t count = std::min(settings.size(), state_->states.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& s = settings[index];
        auto& state = state_->states[index];
        switch (s.kind) {
        case EffectKind::Equalizer:
            if (auto* eq = std::get_if<EqualizerState>(&state))
                equalize(*eq, s, t, rate * 0.5, absoluteFrame, frame);
            break;
        case EffectKind::Compressor:
            if (auto* comp = std::get_if<CompressorState>(&state))
                compress(*comp, s, t, rate, channels_, frame);
            break;
        case EffectKind::AutoLevel:
            if (auto* level = std::get_if<AutoLevelState>(&state))
                autoLevel(*level, s, t, rate, channels_, frame);
            break;
        case EffectKind::Mono:
            foldMono(s, t, channels_, frame);
            break;
        case EffectKind::Hiss:
            if (auto* hiss = std::get_if<HissState>(&state))
                addHiss(*hiss, s, t, channels_, absoluteFrame, frame);
            break;
        }
    }
    last_ = absoluteFrame;
}

} // namespace bloom::media::audio::effects
