#pragma once

// #637 Write to a Path: what a Path-producing value node answers at one time. The node may feed
// nothing at all, so the query compiles a private copy of the project in which the output is read by
// a muted Shape Source (the way Properties reads a driver), then evaluates the value graph. It runs
// on the calling thread and observes `cancellation`; callers keep it off the UI thread.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <optional>
#include <string>
#include <string_view>

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
                                          const runtime::CancellationToken& cancellation);

} // namespace bloom::scripting
