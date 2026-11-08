#pragma once

// #569 audio effects: the processors an audio layer's effect chain runs on the mixer, one frame at
// a time, after the clip's level. Playback and the offline export mix run this same code on the same
// absolute sample positions, so a layer sounds the same in both.
//
// The filters (high-pass, low-pass, peaking and shelving bands) are the standard biquads of the
// bundled audio library. The dynamics (compressor, auto level) and the noise are small, fixed
// algorithms with no library equivalent; they are specified in docs/architecture/media-io.md,
// "Audio Effects".
//
// Parameters travel as plain value arrays in the order of kEffectRoles. The compiled plan resolves
// them from the document at a time; the exporter also resolves them at the end of a block, and the
// processor moves linearly between the two over the block (see EffectSettings::endValues).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::media::audio::effects {

enum class EffectKind : std::uint8_t { Equalizer, Compressor, AutoLevel, Mono, Hiss };

inline constexpr std::size_t kMaxEffectValues = 9;
// The widest channel layout an effect chain processes; a wider mix plays without effects.
inline constexpr std::uint32_t kMaxEffectChannels = 64;

// The parameter roles of each kind, in the order of EffectSettings::values.
inline constexpr std::array<std::string_view, 9> kEqualizerRoles{
    "highPass", "lowPass", "lowGain", "lowFrequency", "midGain",
    "midFrequency", "midQ", "highGain", "highFrequency"};
inline constexpr std::array<std::string_view, 6> kCompressorRoles{
    "threshold", "ratio", "attack", "release", "knee", "makeup"};
inline constexpr std::array<std::string_view, 4> kAutoLevelRoles{"target", "maxGain", "response",
                                                                 "gate"};
inline constexpr std::array<std::string_view, 1> kMonoRoles{"amount"};
inline constexpr std::array<std::string_view, 3> kHissRoles{"level", "tone", "seed"};

[[nodiscard]] constexpr std::span<const std::string_view> effectRoles(EffectKind kind) noexcept {
    switch (kind) {
    case EffectKind::Equalizer:
        return kEqualizerRoles;
    case EffectKind::Compressor:
        return kCompressorRoles;
    case EffectKind::AutoLevel:
        return kAutoLevelRoles;
    case EffectKind::Mono:
        return kMonoRoles;
    case EffectKind::Hiss:
        return kHissRoles;
    }
    return {};
}

// One effect of a chain with its parameters: `values` at the start of a ramp, `endValues` at its
// end. Without a ramp only `values` is read.
struct EffectSettings final {
    EffectKind kind = EffectKind::Mono;
    std::array<float, kMaxEffectValues> values{};
    std::array<float, kMaxEffectValues> endValues{};
    friend bool operator==(const EffectSettings&, const EffectSettings&) = default;
};

// Settings of `kind` from parameter values in the node's definition order; `endValues` (empty means
// the same as `values`) are the values at the end of a ramp. Values past kMaxEffectValues are
// ignored.
[[nodiscard]] EffectSettings makeEffectSettings(EffectKind kind, std::span<const double> values,
                                                std::span<const double> endValues = {}) noexcept;

// How far back the dynamics (compressor, auto level) look to warm their state when playback starts
// or jumps, in seconds. A chain restarted anywhere has heard this much of the audio before it, so
// the result does not depend on where playback began (export starts the same way).
inline constexpr double kEffectWarmupSeconds = 0.5;

// The state of one clip's chain across mixer blocks. Create it on a control thread; process() never
// allocates.
class EffectChain final {
  public:
    EffectChain(std::uint32_t rate, std::uint32_t channels);
    ~EffectChain();
    EffectChain(const EffectChain&) = delete;
    EffectChain& operator=(const EffectChain&) = delete;

    // Rebuilds the processors for `settings` (the kinds, in order). Control thread only; keeps the
    // state when the kinds are unchanged.
    void configure(std::span<const EffectSettings> settings);
    [[nodiscard]] bool matches(std::span<const EffectSettings> settings) const noexcept;

    // True when `frame` is the frame after the last one processed (or a few frames on, at a
    // transport rate above 1), so the state carries on.
    [[nodiscard]] bool continuous(std::int64_t frame) const noexcept;
    // Forgets every state: the next frame processed starts the chain afresh.
    void reset() noexcept;

    // Processes one frame in place. `frame` holds `channels` samples; `absoluteFrame` is its
    // position on the transport's sample grid (the noise and the coefficient updates key off it),
    // and `progress` in [0, 1] moves each parameter from values to endValues.
    void process(float* frame, std::int64_t absoluteFrame,
                 std::span<const EffectSettings> settings, double progress) noexcept;
    // Marks the chain as not having processed this frame, so the next one restarts the state.
    void skip() noexcept { last_ = kNone; }

  private:
    struct State;
    static constexpr std::int64_t kNone = INT64_MIN;
    std::uint32_t rate_;
    std::uint32_t channels_;
    std::int64_t last_ = kNone;
    std::unique_ptr<State> state_;
};

} // namespace bloom::media::audio::effects
