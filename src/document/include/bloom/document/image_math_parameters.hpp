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
// #349 Image Statistics (`bloom.image-statistics`) takes an Image on `input` and gives five Scalars
// that drive other nodes (a Levels or a Map Range, for instance, to normalize a depth pass): the
// `minimum`, `maximum`, `mean` and `deviation` (population standard deviation) of one value of every
// counted pixel of the input's data window, and the `count` of pixels counted. `channel` is the value
// measured (the luminance, Rec. 709 weights on the color, one color channel, or alpha), `space` how
// premultiplication is treated (the color is divided by alpha, and a pixel without alpha has no color
// value, or the stored values are measured as they are) and `pixels` whether every pixel counts or
// only those with alpha above zero. A value that is not a finite number (NaN or an infinity, as a
// data pass carries) is never counted. An image with no counted pixel gives 0 for every statistic
// and a `count` of 0. The measure does not depend on a region of interest: the whole data window is
// counted. The statistics of a frame are those of that frame's own pixels; a frame whose statistics
// are not ready is reported as not ready and never takes another frame's.
// docs/architecture/image-statistics.md is the specification.
//
// #347 Audio Amplitude, Audio Waveform and Audio Spectrum take the sound of an Audio or Video
// source on their `audio` input (any audio effects between them are not applied: an analysis reads
// the source's own sound) and read a window of it around the frame, so a time on the timeline is the
// same sound in preview and in export.
//   Audio Amplitude (`bloom.audio-amplitude`) gives the Scalar `amplitude`: the `measure` (the Peak
//             or the RMS) of every channel's samples over `window` milliseconds centred on the frame
//             plus `offset` seconds, as a linear level or in decibels (`unit`; silence is -120 dB).
//   Audio Waveform (`bloom.audio-waveform`) gives the `width` by `height` Image `image`: the
//             minimum and maximum of `window` seconds of audio centred on the frame in each pixel
//             column, drawn in `plotColor` over `background`, scaled by `verticalScale`. `channels` mixes the
//             channels into one trace or stacks a lane for each channel (up to 8).
//   Audio Spectrum (`bloom.audio-spectrum`) gives the `width` by `height` Image `image`: the
//             magnitude spectrum of `transformSize` frames centred on the frame, from `lowFrequency` to
//             `highFrequency` on a logarithmic or linear `axis`, drawn as bars of `plotColor` over
//             `background` between the `floor` and `ceiling` decibels.
// docs/architecture/media-io.md, "Audio analysis", is the specification.
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
inline constexpr std::string_view kImageStatisticsNodeType = "bloom.image-statistics";
inline constexpr std::string_view kAudioAmplitudeNodeType = "bloom.audio-amplitude";
inline constexpr std::string_view kAudioWaveformNodeType = "bloom.audio-waveform";
inline constexpr std::string_view kAudioSpectrumNodeType = "bloom.audio-spectrum";
inline constexpr std::uint32_t kImagePixelNodeSchemaVersion = 1;

inline constexpr std::string_view kAudioAnalysisInputPort = "audio";
inline constexpr std::string_view kAudioAmplitudeOutputPort = "amplitude";

// Image Statistics' outputs, in order.
inline constexpr std::array<std::string_view, 5> kImageStatisticsOutputPorts{
    "minimum", "maximum", "mean", "deviation", "count"};

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

// Stored values of Image Statistics' choices.
inline constexpr std::int64_t kStatisticsLuminance = 0;
inline constexpr std::int64_t kStatisticsRed = 1;
inline constexpr std::int64_t kStatisticsGreen = 2;
inline constexpr std::int64_t kStatisticsBlue = 3;
inline constexpr std::int64_t kStatisticsAlpha = 4;
inline constexpr std::int64_t kStatisticsAllPixels = 0;
inline constexpr std::int64_t kStatisticsOpaqueOnly = 1;

// Stored values of the audio analysis nodes' choices.
inline constexpr std::int64_t kAudioMeasurePeak = 0;
inline constexpr std::int64_t kAudioMeasureRms = 1;
inline constexpr std::int64_t kAudioUnitLinear = 0;
inline constexpr std::int64_t kAudioUnitDecibels = 1;
inline constexpr std::int64_t kAudioWaveformMixed = 0;
inline constexpr std::int64_t kAudioWaveformSeparate = 1;
inline constexpr std::int64_t kAudioAxisLogarithmic = 0;
inline constexpr std::int64_t kAudioAxisLinear = 1;
// The transform lengths Audio Spectrum offers, in stored-value order.
inline constexpr std::array<std::int64_t, 4> kAudioSpectrumSizes{1024, 2048, 4096, 8192};
inline constexpr std::int64_t kAudioSpectrumDefaultSize = 1;
inline constexpr std::int64_t kAudioPlotMinimumSide = 16;
inline constexpr std::int64_t kAudioPlotMaximumSide = 4096;

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

inline constexpr std::array<std::string_view, 5> kStatisticsChannels{"Luminance", "Red", "Green",
                                                                      "Blue", "Alpha"};
inline constexpr std::array<std::string_view, 2> kStatisticsPixels{"All pixels",
                                                                    "Pixels with alpha"};

inline constexpr std::array<std::string_view, 2> kAudioMeasures{"Peak", "RMS"};
inline constexpr std::array<std::string_view, 2> kAudioUnits{"Linear", "Decibels"};
inline constexpr std::array<std::string_view, 2> kAudioWaveformChannels{"Mixed", "Separate lanes"};
inline constexpr std::array<std::string_view, 2> kAudioAxes{"Logarithmic", "Linear"};
inline constexpr std::array<std::string_view, 4> kAudioSpectrumSizeLabels{"1024", "2048", "4096",
                                                                          "8192"};

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

inline constexpr std::array kImageStatisticsParameters{
    choice("channel", kStatisticsChannels, kStatisticsLuminance, "Measure"),
    choice("space", kSpaces, kImagePixelStraight, "Operate on"),
    choice("pixels", kStatisticsPixels, kStatisticsAllPixels, "Pixels counted"),
};

inline constexpr std::array kAudioAmplitudeParameters{
    choice("measure", kAudioMeasures, kAudioMeasureRms, "Measure"),
    choice("unit", kAudioUnits, kAudioUnitLinear, "Unit"),
    scalar("window", 50.0, 1.0, 60000.0, "Window", 1.0, 1000.0, 1.0, 0, "ms"),
    scalar("offset", 0.0, -86400.0, 86400.0, "Time offset", -5.0, 5.0, 0.01, 3, "s"),
};

inline constexpr std::array kAudioWaveformParameters{
    integer("width", 1024, kAudioPlotMinimumSide, kAudioPlotMaximumSide, "Width", "px"),
    integer("height", 256, kAudioPlotMinimumSide, kAudioPlotMaximumSide, "Height", "px"),
    scalar("window", 2.0, 0.01, 60.0, "Window", 0.05, 10.0, 0.05, 2, "s"),
    choice("channels", kAudioWaveformChannels, kAudioWaveformMixed, "Channels"),
    scalar("verticalScale", 1.0, 0.0, 64.0, "Vertical scale", 0.0, 4.0, 0.05, 2),
    color("plotColor", {0.1, 0.8, 0.35, 1.0}, "Waveform color"),
    color("background", {0.015, 0.015, 0.02, 1.0}, "Background"),
};

inline constexpr std::array kAudioSpectrumParameters{
    integer("width", 1024, kAudioPlotMinimumSide, kAudioPlotMaximumSide, "Width", "px"),
    integer("height", 256, kAudioPlotMinimumSide, kAudioPlotMaximumSide, "Height", "px"),
    choice("transformSize", kAudioSpectrumSizeLabels, kAudioSpectrumDefaultSize, "Transform size"),
    choice("axis", kAudioAxes, kAudioAxisLogarithmic, "Frequency axis"),
    scalar("lowFrequency", 20.0, 1.0, 96000.0, "Lowest frequency", 20.0, 2000.0, 1.0, 0, "Hz"),
    scalar("highFrequency", 20000.0, 20.0, 192000.0, "Highest frequency", 1000.0, 24000.0, 10.0, 0,
           "Hz"),
    scalar("floor", -90.0, -200.0, 0.0, "Floor", -120.0, 0.0, 1.0, 0, "dB"),
    scalar("ceiling", 0.0, -100.0, 60.0, "Ceiling", -60.0, 12.0, 1.0, 0, "dB"),
    color("plotColor", {0.25, 0.65, 1.0, 1.0}, "Bar color"),
    color("background", {0.015, 0.015, 0.02, 1.0}, "Background"),
};

} // namespace image_pixel_parameters

// The nodes that read a sound on their `audio` input (#347, #630).
[[nodiscard]] constexpr bool isAudioAnalysisNodeType(const std::string_view typeId) noexcept {
    return typeId == kAudioAmplitudeNodeType || typeId == kAudioWaveformNodeType ||
           typeId == kAudioSpectrumNodeType;
}

[[nodiscard]] constexpr bool isImagePixelNodeType(const std::string_view typeId) noexcept {
    return typeId == kImageMathNodeType || typeId == kImageMapRangeNodeType ||
           typeId == kImageClampNodeType || typeId == kImageCompareNodeType ||
           typeId == kHistogramNodeType || typeId == kImageStatisticsNodeType ||
           typeId == kAudioAmplitudeNodeType ||
           typeId == kAudioWaveformNodeType || typeId == kAudioSpectrumNodeType;
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
    if (typeId == kImageStatisticsNodeType)
        return image_pixel_parameters::kImageStatisticsParameters;
    if (typeId == kAudioAmplitudeNodeType)
        return image_pixel_parameters::kAudioAmplitudeParameters;
    if (typeId == kAudioWaveformNodeType)
        return image_pixel_parameters::kAudioWaveformParameters;
    if (typeId == kAudioSpectrumNodeType)
        return image_pixel_parameters::kAudioSpectrumParameters;
    return {};
}

// The spec of a "<type id>.<role>" schema key, or null.
[[nodiscard]] constexpr const DepthParameterSpec*
imagePixelParameterSpec(const std::string_view schemaKey) noexcept {
    for (const auto type : {kImageMathNodeType, kImageMapRangeNodeType, kImageClampNodeType,
                            kImageCompareNodeType, kHistogramNodeType, kImageStatisticsNodeType,
                            kAudioAmplitudeNodeType,
                            kAudioWaveformNodeType, kAudioSpectrumNodeType}) {
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
// A constant (Choice, Integer, Boolean): no socket and no animation. A Scalar is animatable, drivable
// and has a socket; a Color is animatable (keyframes) and has no socket, so its row reads as one swatch.
[[nodiscard]] constexpr bool isImagePixelConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = imagePixelParameterSpec(key);
    return spec != nullptr && spec->kind != DepthParameterKind::Scalar &&
           spec->kind != DepthParameterKind::Color;
}
[[nodiscard]] constexpr bool isImagePixelColorSchemaKey(const std::string_view key) noexcept {
    const auto* spec = imagePixelParameterSpec(key);
    return spec != nullptr && spec->kind == DepthParameterKind::Color;
}
[[nodiscard]] constexpr bool isImagePixelScalarWithinDomain(const std::string_view key,
                                                            const double value) noexcept {
    const auto* spec = imagePixelParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
