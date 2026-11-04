#pragma once

// ADR 0026: the compiled form of a ChannelSet subgraph. Channel algebra (Remove, Channel Copy and
// Shuffle's channel output) touches no pixels, so it compiles to this symbolic expression. It is
// resolved against the probed source channel lists at evaluation (host parameter resolution), and
// the only pixel work is the Shuffle that maps resolved channels into a process image.

#include <bloom/document/asset.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/render/channel_set_image.hpp>
#include <bloom/runtime/compiled_composition_source.hpp>
#include <bloom/runtime/operation_index.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

class CompiledCompositionPlan;

// One Image source's `channels` output: every channel of the selected part.
//
// #480: or, with `imageBacked`, an image packed as named channels -- an Image to Channel Set node
// or a Video source's `channels` output. Its channels are `imageChannels` (lane i is the image's
// stored red, green, blue or alpha) and are known when the graph compiles; their pixels are the
// stored samples of operation `image` (working-space, premultiplied). An absent `image` (muted,
// unconnected or empty) reads zero. Asset fields are unused. A Composition Output's channel layer
// carries its image as `imagePlan` instead: the plan re-rooted at that image, which the export
// evaluates through the frame's route (the operation itself is not part of the frame's plan).
//
// #480: a file source reached through Composition Source `channels` outputs (a precomp's channel
// layers) lists those references in `nesting`, outermost first. Entry 0 belongs to the plan that
// holds this expression; entry k+1 to the nested plan entry k names. The source is read at the
// innermost nested time (each mapping applied in turn: offset, scale, loop and its animated
// operands sampled on that plan's clock) with that nested plan's frame rate. Empty: the source is
// read at the time of the operation that reads it.
struct CompiledChannelSource final {
    document::NodeId sourceNodeId;
    std::optional<document::AssetRecord> asset;
    std::int64_t startFrame = 0;
    std::int64_t loopMode = 0;
    std::int64_t colorSpace = 0;
    std::string inputColorSpaceId{};
    std::string part{};
    bool imageBacked = false;
    std::optional<OperationIndex> image{};
    std::vector<render::ChannelDescriptor> imageChannels{};
    std::shared_ptr<const CompiledCompositionPlan> imagePlan{};
    std::vector<CompiledCompositionSource> nesting{};
    friend bool operator==(const CompiledChannelSource&, const CompiledChannelSource&) = default;
};

enum class CompiledChannelStepKind : std::uint8_t {
    // The channels of `sources[source]`.
    Source,
    // `steps[input]` without (or, with `keep`, only) the channels matching `select`.
    Remove,
    // `steps[input]` plus the channels of `steps[from]` matching `select`; a copied channel
    // replaces a same-named one.
    Copy,
    // Four channels named `<layer>.R/G/B/A` mapped from `steps[input]` by `mapping` (Shuffle's
    // `channels` output).
    Rename,
};

struct CompiledChannelStep final {
    CompiledChannelStepKind kind = CompiledChannelStepKind::Source;
    document::NodeId nodeId;
    std::uint32_t source = 0;
    std::uint32_t input = 0;
    std::uint32_t from = 0;
    std::string select{};
    bool keep = false;
    std::array<std::string, 4> mapping{};
    std::string layer{};
    friend bool operator==(const CompiledChannelStep&, const CompiledChannelStep&) = default;
};

// Steps are topologically ordered; every step names only earlier steps, and the last is the root.
struct CompiledChannelExpression final {
    std::vector<CompiledChannelSource> sources{};
    std::vector<CompiledChannelStep> steps{};
    friend bool operator==(const CompiledChannelExpression&,
                           const CompiledChannelExpression&) = default;
};

} // namespace bloom::runtime
