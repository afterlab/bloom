#pragma once

#include <bloom/media/cache/source_mirror.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

// The user's disk-cache settings in a form every Bloom process can read (#718). The application
// edits them in Preferences and keeps them in its own settings store; it also publishes a copy
// as a small text file in the per-user cache folder, which the tools without that store
// (`bloom-cli`, `bloom-mcp`, the Python module) read, so a headless or scripted render follows
// the same "Keep local copies of source media" choice and uses the same local copies.
namespace bloom::media::cache {

struct SourceMirrorUserSettings final {
    bool enabled = true;
    // An absolute override of the cache folder; empty uses the platform cache directory plus a
    // "media" leaf.
    std::filesystem::path directory;
    // 0: derived from the cache volume's free space.
    std::uint64_t byteBudget = 0;
    LocalCopyPolicy policy{};
    friend bool operator==(const SourceMirrorUserSettings&,
                           const SourceMirrorUserSettings&) = default;
};

// The default cache folder: bloom::platform::userCacheDirectory("bloom") / "media", or nullopt
// when the platform resolves no per-user cache folder.
[[nodiscard]] std::optional<std::filesystem::path> defaultSourceMirrorDirectory();

// The published copy lives at `<default cache folder>/settings.v1`. Writing replaces it
// atomically; a platform without a cache folder publishes nothing (false). Reading returns the
// defaults when the file is absent or unreadable, and skips lines it does not understand.
bool publishSourceMirrorSettings(const SourceMirrorUserSettings& settings);
[[nodiscard]] SourceMirrorUserSettings publishedSourceMirrorSettings();
// The same text format on a given file, for tests.
bool writeSourceMirrorSettings(const std::filesystem::path& file,
                               const SourceMirrorUserSettings& settings);
[[nodiscard]] SourceMirrorUserSettings readSourceMirrorSettings(const std::filesystem::path& file);

// Builds the mirror `settings` describe, or null when it is disabled or no folder resolves:
// "no disk cache this session", never a failure.
[[nodiscard]] std::shared_ptr<SourceMirror>
makeSourceMirror(const SourceMirrorUserSettings& settings);
// The mirror of the published settings: what a headless tool attaches to its evaluators.
[[nodiscard]] std::shared_ptr<SourceMirror> openUserSourceMirror();

} // namespace bloom::media::cache
