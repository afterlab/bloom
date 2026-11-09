#include <bloom/commands/track_operations.hpp>

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/data_block_operations.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/data_block.hpp>

#include <cstddef>

namespace bloom::commands {

Transaction makeStoreTrackTransaction(const TrackStorage& storage, const std::vector<TrackRow>& rows) {
    Transaction transaction("Track Point");
    document::DataBlockTable table;
    // A table's columns are kept sorted by name.
    table.columns = {{"frame", document::DataBlockValueKind::Integer, {}},
                     {"score", document::DataBlockValueKind::Float64, {}},
                     {"x", document::DataBlockValueKind::Float64, {}},
                     {"y", document::DataBlockValueKind::Float64, {}}};
    std::vector<ParameterKey> keys;
    keys.reserve(rows.size());
    for (const auto& row : rows) {
        table.columns[0].values.emplace_back(row.frame);
        table.columns[1].values.emplace_back(row.score);
        table.columns[2].values.emplace_back(row.x);
        table.columns[3].values.emplace_back(row.y);
        const auto time = core::RationalTime::create(
            row.frame * static_cast<std::int64_t>(storage.frameRateDenominator),
            static_cast<std::int64_t>(storage.frameRateNumerator));
        if (time)
            keys.push_back({*time, document::Vec2d{row.x, row.y},
                            document::KeyframeInterpolation::Linear});
    }

    document::DataBlockRecord block;
    block.kind = document::DataBlockKind::Table;
    block.owner = storage.composition;
    block.subject = storage.node;
    block.typeId = kTrackBlockTypeId;
    block.schemaVersion = {1, 0};
    block.mediaType = "application/vnd.bloom.point-track";
    block.payload = std::move(table);
    block.tags = {"track"};
    document::DataBlockProducer producer{"bloom.point-tracker", "1", {}};
    std::vector<std::byte> text;
    text.reserve(storage.settings.size());
    for (const auto c : storage.settings)
        text.push_back(static_cast<std::byte>(c));
    if (const auto digest = core::Sha256Hasher::hash(text))
        producer.parametersDigest = *digest;
    block.provenance.source = std::move(producer);
    block.provenance.createdAt = storage.createdAt;
    transaction.emplace<CreateDataBlock>(std::move(block));
    transaction.emplace<SetParameterKeys>(storage.composition, storage.position, std::move(keys), true);
    return transaction;
}

} // namespace bloom::commands
