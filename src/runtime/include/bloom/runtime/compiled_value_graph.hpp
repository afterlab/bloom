#pragma once

#include <bloom/core/camera_shake.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/scalar_primitives.hpp>
#include <bloom/document/data_block.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_operations.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/operation_index.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace bloom::runtime {

// The value graph: a second, much smaller compiled program that runs once per frame. Ordinary
// values run before image operations; Layer Bounds readouts and their dependants run in the
// post-image pass.
//
// It is deliberately parallel to the image chain rather than part of it. CompiledOperation is a
// closed variant of Image-producing steps addressed by OperationIndex, and every one of them is
// per-pixel work; these are per-frame numbers. Interleaving them would mean one topological order
// over two kinds of work that never share an address space, and an evaluator arm for a "step" that
// produces no pixels at all. So CompiledOperation and its lowering switch are untouched, and the
// only change to an existing type is one more alternative on each parameter operand.
//
// WHY THE VALUES ARE NOT STATICALLY TYPED HERE. The image chain's operands are typed
// (CompiledScalarParameter, CompiledVec2Parameter, CompiledColorParameter) because there are three
// of them and each is read by one field of one operation. The value graph has seven kinds and
// thirteen kernels, and nearly every kernel is kind-agnostic -- a Switch over Scalar and a Switch
// over Colour are one operation on different storage. A typed operand per kind would be a
// seven-by-thirteen matrix of structs that all say the same thing. So the lowered form carries a
// uniform CompiledValue and the TYPING LIVES WHERE IT BELONGS: the document layer, whose node
// definitions declare every socket's kind and whose validation refuses a mismatched link, and the
// compiler, which inserts an explicit promotion operation rather than ever reinterpreting a value.
// The evaluator still checks what it reads -- a kernel given the wrong alternative reports a scoped
// diagnostic and substitutes the node's documented fallback -- so a malformed plan is diagnosed,
// not trusted.

// One value a value-graph output can hold. The same closed set as document::ParameterValue minus
// RationalTime: a rational is how Bloom names an INSTANT, and a value graph carries quantities. A
// Time node's seconds output is a Scalar for exactly that reason -- it is a number an artist does
// arithmetic on, not a timeline address.
//
// #589: a Path output is shared, never copied: several consumers read one frame's immutable value,
// and a reroute hands on a pointer. `value` is never null; two values compare by what they hold.
struct CompiledPathValue final {
    std::shared_ptr<const document::PathValue> value = std::make_shared<document::PathValue>();
    friend bool operator==(const CompiledPathValue& left, const CompiledPathValue& right) {
        return left.value == right.value || *left.value == *right.value;
    }
};

using CompiledValue = std::variant<bool, std::int64_t, double, document::Vec2d, document::Vec3d,
                                   core::Color4d, std::string, CompiledPathValue>;

// Where one value lands. The plan's value outputs are a flat table: every operation claims a
// consecutive run of entries in it, so a Separate RGBA's four channels and a Time node's two units
// are each addressable on their own without a second level of indexing.
class ValueOutputIndex final {
  public:
    [[nodiscard]] static constexpr ValueOutputIndex fromRaw(const std::size_t value) noexcept {
        return ValueOutputIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const ValueOutputIndex&,
                                      const ValueOutputIndex&) noexcept = default;

  private:
    explicit constexpr ValueOutputIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

// One operand of a value operation: the authored constant, or an earlier value output.
//
// `id` is the parameter the constant came from, carried for the same reason every image-chain
// operand carries one -- so a diagnostic can name the exact parameter that failed rather than only
// the node. It is invalid for the one operand with no parameter behind it, a Reroute's
// pass-through.
// The three curve alternatives are task FIX1, item G: a literal Scalar, Vector 2 or Colour node may
// carry a curve of its own, so its authored value is sampled per frame exactly as a layer
// parameter's is -- same curve kinds, same sampler, same semantics version -- and a downstream
// driven parameter reads the sampled number rather than a constant. A literal of a kind with no
// curve (Vector 3, Integer, Boolean, String) and every generic OPERAND stay constant-or-output.
struct CompiledValueOperand final {
    document::ParameterId id;
    std::variant<CompiledValue, ValueOutputIndex, ScalarCurveIndex, Vec2CurveIndex, Vec3CurveIndex,
                 Color4CurveIndex>
        source;

    friend bool operator==(const CompiledValueOperand&, const CompiledValueOperand&) = default;
};

// A literal Value node, a Reroute, and nothing else: one operand straight through to one output.
// The two are the same lowered operation because they are the same lowered operation -- a literal
// is a pass-through of an authored constant, and a Reroute is a pass-through of someone else's
// value.
struct CompiledValuePassthrough final {
    CompiledValueOperand value;

    friend bool operator==(const CompiledValuePassthrough&,
                           const CompiledValuePassthrough&) = default;
};

// Zero inputs, two outputs: the frame being rendered, in seconds and as a frame number. Filled from
// the SAME request time the plan's animation curves are sampled at, so a Time node and a curve in
// one plan can never disagree about when "now" is.
struct CompiledValueTime final {
    friend bool operator==(const CompiledValueTime&, const CompiledValueTime&) = default;
};

// The frozen 24-operation scalar vocabulary. `operands` is exactly as long as the chosen
// operation's own arity -- the node declares five sockets, and the compiler lowers only the ones
// the operation reads, so the kernel is never handed an operand it would silently fold in.
struct CompiledValueScalarMath final {
    core::primitives::ScalarPrimitive operation = core::primitives::ScalarPrimitive::Add;
    bool clampResult = false;
    std::vector<CompiledValueOperand> operands;

    friend bool operator==(const CompiledValueScalarMath&,
                           const CompiledValueScalarMath&) = default;
};

// Componentwise vector arithmetic plus Scale, Normalize and Cross. The componentwise operations
// route through the scalar kernel one component at a time; only the three that reduce or
// renormalize across components reach the vector tranche.
struct CompiledValueVectorMath final {
    document::VectorOperation operation = document::VectorOperation::Add;
    std::uint8_t components = 2;
    CompiledValueOperand left;
    CompiledValueOperand right;
    CompiledValueOperand factor;

    friend bool operator==(const CompiledValueVectorMath&,
                           const CompiledValueVectorMath&) = default;
};

struct CompiledValueVectorReduce final {
    document::VectorReduction reduction = document::VectorReduction::Length;
    std::uint8_t components = 2;
    CompiledValueOperand left;
    CompiledValueOperand right;

    friend bool operator==(const CompiledValueVectorReduce&,
                           const CompiledValueVectorReduce&) = default;
};

struct CompiledValueMapRange final {
    document::RangeInterpolation interpolation = document::RangeInterpolation::Linear;
    bool clampResult = false;
    CompiledValueOperand value;
    CompiledValueOperand fromMinimum;
    CompiledValueOperand fromMaximum;
    CompiledValueOperand toMinimum;
    CompiledValueOperand toMaximum;

    friend bool operator==(const CompiledValueMapRange&, const CompiledValueMapRange&) = default;
};

struct CompiledValueClamp final {
    CompiledValueOperand value;
    CompiledValueOperand minimum;
    CompiledValueOperand maximum;

    friend bool operator==(const CompiledValueClamp&, const CompiledValueClamp&) = default;
};

// `color` selects straight-RGBA channel-independent interpolation over the plain scalar form. Not a
// vector mix of four floats: a Color is not a Vec4, so mixing one is its own operation with no
// gamut or OCIO step implied.
struct CompiledValueMix final {
    bool color = false;
    CompiledValueOperand factor;
    CompiledValueOperand start;
    CompiledValueOperand end;

    friend bool operator==(const CompiledValueMix&, const CompiledValueMix&) = default;
};

struct CompiledValueCompare final {
    document::CompareOperation operation = document::CompareOperation::Greater;
    CompiledValueOperand left;
    CompiledValueOperand right;
    CompiledValueOperand epsilon;

    friend bool operator==(const CompiledValueCompare&, const CompiledValueCompare&) = default;
};

// Both branches are lowered, and only the selected one is READ. The unselected branch is not
// evaluated for its own sake -- but if it names an earlier value output, that output was already
// computed by the pass, because a value graph is evaluated in topological order rather than pulled
// on demand. Cheap enough to be the right trade at this size, and it keeps the pass a single linear
// sweep with no re-entrancy.
struct CompiledValueSwitch final {
    CompiledValueOperand condition;
    CompiledValueOperand ifFalse;
    CompiledValueOperand ifTrue;

    friend bool operator==(const CompiledValueSwitch&, const CompiledValueSwitch&) = default;
};

// One vector or colour in, its components out, in declaration order.
struct CompiledValueSeparate final {
    CompiledValueOperand value;
    std::uint8_t componentCount = 2;
    bool color = false;

    friend bool operator==(const CompiledValueSeparate&, const CompiledValueSeparate&) = default;
};

struct CompiledValueCombine final {
    std::vector<CompiledValueOperand> components;
    bool color = false;

    friend bool operator==(const CompiledValueCombine&, const CompiledValueCombine&) = default;
};

// Deterministic: the same seed is the same value in every process and every frame. A Random node
// varies over time only when something wires a changing number into its seed.
struct CompiledValueRandom final {
    CompiledValueOperand seed;
    CompiledValueOperand minimum;
    CompiledValueOperand maximum;

    friend bool operator==(const CompiledValueRandom&, const CompiledValueRandom&) = default;
};

// Which widening a promotion performs. Every promotion the connect-time whitelist accepts becomes
// one of these, compiled as its OWN operation rather than applied silently where the value is read:
// a widening that shows up in a plan dump is a widening that can be diagnosed.
enum class ValuePromotion : std::uint8_t {
    IntegerToScalar,
    BooleanToInteger,
    BooleanToScalar,
    ScalarToVector2,
    ScalarToVector3,
};

struct CompiledValuePromotion final {
    ValuePromotion promotion = ValuePromotion::IntegerToScalar;
    CompiledValueOperand value;

    friend bool operator==(const CompiledValuePromotion&, const CompiledValuePromotion&) = default;
};

// Task UTIL-1's whole library, as one kernel. `operation` names WHICH conversion, string
// operation, numeric operation or readout this is; `operands` are the node's socket-backed inputs
// in its descriptor's order, and `selectors` its inline selectors in theirs. Both lists are exactly
// as long as the descriptor says, so the kernel indexes them positionally rather than searching by
// name once per frame.
//
// One variant alternative rather than sixty is the same trade CompiledValueSwitch already makes
// across seven socket kinds: these operations differ in what they COMPUTE, not in how they are
// addressed, and sixty structs holding the same two vectors would be sixty places for the operand
// order to drift from the table that produced it.
struct CompiledValueUtility final {
    document::ValueUtilityKernel operation = document::ValueUtilityKernel::ScalarToString;
    std::vector<CompiledValueOperand> operands;
    std::vector<std::int64_t> selectors;
    std::optional<document::DataBlockRecord> dataBlock;

    friend bool operator==(const CompiledValueUtility&, const CompiledValueUtility&) = default;
};

// Image bounds are produced by the image pass, so this readout is the one value kernel whose
// source is an image operation address rather than a value operand. It expands to four Vec2
// outputs: size, origin, authored anchor, and evaluated center.
struct CompiledBoundsReadout final {
    OperationIndex operationIndex;

    friend bool operator==(const CompiledBoundsReadout&, const CompiledBoundsReadout&) = default;
};

// #541: the colour of a gradient value at a fixed position, one Colour output. Glow's tint
// gradient lowers to one per octave when the gradient is animated (a constant gradient folds into
// constant colours instead). The gradient is inline, a constant or a whole-value Gradient curve
// sampled at the frame, so the kernel has no value operand.
struct CompiledValueGradientStopOperand final {
    std::uint32_t stop = 0;
    document::GradientStopComponent component = document::GradientStopComponent::Color;
    CompiledValueOperand value;
    friend bool operator==(const CompiledValueGradientStopOperand&,
                           const CompiledValueGradientStopOperand&) = default;
};

struct CompiledValueGradientSample final {
    std::variant<document::GradientValue, CompiledGradientCurve> gradient;
    double position = 0.0;
    // #543: the stops a value node drives, applied over the sampled gradient in this order.
    std::vector<CompiledValueGradientStopOperand> drivers{};

    friend bool operator==(const CompiledValueGradientSample&,
                           const CompiledValueGradientSample&) = default;
};

// #589: a literal Path node whose value is keyed. Path curves are whole-value and inline, so this
// sits beside the literal's Passthrough rather than in the plan's curve tables.
struct CompiledValuePathCurve final {
    CompiledPathCurve curve;
    friend bool operator==(const CompiledValuePathCurve&, const CompiledValuePathCurve&) = default;
};

// #525 Field Sample: a field program (compiled_plan.hpp, FieldProgram) read at one position,
// three outputs: its result's first lane (Scalar), its first two (Vector 2) and all four as a
// straight colour (Colour). `operands` are the program's Scalar and Vector 2 operands in
// forEachFieldOperand order, so the value graph's own sampling resolves them; its curves and ramps
// are inline in the program and sampled at the frame. The program reads no image.
struct FieldProgram;
struct CompiledValueFieldSample final {
    std::shared_ptr<const FieldProgram> program;
    std::vector<CompiledValueOperand> operands;
    CompiledValueOperand position;
    // Equal programs compare equal (field_raster.cpp), not equal pointers.
    friend bool operator==(const CompiledValueFieldSample&,
                           const CompiledValueFieldSample&) noexcept;
};

// #512 Camera Shake: the node's seeded motion (core/camera_shake.hpp) at the frame being rendered,
// scaled by its amplitudes, as the three values Transform 2D takes. Three outputs: the translation
// (Vector 2, pixels), the roll (Scalar, degrees clockwise) and the scale (Vector 2: 1, or with
// `overscan` the smallest scale that keeps a `width` by `height` frame covered at the largest
// motion the amplitudes allow). The settings are constants; the amplitudes are operands, so they
// animate and take drivers. Like a Time node it varies with the frame, which
// analyzeTimeDependence knows.
struct CompiledValueShake final {
    core::camera_shake::Settings settings;
    CompiledValueOperand amplitudeX;
    CompiledValueOperand amplitudeY;
    CompiledValueOperand roll;
    // Animatable: the settings' frequency, roughness and smoothing are replaced by these at the
    // frame (the motion at a frame is the noise at that frame's own frequency).
    CompiledValueOperand frequency;
    CompiledValueOperand roughness;
    CompiledValueOperand smoothing;
    bool overscan = true;
    double width = 0.0;
    double height = 0.0;
    // #568 Rolling Shutter reads the same shake at other moments of the frame: the motion is
    // evaluated `timeOffsetFrames` frames after the frame being rendered. With `components` the
    // three outputs are the scalars (offset x, offset y, roll) with no overscan scale, which a
    // field program takes as operands. Both are zero/false for the node's own operation.
    double timeOffsetFrames = 0.0;
    bool components = false;

    friend bool operator==(const CompiledValueShake&, const CompiledValueShake&) = default;
};

// #342: Group Input and Group Output. One operand per output slot, copied through in order. A slot
// the node cannot carry as a value (an Image, Mask or ChannelSet, which the expansion joined by
// edges) holds a constant placeholder that nothing reads.
struct CompiledValueGroupBoundary final {
    std::vector<CompiledValueOperand> operands;

    friend bool operator==(const CompiledValueGroupBoundary&,
                           const CompiledValueGroupBoundary&) = default;
};

struct CompiledFunctionBody;

// #590: a Function node. It runs a group's cut-out body (compiled_value_graph.hpp, below) for one
// argument. The body is shared by the plan's copies of this operation and is never edited once the
// plan exists.
struct CompiledValueFunction final {
    CompiledValueOperand argument;
    std::shared_ptr<const CompiledFunctionBody> body;

    friend bool operator==(const CompiledValueFunction&, const CompiledValueFunction&);
};

// #595: Function Plot. The body is sampled at `samples` arguments from `start` to `end` and the
// answers become the anchors of a Path (in pixels: origin + (x * scale.x, -y * scale.y)).
struct CompiledValueFunctionPlot final {
    std::shared_ptr<const CompiledFunctionBody> body;
    CompiledValueOperand start;
    CompiledValueOperand end;
    CompiledValueOperand samples;
    CompiledValueOperand origin;
    CompiledValueOperand scale;
    CompiledValueOperand smooth;

    friend bool operator==(const CompiledValueFunctionPlot&, const CompiledValueFunctionPlot&);
};

// #347 Audio Amplitude: the level of the audio of a source over a window around the frame, one
// Scalar. The measurement is prepared on a worker before the frame is evaluated (audio_analysis.hpp)
// and read here from the analysis cache; a frame whose analysis is not prepared reads 0 and says so.
// A null `source` is an input with no audio wired: the level is 0. `window` is milliseconds and
// `offset` seconds, both operands; `measure` is 0 Peak or 1 RMS and `unit` 0 Linear or 1 Decibels.
struct AudioAnalysisSource;
struct CompiledValueAudioAmplitude final {
    std::shared_ptr<const AudioAnalysisSource> source;
    CompiledValueOperand window;
    CompiledValueOperand offset;
    std::int64_t measure = 1;
    std::int64_t unit = 0;
    friend bool operator==(const CompiledValueAudioAmplitude&,
                           const CompiledValueAudioAmplitude&) noexcept;
};

using CompiledValueKernel =
    std::variant<CompiledValuePassthrough, CompiledValueTime, CompiledValueScalarMath,
                 CompiledValueVectorMath, CompiledValueVectorReduce, CompiledValueMapRange,
                 CompiledValueClamp, CompiledValueMix, CompiledValueCompare, CompiledValueSwitch,
                 CompiledValueSeparate, CompiledValueCombine, CompiledValueRandom,
                 CompiledValuePromotion, CompiledValueUtility, CompiledBoundsReadout,
                 CompiledValueGradientSample, CompiledValueFieldSample, CompiledValueShake,
                 CompiledValuePathCurve, CompiledValueGroupBoundary, CompiledValueFunction,
                 CompiledValueFunctionPlot, CompiledValueAudioAmplitude>;

// Every operand one kernel reads, in declaration order. Written once, here beside the kernels, so a
// caller that has to walk them -- the evaluator's preflight, which has to know which animation
// curves the value graph references before it can call an unreferenced one a malformed plan -- does
// not carry its own copy of the kernel list that would silently go stale when a kernel is added.
template <typename Visit> void forEachValueOperand(const CompiledValueKernel& kernel, Visit visit) {
    std::visit(
        [&visit](const auto& step) {
            using Step = std::decay_t<decltype(step)>;
            if constexpr (std::is_same_v<Step, CompiledValueTime> ||
                          std::is_same_v<Step, CompiledBoundsReadout> ||
                          std::is_same_v<Step, CompiledValuePathCurve>) {
                // The one kernel with no operands: its value is the frame being rendered.
            } else if constexpr (std::is_same_v<Step, CompiledValueGradientSample>) {
                for (const auto& driver : step.drivers)
                    visit(driver.value);
            } else if constexpr (std::is_same_v<Step, CompiledValueFieldSample>) {
                for (const auto& operand : step.operands)
                    visit(operand);
                visit(step.position);
            } else if constexpr (std::is_same_v<Step, CompiledValueAudioAmplitude>) {
                visit(step.window);
                visit(step.offset);
            } else if constexpr (std::is_same_v<Step, CompiledValueShake>) {
                visit(step.amplitudeX);
                visit(step.amplitudeY);
                visit(step.roll);
                visit(step.frequency);
                visit(step.roughness);
                visit(step.smoothing);
            } else if constexpr (std::is_same_v<Step, CompiledValueFunction>) {
                visit(step.argument);
            } else if constexpr (std::is_same_v<Step, CompiledValueFunctionPlot>) {
                visit(step.start);
                visit(step.end);
                visit(step.samples);
                visit(step.origin);
                visit(step.scale);
                visit(step.smooth);
            } else if constexpr (std::is_same_v<Step, CompiledValuePassthrough> ||
                                 std::is_same_v<Step, CompiledValueSeparate> ||
                                 std::is_same_v<Step, CompiledValuePromotion>) {
                visit(step.value);
            } else if constexpr (std::is_same_v<Step, CompiledValueScalarMath> ||
                                 std::is_same_v<Step, CompiledValueUtility> ||
                                 std::is_same_v<Step, CompiledValueGroupBoundary>) {
                for (const auto& operand : step.operands)
                    visit(operand);
            } else if constexpr (std::is_same_v<Step, CompiledValueCombine>) {
                for (const auto& operand : step.components)
                    visit(operand);
            } else if constexpr (std::is_same_v<Step, CompiledValueVectorMath>) {
                visit(step.left);
                visit(step.right);
                visit(step.factor);
            } else if constexpr (std::is_same_v<Step, CompiledValueVectorReduce>) {
                visit(step.left);
                visit(step.right);
            } else if constexpr (std::is_same_v<Step, CompiledValueMapRange>) {
                visit(step.value);
                visit(step.fromMinimum);
                visit(step.fromMaximum);
                visit(step.toMinimum);
                visit(step.toMaximum);
            } else if constexpr (std::is_same_v<Step, CompiledValueClamp>) {
                visit(step.value);
                visit(step.minimum);
                visit(step.maximum);
            } else if constexpr (std::is_same_v<Step, CompiledValueMix>) {
                visit(step.factor);
                visit(step.start);
                visit(step.end);
            } else if constexpr (std::is_same_v<Step, CompiledValueCompare>) {
                visit(step.left);
                visit(step.right);
                visit(step.epsilon);
            } else if constexpr (std::is_same_v<Step, CompiledValueSwitch>) {
                visit(step.condition);
                visit(step.ifFalse);
                visit(step.ifTrue);
            } else {
                static_assert(std::is_same_v<Step, CompiledValueRandom>,
                              "every value kernel must enumerate its operands here");
                visit(step.seed);
                visit(step.minimum);
                visit(step.maximum);
            }
        },
        kernel);
}

// One step of the value graph. `firstOutput` and `outputCount` are the run of entries in the plan's
// flat value-output table this step fills; a promotion the compiler synthesized carries no document
// node, so `sourceNodeId` is invalid for exactly those.
struct CompiledValueOperation final {
    document::NodeId sourceNodeId;
    ValueOutputIndex firstOutput;
    std::uint8_t outputCount = 1;
    CompiledValueKernel kernel;
    // A bounds readout and every value operation transitively fed by it run after the image pass.
    // Existing aggregate initializers remain valid because this additive flag has a default.
    bool requiresPostImage = false;

    friend bool operator==(const CompiledValueOperation&, const CompiledValueOperation&) = default;
};

// #590: the part of a group that computes its answer, cut out of the value graph and kept as a small
// program of its own with its own output table. The argument enters as operand `inputSlot` of the
// Group Input operation `inputOperation`; a function that never reads its argument has none.
struct CompiledFunctionBody final {
    // Topologically ordered, with output indices local to this program.
    std::vector<CompiledValueOperation> operations;
    std::size_t outputCount = 0;
    std::optional<std::size_t> inputOperation;
    std::size_t inputSlot = 0;
    // Where the answer is.
    ValueOutputIndex result = ValueOutputIndex::fromRaw(0);
    // Whether the answer can change with the time of the frame (a Time node or a keyed value in the
    // body), so the plan's time-dependence analysis can see through a consumer.
    bool timeDependent = false;

    friend bool operator==(const CompiledFunctionBody&, const CompiledFunctionBody&) = default;
};

[[nodiscard]] inline bool sameFunctionBody(const std::shared_ptr<const CompiledFunctionBody>& left,
                                           const std::shared_ptr<const CompiledFunctionBody>& right) {
    return left == right || (left != nullptr && right != nullptr && *left == *right);
}

inline bool operator==(const CompiledValueFunctionPlot& left,
                       const CompiledValueFunctionPlot& right) {
    return sameFunctionBody(left.body, right.body) && left.start == right.start &&
           left.end == right.end && left.samples == right.samples && left.origin == right.origin &&
           left.scale == right.scale && left.smooth == right.smooth;
}

inline bool operator==(const CompiledValueFunction& left, const CompiledValueFunction& right) {
    return left.argument == right.argument && sameFunctionBody(left.body, right.body);
}

} // namespace bloom::runtime
