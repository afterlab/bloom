#include <bloom/host/save_failure_description.hpp>

#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/save_publication.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/manifest_decode.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/staged_save.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>
#include <bloom/project/zip_container_writer.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>

// Issue #737: every typed reason a Save or Save As can stop names its stage and its cause, in the
// artist's message and as stable tokens, and never collapses into one fixed sentence. One check per
// stage and per payload kind, plus a sweep over every enumerator of every payload enum so a new
// enumerator can never be reported without a token.

namespace {

using bloom::host::describeSavePublicationFailure;
using bloom::host::describeSessionSaveFailure;
using bloom::host::PublicationAdmissionStatus;
using bloom::host::PublicationGuardStatus;
using bloom::host::PublicationRegistrationStatus;
using bloom::host::SaveFailureDescription;
using bloom::host::SavePublicationFailure;
using bloom::host::SavePublicationResourceExhausted;
using bloom::host::SavePublicationStage;
using bloom::host::SavePublicationUnexpectedFailure;
using bloom::host::SessionSaveResult;
using bloom::platform::StagedArtifactError;
using bloom::project::SaveArchiveFailure;
using bloom::project::SaveArchiveStage;
using bloom::project::StagedSaveFailure;
using bloom::project::StagedSaveStage;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool contains(const std::string_view text, const std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

[[nodiscard]] bool isToken(const std::string_view text) {
    if (text.empty() || text.front() == '-' || text.back() == '-')
        return false;
    for (const char c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
            return false;
    }
    return true;
}

void expectDescription(Expectations& e, const SaveFailureDescription& description,
                       const std::string_view stage, const std::string_view cause,
                       const std::string_view messagePart,
                       const std::source_location location = std::source_location::current()) {
    const auto detail = " [stage=" + description.stage + " cause=" + description.cause +
                        " message=" + description.message + "]";
    e.expect(description.stage == stage, "stage is " + std::string(stage) + detail, location);
    e.expect(description.cause == cause, "cause is " + std::string(cause) + detail, location);
    e.expect(contains(description.message, messagePart),
             "message names \"" + std::string(messagePart) + "\"" + detail, location);
    e.expect(description.message.ends_with('.'), "message is one sentence" + detail, location);
    e.expect(!contains(description.message, "unrecognized"), "no unnamed cause" + detail, location);
}

[[nodiscard]] SaveFailureDescription staged(const StagedSaveFailure& failure) {
    return describeSavePublicationFailure(
        SavePublicationFailure(SavePublicationStage::StagedSave, failure));
}

[[nodiscard]] SaveFailureDescription archive(const StagedSaveStage stage,
                                             const SaveArchiveFailure& failure) {
    return staged(StagedSaveFailure(stage, failure));
}

// ---- Publication stages, each with the payload kinds it carries -------------------------------

void testAdmission(Expectations& e) {
    expectDescription(
        e,
        describeSavePublicationFailure(SavePublicationFailure(
            SavePublicationStage::Admission, PublicationAdmissionStatus::PublicationTrackingLimit)),
        "publication.admission", "publication-tracking-limit",
        "Save failed while starting the save: too many saves are in progress.");
    expectDescription(
        e,
        describeSavePublicationFailure(SavePublicationFailure(
            SavePublicationStage::Admission, PublicationAdmissionStatus::RuntimeIdentityExhausted)),
        "publication.admission", "runtime-identity-exhausted", "restart Bloom");
    expectDescription(
        e,
        describeSavePublicationFailure(SavePublicationFailure(
            SavePublicationStage::Admission, PublicationAdmissionStatus::ResourceUnavailable)),
        "publication.admission", "resource-unavailable", "ran out of memory");
}

void testPreflight(Expectations& e) {
    expectDescription(
        e,
        describeSavePublicationFailure(SavePublicationFailure(
            SavePublicationStage::Preflight, StagedArtifactError::ParentNotDirectory)),
        "publication.preflight", "parent-not-directory",
        "Save failed while checking the destination file: its folder is not a "
        "folder.");
    expectDescription(e,
                      describeSavePublicationFailure(SavePublicationFailure(
                          SavePublicationStage::Preflight, StagedArtifactError::TargetLeafSymlink)),
                      "publication.preflight", "target-leaf-symlink", "symbolic link");
}

void testRegistration(Expectations& e) {
    expectDescription(
        e,
        describeSavePublicationFailure(SavePublicationFailure(
            SavePublicationStage::Registration, PublicationRegistrationStatus::InvalidTarget)),
        "publication.registration", "invalid-target",
        "Save failed while registering the destination file: invalid target.");
    expectDescription(e,
                      describeSavePublicationFailure(SavePublicationFailure(
                          SavePublicationStage::Registration,
                          PublicationRegistrationStatus::PublicationTrackingLimit)),
                      "publication.registration", "publication-tracking-limit",
                      "too many saves are in progress");
}

void testStaging(Expectations& e) {
    expectDescription(e,
                      describeSavePublicationFailure(SavePublicationFailure(
                          SavePublicationStage::Staging, StagedArtifactError::StageCreationFailed)),
                      "publication.staging", "stage-creation-failed",
                      "Save failed while creating the temporary file: the temporary file could "
                      "not be created in the destination folder");
}

void testGuard(Expectations& e) {
    expectDescription(e,
                      describeSavePublicationFailure(SavePublicationFailure(
                          SavePublicationStage::Guard, PublicationGuardStatus::TargetBusy)),
                      "publication.guard", "target-busy",
                      "another save of the same file is being written");
    expectDescription(e,
                      describeSavePublicationFailure(SavePublicationFailure(
                          SavePublicationStage::Guard, PublicationGuardStatus::InvalidClaim)),
                      "publication.guard", "invalid-claim", "invalid claim");
}

void testCancelled(Expectations& e) {
    const auto description = describeSavePublicationFailure(
        SavePublicationFailure(SavePublicationStage::Cancelled, std::monostate{}));
    expectDescription(e, description, "publication.cancelled", "cancelled",
                      "The save was cancelled before the file was written.");
}

void testResourceExhaustedAndUnexpectedAtEveryStage(Expectations& e) {
    for (const auto& [stage, token] :
         {std::pair{SavePublicationStage::Admission, "admission"},
          std::pair{SavePublicationStage::Preflight, "preflight"},
          std::pair{SavePublicationStage::Registration, "registration"},
          std::pair{SavePublicationStage::Staging, "staging"},
          std::pair{SavePublicationStage::StagedSave, "staged-save"},
          std::pair{SavePublicationStage::Guard, "guard"}}) {
        const std::string stageToken = std::string("publication.") + token;
        expectDescription(e,
                          describeSavePublicationFailure(
                              SavePublicationFailure(stage, SavePublicationResourceExhausted{})),
                          stageToken, "resource-exhausted", "ran out of memory for the save");
        expectDescription(e,
                          describeSavePublicationFailure(
                              SavePublicationFailure(stage, SavePublicationUnexpectedFailure{})),
                          stageToken, "unexpected-failure", "an unexpected internal error");
    }
}

// ---- The staged save's own steps --------------------------------------------------------------

void testStagedSaveBuild(Expectations& e) {
    const bloom::project::SaveArchiveLimits limits{};
    expectDescription(
        e,
        archive(StagedSaveStage::Build,
                SaveArchiveFailure(
                    SaveArchiveStage::DocumentEncode,
                    bloom::project::SaveArchiveDocumentEncodingFailure{
                        .error = bloom::project::CanonicalDocumentError::ValueCountExceeded})),
        "publication.staged-save.build.document-encode", "value-count-exceeded",
        "Save failed while encoding document.json: the project holds more values than one "
        "project file can (the limit is " +
            std::to_string(limits.document.maximumValues) + ").");
    expectDescription(
        e,
        archive(StagedSaveStage::Build,
                SaveArchiveFailure(
                    SaveArchiveStage::DocumentEncode,
                    bloom::project::SaveArchiveDocumentEncodingFailure{
                        .error = bloom::project::CanonicalDocumentError::DocumentSizeExceeded})),
        "publication.staged-save.build.document-encode", "document-size-exceeded",
        "document.json would be larger than its limit of " +
            std::to_string(limits.document.maximumOutputBytes) + " bytes");
    expectDescription(
        e,
        archive(
            StagedSaveStage::Build,
            SaveArchiveFailure(
                SaveArchiveStage::DocumentEncode,
                bloom::project::SaveArchiveDocumentEncodingFailure{
                    .error = bloom::project::CanonicalDocumentError::ContainerEntryCountExceeded})),
        "publication.staged-save.build.document-encode", "container-entry-count-exceeded",
        "one list in the project has more entries");
    // The documented non-finite wording is kept exactly (docs/ux/ui-grammar.md).
    const auto nonFinite =
        archive(StagedSaveStage::Build,
                SaveArchiveFailure(
                    SaveArchiveStage::DocumentEncode,
                    bloom::project::SaveArchiveDocumentEncodingFailure{
                        .error = bloom::project::CanonicalDocumentError::NonFiniteValue,
                        .fieldPath = bloom::project::SaveArchiveErrorPath::from(
                            "project/Composition[7]/AnimationCurve[12]/Keyframe[4]/value")}));
    e.expect(nonFinite.message == "Save failed: project/Composition[7]/AnimationCurve[12]/"
                                  "Keyframe[4]/value is not a finite number.",
             "the non-finite message keeps its documented wording: " + nonFinite.message);
    e.expect(nonFinite.cause == "non-finite-value" &&
                 nonFinite.stage == "publication.staged-save.build.document-encode",
             "the non-finite failure still names its stage and cause");
    expectDescription(
        e,
        archive(StagedSaveStage::Build,
                SaveArchiveFailure(
                    SaveArchiveStage::ManifestEncode,
                    bloom::project::SaveArchiveManifestEncodingFailure{
                        .error = bloom::project::CanonicalManifestError::ManifestSizeExceeded})),
        "publication.staged-save.build.manifest-encode", "manifest-size-exceeded",
        "Save failed while encoding manifest.json: manifest.json would be larger");
    expectDescription(
        e,
        archive(StagedSaveStage::Build,
                SaveArchiveFailure(
                    SaveArchiveStage::ContainerWrite,
                    bloom::project::SaveArchiveContainerWriteFailure{
                        .error = bloom::project::ZipContainerWriteError::ArchiveSizeLimitExceeded,
                        .entry = bloom::project::ZipContainerWriteEntry::Document})),
        "publication.staged-save.build.container-write", "archive-size-limit-exceeded",
        "Save failed while compressing the project file: the project file would be larger than "
        "its limit of " +
            std::to_string(limits.container.maxArchiveBytes) + " bytes.");
    expectDescription(e,
                      archive(StagedSaveStage::Build,
                              SaveArchiveFailure(SaveArchiveStage::ContainerWrite,
                                                 bloom::project::SaveArchiveResourceExhausted{})),
                      "publication.staged-save.build.container-write", "resource-exhausted",
                      "ran out of memory");
}

void testStagedSavePlatformSteps(Expectations& e) {
    using bloom::project::StagedSaveLeaseCall;
    using bloom::project::StagedSavePlatformFailure;
    expectDescription(e,
                      staged(StagedSaveFailure(
                          StagedSaveStage::StageWrite,
                          StagedSavePlatformFailure{.error = StagedArtifactError::StageWriteFailed,
                                                    .call = StagedSaveLeaseCall::Write})),
                      "publication.staged-save.stage-write", "stage-write-failed",
                      "Save failed while writing the temporary file: the temporary file could "
                      "not be written (check the free disk space).");
    expectDescription(e,
                      staged(StagedSaveFailure(
                          StagedSaveStage::StageFinish,
                          StagedSavePlatformFailure{.error = StagedArtifactError::StageFlushFailed,
                                                    .call = StagedSaveLeaseCall::FinishWriting})),
                      "publication.staged-save.stage-finish", "stage-flush-failed",
                      "while finishing the temporary file");
    expectDescription(e,
                      staged(StagedSaveFailure(StagedSaveStage::StagedSizeDisagreement,
                                               bloom::project::StagedSaveSizeDisagreement{
                                                   .builtArchiveBytes = 100, .stageBytes = 90})),
                      "publication.staged-save.staged-size-disagreement", "size-disagreement",
                      "the temporary file holds 90 bytes, but the project file is 100 bytes");
    expectDescription(e,
                      staged(StagedSaveFailure(StagedSaveStage::ReadBackAllocation,
                                               bloom::project::SaveArchiveResourceExhausted{})),
                      "publication.staged-save.read-back-allocation", "resource-exhausted",
                      "while preparing to read the temporary file back");
    expectDescription(
        e,
        staged(StagedSaveFailure(
            StagedSaveStage::StageRead,
            StagedSavePlatformFailure{.error = StagedArtifactError::StageVerificationReadFailed,
                                      .call = StagedSaveLeaseCall::ReadForVerification})),
        "publication.staged-save.stage-read", "stage-verification-read-failed",
        "while reading the temporary file back: stage verification read failed");
    expectDescription(
        e,
        staged(StagedSaveFailure(
            StagedSaveStage::Accept,
            StagedSavePlatformFailure{.error = StagedArtifactError::StageNotVerifying,
                                      .call = StagedSaveLeaseCall::AcceptVerification})),
        "publication.staged-save.accept", "stage-not-verifying",
        "while accepting the verified file");
    expectDescription(e,
                      staged(StagedSaveFailure(StagedSaveStage::StageWrite,
                                               bloom::project::SaveArchiveUnexpectedFailure{})),
                      "publication.staged-save.stage-write", "unexpected-failure",
                      "an unexpected internal error");
}

void testStagedSaveVerification(Expectations& e) {
    namespace p = bloom::project;
    const auto verify = [](const SaveArchiveStage stage, auto payload) {
        return archive(StagedSaveStage::Verification, SaveArchiveFailure(stage, payload));
    };
    expectDescription(e,
                      verify(SaveArchiveStage::ContainerRead,
                             p::SaveArchiveContainerReadFailure{
                                 .error = p::ZipContainerError::CrcMismatch, .byteOffset = 42}),
                      "publication.staged-save.verification.container-read", "crc-mismatch",
                      "Save failed while verifying the written file (reading the project file "
                      "back): crc mismatch at byte 42.");
    expectDescription(
        e,
        verify(SaveArchiveStage::DocumentParse,
               p::SaveArchiveJsonParseFailure{.error = p::StrictJsonDomError::ValueLimitExceeded,
                                              .byteOffset = 0,
                                              .path = {}}),
        "publication.staged-save.verification.document-parse", "value-limit-exceeded",
        "(parsing document.json): the written file holds more values than Bloom reads back");
    expectDescription(
        e,
        verify(SaveArchiveStage::DocumentParse,
               p::SaveArchiveJsonParseFailure{
                   .error = p::StrictJsonDomError::ResourceExhausted, .byteOffset = 0, .path = {}}),
        "publication.staged-save.verification.document-parse", "resource-exhausted",
        "ran out of memory");
    expectDescription(e,
                      verify(SaveArchiveStage::ManifestDecode,
                             p::SaveArchiveManifestDecodeFailure{
                                 .outcome = p::ManifestDecodeOutcome::Failed,
                                 .error = p::ManifestDecodeError::MissingMember,
                                 .path = p::SaveArchiveErrorPath::from("manifest/format")}),
                      "publication.staged-save.verification.manifest-decode", "missing-member",
                      "missing member at manifest/format");
    expectDescription(
        e, verify(SaveArchiveStage::VersionAgreement, p::SaveArchiveVersionAgreementFailure{}),
        "publication.staged-save.verification.version-agreement", "version-disagreement",
        "name different versions");
    expectDescription(e,
                      verify(SaveArchiveStage::DocumentDecode,
                             p::SaveArchiveDocumentDecodeFailure{
                                 .outcome = p::DocumentDecodeOutcome::Failed,
                                 .error = p::DocumentDecodeError::NonIncreasingKeyframeTime,
                                 .preservationReason = p::RoundTripPreservationReason::None,
                                 .path = p::SaveArchiveErrorPath::from("project/Composition[2]"),
                                 .nodeTypeId = {},
                                 .nodeVersion = 0}),
                      "publication.staged-save.verification.document-decode",
                      "non-increasing-keyframe-time",
                      "non increasing keyframe time at project/Composition[2]");
    expectDescription(
        e, verify(SaveArchiveStage::RequirementsValidation, p::SaveArchiveRequirementsFailure{}),
        "publication.staged-save.verification.requirements-validation", "requirements-invalid",
        "did not validate");
    expectDescription(e,
                      verify(SaveArchiveStage::Reconstruction,
                             p::ReconstructionRejected{.stage = p::ReconstructionStage::GraphEdge,
                                                       .compositionId = {},
                                                       .recordId = 9,
                                                       .nodeTypeId = {},
                                                       .nodeVersion = 0}),
                      "publication.staged-save.verification.reconstruction", "graph-edge",
                      "could not be rebuilt from the file (graph edge, record 9)");
    expectDescription(
        e,
        verify(SaveArchiveStage::DocumentByteComparison,
               p::SaveArchiveVerificationMismatch{.entry = p::SaveArchiveEntry::Document}),
        "publication.staged-save.verification.document-byte-comparison", "verification-mismatch",
        "document.json read back from the file differs from what was written");
    expectDescription(e,
                      verify(SaveArchiveStage::DocumentReencode, p::SaveArchiveResourceExhausted{}),
                      "publication.staged-save.verification.document-reencode",
                      "resource-exhausted", "(re-encoding document.json)");
    expectDescription(e, verify(SaveArchiveStage::ManifestParse, p::SaveArchiveUnexpectedFailure{}),
                      "publication.staged-save.verification.manifest-parse", "unexpected-failure",
                      "(parsing manifest.json)");
}

// ---- Every enumerator of every payload enum has its own token ---------------------------------

template <typename Enum, typename Describe>
void sweep(Expectations& e, const std::string_view name, const int first, const int last,
           Describe describe) {
    std::set<std::string> causes;
    for (int value = first; value <= last; ++value) {
        const SaveFailureDescription description = describe(static_cast<Enum>(value));
        e.expect(isToken(description.cause) && isToken(description.stage),
                 std::string(name) + " " + std::to_string(value) +
                     " has tokens: " + description.stage + " / " + description.cause);
        e.expect(!contains(description.cause, "unrecognized") && !description.message.empty(),
                 std::string(name) + " " + std::to_string(value) + " is named");
        causes.insert(description.cause);
    }
    e.expect(causes.size() == static_cast<std::size_t>(last - first + 1),
             std::string(name) + ": every enumerator has a distinct token");
}

void testEveryEnumeratorIsNamed(Expectations& e) {
    namespace p = bloom::project;
    sweep<StagedArtifactError>(e, "StagedArtifactError", 1,
                               static_cast<int>(StagedArtifactError::FaultInjected),
                               [](const StagedArtifactError error) {
                                   return describeSavePublicationFailure(SavePublicationFailure(
                                       SavePublicationStage::Preflight, error));
                               });
    sweep<PublicationAdmissionStatus>(
        e, "PublicationAdmissionStatus", 1,
        static_cast<int>(PublicationAdmissionStatus::ResourceUnavailable),
        [](const PublicationAdmissionStatus status) {
            return describeSavePublicationFailure(
                SavePublicationFailure(SavePublicationStage::Admission, status));
        });
    sweep<PublicationRegistrationStatus>(
        e, "PublicationRegistrationStatus", 1,
        static_cast<int>(PublicationRegistrationStatus::ResourceUnavailable),
        [](const PublicationRegistrationStatus status) {
            return describeSavePublicationFailure(
                SavePublicationFailure(SavePublicationStage::Registration, status));
        });
    sweep<PublicationGuardStatus>(e, "PublicationGuardStatus", 1,
                                  static_cast<int>(PublicationGuardStatus::AlreadyEntered),
                                  [](const PublicationGuardStatus status) {
                                      return describeSavePublicationFailure(SavePublicationFailure(
                                          SavePublicationStage::Guard, status));
                                  });
    sweep<p::CanonicalDocumentError>(
        e, "CanonicalDocumentError", 1, static_cast<int>(p::CanonicalDocumentError::NonFiniteValue),
        [](const p::CanonicalDocumentError error) {
            return archive(
                StagedSaveStage::Build,
                SaveArchiveFailure(SaveArchiveStage::DocumentEncode,
                                   p::SaveArchiveDocumentEncodingFailure{.error = error}));
        });
    sweep<p::CanonicalManifestError>(
        e, "CanonicalManifestError", 1,
        static_cast<int>(p::CanonicalManifestError::OutputCapacityExceeded),
        [](const p::CanonicalManifestError error) {
            return archive(
                StagedSaveStage::Build,
                SaveArchiveFailure(SaveArchiveStage::ManifestEncode,
                                   p::SaveArchiveManifestEncodingFailure{.error = error}));
        });
    sweep<p::ZipContainerWriteError>(
        e, "ZipContainerWriteError", 1,
        static_cast<int>(p::ZipContainerWriteError::QualifiedCompressorFailure),
        [](const p::ZipContainerWriteError error) {
            return archive(StagedSaveStage::Build,
                           SaveArchiveFailure(
                               SaveArchiveStage::ContainerWrite,
                               p::SaveArchiveContainerWriteFailure{
                                   .error = error, .entry = p::ZipContainerWriteEntry::Document}));
        });
    sweep<p::ZipContainerError>(
        e, "ZipContainerError", 1,
        static_cast<int>(p::ZipContainerError::QualifiedReaderDisagreement),
        [](const p::ZipContainerError error) {
            return archive(StagedSaveStage::Verification,
                           SaveArchiveFailure(SaveArchiveStage::ContainerRead,
                                              p::SaveArchiveContainerReadFailure{.error = error,
                                                                                 .byteOffset = 0}));
        });
    sweep<p::StrictJsonDomError>(
        e, "StrictJsonDomError", 1, static_cast<int>(p::StrictJsonDomError::ParseFailed),
        [](const p::StrictJsonDomError error) {
            return archive(StagedSaveStage::Verification,
                           SaveArchiveFailure(SaveArchiveStage::DocumentParse,
                                              p::SaveArchiveJsonParseFailure{
                                                  .error = error, .byteOffset = 0, .path = {}}));
        });
    sweep<p::ManifestDecodeError>(
        e, "ManifestDecodeError", 1, static_cast<int>(p::ManifestDecodeError::InvalidGenerator),
        [](const p::ManifestDecodeError error) {
            return archive(
                StagedSaveStage::Verification,
                SaveArchiveFailure(
                    SaveArchiveStage::ManifestDecode,
                    p::SaveArchiveManifestDecodeFailure{
                        .outcome = p::ManifestDecodeOutcome::Failed, .error = error, .path = {}}));
        });
    sweep<p::DocumentDecodeError>(
        e, "DocumentDecodeError", 1,
        static_cast<int>(p::DocumentDecodeError::InvalidKeyframeHandleMode),
        [](const p::DocumentDecodeError error) {
            return archive(
                StagedSaveStage::Verification,
                SaveArchiveFailure(SaveArchiveStage::DocumentDecode,
                                   p::SaveArchiveDocumentDecodeFailure{
                                       .outcome = p::DocumentDecodeOutcome::Failed,
                                       .error = error,
                                       .preservationReason = p::RoundTripPreservationReason::None,
                                       .path = {},
                                       .nodeTypeId = {},
                                       .nodeVersion = 0}));
        });
    sweep<p::ReconstructionStage>(
        e, "ReconstructionStage", 1, static_cast<int>(p::ReconstructionStage::DocumentConstruct),
        [](const p::ReconstructionStage stage) {
            return archive(StagedSaveStage::Verification,
                           SaveArchiveFailure(SaveArchiveStage::Reconstruction,
                                              p::ReconstructionRejected{.stage = stage,
                                                                        .compositionId = {},
                                                                        .recordId = 1,
                                                                        .nodeTypeId = {},
                                                                        .nodeVersion = 0}));
        });
    // Every staged-save step and archive step names its own stage token.
    std::set<std::string> stages;
    for (int step = 1; step <= static_cast<int>(StagedSaveStage::Accept); ++step) {
        stages.insert(staged(StagedSaveFailure(static_cast<StagedSaveStage>(step),
                                               p::SaveArchiveResourceExhausted{}))
                          .stage);
    }
    e.expect(stages.size() == static_cast<std::size_t>(StagedSaveStage::Accept),
             "every staged-save step has its own stage token");
    stages.clear();
    for (int step = 1; step <= static_cast<int>(SaveArchiveStage::DocumentByteComparison); ++step) {
        stages.insert(archive(StagedSaveStage::Verification,
                              SaveArchiveFailure(static_cast<SaveArchiveStage>(step),
                                                 p::SaveArchiveResourceExhausted{}))
                          .stage);
    }
    e.expect(stages.size() == static_cast<std::size_t>(SaveArchiveStage::DocumentByteComparison),
             "every archive step has its own stage token");
}

// ---- Session results: capture, publication, and a publication that did not replace the file ----

void testSessionResults(Expectations& e) {
    using bloom::host::SessionSaveInputStatus;
    for (int status = 1; status <= static_cast<int>(SessionSaveInputStatus::StaleIntent);
         ++status) {
        const auto description = describeSessionSaveFailure(
            SessionSaveResult::captureFailure(static_cast<SessionSaveInputStatus>(status)));
        e.expect(description.has_value() && description->stage == "capture" &&
                     isToken(description->cause) &&
                     contains(description->message, "Save failed while reading the project: "),
                 "a capture failure names its cause");
    }
    const auto pathRequired = describeSessionSaveFailure(
        SessionSaveResult::captureFailure(SessionSaveInputStatus::PathRequired));
    e.expect(pathRequired.has_value() && pathRequired->cause == "path-required" &&
                 contains(pathRequired->message, "use Save As"),
             "a pathless save says to use Save As");
    const auto exhausted = describeSessionSaveFailure(
        SessionSaveResult::captureFailure(bloom::host::SessionSaveResourceExhausted{}));
    e.expect(exhausted.has_value() && exhausted->cause == "resource-exhausted",
             "a capture that ran out of memory says so");

    // A publication failure carries a failed temporary-file cleanup as a secondary note.
    const auto rejected = describeSessionSaveFailure(SessionSaveResult::publicationFailure(
        SavePublicationFailure(
            SavePublicationStage::StagedSave,
            StagedSaveFailure(
                StagedSaveStage::Verification,
                SaveArchiveFailure(SaveArchiveStage::DocumentByteComparison,
                                   bloom::project::SaveArchiveVerificationMismatch{
                                       .entry = bloom::project::SaveArchiveEntry::Document}))),
        StagedArtifactError::StageVerificationRejected));
    e.expect(rejected.has_value() &&
                 rejected->stage ==
                     "publication.staged-save.verification.document-byte-comparison" &&
                 rejected->cause == "verification-mismatch" &&
                 contains(rejected->message,
                          "Removing the temporary file also failed (stage-verification-rejected)."),
             "the cleanup diagnostic is reported after the cause, never instead of it");

    // publish() ran and failed before it replaced the file.
    const auto failedPublish = describeSessionSaveFailure(SessionSaveResult::unpublished(
        bloom::platform::StagedArtifactPublicationResult{
            .outcome = bloom::platform::StagedArtifactPublicationOutcome::FailedBeforePublication,
            .error = StagedArtifactError::AtomicPublicationFailed},
        bloom::host::PublicationIntentId{}));
    e.expect(failedPublish.has_value() && failedPublish->stage == "publication.publish" &&
                 failedPublish->cause == "atomic-publication-failed" &&
                 contains(failedPublish->message,
                          "while replacing the project file with the written one: the temporary "
                          "file could not replace the destination"),
             "a publish that failed before replacing the file names why: " +
                 (failedPublish ? failedPublish->message : std::string{}));

    // Outcomes that are not failures of this save have no description.
    const auto superseded = describeSessionSaveFailure(SessionSaveResult::unpublished(
        bloom::platform::StagedArtifactPublicationResult{
            .outcome = bloom::platform::StagedArtifactPublicationOutcome::Superseded,
            .error = StagedArtifactError::None},
        bloom::host::PublicationIntentId{}));
    e.expect(!superseded.has_value(), "a superseded save is not described as a failure");
    const auto accepted = describeSessionSaveFailure(SessionSaveResult::published(
        bloom::platform::StagedArtifactPublicationResult{
            .outcome = bloom::platform::StagedArtifactPublicationOutcome::Published,
            .error = StagedArtifactError::None},
        bloom::host::PublicationIntentId{}, bloom::host::ProjectSessionSavepointStatus::Accepted));
    e.expect(!accepted.has_value(), "an accepted save has no failure");
    const auto stale = describeSessionSaveFailure(SessionSaveResult::published(
        bloom::platform::StagedArtifactPublicationResult{
            .outcome = bloom::platform::StagedArtifactPublicationOutcome::Published,
            .error = StagedArtifactError::None},
        bloom::host::PublicationIntentId{},
        bloom::host::ProjectSessionSavepointStatus::StaleIntent));
    e.expect(stale.has_value() && stale->stage == "savepoint" && stale->cause == "stale-intent" &&
                 contains(stale->message, "reopen the file"),
             "a written file this session did not accept says to reopen it");
}

} // namespace

int main() {
    Expectations expectations;
    testAdmission(expectations);
    testPreflight(expectations);
    testRegistration(expectations);
    testStaging(expectations);
    testGuard(expectations);
    testCancelled(expectations);
    testResourceExhaustedAndUnexpectedAtEveryStage(expectations);
    testStagedSaveBuild(expectations);
    testStagedSavePlatformSteps(expectations);
    testStagedSaveVerification(expectations);
    testEveryEnumeratorIsNamed(expectations);
    testSessionResults(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
