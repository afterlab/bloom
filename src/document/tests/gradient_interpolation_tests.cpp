// #546: the gradient interpolation, colour and hue modes against an independent port of the
// reference (Blender's BKE_colorband_evaluate) in binary64: every interpolation in RGB before the
// first stop, inside a segment and after the last stop, HSV and HSL in every hue direction, and the
// names and validity of the modes.

#include <bloom/document/gradient_interpolation.hpp>
#include <bloom/document/parameter.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

struct Expected final {
    std::string_view interpolation;
    std::string_view colorMode;
    std::string_view hue;
    double t;
    std::array<double, 4> color;
};

[[nodiscard]] GradientValue withModes(GradientValue value, const std::string_view interpolation,
                                      const std::string_view colorMode,
                                      const std::string_view hue) {
    value.modes = {*gradientInterpolationFromName(interpolation),
                   *gradientColorModeFromName(colorMode), *gradientHueDirectionFromName(hue)};
    return value;
}

void expectColor(const GradientValue& value, const Expected& expected) {
    const auto color = gradientColorAt(value, expected.t);
    const std::array actual{color.red, color.green, color.blue, color.alpha};
    bool close = true;
    for (std::size_t i = 0; i < 4; ++i)
        close = close && std::abs(actual[i] - expected.color[i]) <= 1e-7;
    expect(close, std::string(expected.interpolation) + " " + std::string(expected.colorMode) +
                      " " + std::string(expected.hue) + " at " + std::to_string(expected.t) +
                      " matches the reference: got " + std::to_string(actual[0]) + ", " +
                      std::to_string(actual[1]) + ", " + std::to_string(actual[2]) + ", " +
                      std::to_string(actual[3]));
}

void everyModeMatchesTheReference() {
    // Stored out of order: the reader sorts them.
    const GradientValue three{
        {{0.5, {0.0, 1.0, 0.0, 0.5}}, {0.2, {1.0, 0.0, 0.0, 1.0}}, {0.9, {0.2, 0.4, 1.0, 1.0}}}};
    static constexpr std::array<Expected, 41> table{{
        {"constant", "rgb", "near", 0.1, {1, 0, 0, 1}},
        {"constant", "rgb", "near", 0.275, {1, 0, 0, 1}},
        {"constant", "rgb", "near", 0.7, {0, 1, 0, 0.5}},
        {"constant", "rgb", "near", 0.95, {0.2, 0.4, 1, 1}},
        {"linear", "rgb", "near", 0.1, {1, 0, 0, 1}},
        {"linear", "rgb", "near", 0.275, {0.75, 0.25, 0, 0.875}},
        {"linear", "rgb", "near", 0.7, {0.1, 0.7, 0.5, 0.75}},
        {"linear", "rgb", "near", 0.95, {0.2, 0.4, 1, 1}},
        {"ease", "rgb", "near", 0.1, {1, 0, 0, 1}},
        {"ease", "rgb", "near", 0.275, {0.84375, 0.15625, 0, 0.921875}},
        {"ease", "rgb", "near", 0.7, {0.1, 0.7, 0.5, 0.75}},
        {"ease", "rgb", "near", 0.95, {0.2, 0.4, 1, 1}},
        // The spline modes do not hold past the end stops, and Cardinal overshoots.
        {"cardinal", "rgb", "near", 0.1, {1.08875, -0.08875, 0, 1}},
        {"cardinal", "rgb", "near", 0.275, {0.77053125, 0.24278125, -0.03328125, 0.871953125}},
        {"cardinal", "rgb", "near", 0.7, {0.01125, 0.78875, 0.5, 0.705625}},
        {"cardinal", "rgb", "near", 0.95, {0.21775, 0.34675, 1.08875, 1}},
        {"b-spline", "rgb", "near", 0.1, {0.9791666525, 0.0208333275, 0, 0.98958331625}},
        {"b-spline",
         "rgb",
         "near",
         0.275,
         {0.68281248975, 0.316145825125, 0.0026041628125, 0.8424479}},
        {"b-spline", "rgb", "near", 0.7, {0.12083333, 0.679166655, 0.4999999875, 0.76041665}},
        {"b-spline",
         "rgb",
         "near",
         0.95,
         {0.1958333295, 0.4124999915, 0.9791666475, 0.98958331375}},
        {"exponential", "rgb", "near", 0.1, {1, 0, 0, 1}},
        {"exponential",
         "rgb",
         "near",
         0.275,
         {0.99544784530841413, 0.0045521546915858746, 0, 0.99772392265420706}},
        {"exponential",
         "rgb",
         "near",
         0.7,
         {0.0060606060606060554, 0.98181818181818181, 0.030303030303030276, 0.51515151515151514}},
        {"exponential", "rgb", "near", 0.95, {0.2, 0.4, 1, 1}},
        {"linear", "hsv", "near", 0.275, {1, 0.5, 0, 0.875}},
        {"linear", "hsv", "near", 0.7, {0.1, 1, 0.8875, 0.75}},
        {"linear", "hsv", "far", 0.275, {1, 0, 1, 0.875}},
        {"linear", "hsv", "far", 0.7, {1, 0.1, 0.2125, 0.75}},
        {"linear", "hsv", "clockwise", 0.275, {1, 0.5, 0, 0.875}},
        {"linear", "hsv", "clockwise", 0.7, {0.1, 1, 0.8875, 0.75}},
        {"linear", "hsv", "counter-clockwise", 0.275, {1, 0, 1, 0.875}},
        {"linear", "hsv", "counter-clockwise", 0.7, {1, 0.1, 0.2125, 0.75}},
        {"linear", "hsl", "near", 0.275, {1, 0.5, 0, 0.875}},
        {"linear", "hsl", "near", 0.7, {0.1, 1, 0.8875, 0.75}},
        {"linear", "hsl", "far", 0.275, {1, 0, 1, 0.875}},
        {"linear", "hsl", "far", 0.7, {1, 0.1, 0.2125, 0.75}},
        {"linear", "hsl", "clockwise", 0.275, {1, 0.5, 0, 0.875}},
        {"linear", "hsl", "clockwise", 0.7, {0.1, 1, 0.8875, 0.75}},
        {"linear", "hsl", "counter-clockwise", 0.275, {1, 0, 1, 0.875}},
        {"linear", "hsl", "counter-clockwise", 0.7, {1, 0.1, 0.2125, 0.75}},
        // HSV and HSL blend linearly whatever the interpolation, as the reference does.
        {"constant", "hsv", "near", 0.275, {1, 0.5, 0, 0.875}},
    }};
    for (const auto& expected : table)
        expectColor(withModes(three, expected.interpolation, expected.colorMode, expected.hue),
                    expected);

    // Red to blue: the shorter way round is down through magenta (Counter-clockwise), the
    // longer one up through yellow (Clockwise).
    const GradientValue redBlue{{{0.0, {1.0, 0.0, 0.0, 1.0}}, {1.0, {0.0, 0.0, 1.0, 1.0}}}};
    static constexpr std::array<Expected, 8> hues{{
        {"linear", "hsv", "near", 0.25, {1, 0, 0.5, 1}},
        {"linear", "hsv", "far", 0.25, {1, 1, 0, 1}},
        {"linear", "hsv", "clockwise", 0.25, {1, 1, 0, 1}},
        {"linear", "hsv", "counter-clockwise", 0.25, {1, 0, 0.5, 1}},
        {"linear", "hsl", "near", 0.25, {1, 0, 0.5, 1}},
        {"linear", "hsl", "far", 0.25, {1, 1, 0, 1}},
        {"linear", "hsl", "clockwise", 0.25, {1, 1, 0, 1}},
        {"linear", "hsl", "counter-clockwise", 0.25, {1, 0, 0.5, 1}},
    }};
    for (const auto& expected : hues)
        expectColor(withModes(redBlue, expected.interpolation, expected.colorMode, expected.hue),
                    expected);
    // Far between two stops of one hue goes all the way round.
    const GradientValue sameHue{{{0.0, {0.8, 0.2, 0.2, 1.0}}, {1.0, {0.4, 0.1, 0.1, 1.0}}}};
    expectColor(withModes(sameHue, "linear", "hsv", "near"),
                {"linear", "hsv", "near", 0.5, {0.6, 0.15, 0.15, 1}});
    expectColor(withModes(sameHue, "linear", "hsv", "far"),
                {"linear", "hsv", "far", 0.5, {0.15, 0.6, 0.6, 1}});
    // One stop is one colour in every mode.
    const GradientValue one{{{0.3, {0.25, 0.5, 0.75, 0.5}}}};
    for (const auto interpolation : kGradientInterpolations)
        for (const auto colorMode : kGradientColorModes) {
            auto value = one;
            value.modes = {interpolation, colorMode, GradientHueDirection::Near};
            const auto color = gradientColorAt(value, 0.8);
            expect(color == one.stops.front().color,
                   "a one-stop gradient is its colour in every mode");
        }
}

void modesAreNamedAndValidated() {
    for (const auto value : kGradientInterpolations)
        expect(gradientInterpolationFromName(gradientInterpolationName(value)) == value &&
                   !gradientInterpolationLabel(value).empty(),
               "an interpolation's name reads back");
    for (const auto value : kGradientColorModes)
        expect(gradientColorModeFromName(gradientColorModeName(value)) == value &&
                   !gradientColorModeLabel(value).empty(),
               "a colour mode's name reads back");
    for (const auto value : kGradientHueDirections)
        expect(gradientHueDirectionFromName(gradientHueDirectionName(value)) == value &&
                   !gradientHueDirectionLabel(value).empty(),
               "a hue direction's name reads back");
    expect(gradientInterpolationName(GradientInterpolation::BSpline) == "b-spline" &&
               gradientHueDirectionName(GradientHueDirection::CounterClockwise) ==
                   "counter-clockwise" &&
               !gradientInterpolationFromName("Linear") && !gradientColorModeFromName("lab"),
           "the stored names are exact");
    GradientValue value{{{0.0, {0.0, 0.0, 0.0, 1.0}}}};
    expect(value.modes == GradientModes{} && value.modes.isLinearRgb() && value.isValid(),
           "a gradient defaults to Linear in RGB, Near");
    value.modes.interpolation = static_cast<GradientInterpolation>(6);
    expect(!value.isValid(), "an unknown interpolation is invalid");
    value.modes = {};
    value.modes.colorMode = static_cast<GradientColorMode>(3);
    expect(!value.isValid(), "an unknown colour mode is invalid");
    value.modes = {};
    value.modes.hueDirection = static_cast<GradientHueDirection>(4);
    expect(!value.isValid(), "an unknown hue direction is invalid");
    auto other = GradientValue{{{0.0, {0.0, 0.0, 0.0, 1.0}}}};
    other.modes.interpolation = GradientInterpolation::Ease;
    expect(!(other == GradientValue{{{0.0, {0.0, 0.0, 0.0, 1.0}}}}),
           "the modes are part of a gradient's value");
}

void binary32MatchesBinary64() {
    // The CPU reference's binary32 evaluation stays within a few ulps of the binary64 one.
    const GradientValue three{
        {{0.2, {1.0, 0.0, 0.0, 1.0}}, {0.5, {0.0, 1.0, 0.0, 0.5}}, {0.9, {0.2, 0.4, 1.0, 1.0}}}};
    const std::array<gradient_math::Stop<float>, 3> table{{
        {0.2F, 1.0F / (0.5F - 0.2F), {1.0F, 0.0F, 0.0F, 1.0F}},
        {0.5F, 1.0F / (0.9F - 0.5F), {0.0F, 1.0F, 0.0F, 0.5F}},
        {0.9F, 0.0F, {0.2F, 0.4F, 1.0F, 1.0F}},
    }};
    const auto at = [&](const std::size_t index) { return table[index]; };
    for (const auto interpolation : kGradientInterpolations)
        for (const auto colorMode : kGradientColorModes)
            for (const auto hue : kGradientHueDirections)
                for (int sample = 0; sample <= 40; ++sample) {
                    const double t = sample / 40.0;
                    auto value = three;
                    value.modes = {interpolation, colorMode, hue};
                    const auto wide = gradientColorAt(value, t);
                    const auto narrow =
                        gradient_math::sample<float>(3, at, 1.0F / 0.2F, 1.0F / (1.0F - 0.9F),
                                                     static_cast<float>(t), value.modes);
                    const std::array reference{wide.red, wide.green, wide.blue, wide.alpha};
                    bool close = true;
                    for (std::size_t i = 0; i < 4; ++i)
                        close = close &&
                                std::abs(static_cast<double>(narrow[i]) - reference[i]) <= 2e-6;
                    expect(close, std::string("binary32 ") +
                                      std::string(gradientInterpolationName(interpolation)) + " " +
                                      std::string(gradientColorModeName(colorMode)) + " " +
                                      std::string(gradientHueDirectionName(hue)) + " at " +
                                      std::to_string(t) + " is within 2e-6 of binary64");
                }
}
} // namespace

int main() {
    everyModeMatchesTheReference();
    modesAreNamedAndValidated();
    binary32MatchesBinary64();
    if (failures != 0) {
        std::cerr << failures << " gradient interpolation expectation(s) failed\n";
        return 1;
    }
    return 0;
}
