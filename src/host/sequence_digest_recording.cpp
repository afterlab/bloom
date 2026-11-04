#include <bloom/host/sequence_digest_recording.hpp>

#include <bloom/core/file_uri.hpp>
#include <bloom/platform/source_identity.hpp>

#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace bloom::host {
namespace {
// media::resolveImagePath's rule, which host cannot include (module boundary): an external
// locator holds its absolute file URI; a project-relative one resolves under the project folder
// when the file is there, else through its relink hint.
std::filesystem::path resolveLocator(const document::AssetLocator& locator,
                                     const std::filesystem::path& projectDirectory) {
    const std::string_view path = locator.path;
    if (path.find(':') != std::string_view::npos)
        return core::absolutePathFromFileUri(path).value_or(std::filesystem::path{});
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos)
        return {};
    if (!projectDirectory.empty()) {
        const auto candidate =
            projectDirectory / std::filesystem::path(std::u8string(
                                   reinterpret_cast<const char8_t*>(path.data()), path.size()));
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return core::absolutePathFromFileUri(locator.relinkHint).value_or(std::filesystem::path{});
}

// Completes one member from the memo; true when it changed.
bool complete(document::AssetSequenceMember& member, const std::filesystem::path& projectDirectory,
              VerifiedDigestRecording& recording) {
    if (member.contentDigest.has_value() == member.stamp.has_value())
        return false;
    const auto path = resolveLocator(member.locator, projectDirectory);
    if (path.empty())
        return false;
    const auto observed = platform::observeSource(path);
    if (!observed.identity || !observed.digest)
        return false;
    if (member.stamp) {
        if (observed.identity->size != member.stamp->size ||
            observed.identity->modifiedNanoseconds != member.stamp->modifiedNanoseconds)
            return false;
        member.contentDigest = *observed.digest;
        ++recording.digestsRecorded;
        return true;
    }
    if (*observed.digest != *member.contentDigest)
        return false;
    member.stamp =
        document::AssetFileStamp{observed.identity->size, observed.identity->modifiedNanoseconds};
    ++recording.stampsRecorded;
    return true;
}
} // namespace

std::filesystem::path assetBaseDirectory(const std::optional<std::filesystem::path>& projectPath) {
    if (projectPath)
        return projectPath->parent_path();
    std::error_code error;
    auto current = std::filesystem::current_path(error);
    return error ? std::filesystem::path{} : current;
}

std::optional<document::Project>
recordVerifiedSequenceDigests(const document::Project& project,
                              const std::filesystem::path& projectDirectory,
                              VerifiedDigestRecording* const recording) {
    VerifiedDigestRecording counts;
    std::optional<document::Project> completed;
    for (const auto& asset : project.assets()) {
        if (asset.kind != document::AssetKind::Sequence)
            continue;
        std::optional<document::AssetRecord> updated;
        for (std::size_t index = 0; index < asset.manifest.members.size(); ++index) {
            auto member = asset.manifest.members[index];
            if (!complete(member, projectDirectory, counts))
                continue;
            if (!updated)
                updated = asset;
            updated->manifest.members[index] = std::move(member);
        }
        if (!updated)
            continue;
        if (!completed)
            completed.emplace(project);
        if (auto* target = completed->findAsset(asset.id))
            *target = std::move(*updated);
    }
    if (recording != nullptr)
        *recording = counts;
    return completed;
}

std::optional<document::Snapshot>
snapshotWithVerifiedSequenceDigests(const document::Snapshot& snapshot,
                                    const std::filesystem::path& projectDirectory,
                                    VerifiedDigestRecording* const recording) {
    auto completed = recordVerifiedSequenceDigests(snapshot.project(), projectDirectory, recording);
    if (!completed)
        return std::nullopt;
    // A detached document only to own the completed state; it is never committed to and never
    // reaches the session.
    const document::Document detached(std::move(*completed), snapshot.ids().highWater());
    return detached.snapshot();
}

} // namespace bloom::host
