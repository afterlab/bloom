#include <bloom/runtime/footage_time.hpp>

#include <bloom/runtime/composition_time.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace bloom::runtime::detail {

std::optional<core::RationalTime> shiftedByFrames(const core::RationalTime time,
                                                  const std::int64_t start,
                                                  const document::FrameRate rate) {
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (start < -1000000000 || start > 1000000000)
        return {};
    const auto offset = core::RationalTime::create(start * rate.denominator(), rate.numerator());
    if (!offset)
        return {};
    const auto divisor = std::gcd(time.denominator(), offset->denominator());
    if (divisor <= 0)
        return {};
    const auto leftScale = offset->denominator() / divisor,
               rightScale = time.denominator() / divisor;
    if (leftScale <= 0 || rightScale <= 0)
        return {};
    if (time.numerator() == -limit - 1 || std::abs(time.numerator()) > limit / leftScale ||
        std::abs(offset->numerator()) > limit / rightScale ||
        time.denominator() > limit / leftScale)
        return {};
    const auto left = time.numerator() * leftScale, right = offset->numerator() * rightScale;
    if ((right > 0 && left < -limit + right) || (right < 0 && left > limit + right))
        return {};
    return core::RationalTime::create(left - right, time.denominator() * leftScale);
}

std::optional<std::uint64_t> footageFrameIndex(const document::AssetRecord& asset,
                                               const core::RationalTime time,
                                               const std::int64_t startFrame,
                                               const std::int64_t loopMode,
                                               const document::FrameRate compositionRate) {
    const auto frames = document::footageFrameCount(asset);
    if (frames == 0)
        return std::nullopt;
    const auto loops = static_cast<std::uint64_t>(
        std::clamp<std::uint32_t>(asset.interpretation.loopCount, 1, document::kMaxAssetLoopCount));
    const auto total = frames * loops;
    std::int64_t index = 0;
    if (const auto rate = document::footageFrameRate(asset)) {
        const auto local = shiftedByFrames(time, startFrame, compositionRate);
        const auto frame = local ? valueGraphFrameIndex(*local, *rate) : std::nullopt;
        if (!frame)
            return std::nullopt;
        index = *frame;
    } else {
        // One member per composition frame (a sequence without an assumed rate).
        const auto frame = valueGraphFrameIndex(time, compositionRate);
        if (!frame)
            return std::nullopt;
        // Long-double subtraction avoids signed overflow even for hostile hand-built plans; 2^63
        // is exactly representable even where long double is only double (MSVC).
        const auto elapsed =
            static_cast<long double>(*frame) - static_cast<long double>(startFrame);
        if (elapsed < -9223372036854775808.0L || elapsed >= 9223372036854775808.0L)
            return std::nullopt;
        index = static_cast<std::int64_t>(elapsed);
    }
    auto offset = static_cast<std::uint64_t>(std::max<std::int64_t>(0, index));
    if (loopMode == 1) {
        offset %= total;
    } else if (loopMode == 2 && total > 1) {
        const auto period = 2 * (total - 1);
        offset %= period;
        if (offset >= total)
            offset = period - offset;
    } else {
        offset = std::min(offset, total - 1);
    }
    return offset % frames;
}

std::optional<core::RationalTime> footageAudioTime(const FootageAudioTiming& footage,
                                                   const core::RationalTime start,
                                                   const core::RationalTime time) {
    if (time < start)
        return time;
    // Binary64 seconds: an audio clip time is resolved to an output sample right after, far
    // coarser than the representation error here.
    const double relative = time.toSeconds() - start.toSeconds();
    const double pass = footage.pass.toSeconds();
    if (!std::isfinite(relative) || !(pass > 0.0))
        return std::nullopt;
    const double passIndex = std::floor(relative / pass);
    if (passIndex >= static_cast<double>(footage.loops))
        return std::nullopt;
    const double wrapped = std::max(0.0, relative - passIndex * pass);
    const double scaled =
        wrapped * static_cast<double>(footage.numerator) / static_cast<double>(footage.denominator);
    return secondsToTime(start.toSeconds() + scaled);
}

} // namespace bloom::runtime::detail
