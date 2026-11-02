#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace bloom::host {

// An existing export target as a destination sample recorded it: its size and its modification
// time in std::filesystem::file_time_type ticks. A replacement the artist confirmed covers exactly
// this file (#453).
struct ExistingExportTargetV1 final {
    std::uint64_t byteSize = 0;
    std::int64_t modificationTicks = 0;
    friend bool operator==(const ExistingExportTargetV1&, const ExistingExportTargetV1&) = default;
};

// Blocking I/O, worker threads only: the record of the regular file at `path`, or nothing when no
// regular file is there or it cannot be read.
[[nodiscard]] std::optional<ExistingExportTargetV1>
sampleExistingExportTargetV1(const std::filesystem::path& path);

} // namespace bloom::host
