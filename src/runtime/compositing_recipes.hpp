#ifndef BLOOM_RUNTIME_COMPOSITING_RECIPES_HPP
#define BLOOM_RUNTIME_COMPOSITING_RECIPES_HPP

// Private to src/runtime. #339 Drop Shadow, Glow and Light Wrap are recipes: each lowers to a
// fixed chain of the existing image primitives (the separable blurs, Transform 2D, Luma Key, Set
// Alpha, Multiply Alpha, Image Mix, Levels and an explicitly blended Merge) with no pixel kernel of
// its own. Every operation of the chain therefore keeps its primitive's CPU reference, resident GPU
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

    [[nodiscard]] OperationIndex effect(OperationIndex input, ImageEffectKernel kernel,
                                        std::optional<OperationIndex> secondary = {});
    // `entries` are top first (the Merge's own order); each is a plain image with an explicit mode.
    [[nodiscard]] OperationIndex
    merge(std::initializer_list<std::pair<OperationIndex, core::BlendMode>> entries);

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

struct GlowOperands final {
    CompiledScalarParameter threshold;
    CompiledScalarParameter knee;
    CompiledScalarParameter intensity;
    CompiledScalarParameter size; // the widest octave's blur radius
    CompiledScalarParameter falloff;
    CompiledColorParameter tint;
    std::int64_t octaves = 5;
    bool glowOnly = false;
};

struct LightWrapOperands final {
    CompiledScalarParameter radius;
    CompiledScalarParameter amount;
    bool wrapOnly = false;
};

// Each returns the operation that publishes the node's image.
[[nodiscard]] OperationIndex appendDropShadow(RecipeEmitter& emitter, OperationIndex input,
                                              const DropShadowOperands& operands);
// `key` replaces the input as the image that glows; `mask` (an ADR 0028 Mask) limits it.
[[nodiscard]] OperationIndex appendGlow(RecipeEmitter& emitter, OperationIndex input,
                                        std::optional<OperationIndex> key,
                                        std::optional<OperationIndex> mask,
                                        const GlowOperands& operands);
[[nodiscard]] OperationIndex appendLightWrap(RecipeEmitter& emitter, OperationIndex input,
                                             OperationIndex background,
                                             const LightWrapOperands& operands);

// A plan's value operations and output count, for callers (tests, fixtures) that build a plan
// definition directly: the sink appends to `definition`.
[[nodiscard]] RecipeEmitter::ValueSink
definitionValueSink(CompiledCompositionPlanDefinition& definition);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_COMPOSITING_RECIPES_HPP
