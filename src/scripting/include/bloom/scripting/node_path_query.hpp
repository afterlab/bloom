#pragma once

// #637 Write to a Path: what a Path-producing value node answers at one time. The node may feed
// nothing at all, so the query compiles a private copy of the project in which the output is read by
// a muted Shape Source (the way Properties reads a driver), then evaluates the value graph. It runs
// on the calling thread and observes `cancellation`; callers keep it off the UI thread.

#include <bloom/commands/keyframe_bake_operations.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::scripting {

struct NodePathQuery final {
    std::optional<document::PathValue> path;
    // Why there is no path (a missing node or port, a compile or evaluation failure, a diagnostic
    // the node raised); empty on success.
    std::string diagnostic;
    bool cancelled = false;
};

// The Path output `port` of `node` at `time`. An empty `port` is the node's first Path output.
[[nodiscard]] NodePathQuery queryNodePath(const document::Snapshot& snapshot,
                                          document::CompositionId composition,
                                          document::NodeId node, std::string_view port,
                                          core::RationalTime time,
                                          const runtime::CancellationToken& cancellation,
                                          const std::filesystem::path& assetBaseDirectory = {});

// #627: what a number-list output of a value node answers at one time. Nothing has to read the
// node: the plan is compiled with it as a value readout.
// `assetBaseDirectory` resolves relative asset locators for a node that reads media (an audio
// analysis node renders and analyzes its sound here, on the calling worker).
struct NodeNumberListQuery final {
    std::optional<document::NumberListValue> list;
    std::string diagnostic;
    bool cancelled = false;
};

// The Number List output `port` of `node` at `time`. An empty `port` is the node's first.
[[nodiscard]] NodeNumberListQuery queryNodeNumberList(const document::Snapshot& snapshot,
                                                      document::CompositionId composition,
                                                      document::NodeId node, std::string_view port,
                                                      core::RationalTime time,
                                                      const runtime::CancellationToken& cancellation,
                                                      const std::filesystem::path& assetBaseDirectory = {});

// #636 Bake to keyframes: what a Scalar output of a value node answers at every frame of the
// composition, first frame to last. The plan is compiled once with the node as a value readout
// and evaluated frame by frame on the calling worker; an audio analysis node renders and analyzes
// its sound there the first time it is asked.
struct NodeScalarKeysQuery final {
    // One key per frame, ascending; empty when the read failed.
    std::vector<commands::BakedKey> keys;
    std::string diagnostic;
    bool cancelled = false;
};

// The Scalar output `port` of `node` over the composition. An empty `port` is the node's first.
[[nodiscard]] NodeScalarKeysQuery queryNodeScalarKeys(const document::Snapshot& snapshot,
                                                      document::CompositionId composition,
                                                      document::NodeId node, std::string_view port,
                                                      const runtime::CancellationToken& cancellation,
                                                      const std::filesystem::path& assetBaseDirectory = {});

// The bake that is reading frames now, for a progress readout: frames read and frames to read.
// `framesTotal` is 0 when no bake is running. Never blocks.
struct NodeBakeActivity final {
    std::uint64_t framesDone = 0;
    std::uint64_t framesTotal = 0;
};
[[nodiscard]] NodeBakeActivity nodeBakeActivity() noexcept;

// #591: every output of a node at one time, as values -- what a Space Readout, a To Space or any other
// value node answers, read without driving a parameter or reading a card. Like the reads above,
// nothing has to consume the node: the plan is compiled with it as a value readout. Pictures, sound,
// masks, channel sets and fields carry no one value, so those outputs are listed with a note.
struct NodeOutputValue final {
    std::string name;
    document::SocketValueKind kind = document::SocketValueKind::Scalar;
    // The value at the time; none for an output that carries no one value, or one the node gave
    // nothing for (then `note` says why).
    std::optional<runtime::CompiledValue> value;
    std::string note;
};
struct NodeOutputsQuery final {
    // One entry per output the node's definition declares, in its order.
    std::vector<NodeOutputValue> outputs;
    // What the node itself reported at this time ("summary: detail"), with its outputs still read
    // (a node that cannot answer gives its documented fallback).
    std::vector<std::string> nodeDiagnostics;
    // Why nothing could be read (a missing node, a plan that does not compile); empty on success.
    std::string diagnostic;
    bool cancelled = false;
};

[[nodiscard]] NodeOutputsQuery queryNodeOutputs(const document::Snapshot& snapshot,
                                                document::CompositionId composition,
                                                document::NodeId node, core::RationalTime time,
                                                const runtime::CancellationToken& cancellation,
                                                const std::filesystem::path& assetBaseDirectory = {});

// The parameter value a computed value is written as (MCP and Python project both the same way);
// none for a space, which has no stored form.
[[nodiscard]] std::optional<document::ParameterValue>
parameterValueOf(const runtime::CompiledValue& value);

} // namespace bloom::scripting
