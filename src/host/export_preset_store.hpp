#pragma once

#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_presets.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Private to src/host: the per-user preset store (ADR 0033, "Preferences") and the encoder probe.
// Every function here performs I/O and runs only on a blocking-I/O worker.
namespace bloom::host {

inline constexpr std::string_view kExportPresetStoreFileName = "export-presets.json";
inline constexpr std::string_view kExportPresetStoreFormat = "bloom.export-presets";
inline constexpr std::uint32_t kExportPresetStoreVersion = 1;

struct ExportUserPresetRecord final {
    std::string id;
    std::string displayName;
    document::ExportOutputSettings settings;
};

// A missing file is an empty store. A corrupt, foreign or newer-versioned file is left untouched
// and reported as nullopt (the catalogue then refuses writes with StoreUnavailable).
[[nodiscard]] std::optional<std::vector<ExportUserPresetRecord>>
readExportUserPresets(const std::filesystem::path& path);
// Atomic replace through a sibling temporary file. On Windows another program (a scanner, an
// indexer, a backup) can hold the store open for a moment, which refuses the replace with a
// sharing violation or access denied: that replace is tried again, with ReplaceFileW (MoveFileExW
// for a store that does not exist yet), for at most kExportPresetStoreReplaceWindow, calling
// `refused` with the attempt's number before each new attempt. Empty on success, else what went
// wrong with the system's cause; the temporary is removed.
inline constexpr std::chrono::milliseconds kExportPresetStoreReplaceWindow{2000};
[[nodiscard]] std::optional<std::string>
writeExportUserPresets(const std::filesystem::path& path,
                       const std::vector<ExportUserPresetRecord>& records,
                       const std::function<void(int)>& refused = {});

// Serialization shared with exportJobScript(): the 1.29 `settings` object as compact JSON.
[[nodiscard]] std::string exportSettingsJson(const document::ExportOutputSettings& settings);

// Probes this computer's encoders (VA-API render nodes and their encode entry
// points through the media worker, the worker itself). Worker thread only.
[[nodiscard]] ExportEncoderStatus probeExportEncoders(const ExportEncoderStatus& initial);

} // namespace bloom::host
