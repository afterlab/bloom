#pragma once

// #340 Levels, Curves and Hue/Saturation: the durable vocabulary of the three per-channel grade
// nodes. Every value is a plain number with no colour encoding, so a Raw data pass (depth, mist,
// normals) is graded exactly as stored; the nodes never insert a colour transform.
//
// Levels and Curves address five channels in this order: the RGB master (applied to red, green and
// blue first), then red, green, blue and alpha. Each Levels channel has five animatable Scalars.
// Curves schema 2 (#493) has one curve per channel plus four hue curves, each an animatable Path
// value of any number of (input, output) points (bloom/document/grade_curves.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

// Curves schema 1 (#340) had five Vector 2 points per channel ("bloom.curves.<channel>.point-<n>",
// roles "<channel>Point<n>"). That vocabulary survives only so Project I/O can migrate a schema 1
// node on open; no current definition, validation or command accepts it.
inline constexpr std::size_t kLegacyCurvePointCount = 5;
inline constexpr std::uint32_t kLegacyCurvesNodeSchemaVersion = 1;

// Curves schema 2 (#493): nine curves, each its own animatable Path parameter with no socket. The
// five channel curves come first in the channel order above; the four hue curves follow. A hue
// curve's input is the periodic hue in [0, 1] (bloom/runtime/grade_curve.hpp defines the model);
// Luma vs Saturation's input is the luma. Hue vs Hue's output is a hue shift in turns; the other
// three output a gain, 1 meaning unchanged.
inline constexpr std::uint32_t kCurvesNodeSchemaVersion = 2;
enum class GradeCurve : std::uint8_t {
    Master,
    Red,
    Green,
    Blue,
    Alpha,
    HueVsHue,
    HueVsSaturation,
    HueVsLuma,
    LumaVsSaturation,
};
inline constexpr std::size_t kGradeCurveCount = 9;
inline constexpr std::array<GradeCurve, kGradeCurveCount> kGradeCurves{
    GradeCurve::Master,   GradeCurve::Red,           GradeCurve::Green,
    GradeCurve::Blue,     GradeCurve::Alpha,         GradeCurve::HueVsHue,
    GradeCurve::HueVsSaturation, GradeCurve::HueVsLuma, GradeCurve::LumaVsSaturation};
// Roles and schema keys in kGradeCurves order.
inline constexpr std::array<std::string_view, kGradeCurveCount> kGradeCurveRoles{
    "master", "red", "green", "blue", "alpha", "hueVsHue", "hueVsSaturation", "hueVsLuma",
    "lumaVsSaturation"};
inline constexpr std::array<std::string_view, kGradeCurveCount> kGradeCurveSchemaKeys{
    "bloom.curves.master",          "bloom.curves.red",
    "bloom.curves.green",           "bloom.curves.blue",
    "bloom.curves.alpha",           "bloom.curves.hue-vs-hue",
    "bloom.curves.hue-vs-saturation", "bloom.curves.hue-vs-luma",
    "bloom.curves.luma-vs-saturation"};
// A curve holds at most this many points; the CPU and GPU find a point's segment by binary search.
inline constexpr std::size_t kMaximumCurvePoints = 256;
// Hue vs Hue shifts by at most half a turn either way; a gain is within [0, 1000].
inline constexpr double kMaximumHueShift = 0.5;
inline constexpr double kMaximumCurveGain = 1000.0;

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

[[nodiscard]] constexpr std::size_t gradeCurveIndex(const GradeCurve curve) noexcept {
    return static_cast<std::size_t>(curve);
}
[[nodiscard]] constexpr std::string_view gradeCurveRole(const GradeCurve curve) noexcept {
    return kGradeCurveRoles[gradeCurveIndex(curve)];
}
[[nodiscard]] constexpr std::string_view gradeCurveSchemaKey(const GradeCurve curve) noexcept {
    return kGradeCurveSchemaKeys[gradeCurveIndex(curve)];
}
[[nodiscard]] constexpr std::optional<GradeCurve>
gradeCurveFromSchemaKey(const std::string_view schemaKey) noexcept {
    for (std::size_t index = 0; index < kGradeCurveCount; ++index)
        if (kGradeCurveSchemaKeys[index] == schemaKey)
            return kGradeCurves[index];
    return std::nullopt;
}
[[nodiscard]] constexpr bool isGradeCurveSchemaKey(const std::string_view schemaKey) noexcept {
    return gradeCurveFromSchemaKey(schemaKey).has_value();
}
// Hue vs Hue, Hue vs Saturation and Hue vs Luma: periodic over the hue.
[[nodiscard]] constexpr bool isHueGradeCurve(const GradeCurve curve) noexcept {
    return curve == GradeCurve::HueVsHue || curve == GradeCurve::HueVsSaturation ||
           curve == GradeCurve::HueVsLuma;
}
// The channel curves map a value to a value; the other four output a shift or a gain.
[[nodiscard]] constexpr bool isChannelGradeCurve(const GradeCurve curve) noexcept {
    return gradeCurveIndex(curve) < kGradeChannelCount;
}
// The output that changes nothing on a hue or Luma vs Saturation curve.
[[nodiscard]] constexpr double neutralGradeCurveOutput(const GradeCurve curve) noexcept {
    return curve == GradeCurve::HueVsHue ? 0.0 : 1.0;
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
// Schema 1 only (migration): `point` counts from zero; the schema key and role count from one.
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
    return static_cast<double>(point) / static_cast<double>(kLegacyCurvePointCount - 1);
}

} // namespace bloom::document
