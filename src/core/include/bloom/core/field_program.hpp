#ifndef BLOOM_CORE_FIELD_PROGRAM_HPP
#define BLOOM_CORE_FIELD_PROGRAM_HPP

// #525 Fields: the procedural owner's field programs (docs/architecture/fields.md). A field is a
// lazy function of a field position -- composition pixels from the composition's centre, +x right,
// +y up -- compiled into a bounded program of at most 64 pure per-sample instructions over sixteen
// vec4 registers. The host prepares one table of binary32 values per program (this header encodes
// it); the CPU reference (evaluateField / fieldPixel) and the GPU program built around
// fieldKernelGlsl() interpret the identical table with the same binary32 operations, from one
// kernel source (src/core/field_kernel.glsl). Every per-frame quantity -- a rotation's cosine, a
// lattice offset split into whole cells and a fraction, an evolution phase from exact rational
// time, a seed's 32-bit key -- is resolved here in binary64 before it is narrowed into the table,
// so the device never evaluates a transcendental the CPU did not.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::core::field {

// Advances whenever any operation's arithmetic changes; part of every table (and so of every cache
// identity a table enters).
inline constexpr std::uint32_t kFieldProgramSemanticsVersion = 1;
inline constexpr float kFieldTableMagic = 525.0F;
inline constexpr std::size_t kMaxFieldInstructions = 64;
inline constexpr std::size_t kFieldRegisterCount = 16;
inline constexpr std::size_t kMaxFieldImages = 4;
inline constexpr std::size_t kMaxFieldSamples = 16;
inline constexpr std::size_t kMaxNoiseOctaves = 10;
inline constexpr std::size_t kMaxFieldCurveKnots = 16;
inline constexpr std::size_t kFieldRampEntries = 256;
// vec4 offsets of the table's fixed blocks (field_kernel.glsl, "The prepared table").
inline constexpr std::size_t kFieldTableHeader = 0;
inline constexpr std::size_t kFieldTableMapping = 1;
inline constexpr std::size_t kFieldTableTarget = 2;
inline constexpr std::size_t kFieldTableSampling = 3;
inline constexpr std::size_t kFieldTableSamples = 4;
inline constexpr std::size_t kFieldTableSlots = 12;
inline constexpr std::size_t kFieldTableCode = 24;
// The largest table a program may prepare (the GPU storage-buffer ceiling is 4 MiB).
inline constexpr std::size_t kMaxFieldTableFloats = 256 * 1024;

// The instruction vocabulary. Every value is a Required GPU coverage feature
// (runtime::gpuFeatureCoverage, feature.field.op.<name>).
enum class FieldOp : std::uint8_t {
    Coordinate = 0,
    Constant = 1,
    Noise = 2,
    Voronoi = 3,
    Random = 4,
    Pattern = 5,
    Gradient = 6,
    Shape = 7,
    Image = 8,
    Force = 9,
    Math = 10,
    Remap = 11,
    Transform = 12,
    Swizzle = 13,
    Combine = 14,
    Offset = 15,
    Derivative = 16,
    CellCentre = 17,
    Arrow = 18,
    Streak = 19,
    Ramp = 20,
    LensWarp = 21,
};
inline constexpr std::array kFieldOps{
    FieldOp::Coordinate, FieldOp::Constant, FieldOp::Noise,      FieldOp::Voronoi,
    FieldOp::Random,     FieldOp::Pattern,  FieldOp::Gradient,   FieldOp::Shape,
    FieldOp::Image,      FieldOp::Force,    FieldOp::Math,       FieldOp::Remap,
    FieldOp::Transform,  FieldOp::Swizzle,  FieldOp::Combine,    FieldOp::Offset,
    FieldOp::Derivative, FieldOp::CellCentre, FieldOp::Arrow,    FieldOp::Streak,
    FieldOp::Ramp,       FieldOp::LensWarp};

// Stable lower-case names (coverage ids, diagnostics). No default case: a new op is a -Wswitch
// error until it is named.
[[nodiscard]] constexpr std::string_view fieldOpName(const FieldOp op) noexcept {
    switch (op) {
    case FieldOp::Coordinate:
        return "coordinate";
    case FieldOp::Constant:
        return "constant";
    case FieldOp::Noise:
        return "noise";
    case FieldOp::Voronoi:
        return "voronoi";
    case FieldOp::Random:
        return "random";
    case FieldOp::Pattern:
        return "pattern";
    case FieldOp::Gradient:
        return "gradient";
    case FieldOp::Shape:
        return "shape";
    case FieldOp::Image:
        return "image";
    case FieldOp::Force:
        return "force";
    case FieldOp::Math:
        return "math";
    case FieldOp::Remap:
        return "remap";
    case FieldOp::Transform:
        return "transform";
    case FieldOp::Swizzle:
        return "swizzle";
    case FieldOp::Combine:
        return "combine";
    case FieldOp::Offset:
        return "offset";
    case FieldOp::Derivative:
        return "derivative";
    case FieldOp::CellCentre:
        return "cell-centre";
    case FieldOp::Arrow:
        return "arrow";
    case FieldOp::Streak:
        return "streak";
    case FieldOp::Ramp:
        return "ramp";
    case FieldOp::LensWarp:
        return "lens-warp";
    }
    return {};
}

// What the result register holds, and how a raster turns it into premultiplied RGBA.
// Premultiplied: the register is already premultiplied RGBA and a raster keeps it whole.
enum class FieldResultKind : std::uint8_t { Scalar = 0, Vector = 1, Color = 2, Premultiplied = 3 };
enum class FieldOutputMode : std::uint8_t { Raw = 0, Mask = 1, Color = 2 };

// --- Operation parameters (binary64 authoring values; the encoders resolve and narrow them) -------

enum class NoiseType : std::uint8_t { Perlin, Simplex, Linear, SoftLinear, Block, Spline };
enum class NoiseFractal : std::uint8_t {
    None,
    Fbm,
    Turbulence,
    Ridged,
    Billow,
    Dynamic,
    DynamicProgressive,
    DynamicTwist
};
enum class NoiseOverflow : std::uint8_t { Clip, SoftClamp, WrapBack, AllowHdr };

struct NoiseParameters final {
    NoiseType type = NoiseType::Perlin;
    NoiseFractal fractal = NoiseFractal::Fbm;
    std::int64_t seed = 0;
    // A salt folded into the seed's key so two noises of one node (the vector output's second
    // component) are decorrelated.
    std::uint32_t stream = 0;
    double sizeX = 200.0;
    double sizeY = 200.0;
    double offsetX = 0.0;
    double offsetY = 0.0;
    double rotationDegrees = 0.0;
    double octaves = 6.0;
    double lacunarity = 2.0;
    double gain = 0.5;
    double octaveRotationDegrees = 0.0;
    double octaveOffsetX = 0.0;
    double octaveOffsetY = 0.0;
    double ridgeOffset = 1.0;
    double ridgeGain = 2.0;
    // The evolution phase in lattice periods (already from time when looping, NoiseEvolution).
    double evolution = 0.0;
    bool flow = false;
    // Whole evolution periods after which the noise repeats (0: none).
    std::int64_t loopPeriod = 0;
    double contrast = 1.0;
    double brightness = 0.0;
    bool invert = false;
    NoiseOverflow overflow = NoiseOverflow::AllowHdr;
    bool bipolar = false;
};

// The evolution phase a Noise samples at `seconds`: the authored evolution, or with a loop the
// whole number of cycles `max(1, round(evolution))` advanced over each `loopSeconds`, so the
// sequence repeats seamlessly. `period` receives the loop's whole periods (0 without a loop).
[[nodiscard]] double noiseEvolutionPhase(double evolution, bool loop, double loopSeconds,
                                         double seconds, std::int64_t& period) noexcept;

enum class VoronoiMode : std::uint8_t { F1, F2, F2MinusF1, Edge, SmoothF1 };
enum class VoronoiMetric : std::uint8_t { Euclidean, Manhattan, Chebyshev };
struct VoronoiParameters final {
    VoronoiMode mode = VoronoiMode::F1;
    VoronoiMetric metric = VoronoiMetric::Euclidean;
    std::int64_t seed = 0;
    double size = 100.0;
    double jitter = 1.0;
    double smoothness = 0.0; // field pixels
    double evolution = 0.0;  // feature-point orbit turns
    std::int64_t octaves = 1;
    // 0: [distance, cell id, edge distance, 0]; 1: [feature x, y, direction x, y].
    std::uint8_t selector = 0;
    bool edges = false;
};

struct RandomParameters final {
    std::int64_t seed = 0;
    double cellX = 1.0;
    double cellY = 1.0;
    double evolution = 0.0;
    bool smooth = false;
    bool gaussian = false;
    std::uint8_t selector = 0; // 0 scalar, 1 vector, 2 colour
};

enum class PatternShape : std::uint8_t { Stripes, Checker, Rings, Dots, GridLines, Hex, Wave };
enum class PatternWaveform : std::uint8_t { Sine, Triangle, Square, Saw };
struct PatternParameters final {
    PatternShape shape = PatternShape::Stripes;
    PatternWaveform waveform = PatternWaveform::Sine;
    double size = 50.0;
    double width = 0.5;
    double rotationDegrees = 0.0;
    double phase = 0.0; // cycles, speed times time already added
    double centreX = 0.0;
    double centreY = 0.0;
    double softness = 1.0; // field pixels
};

enum class GradientFieldMode : std::uint8_t { Linear, Radial, Angular, Diamond };
enum class GradientFieldRepeat : std::uint8_t { Clamp, Repeat, Mirror };
struct GradientFieldParameters final {
    GradientFieldMode mode = GradientFieldMode::Linear;
    GradientFieldRepeat repeat = GradientFieldRepeat::Clamp;
    double startX = -480.0;
    double startY = 0.0;
    double endX = 480.0;
    double endY = 0.0;
    double cycle = 0.0;
};

enum class ShapePrimitive : std::uint8_t { Circle, Box, RoundedBox, Segment, Polygon, Star, Ring };
enum class ShapeFalloffCurve : std::uint8_t { Linear, Smooth, Exponential, Step };
struct ShapeParameters final {
    bool mask = false;
    std::uint8_t slot = 0;
    // Field pixels per texel of the mask's distance image (1 / the evaluation scale).
    double unitsPerTexel = 1.0;
    ShapePrimitive primitive = ShapePrimitive::Circle;
    double centreX = 0.0;
    double centreY = 0.0;
    double sizeX = 200.0;
    double sizeY = 200.0;
    double rotationDegrees = 0.0;
    double cornerRadius = 0.0;
    std::int64_t sides = 5;
    double innerRatio = 0.5;
    double falloff = 100.0;
    ShapeFalloffCurve curve = ShapeFalloffCurve::Linear;
    bool invert = false;
};

enum class FieldImageFilter : std::uint8_t { Bilinear, Nearest, Bicubic };
enum class FieldImageEdge : std::uint8_t { Clamp, Transparent, Repeat, Mirror };
struct ImageParameters final {
    std::uint8_t slot = 0;
    FieldImageFilter filter = FieldImageFilter::Bilinear;
    FieldImageEdge edge = FieldImageEdge::Clamp;
    std::uint8_t selector = 0; // 0 scalar channel, 1 channel pair, 2 straight colour, 3 premultiplied
    std::uint8_t channel = 4;  // 0..3 red..alpha, 4 luma
    std::uint8_t first = 0;
    std::uint8_t second = 1;
};

// A Fritsch-Carlson monotone cubic, as runtime::GradeCurveShape prepares the Curves master curve:
// sorted knots, n + 1 segment origins and segments [a, b, c, d].
struct FieldCurve final {
    std::vector<double> knots;
    std::vector<double> origins;
    std::vector<std::array<double, 4>> segments;
};

enum class ForceMode : std::uint8_t { Vortex, Radial, Uniform };
enum class ForceFalloff : std::uint8_t { Linear, Smooth, InverseSquare, Curve };
struct ForceParameters final {
    ForceMode mode = ForceMode::Vortex;
    ForceFalloff falloff = ForceFalloff::Linear;
    double centreX = 0.0;
    double centreY = 0.0;
    double strength = 1.0;
    double radius = 200.0;
    bool clockwise = false;
    double angleDegrees = 0.0;
    std::optional<FieldCurve> curve{};
};

enum class FieldMathOperation : std::uint8_t {
    Add,
    Subtract,
    Multiply,
    Divide,
    Min,
    Max,
    Power,
    Abs,
    Difference,
    Lerp,
    Screen,
    SmoothMin,
    SmoothMax,
    Dot,
    Length,
    Normalize,
    Scale,
    Rotate
};
struct MathParameters final {
    FieldMathOperation operation = FieldMathOperation::Add;
    double smoothness = 0.1;
    double angleDegrees = 0.0;
};

struct RemapParameters final {
    double inLow = 0.0;
    double inHigh = 1.0;
    double outLow = 0.0;
    double outHigh = 1.0;
    bool clamp = false;
    bool invert = false;
    std::int64_t steps = 0;
    double bias = 0.5;
    double gain = 0.5;
    bool vector = false;
    std::optional<FieldCurve> curve{};
};

enum class FieldCoordinateMode : std::uint8_t { None, Polar, Tile, Mirror, Kaleidoscope };
struct TransformParameters final {
    double translateX = 0.0;
    double translateY = 0.0;
    double rotationDegrees = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double pivotX = 0.0;
    double pivotY = 0.0;
    FieldCoordinateMode mode = FieldCoordinateMode::None;
    double tileX = 200.0;
    double tileY = 200.0;
    std::int64_t segments = 6;
    double warpAmount = 0.0;
};

// The Brown-Conrady lens model about a centre (field_kernel.glsl, "Lens warp"). `reverse` reads the
// inverse distortion (Redistort); `scale` is the length in field pixels that normalises coordinates.
struct LensWarpParameters final {
    double k1 = 0.0;
    double k2 = 0.0;
    double p1 = 0.0;
    double p2 = 0.0;
    double centreX = 0.0;
    double centreY = 0.0;
    double scale = 1.0;
    bool reverse = false;
};

enum class FieldDerivativeMode : std::uint8_t { Gradient, CurlOfScalar, CurlOfVector, Divergence };

// A lane of a Swizzle: 0..3 a component of the input, 4 zero, 5 one.
struct SwizzleParameters final {
    std::array<std::uint8_t, 4> lanes{0, 1, 2, 3};
    std::array<double, 4> scales{1.0, 1.0, 1.0, 1.0};
};

// --- The table builder ------------------------------------------------------------------------

// Instruction inputs: register indices, or -1 for an absent input.
using FieldInputs = std::array<int, 4>;
inline constexpr FieldInputs kNoFieldInputs{-1, -1, -1, -1};

class FieldProgramBuilder final {
  public:
    // Appends one instruction whose parameter block is `parameters` (padded to whole vec4).
    // False once the program is full or an index is out of range.
    bool emit(FieldOp op, int output, FieldInputs inputs, std::span<const float> parameters);
    [[nodiscard]] std::size_t instructionCount() const noexcept { return code_.size(); }
    // The table with no raster geometry (applyFieldRasterGeometry fills it): null when empty,
    // too large or the result register is out of range.
    [[nodiscard]] std::optional<std::vector<float>> finish(int resultRegister,
                                                           FieldResultKind kind,
                                                           FieldOutputMode mode) const;

  private:
    struct Instruction final {
        FieldOp op = FieldOp::Constant;
        int output = 0;
        FieldInputs inputs = kNoFieldInputs;
        std::size_t parameterOffset = 0; // vec4 within parameters_
    };
    std::vector<Instruction> code_;
    std::vector<float> parameters_;
};

// The parameter blocks, in binary32 as the kernel reads them.
[[nodiscard]] std::vector<float> encodeConstant(std::array<double, 4> value);
// Null when a value cannot be represented (non-finite, a size not above 1e-6, an offset beyond
// 2^30 cells).
[[nodiscard]] std::optional<std::vector<float>> encodeNoise(const NoiseParameters&);
[[nodiscard]] std::optional<std::vector<float>> encodeVoronoi(const VoronoiParameters&);
[[nodiscard]] std::optional<std::vector<float>> encodeRandom(const RandomParameters&);
[[nodiscard]] std::optional<std::vector<float>> encodePattern(const PatternParameters&);
[[nodiscard]] std::optional<std::vector<float>> encodeGradient(const GradientFieldParameters&);
[[nodiscard]] std::optional<std::vector<float>> encodeShape(const ShapeParameters&);
[[nodiscard]] std::vector<float> encodeImage(const ImageParameters&);
// Force and Remap blocks carry their curve after their fixed vec4s, addressed from the block.
[[nodiscard]] std::optional<std::vector<float>> encodeForce(const ForceParameters&);
[[nodiscard]] std::optional<std::vector<float>> encodeMath(const MathParameters&);
[[nodiscard]] std::optional<std::vector<float>> encodeRemap(const RemapParameters&);
[[nodiscard]] std::optional<std::vector<float>> encodeTransform(const TransformParameters&);
[[nodiscard]] std::vector<float> encodeSwizzle(const SwizzleParameters&);
// Null when a value cannot be represented (non-finite, a scale not above 1e-6).
[[nodiscard]] std::optional<std::vector<float>> encodeLensWarp(const LensWarpParameters&);
[[nodiscard]] std::vector<float> encodeOffset(double dx, double dy);
[[nodiscard]] std::vector<float> encodeDerivative(FieldDerivativeMode mode, double step,
                                                  double strength);
[[nodiscard]] std::vector<float> encodeCellCentre(double spacing);
[[nodiscard]] std::vector<float> encodeArrow(double spacing, double lengthPerUnit,
                                             double halfWidth);
[[nodiscard]] std::vector<float> encodeStreak(double step, double noiseCell);
// 256 straight colours over [low, high] (equal bounds: the first colour).
[[nodiscard]] std::optional<std::vector<float>>
encodeRamp(double low, double high, std::span<const std::array<double, 4>> colours);

// --- Raster geometry --------------------------------------------------------------------------

struct FieldImageSlot final {
    // Texel coordinate of field position (x, y): u = kx x + cx, v = ky y + cy (texel i's centre at
    // u = i + 1/2).
    double kx = 0.0;
    double cx = 0.0;
    double ky = 0.0;
    double cy = 0.0;
    // Where the image's texels sit in the dispatched image (the GPU's atlas; 0 on the CPU) and its
    // extent; a width of 0 is an absent image (every sample transparent).
    std::int64_t originX = 0;
    std::int64_t originY = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
};

struct FieldRasterGeometry final {
    // Field position of the target pixel (i, j)'s sub-sample (i + ox, j + oy): (ax (i + ox) + bx,
    // ay (j + oy) + by).
    double ax = 1.0;
    double bx = 0.0;
    double ay = -1.0;
    double by = 0.0;
    std::int64_t targetX = 0;
    std::int64_t targetY = 0;
    std::int64_t targetWidth = 0;
    std::int64_t targetHeight = 0;
    // 1, 4 or 16 sub-samples on a regular grid.
    std::uint32_t samples = 1;
    std::array<FieldImageSlot, kMaxFieldImages> slots{};
};

// The geometry of a raster whose target is `width` x `height` pixels at evaluation scale
// (hScale, vScale) of a composition `compositionWidth` x `compositionHeight` full-resolution pixels,
// the target's pixel 0 at data-window origin (originX, originY) of the evaluation canvas.
[[nodiscard]] FieldRasterGeometry fieldRasterGeometry(std::int64_t originX, std::int64_t originY,
                                                      std::int64_t width, std::int64_t height,
                                                      double compositionWidth,
                                                      double compositionHeight, double hScale,
                                                      double vScale, std::uint32_t samples);
// The slot mapping of an image whose data window starts at (originX, originY) of the evaluation
// canvas, `width` x `height` texels (its position in the dispatched image is set separately).
[[nodiscard]] FieldImageSlot fieldImageSlot(std::int64_t originX, std::int64_t originY,
                                            std::int64_t width, std::int64_t height,
                                            double compositionWidth, double compositionHeight,
                                            double hScale, double vScale);
// Writes the geometry into a finished table. False when a value cannot be represented.
[[nodiscard]] bool applyFieldRasterGeometry(std::vector<float>& table,
                                            const FieldRasterGeometry& geometry);

// Sets every mask Shape Distance's field pixels per texel of its distance image.
void setFieldTexelScale(std::vector<float>& table, double unitsPerTexel) noexcept;

// Structural check of a table before either route interprets it: the magic and version, every
// instruction's op, registers and parameter block within the table, every value finite.
[[nodiscard]] bool validFieldTable(std::span<const float> table) noexcept;

// --- Evaluation (the CPU reference) -----------------------------------------------------------

// A stored premultiplied texel of image slot `slot` at (x, y) of its data window, always in range.
using FieldTexelFetch = std::array<float, 4> (*)(const void* context, int slot, int x, int y);
struct FieldImages final {
    const void* context = nullptr;
    FieldTexelFetch fetch = nullptr;
};

// The result register at field position (x, y). The table must be valid.
[[nodiscard]] std::array<float, 4> evaluateField(std::span<const float> table,
                                                 const FieldImages& images, float x,
                                                 float y) noexcept;
// One premultiplied output pixel of the target at dispatched pixel (x, y): transparent outside the
// target.
[[nodiscard]] std::array<float, 4> fieldPixel(std::span<const float> table,
                                              const FieldImages& images, int x, int y) noexcept;

// The kernel's source text, for the GPU program (field_kernel.glsl).
[[nodiscard]] std::string_view fieldKernelGlsl() noexcept;

// --- Mask distance (Felzenszwalb and Huttenlocher, "Distance Transforms of Sampled Functions",
// Theory of Computing 8, 2012) ---------------------------------------------------------------------

// The exact 1D squared distance transform d(q) = min_p ((q - p)^2 + f(p)) of `f`, every result
// limited to `cap`, into `d` (f.size()); `vertices` and `bounds` are scratch of f.size() and
// f.size() + 1.
void squaredDistance1d(std::span<const float> f, std::span<float> d, std::span<int> vertices,
                       std::span<double> bounds, float cap) noexcept;

} // namespace bloom::core::field

#endif // BLOOM_CORE_FIELD_PROGRAM_HPP
