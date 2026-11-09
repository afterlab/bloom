#pragma once

#include <bloom/document/parameter.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {

struct NodeTypeKey {
    std::string typeId;
    std::uint32_t schemaVersion = 0;

    friend bool operator==(const NodeTypeKey&, const NodeTypeKey&) = default;
};

enum class SocketValueKind {
    Image,
    Audio,
    Color,
    Scalar,
    Vector2,
    String,
    // Task S7. Integer and Boolean carry ParameterValue's existing std::int64_t and bool members,
    // so neither needed a new authoring type; Vector3 needed document::Vec3d.
    Integer,
    Boolean,
    Vector3,
    // ADR 0028: single-channel coverage. Never interchangeable with Image in either direction.
    Mask,
    // ADR 0026: a multi-channel image (render::ChannelSetImage). Never converts to or from Image.
    ChannelSet,
    // #525 Fields (field_nodes.hpp): lazy functions of a composition position, never pixels. They
    // connect only to their own kind (or to a Fields input that reads any field), never to an
    // Image, a Mask or a value.
    ScalarField,
    Vector2Field,
    ColorField,
    // #589: a whole path value (document::PathValue), evaluated per frame like the other value
    // kinds and read by any number of consumers. Connects only to its own kind.
    Path,
};

enum class ParameterValueKind {
    Color4d,
    Vec2d,
    Float64,
    String,
    // A small signed integer. Its first use was an enumeration stored under its own closed mapping
    // (the Layer Output blend mode), which is why there is no separate "Enum" kind: the stored
    // value IS an integer, and the schema key -- not the value kind -- is what names the
    // enumeration the integer belongs to. Task S7 gave it a second, plainer use: an Integer value
    // node and every index, seed and count operand in the value library.
    Integer,
    // Task S7.
    Boolean,
    Vec3d,
    Path,
    // #541: a colour gradient (GradientValue).
    Gradient,
};

// The lower-case names scripting clients and diagnostics use for the two kinds ("scalar",
// "integer", "vector2", ...). Stable wire spellings: MCP's node-types query publishes them.
[[nodiscard]] constexpr std::string_view socketValueKindName(const SocketValueKind kind) noexcept {
    switch (kind) {
    case SocketValueKind::Image:
        return "image";
    case SocketValueKind::Audio:
        return "audio";
    case SocketValueKind::Color:
        return "color";
    case SocketValueKind::Scalar:
        return "scalar";
    case SocketValueKind::Vector2:
        return "vector2";
    case SocketValueKind::String:
        return "string";
    case SocketValueKind::Integer:
        return "integer";
    case SocketValueKind::Boolean:
        return "boolean";
    case SocketValueKind::Vector3:
        return "vector3";
    case SocketValueKind::Mask:
        return "mask";
    case SocketValueKind::ChannelSet:
        return "channel-set";
    case SocketValueKind::ScalarField:
        return "scalar-field";
    case SocketValueKind::Vector2Field:
        return "vector2-field";
    case SocketValueKind::ColorField:
        return "color-field";
    case SocketValueKind::Path:
        return "path";
    }
    return "image";
}

[[nodiscard]] constexpr std::string_view
parameterValueKindName(const ParameterValueKind kind) noexcept {
    switch (kind) {
    case ParameterValueKind::Color4d:
        return "color";
    case ParameterValueKind::Vec2d:
        return "vector2";
    case ParameterValueKind::Float64:
        return "scalar";
    case ParameterValueKind::String:
        return "string";
    case ParameterValueKind::Integer:
        return "integer";
    case ParameterValueKind::Boolean:
        return "boolean";
    case ParameterValueKind::Vec3d:
        return "vector3";
    case ParameterValueKind::Path:
        return "path";
    case ParameterValueKind::Gradient:
        return "gradient";
    }
    return "scalar";
}

// The one correspondence between an authored value's kind and the socket kind that carries it.
// Every value-graph node definition is checked against this rather than restating the pairing, so a
// parameter and the socket that can drive it cannot disagree about what they hold.
[[nodiscard]] constexpr std::optional<SocketValueKind>
socketKindForParameterValueKind(const ParameterValueKind kind) noexcept {
    switch (kind) {
    case ParameterValueKind::Path:
        return SocketValueKind::Path; // #589.
    case ParameterValueKind::Gradient:
        return std::nullopt; // Gradients (#541) are inline: no socket carries them.
    case ParameterValueKind::Color4d:
        return SocketValueKind::Color;
    case ParameterValueKind::Vec2d:
        return SocketValueKind::Vector2;
    case ParameterValueKind::Float64:
        return SocketValueKind::Scalar;
    case ParameterValueKind::String:
        return SocketValueKind::String;
    case ParameterValueKind::Integer:
        return SocketValueKind::Integer;
    case ParameterValueKind::Boolean:
        return SocketValueKind::Boolean;
    case ParameterValueKind::Vec3d:
        return SocketValueKind::Vector3;
    }
    return SocketValueKind::Image;
}

// The promotion whitelist (task S7, item 1). A connection is accepted when the kinds are EQUAL, or
// when the source kind appears here as promotable to the destination kind.
//
// Deliberately a short explicit whitelist rather than implicit numeric coercion anywhere a number
// meets a number. Each entry below is a widening with exactly one answer and no lost information:
//
//   Integer -> Scalar    every std::int64_t a document can hold is exactly representable as a
//                        double up to 2^53, and the compiler emits an explicit conversion operation
//                        so the widening is visible in a plan dump rather than implied.
//   Boolean -> Integer   false is 0 and true is 1, the same mapping every stored boolean already
//   has. Boolean -> Scalar    the same mapping, widened once more; offered directly so a Compare
//   result
//                        can feed a Mix factor without an intervening conversion node.
//   Scalar  -> Vector2   "splat": the one value in every component. A vector built from one number
//   Scalar  -> Vector3   has no other defensible reading, and it is what every comparable tool
//   does.
//
// Nothing promotes INTO Boolean, String, or Image, and no vector promotes to another width: each of
// those would have to invent information (which components? which spelling? which pixels?), and a
// refusal the artist can see is better than a guess they cannot.
[[nodiscard]] constexpr bool
isPromotedSocketConnection(const SocketValueKind source,
                           const SocketValueKind destination) noexcept {
    switch (source) {
    case SocketValueKind::Integer:
        return destination == SocketValueKind::Scalar;
    case SocketValueKind::Boolean:
        return destination == SocketValueKind::Integer || destination == SocketValueKind::Scalar;
    case SocketValueKind::Scalar:
        return destination == SocketValueKind::Vector2 || destination == SocketValueKind::Vector3;
    case SocketValueKind::Image:
    case SocketValueKind::Audio:
    case SocketValueKind::Color:
    case SocketValueKind::Vector2:
    case SocketValueKind::String:
    case SocketValueKind::Vector3:
    case SocketValueKind::Mask:
    case SocketValueKind::ChannelSet:
    case SocketValueKind::ScalarField:
    case SocketValueKind::Vector2Field:
    case SocketValueKind::ColorField:
    case SocketValueKind::Path:
        return false;
    }
    return false;
}

// The ONE question every connect-time and validation-time kind check asks, so the editor's drag
// affinity, ConnectPorts, CanonicalGraph::validate() and the compiler's edge check cannot disagree
// about which links exist.
[[nodiscard]] constexpr bool
isAcceptedSocketConnection(const SocketValueKind source,
                           const SocketValueKind destination) noexcept {
    return source == destination || isPromotedSocketConnection(source, destination);
}

struct InputPortDefinition {
    std::string name;
    SocketValueKind valueKind = SocketValueKind::Image;
    bool required = true;
    // #490: an Image input that also takes a Mask or a ChannelSet: the Image Preview's inspection
    // input, which shows it as its image, and (#483) a depth effect's `depth`, which reads one
    // channel of it. False for every other port.
    bool acceptsPixelKinds = false;
    // #525: a Fields input that reads any field kind (ScalarField, Vector2Field or ColorField).
    bool acceptsFieldKinds = false;
    // #383: an effect's Mask input (matte_nodes.hpp) that also takes an Image, read through its
    // matte selector. False for every other port.
    bool acceptsImageMatte = false;

    friend bool operator==(const InputPortDefinition&, const InputPortDefinition&) = default;
};

// The pixel-carrying kinds an inspection input (acceptsPixelKinds) takes.
[[nodiscard]] constexpr bool isPixelSocketKind(const SocketValueKind kind) noexcept {
    return kind == SocketValueKind::Image || kind == SocketValueKind::Mask ||
           kind == SocketValueKind::ChannelSet;
}

// #525: the field kinds an any-field input (acceptsFieldKinds) takes.
[[nodiscard]] constexpr bool isFieldSocketKind(const SocketValueKind kind) noexcept {
    return kind == SocketValueKind::ScalarField || kind == SocketValueKind::Vector2Field ||
           kind == SocketValueKind::ColorField;
}

// isAcceptedSocketConnection() for a declared input: the same rule, plus the pixel kinds an
// inspection or field-reading input takes and the field kinds an any-field input takes. Every
// connect and validation path that has the input's declaration asks this one.
[[nodiscard]] constexpr bool isAcceptedInputConnection(const SocketValueKind source,
                                                       const InputPortDefinition& input) noexcept {
    return isAcceptedSocketConnection(source, input.valueKind) ||
           (input.acceptsPixelKinds && isPixelSocketKind(source)) ||
           (input.acceptsFieldKinds && isFieldSocketKind(source)) ||
           (input.acceptsImageMatte && source == SocketValueKind::Image);
}

struct OutputPortDefinition {
    std::string name;
    SocketValueKind valueKind = SocketValueKind::Image;

    friend bool operator==(const OutputPortDefinition&, const OutputPortDefinition&) = default;
};

struct ParameterDefinition {
    std::string role;
    std::string schemaKey;
    ParameterValueKind valueKind = ParameterValueKind::Float64;
    bool required = true;
    bool supportsAnimation = false;
    ParameterValue defaultValue = 0.0;

    friend bool operator==(const ParameterDefinition&, const ParameterDefinition&) = default;
};

struct LayerSlotInputDefinition {
    std::string role;
    SocketValueKind valueKind = SocketValueKind::Image;
    bool requiredPerSlot = true;

    friend bool operator==(const LayerSlotInputDefinition&,
                           const LayerSlotInputDefinition&) = default;
};

// A node's authoring family. It is NOT where the node is listed: the Add menu, the search, the card
// eyebrow and MCP read the group in node_groups.hpp (#554). What still reads the family is
// behaviour. Grade, Color and Filters image effects take the optional effect mask
// (takesEffectMask), and the mask lowers differently for Filters (snapshot_compiler_mattes.ipp);
// value nodes must be Values, Math or Utilities. A new image effect that should take a mask
// declares one of those three, wherever its group is. Never persisted: a document stores the node's
// type id.
enum class NodeCategory : std::uint8_t {
    Sources,
    Layers,
    Compositing,
    // The OCIO transforms.
    Color,
    Values,
    // The arithmetic, apart from the plumbing in Utilities (task UTIL-1).
    Math,
    Output,
    Utilities,
    // Geometric image operations (Flip, Rotate 90).
    Transform,
    // #500: the blurs, the matte filters and the other effects that mix a mask over the result.
    Filters,
    // #500: image grades that never convert colour (Grade, Levels, Curves, Hue/Saturation,
    // Exposure, Offset, Multiply, Invert), apart from colour management.
    Grade,
    // #525: the Fields family (field_nodes.hpp).
    Fields,
};

// How many instances of a node type one composition may hold (task S1, item 5). The composition's
// single evaluation endpoint and its single layer-stack operator are structural singletons: a
// second one is not a graph a composition can mean, which is why the refusal belongs to the
// definition rather than to each surface that offers an Add.
enum class NodeCardinality : std::uint8_t {
    Many,
    OnePerComposition,
};

enum class NodeLoweringKind {
    Solid,
    Checker,
    Gradient,
    Shape,
    Text,
    ImageSource,
    ImageEffect,
    VideoSource,
    AudioSource,
    // #569: an audio effect between an audio source and a Layer's audio input.
    AudioEffect,
    CompositionSource,
    LayerOutput,
    LayerStack,
    CompositionOutput,
    // #502: an Adjustment's Image output is the stack below the Merge entry its chain lands in.
    Adjustment,
    // The value-graph lowerings (task S7). Each compiles to a runtime::CompiledValueOperation, not
    // a CompiledOperation: these produce ONE value per frame, not pixels, and they are evaluated in
    // their own pass before any image operation reads one.
    //
    // They are grouped by the KERNEL they reach rather than one per node type, so the twenty-odd
    // library nodes share a handful of lowerings the way the Solid and Text sources share
    // compiledColorParameter(): a Switch over Scalar and a Switch over Colour are the same
    // operation on different storage, and giving each its own lowering would be the same code
    // twice.
    ValueConstant,
    ValueTime,
    ValueScalarMath,
    ValueVectorMath,
    ValueVectorReduce,
    ValueMapRange,
    ValueClamp,
    ValueMix,
    ValueColorMix,
    ValueCompare,
    ValueSwitch,
    ValueSeparate,
    ValueCombine,
    ValueRandom,
    ValueReroute,
    // The image-pass readout is evaluated after image bounds exist. It is still a value node, but
    // it has a transport Image input rather than a parameter-backed operand, so it has its own
    // lowering instead of being forced through the generic utility descriptor table.
    ValueBoundsReadout,
    // Task UTIL-1's conversion, string, logic, numeric and readout library. ONE lowering for all of
    // it, because every one of those nodes has the same shape -- a fixed operand list in, a fixed
    // output list out, and a kernel selected by the node's own TYPE rather than by a stored
    // operation. The shape itself lives in document::valueUtilityDescriptors(), so the definition,
    // the validation, the lowering and the kernel all read one table instead of four copies of it.
    ValueUtility,
    Unsupported,
    // ADR 0028: the Mask node (path coverage over the composition canvas).
    Mask,
    // ADR 0026: Shuffle, Channel Copy and Remove. Channel algebra compiles away; the pixel work is
    // the Shuffle gather (runtime::CompiledChannelShuffle).
    Channel,
    // #364: the Cryptomatte node (runtime::CompiledCryptomatte).
    Cryptomatte,
    // #490: the Image Preview inspection sink. It lowers to nothing: it has no output, so no plan
    // reaches it, and the interface previews its input through a Viewer-only inspection plan.
    ImagePreview,
    // #525: a Fields node. Its field outputs compile into the field program of the raster or
    // sample that reads them; its `image` and `mask` outputs (and Rasterize Field's) are rasters.
    Field,
    // #525 Field Sample: a field read at one position, a value (runtime::CompiledValueFieldSample).
    ValueFieldSample,
    // #587 Test Card: a source drawn procedurally by one field raster over a canvas the size of the
    // composition (or a custom size), then the node's signal color space into the working space.
    TestCard,
    // #383: Mask Combine and Image to Mask (matte_nodes.hpp), lowered to image combines.
    Matte,
    // #342: a Node Group instance. It has no operation of its own: the compiler replaces it with
    // the nodes of the group it names (node_group_nodes.hpp) before anything is lowered. One that
    // cannot be replaced is a compile error naming why.
    NodeGroup,
    // #342: a group definition's Group Input or Group Output. Each republishes its value slots
    // through the value graph; its pixel slots are edges the expansion has already joined.
    GroupBoundary,
};

// Whether this lowering belongs to the value graph rather than the image chain. The compiler reads
// it to split one reachable node set into the two passes, so the split is declared beside the
// lowerings instead of being an ever-growing condition at the call site.
[[nodiscard]] constexpr bool isValueLowering(const NodeLoweringKind lowering) noexcept {
    switch (lowering) {
    case NodeLoweringKind::ValueConstant:
    case NodeLoweringKind::ValueTime:
    case NodeLoweringKind::ValueScalarMath:
    case NodeLoweringKind::ValueVectorMath:
    case NodeLoweringKind::ValueVectorReduce:
    case NodeLoweringKind::ValueMapRange:
    case NodeLoweringKind::ValueClamp:
    case NodeLoweringKind::ValueMix:
    case NodeLoweringKind::ValueColorMix:
    case NodeLoweringKind::ValueCompare:
    case NodeLoweringKind::ValueSwitch:
    case NodeLoweringKind::ValueSeparate:
    case NodeLoweringKind::ValueCombine:
    case NodeLoweringKind::ValueRandom:
    case NodeLoweringKind::ValueReroute:
    case NodeLoweringKind::ValueBoundsReadout:
    case NodeLoweringKind::ValueUtility:
    case NodeLoweringKind::ValueFieldSample:
    case NodeLoweringKind::GroupBoundary:
        return true;
    case NodeLoweringKind::NodeGroup:
        return false;
    case NodeLoweringKind::Field:
    case NodeLoweringKind::Shape:
    case NodeLoweringKind::Solid:
    case NodeLoweringKind::Checker:
    case NodeLoweringKind::Gradient:
    case NodeLoweringKind::Text:
    case NodeLoweringKind::VideoSource:
    case NodeLoweringKind::ImageSource:
    case NodeLoweringKind::ImageEffect:
    case NodeLoweringKind::AudioSource:
    case NodeLoweringKind::AudioEffect:
    case NodeLoweringKind::CompositionSource:
    case NodeLoweringKind::LayerOutput:
    case NodeLoweringKind::LayerStack:
    case NodeLoweringKind::CompositionOutput:
    case NodeLoweringKind::Unsupported:
    case NodeLoweringKind::Mask:
    case NodeLoweringKind::Channel:
    case NodeLoweringKind::Cryptomatte:
    case NodeLoweringKind::ImagePreview:
    case NodeLoweringKind::Adjustment:
    case NodeLoweringKind::Matte:
    case NodeLoweringKind::TestCard:
        return false;
    }
    return false;
}

struct NodeDefinition {
    NodeTypeKey key;
    NodeLoweringKind lowering = NodeLoweringKind::Unsupported;
    std::vector<InputPortDefinition> inputs;
    std::vector<OutputPortDefinition> outputs;
    std::vector<ParameterDefinition> parameters;
    std::optional<LayerSlotInputDefinition> layerSlotInput;
    NodeCardinality cardinality = NodeCardinality::Many;
    NodeCategory category = NodeCategory::Utilities;
    // The audio transport for a Layer Stack shares the stable layer slots with the image
    // transport. It is appended so existing aggregate-initialized extension definitions remain
    // source-compatible.
    std::optional<LayerSlotInputDefinition> audioLayerSlotInput{};

    friend bool operator==(const NodeDefinition&, const NodeDefinition&) = default;
};

// #543: the gradient parameter of `definition` whose stop `port` names (gradientStopPortName), or
// null when `port` is not a stop socket of one of its gradient parameters.
[[nodiscard]] const ParameterDefinition*
gradientParameterOfStopPort(const NodeDefinition& definition, std::string_view port);
// The input `definition` declares under `port`: one of its inputs or, #543, the colour (Color) or
// position (Scalar) socket of a stop of one of its gradient parameters. Every connect and
// validation path that asks for a port's declaration asks this.
[[nodiscard]] std::optional<InputPortDefinition> findInputPort(const NodeDefinition& definition,
                                                               std::string_view port);

enum class NodeRegistrationStatus {
    Registered,
    InvalidDefinition,
    DuplicateDefinition,
    Frozen,
};

class NodeDefinitionRegistry final {
  public:
    NodeDefinitionRegistry() = default;
    NodeDefinitionRegistry(const NodeDefinitionRegistry&) = delete;
    NodeDefinitionRegistry& operator=(const NodeDefinitionRegistry&) = delete;
    NodeDefinitionRegistry(NodeDefinitionRegistry&&) = delete;
    NodeDefinitionRegistry& operator=(NodeDefinitionRegistry&&) = delete;

    [[nodiscard]] NodeRegistrationStatus registerDefinition(NodeDefinition definition);
    void freeze();

    [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }
    [[nodiscard]] std::span<const NodeDefinition> definitions() const noexcept {
        return definitions_;
    }
    [[nodiscard]] const NodeDefinition* find(std::string_view typeId,
                                             std::uint32_t schemaVersion) const noexcept;
    [[nodiscard]] bool containsType(std::string_view typeId) const noexcept;

  private:
    std::vector<NodeDefinition> definitions_;
    bool frozen_ = false;
};

[[nodiscard]] bool registerBuiltInNodeDefinitions(NodeDefinitionRegistry& registry);
// Unknown module types retain the existing preservation contract. Built-in types must use
// their current registered version; retired per-kind reroutes have no supported version.
[[nodiscard]] bool isSupportedNodeVersion(std::string_view typeId, std::uint32_t version);

[[nodiscard]] const NodeDefinitionRegistry& builtInNodeDefinitions();

} // namespace bloom::document
