#pragma once

// #513: storing the result of a tracking job. One undo step creates the track as a data block (a
// Table: one row per frame; the columns frame, score, x and y, sorted by name) and keys the Stabilize node's position with the
// same positions, so applying the track is the node's ordinary animation: editable, undoable and
// rendered by the same field raster as any keyed value. The block is the durable record of the track
// (who made it, with which settings, over which frames); the keys are how it is applied.

#include <bloom/commands/transaction.hpp>
#include <bloom/document/ids.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bloom::commands {

struct TrackRow final {
    std::int64_t frame = 0;
    // Pixels from the composition's centre, y down.
    double x = 0.0;
    double y = 0.0;
    double score = 1.0;
};

struct TrackStorage final {
    document::CompositionId composition;
    document::NodeId node;
    // The node's `position` parameter, which receives the keys.
    document::ParameterId position;
    std::uint32_t frameRateNumerator = 24;
    std::uint32_t frameRateDenominator = 1;
    // The tracker's settings as text, digested into the block's provenance so two tracks made with
    // different settings can be told apart.
    std::string settings;
    std::string createdAt;
};

// The transaction that stores `rows` (at least one, frames ascending): the data block and the keys.
[[nodiscard]] Transaction makeStoreTrackTransaction(const TrackStorage& storage,
                                                    const std::vector<TrackRow>& rows);

inline constexpr const char* kTrackBlockTypeId = "bloom.point-track";

} // namespace bloom::commands
