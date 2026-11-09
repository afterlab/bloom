#pragma once

// #481 Image Math, Image Map Range and Image Clamp: the durable vocabulary of the three Channels
// nodes that do arithmetic on pixels, so render passes (mist, Z-depth, normals, motion vectors, IDs)
// can be normalized and limited as well as a single value can. Each takes an Image on `input` and
// gives an Image on `image`; Image Math also takes an optional Image on `second`.
//
//   Image Math       `operation` of the input and an operand, per pixel: Add, Subtract, Multiply,
//                    Divide, Power, Minimum, Maximum (the operand is the `value`, or the `second`
//                    image's pixel at the same position when that input is connected, and
//                    transparent black where it has no pixels) and Absolute (no operand).
//   Image Map Range  maps `inputMin`..`inputMax` linearly onto `outputMin`..`outputMax`, any
//                    finite floats in either direction; `clamp` (off) limits the result to the
//                    output range. An empty input range maps every value to `outputMin`.
//   Image Clamp      limits each channel to its own minimum and maximum (the alpha channel to
//                    [0, 1]); a minimum above the maximum is the maximum.
//
// #349 Image Compare (`bloom.image-compare`) takes two Images, `input` (A) and `second` (B), and gives
// the Image `image` over A's data window, with B transparent black where it has no pixels. `mode` is
// Difference (|A - B| of the stored values times `gain`, with the larger alpha), Split (A on one side
// of a line, B on the other: `splitPosition` runs the line across the display window from B's side to A's
// and `angle` turns it) or Checkerboard (A and B alternate in squares of `cellSize` pixels from the
// display window's corner). Split and Checkerboard are laid on the display window, so the same pixel
// shows the same image whatever region or data window is rendered. A result that is not finite is 0.
//
// #349 Histogram (`bloom.histogram`) takes an Image on `input` and gives an Image on `image`: a
// `bins` by `height` bar graph of how many pixels of the input's data window have each value, in
// `bins` equal bins of the range `rangeMin`..`rangeMax`. `channels` is the value counted (the red,
// green and blue channels in their own colors with the luminance, Rec. 709 weights on the color,
// beside them, the luminance alone, one color channel or alpha), `space` how premultiplication is
// treated (the color is divided by alpha, and a pixel without alpha counts nowhere in a color
// channel, or the stored values are counted as they are), and `graphScale` the bars' height against the
// tallest bin: linear, or logarithmic in one plus the count. A value outside the range or not
// finite is not counted. The graph does not depend on a region of interest: the whole data window
// is counted.
//
// Every node declares which channels it changes and how premultiplication is treated:
//   channels  Color (red, green and blue; the default and alpha is passed through), Red, Green,
//             Blue, Alpha, or Color and alpha. A channel that is not changed keeps its stored value.
//   operate on  Straight color (the default): red, green and blue are divided by alpha, changed and
//             multiplied by the result's alpha, so a soft edge keeps its color and a change to
//             alpha re-weights the color; or Stored values: the stored (premultiplied) values are
//             changed as they are, which is what a data pass wants, whose alpha is one.
// A result that is not finite (a division by zero, a negative number raised to a fractional power,
// an overflow, or a non-finite input) is 0. Alpha is always limited to [0, 1].
// docs/architecture/image-filters.md, "Image Math, Map Range and Clamp", is the specification.
//
// Schema keys are "<type id>.<role>", as the depth effects'. Every Scalar is animatable, drivable
// and has a socket of its own; every Choice and Boolean is a constant with no socket.

#include <bloom/document/depth_effect_parameters.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kImageMathNodeType = "bloom.image-math";
inline constexpr std::string_view kImageMapRangeNodeType = "bloom.image-map-range";
inline constexpr std::string_view kImageClampNodeType = "bloom.image-clamp";
inline constexpr std::string_view kImageCompareNodeType = "bloom.image-compare";
inline constexpr std::string_view kHistogramNodeType = "bloom.histogram";
inline constexpr std::uint32_t kImagePixelNodeSchemaVersion = 1;

inline constexpr std::string_view kImagePixelInputPort = "input";
inline constexpr std::string_view kImagePixelSecondPort = "second";
inline constexpr std::string_view kImagePixelOutputPort = "image";

// Stored values of Image Math's operation, in the order its selector offers them.
inline constexpr std::int64_t kImageMathAdd = 0;
inline constexpr std::int64_t kImageMathSubtract = 1;
inline constexpr std::int64_t kImageMathMultiply = 2;
inline constexpr std::int64_t kImageMathDivide = 3;
inline constexpr std::int64_t kImageMathPower = 4;
inline constexpr std::int64_t kImageMathMinimum = 5;
inline constexpr std::int64_t kImageMathMaximum = 6;
inline constexpr std::int64_t kImageMathAbsolute = 7;

// Stored values of Image Compare's mode.
inline constexpr std::int64_t kImageCompareDifference = 0;
inline constexpr std::int64_t kImageCompareSplit = 1;
inline constexpr std::int64_t kImageCompareCheckerboard = 2;
inline constexpr double kMaximumImageCompareCell = 4096.0;

// Stored values of Histogram's channels, scale, bins and height.
inline constexpr std::int64_t kHistogramRgb = 0;
inline constexpr std::int64_t kHistogramLuminance = 1;
inline constexpr std::int64_t kHistogramRgbLuminance = 2;
inline constexpr std::int64_t kHistogramRed = 3;
inline constexpr std::int64_t kHistogramGreen = 4;
inline constexpr std::int64_t kHistogramBlue = 5;
inline constexpr std::int64_t kHistogramAlpha = 6;
inline constexpr std::int64_t kHistogramLinear = 0;
inline constexpr std::int64_t kHistogramLogarithmic = 1;
// The bin counts and graph heights the node offers, in stored-value order.
inline constexpr std::array<std::int64_t, 4> kHistogramBins{64, 128, 256, 512};
inline constexpr std::array<std::int64_t, 4> kHistogramHeights{64, 128, 256, 512};
inline constexpr std::int64_t kHistogramDefaultBins = 2;
inline constexpr std::int64_t kHistogramDefaultHeight = 1;

// Stored values of the channels each node changes.
inline constexpr std::int64_t kImagePixelColor = 0;
inline constexpr std::int64_t kImagePixelRed = 1;
inline constexpr std::int64_t kImagePixelGreen = 2;
inline constexpr std::int64_t kImagePixelBlue = 3;
inline constexpr std::int64_t kImagePixelAlpha = 4;
inline constexpr std::int64_t kImagePixelColorAndAlpha = 5;

// Stored values of how premultiplication is treated.
inline constexpr std::int64_t kImagePixelStraight = 0;
inline constexpr std::int64_t kImagePixelStored = 1;

// A value, an input or output range limit or a clamp limit is finite and within this.
inline constexpr double kMaximumImagePixelValue = 1.0e6;

namespace image_pixel_parameters {

using namespace depth_parameters;

inline constexpr std::array<std::string_view, 8> kOperations{
    "Add", "Subtract", "Multiply", "Divide", "Power", "Minimum", "Maximum", "Absolute"};
inline constexpr std::array<std::string_view, 6> kChannels{
    "Color (RGB)", "Red", "Green", "Blue", "Alpha", "Color and alpha"};
inline constexpr std::array<std::string_view, 2> kSpaces{"Straight color", "Stored values"};

inline constexpr std::array<std::string_view, 3> kCompareModes{"Difference", "Split",
                                                                 "Checkerboard"};

inline constexpr std::array<std::string_view, 7> kHistogramChannels{
    "Red, green and blue", "Luminance", "Red, green, blue and luminance", "Red",
    "Green",               "Blue",      "Alpha"};
inline constexpr std::array<std::string_view, 4> kHistogramBinLabels{"64", "128", "256", "512"};
inline constexpr std::array<std::string_view, 4> kHistogramHeightLabels{"64", "128", "256", "512"};
inline constexpr std::array<std::string_view, 2> kHistogramScales{"Linear", "Logarithmic"};

inline constexpr double kV = kMaximumImagePixelValue;

inline constexpr std::array kImageMathParameters{
    choice("operation", kOperations, kImageMathMultiply, "Operation"),
    scalar("value", 1.0, -kV, kV, "Value", -10.0, 10.0, 0.01, 3),
    choice("channels", kChannels, kImagePixelColor, "Channels"),
    choice("space", kSpaces, kImagePixelStraight, "Operate on"),
};

inline constexpr std::array kImageMapRangeParameters{
    scalar("inputMin", 0.0, -kV, kV, "Input minimum", -10.0, 10.0, 0.01, 3),
    scalar("inputMax", 1.0, -kV, kV, "Input maximum", -10.0, 10.0, 0.01, 3),
    scalar("outputMin", 0.0, -kV, kV, "Output minimum", -10.0, 10.0, 0.01, 3),
    scalar("outputMax", 1.0, -kV, kV, "Output maximum", -10.0, 10.0, 0.01, 3),
    boolean("clamp", "Clamp to output range"),
    choice("channels", kChannels, kImagePixelColor, "Channels"),
    choice("space", kSpaces, kImagePixelStraight, "Operate on"),
};

inline constexpr std::array kImageClampParameters{
    scalar("minRed", 0.0, -kV, kV, "Red minimum", -10.0, 10.0, 0.01, 3),
    scalar("maxRed", 1.0, -kV, kV, "Red maximum", -10.0, 10.0, 0.01, 3),
    scalar("minGreen", 0.0, -kV, kV, "Green minimum", -10.0, 10.0, 0.01, 3),
    scalar("maxGreen", 1.0, -kV, kV, "Green maximum", -10.0, 10.0, 0.01, 3),
    scalar("minBlue", 0.0, -kV, kV, "Blue minimum", -10.0, 10.0, 0.01, 3),
    scalar("maxBlue", 1.0, -kV, kV, "Blue maximum", -10.0, 10.0, 0.01, 3),
    scalar("minAlpha", 0.0, 0.0, 1.0, "Alpha minimum", 0.0, 1.0, 0.01, 3),
    scalar("maxAlpha", 1.0, 0.0, 1.0, "Alpha maximum", 0.0, 1.0, 0.01, 3),
    choice("space", kSpaces, kImagePixelStraight, "Operate on"),
};

inline constexpr std::array kImageCompareParameters{
    choice("mode", kCompareModes, kImageCompareDifference, "Mode"),
    scalar("gain", 1.0, 0.0, kV, "Difference gain", 0.0, 16.0, 0.1, 2),
    scalar("splitPosition", 0.5, 0.0, 1.0, "Split position", 0.0, 1.0, 0.01, 3),
    scalar("angle", 0.0, -360.0, 360.0, "Split angle", -180.0, 180.0, 1.0, 1, "°"),
    scalar("cellSize", 64.0, 1.0, kMaximumImageCompareCell, "Cell size", 1.0, 512.0, 1.0, 0, "px"),
};

inline constexpr std::array kHistogramParameters{
    choice("channels", kHistogramChannels, kHistogramRgb, "Channels"),
    choice("space", kSpaces, kImagePixelStraight, "Operate on"),
    scalar("rangeMin", 0.0, -kV, kV, "Range minimum", -10.0, 10.0, 0.01, 3),
    scalar("rangeMax", 1.0, -kV, kV, "Range maximum", -10.0, 10.0, 0.01, 3),
    choice("bins", kHistogramBinLabels, kHistogramDefaultBins, "Bins"),
    choice("height", kHistogramHeightLabels, kHistogramDefaultHeight, "Graph height"),
    choice("graphScale", kHistogramScales, kHistogramLinear, "Scale"),
};

} // namespace image_pixel_parameters

[[nodiscard]] constexpr bool isImagePixelNodeType(const std::string_view typeId) noexcept {
    return typeId == kImageMathNodeType || typeId == kImageMapRangeNodeType ||
           typeId == kImageClampNodeType || typeId == kImageCompareNodeType ||
           typeId == kHistogramNodeType;
}

// The parameters of an image pixel node in definition order, or an empty span.
[[nodiscard]] constexpr std::span<const DepthParameterSpec>
imagePixelParameters(const std::string_view typeId) noexcept {
    if (typeId == kImageMathNodeType)
        return image_pixel_parameters::kImageMathParameters;
    if (typeId == kImageMapRangeNodeType)
        return image_pixel_parameters::kImageMapRangeParameters;
    if (typeId == kImageClampNodeType)
        return image_pixel_parameters::kImageClampParameters;
    if (typeId == kImageCompareNodeType)
        return image_pixel_parameters::kImageCompareParameters;
    if (typeId == kHistogramNodeType)
        return image_pixel_parameters::kHistogramParameters;
    return {};
}

// The spec of a "<type id>.<role>" schema key, or null.
[[nodiscard]] constexpr const DepthParameterSpec*
imagePixelParameterSpec(const std::string_view schemaKey) noexcept {
    for (const auto type : {kImageMathNodeType, kImageMapRangeNodeType, kImageClampNodeType,
                            kImageCompareNodeType, kHistogramNodeType}) {
        if (schemaKey.size() <= type.size() + 1 || !schemaKey.starts_with(type) ||
            schemaKey[type.size()] != '.')
            continue;
        const auto role = schemaKey.substr(type.size() + 1);
        for (const auto& spec : imagePixelParameters(type))
            if (spec.role == role)
                return &spec;
    }
    return nullptr;
}

[[nodiscard]] constexpr bool isImagePixelScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = imagePixelParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Scalar;
}
// A constant (Choice, Boolean): no socket and no animation.
[[nodiscard]] constexpr bool isImagePixelConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = imagePixelParameterSpec(key);
    return spec != nullptr && spec->kind != DepthParameterKind::Scalar;
}
[[nodiscard]] constexpr bool isImagePixelScalarWithinDomain(const std::string_view key,
                                                            const double value) noexcept {
    const auto* spec = imagePixelParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
