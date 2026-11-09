#include <bloom/media/audio/analysis.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <new>

namespace bloom::media::audio::analysis {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint64_t kChunkFrames = 16384;

template <typename T> using Result = AudioResult<T>;

[[nodiscard]] bool usable(const AudioBuffer& buffer) noexcept {
    if (buffer.rate == 0 || buffer.channels == 0)
        return false;
    if (buffer.stream)
        return buffer.stream->probe().channels == buffer.channels;
    return buffer.planes.size() == buffer.channels;
}

// The interleaved samples of the frames [first, first + count), silence outside the source.
[[nodiscard]] Result<std::size_t> fillChunk(const AudioBuffer& buffer, const std::int64_t first,
                                            const std::size_t count, std::vector<float>& out,
                                            const CancelAudioWork& cancel) noexcept {
    const auto channels = static_cast<std::size_t>(buffer.channels);
    out.assign(count * channels, 0.0F);
    // The part of the chunk that lies inside the source.
    const std::int64_t begin = std::max<std::int64_t>(first, 0);
    const std::int64_t sourceEnd =
        static_cast<std::int64_t>(std::min<std::uint64_t>(
            buffer.stream ? buffer.stream->probe().frames : buffer.frames,
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
    const std::int64_t end = std::min<std::int64_t>(first + static_cast<std::int64_t>(count), sourceEnd);
    if (end <= begin)
        return Result<std::size_t>::success(count);
    const auto offset = static_cast<std::size_t>(begin - first);
    const auto inside = static_cast<std::size_t>(end - begin);
    if (buffer.stream) {
        const auto read = buffer.stream->readInterleaved(
            static_cast<std::uint64_t>(begin),
            std::span<float>(out.data() + offset * channels, inside * channels), cancel);
        if (!read)
            return Result<std::size_t>::failure(*read.error());
        return Result<std::size_t>::success(count);
    }
    for (std::size_t channel = 0; channel < channels; ++channel) {
        const auto& plane = buffer.planes[channel];
        if (plane.size() < static_cast<std::size_t>(end))
            return Result<std::size_t>::failure(AudioError::codeOnly(AudioErrorCode::InvalidBuffer));
        for (std::size_t frame = 0; frame < inside; ++frame)
            out[(offset + frame) * channels + channel] =
                plane[static_cast<std::size_t>(begin) + frame];
    }
    return Result<std::size_t>::success(count);
}

[[nodiscard]] bool cancelled(const CancelAudioWork& cancel) noexcept {
    return cancel && cancel();
}

// Calls visit(firstOffset, samples, frames) for each chunk of the window, in order.
template <typename Visit>
[[nodiscard]] std::optional<AudioError> forEachChunk(const AudioBuffer& buffer,
                                                     const std::int64_t first,
                                                     const std::uint64_t frames,
                                                     const CancelAudioWork& cancel,
                                                     Visit&& visit) noexcept {
    std::vector<float> chunk;
    for (std::uint64_t done = 0; done < frames;) {
        if (cancelled(cancel))
            return AudioError::codeOnly(AudioErrorCode::Cancelled);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkFrames, frames - done));
        const auto filled = fillChunk(buffer, first + static_cast<std::int64_t>(done), count, chunk, cancel);
        if (!filled)
            return *filled.error();
        visit(done, std::span<const float>(chunk), count);
        done += count;
    }
    return std::nullopt;
}

AudioError invalid() noexcept { return AudioError::codeOnly(AudioErrorCode::InvalidBuffer); }

} // namespace

AudioResult<std::vector<float>> readFrames(const AudioBuffer& buffer, const std::int64_t first,
                                           const std::uint64_t count,
                                           const CancelAudioWork& cancel) noexcept {
    if (!usable(buffer) || count > kMaximumWindowFrames)
        return Result<std::vector<float>>::failure(invalid());
    try {
        std::vector<float> all;
        all.reserve(static_cast<std::size_t>(count) * buffer.channels);
        const auto error = forEachChunk(buffer, first, count, cancel,
                                        [&](std::uint64_t, std::span<const float> samples, std::size_t frames) {
                                            all.insert(all.end(), samples.begin(),
                                                       samples.begin() + static_cast<std::ptrdiff_t>(frames * buffer.channels));
                                        });
        if (error)
            return Result<std::vector<float>>::failure(*error);
        return Result<std::vector<float>>::success(std::move(all));
    } catch (const std::bad_alloc&) {
        return Result<std::vector<float>>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

AudioResult<double> amplitude(const AudioBuffer& buffer, const std::int64_t first,
                              const std::uint64_t frames, const Measure measure,
                              const CancelAudioWork& cancel) noexcept {
    if (!usable(buffer) || frames == 0 || frames > kMaximumWindowFrames)
        return Result<double>::failure(invalid());
    try {
        double peak = 0.0;
        long double sum = 0.0L;
        const auto error = forEachChunk(
            buffer, first, frames, cancel,
            [&](std::uint64_t, std::span<const float> samples, std::size_t count) {
                const auto total = count * buffer.channels;
                for (std::size_t i = 0; i < total; ++i) {
                    const double v = static_cast<double>(samples[i]);
                    peak = std::max(peak, std::abs(v));
                    sum += static_cast<long double>(v) * static_cast<long double>(v);
                }
            });
        if (error)
            return Result<double>::failure(*error);
        if (measure == Measure::Peak)
            return Result<double>::success(peak);
        const long double mean = sum / (static_cast<long double>(frames) * buffer.channels);
        return Result<double>::success(static_cast<double>(std::sqrt(mean)));
    } catch (const std::bad_alloc&) {
        return Result<double>::failure(AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

AudioResult<WaveformColumns> waveform(const AudioBuffer& buffer, const std::int64_t first,
                                      const std::uint64_t frames, const std::uint32_t columns,
                                      const bool separateChannels,
                                      const CancelAudioWork& cancel) noexcept {
    if (!usable(buffer) || frames == 0 || frames > kMaximumWindowFrames || columns == 0 ||
        columns > kMaximumColumns)
        return Result<WaveformColumns>::failure(invalid());
    const std::uint32_t lanes =
        separateChannels ? std::min(buffer.channels, kMaximumLanes) : 1U;
    try {
        WaveformColumns result;
        result.columns = columns;
        result.lanes = lanes;
        result.values.assign(static_cast<std::size_t>(columns) * lanes * 2, 0.0F);
        std::vector<bool> seen(static_cast<std::size_t>(columns) * lanes, false);
        // The first frame of column c (offsets within the window).
        const auto start = [&](const std::uint64_t column) {
            const auto whole = frames / columns;
            const auto rest = frames % columns;
            return whole * column + (rest * column) / columns;
        };
        std::uint32_t column = 0;
        const auto error = forEachChunk(
            buffer, first, frames, cancel,
            [&](const std::uint64_t done, std::span<const float> samples, const std::size_t count) {
                for (std::size_t i = 0; i < count; ++i) {
                    const std::uint64_t offset = done + i;
                    // Columns narrower than a frame (a window shorter than the image is wide) take
                    // the frame each of them starts on: every column reads at least one frame.
                    while (column + 1 < columns && offset >= start(column + 1))
                        ++column;
                    for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                        float value = 0.0F;
                        if (separateChannels) {
                            value = samples[i * buffer.channels + lane];
                        } else {
                            double mean = 0.0;
                            for (std::uint32_t c = 0; c < buffer.channels; ++c)
                                mean += static_cast<double>(samples[i * buffer.channels + c]);
                            value = static_cast<float>(mean / buffer.channels);
                        }
                        const auto slot = (static_cast<std::size_t>(column) * lanes + lane);
                        auto& low = result.values[slot * 2];
                        auto& high = result.values[slot * 2 + 1];
                        if (!seen[slot]) {
                            low = value;
                            high = value;
                            seen[slot] = true;
                        } else {
                            low = std::min(low, value);
                            high = std::max(high, value);
                        }
                    }
                }
            });
        if (error)
            return Result<WaveformColumns>::failure(*error);
        // A column that starts where the previous one ended read no frame of its own when the window
        // is shorter than the image is wide: it repeats the nearest earlier column.
        for (std::uint32_t c = 1; c < columns; ++c)
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                const auto slot = static_cast<std::size_t>(c) * lanes + lane;
                if (!seen[slot]) {
                    const auto previous = slot - lanes;
                    result.values[slot * 2] = result.values[previous * 2];
                    result.values[slot * 2 + 1] = result.values[previous * 2 + 1];
                    seen[slot] = true;
                }
            }
        return Result<WaveformColumns>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<WaveformColumns>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

namespace {

// In-place iterative radix-2 transform of a power-of-two length.
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(length);
        for (std::size_t i = 0; i < n; i += length)
            for (std::size_t k = 0; k < length / 2; ++k) {
                const std::complex<double> w(std::cos(angle * static_cast<double>(k)),
                                             std::sin(angle * static_cast<double>(k)));
                const auto u = a[i + k];
                const auto v = a[i + k + length / 2] * w;
                a[i + k] = u + v;
                a[i + k + length / 2] = u - v;
            }
    }
}

[[nodiscard]] double columnFrequency(const SpectrumSettings& s, const double unit) {
    return s.logarithmic ? s.lowHz * std::pow(s.highHz / s.lowHz, unit)
                         : s.lowHz + (s.highHz - s.lowHz) * unit;
}

} // namespace

AudioResult<std::vector<float>> spectrum(const AudioBuffer& buffer, const std::int64_t center,
                                         const SpectrumSettings& settings,
                                         const std::uint32_t columns,
                                         const CancelAudioWork& cancel) noexcept {
    const auto n = settings.fftSize;
    const bool power = n >= kMinimumFftSize && n <= kMaximumFftSize && (n & (n - 1)) == 0;
    if (!usable(buffer) || !power || columns == 0 || columns > kMaximumColumns ||
        !(settings.lowHz > 0.0) || !(settings.highHz > settings.lowHz) ||
        !(settings.ceilingDb > settings.floorDb))
        return Result<std::vector<float>>::failure(invalid());
    try {
        const auto samples =
            readFrames(buffer, center - static_cast<std::int64_t>(n / 2), n, cancel);
        if (!samples)
            return Result<std::vector<float>>::failure(*samples.error());
        std::vector<std::complex<double>> a(n);
        double windowSum = 0.0;
        for (std::uint32_t i = 0; i < n; ++i) {
            double mono = 0.0;
            for (std::uint32_t c = 0; c < buffer.channels; ++c)
                mono += static_cast<double>(
                    (*samples.value())[static_cast<std::size_t>(i) * buffer.channels + c]);
            mono /= buffer.channels;
            const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * i / static_cast<double>(n));
            windowSum += window;
            a[i] = mono * window;
        }
        if (cancelled(cancel))
            return Result<std::vector<float>>::failure(
                AudioError::codeOnly(AudioErrorCode::Cancelled));
        fft(a);
        const std::uint32_t bins = n / 2;
        std::vector<double> magnitude(bins);
        for (std::uint32_t k = 0; k < bins; ++k)
            magnitude[k] = 2.0 * std::abs(a[k]) / windowSum;
        const double binWidth = static_cast<double>(buffer.rate) / static_cast<double>(n);
        const auto level = [&](const double magnitudeValue) {
            const double db = 20.0 * std::log10(std::max(magnitudeValue, 1e-12));
            return static_cast<float>(std::clamp(
                (db - settings.floorDb) / (settings.ceilingDb - settings.floorDb), 0.0, 1.0));
        };
        std::vector<float> out(columns);
        for (std::uint32_t c = 0; c < columns; ++c) {
            const double f0 = columnFrequency(settings, static_cast<double>(c) / columns);
            const double f1 = columnFrequency(settings, static_cast<double>(c + 1) / columns);
            auto low = static_cast<std::int64_t>(std::ceil(f0 / binWidth));
            auto high = static_cast<std::int64_t>(std::ceil(f1 / binWidth)) - 1;
            if (high < low) {
                low = high = static_cast<std::int64_t>(std::llround(0.5 * (f0 + f1) / binWidth));
            }
            low = std::clamp<std::int64_t>(low, 1, bins - 1);
            high = std::clamp<std::int64_t>(high, low, bins - 1);
            double peak = 0.0;
            for (auto k = low; k <= high; ++k)
                peak = std::max(peak, magnitude[static_cast<std::size_t>(k)]);
            out[c] = level(peak);
        }
        return Result<std::vector<float>>::success(std::move(out));
    } catch (const std::bad_alloc&) {
        return Result<std::vector<float>>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

} // namespace bloom::media::audio::analysis
