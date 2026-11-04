#pragma once

// #340 Levels, Curves and Hue/Saturation: the durable vocabulary of the three per-channel grade
// nodes. Every value is a plain number with no colour encoding, so a Raw data pass (depth, mist,
// normals) is graded exactly as stored; the nodes never insert a colour transform.
//
// Levels and Curves address five channels in this order: the RGB master (applied to red, green and
// blue first), then red, green, blue and alpha. Each Levels channel has five animatable Scalars and
// each Curves channel has kCurvePointCount animatable Vector 2 control points (input, output).

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kLevelsNodeType = "bloom.levels";
inline constexpr std::string_view kCurvesNodeType = "bloom.curves";
inline constexpr std::string_view kHueSaturationNodeType = "bloom.hue-saturation";

inline constexpr std::size_t kGradeChannelCount = 5;
inline constexpr std::array<std::string_view, kGradeChannelCount> kGradeChannelNames{
    "master", "red", "green", "blue", "alpha"};

inline constexpr std::size_t kLevelsFieldCount = 5;
// Input black, input white, gamma, output black, output white: the order of every Levels channel's
// parameters, sockets and compiled operands.
inline constexpr std::array<std::string_view, kLevelsFieldCount> kLevelsFieldNames{
    "input-black", "input-white", "gamma", "output-black", "output-white"};
inline constexpr std::array<std::string_view, kLevelsFieldCount> kLevelsFieldRoleSuffixes{
    "InputBlack", "InputWhite", "Gamma", "OutputBlack", "OutputWhite"};
inline constexpr std::array<double, kLevelsFieldCount> kLevelsFieldDefaults{0.0, 1.0, 1.0, 0.0,
                                                                            1.0};
inline constexpr std::size_t kLevelsGammaField = 2;

// Five points per channel, spread evenly over [0, 1] on the identity line by default.
inline constexpr std::size_t kCurvePointCount = 5;

inline constexpr std::string_view kLevelsClampSchemaKey = "bloom.levels.clamp";
inline constexpr std::string_view kCurvesClampSchemaKey = "bloom.curves.clamp";
inline constexpr std::string_view kHueSaturationHueSchemaKey = "bloom.hue-saturation.hue";
inline constexpr std::string_view kHueSaturationSaturationSchemaKey =
    "bloom.hue-saturation.saturation";

// Levels black/white points and Curves coordinates are finite values bounded to +-1e6, so every
// value the grade prepares stays a normal binary32 number on the CPU and GPU routes. Gamma is
// bounded to [0.01, 100]; Hue/Saturation's hue is degrees bounded like the values and its
// saturation is a factor in [0, 1000].
inline constexpr double kMaximumGradeValue = 1.0e6;
inline constexpr double kMinimumLevelsGamma = 0.01;
inline constexpr double kMaximumLevelsGamma = 100.0;
inline constexpr double kMaximumGradeSaturation = 1000.0;

namespace detail {
[[nodiscard]] constexpr bool isGradeChannelName(const std::string_view name) noexcept {
    for (const auto channel : kGradeChannelNames)
        if (channel == name)
            return true;
    return false;
}
} // namespace detail

// "bloom.levels.<channel>.<field>"
[[nodiscard]] constexpr bool isLevelsValueSchemaKey(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.levels.";
    if (!schemaKey.starts_with(prefix))
        return false;
    const auto rest = schemaKey.substr(prefix.size());
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos || !detail::isGradeChannelName(rest.substr(0, dot)))
        return false;
    const auto field = rest.substr(dot + 1);
    for (const auto name : kLevelsFieldNames)
        if (name == field)
            return true;
    return false;
}

[[nodiscard]] constexpr bool isLevelsGammaSchemaKey(const std::string_view schemaKey) noexcept {
    return isLevelsValueSchemaKey(schemaKey) && schemaKey.ends_with(".gamma");
}

// "bloom.curves.<channel>.point-<1..kCurvePointCount>"
[[nodiscard]] constexpr bool isCurvePointSchemaKey(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.curves.";
    if (!schemaKey.starts_with(prefix))
        return false;
    const auto rest = schemaKey.substr(prefix.size());
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos || !detail::isGradeChannelName(rest.substr(0, dot)))
        return false;
    const auto point = rest.substr(dot + 1);
    return point.size() == 7 && point.starts_with("point-") && point[6] >= '1' &&
           point[6] < static_cast<char>('1' + kCurvePointCount);
}

[[nodiscard]] inline std::string levelsSchemaKey(const std::size_t channel,
                                                 const std::size_t field) {
    return "bloom.levels." + std::string(kGradeChannelNames[channel]) + "." +
           std::string(kLevelsFieldNames[field]);
}
// "masterInputBlack", "redGamma", ...
[[nodiscard]] inline std::string levelsRole(const std::size_t channel, const std::size_t field) {
    return std::string(kGradeChannelNames[channel]) + std::string(kLevelsFieldRoleSuffixes[field]);
}
// `point` counts from zero; the schema key and role count from one.
[[nodiscard]] inline std::string curvePointSchemaKey(const std::size_t channel,
                                                     const std::size_t point) {
    return "bloom.curves." + std::string(kGradeChannelNames[channel]) + ".point-" +
           std::to_string(point + 1);
}
// "masterPoint1", "alphaPoint5", ...
[[nodiscard]] inline std::string curvePointRole(const std::size_t channel,
                                                const std::size_t point) {
    return std::string(kGradeChannelNames[channel]) + "Point" + std::to_string(point + 1);
}
[[nodiscard]] constexpr double defaultCurvePointCoordinate(const std::size_t point) noexcept {
    return static_cast<double>(point) / static_cast<double>(kCurvePointCount - 1);
}

} // namespace bloom::document
