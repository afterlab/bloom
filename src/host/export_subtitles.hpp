#pragma once
// #752: an export's subtitle tracks (docs/architecture/subtitles.md, "Export"). Private to
// src/host: the export drivers prepare the tracks before rendering and publish the sidecar files
// after the output's own files.
//
// Preparation (a task): compile the snapshot, run the track pass over the export's range
// (runtime::collectSubtitleTracks), write each sidecar format of each track into memory and parse
// it straight back, and turn each track into the embedded track the media worker muxes. Nothing
// reaches the destination yet. Publication (a task, after the output published): each sidecar is
// staged beside the export, read back, parsed and compared with its track again, then published
// atomically under the output's overwrite policy.

#include <bloom/core/sha256.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/export_verification.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::runtime {
class SnapshotCompiler;
}

namespace bloom::host {
class PublicationCoordinator;
}

namespace bloom::host::detail {

// The ISO 639-2 code of a BCP 47 tag's primary language subtag: the terminology form (MOV, MP4)
// or the bibliographic one (Matroska and WebM). A three-letter subtag is kept; "und" when the tag
// names no language Bloom knows.
[[nodiscard]] std::string iso6392Language(std::string_view bcp47, bool bibliographic);

// The language part of a sidecar's name: the BCP 47 tag with anything but letters, digits and
// '-' removed ("und" when nothing is left).
[[nodiscard]] std::string sidecarLanguage(std::string_view bcp47);

// `<base>.<lang>.<ext>` beside `basePath` (its stem), with `.<n>` before the extension when
// `number` is set (two tracks share a language).
[[nodiscard]] std::filesystem::path sidecarPath(const std::filesystem::path& basePath,
                                                std::string_view language,
                                                std::optional<std::size_t> number,
                                                document::ExportSubtitleFormat format);

// One sidecar file, written into memory and parsed back.
struct ExportSubtitleSidecar final {
    document::NodeId node;
    std::string language; // BCP 47
    std::string title;
    document::ExportSubtitleFormat format = document::ExportSubtitleFormat::Srt;
    std::filesystem::path target;
    std::string text;
    // The composition's frame rate (the output frames' rate).
    media::provider::Rational rate{24, 1};
    // What each cue shows, in output frames [first, end) with its plain text: what the file must
    // read back as.
    std::vector<media::provider::SubtitleCueV1> expected;
};

struct ExportSubtitlePlan final {
    std::vector<ExportSubtitleSidecar> sidecars;
    // In track order; empty unless the output embeds.
    std::vector<media::provider::SubtitleTrackV1> embedded;
    // The embedded tracks for the results (node, language, title, cue count).
    std::vector<ExportSubtitleFile> embeddedFiles;
    // Sentences for the export log: the tracks found, a node named that never shows, a format
    // that cannot carry part of a cue.
    std::vector<std::string> notes;
};

struct ExportSubtitleFailure final {
    ExportFailureCode code = ExportFailureCode::Internal;
    std::string diagnostic;
    std::string reason;
    bool cancelled = false;
};

using ExportSubtitlePlanOutcome = std::variant<ExportSubtitlePlan, ExportSubtitleFailure>;

struct ExportSubtitleRequest final {
    document::Snapshot snapshot;
    document::CompositionId composition;
    // Composition frames, inclusive.
    std::uint64_t firstFrame = 0;
    std::uint64_t lastFrame = 0;
    document::ExportSubtitles settings;
    document::ExportFormat format = document::ExportFormat::ProResMov;
    // The destination before sequence naming (ExportResolvedDestination::basePath).
    std::filesystem::path basePath;
    // How an existing sidecar is treated (the output's overwrite policy; see the driver).
    platform::ArtifactOverwritePolicy sidecarPolicy = platform::ArtifactOverwritePolicy::CreateOnly;
};

// Runs on a task. `progress` gets frames walked and the range's frame count.
[[nodiscard]] ExportSubtitlePlanOutcome
prepareExportSubtitles(const ExportSubtitleRequest& request,
                       const runtime::SnapshotCompiler& compiler,
                       const runtime::CancellationToken& cancellation,
                       const std::function<void(std::uint64_t, std::uint64_t)>& progress);

struct ExportSidecarPublication final {
    // Every sidecar published and verified, in order.
    std::vector<ExportSubtitleFile> files;
    // Set when a sidecar could not be published or did not read back as its track; the sidecars
    // before it stay published.
    std::optional<ExportSubtitleFailure> failure;
};

// Runs on a blocking-I/O task, after the output's own files were published.
[[nodiscard]] ExportSidecarPublication publishExportSubtitleSidecars(
    PublicationCoordinator& coordinator, platform::StagedArtifactCoordinator& artifacts,
    const std::vector<ExportSubtitleSidecar>& sidecars, platform::ArtifactOverwritePolicy policy,
    const runtime::CancellationToken& cancellation);

// Whether the file `text` (as `format`) reads back as `expected`: the same number of cues, each
// on the same output frames at `rate` and with the same plain text. Empty when it does; otherwise
// the first difference as a sentence. Shared by the in-memory check and the published file's.
[[nodiscard]] std::string
sidecarDifference(std::string_view text, document::ExportSubtitleFormat format,
                  const std::vector<media::provider::SubtitleCueV1>& expected,
                  media::provider::Rational rate);

} // namespace bloom::host::detail
