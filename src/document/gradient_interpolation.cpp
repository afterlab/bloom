#include <bloom/document/gradient_interpolation.hpp>

#include <array>
#include <utility>

namespace bloom::document {
namespace {

template <typename Enum, std::size_t Count>
[[nodiscard]] std::optional<Enum> fromName(const std::array<Enum, Count>& values,
                                           std::string_view (*name)(Enum) noexcept,
                                           const std::string_view text) noexcept {
    for (const auto value : values)
        if (name(value) == text)
            return value;
    return std::nullopt;
}

} // namespace

std::string_view gradientInterpolationName(const GradientInterpolation value) noexcept {
    switch (value) {
    case GradientInterpolation::Linear:
        return "linear";
    case GradientInterpolation::Constant:
        return "constant";
    case GradientInterpolation::Ease:
        return "ease";
    case GradientInterpolation::Cardinal:
        return "cardinal";
    case GradientInterpolation::BSpline:
        return "b-spline";
    case GradientInterpolation::Exponential:
        return "exponential";
    }
    return {};
}

std::optional<GradientInterpolation>
gradientInterpolationFromName(const std::string_view name) noexcept {
    return fromName(kGradientInterpolations, &gradientInterpolationName, name);
}

std::string_view gradientColorModeName(const GradientColorMode value) noexcept {
    switch (value) {
    case GradientColorMode::Rgb:
        return "rgb";
    case GradientColorMode::Hsv:
        return "hsv";
    case GradientColorMode::Hsl:
        return "hsl";
    }
    return {};
}

std::optional<GradientColorMode> gradientColorModeFromName(const std::string_view name) noexcept {
    return fromName(kGradientColorModes, &gradientColorModeName, name);
}

std::string_view gradientHueDirectionName(const GradientHueDirection value) noexcept {
    switch (value) {
    case GradientHueDirection::Near:
        return "near";
    case GradientHueDirection::Far:
        return "far";
    case GradientHueDirection::Clockwise:
        return "clockwise";
    case GradientHueDirection::CounterClockwise:
        return "counter-clockwise";
    }
    return {};
}

std::optional<GradientHueDirection>
gradientHueDirectionFromName(const std::string_view name) noexcept {
    return fromName(kGradientHueDirections, &gradientHueDirectionName, name);
}

std::string_view gradientInterpolationLabel(const GradientInterpolation value) noexcept {
    switch (value) {
    case GradientInterpolation::Linear:
        return "Linear";
    case GradientInterpolation::Constant:
        return "Constant";
    case GradientInterpolation::Ease:
        return "Ease";
    case GradientInterpolation::Cardinal:
        return "Cardinal";
    case GradientInterpolation::BSpline:
        return "B-Spline";
    case GradientInterpolation::Exponential:
        return "Exponential";
    }
    return {};
}

std::string_view gradientColorModeLabel(const GradientColorMode value) noexcept {
    switch (value) {
    case GradientColorMode::Rgb:
        return "RGB";
    case GradientColorMode::Hsv:
        return "HSV";
    case GradientColorMode::Hsl:
        return "HSL";
    }
    return {};
}

std::string_view gradientHueDirectionLabel(const GradientHueDirection value) noexcept {
    switch (value) {
    case GradientHueDirection::Near:
        return "Near";
    case GradientHueDirection::Far:
        return "Far";
    case GradientHueDirection::Clockwise:
        return "Clockwise";
    case GradientHueDirection::CounterClockwise:
        return "Counter-clockwise";
    }
    return {};
}

} // namespace bloom::document
