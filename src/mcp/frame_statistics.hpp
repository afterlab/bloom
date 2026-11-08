#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bloom::mcp {

// One channel's statistics over a set of pixels. The histogram has `bins` equal bins over
// [0, 1]; `below` counts samples under 0 and `above` samples over 1 (scene-referred values exceed
// 1), so the counts sum to the sample count.
struct ChannelStatistics final {
    double mean = 0.0;
    double min = 0.0;
    double max = 0.0;
    std::vector<std::uint64_t> histogram;
    std::uint64_t below = 0;
    std::uint64_t above = 0;
};

struct FrameStatistics final {
    std::uint64_t pixels = 0;
    ChannelStatistics luma;
    ChannelStatistics red;
    ChannelStatistics green;
    ChannelStatistics blue;
    double alphaMean = 0.0;
};

// Accumulates pixels from any number of frames. Luma is the Rec.709 weighted sum of the
// working-space red, green and blue the accumulator is given; the values are used as they are
// (premultiplied, so a transparent pixel counts as black).
class StatisticsAccumulator final {
  public:
    explicit StatisticsAccumulator(std::size_t bins);

    void addPixel(float red, float green, float blue, float alpha);
    // `rgba` holds width * height pixels as four floats each; the region is in pixels from the
    // top-left corner and must lie inside the image.
    void add(std::span<const float> rgba, std::uint64_t width, std::uint64_t height,
             std::uint64_t regionX, std::uint64_t regionY, std::uint64_t regionWidth,
             std::uint64_t regionHeight);
    [[nodiscard]] FrameStatistics result() const;

  private:
    struct Channel final {
        long double sum = 0.0L;
        double min = 0.0;
        double max = 0.0;
        std::vector<std::uint64_t> histogram;
        std::uint64_t below = 0;
        std::uint64_t above = 0;
    };
    void sample(Channel& channel, double value);

    std::size_t bins_;
    std::uint64_t pixels_ = 0;
    long double alphaSum_ = 0.0L;
    Channel luma_;
    Channel red_;
    Channel green_;
    Channel blue_;
};

} // namespace bloom::mcp
