#ifndef BLOOM_RUNTIME_COMPOSITING_RECIPES_HPP
#define BLOOM_RUNTIME_COMPOSITING_RECIPES_HPP

// Private to src/runtime. #339 Drop Shadow, Glow and Light Wrap are recipes: each lowers to a
// fixed chain of the image primitives (the separable blurs, Transform 2D, Luma Key, Set Alpha,
// Multiply Alpha, Image Mix, Levels, Channel Select and an explicitly blended Merge; #506 Glow's
// Streak rays and roll-off add the Directional Blur and Highlight Roll-off primitives) with no
// pixel kernel of its own. Every operation of the chain therefore keeps its primitive's CPU reference, resident GPU
// command, cache identity, ROI, budget, cancellation and retirement unchanged; the recipe only
// decides which primitives run and in what order.
//
// A value the chain needs that is not an authored parameter (a blur radius halved per octave, the
// shadow offset from its direction and distance, a tint scaled by the intensity) is DERIVED by
// value-graph operations appended to the plan, so an animated or driven parameter stays animated
// or driven through the derivation and every derived value is resolved by the ordinary per-frame
// parameter preflight. A derivation whose operands are all constants is folded to its constant at
// compile time with the same core::primitives scalar evaluation the value graph runs.
//
// The snapshot compiler is the production caller; the GPU coverage fixtures and the recipe tests
// build their plans through the same functions, so the evidence covers the production chains.
// docs/architecture/image-filters.md ("Drop Shadow, Glow and Light Wrap") is the specification.

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/scalar_primitives.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

class RecipeEmitter final {
  public:
    // Appends one value-graph operation of `outputCount` outputs and returns its first output.
    using ValueSink = std::function<ValueOutputIndex(CompiledValueKernel, std::uint8_t)>;

    RecipeEmitter(document::NodeId node, std::vector<CompiledOperation>& operations,
                  ValueSink values)
        : node_(node), operations_(operations), values_(std::move(values)) {}

    // `mask` is Image Mix's optional Mask (ADR 0028).
    [[nodiscard]] OperationIndex effect(OperationIndex input, ImageEffectKernel kernel,
                                        std::optional<OperationIndex> secondary = {},
                                        std::optional<OperationIndex> mask = {});
    // #483: the images an effect reads beside its input (CompiledImageEffect::fieldImages).
    void setImages(OperationIndex effect, std::vector<OperationIndex> images);
    // `entries` are top first (the Merge's own order); each is a plain image with an explicit mode.
    [[nodiscard]] OperationIndex
    merge(std::initializer_list<std::pair<OperationIndex, core::BlendMode>> entries);
    [[nodiscard]] OperationIndex
    merge(const std::vector<std::pair<OperationIndex, core::BlendMode>>& entries);

    // A derived value names no document parameter (ADR 0028's synthesized operand): every value
    // below has an invalid identity, so it claims none and an authored parameter keeps the one
    // kernel field it drives directly.
    [[nodiscard]] static CompiledScalarParameter constant(double value);
    // One scalar primitive over `operands`, folded when every operand is constant.
    [[nodiscard]] CompiledScalarParameter
    scalar(core::primitives::ScalarPrimitive primitive,
           std::initializer_list<CompiledScalarParameter> operands);
    // A second use of an authored operand: the same value without its identity (a curve is read
    // through the value graph, since a curve has one image-parameter owner).
    [[nodiscard]] CompiledScalarParameter reuse(const CompiledScalarParameter& operand);
    // The straight red, green, blue and alpha of a colour operand.
    [[nodiscard]] std::array<CompiledScalarParameter, 4>
    components(const CompiledColorParameter& color);
    // #541: a gradient's colour at `position` (gradientTintAt): a constant for a constant
    // gradient, otherwise a value-graph sample of its curve at the frame.
    [[nodiscard]] CompiledColorParameter gradientColor(const CompiledGradientParameter& gradient,
                                                       double position);
    [[nodiscard]] CompiledVec2Parameter vector(const CompiledScalarParameter& x,
                                               const CompiledScalarParameter& y);

  private:
    document::NodeId node_;
    std::vector<CompiledOperation>& operations_;
    ValueSink values_;
};

struct DropShadowOperands final {
    CompiledColorParameter color;
    CompiledScalarParameter opacity;
    CompiledScalarParameter direction; // degrees clockwise from up
    CompiledScalarParameter distance;  // full-resolution pixels
    CompiledScalarParameter softness;  // blur radius, full-resolution pixels
    bool shadowOnly = false;
};

// #506: Classic is the #339 glow; Physical and Streak are docs/architecture/image-filters.md,
// "Glow modes".
enum class GlowMode : std::uint8_t { Classic, Physical, Streak };

// Streak rays are kStreakPasses chained Directional Blur passes of four taps, so tap t of
// 0..kStreakTaps - 1 lies t * length / (kStreakTaps - 1) along the ray with weight proportional to
// kStreakEndWeight^(t / (kStreakTaps - 1)): an exponential streak whose last tap weighs 1% of its
// first.
inline constexpr std::int64_t kStreakPasses = 5;
inline constexpr std::int64_t kStreakTaps = 1024; // 4^kStreakPasses
inline constexpr double kStreakEndWeight = 0.01;

struct GlowOperands final {
    CompiledScalarParameter threshold;
    CompiledScalarParameter knee;
    CompiledScalarParameter intensity;
    CompiledScalarParameter size; // the widest octave's blur radius
    CompiledScalarParameter falloff;
    CompiledColorParameter tint;
    std::int64_t octaves = 5;
    bool glowOnly = false;
    // #506 modes and shared controls; these defaults are the Classic glow of #339.
    GlowMode mode = GlowMode::Classic;
    CompiledScalarParameter exponent = RecipeEmitter::constant(2.0); // Physical: PSF ~ r^-exponent
    // Per-channel fractions of `size` (Streak: of `length`).
    std::array<CompiledScalarParameter, 3> radius{
        RecipeEmitter::constant(1.0), RecipeEmitter::constant(1.0), RecipeEmitter::constant(1.0)};
    bool gradient = false;
    // #541: the octave tints with the gradient on; unbound, the default warm gradient.
    CompiledGradientParameter tintGradient{{}, document::defaultGlowTintGradient()};
    std::int64_t points = 4;                                          // Streak rays
    CompiledScalarParameter angle = RecipeEmitter::constant(0.0);     // degrees clockwise from right
    CompiledScalarParameter length = RecipeEmitter::constant(128.0);  // Streak ray length, pixels
    CompiledScalarParameter chromatic = RecipeEmitter::constant(0.0); // Streak dispersion
    CompiledScalarParameter dirtAmount = RecipeEmitter::constant(1.0);
    CompiledScalarParameter rollOff = RecipeEmitter::constant(0.0);
};

struct LightWrapOperands final {
    CompiledScalarParameter radius;
    CompiledScalarParameter amount;
    bool wrapOnly = false;
};

// #516 Edge Extend: the colour under a soft alpha edge, taken from the solid pixels near it.
struct EdgeExtendOperands final {
    CompiledScalarParameter distance; // blur radius, full-resolution pixels
    CompiledScalarParameter amount;   // [0, 1]
};

// #509 Alpha Contour: a band along the input's alpha edge. Width, softness and offset are
// full-resolution pixels; offset moves the band outward (positive) or inward (negative).
enum class AlphaContourComposite : std::uint8_t { Over, Under, ContourOnly };
struct AlphaContourOperands final {
    CompiledScalarParameter width;
    CompiledScalarParameter softness;
    CompiledScalarParameter offset;
    CompiledColorParameter color; // straight RGBA; its alpha scales the band
    AlphaContourComposite composite = AlphaContourComposite::Over;
};

// #528 Inner Shadow: the shadow everything outside the input's alpha casts onto its inside.
struct InnerShadowOperands final {
    CompiledColorParameter color;
    CompiledScalarParameter opacity;
    CompiledScalarParameter direction; // degrees clockwise from up
    CompiledScalarParameter distance;  // full-resolution pixels
    CompiledScalarParameter softness;  // blur radius, full-resolution pixels
    CompiledScalarParameter choke;     // [0, 1]
    bool shadowOnly = false;
};

// #528 Inner Glow: a glow that fades inward from the input's alpha edge, or outward from its middle.
struct InnerGlowOperands final {
    CompiledColorParameter color;
    CompiledScalarParameter opacity;
    CompiledScalarParameter intensity;
    CompiledScalarParameter size;  // blur radius, full-resolution pixels
    CompiledScalarParameter choke; // [0, 1]
    bool centre = false;           // source Centre; otherwise Edge
    bool glowOnly = false;
};

// #486 Sharpen, which unlike the recipes above has one pixel kernel of its own over the blur.
struct SharpenOperands final {
    CompiledScalarParameter amount;
    CompiledScalarParameter radius;
    CompiledScalarParameter threshold;
};

// Each returns the operation that publishes the node's image.
[[nodiscard]] OperationIndex appendDropShadow(RecipeEmitter& emitter, OperationIndex input,
                                              const DropShadowOperands& operands);
// `key` replaces the input as the image that glows; `mask` (an ADR 0028 Mask) limits it; `dirt`
// (#506) adds the glow modulated by its luma.
[[nodiscard]] OperationIndex appendGlow(RecipeEmitter& emitter, OperationIndex input,
                                        std::optional<OperationIndex> key,
                                        std::optional<OperationIndex> mask,
                                        const GlowOperands& operands,
                                        std::optional<OperationIndex> dirt = {});
[[nodiscard]] OperationIndex appendLightWrap(RecipeEmitter& emitter, OperationIndex input,
                                             OperationIndex background,
                                             const LightWrapOperands& operands);
[[nodiscard]] OperationIndex appendInnerShadow(RecipeEmitter& emitter, OperationIndex input,
                                               const InnerShadowOperands& operands);
[[nodiscard]] OperationIndex appendInnerGlow(RecipeEmitter& emitter, OperationIndex input,
                                             const InnerGlowOperands& operands);
[[nodiscard]] OperationIndex appendEdgeExtend(RecipeEmitter& emitter, OperationIndex input,
                                              const EdgeExtendOperands& operands);

// The input's exact mask distance transform (MaskDistanceKernel), reaching past the widest band
// the operands can describe, and a field raster over it (FieldRasterKernel) that turns each
// pixel's signed distance into the band's coverage times the colour, then the band composited
// with the input as `composite` asks.
[[nodiscard]] OperationIndex appendAlphaContour(RecipeEmitter& emitter, OperationIndex input,
                                                const AlphaContourOperands& operands);
// The distance the band's mask distance transform must reach for these operands: from constants,
// the band's own extent; otherwise the widest band the parameter domains allow.
[[nodiscard]] double alphaContourReach(const AlphaContourOperands& operands);

// The input's Gaussian blur with the frame edge clamped, so the edge of the image is not an edge of
// the picture, then the unsharp mask over the two.
[[nodiscard]] OperationIndex appendSharpen(RecipeEmitter& emitter, OperationIndex input,
                                           const SharpenOperands& operands);

// Gaussian Blur with a radius per channel: each of red, green, blue and alpha is the associated
// channel of the input blurred by `radius` times its own scale (and `radiusY` times it, with
// separate axes). A blur at the widest radius owns the output window, then each channel is
// combined over it from its own blur.
struct ChannelBlurOperands final {
    CompiledScalarParameter radius;
    CompiledScalarParameter radiusY;
    bool separate = false;
    BlurEdgeMode edge = BlurEdgeMode::Transparent;
    std::array<CompiledScalarParameter, 4> scales; // red, green, blue, alpha; each in [0, 1]
};
[[nodiscard]] OperationIndex appendChannelBlur(RecipeEmitter& emitter, OperationIndex input,
                                               const ChannelBlurOperands& operands);

// A plan's value operations and output count, for callers (tests, fixtures) that build a plan
// definition directly: the sink appends to `definition`.
[[nodiscard]] RecipeEmitter::ValueSink
definitionValueSink(CompiledCompositionPlanDefinition& definition);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_COMPOSITING_RECIPES_HPP
