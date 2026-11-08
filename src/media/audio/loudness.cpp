#include <bloom/media/audio/loudness.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>
#include <numbers>

extern "C" {
#include "ebur128.h"
}

namespace bloom::media::audio::loudness {
namespace {

constexpr std::uint32_t kMinimumRate = 8000;
constexpr std::uint32_t kMaximumRate = 192000;
constexpr std::uint32_t kMaximumChannels = 8;

[[nodiscard]] double toDb(const double linear) noexcept {
    return linear > 0.0 ? 20.0 * std::log10(linear) : -std::numeric_limits<double>::infinity();
}
[[nodiscard]] double fromDb(const double db) noexcept { return std::pow(10.0, db / 20.0); }

} // namespace

struct Meter::State final {
    ebur128_state* library = nullptr;
    ~State() {
        if (library != nullptr)
            ebur128_destroy(&library);
    }
};

Meter::Meter(const std::uint32_t sampleRate, const std::uint32_t channels)
    : state_(std::make_unique<State>()), channels_(channels) {
    if (sampleRate < kMinimumRate || sampleRate > kMaximumRate || channels == 0 ||
        channels > kMaximumChannels)
        return;
    state_->library = ebur128_init(channels, sampleRate, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
}

Meter::~Meter() = default;

bool Meter::valid() const noexcept { return state_->library != nullptr; }

bool Meter::add(const std::span<const float> samples) noexcept {
    if (state_->library == nullptr || samples.size() % channels_ != 0)
        return false;
    const auto frames = samples.size() / channels_;
    if (frames == 0)
        return true;
    if (ebur128_add_frames_float(state_->library, samples.data(), frames) != EBUR128_SUCCESS)
        return false;
    frames_ += frames;
    return true;
}

Measurement Meter::measurement() const noexcept {
    Measurement result;
    result.frames = frames_;
    if (state_->library == nullptr)
        return result;
    double loudness = 0.0;
    if (ebur128_loudness_global(state_->library, &loudness) == EBUR128_SUCCESS &&
        std::isfinite(loudness)) {
        result.loudnessMeasured = true;
        result.integratedLufs = loudness;
    }
    double peak = 0.0;
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
        double one = 0.0;
        if (ebur128_true_peak(state_->library, channel, &one) == EBUR128_SUCCESS)
            peak = std::max(peak, one);
    }
    result.truePeakDbtp = toDb(peak);
    return result;
}

double gainToTarget(const Measurement& measured, const Target& target) noexcept {
    return measured.loudnessMeasured ? target.integratedLufs - measured.integratedLufs : 0.0;
}

namespace {

// The limiter's interpolator mirrors the reference library's true-peak estimate (49 taps,
// Hann-windowed sinc, four times oversampling below 96 kHz and two times below 192 kHz), so the
// peak it keeps under the ceiling is the one the meter will read.
constexpr std::size_t kTaps = 49;
// The look-ahead window in seconds, and the release time of the gain.
constexpr double kLookaheadSeconds = 0.001;
constexpr double kReleaseSeconds = 0.08;
// Headroom under the ceiling, in dB, for the difference between the estimates.
constexpr double kHeadroomDb = 0.1;

struct Phase final {
    std::vector<double> coefficients;
    std::vector<std::size_t> ages;
};

} // namespace

struct Limiter::State final {
    std::uint32_t channels = 0;
    double gain = 1.0;
    double ceiling = 1.0;
    // Interpolation.
    std::size_t factor = 1;
    std::vector<Phase> phases;
    std::size_t historySize = 1;
    std::vector<std::vector<double>> history; // per channel, newest first
    // Look-ahead.
    std::size_t window = 1;       // W
    std::size_t detectorDelay = 0; // input frames between a sample and its estimate
    std::size_t latency = 0;
    double releaseCoefficient = 0.0;
    std::deque<std::pair<std::uint64_t, double>> minimum; // monotonic queue of (index, g_req)
    std::deque<double> hatWindow;                          // the last W values of g_hat
    double hatSum = 0.0;
    double previousGain = 1.0;
    double lowestGain = 1.0;
    std::vector<float> delayLine; // ring of the last `latency` input frames, gain applied
    std::size_t delayHead = 0;    // the oldest frame
    std::uint64_t estimates = 0;               // detector steps done
    std::uint64_t pushed = 0;

    // Takes one input frame (gain applied); returns whether an output frame is ready and writes it.
    bool step(const float* frame, float* output) {
        // The frame leaving (when the line is full) is the oldest; the new one takes its place.
        float* const slot = delayLine.data() + delayHead * channels;
        std::vector<float> leaving(slot, slot + channels);
        std::copy(frame, frame + channels, slot);
        delayHead = (delayHead + 1) % latency;
        // The estimate of the interpolated peak around frame (pushed - detectorDelay).
        double peak = 0.0;
        for (std::uint32_t c = 0; c < channels; ++c) {
            auto& h = history[c];
            std::rotate(h.rbegin(), h.rbegin() + 1, h.rend());
            h[0] = static_cast<double>(frame[c]);
            for (const auto& phase : phases) {
                double acc = 0.0;
                for (std::size_t t = 0; t < phase.ages.size(); ++t)
                    acc += h[phase.ages[t]] * phase.coefficients[t];
                peak = std::max(peak, std::abs(acc));
            }
            if (phases.empty())
                peak = std::max(peak, std::abs(h[0]));
        }
        const std::uint64_t k = pushed; // the index of this estimate
        const double required = peak > ceiling ? ceiling / peak : 1.0;
        // Sliding minimum of g_req over the last W estimates: g_hat for the window ending at k.
        while (!minimum.empty() && minimum.back().second >= required)
            minimum.pop_back();
        minimum.emplace_back(k, required);
        while (minimum.front().first + window <= k)
            minimum.pop_front();
        const double hat = minimum.front().second;
        hatWindow.push_back(hat);
        hatSum += hat;
        if (hatWindow.size() > window) {
            hatSum -= hatWindow.front();
            hatWindow.pop_front();
        }
        ++pushed;
        if (pushed <= latency)
            return false;
        // Frame (pushed - 1 - latency) leaves with its smoothed, release-limited gain.
        double smoothed = hatSum / static_cast<double>(window);
        const double released = previousGain + (1.0 - previousGain) * (1.0 - releaseCoefficient);
        previousGain = std::min(smoothed, released);
        lowestGain = std::min(lowestGain, previousGain);
        for (std::uint32_t c = 0; c < channels; ++c)
            output[c] = static_cast<float>(static_cast<double>(leaving[c]) * previousGain);
        return true;
    }
};

Limiter::Limiter(const std::uint32_t sampleRate, const std::uint32_t channels,
                 const double gainDb, const double ceilingDbtp)
    : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.channels = std::max<std::uint32_t>(channels, 1);
    s.gain = fromDb(gainDb);
    s.ceiling = fromDb(ceilingDbtp - kHeadroomDb);
    s.factor = sampleRate < 96000 ? 4 : sampleRate < 192000 ? 2 : 1;
    if (s.factor > 1) {
        s.phases.resize(s.factor);
        for (std::size_t j = 0; j < kTaps; ++j) {
            const double m = static_cast<double>(j) - static_cast<double>(kTaps - 1) / 2.0;
            double c = 1.0;
            if (std::abs(m) > 1e-6) {
                const double x = m * std::numbers::pi / static_cast<double>(s.factor);
                c = std::sin(x) / x;
            }
            c *= 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * static_cast<double>(j) /
                                       static_cast<double>(kTaps - 1)));
            if (std::abs(c) > 1e-6) {
                auto& phase = s.phases[j % s.factor];
                phase.coefficients.push_back(c);
                phase.ages.push_back(j / s.factor);
            }
        }
        s.historySize = (kTaps + s.factor - 1) / s.factor;
        s.detectorDelay = (kTaps - 1) / 2 / s.factor;
    }
    s.history.assign(s.channels, std::vector<double>(s.historySize, 0.0));
    s.window = std::max<std::size_t>(
        2, static_cast<std::size_t>(kLookaheadSeconds * static_cast<double>(sampleRate)));
    // Frame m leaves once the estimate for frame m + W - 1 exists: W - 1 frames of look-ahead
    // beyond the interpolator's own delay.
    s.latency = s.detectorDelay + s.window - 1;
    s.delayLine.assign(s.latency * s.channels, 0.0F);
    s.releaseCoefficient = std::exp(-1.0 / (kReleaseSeconds * static_cast<double>(sampleRate)));
}

Limiter::~Limiter() = default;

std::size_t Limiter::latencyFrames() const noexcept { return state_->latency; }

double Limiter::maximumReductionDb() const noexcept {
    return -toDb(state_->lowestGain);
}

void Limiter::process(const std::span<const float> samples, std::vector<float>& out) {
    auto& s = *state_;
    const auto frames = samples.size() / s.channels;
    std::vector<float> scaled(s.channels);
    std::vector<float> frame(s.channels);
    for (std::size_t index = 0; index < frames; ++index) {
        for (std::uint32_t c = 0; c < s.channels; ++c)
            scaled[c] = static_cast<float>(static_cast<double>(samples[index * s.channels + c]) *
                                           s.gain);
        if (s.step(scaled.data(), frame.data()))
            out.insert(out.end(), frame.begin(), frame.end());
    }
}

void Limiter::flush(std::vector<float>& out) {
    auto& s = *state_;
    const std::vector<float> silence(s.channels, 0.0F);
    std::vector<float> frame(s.channels);
    for (std::size_t index = 0; index < s.latency; ++index)
        if (s.step(silence.data(), frame.data()))
            out.insert(out.end(), frame.begin(), frame.end());
}

} // namespace bloom::media::audio::loudness
