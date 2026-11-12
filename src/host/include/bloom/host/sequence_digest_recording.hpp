#pragma once

#include <bloom/document/document.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>

// #495 (docs/architecture/project-format.md "Verified Member Digests"). A save records what the
// process memo of ADR 0025 (platform::verifiedSourceDigests()) already proved about image-sequence
// members, so a reopened project knows each verified member's digest and its stamp. It never hashes
// and never waits: one stat and one memo lookup per member that lacks one of the two. Only the
// bytes a save publishes change; the live document, its revision, dirty state and undo history are
// not touched.
namespace bloom::host {

struct VerifiedDigestRecording final {
    // Pending members (stamp only) that gained the digest the memo holds for that stamp.
    std::size_t digestsRecorded = 0;
    // Legacy members (digest only) that gained the stamp of the file the memo verified.
    std::size_t stampsRecorded = 0;
};

// Where a session's project-relative locators resolve: the project file's folder, or the process
// working directory for a project not yet saved (the Assets controller's base directory).
[[nodiscard]] std::filesystem::path
assetBaseDirectory(const std::optional<std::filesystem::path>& projectPath);

// The project with every image-sequence member completed from the memo, or nullopt when nothing
// changes. Per member, resolved as the runtime resolves it (the locator under `projectDirectory`,
// else its relink hint) and observed once (platform::observeSource):
//  - pending (stamp only): gains the memo digest when the file still carries the member's stamp;
//  - legacy (digest only): gains the observed stamp when the memo digest equals its digest (a
//    different memo digest means the content changed, and the member is left as it is);
//  - verified (both): unchanged.
// A member the memo does not know, a missing file and every other asset are left as they are.
[[nodiscard]] std::optional<document::Project>
recordVerifiedSequenceDigests(const document::Project& project,
                              const std::filesystem::path& projectDirectory,
                              VerifiedDigestRecording* recording = nullptr);

// The same over a captured snapshot: a detached snapshot (owned by no live document) of the
// completed project with the captured allocator state, or nullopt when nothing changes.
[[nodiscard]] std::optional<document::Snapshot>
snapshotWithVerifiedSequenceDigests(const document::Snapshot& snapshot,
                                    const std::filesystem::path& projectDirectory,
                                    VerifiedDigestRecording* recording = nullptr);

} // namespace bloom::host
