#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace bloom::platform {

// Resolves the platform's per-user cache root for Bloom-owned derived data (currently the media
// disk cache; see docs/architecture/media-io.md "Disk cache"). The directory itself is NOT
// created by this function -- callers create it (and its subdirectories) lazily on first use.
//
// Linux: `$XDG_CACHE_HOME/<appSubdirectory>` when `XDG_CACHE_HOME` is set to an absolute path,
// else `$HOME/.cache/<appSubdirectory>`.
// Windows: `%LOCALAPPDATA%/<appSubdirectory>/Cache`.
// macOS takes the Linux rule (`$HOME/.cache/<appSubdirectory>` unless `XDG_CACHE_HOME` is set).
// Any platform without a resolvable home/local-app-data variable: `nullopt`. Derived
// caches are always optional runtime state (docs/architecture/media-io.md), so a caller must
// treat a missing directory as "disk cache unavailable" rather than fabricate a relative path.
[[nodiscard]] std::optional<std::filesystem::path>
userCacheDirectory(std::string_view appSubdirectory = "bloom");

// The per-user directory of the GPU device's persisted pipeline cache (#457):
// `userCacheDirectory("bloom")/gpu-pipeline-cache`, or empty when no cache root resolves, in which
// case the pipeline cache stays in memory. Not created here.
[[nodiscard]] std::filesystem::path gpuPipelineCacheDirectory();

} // namespace bloom::platform
