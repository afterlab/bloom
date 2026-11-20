#include <bloom/host/save_failure_description.hpp>
#include <bloom/host/copy_publication.hpp>

#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/manifest_decode.hpp>
#include <bloom/project/staged_copy.hpp>
#include <bloom/project/staged_save.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>
#include <bloom/project/zip_container_writer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

namespace bloom::host {
namespace {

// One stable token per enumerator, in declaration order. Each static_assert fails the build when
// an enumerator is added without its token, so no failure can ever be reported as "unknown".
// StagedArtifactError (src/platform/include/bloom/platform/staged_artifact.hpp)
constexpr std::array<std::string_view, 35> kStagedArtifactErrorTokens{
    "none",
    "invalid-configuration",
    "resource-unavailable",
    "unsupported-platform",
    "admission-limit",
    "target-record-limit",
    "target-identity-exhausted",
    "coordinator-mismatch",
    "invalid-target-path",
    "parent-resolution-failed",
    "parent-not-directory",
    "target-leaf-symlink",
    "target-leaf-not-regular",
    "target-inspection-failed",
    "artifact-size-limit",
    "external-modification-conflict",
    "overwrite-policy-conflict",
    "stage-creation-failed",
    "stage-identity-mismatch",
    "stage-write-failed",
    "stage-not-writable",
    "stage-writer-close-failed",
    "stage-reopen-failed",
    "stage-not-verifying",
    "stage-verification-read-offset-out-of-range",
    "stage-verification-read-failed",
    "stage-flush-failed",
    "stage-verification-not-accepted",
    "stage-verification-rejected",
    "invalid-publication-disposition",
    "parent-identity-mismatch",
    "atomic-create-unsupported",
    "atomic-publication-failed",
    "parent-durability-failed",
    "fault-injected",
};
static_assert(static_cast<std::size_t>(platform::StagedArtifactError::FaultInjected) + 1 ==
              kStagedArtifactErrorTokens.size());
// PublicationAdmissionStatus (src/host/include/bloom/host/publication_coordinator.hpp)
constexpr std::array<std::string_view, 4> kPublicationAdmissionStatusTokens{
    "accepted",
    "publication-tracking-limit",
    "runtime-identity-exhausted",
    "resource-unavailable",
};
static_assert(static_cast<std::size_t>(PublicationAdmissionStatus::ResourceUnavailable) + 1 ==
              kPublicationAdmissionStatusTokens.size());
// PublicationRegistrationStatus (src/host/include/bloom/host/publication_coordinator.hpp)
constexpr std::array<std::string_view, 5> kPublicationRegistrationStatusTokens{
    "registered",           "invalid-admission", "invalid-target", "publication-tracking-limit",
    "resource-unavailable",
};
static_assert(static_cast<std::size_t>(PublicationRegistrationStatus::ResourceUnavailable) + 1 ==
              kPublicationRegistrationStatusTokens.size());
// PublicationGuardStatus (src/host/include/bloom/host/publication_coordinator.hpp)
constexpr std::array<std::string_view, 5> kPublicationGuardStatusTokens{
    "entered", "invalid-claim", "superseded", "target-busy", "already-entered",
};
static_assert(static_cast<std::size_t>(PublicationGuardStatus::AlreadyEntered) + 1 ==
              kPublicationGuardStatusTokens.size());
// CanonicalManifestError (src/project/include/bloom/project/canonical_manifest.hpp)
constexpr std::array<std::string_view, 21> kCanonicalManifestErrorTokens{
    "none",
    "invalid-limits",
    "invalid-format",
    "invalid-container-version",
    "invalid-document-path",
    "invalid-document-schema-version",
    "invalid-generator",
    "requirement-count-exceeded",
    "invalid-provider-id",
    "invalid-capability-id",
    "invalid-requirement-schema-version",
    "duplicate-requirement-identity",
    "invalid-requirement-order",
    "provided-node-type-count-exceeded",
    "invalid-provided-node-type-id",
    "duplicate-provided-node-type-id",
    "invalid-provided-node-type-order",
    "value-count-exceeded",
    "size-overflow",
    "manifest-size-exceeded",
    "output-capacity-exceeded",
};
static_assert(static_cast<std::size_t>(project::CanonicalManifestError::OutputCapacityExceeded) +
                  1 ==
              kCanonicalManifestErrorTokens.size());
// CanonicalDocumentError (src/project/include/bloom/project/canonical_document.hpp)
constexpr std::array<std::string_view, 25> kCanonicalDocumentErrorTokens{
    "none",
    "missing-input",
    "invalid-limits",
    "invalid-name",
    "invalid-collection-identity",
    "missing-animation-curve",
    "invalid-composition-duration",
    "invalid-composition-format",
    "invalid-parameter",
    "invalid-animation-curve",
    "invalid-graph",
    "invalid-extension-record",
    "invalid-color-settings",
    "invalid-process-color-space-id",
    "invalid-working-color-space-id",
    "ocio-portability-mismatch",
    "invalid-ocio-revision-algorithm",
    "sort-buffer-too-small",
    "payload-buffer-too-small",
    "value-count-exceeded",
    "container-entry-count-exceeded",
    "document-size-exceeded",
    "output-capacity-exceeded",
    "round-trip-state-mismatch",
    "non-finite-value",
};
static_assert(static_cast<std::size_t>(project::CanonicalDocumentError::NonFiniteValue) + 1 ==
              kCanonicalDocumentErrorTokens.size());
// ZipContainerError (src/project/include/bloom/project/zip_container.hpp)
constexpr std::array<std::string_view, 27> kZipContainerErrorTokens{
    "none",
    "invalid-limits",
    "archive-too-large",
    "archive-truncated",
    "missing-or-malformed-eocd",
    "archive-comment-forbidden",
    "multi-disk-forbidden",
    "wrong-entry-count",
    "wrong-entry-name",
    "wrong-entry-order",
    "utf-8-flag-missing",
    "forbidden-general-purpose-flag",
    "unsupported-compression-method",
    "extra-field-forbidden",
    "entry-comment-forbidden",
    "zip64-forbidden",
    "local-central-header-disagreement",
    "overlapping-or-unaccounted-byte-range",
    "non-regular-entry",
    "executable-entry",
    "expanded-size-limit-exceeded",
    "stored-size-mismatch",
    "expanded-size-mismatch",
    "crc-mismatch",
    "size-overflow",
    "resource-exhausted",
    "qualified-reader-disagreement",
};
static_assert(static_cast<std::size_t>(project::ZipContainerError::QualifiedReaderDisagreement) +
                  1 ==
              kZipContainerErrorTokens.size());
// ZipContainerWriteError (src/project/include/bloom/project/zip_container_writer.hpp)
constexpr std::array<std::string_view, 8> kZipContainerWriteErrorTokens{
    "none",
    "invalid-limits",
    "entry-size-limit-exceeded",
    "total-expanded-limit-exceeded",
    "archive-size-limit-exceeded",
    "size-overflow",
    "resource-exhausted",
    "qualified-compressor-failure",
};
static_assert(
    static_cast<std::size_t>(project::ZipContainerWriteError::QualifiedCompressorFailure) + 1 ==
    kZipContainerWriteErrorTokens.size());
// StrictJsonDomError (src/project/include/bloom/project/strict_json_dom.hpp)
constexpr std::array<std::string_view, 19> kStrictJsonDomErrorTokens{
    "none",
    "invalid-limits",
    "input-too-large",
    "bom-forbidden",
    "empty-input",
    "invalid-utf-8",
    "invalid-syntax",
    "invalid-escape",
    "invalid-unicode-scalar",
    "invalid-number",
    "trailing-data",
    "depth-limit-exceeded",
    "value-limit-exceeded",
    "container-entry-limit-exceeded",
    "decoded-string-limit-exceeded",
    "size-overflow",
    "duplicate-object-key",
    "resource-exhausted",
    "parse-failed",
};
static_assert(static_cast<std::size_t>(project::StrictJsonDomError::ParseFailed) + 1 ==
              kStrictJsonDomErrorTokens.size());
// ManifestDecodeError (src/project/include/bloom/project/manifest_decode.hpp)
constexpr std::array<std::string_view, 17> kManifestDecodeErrorTokens{
    "none",
    "wrong-value-kind",
    "missing-member",
    "unknown-member",
    "member-out-of-order",
    "invalid-json-uint32",
    "unsupported-major-version",
    "invalid-format",
    "invalid-document-path",
    "invalid-provider-id",
    "invalid-capability-id",
    "invalid-requirement-order",
    "duplicate-requirement-identity",
    "invalid-provided-node-type-id",
    "invalid-provided-node-type-order",
    "duplicate-provided-node-type-id",
    "invalid-generator",
};
static_assert(static_cast<std::size_t>(project::ManifestDecodeError::InvalidGenerator) + 1 ==
              kManifestDecodeErrorTokens.size());
// DocumentDecodeError (src/project/include/bloom/project/document_decode.hpp)
constexpr std::array<std::string_view, 50> kDocumentDecodeErrorTokens{
    "none",
    "unsupported-node-version",
    "unsupported-schema-version",
    "wrong-value-kind",
    "missing-member",
    "unknown-member",
    "member-out-of-order",
    "invalid-id",
    "invalid-json-uint32",
    "invalid-rational-component",
    "unreduced-rational",
    "non-positive-duration",
    "invalid-digest-spelling",
    "invalid-ocio-locator-kind",
    "unsorted-compositions",
    "duplicate-composition",
    "domain-violation",
    "invalid-float64",
    "invalid-int64-value",
    "unsupported-parameter-source",
    "invalid-constant-value-kind",
    "unsorted-parameters",
    "duplicate-parameter",
    "unsorted-animation-curves",
    "duplicate-animation-curve",
    "invalid-animation-curve-kind",
    "empty-keyframes",
    "non-increasing-keyframe-time",
    "invalid-interpolation",
    "final-keyframe-not-linear",
    "invalid-keyframe-handle",
    "unsorted-nodes",
    "duplicate-node",
    "unsorted-bindings",
    "duplicate-binding",
    "unsorted-edges",
    "duplicate-edge",
    "invalid-edge-destination-kind",
    "unsorted-layer-outputs",
    "duplicate-layer-output",
    "dangling-reference",
    "invalid-allocator-high-water",
    "unsorted-extension-records",
    "duplicate-extension-record",
    "invalid-extension-target-kind",
    "invalid-reference-policy-kind",
    "invalid-base64-payload",
    "unsorted-unknown-member",
    "invalid-keyframe-easing",
    "invalid-keyframe-handle-mode",
};
static_assert(static_cast<std::size_t>(project::DocumentDecodeError::InvalidKeyframeHandleMode) +
                  1 ==
              kDocumentDecodeErrorTokens.size());
// ReconstructionStage (src/project/include/bloom/project/document_reconstruct.hpp)
constexpr std::array<std::string_view, 13> kReconstructionStageTokens{
    "none",
    "unsupported-node-version",
    "graph-node",
    "graph-edge",
    "layer-output",
    "layer-stack-entry",
    "parameter-store",
    "animation-curve-store",
    "composition-add",
    "extension-record-add",
    "data-block-add",
    "project-validate",
    "document-construct",
};
static_assert(static_cast<std::size_t>(project::ReconstructionStage::DocumentConstruct) + 1 ==
              kReconstructionStageTokens.size());
// SaveArchiveStage (src/project/include/bloom/project/save_archive.hpp)
constexpr std::array<std::string_view, 16> kSaveArchiveStageTokens{
    "none",
    "manifest-encode",
    "document-encode",
    "container-write",
    "container-read",
    "manifest-parse",
    "document-parse",
    "manifest-decode",
    "version-agreement",
    "document-decode",
    "reconstruction",
    "requirements-validation",
    "manifest-reencode",
    "document-reencode",
    "manifest-byte-comparison",
    "document-byte-comparison",
};
static_assert(static_cast<std::size_t>(project::SaveArchiveStage::DocumentByteComparison) + 1 ==
              kSaveArchiveStageTokens.size());
// StagedSaveStage (src/project/include/bloom/project/staged_save.hpp)
constexpr std::array<std::string_view, 9> kStagedSaveStageTokens{
    "none",
    "build",
    "stage-write",
    "stage-finish",
    "staged-size-disagreement",
    "read-back-allocation",
    "stage-read",
    "verification",
    "accept",
};
static_assert(static_cast<std::size_t>(project::StagedSaveStage::Accept) + 1 ==
              kStagedSaveStageTokens.size());
// SessionSaveInputStatus (src/host/include/bloom/host/project_session.hpp)
constexpr std::array<std::string_view, 6> kSessionSaveInputStatusTokens{
    "captured",      "read-only",    "invalid-session", "color-settings-unavailable",
    "path-required", "stale-intent",
};
static_assert(static_cast<std::size_t>(SessionSaveInputStatus::StaleIntent) + 1 ==
              kSessionSaveInputStatusTokens.size());
// ProjectSessionSavepointStatus (src/host/include/bloom/host/project_session.hpp)
constexpr std::array<std::string_view, 8> kProjectSessionSavepointStatusTokens{
    "accepted",     "read-only",        "invalid-session", "invalid-publication-intent",
    "stale-intent", "unknown-revision", "path-required",   "path-authority-mismatch",
};
static_assert(static_cast<std::size_t>(ProjectSessionSavepointStatus::PathAuthorityMismatch) + 1 ==
              kProjectSessionSavepointStatusTokens.size());
// SavePublicationStage (src/host/include/bloom/host/save_publication.hpp)
constexpr std::array<std::string_view, 8> kSavePublicationStageTokens{
    "none",    "admission",   "preflight", "registration",
    "staging", "staged-save", "guard",     "cancelled",
};
static_assert(static_cast<std::size_t>(SavePublicationStage::Cancelled) + 1 ==
              kSavePublicationStageTokens.size());
// StagedSaveLeaseCall (src/project/include/bloom/project/staged_save.hpp)
constexpr std::array<std::string_view, 5> kStagedSaveLeaseCallTokens{
    "none", "write", "finish-writing", "read-for-verification", "accept-verification",
};
static_assert(static_cast<std::size_t>(project::StagedSaveLeaseCall::AcceptVerification) + 1 ==
              kStagedSaveLeaseCallTokens.size());
// CopyPublicationStage (src/host/include/bloom/host/copy_publication.hpp), issue #748
constexpr std::array<std::string_view, 8> kCopyPublicationStageTokens{
    "none",    "admission",   "preflight", "registration",
    "staging", "staged-copy", "guard",     "cancelled",
};
static_assert(static_cast<std::size_t>(CopyPublicationStage::Cancelled) + 1 ==
              kCopyPublicationStageTokens.size());
// StagedCopyStage (src/project/include/bloom/project/staged_copy.hpp), issue #748
constexpr std::array<std::string_view, 8> kStagedCopyStageTokens{
    "none",       "stage-write", "stage-finish", "staged-size-disagreement",
    "read-back-allocation", "stage-read", "verification", "accept",
};
static_assert(static_cast<std::size_t>(project::StagedCopyStage::Accept) + 1 ==
              kStagedCopyStageTokens.size());

template <typename Enum, std::size_t N>
[[nodiscard]] std::string_view token(const std::array<std::string_view, N>& tokens,
                                     const Enum value) noexcept {
    const auto index = static_cast<std::size_t>(value);
    return index < N ? tokens[index] : std::string_view{"unrecognized"};
}

// The token read as words: "stage-write-failed" -> "stage write failed". Used for every cause
// that has no phrase of its own below; such causes are internal-protocol failures an artist can
// only report, so naming them exactly matters more than polish.
[[nodiscard]] std::string words(const std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c == '-')
            c = ' ';
    }
    return out;
}

[[nodiscard]] std::string number(const std::uint64_t value) { return std::to_string(value); }

struct Cause final {
    std::string token;
    std::string phrase;
};

[[nodiscard]] Cause plain(const std::string_view tokenText) {
    return {.token = std::string(tokenText), .phrase = words(tokenText)};
}

[[nodiscard]] Cause phrased(const std::string_view tokenText, std::string phrase) {
    return {.token = std::string(tokenText), .phrase = std::move(phrase)};
}

// A budget rejection and a failed system allocation read alike: both are "not enough memory for
// this save", and the stage names where it was needed.
constexpr std::string_view kOutOfMemory =
    "Bloom ran out of memory for the save (it needed more than one save's memory budget or than "
    "the computer had free)";
constexpr std::string_view kUnexpected = "an unexpected internal error occurred";

[[nodiscard]] Cause resourceExhausted() {
    return phrased("resource-exhausted", std::string(kOutOfMemory));
}
[[nodiscard]] Cause unexpectedFailure() {
    return phrased("unexpected-failure", std::string(kUnexpected));
}

[[nodiscard]] Cause platformCause(const platform::StagedArtifactError error) {
    using E = platform::StagedArtifactError;
    const auto name = stagedArtifactErrorToken(error);
    switch (error) {
    case E::ResourceUnavailable:
        return phrased(name, "the system ran out of memory or file handles");
    case E::InvalidTargetPath:
        return phrased(name, "the path is not a valid file path");
    case E::ParentResolutionFailed:
        return phrased(name, "its folder could not be found");
    case E::ParentNotDirectory:
        return phrased(name, "its folder is not a folder");
    case E::TargetLeafSymlink:
        return phrased(name, "the destination is a symbolic link");
    case E::TargetLeafNotRegular:
        return phrased(name, "the destination is not a regular file");
    case E::TargetInspectionFailed:
        return phrased(name, "the destination file could not be inspected");
    case E::ArtifactSizeLimit:
        return phrased(name, "the file is larger than the most Bloom writes in one file");
    case E::ExternalModificationConflict:
        return phrased(name, "the file changed outside Bloom");
    case E::OverwritePolicyConflict:
        return phrased(name, "the destination already exists");
    case E::StageCreationFailed:
        return phrased(name, "the temporary file could not be created in the destination folder "
                             "(check that the folder is writable)");
    case E::StageWriteFailed:
        return phrased(name, "the temporary file could not be written (check the free disk space)");
    case E::StageFlushFailed:
        return phrased(name, "the temporary file could not be flushed to the disk");
    case E::AtomicPublicationFailed:
        return phrased(name, "the temporary file could not replace the destination");
    case E::ParentDurabilityFailed:
        return phrased(name, "the folder could not be flushed to the disk");
    default:
        return plain(name);
    }
}

[[nodiscard]] Cause admissionCause(const PublicationAdmissionStatus status) {
    using S = PublicationAdmissionStatus;
    const auto name = token(kPublicationAdmissionStatusTokens, status);
    switch (status) {
    case S::PublicationTrackingLimit:
        return phrased(name, "too many saves are in progress");
    case S::RuntimeIdentityExhausted:
        return phrased(name, "Bloom ran out of save identifiers (restart Bloom and save again)");
    case S::ResourceUnavailable:
        return phrased(name, std::string(kOutOfMemory));
    default:
        return plain(name);
    }
}

[[nodiscard]] Cause registrationCause(const PublicationRegistrationStatus status) {
    using S = PublicationRegistrationStatus;
    const auto name = token(kPublicationRegistrationStatusTokens, status);
    switch (status) {
    case S::PublicationTrackingLimit:
        return phrased(name, "too many saves are in progress");
    case S::ResourceUnavailable:
        return phrased(name, std::string(kOutOfMemory));
    default:
        return plain(name);
    }
}

[[nodiscard]] Cause guardCause(const PublicationGuardStatus status) {
    const auto name = token(kPublicationGuardStatusTokens, status);
    if (status == PublicationGuardStatus::TargetBusy)
        return phrased(name, "another save of the same file is being written");
    return plain(name);
}

[[nodiscard]] std::string entryName(const project::SaveArchiveEntry entry) {
    switch (entry) {
    case project::SaveArchiveEntry::Manifest:
        return "manifest.json";
    case project::SaveArchiveEntry::Document:
        return "document.json";
    case project::SaveArchiveEntry::None:
        break;
    }
    return "the project file";
}

[[nodiscard]] std::string writeEntryName(const project::ZipContainerWriteEntry entry) {
    switch (entry) {
    case project::ZipContainerWriteEntry::Manifest:
        return "manifest.json";
    case project::ZipContainerWriteEntry::Document:
        return "document.json";
    case project::ZipContainerWriteEntry::None:
        break;
    }
    return "an entry";
}

[[nodiscard]] std::string at(const std::string_view path) {
    return path.empty() ? std::string{} : " at " + std::string(path);
}

[[nodiscard]] Cause
manifestEncodingCause(const project::SaveArchiveManifestEncodingFailure& failure,
                      const project::SaveArchiveLimits& limits) {
    using E = project::CanonicalManifestError;
    const auto name = token(kCanonicalManifestErrorTokens, failure.error);
    switch (failure.error) {
    case E::ValueCountExceeded:
        return phrased(name, "manifest.json holds more values than one project file can (the "
                             "limit is " +
                                 number(limits.manifest.maximumValues) + ")");
    case E::ManifestSizeExceeded:
        return phrased(name, "manifest.json would be larger than its limit of " +
                                 number(limits.manifest.maximumOutputBytes) + " bytes");
    case E::RequirementCountExceeded:
        return phrased(name, "the project needs more extensions than one project file can list");
    default:
        return plain(name);
    }
}

[[nodiscard]] Cause
documentEncodingCause(const project::SaveArchiveDocumentEncodingFailure& failure,
                      const project::SaveArchiveLimits& limits) {
    using E = project::CanonicalDocumentError;
    const auto name = token(kCanonicalDocumentErrorTokens, failure.error);
    switch (failure.error) {
    case E::ValueCountExceeded:
        return phrased(name, "the project holds more values than one project file can (the limit "
                             "is " +
                                 number(limits.document.maximumValues) + ")");
    case E::ContainerEntryCountExceeded:
        return phrased(name, "one list in the project has more entries than one project file "
                             "allows (the limit is " +
                                 number(limits.document.maximumContainerEntries) + ")");
    case E::DocumentSizeExceeded:
        return phrased(name, "document.json would be larger than its limit of " +
                                 number(limits.document.maximumOutputBytes) + " bytes");
    case E::NonFiniteValue:
        return phrased(name, std::string(failure.fieldPath.view()) + " is not a finite number");
    default:
        return plain(name);
    }
}

[[nodiscard]] Cause containerWriteCause(const project::SaveArchiveContainerWriteFailure& failure,
                                        const project::SaveArchiveLimits& limits) {
    using E = project::ZipContainerWriteError;
    const auto name = token(kZipContainerWriteErrorTokens, failure.error);
    switch (failure.error) {
    case E::EntrySizeLimitExceeded:
        return phrased(name, writeEntryName(failure.entry) + " is larger than its limit");
    case E::TotalExpandedLimitExceeded:
        return phrased(name, "the project's entries together are larger than their limit of " +
                                 number(limits.container.maxTotalExpandedBytes) + " bytes");
    case E::ArchiveSizeLimitExceeded:
        return phrased(name, "the project file would be larger than its limit of " +
                                 number(limits.container.maxArchiveBytes) + " bytes");
    case E::ResourceExhausted:
        return phrased(name, std::string(kOutOfMemory));
    default:
        return plain(name);
    }
}

[[nodiscard]] Cause jsonParseCause(const project::SaveArchiveJsonParseFailure& failure,
                                   const project::SaveArchiveLimits& limits) {
    using E = project::StrictJsonDomError;
    const auto name = token(kStrictJsonDomErrorTokens, failure.error);
    switch (failure.error) {
    case E::ValueLimitExceeded:
        return phrased(name, "the written file holds more values than Bloom reads back (the "
                             "limit is " +
                                 number(limits.json.maximumValues) + ")");
    case E::ContainerEntryLimitExceeded:
        return phrased(name, "one list in the written file has more entries than Bloom reads back "
                             "(the limit is " +
                                 number(limits.json.maximumContainerEntries) + ")");
    case E::ResourceExhausted:
        return phrased(name, std::string(kOutOfMemory));
    default:
        return phrased(name, words(name) + at(failure.path.view()));
    }
}

[[nodiscard]] Cause archiveCause(const project::SaveArchiveFailure& failure,
                                 const project::SaveArchiveLimits& limits) {
    return std::visit(
        [&](const auto& payload) -> Cause {
            using P = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<P, std::monostate>) {
                return phrased("none", "no cause was recorded");
            } else if constexpr (std::is_same_v<P, project::SaveArchiveManifestEncodingFailure>) {
                return manifestEncodingCause(payload, limits);
            } else if constexpr (std::is_same_v<P, project::SaveArchiveDocumentEncodingFailure>) {
                return documentEncodingCause(payload, limits);
            } else if constexpr (std::is_same_v<P, project::SaveArchiveContainerWriteFailure>) {
                return containerWriteCause(payload, limits);
            } else if constexpr (std::is_same_v<P, project::SaveArchiveContainerReadFailure>) {
                const auto name = token(kZipContainerErrorTokens, payload.error);
                if (payload.error == project::ZipContainerError::ResourceExhausted)
                    return phrased(name, std::string(kOutOfMemory));
                return phrased(name, words(name) + " at byte " + number(payload.byteOffset));
            } else if constexpr (std::is_same_v<P, project::SaveArchiveJsonParseFailure>) {
                return jsonParseCause(payload, limits);
            } else if constexpr (std::is_same_v<P, project::SaveArchiveManifestDecodeFailure>) {
                const auto name = token(kManifestDecodeErrorTokens, payload.error);
                return phrased(name, words(name) + at(payload.path.view()));
            } else if constexpr (std::is_same_v<P, project::SaveArchiveVersionAgreementFailure>) {
                return phrased("version-disagreement",
                               "manifest.json and document.json name different versions");
            } else if constexpr (std::is_same_v<P, project::SaveArchiveDocumentDecodeFailure>) {
                const auto name = token(kDocumentDecodeErrorTokens, payload.error);
                return phrased(name, words(name) + at(payload.path.view()));
            } else if constexpr (std::is_same_v<P, project::SaveArchiveRequirementsFailure>) {
                return phrased("requirements-invalid",
                               "the extensions the project needs did not validate");
            } else if constexpr (std::is_same_v<P, project::ReconstructionRejected>) {
                const auto name = token(kReconstructionStageTokens, payload.stage);
                return phrased(name, "the project could not be rebuilt from the file (" +
                                         words(name) + ", record " + number(payload.recordId) +
                                         ")");
            } else if constexpr (std::is_same_v<P, project::SaveArchiveVerificationMismatch>) {
                return phrased("verification-mismatch",
                               entryName(payload.entry) +
                                   " read back from the file differs from what was written");
            } else if constexpr (std::is_same_v<P, project::SaveArchiveResourceExhausted>) {
                return resourceExhausted();
            } else {
                static_assert(std::is_same_v<P, project::SaveArchiveUnexpectedFailure>);
                return unexpectedFailure();
            }
        },
        failure.payload());
}

struct Step final {
    std::string token;
    std::string phrase;
};

[[nodiscard]] std::string archiveStepPhrase(const project::SaveArchiveStage stage) {
    using S = project::SaveArchiveStage;
    switch (stage) {
    case S::ManifestEncode:
        return "encoding manifest.json";
    case S::DocumentEncode:
        return "encoding document.json";
    case S::ContainerWrite:
        return "compressing the project file";
    case S::ContainerRead:
        return "reading the project file back";
    case S::ManifestParse:
        return "parsing manifest.json";
    case S::DocumentParse:
        return "parsing document.json";
    case S::ManifestDecode:
        return "decoding manifest.json";
    case S::VersionAgreement:
        return "comparing the file's versions";
    case S::DocumentDecode:
        return "decoding document.json";
    case S::Reconstruction:
        return "rebuilding the project from the file";
    case S::RequirementsValidation:
        return "validating the extensions the project needs";
    case S::ManifestReencode:
        return "re-encoding manifest.json";
    case S::DocumentReencode:
        return "re-encoding document.json";
    case S::ManifestByteComparison:
        return "comparing manifest.json";
    case S::DocumentByteComparison:
        return "comparing document.json";
    case S::None:
        break;
    }
    return "building the project file";
}

[[nodiscard]] Step stagedSaveStep(const project::StagedSaveFailure& failure) {
    using S = project::StagedSaveStage;
    std::string stepToken(token(kStagedSaveStageTokens, failure.stage()));
    const auto* archive = failure.payloadAs<project::SaveArchiveFailure>();
    if (archive != nullptr && archive->stage() != project::SaveArchiveStage::None)
        stepToken += "." + std::string(token(kSaveArchiveStageTokens, archive->stage()));
    std::string phrase;
    switch (failure.stage()) {
    case S::Build:
        phrase = "while " + (archive != nullptr ? archiveStepPhrase(archive->stage())
                                                : std::string("building the project file"));
        break;
    case S::StageWrite:
        phrase = "while writing the temporary file";
        break;
    case S::StageFinish:
        phrase = "while finishing the temporary file";
        break;
    case S::StagedSizeDisagreement:
        phrase = "while checking the temporary file's size";
        break;
    case S::ReadBackAllocation:
        phrase = "while preparing to read the temporary file back";
        break;
    case S::StageRead:
        phrase = "while reading the temporary file back";
        break;
    case S::Verification:
        phrase = "while verifying the written file (" +
                 (archive != nullptr ? archiveStepPhrase(archive->stage())
                                     : std::string("checking it")) +
                 ")";
        break;
    case S::Accept:
        phrase = "while accepting the verified file";
        break;
    case S::None:
        phrase = "while writing the project file";
        break;
    }
    return {.token = std::move(stepToken), .phrase = std::move(phrase)};
}

[[nodiscard]] Cause stagedSaveCause(const project::StagedSaveFailure& failure,
                                    const project::SaveArchiveLimits& limits) {
    return std::visit(
        [&](const auto& payload) -> Cause {
            using P = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<P, std::monostate>) {
                return phrased("none", "no cause was recorded");
            } else if constexpr (std::is_same_v<P, project::SaveArchiveFailure>) {
                return archiveCause(payload, limits);
            } else if constexpr (std::is_same_v<P, project::StagedSavePlatformFailure>) {
                return platformCause(payload.error);
            } else if constexpr (std::is_same_v<P, project::StagedSaveSizeDisagreement>) {
                return phrased("size-disagreement",
                               "the temporary file holds " + number(payload.stageBytes) +
                                   " bytes, but the project file is " +
                                   number(payload.builtArchiveBytes) + " bytes");
            } else if constexpr (std::is_same_v<P, project::SaveArchiveResourceExhausted>) {
                return resourceExhausted();
            } else {
                static_assert(std::is_same_v<P, project::SaveArchiveUnexpectedFailure>);
                return unexpectedFailure();
            }
        },
        failure.payload());
}

[[nodiscard]] Cause publicationCause(const SavePublicationFailure& failure,
                                     const project::SaveArchiveLimits& limits) {
    return std::visit(
        [&](const auto& payload) -> Cause {
            using P = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<P, std::monostate>) {
                if (failure.stage() == SavePublicationStage::Cancelled)
                    return phrased("cancelled", "the save was cancelled");
                return phrased("none", "no cause was recorded");
            } else if constexpr (std::is_same_v<P, PublicationAdmissionStatus>) {
                return admissionCause(payload);
            } else if constexpr (std::is_same_v<P, platform::StagedArtifactError>) {
                return platformCause(payload);
            } else if constexpr (std::is_same_v<P, PublicationRegistrationStatus>) {
                return registrationCause(payload);
            } else if constexpr (std::is_same_v<P, project::StagedSaveFailure>) {
                return stagedSaveCause(payload, limits);
            } else if constexpr (std::is_same_v<P, PublicationGuardStatus>) {
                return guardCause(payload);
            } else if constexpr (std::is_same_v<P, SavePublicationResourceExhausted>) {
                return resourceExhausted();
            } else {
                static_assert(std::is_same_v<P, SavePublicationUnexpectedFailure>);
                return unexpectedFailure();
            }
        },
        failure.payload());
}

[[nodiscard]] Step publicationStep(const SavePublicationFailure& failure) {
    using S = SavePublicationStage;
    const std::string prefix =
        "publication." + std::string(token(kSavePublicationStageTokens, failure.stage()));
    switch (failure.stage()) {
    case S::Admission:
        return {.token = prefix, .phrase = "while starting the save"};
    case S::Preflight:
        return {.token = prefix, .phrase = "while checking the destination file"};
    case S::Registration:
        return {.token = prefix, .phrase = "while registering the destination file"};
    case S::Staging:
        return {.token = prefix, .phrase = "while creating the temporary file"};
    case S::StagedSave:
        if (const auto* staged = failure.payloadAs<project::StagedSaveFailure>()) {
            auto step = stagedSaveStep(*staged);
            step.token = prefix + "." + step.token;
            return step;
        }
        return {.token = prefix, .phrase = "while writing the project file"};
    case S::Guard:
        return {.token = prefix, .phrase = "while starting to replace the project file"};
    case S::Cancelled:
        return {.token = prefix, .phrase = "before the file was written"};
    case S::None:
        break;
    }
    return {.token = prefix, .phrase = "before the file could be written"};
}

[[nodiscard]] SaveFailureDescription describe(Step step, Cause cause) {
    std::string message = "Save failed " + step.phrase + ": " + cause.phrase + ".";
    return {.stage = std::move(step.token),
            .cause = std::move(cause.token),
            .message = std::move(message)};
}

[[nodiscard]] Cause captureCause(const SessionSaveCaptureOutcome& outcome) {
    return std::visit(
        [](const auto& payload) -> Cause {
            using P = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<P, std::monostate>) {
                return phrased("none", "no cause was recorded");
            } else if constexpr (std::is_same_v<P, SessionSaveInputStatus>) {
                using S = SessionSaveInputStatus;
                const auto name = token(kSessionSaveInputStatusTokens, payload);
                switch (payload) {
                case S::ReadOnly:
                    return phrased(name, "the project is read-only");
                case S::ColorSettingsUnavailable:
                    return phrased(name, "the project's color settings are not available");
                case S::PathRequired:
                    return phrased(name, "the project has never been saved (use Save As)");
                case S::StaleIntent:
                    return phrased(name, "the project's file changed while the save started "
                                         "(save again)");
                default:
                    return plain(name);
                }
            } else if constexpr (std::is_same_v<P, SessionSaveResourceExhausted>) {
                return resourceExhausted();
            } else {
                static_assert(std::is_same_v<P, SessionSaveUnexpectedFailure>);
                return unexpectedFailure();
            }
        },
        outcome);
}

// Issue #748: Save a Copy's steps and causes, in the save's own words.
[[nodiscard]] Step stagedCopyStep(const project::StagedCopyFailure& failure) {
    using S = project::StagedCopyStage;
    std::string phrase;
    switch (failure.stage()) {
    case S::StageWrite:
        phrase = "while writing the temporary file";
        break;
    case S::StageFinish:
        phrase = "while finishing the temporary file";
        break;
    case S::StagedSizeDisagreement:
        phrase = "while checking the temporary file's size";
        break;
    case S::ReadBackAllocation:
        phrase = "while preparing to read the temporary file back";
        break;
    case S::StageRead:
        phrase = "while reading the temporary file back";
        break;
    case S::Verification:
        phrase = "while verifying the written copy";
        break;
    case S::Accept:
        phrase = "while accepting the verified copy";
        break;
    case S::None:
        phrase = "while writing the copy";
        break;
    }
    return {.token = std::string(token(kStagedCopyStageTokens, failure.stage())),
            .phrase = std::move(phrase)};
}

[[nodiscard]] Cause stagedCopyCause(const project::StagedCopyFailure& failure) {
    return std::visit(
        [&](const auto& payload) -> Cause {
            using P = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<P, std::monostate>) {
                return phrased("none", "no cause was recorded");
            } else if constexpr (std::is_same_v<P, project::StagedSavePlatformFailure>) {
                return platformCause(payload.error);
            } else if constexpr (std::is_same_v<P, project::StagedCopySizeDisagreement>) {
                return phrased("size-disagreement",
                               "the temporary file holds " + number(payload.stageBytes) +
                                   " bytes, but the project file is " +
                                   number(payload.sourceBytes) + " bytes");
            } else if constexpr (std::is_same_v<P, project::StagedCopyByteMismatch>) {
                return phrased("byte-mismatch", "the written copy differs from the project file "
                                                "at byte " +
                                                    number(payload.byteOffset));
            } else if constexpr (std::is_same_v<P, project::StagedCopyContainerSanityFailure>) {
                const auto name = token(kZipContainerErrorTokens, payload.error);
                return phrased(name, "the written copy is not a readable project file (" +
                                         words(name) + " at byte " + number(payload.byteOffset) +
                                         ")");
            } else if constexpr (std::is_same_v<P, project::StagedCopyResourceExhausted>) {
                return resourceExhausted();
            } else {
                static_assert(std::is_same_v<P, project::StagedCopyUnexpectedFailure>);
                return unexpectedFailure();
            }
        },
        failure.payload());
}

[[nodiscard]] Cause copyPublicationCause(const CopyPublicationFailure& failure) {
    return std::visit(
        [&](const auto& payload) -> Cause {
            using P = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<P, std::monostate>) {
                if (failure.stage() == CopyPublicationStage::Cancelled)
                    return phrased("cancelled", "the copy was cancelled");
                return phrased("none", "no cause was recorded");
            } else if constexpr (std::is_same_v<P, PublicationAdmissionStatus>) {
                return admissionCause(payload);
            } else if constexpr (std::is_same_v<P, platform::StagedArtifactError>) {
                return platformCause(payload);
            } else if constexpr (std::is_same_v<P, PublicationRegistrationStatus>) {
                return registrationCause(payload);
            } else if constexpr (std::is_same_v<P, project::StagedCopyFailure>) {
                return stagedCopyCause(payload);
            } else if constexpr (std::is_same_v<P, PublicationGuardStatus>) {
                return guardCause(payload);
            } else if constexpr (std::is_same_v<P, CopyPublicationResourceExhausted>) {
                return resourceExhausted();
            } else {
                static_assert(std::is_same_v<P, CopyPublicationUnexpectedFailure>);
                return unexpectedFailure();
            }
        },
        failure.payload());
}

[[nodiscard]] Step copyPublicationStep(const CopyPublicationFailure& failure) {
    using S = CopyPublicationStage;
    const std::string prefix =
        "copy." + std::string(token(kCopyPublicationStageTokens, failure.stage()));
    switch (failure.stage()) {
    case S::Admission:
        return {.token = prefix, .phrase = "while starting the copy"};
    case S::Preflight:
        return {.token = prefix, .phrase = "while checking the destination file"};
    case S::Registration:
        return {.token = prefix, .phrase = "while registering the destination file"};
    case S::Staging:
        return {.token = prefix, .phrase = "while creating the temporary file"};
    case S::StagedCopy:
        if (const auto* staged = failure.payloadAs<project::StagedCopyFailure>()) {
            auto step = stagedCopyStep(*staged);
            step.token = prefix + "." + step.token;
            return step;
        }
        return {.token = prefix, .phrase = "while writing the copy"};
    case S::Guard:
        return {.token = prefix, .phrase = "while starting to replace the destination file"};
    case S::Cancelled:
        return {.token = prefix, .phrase = "before the copy was written"};
    case S::None:
        break;
    }
    return {.token = prefix, .phrase = "before the copy could be written"};
}

[[nodiscard]] SaveFailureDescription describeCopy(Step step, Cause cause) {
    std::string message = "Save a Copy failed " + step.phrase + ": " + cause.phrase + ".";
    return {.stage = std::move(step.token),
            .cause = std::move(cause.token),
            .message = std::move(message)};
}

} // namespace

std::optional<SaveFailureDescription>
describeCopyPublicationFailure(const CopyPublicationResult& result) {
    if (const auto* failure = result.failure()) {
        if (failure->stage() == CopyPublicationStage::Cancelled)
            return SaveFailureDescription{.stage = "copy.cancelled",
                                          .cause = "cancelled",
                                          .message =
                                              "Save a Copy was cancelled before the copy was "
                                              "written."};
        auto description = describeCopy(copyPublicationStep(*failure), copyPublicationCause(*failure));
        if (const auto reject = result.rejectDiagnostic();
            reject.has_value() && *reject != platform::StagedArtifactError::None)
            description.message += " Removing the temporary file also failed (" +
                                   std::string(stagedArtifactErrorToken(*reject)) + ").";
        return description;
    }
    const auto* publication = result.publication();
    if (publication != nullptr &&
        publication->outcome != platform::StagedArtifactPublicationOutcome::FailedBeforePublication)
        return std::nullopt;
    const auto error =
        publication != nullptr ? publication->error : platform::StagedArtifactError::None;
    return describeCopy({.token = "copy.publish",
                         .phrase = "while replacing the destination file with the written copy"},
                        error == platform::StagedArtifactError::None
                            ? phrased("none", "no cause was recorded")
                            : platformCause(error));
}

SaveFailureDescription describeFileCopyFailure(const std::error_code& error,
                                               const bool stagedFileWritten) {
    const auto cause = error == std::errc::no_space_on_device    ? "no-space"
                       : error == std::errc::permission_denied   ? "permission-denied"
                       : error == std::errc::read_only_file_system ? "read-only-file-system"
                       : error == std::errc::is_a_directory      ? "is-a-directory"
                                                                 : "filesystem-error";
    return describeCopy(stagedFileWritten
                            ? Step{.token = "copy.publish",
                                   .phrase = "while replacing the destination file with the "
                                             "written copy"}
                            : Step{.token = "copy.staged-copy.stage-write",
                                   .phrase = "while writing the temporary file"},
                        phrased(cause, error.message()));
}

std::string_view stagedArtifactErrorToken(const platform::StagedArtifactError error) {
    return token(kStagedArtifactErrorTokens, error);
}

SaveFailureDescription describeSavePublicationFailure(const SavePublicationFailure& failure,
                                                      const project::SaveArchiveLimits& limits) {
    auto step = publicationStep(failure);
    auto cause = publicationCause(failure, limits);
    if (failure.stage() == SavePublicationStage::Cancelled) {
        return {.stage = std::move(step.token),
                .cause = std::move(cause.token),
                .message = "The save was cancelled before the file was written."};
    }
    if (cause.token == "non-finite-value") {
        // The documented wording (docs/ux/ui-grammar.md, "Save And Recovery Messages").
        return {.stage = std::move(step.token),
                .cause = std::move(cause.token),
                .message = "Save failed: " + cause.phrase + "."};
    }
    return describe(std::move(step), std::move(cause));
}

std::optional<SaveFailureDescription>
describeSessionSaveFailure(const SessionSaveResult& result,
                           const project::SaveArchiveLimits& limits) {
    switch (result.stage()) {
    case SessionSaveStage::None:
        return SaveFailureDescription{
            .stage = "none", .cause = "not-run", .message = "The save did not run."};
    case SessionSaveStage::Capture: {
        const auto* outcome = result.captureOutcome();
        auto cause =
            outcome != nullptr ? captureCause(*outcome) : phrased("none", "no cause was recorded");
        return describe({.token = "capture", .phrase = "while reading the project"},
                        std::move(cause));
    }
    case SessionSaveStage::Publication: {
        const auto* failure = result.publicationFailure();
        auto description = failure != nullptr ? describeSavePublicationFailure(*failure, limits)
                                              : describe({.token = "publication",
                                                          .phrase = "before the file "
                                                                    "could be written"},
                                                         phrased("none", "no cause was recorded"));
        if (const auto reject = result.rejectDiagnostic();
            reject.has_value() && *reject != platform::StagedArtifactError::None) {
            description.message += " Removing the temporary file also failed (" +
                                   std::string(stagedArtifactErrorToken(*reject)) + ").";
        }
        return description;
    }
    case SessionSaveStage::Savepoint: {
        const auto* publication = result.publication();
        if (publication == nullptr ||
            publication->outcome ==
                platform::StagedArtifactPublicationOutcome::FailedBeforePublication) {
            const auto error =
                publication != nullptr ? publication->error : platform::StagedArtifactError::None;
            return describe({.token = "publication.publish",
                             .phrase = "while replacing the project file with the written one"},
                            error == platform::StagedArtifactError::None
                                ? phrased("none", "no cause was recorded")
                                : platformCause(error));
        }
        const auto status = result.savepointStatus();
        if (publication->targetWasPublished() && status.has_value() &&
            *status != ProjectSessionSavepointStatus::Accepted) {
            return SaveFailureDescription{
                .stage = "savepoint",
                .cause = std::string(token(kProjectSessionSavepointStatusTokens, *status)),
                .message = "The project file was written, but this open project is now out of "
                           "date with it; reopen the file to continue editing it."};
        }
        return std::nullopt;
    }
    }
    return std::nullopt;
}

} // namespace bloom::host
