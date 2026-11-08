#include "frame_statistics.hpp"

#include <algorithm>
#include <cmath>

namespace bloom::mcp {
namespace {

ChannelStatistics finish(const long double sum, const std::uint64_t count, const double min,
                         const double max, const std::vector<std::uint64_t>& histogram,
                         const std::uint64_t below, const std::uint64_t above) {
    ChannelStatistics result;
    result.mean = count == 0 ? 0.0 : static_cast<double>(sum / static_cast<long double>(count));
    result.min = count == 0 ? 0.0 : min;
    result.max = count == 0 ? 0.0 : max;
    result.histogram = histogram;
    result.below = below;
    result.above = above;
    return result;
}

} // namespace

StatisticsAccumulator::StatisticsAccumulator(const std::size_t bins) : bins_(bins) {
    for (auto* channel : {&luma_, &red_, &green_, &blue_})
        channel->histogram.assign(bins_, 0U);
}

void StatisticsAccumulator::sample(Channel& channel, const double value) {
    if (pixels_ == 0) {
        channel.min = value;
        channel.max = value;
    } else {
        channel.min = std::min(channel.min, value);
        channel.max = std::max(channel.max, value);
    }
    channel.sum += static_cast<long double>(value);
    if (value < 0.0) {
        ++channel.below;
    } else if (value > 1.0) {
        ++channel.above;
    } else {
        const auto bin = std::min(bins_ - 1U, static_cast<std::size_t>(value * double(bins_)));
        ++channel.histogram[bin];
    }
}

void StatisticsAccumulator::addPixel(const float red, const float green, const float blue,
                                     const float alpha) {
    const auto r = static_cast<double>(red);
    const auto g = static_cast<double>(green);
    const auto b = static_cast<double>(blue);
    sample(red_, r);
    sample(green_, g);
    sample(blue_, b);
    sample(luma_, 0.2126 * r + 0.7152 * g + 0.0722 * b);
    alphaSum_ += static_cast<long double>(alpha);
    ++pixels_;
}

void StatisticsAccumulator::add(const std::span<const float> rgba, const std::uint64_t width,
                                const std::uint64_t height, const std::uint64_t regionX,
                                const std::uint64_t regionY, const std::uint64_t regionWidth,
                                const std::uint64_t regionHeight) {
    if (rgba.size() < width * height * 4U || regionX + regionWidth > width ||
        regionY + regionHeight > height)
        return;
    for (std::uint64_t y = regionY; y < regionY + regionHeight; ++y) {
        for (std::uint64_t x = regionX; x < regionX + regionWidth; ++x) {
            const auto* pixel = rgba.data() + (y * width + x) * 4U;
            addPixel(pixel[0], pixel[1], pixel[2], pixel[3]);
        }
    }
}

FrameStatistics StatisticsAccumulator::result() const {
    FrameStatistics result;
    result.pixels = pixels_;
    const auto of = [&](const Channel& channel) {
        return finish(channel.sum, pixels_, channel.min, channel.max, channel.histogram,
                      channel.below, channel.above);
    };
    result.luma = of(luma_);
    result.red = of(red_);
    result.green = of(green_);
    result.blue = of(blue_);
    result.alphaMean =
        pixels_ == 0 ? 0.0 : static_cast<double>(alphaSum_ / static_cast<long double>(pixels_));
    return result;
}

} // namespace bloom::mcp
