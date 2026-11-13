#pragma once

#include <bloom/media/cache/source_mirror_settings.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

class QSettings;

namespace bloom::runtime {
class OperationCache;
} // namespace bloom::runtime

// Settings and lifecycle for the media disk cache (docs/architecture/media-io.md "Disk cache").
// These keys are edited by the Preferences window (Edit | Preferences... -> Memory & Caches, docs/
// user-guide/preferences.md), which reads and writes them through
// bloom::ui::ApplicationPreferences; this header remains the reader that owns their parsing and the
// purge command. They are read once at startup, so a change takes effect after restart, exactly
// like the RAM preview and operation-cache byte budgets.
namespace bloom::ui {

// "media/disk-cache-enabled", default true.
[[nodiscard]] bool mediaDiskCacheEnabledFromSettings(const QSettings& settings);

// "media/disk-cache-directory": an absolute path override. Empty, missing, or relative reads as
// "use the platform default" (bloom::platform::userCacheDirectory() plus a "media" leaf).
[[nodiscard]] std::filesystem::path mediaDiskCacheDirectoryFromSettings(const QSettings& settings);

// "media/disk-cache-budget-bytes": missing, zero, unparseable, or negative reads as `fallback`
// (the caller resolves that from physical disk free space via
// media::cache::defaultSourceMirrorByteBudget()).
[[nodiscard]] std::uint64_t mediaDiskCacheByteBudgetFromSettings(const QSettings& settings,
                                                                 std::uint64_t fallback);

// "media/local-copies" ("automatic", "always" or "never"; anything else reads as automatic) and
// "media/local-copies-folders" (a list of "keep:<absolute folder>" and "skip:<absolute folder>"):
// the preference "Keep local copies of source media" and its per-folder overrides (#718).
[[nodiscard]] media::cache::LocalCopyPolicy localCopyPolicyFromSettings(const QSettings& settings);
void writeLocalCopyPolicy(QSettings& settings, const media::cache::LocalCopyPolicy& policy);

// Every disk-cache setting as the Qt-free record the cache module and the headless tools use.
[[nodiscard]] media::cache::SourceMirrorUserSettings
sourceMirrorSettingsFromSettings(const QSettings& settings);

// Builds the disk cache (the local mirror of source media on slow storage) from the current
// settings and the platform cache directory, and publishes the settings for the tools that have
// no settings store of their own (`bloom-cli`, `bloom-mcp`, Python). Returns nullptr when the
// cache is disabled in settings OR no directory can be resolved at all (no override and no
// platform default) -- both are "disk cache unavailable this session", never a startup failure
// (derived caches are optional runtime state per media-io.md).
[[nodiscard]] std::shared_ptr<media::cache::SourceMirror>
makeMediaDiskCacheFromSettings(const QSettings& settings);

// The "Purge media cache" effect: clears every local copy of source media AND the evaluator's
// in-memory decoded-media entries. This is the owning API the purge worker calls; it must never
// run on the UI thread because SourceMirror::clear() removes the entries from disk. Source files
// are never opened, read, or changed -- only derived data is dropped, so a purged source is
// simply read again on next use. Either pointer may be null. Returns true when at least one
// store was present and cleared.
bool purgeMediaCaches(media::cache::SourceMirror* cache, runtime::OperationCache* operationCache);

} // namespace bloom::ui
