#pragma once

// ADR 0026 Viewer channel inspection: the worker-side listing behind the Viewer's channel menu.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/channel_inspection.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime {

// One Viewer menu entry: a single channel (grey) or a layer's three colour/vector channels (RGB).
struct ChannelInspectionChoice final {
    std::string label;
    std::vector<std::string> channels;
    friend bool operator==(const ChannelInspectionChoice&,
                           const ChannelInspectionChoice&) = default;
};

struct ChannelInspectionListing final {
    std::vector<ChannelInspectionChoice> choices;
    // #487: every resolved channel name and every layer (empty: the root layer), in channel order,
    // for the Shuffle slot pickers and the channel/layer checklists.
    std::vector<std::string> channels;
    std::vector<std::string> layers;
    // Why the channel set could not be listed (empty on success).
    std::string diagnostic;
};

class SnapshotCompiler;

// #487: the channel an empty Shuffle slot reads (`slot` 0-3: R, G, B, A) -- the root layer's when
// the root carries R, G and B, else the first layer (in channel order) that does, as #478 defaults
// an Image's colour layer. Nullopt when no layer carries R, G and B, or (alpha) that layer has no
// A.
[[nodiscard]] std::optional<std::string>
defaultShuffleSlotChannel(std::span<const std::string> channelNames, std::size_t slot);

// Worker-only: compiles `node`'s ChannelSet `port`, captures and probes its sources at `time`
// (identity-verified, like evaluation) and lists the resolved channels and RGB layers.
[[nodiscard]] ChannelInspectionListing
listChannelInspectionChoices(const SnapshotCompiler& compiler, const document::Snapshot& snapshot,
                             document::CompositionId composition, document::NodeId node,
                             const std::string& port, const std::filesystem::path& assetBase,
                             core::RationalTime time, const EvaluationColorIntent& colorIntent,
                             const CancellationToken& cancellation);

} // namespace bloom::runtime
