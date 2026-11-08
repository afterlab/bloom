// The filter half of the bundled audio library. The library's implementation is compiled once, in
// miniaudio_backend.cpp; this file only calls its filter API.
#include <miniaudio.h>

#include "miniaudio_filters.hpp"

#include <memory>

namespace bloom::media::audio::detail {

namespace {
// The high-pass and low-pass are fourth order (two cascaded second-order sections, 24 dB per
// octave), so a band limit such as a telephone's is steep enough to hear.
constexpr ma_uint32 kPassOrder = 4;
constexpr double kShelfSlope = 1.0;
} // namespace

struct MiniaudioFilter::Impl final {
    FilterType type;
    ma_uint32 channels;
    ma_uint32 rate;
    bool ready = false;
    ma_hpf hpf{};
    ma_lpf lpf{};
    ma_peak2 peak{};
    ma_loshelf2 low{};
    ma_hishelf2 high{};

    Impl(const FilterType t, const std::uint32_t c, const std::uint32_t r)
        : type(t), channels(c), rate(r) {}
    ~Impl() {
        if (!ready)
            return;
        switch (type) {
        case FilterType::HighPass:
            ma_hpf_uninit(&hpf, nullptr);
            break;
        case FilterType::LowPass:
            ma_lpf_uninit(&lpf, nullptr);
            break;
        default:
            break;
        }
    }
};

MiniaudioFilter::MiniaudioFilter(const FilterType type, const std::uint32_t channels,
                                 const std::uint32_t sampleRate)
    : impl_(std::make_unique<Impl>(type, channels, sampleRate)) {}

MiniaudioFilter::~MiniaudioFilter() = default;

bool MiniaudioFilter::configure(const double frequency, const double gainDb,
                                const double q) noexcept {
    auto& f = *impl_;
    const bool first = !f.ready;
    ma_result result = MA_ERROR;
    switch (f.type) {
    case FilterType::HighPass: {
        const auto config = ma_hpf_config_init(ma_format_f32, f.channels, f.rate, frequency,
                                               kPassOrder);
        result = first ? ma_hpf_init(&config, nullptr, &f.hpf) : ma_hpf_reinit(&config, &f.hpf);
        break;
    }
    case FilterType::LowPass: {
        const auto config = ma_lpf_config_init(ma_format_f32, f.channels, f.rate, frequency,
                                               kPassOrder);
        result = first ? ma_lpf_init(&config, nullptr, &f.lpf) : ma_lpf_reinit(&config, &f.lpf);
        break;
    }
    case FilterType::Peak: {
        const auto config =
            ma_peak2_config_init(ma_format_f32, f.channels, f.rate, gainDb, q, frequency);
        result = first ? ma_peak2_init(&config, nullptr, &f.peak) : ma_peak2_reinit(&config, &f.peak);
        break;
    }
    case FilterType::LowShelf: {
        const auto config = ma_loshelf2_config_init(ma_format_f32, f.channels, f.rate, gainDb,
                                                    kShelfSlope, frequency);
        result =
            first ? ma_loshelf2_init(&config, nullptr, &f.low) : ma_loshelf2_reinit(&config, &f.low);
        break;
    }
    case FilterType::HighShelf: {
        const auto config = ma_hishelf2_config_init(ma_format_f32, f.channels, f.rate, gainDb,
                                                    kShelfSlope, frequency);
        result = first ? ma_hishelf2_init(&config, nullptr, &f.high)
                       : ma_hishelf2_reinit(&config, &f.high);
        break;
    }
    }
    if (result == MA_SUCCESS)
        f.ready = true;
    return result == MA_SUCCESS;
}

void MiniaudioFilter::clear() noexcept {
    auto& f = *impl_;
    if (!f.ready)
        return;
    switch (f.type) {
    case FilterType::HighPass:
        // The library offers no clear for the high-pass; its even-order sections are biquads.
        for (ma_uint32 section = 0; section < f.hpf.hpf2Count; ++section)
            (void)ma_biquad_clear_cache(&f.hpf.pHPF2[section].bq);
        break;
    case FilterType::LowPass:
        (void)ma_lpf_clear_cache(&f.lpf);
        break;
    case FilterType::Peak:
        (void)ma_biquad_clear_cache(&f.peak.bq);
        break;
    case FilterType::LowShelf:
        (void)ma_biquad_clear_cache(&f.low.bq);
        break;
    case FilterType::HighShelf:
        (void)ma_biquad_clear_cache(&f.high.bq);
        break;
    }
}

void MiniaudioFilter::process(float* frame) noexcept {
    auto& f = *impl_;
    if (!f.ready)
        return;
    switch (f.type) {
    case FilterType::HighPass:
        (void)ma_hpf_process_pcm_frames(&f.hpf, frame, frame, 1);
        break;
    case FilterType::LowPass:
        (void)ma_lpf_process_pcm_frames(&f.lpf, frame, frame, 1);
        break;
    case FilterType::Peak:
        (void)ma_peak2_process_pcm_frames(&f.peak, frame, frame, 1);
        break;
    case FilterType::LowShelf:
        (void)ma_loshelf2_process_pcm_frames(&f.low, frame, frame, 1);
        break;
    case FilterType::HighShelf:
        (void)ma_hishelf2_process_pcm_frames(&f.high, frame, frame, 1);
        break;
    }
}

} // namespace bloom::media::audio::detail
