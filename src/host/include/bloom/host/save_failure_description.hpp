#pragma once

#include <bloom/host/copy_publication.hpp>
#include <bloom/host/save_publication.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/project/save_archive.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// Names why a Save or Save As did not write the project file (issue #737). Every typed failure the
// save pipeline reports -- the capture stage, every SavePublicationStage with every payload it can
// carry (nested staged-save and archive failures included), and a publication that failed before it
// replaced the file -- becomes one description with:
//
// - `stage`: a stable token path naming where the save stopped, for example "publication.preflight"
//   or "publication.staged-save.build.document-encode";
// - `cause`: a stable token naming the payload, for example "parent-not-directory" or
//   "value-count-exceeded";
// - `message`: one plain sentence for the artist that names both, for example "Save failed while
//   encoding document.json: the project holds more values than one project file can (the limit is
//   16000000)."
//
// The tokens are the machine-readable detail of the MCP `bloom.mcp.save-failed` diagnostic and are
// part of its contract: they never change spelling once released. The messages are what the File
// menu's failure dialog and the status bar show (docs/ux/ui-grammar.md, "Save And Recovery
// Messages"). Qt-free, so the UI and the MCP adapter read the same words.
namespace bloom::host {

struct SaveFailureDescription final {
    std::string stage;
    std::string cause;
    std::string message;

    friend bool operator==(const SaveFailureDescription&, const SaveFailureDescription&) = default;
};

// The description of one publication failure. `limits` are the limits the save ran with, so a
// limit-exceeded cause names the number that was exceeded.
[[nodiscard]] SaveFailureDescription
describeSavePublicationFailure(const SavePublicationFailure& failure,
                               const project::SaveArchiveLimits& limits = {});

// The description of a session save that did not publish the project file and accept it, or
// nullopt for one that did (including the outcomes that are not failures of this save: published
// with a durability warning, superseded by a newer save, cancelled before publication, and an
// external-modification conflict, which ProjectHost reports in their own words).
[[nodiscard]] std::optional<SaveFailureDescription>
describeSessionSaveFailure(const SessionSaveResult& result,
                           const project::SaveArchiveLimits& limits = {});

// Issue #748: the description of a Save a Copy that did not write the copy, in the same tokens and
// words ("copy.staged-copy.stage-write", "stage-write-failed", "Save a Copy failed while writing
// the temporary file: ..."), or nullopt for one that published it (a durability warning, a newer
// save superseding it and an external-modification conflict are reported in their own words).
[[nodiscard]] std::optional<SaveFailureDescription>
describeCopyPublicationFailure(const CopyPublicationResult& result);

// Issue #748: the description of a Save a Copy that `bloom-mcp` on stdio runs as a plain file copy
// staged beside the destination and renamed over it: "copy.staged-copy.stage-write" when the
// staged file could not be written, "copy.publish" when it could not replace the destination, with
// the cause ("no-space", "permission-denied", "read-only-file-system", "is-a-directory",
// "filesystem-error") and the system's own words.
[[nodiscard]] SaveFailureDescription describeFileCopyFailure(const std::error_code& error,
                                                             bool stagedFileWritten);

// The stable token of one platform staged-artifact error ("stage-write-failed", ...), shared with
// Save a Copy's own failure words.
[[nodiscard]] std::string_view stagedArtifactErrorToken(platform::StagedArtifactError error);

} // namespace bloom::host
