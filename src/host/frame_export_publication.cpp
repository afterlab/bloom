#include <bloom/host/frame_export_publication.hpp>

#include <bloom/core/sha256.hpp>
#include <bloom/output/dpx_export_write.hpp>
#include <bloom/output/flat_exr_options.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/host/save_failure_description.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace bloom::host {

FrameExportApprovalResultV1::FrameExportApprovalResultV1(
    const FrameExportApprovalStatusV1 status) noexcept
    : status_(status) {}

FrameExportApprovalResultV1::FrameExportApprovalResultV1(
    std::unique_ptr<FrameExportRequestV1> request) noexcept
    : status_(FrameExportApprovalStatusV1::Approved), request_(std::move(request)) {}

FrameExportApprovalResultV1::~FrameExportApprovalResultV1() = default;

std::unique_ptr<FrameExportRequestV1> FrameExportApprovalResultV1::takeRequest() && noexcept {
    return std::move(request_);
}

FrameExportRequestV1::FrameExportRequestV1(
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt, PublicationTargetClaim claim,
    const FrameExportLimitsV1 limits) noexcept
    : attempt_(std::move(attempt)), claim_(std::move(claim)), limits_(limits) {}

FrameExportApprovalResultV1
approveFrameExportV1(PublicationCoordinator& coordinator,
                     std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt,
                     const core::Sha256Digest approverDigest, const FrameExportLimitsV1 limits,
                     PublicationAdmission* const preAdmitted) {
    if (attempt == nullptr || !attempt->approvable()) {
        return FrameExportApprovalResultV1(FrameExportApprovalStatusV1::NotApprovable);
    }
    if (!attempt->digest().has_value() || *attempt->digest() != approverDigest) {
        return FrameExportApprovalResultV1(FrameExportApprovalStatusV1::DigestMismatch);
    }

    std::optional<PublicationAdmission> admission;
    if (preAdmitted != nullptr) {
        admission.emplace(std::move(*preAdmitted));
    } else {
        auto admissionResult = coordinator.admit();
        if (!admissionResult) {
            FrameExportApprovalResultV1 result(FrameExportApprovalStatusV1::AdmissionFailed);
            result.admissionFailure_ = admissionResult.status();
            return result;
        }
        admission.emplace(std::move(admissionResult).takeAdmission());
    }

    auto registrationResult =
        coordinator.registerTarget(std::move(*admission), attempt->target().targetKey);
    if (!registrationResult) {
        FrameExportApprovalResultV1 result(FrameExportApprovalStatusV1::RegistrationFailed);
        result.registrationFailure_ = registrationResult.status();
        return result;
    }
    auto claim = std::move(registrationResult).takeClaim();

    try {
        auto request = std::make_unique<FrameExportRequestV1>(
            FrameExportRequestV1(std::move(attempt), std::move(claim), limits));
        return FrameExportApprovalResultV1(std::move(request));
    } catch (const std::bad_alloc&) {
        // The claim (and, transitively, its target-claim record) abandons via RAII on this path;
        // nothing else was published or entered.
        return FrameExportApprovalResultV1(FrameExportApprovalStatusV1::AdmissionFailed);
    }
}

FrameExportPublicationResultV1
FrameExportPublicationResultV1::published(platform::StagedArtifactPublicationResult publication,
                                          const PublicationIntentId intentId,
                                          const std::optional<core::Sha256Digest> semanticDigest,
                                          const std::optional<core::Sha256Digest> artifactDigest,
                                          const std::uint64_t artifactByteCount) noexcept {
    FrameExportPublicationResultV1 result;
    result.publication_ = publication;
    result.intentId_ = intentId;
    result.semanticDigest_ = semanticDigest;
    result.artifactDigest_ = artifactDigest;
    result.artifactByteCount_ = artifactByteCount;
    return result;
}

FrameExportPublicationResultV1 FrameExportPublicationResultV1::failure(
    FrameExportPublicationFailureV1 failureValue,
    const std::optional<platform::StagedArtifactError> rejectDiagnostic) noexcept {
    FrameExportPublicationResultV1 result;
    result.failure_.emplace(failureValue); // trivially copyable; std::move() would be a no-op
    result.rejectDiagnostic_ = rejectDiagnostic;
    return result;
}

namespace {

[[nodiscard]] std::optional<std::uint64_t> addChecked(const std::uint64_t left,
                                                      const std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return std::nullopt;
    return left + right;
}

[[nodiscard]] std::optional<std::uint64_t> multiplyChecked(const std::uint64_t left,
                                                           const std::uint64_t right) noexcept {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right)
        return std::nullopt;
    return left * right;
}

[[nodiscard]] std::filesystem::path uniqueScratchFilePath(const std::filesystem::path& directory,
                                                          const std::string_view extension) {
    static std::atomic<std::uint64_t> counter{0};
    const auto suffix = counter.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return directory / (".bloom-export-scratch-" + std::to_string(now) + "-" +
                        std::to_string(suffix) + std::string(extension));
}

// The doc's ordered stage vocabulary, mapped onto the existing TaskContext::reportProgress()
// phase strings (frame-output.md, "Non-Blocking Execution"). EXR only ever produces Writing,
// Verifying, and Publishing here -- exactly the three the previous inline ternary produced.
[[nodiscard]] std::string exportStagePhase(const output::OutputExportStageV1 stage) {
    switch (stage) {
    case output::OutputExportStageV1::Resolving:
        return "Resolving";
    case output::OutputExportStageV1::Evaluating:
        return "Evaluating";
    case output::OutputExportStageV1::Identifying:
        return "Identifying";
    case output::OutputExportStageV1::ColorPreparing:
        return "ColorPreparing";
    case output::OutputExportStageV1::Analyzing:
        return "Analyzing";
    case output::OutputExportStageV1::PreparingOutput:
        return "PreparingOutput";
    case output::OutputExportStageV1::Writing:
        return "Writing";
    case output::OutputExportStageV1::Verifying:
        return "Verifying";
    case output::OutputExportStageV1::Publishing:
        break;
    }
    return "Publishing";
}

// The preset-independent shape of "the staged bytes exist at the scratch path and carry these two
// digests", so the publication half below stays one code path for both presets.
struct WriteVerifyOutcomeV1 final {
    bool written = false;
    bool cancelled = false;
    FrameExportPublicationStageV1 stage = FrameExportPublicationStageV1::Writing;
    FrameExportPublicationFailurePayloadV1 payload{};
    core::Sha256Digest semanticDigest;
    core::Sha256Digest artifactDigest;
    std::uint64_t artifactByteCount = 0;
};

// Maps the PNG bridge's typed error onto the publication stage it happened at. ColorPreparing and
// PreparingOutput both belong to the doc's node 2 ("dependent output preparation"); the remaining
// codes belong to node 3's writer/verifier halves.
[[nodiscard]] FrameExportPublicationStageV1
pngFailureStage(const output::PngExportWriteErrorCodeV1 error) noexcept {
    switch (error) {
    case output::PngExportWriteErrorCodeV1::MissingDisplayProducts:
    case output::PngExportWriteErrorCodeV1::PreparedBytesLimitExceeded:
    case output::PngExportWriteErrorCodeV1::ColorPrepareFailed:
        return FrameExportPublicationStageV1::ColorPreparing;
    case output::PngExportWriteErrorCodeV1::VerifyFailed:
        return FrameExportPublicationStageV1::Verifying;
    case output::PngExportWriteErrorCodeV1::None:
    case output::PngExportWriteErrorCodeV1::InvalidAttempt:
    case output::PngExportWriteErrorCodeV1::WriteFailed:
    case output::PngExportWriteErrorCodeV1::ArtifactHashFailed:
    case output::PngExportWriteErrorCodeV1::InternalInvariant:
        break;
    }
    return FrameExportPublicationStageV1::Writing;
}

class ScratchFileGuard final {
  public:
    explicit ScratchFileGuard(std::filesystem::path path) noexcept : path_(std::move(path)) {}
    ScratchFileGuard(const ScratchFileGuard&) = delete;
    ScratchFileGuard& operator=(const ScratchFileGuard&) = delete;
    ~ScratchFileGuard() {
        std::error_code errorCode;
        std::filesystem::remove(path_, errorCode);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] FrameExportPublicationResultV1
cancelledResult(const FrameExportPublicationStageV1 /*lastStage*/) noexcept {
    // The stage a cancellation checkpoint was observed at is not separately distinguished --
    // mirrors SavePublicationStage::Cancelled's identical established rationale (every checkpoint
    // here never calls lease.publish(), so the caller-observable contract, "no replacement
    // occurred", is identical at all of them).
    return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
        FrameExportPublicationStageV1::Cancelled, std::monostate{}));
}

} // namespace

namespace {
FrameExportPublicationResultV1 executeExportPublicationImpl(
    runtime::TaskContext& context, platform::StagedArtifactCoordinator& artifacts,
    FrameExportRequestV1 request, const std::filesystem::path& scratchDirectory,
    output::OutputExportClockV1 clock, output::OutputExportProgressCallbackV1 progress,
    const FrameExportPublicationGateV1& beforePublication,
    FrameExportPublicationStageV1& failureStage) {
    if (!clock) {
        clock = output::systemOutputExportClockV1();
    }
    const auto start = clock();
    auto lastProgress = start;
    const auto& limits = request.limits();

    const auto deadlineExceeded = [&](const std::chrono::steady_clock::time_point now) noexcept {
        return now - start > limits.totalDeadline;
    };
    const auto noProgressExceeded = [&](const std::chrono::steady_clock::time_point now) noexcept {
        return now - lastProgress > limits.noProgressInterval;
    };

    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Preflight);
    }

    const auto& attempt = *request.attempt();
    const auto& target = attempt.target();

    // --- Preflight (design decision 4 node 1: "CPU output preflight validates the retained
    // attempt/request binding and checked aggregate resources without reevaluating the
    // composition or rehashing the process frame") -- revalidates the SAME platform preflight the
    // attempt's own Resolving stage already ran, reusing artifacts.preflight() rather than
    // reimplementing any of the checks it owns.
    auto preflightResult = artifacts.preflight({.targetPath = target.targetPath,
                                                .overwritePolicy = target.overwritePolicy,
                                                .expectedTarget = target.observation});
    if (!preflightResult) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Preflight, preflightResult.error()));
    }
    auto platformTarget = std::move(preflightResult).takeTarget();
    if (platformTarget.targetKey() != target.targetKey) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Preflight, FrameExportUnexpectedFailureV1{}));
    }

    // Admission precedes staging and every writer allocation. The attempt charge already holds
    // process pixels and any GPU-encoded display payload; this scratch claim covers preparation,
    // writer, verifier and protocol-copy overlap without counting those retained products again.
    const bool isPng = attempt.preset() == output::OutputPresetV1::PngRgba8SrgbV1;
    const bool isTiff = attempt.preset() == output::OutputPresetV1::TiffRgba16SrgbV1;
    const bool isDpx = output::isDpxPresetV1(attempt.preset());
    const auto* descriptor = attempt.frame()->processImage().descriptor();
    if (!descriptor)
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Preflight, FrameExportUnexpectedFailureV1{}));
    const auto extent = descriptor->dataWindow().extent();
    const std::uint64_t width = extent.width();
    const std::uint64_t height = extent.height();
    constexpr std::uint64_t chunk = output::kOutputAdapterMaximumStreamingChunkBytesV1;
    // zlib's default deflate tables/hash state are below 320 KiB; inflate is below 40 KiB.
    // One MiB covers their state and small codec objects in either sequential stage.
    constexpr std::uint64_t pngCodecStateEstimate = 1024ULL * 1024ULL;
    std::optional<std::uint64_t> additional;
    std::uint64_t preparedBytes = 0;
    if (isPng) {
        const auto counted = output::checkedPngPreparedByteCountV1(attempt);
        if (!counted.has_value()) {
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::Preflight, FrameExportUnexpectedFailureV1{}));
        }
        if (limits.preparedPngByteLimit && *counted > *limits.preparedPngByteLimit) {
            return FrameExportPublicationResultV1::failure(
                FrameExportPublicationFailureV1(FrameExportPublicationStageV1::ColorPreparing,
                                                FrameExportPreparedBytesExceededV1{}));
        }
        preparedBytes = *counted;
        const auto rowPayload = multiplyChecked(width, 4);
        const auto row = rowPayload ? addChecked(*rowPayload, 1) : std::nullopt;
        const auto decoded = row ? multiplyChecked(*row, height) : std::nullopt;
        // The writer streams row spans directly; it owns a 16 MiB pending IDAT block and a
        // 64 KiB drain buffer, with no full-row copy.
        constexpr std::uint64_t writer = chunk + 64ULL * 1024ULL;
        const auto verifier =
            decoded ? addChecked(*decoded, std::max(preparedBytes, chunk)) : std::nullopt;
        const auto scratch = verifier
                                 ? addChecked(std::max(writer, *verifier), pngCodecStateEstimate)
                                 : std::nullopt;
        additional = scratch ? addChecked(preparedBytes, *scratch) : std::nullopt;
    } else if (isTiff || isDpx) {
        // The protocol prepares one frame and holds one serialized/copy buffer until acknowledged.
        const auto protocol =
            multiplyChecked(static_cast<std::uint64_t>(descriptor->layout().pixelStorageBytes), 2);
        additional = protocol ? addChecked(*protocol, chunk) : std::nullopt;
    } else {
        // Reopen verification owns componentBits for the full image. The optional transform row
        // may coexist with it; codec-owned workspace follows the qualified OpenEXR version's
        // compression geometry and one-thread endpoints. This is an admission estimate rather
        // than telemetry of OpenEXR's actual allocations.
        const auto row = multiplyChecked(width, 16);
        const auto componentBits =
            static_cast<std::uint64_t>(descriptor->layout().pixelStorageBytes);
        const auto verifier = row ? addChecked(componentBits, *row) : std::nullopt;
        const auto compression = attempt.report()->exr()
                                     ? attempt.report()->exr()->options().compression
                                     : output::FlatExrCompressionV1::Zip;
        const auto codec = output::flatExrCodecWorkspaceBytesV1(width, height, compression);
        additional = verifier && codec ? addChecked(*verifier, *codec) : std::nullopt;
    }
    output::ExportResourceAdmissionStatusV1 scratchStatus{};
    auto scratchClaim = additional ? attempt.resources()->reserveScratch(*additional, scratchStatus,
                                                                         context.cancellation())
                                   : nullptr;
    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Preflight);
    }
    if (!scratchClaim) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Preflight,
            FrameExportResourceExhaustedV1{.requestedBytes = additional.value_or(0),
                                           .status = scratchStatus}));
    }

    // #740: the scratch folder is the caller's, but it lives in a cache folder anything outside
    // Bloom may clear while Bloom runs. It is created again before every publication, so a
    // removed folder never fails every later export until Bloom restarts.
    if (!scratchDirectory.empty()) {
        std::error_code folderError;
        std::filesystem::create_directories(scratchDirectory, folderError);
        const bool isFolder =
            !folderError && std::filesystem::is_directory(scratchDirectory, folderError);
        if (!isFolder)
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::Preflight,
                FrameExportScratchUnavailableV1{
                    .systemError = folderError
                                       ? folderError.value()
                                       : static_cast<int>(std::errc::not_a_directory)}));
    }

    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Preflight);
    }

    // --- Staging.
    failureStage = FrameExportPublicationStageV1::Staging;
    auto stageResult = artifacts.stage(std::move(platformTarget));
    if (!stageResult) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Staging, stageResult.error()));
    }
    auto lease = std::move(stageResult).takeLease();

    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Staging);
    }
    failureStage = isPng ? FrameExportPublicationStageV1::ColorPreparing
                         : FrameExportPublicationStageV1::Writing;

    // --- (PNG only: ColorPreparing + PreparingOutput, node 2) then Writing + Verifying (design
    // decision 4 node 3's first half), through the preset's own bridge: F1's flat OpenEXR
    // writer/verifier, or G1's PNG writer/verifier behind the retained qualified display
    // processor. Both target a caller-owned scratch path bridging their path-based APIs to the
    // lease's byte-oriented staging (see bloom/output/flat_exr_export_write.hpp and
    // bloom/output/png_export_write.hpp's own namespace comments for the full rationale); the
    // publication half below is one shared code path for both. The bridging progress callback --
    // the same sink for both presets -- also resets the no-progress clock;
    // a mid-call deadline/no-progress violation can only be detected once the bridge's own
    // (synchronous, uninterruptible from here) call returns -- runtime::CancellationToken exposes
    // no caller-side "request my own cancellation" seam, and changing task_scheduler.hpp is out of
    // this task's scope. See the implementor's report for the full limitation.
    const ScratchFileGuard scratch(uniqueScratchFilePath(scratchDirectory, isPng    ? ".png"
                                                                           : isTiff ? ".tiff"
                                                                           : isDpx  ? ".dpx"
                                                                                    : ".exr"));
    const auto stageProgressSink = [&](const output::OutputExportProgressV1& stageProgress) {
        lastProgress = clock();
        context.reportProgress({.phase = exportStagePhase(stageProgress.stage),
                                .subphase = "",
                                .completed = stageProgress.completed,
                                .total = stageProgress.total});
        if (progress) {
            progress(stageProgress);
        }
    };

    WriteVerifyOutcomeV1 writeVerify;
    if (isPng) {
        const output::PngExportWriterV1 pngWriter;
        const auto pngResult = pngWriter.run(attempt, scratch.path(), context.cancellation(),
                                             stageProgressSink, limits.preparedPngByteLimit);
        writeVerify.cancelled = pngResult.status() == output::PngExportWriteStatusV1::Cancelled;
        writeVerify.written = pngResult.status() == output::PngExportWriteStatusV1::Written;
        if (writeVerify.written) {
            writeVerify.semanticDigest = pngResult.semanticDigest();
            writeVerify.artifactDigest = pngResult.artifactDigest();
            writeVerify.artifactByteCount = pngResult.artifactByteCount();
        } else if (!writeVerify.cancelled) {
            writeVerify.stage = pngFailureStage(pngResult.error());
            writeVerify.payload = pngResult.error();
        }
    } else if (isTiff) {
        const output::TiffExportWriterV1 writer;
        const auto tiffResult =
            writer.run(attempt, scratch.path(), nullptr, context.cancellation());
        writeVerify.cancelled = tiffResult.cancelled;
        writeVerify.written = tiffResult.written;
        if (tiffResult.written) {
            writeVerify.semanticDigest = tiffResult.semanticDigest;
            writeVerify.artifactDigest = tiffResult.artifactDigest;
            writeVerify.artifactByteCount = tiffResult.artifactBytes;
        }
        if (!writeVerify.written && !writeVerify.cancelled) {
            writeVerify.stage = FrameExportPublicationStageV1::Writing;
            writeVerify.payload = tiffResult.error;
        }
    } else if (isDpx) {
        const output::DpxExportWriterV1 writer;
        const auto dpxResult = writer.run(attempt, scratch.path(), context.cancellation());
        writeVerify.cancelled = dpxResult.cancelled;
        writeVerify.written = dpxResult.written;
        if (dpxResult.written) {
            writeVerify.semanticDigest = dpxResult.semanticDigest;
            writeVerify.artifactDigest = dpxResult.artifactDigest;
            writeVerify.artifactByteCount = dpxResult.artifactBytes;
        }
        if (!writeVerify.written && !writeVerify.cancelled) {
            // A header that differs from what was asked for is a verification failure.
            writeVerify.stage = dpxResult.error == output::DpxExportWriteErrorCodeV1::TagsDiffer
                                    ? FrameExportPublicationStageV1::Verifying
                                    : FrameExportPublicationStageV1::Writing;
            writeVerify.payload = dpxResult.error;
        }
    } else {
        const output::FlatExrExportWriterV1 writer;
        const auto exrResult =
            writer.run(attempt, scratch.path(), context.cancellation(), stageProgressSink);
        writeVerify.cancelled = exrResult.status() == output::FlatExrExportWriteStatusV1::Cancelled;
        writeVerify.written = exrResult.status() == output::FlatExrExportWriteStatusV1::Written;
        if (writeVerify.written) {
            writeVerify.semanticDigest = exrResult.semanticDigest();
            writeVerify.artifactDigest = exrResult.artifactDigest();
            writeVerify.artifactByteCount = exrResult.artifactByteCount();
        } else if (!writeVerify.cancelled) {
            writeVerify.stage =
                exrResult.error() == output::FlatExrExportWriteErrorCodeV1::WriteFailed
                    ? FrameExportPublicationStageV1::Writing
                    : FrameExportPublicationStageV1::Verifying;
            writeVerify.payload = exrResult.error();
        }
    }

    const auto afterWriteVerify = clock();
    if (deadlineExceeded(afterWriteVerify)) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Writing,
            FrameExportDeadlineExceededV1{.limit = limits.totalDeadline}));
    }
    if (noProgressExceeded(afterWriteVerify)) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Writing,
            FrameExportNoProgressExceededV1{.limit = limits.noProgressInterval}));
    }
    if (writeVerify.cancelled) {
        return cancelledResult(FrameExportPublicationStageV1::Writing);
    }
    if (!writeVerify.written) {
        return FrameExportPublicationResultV1::failure(
            FrameExportPublicationFailureV1(writeVerify.stage, writeVerify.payload));
    }

    // --- Artifact copy (design decision 4 node 3's "artifact SHA-256 + flush"): stream the
    // verified scratch bytes into the lease's own exclusive staged file, checking cancellation
    // and this stage's deadline/no-progress interval BETWEEN chunks -- the one segment of this
    // pipeline this code fully owns and can therefore genuinely preempt.
    failureStage = FrameExportPublicationStageV1::ArtifactCopy;
    {
        std::ifstream scratchStream(scratch.path(), std::ios::binary);
        if (!scratchStream) {
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::ArtifactCopy, FrameExportUnexpectedFailureV1{}));
        }
        std::vector<std::byte> buffer(output::kOutputAdapterMaximumStreamingChunkBytesV1);
        while (scratchStream) {
            const auto now = clock();
            if (context.isCancellationRequested()) {
                return cancelledResult(FrameExportPublicationStageV1::ArtifactCopy);
            }
            if (deadlineExceeded(now)) {
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy,
                    FrameExportDeadlineExceededV1{.limit = limits.totalDeadline}));
            }
            if (noProgressExceeded(now)) {
                return FrameExportPublicationResultV1::failure(
                    FrameExportPublicationFailureV1(FrameExportPublicationStageV1::ArtifactCopy,
                                                    FrameExportNoProgressExceededV1{
                                                        .limit = limits.noProgressInterval}));
            }
            scratchStream.read(reinterpret_cast<char*>(buffer.data()),
                               static_cast<std::streamsize>(buffer.size()));
            const auto readCount = static_cast<std::size_t>(scratchStream.gcount());
            if (readCount == 0) {
                break;
            }
            const auto writeResult = lease.write(std::span(buffer).first(readCount));
            if (!writeResult) {
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy, writeResult.error));
            }
            lastProgress = clock();
        }
        if (scratchStream.bad()) {
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::ArtifactCopy, FrameExportUnexpectedFailureV1{}));
        }
    }

    const auto finishResult = lease.finishWriting();
    if (!finishResult) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::ArtifactCopy, finishResult.error));
    }

    // #544: the caller's turn to publish. Waiting for earlier frames is not this publication's
    // own lack of progress, so the no-progress interval starts again after it.
    if (beforePublication) {
        if (!beforePublication(context.cancellation()) || context.isCancellationRequested()) {
            static_cast<void>(lease.rejectVerification());
            return cancelledResult(FrameExportPublicationStageV1::ArtifactCopy);
        }
        lastProgress = clock();
    }

    // Read the lease's own staged bytes back and recompute their SHA-256, proving the exclusive
    // staged file the coordinator will atomically publish is byte-identical to what F1 verified --
    // "F1 verifier on the staged identity (Verifying)" extended to the file that is actually
    // published, not only the bridging scratch copy.
    {
        core::Sha256Hasher rehasher;
        std::vector<std::byte> buffer(output::kOutputAdapterMaximumStreamingChunkBytesV1);
        std::uint64_t offset = 0;
        while (true) {
            const auto now = clock();
            if (context.isCancellationRequested()) {
                static_cast<void>(lease.rejectVerification());
                return cancelledResult(FrameExportPublicationStageV1::ArtifactCopy);
            }
            if (deadlineExceeded(now) || noProgressExceeded(now)) {
                static_cast<void>(lease.rejectVerification());
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy,
                    deadlineExceeded(now)
                        ? FrameExportPublicationFailurePayloadV1(
                              FrameExportDeadlineExceededV1{.limit = limits.totalDeadline})
                        : FrameExportPublicationFailurePayloadV1(FrameExportNoProgressExceededV1{
                              .limit = limits.noProgressInterval})));
            }
            const auto readResult = lease.readForVerification(offset, std::span(buffer));
            if (!readResult) {
                static_cast<void>(lease.rejectVerification());
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy, readResult.error));
            }
            if (readResult.bytesRead > 0 && !rehasher.update(std::span(buffer).first(
                                                static_cast<std::size_t>(readResult.bytesRead)))) {
                static_cast<void>(lease.rejectVerification());
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy, FrameExportUnexpectedFailureV1{}));
            }
            offset += readResult.bytesRead;
            lastProgress = clock();
            if (readResult.endOfFile) {
                break;
            }
        }
        if (rehasher.finalize() != writeVerify.artifactDigest) {
            static_cast<void>(lease.rejectVerification());
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::ArtifactCopy, FrameExportUnexpectedFailureV1{}));
        }
    }

    const auto acceptResult = lease.acceptVerification();
    if (!acceptResult) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::ArtifactCopy, acceptResult.error));
    }

    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Guard);
    }

    // --- Guard + Publishing: identical composition to executeCopyPublication()'s own final
    // switch (see its declaration comment for the full unreachability argument for
    // InvalidClaim/TargetBusy/AlreadyEntered in a single-executor composition).
    failureStage = FrameExportPublicationStageV1::Guard;
    auto& claim = FrameExportRequestAccessV1::claim(request);
    auto guardResult = claim.tryEnterPublication();
    const auto intentId = claim.intentId();
    switch (guardResult.status()) {
    case PublicationGuardStatus::Entered: {
        auto guard = std::move(guardResult).takeGuard();
        auto publication = lease.publish(platform::PublicationDisposition::Proceed);
        return FrameExportPublicationResultV1::published(
            publication, intentId, writeVerify.semanticDigest, writeVerify.artifactDigest,
            writeVerify.artifactByteCount);
    }
    case PublicationGuardStatus::Superseded:
        return FrameExportPublicationResultV1::published(
            lease.publish(platform::PublicationDisposition::Superseded), intentId, std::nullopt,
            std::nullopt, 0);
    case PublicationGuardStatus::InvalidClaim:
    case PublicationGuardStatus::TargetBusy:
    case PublicationGuardStatus::AlreadyEntered:
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Guard, guardResult.status()));
    }
    return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
        FrameExportPublicationStageV1::Guard, guardResult.status()));
}
} // namespace

FrameExportPublicationResultV1 executeExportPublication(
    runtime::TaskContext& context, platform::StagedArtifactCoordinator& artifacts,
    FrameExportRequestV1 request, const std::filesystem::path& scratchDirectory,
    output::OutputExportClockV1 clock, output::OutputExportProgressCallbackV1 progress,
    FrameExportPublicationGateV1 beforePublication) noexcept {
    auto failureStage = FrameExportPublicationStageV1::Preflight;
    try {
        return executeExportPublicationImpl(context, artifacts, std::move(request),
                                            scratchDirectory, std::move(clock),
                                            std::move(progress), beforePublication, failureStage);
    } catch (const std::bad_alloc&) {
        // Unwinding has already destroyed the staged lease, scratch guard and scratch claim.
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            failureStage, FrameExportResourceExhaustedV1{.allocationFailed = true}));
    } catch (...) {
        return FrameExportPublicationResultV1::failure(
            FrameExportPublicationFailureV1(failureStage, FrameExportUnexpectedFailureV1{}));
    }
}

namespace {

constexpr std::array<std::string_view, 8> kFrameStageTokens{
    "preflight", "staging",       "color-preparing", "writing",
    "verifying", "artifact-copy", "guard",           "cancelled",
};
static_assert(static_cast<std::size_t>(FrameExportPublicationStageV1::Cancelled) + 1 ==
              kFrameStageTokens.size());
constexpr std::array<std::string_view, 8> kFrameStagePhrases{
    "while preparing to write it",
    "while creating its temporary file",
    "while converting it to the output colors",
    "while writing it",
    "while reading it back to verify it",
    "while copying it into place",
    "while publishing it",
    "because the export was cancelled",
};
static_assert(kFrameStagePhrases.size() == kFrameStageTokens.size());
constexpr std::array<std::string_view, 6> kFailureKindTokens{
    "timeout", "memory", "output-path", "writer", "cancelled", "internal",
};
static_assert(static_cast<std::size_t>(FrameExportPublicationFailureKindV1::Internal) + 1 ==
              kFailureKindTokens.size());
constexpr std::array<std::string_view, 5> kAdmissionTokens{
    "reserved",
    "arithmetic-overflow",
    "job-allowance-exceeded",
    "service-allowance-exceeded",
    "capacity-refresh-failed",
};
static_assert(static_cast<std::size_t>(output::ExportResourceAdmissionStatusV1::CapacityRefreshFailed) +
                  1 ==
              kAdmissionTokens.size());
constexpr std::array<std::string_view, 5> kExrWriterTokens{
    "none", "exr-write-failed", "exr-verify-failed", "exr-artifact-hash-failed",
    "exr-internal-invariant",
};
static_assert(static_cast<std::size_t>(output::FlatExrExportWriteErrorCodeV1::InternalInvariant) +
                  1 ==
              kExrWriterTokens.size());
constexpr std::array<std::string_view, 9> kPngWriterTokens{
    "none",
    "png-invalid-attempt",
    "png-missing-display-products",
    "png-prepared-bytes-limit-exceeded",
    "png-color-prepare-failed",
    "png-write-failed",
    "png-verify-failed",
    "png-artifact-hash-failed",
    "png-internal-invariant",
};
static_assert(static_cast<std::size_t>(output::PngExportWriteErrorCodeV1::InternalInvariant) + 1 ==
              kPngWriterTokens.size());
constexpr std::array<std::string_view, 4> kTiffWriterTokens{
    "none", "tiff-provider-missing", "tiff-provider-failed", "tiff-cancelled"};
static_assert(static_cast<std::size_t>(output::TiffExportWriteErrorCodeV1::Cancelled) + 1 ==
              kTiffWriterTokens.size());
constexpr std::array<std::string_view, 5> kDpxWriterTokens{
    "none", "dpx-provider-missing", "dpx-provider-failed", "dpx-cancelled", "dpx-tags-differ"};
static_assert(static_cast<std::size_t>(output::DpxExportWriteErrorCodeV1::TagsDiffer) + 1 ==
              kDpxWriterTokens.size());
constexpr std::array<std::string_view, 5> kGuardTokens{
    "entered", "invalid-claim", "superseded", "target-busy", "already-entered",
};
static_assert(static_cast<std::size_t>(PublicationGuardStatus::AlreadyEntered) + 1 ==
              kGuardTokens.size());

template <typename Enum, std::size_t N>
[[nodiscard]] std::string_view tokenOf(const std::array<std::string_view, N>& tokens,
                                       const Enum value) noexcept {
    const auto index = static_cast<std::size_t>(value);
    return index < N ? tokens[index] : std::string_view{"unrecognized"};
}

// "512 MiB", "1.5 GiB", "4096 bytes".
[[nodiscard]] std::string bytesText(const std::uint64_t bytes) {
    constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
    constexpr std::uint64_t gibibyte = 1024ULL * mebibyte;
    if (bytes >= gibibyte) {
        const auto tenths = (bytes * 10 + gibibyte / 2) / gibibyte;
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " GiB";
    }
    if (bytes >= mebibyte)
        return std::to_string((bytes + mebibyte / 2) / mebibyte) + " MiB";
    return std::to_string(bytes) + " bytes";
}

// "120 s", "24 h".
[[nodiscard]] std::string durationText(const std::chrono::steady_clock::duration duration) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    if (seconds >= 3600 && seconds % 3600 == 0)
        return std::to_string(seconds / 3600) + " h";
    if (seconds > 0)
        return std::to_string(seconds) + " s";
    if (const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        millis > 0)
        return std::to_string(millis) + " ms";
    return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()) +
           " ns";
}

struct FrameCause final {
    FrameExportPublicationFailureKindV1 kind = FrameExportPublicationFailureKindV1::Internal;
    std::string token;
    std::string phrase;
    bool retryable = false;
};

[[nodiscard]] FrameCause stagedCause(const platform::StagedArtifactError error) {
    using E = platform::StagedArtifactError;
    using K = FrameExportPublicationFailureKindV1;
    const auto token = std::string(stagedArtifactErrorToken(error));
    switch (error) {
    case E::ResourceUnavailable:
        return {K::Memory, token, "the system ran out of memory or file handles", true};
    case E::AdmissionLimit:
    case E::TargetRecordLimit:
        return {K::OutputPath, token, "too many files are being written at once", true};
    case E::InvalidTargetPath:
        return {K::OutputPath, token, "the output path is not a valid file path", false};
    case E::ParentResolutionFailed:
        return {K::OutputPath, token, "the output folder could not be found", false};
    case E::ParentNotDirectory:
        return {K::OutputPath, token, "the output folder is not a folder", false};
    case E::TargetLeafSymlink:
        return {K::OutputPath, token, "the output file is a symbolic link", false};
    case E::TargetLeafNotRegular:
        return {K::OutputPath, token, "the output path is not a regular file", false};
    case E::TargetInspectionFailed:
        return {K::OutputPath, token, "the output file could not be inspected", false};
    case E::ArtifactSizeLimit:
        return {K::OutputPath, token, "the file is larger than the most Bloom writes in one file",
                false};
    case E::ExternalModificationConflict:
        return {K::OutputPath, token, "the output file changed outside Bloom", false};
    case E::OverwritePolicyConflict:
        return {K::OutputPath, token, "the output file already exists", false};
    case E::StageCreationFailed:
        return {K::OutputPath, token,
                "its temporary file could not be created in the output folder (check that the "
                "folder is writable)",
                false};
    case E::StageWriteFailed:
        return {K::OutputPath, token,
                "its temporary file could not be written in the output folder (check the free "
                "disk space)",
                false};
    case E::StageFlushFailed:
        return {K::OutputPath, token, "its temporary file could not be flushed to the disk",
                false};
    case E::AtomicPublicationFailed:
        return {K::OutputPath, token, "its temporary file could not replace the output file",
                false};
    case E::ParentDurabilityFailed:
        return {K::OutputPath, token, "the output folder could not be flushed to the disk", false};
    default:
        break;
    }
    std::string words = token;
    std::ranges::replace(words, '-', ' ');
    return {K::OutputPath, token, "the output file could not be written (" + words + ")", false};
}

[[nodiscard]] FrameCause writerCause(const std::string_view token, const bool verify) {
    using K = FrameExportPublicationFailureKindV1;
    if (verify)
        return {K::Writer, std::string(token),
                "the written file did not read back as the frame Bloom rendered", false};
    return {K::Writer, std::string(token),
            "the file could not be written in Bloom's export scratch folder (check the free disk "
            "space there)",
            false};
}

[[nodiscard]] FrameCause frameCause(const FrameExportPublicationFailureV1& failure) {
    using K = FrameExportPublicationFailureKindV1;
    return std::visit(
        [&](const auto& payload) -> FrameCause {
            using P = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<P, std::monostate>) {
                if (failure.stage() == FrameExportPublicationStageV1::Cancelled)
                    return {K::Cancelled, "cancelled", "the export was cancelled", false};
                return {K::Internal, "unexpected-failure", "an unexpected internal error occurred",
                        false};
            } else if constexpr (std::is_same_v<P, platform::StagedArtifactError>) {
                return stagedCause(payload);
            } else if constexpr (std::is_same_v<P, output::FlatExrExportWriteErrorCodeV1>) {
                return writerCause(tokenOf(kExrWriterTokens, payload),
                                   payload == output::FlatExrExportWriteErrorCodeV1::VerifyFailed);
            } else if constexpr (std::is_same_v<P, output::PngExportWriteErrorCodeV1>) {
                using E = output::PngExportWriteErrorCodeV1;
                const auto token = std::string(tokenOf(kPngWriterTokens, payload));
                if (payload == E::ColorPrepareFailed)
                    return {K::Writer, token,
                            "the output colors could not be applied to it (a sample that is not "
                            "a number, or the computer ran out of memory)",
                            false};
                if (payload == E::PreparedBytesLimitExceeded)
                    return {K::Memory, token,
                            "the prepared PNG would be larger than the export's limit for one "
                            "file",
                            false};
                if (payload == E::WriteFailed || payload == E::VerifyFailed)
                    return writerCause(token, payload == E::VerifyFailed);
                return {K::Internal, token,
                        "an unexpected internal error occurred in the PNG writer", false};
            } else if constexpr (std::is_same_v<P, output::TiffExportWriteErrorCodeV1>) {
                return {K::Writer, std::string(tokenOf(kTiffWriterTokens, payload)),
                        "the TIFF writer could not write the file", false};
            } else if constexpr (std::is_same_v<P, output::DpxExportWriteErrorCodeV1>) {
                return writerCause(tokenOf(kDpxWriterTokens, payload),
                                   payload == output::DpxExportWriteErrorCodeV1::TagsDiffer);
            } else if constexpr (std::is_same_v<P, PublicationGuardStatus>) {
                if (payload == PublicationGuardStatus::TargetBusy)
                    return {K::OutputPath, "target-busy",
                            "another export is writing the same output file", true};
                return {K::Internal, std::string(tokenOf(kGuardTokens, payload)),
                        "an unexpected internal error occurred while publishing", false};
            } else if constexpr (std::is_same_v<P, FrameExportDeadlineExceededV1>) {
                return {K::Timeout, "deadline-exceeded",
                        "it took longer than the export's limit of " + durationText(payload.limit) +
                            " for one frame's publication",
                        false};
            } else if constexpr (std::is_same_v<P, FrameExportNoProgressExceededV1>) {
                return {K::Timeout, "no-progress-limit",
                        "the writer made no progress for longer than the export's limit of " +
                            durationText(payload.limit),
                        false};
            } else if constexpr (std::is_same_v<P, FrameExportResourceExhaustedV1>) {
                using S = output::ExportResourceAdmissionStatusV1;
                if (payload.allocationFailed)
                    return {K::Memory, "allocation-failed",
                            "the computer ran out of memory; close a RAM preview or other work "
                            "and try again",
                            true};
                const auto amount = bytesText(payload.requestedBytes);
                const auto token = std::string(tokenOf(kAdmissionTokens, payload.status));
                if (payload.status == S::JobAllowanceExceeded ||
                    payload.status == S::ArithmeticOverflow)
                    return {K::Memory, token,
                            "writing it needs " + amount +
                                ", more memory than one export may hold; try a smaller resolution",
                            false};
                return {K::Memory,
                        payload.status == S::CapacityRefreshFailed ? token : "memory-refused",
                        "Bloom could not reserve the " + amount +
                            " of memory writing it needs while other work holds memory; close a "
                            "RAM preview or other exports and try again",
                        true};
            } else if constexpr (std::is_same_v<P, FrameExportPreparedBytesExceededV1>) {
                return {K::Memory, "prepared-bytes-limit",
                        "the prepared PNG would be larger than the export's limit for one file",
                        false};
            } else if constexpr (std::is_same_v<P, FrameExportScratchUnavailableV1>) {
                return {K::OutputPath, "scratch-folder-unavailable",
                        "Bloom's export scratch folder could not be created (" +
                            std::system_category().message(payload.systemError) + ")",
                        false};
            } else {
                static_assert(std::is_same_v<P, FrameExportUnexpectedFailureV1>);
                return {K::Internal, "unexpected-failure", "an unexpected internal error occurred",
                        false};
            }
        },
        failure.payload());
}

} // namespace

FrameExportPublicationFailureDescriptionV1
describeFrameExportPublicationFailureV1(const FrameExportPublicationFailureV1& failure,
                                        const std::uint64_t frame) {
    auto cause = frameCause(failure);
    FrameExportPublicationFailureDescriptionV1 description;
    description.kind = cause.kind;
    description.stage = std::string(tokenOf(kFrameStageTokens, failure.stage()));
    description.cause = std::move(cause.token);
    description.diagnosticCode =
        "export.publication." + std::string(tokenOf(kFailureKindTokens, cause.kind));
    description.message = "Frame " + std::to_string(frame) + " could not be published " +
                          std::string(tokenOf(kFrameStagePhrases, failure.stage())) + ": " +
                          cause.phrase + ".";
    description.retryable = cause.retryable;
    return description;
}

} // namespace bloom::host
