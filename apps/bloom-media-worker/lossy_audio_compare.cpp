#include "lossy_audio_compare.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
namespace bloom::media::ffmpeg {
namespace {
// Mean-square of the silence floor.
const double kFloorEnergy = std::pow(10.0, kLossyAudioFloorDbfs / 10.0);
const double kLevelGain = std::pow(10.0, kLossyAudioLevelDb / 10.0);
const double kSnrGain = std::pow(10.0, kLossyAudioBandSnrDb / 10.0);
[[nodiscard]] double decibels(const double ratio) {
    return 10.0 * std::log10(std::max(ratio, 1e-30));
}
[[nodiscard]] std::string rounded(const double value) {
    const auto tenths = std::llround(value * 10.0);
    return (tenths < 0 ? "-" : "") + std::to_string(std::llabs(tenths) / 10) + "." +
           std::to_string(std::llabs(tenths) % 10);
}
} // namespace
LossyAudioCompare::LossyAudioCompare(const unsigned channels, const unsigned sampleRate)
    : channelCount_(channels), channels_(channels) {
    // An 8th-order Butterworth low-pass as four second-order sections (bilinear transform). The
    // corner stays under a quarter of the sample rate so a low rate still gets a stable filter.
    const double rate = std::max(1.0, static_cast<double>(sampleRate));
    const double corner = std::min(kLossyAudioBandHz, rate * 0.25);
    const double k = std::tan(std::numbers::pi * corner / rate);
    for (std::size_t section = 0; section < sections_.size(); ++section) {
        const double q = 1.0 / (2.0 * std::cos(std::numbers::pi *
                                               (2.0 * static_cast<double>(section) + 1.0) / 16.0));
        const double norm = 1.0 / (1.0 + k / q + k * k);
        sections_[section] = {.b0 = k * k * norm,
                              .b1 = 2.0 * k * k * norm,
                              .b2 = k * k * norm,
                              .a1 = 2.0 * (k * k - 1.0) * norm,
                              .a2 = (1.0 - k / q + k * k) * norm};
    }
}
double LossyAudioCompare::filter(FilterState& state, double sample) const noexcept {
    // Transposed direct form II, one section after another.
    for (std::size_t section = 0; section < sections_.size(); ++section) {
        const auto& s = sections_[section];
        const double out = s.b0 * sample + state.z1[section];
        state.z1[section] = s.b1 * sample - s.a1 * out + state.z2[section];
        state.z2[section] = s.b2 * sample - s.a2 * out;
        sample = out;
    }
    return sample;
}
void LossyAudioCompare::push(const std::span<const float> reference,
                             const std::span<const float> decoded) {
    if (channelCount_ == 0 || reference.size() != decoded.size() ||
        reference.size() % channelCount_ != 0) {
        if (measure_.failure.empty())
            measure_.failure = "Lossy audio comparison received mismatched blocks";
        return;
    }
    const std::size_t frames = reference.size() / channelCount_;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (unsigned c = 0; c < channelCount_; ++c) {
            const auto r = static_cast<double>(reference[frame * channelCount_ + c]);
            const auto d = static_cast<double>(decoded[frame * channelCount_ + c]);
            if (!std::isfinite(r) || !std::isfinite(d)) {
                nonFinite_ = true;
                continue;
            }
            auto& channel = channels_[c];
            const double rb = filter(channel.reference, r);
            const double db = filter(channel.decoded, d);
            channel.referenceBandEnergy += rb * rb;
            channel.errorBandEnergy += (db - rb) * (db - rb);
            auto& window = channel.accumulating;
            window.referenceBand += rb * rb;
            window.decodedBand += db * db;
            window.referenceFull += r * r;
            window.decodedFull += d * d;
        }
        ++measure_.frames;
        if (++windowFill_ == kLossyAudioWindowFrames)
            closeWindow();
    }
}
void LossyAudioCompare::closeWindow() {
    if (windowFill_ == 0)
        return;
    const double scale = 1.0 / static_cast<double>(windowFill_);
    for (unsigned c = 0; c < channelCount_; ++c) {
        auto& channel = channels_[c];
        auto closed = channel.accumulating;
        closed.referenceBand *= scale;
        closed.decodedBand *= scale;
        closed.referenceFull *= scale;
        closed.decodedFull *= scale;
        closed.valid = true;
        // The window before `closed` now has both neighbours.
        if (channel.current.valid)
            judge(c, channel.previous.valid ? &channel.previous : nullptr, channel.current,
                  &closed);
        channel.previous = channel.current;
        channel.current = closed;
        channel.accumulating = {};
    }
    windowFill_ = 0;
    ++measure_.windows;
}
void LossyAudioCompare::judge(const unsigned channel, const Window* before, const Window& window,
                              const Window* after) {
    const auto loudest = [&](double Window::* member) {
        double value = window.*member;
        if (before != nullptr)
            value = std::max(value, before->*member);
        if (after != nullptr)
            value = std::max(value, after->*member);
        return value;
    };
    const auto fail = [&](const char* what, const double excess) {
        measure_.worstLevelExcessDb = std::max(measure_.worstLevelExcessDb, excess);
        if (measure_.failure.empty())
            measure_.failure = std::string("Lossy audio ") + what + " in channel " +
                               std::to_string(channel) + " by " + rounded(excess) + " dB";
    };
    // The decoded window is no louder than the reference around it.
    const double bandCeiling = kLevelGain * loudest(&Window::referenceBand) + kFloorEnergy;
    if (window.decodedBand > bandCeiling)
        fail("is louder than its source", decibels(window.decodedBand / bandCeiling));
    const double fullCeiling = kLevelGain * loudest(&Window::referenceFull) + kFloorEnergy;
    if (window.decodedFull > fullCeiling)
        fail("is louder than its source", decibels(window.decodedFull / fullCeiling));
    // The reference window's sound is present in the decoded stream around it.
    const double required = window.referenceBand / kLevelGain - kFloorEnergy;
    const double present = loudest(&Window::decodedBand);
    if (required > 0.0 && present < required)
        fail("is quieter than its source", decibels(required / std::max(present, 1e-30)));
}
LossyAudioMeasure LossyAudioCompare::finish() {
    closeWindow();
    for (unsigned c = 0; c < channelCount_; ++c) {
        auto& channel = channels_[c];
        // The last window has no later neighbour.
        if (channel.current.valid)
            judge(c, channel.previous.valid ? &channel.previous : nullptr, channel.current,
                  nullptr);
        channel.current = {};
        channel.previous = {};
        if (measure_.frames == 0)
            continue;
        const double frames = static_cast<double>(measure_.frames);
        const double reference = channel.referenceBandEnergy / frames;
        const double error = channel.errorBandEnergy / frames;
        if (reference <= kFloorEnergy && error <= kFloorEnergy)
            continue; // Silence in, silence out: nothing to compare in the band.
        const double snr = decibels(std::max(reference, 1e-30) / std::max(error, 1e-30));
        if (!measure_.bandSnrMeasured || snr < measure_.bandSnrDb)
            measure_.bandSnrDb = snr;
        measure_.bandSnrMeasured = true;
        if (error > reference / kSnrGain + kFloorEnergy && measure_.failure.empty())
            measure_.failure = "Lossy audio differs from its source in channel " +
                               std::to_string(c) + ": signal-to-error ratio below 3 kHz is " +
                               rounded(snr) + " dB";
    }
    if (nonFinite_ && measure_.failure.empty())
        measure_.failure = "Lossy audio reopened with a non-finite sample";
    return measure_;
}
} // namespace bloom::media::ffmpeg
