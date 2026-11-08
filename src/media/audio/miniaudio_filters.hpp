#pragma once

// The bundled audio library's biquad filters behind a small interface, so the effect processors
// (strict warnings, no third-party header) never include the library header.

#include <cstdint>
#include <memory>

namespace bloom::media::audio::detail {

enum class FilterType : std::uint8_t { HighPass, LowPass, Peak, LowShelf, HighShelf };

class MiniaudioFilter final {
  public:
    MiniaudioFilter(FilterType type, std::uint32_t channels, std::uint32_t sampleRate);
    ~MiniaudioFilter();
    MiniaudioFilter(const MiniaudioFilter&) = delete;
    MiniaudioFilter& operator=(const MiniaudioFilter&) = delete;

    // Sets the corner or center `frequency` (Hz), the band `gainDb` (peak and shelves) and `q`
    // (peak). The first call allocates; later calls keep the filter state. False on a refusal.
    bool configure(double frequency, double gainDb, double q) noexcept;
    // Forgets the filter's history.
    void clear() noexcept;
    // Filters one frame (`channels` samples) in place.
    void process(float* frame) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::media::audio::detail
