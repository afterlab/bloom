// Windows provider for the staged-artifact coordinator (ADR 0033). It keeps the Linux provider's
// state machine -- preflight pins the parent directory and fingerprints the target, stage creates
// a private ".bloom-stage-*" file, verification re-reads it, publication re-checks every identity
// and renames -- over Win32 handles: directory handles opened with FILE_FLAG_BACKUP_SEMANTICS,
// (volume serial, file index) identities, FILE_FLAG_OPEN_REPARSE_POINT so a link is never followed,
// and MoveFileExW for the publication rename (create-only fails if the target appeared, replace
// swaps in place). Windows cannot flush a directory, so MOVEFILE_WRITE_THROUGH carries the
// durability of the rename. Inheritable-writer streaming stays POSIX-only; the lease answers
// UnsupportedPlatform and callers keep the copy path.

#include "staged_artifact_platform.hpp"

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// bcrypt.h needs windows.h first.
#include <bcrypt.h>

#if !defined(_WIN32)
#error "staged_artifact_windows.cpp requires Windows"
#endif

namespace bloom::platform::detail {

namespace {

class Handle final {
  public:
    Handle() noexcept = default;
    explicit Handle(const HANDLE value) noexcept : value_(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~Handle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool isValid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

    [[nodiscard]] bool closeChecked() noexcept {
        if (!isValid()) {
            return true;
        }
        return ::CloseHandle(std::exchange(value_, INVALID_HANDLE_VALUE)) != 0;
    }

    void reset() noexcept {
        if (isValid()) {
            static_cast<void>(::CloseHandle(value_));
            value_ = INVALID_HANDLE_VALUE;
        }
    }

  private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct NativeFileIdentity final {
    std::uint64_t volume = 0;
    std::uint64_t index = 0;

    friend bool operator==(const NativeFileIdentity&, const NativeFileIdentity&) = default;
};

struct TargetIdentity final {
    NativeFileIdentity parent;
    std::wstring leaf;

    friend bool operator==(const TargetIdentity&, const TargetIdentity&) = default;
};

struct TargetIdentityHash final {
    [[nodiscard]] std::size_t operator()(const TargetIdentity& value) const noexcept {
        auto result = std::hash<std::uint64_t>{}(value.parent.volume);
        result ^= std::hash<std::uint64_t>{}(value.parent.index) + 0x9E3779B97F4A7C15ULL +
                  (result << 6U) + (result >> 2U);
        result ^= std::hash<std::wstring>{}(value.leaf) + 0x9E3779B97F4A7C15ULL + (result << 6U) +
                  (result >> 2U);
        return result;
    }
};

struct TargetRecord final {
    explicit TargetRecord(const core::ArtifactTargetKey value) noexcept : key(value) {}

    core::ArtifactTargetKey key;
    std::mutex publicationMutex;
};

struct WindowsSharedState final {
    explicit WindowsSharedState(const StagedArtifactConfig& value) noexcept : config(value) {}

    [[nodiscard]] bool shouldFail(const StagedArtifactFaultPoint point) noexcept {
        if (config.faults.point != point) {
            return false;
        }
        std::lock_guard lock(mutex);
        ++matchingFaultVisits;
        return matchingFaultVisits == config.faults.occurrence;
    }

    void releaseActiveTarget() noexcept {
        std::lock_guard lock(mutex);
        if (activeTargetCount == 0) {
            std::terminate();
        }
        --activeTargetCount;
    }

    void recordCleanupFailure() noexcept {
        std::lock_guard lock(mutex);
        ++cleanupFailureCount;
    }

    StagedArtifactConfig config;
    mutable std::mutex mutex;
    std::unordered_map<TargetIdentity, std::shared_ptr<TargetRecord>, TargetIdentityHash> records;
    std::size_t activeTargetCount = 0;
    std::uint64_t lastIssuedTargetKey = 0;
    std::uint64_t cleanupFailureCount = 0;
    std::uint64_t matchingFaultVisits = 0;
    bool targetIdentityExhausted = false;
};

class AdmissionToken final {
  public:
    AdmissionToken() noexcept = default;
    explicit AdmissionToken(std::shared_ptr<WindowsSharedState> state) noexcept
        : state_(std::move(state)) {}
    AdmissionToken(const AdmissionToken&) = delete;
    AdmissionToken& operator=(const AdmissionToken&) = delete;
    AdmissionToken(AdmissionToken&& other) noexcept : state_(std::move(other.state_)) {}
    AdmissionToken& operator=(AdmissionToken&& other) noexcept {
        if (this != &other) {
            release();
            state_ = std::move(other.state_);
        }
        return *this;
    }
    ~AdmissionToken() { release(); }

  private:
    void release() noexcept {
        if (state_ != nullptr) {
            state_->releaseActiveTarget();
            state_.reset();
        }
    }

    std::shared_ptr<WindowsSharedState> state_;
};

struct TargetData final {
    std::shared_ptr<WindowsSharedState> shared;
    AdmissionToken admission;
    std::shared_ptr<TargetRecord> record;
    Handle parentHandle;
    NativeFileIdentity parentIdentity;
    std::filesystem::path canonicalParentPath;
    std::wstring leaf;
    ArtifactTargetObservation expectedTarget;
    ArtifactOverwritePolicy overwritePolicy = ArtifactOverwritePolicy::CreateOrReplace;
};

struct InspectionResult final {
    explicit InspectionResult(const StagedArtifactError value) noexcept : error(value) {}
    explicit InspectionResult(const ArtifactTargetObservation value) noexcept
        : observation(value) {}

    StagedArtifactError error = StagedArtifactError::None;
    ArtifactTargetObservation observation;
};

// Everything the provider reads about one open file.
struct FileFacts final {
    NativeFileIdentity identity;
    std::uint64_t size = 0;
    std::uint32_t linkCount = 0;
    std::int64_t modificationSeconds = 0;
    std::uint32_t modificationNanoseconds = 0;
    bool regular = false;
    bool directory = false;
    bool reparse = false;

    [[nodiscard]] bool sameEvidence(const FileFacts& other) const noexcept {
        return identity == other.identity && size == other.size &&
               modificationSeconds == other.modificationSeconds &&
               modificationNanoseconds == other.modificationNanoseconds && other.regular;
    }
};

[[nodiscard]] std::optional<FileFacts> factsOf(const HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(handle, &info) == 0) {
        return std::nullopt;
    }
    constexpr std::uint64_t unixEpochTicks = 116'444'736'000'000'000ULL;
    const auto ticks = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32U) |
                       info.ftLastWriteTime.dwLowDateTime;
    const auto sinceEpoch = ticks >= unixEpochTicks ? ticks - unixEpochTicks : 0;
    FileFacts facts;
    facts.identity = {.volume = info.dwVolumeSerialNumber,
                      .index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
                               info.nFileIndexLow};
    facts.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
    facts.linkCount = info.nNumberOfLinks;
    facts.modificationSeconds = static_cast<std::int64_t>(sinceEpoch / 10'000'000ULL);
    facts.modificationNanoseconds = static_cast<std::uint32_t>((sinceEpoch % 10'000'000ULL) * 100U);
    facts.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    facts.reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    facts.regular =
        !facts.directory && !facts.reparse && (info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) == 0;
    return facts;
}

[[nodiscard]] Handle openDirectory(const std::filesystem::path& path) noexcept {
    return Handle(::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
}

// Opens `leaf` inside the parent without following a link.
[[nodiscard]] Handle openLeaf(const std::filesystem::path& parent, const std::wstring& leaf,
                              const DWORD access, const DWORD share,
                              const DWORD disposition) noexcept {
    try {
        const auto full = parent / leaf;
        return Handle(::CreateFileW(full.c_str(), access, share, nullptr, disposition,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    } catch (...) {
        return Handle();
    }
}

[[nodiscard]] ArtifactTargetFingerprint fingerprintFrom(const FileFacts& facts,
                                                        const std::uint64_t byteSize,
                                                        const core::Sha256Digest digest) noexcept {
    return {.identity = {.first = facts.identity.volume, .second = facts.identity.index},
            .byteSize = byteSize,
            .modificationSeconds = facts.modificationSeconds,
            .modificationNanoseconds = facts.modificationNanoseconds,
            .digest = digest};
}

[[nodiscard]] InspectionResult inspectTarget(const std::filesystem::path& parent,
                                             const std::wstring& leaf,
                                             const std::uint64_t byteLimit) noexcept {
    // A reparse point (symbolic link, junction) is inspected, never followed.
    Handle probe = openLeaf(parent, leaf, FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
    if (!probe.isValid()) {
        const auto code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return InspectionResult(ArtifactTargetObservation::absent());
        }
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }
    const auto pathFacts = factsOf(probe.get());
    if (!pathFacts.has_value()) {
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }
    if (pathFacts->reparse) {
        return InspectionResult(StagedArtifactError::TargetLeafSymlink);
    }
    if (!pathFacts->regular) {
        return InspectionResult(StagedArtifactError::TargetLeafNotRegular);
    }
    if (pathFacts->size > byteLimit) {
        return InspectionResult(StagedArtifactError::ArtifactSizeLimit);
    }
    probe.reset();

    Handle file = openLeaf(parent, leaf, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
    if (!file.isValid()) {
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }
    const auto openedFacts = factsOf(file.get());
    if (!openedFacts.has_value() || !openedFacts->regular ||
        openedFacts->identity != pathFacts->identity) {
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }

    core::Sha256Hasher hasher;
    constexpr std::size_t readBufferByteCount = 65'536;
    std::array<std::byte, readBufferByteCount> buffer{};
    std::uint64_t bytesRead = 0;
    while (true) {
        DWORD count = 0;
        if (::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                       nullptr) == 0) {
            return InspectionResult(StagedArtifactError::TargetInspectionFailed);
        }
        if (count == 0) {
            break;
        }
        if (count > byteLimit - bytesRead ||
            !hasher.update(std::span(buffer).first(static_cast<std::size_t>(count)))) {
            return InspectionResult(StagedArtifactError::ArtifactSizeLimit);
        }
        bytesRead += count;
    }

    const auto finalFacts = factsOf(file.get());
    if (!finalFacts.has_value() || !openedFacts->sameEvidence(*finalFacts) ||
        bytesRead != finalFacts->size) {
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }
    return InspectionResult(ArtifactTargetObservation::existing(
        fingerprintFrom(*finalFacts, bytesRead, hasher.finalize())));
}

[[nodiscard]] bool targetPolicyAccepts(const ArtifactOverwritePolicy policy,
                                       const ArtifactTargetObservation& observation) noexcept {
    switch (policy) {
    case ArtifactOverwritePolicy::CreateOnly:
        return !observation.exists;
    case ArtifactOverwritePolicy::ReplaceExisting:
        return observation.exists;
    case ArtifactOverwritePolicy::CreateOrReplace:
        return true;
    }
    return false;
}

[[nodiscard]] bool revalidateParent(const TargetData& target) noexcept {
    const auto pinned = factsOf(target.parentHandle.get());
    if (!pinned.has_value() || !pinned->directory || pinned->identity != target.parentIdentity) {
        return false;
    }
    const Handle current = openDirectory(target.canonicalParentPath);
    if (!current.isValid()) {
        return false;
    }
    const auto currentFacts = factsOf(current.get());
    return currentFacts.has_value() && currentFacts->directory && !currentFacts->reparse &&
           currentFacts->identity == target.parentIdentity;
}

[[nodiscard]] std::optional<std::wstring> makeStageName(const std::wstring& leaf) {
    constexpr std::size_t nonceByteCount = 16;
    std::array<unsigned char, nonceByteCount> nonce{};
    if (::BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return std::nullopt;
    }
    const auto leafUtf8 = std::filesystem::path(leaf).u8string();
    const auto prefix = stagedArtifactNamePrefix(
        static_cast<std::int64_t>(::GetCurrentProcessId()),
        std::string_view(reinterpret_cast<const char*>(leafUtf8.data()), leafUtf8.size()));
    constexpr std::array<char, 16> hexadecimal = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::wstring result;
    result.reserve(prefix.size() + (nonce.size() * 2));
    for (const char character : prefix) {
        result.push_back(static_cast<wchar_t>(character));
    }
    for (const auto byte : nonce) {
        result.push_back(static_cast<wchar_t>(hexadecimal[byte >> 4U]));
        result.push_back(static_cast<wchar_t>(hexadecimal[byte & 0x0FU]));
    }
    return result;
}

class WindowsTarget final : public StagedArtifactTargetState {
  public:
    explicit WindowsTarget(TargetData data) noexcept : data_(std::move(data)) {}
    ~WindowsTarget() override = default;

    [[nodiscard]] core::ArtifactTargetKey targetKey() const noexcept override {
        return data_.record->key;
    }
    [[nodiscard]] ArtifactTargetObservation observation() const noexcept override {
        return data_.expectedTarget;
    }

    [[nodiscard]] TargetData takeData() noexcept { return std::move(data_); }

    [[nodiscard]] const TargetData& dataForValidation() const noexcept { return data_; }

  private:
    TargetData data_;
};

class WindowsLease final : public StagedArtifactLeaseState {
  public:
    WindowsLease(TargetData target, Handle handle, std::wstring stageName,
                 const NativeFileIdentity stageIdentity) noexcept
        : target_(std::move(target)), handle_(std::move(handle)), stageName_(std::move(stageName)),
          stageIdentity_(stageIdentity) {}

    ~WindowsLease() override { cleanupStage(); }

    [[nodiscard]] core::ArtifactTargetKey targetKey() const noexcept override {
        return target_.record->key;
    }
    [[nodiscard]] std::uint64_t stageBytes() const noexcept override { return stageBytes_; }

    [[nodiscard]] StagedArtifactOperationResult
    write(const std::span<const std::byte> bytes) noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            return terminalOperationResult(StagedArtifactError::StageNotWritable);
        }
        if (phase_ != LeasePhase::Writable) {
            return {.error = StagedArtifactError::StageNotWritable, .stageBytes = stageBytes_};
        }
        if (bytes.size() > target_.shared->config.artifactByteLimit - stageBytes_) {
            return failOperation(StagedArtifactError::ArtifactSizeLimit);
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageWrite)) {
            return failOperation(StagedArtifactError::FaultInjected);
        }

        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - offset, std::numeric_limits<DWORD>::max() / 2));
            DWORD count = 0;
            if (::WriteFile(handle_.get(), bytes.data() + offset, chunk, &count, nullptr) == 0 ||
                count == 0) {
                return failOperation(StagedArtifactError::StageWriteFailed);
            }
            offset += count;
            stageBytes_ += count;
        }
        return {.stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactOperationResult finishWriting() noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            return terminalOperationResult(StagedArtifactError::StageNotWritable);
        }
        if (phase_ != LeasePhase::Writable) {
            return {.error = StagedArtifactError::StageNotWritable, .stageBytes = stageBytes_};
        }

        const bool injectedCloseFailure =
            target_.shared->shouldFail(StagedArtifactFaultPoint::StageWriterClose);
        const bool closeSucceeded = handle_.closeChecked();
        if (injectedCloseFailure) {
            return failOperation(StagedArtifactError::FaultInjected);
        }
        if (!closeSucceeded) {
            return failOperation(StagedArtifactError::StageWriterCloseFailed);
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageReopen)) {
            return failOperation(StagedArtifactError::FaultInjected);
        }

        // Read-write so the accepted stage can be flushed (FlushFileBuffers needs write access);
        // delete sharing lets the publication rename and a cleanup unlink proceed.
        Handle reopened =
            openLeaf(target_.canonicalParentPath, stageName_, GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING);
        if (!reopened.isValid()) {
            const auto code = ::GetLastError();
            const bool identityEvidence =
                code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
            return failOperation(identityEvidence ? StagedArtifactError::StageIdentityMismatch
                                                  : StagedArtifactError::StageReopenFailed);
        }
        handle_ = std::move(reopened);
        if (!revalidateStage()) {
            return failOperation(StagedArtifactError::StageIdentityMismatch);
        }
        phase_ = LeasePhase::Verifying;
        return {.stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactVerificationReadResult
    readForVerification(const std::uint64_t offset,
                        const std::span<std::byte> destination) noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            return terminalReadResult(StagedArtifactError::StageNotVerifying);
        }
        if (phase_ != LeasePhase::Verifying) {
            return {.error = StagedArtifactError::StageNotVerifying, .stageBytes = stageBytes_};
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageVerificationRead)) {
            return failRead(StagedArtifactError::FaultInjected);
        }
        if (offset >= stageBytes_) {
            return {.stageBytes = stageBytes_, .endOfFile = true};
        }
        constexpr auto maximumOffset =
            static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max());
        if (offset > maximumOffset) {
            return failRead(StagedArtifactError::StageVerificationReadOffsetOutOfRange);
        }

        const auto available = stageBytes_ - offset;
        const std::uint64_t requested = destination.size();
        const auto desired = std::min(available, requested);
        std::uint64_t total = 0;
        std::size_t destinationOffset = 0;
        while (total < desired) {
            const auto nativeOffset = offset + total;
            if (nativeOffset > maximumOffset) {
                return failRead(StagedArtifactError::StageVerificationReadOffsetOutOfRange, total);
            }
            const auto chunk = static_cast<DWORD>(
                std::min<std::uint64_t>(desired - total, std::numeric_limits<DWORD>::max() / 2));
            OVERLAPPED position{};
            position.Offset = static_cast<DWORD>(nativeOffset & 0xFFFFFFFFULL);
            position.OffsetHigh = static_cast<DWORD>(nativeOffset >> 32U);
            DWORD count = 0;
            if (::ReadFile(handle_.get(), destination.data() + destinationOffset, chunk, &count,
                           &position) == 0 ||
                count == 0) {
                return failRead(StagedArtifactError::StageVerificationReadFailed, total);
            }
            total += count;
            destinationOffset += count;
        }
        return {.bytesRead = total,
                .stageBytes = stageBytes_,
                .endOfFile = offset + total >= stageBytes_};
    }

    [[nodiscard]] StagedArtifactOperationResult acceptVerification() noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            return terminalOperationResult(StagedArtifactError::StageNotVerifying);
        }
        if (phase_ == LeasePhase::Accepted) {
            return {.stageBytes = stageBytes_};
        }
        if (phase_ != LeasePhase::Verifying) {
            return {.error = StagedArtifactError::StageNotVerifying, .stageBytes = stageBytes_};
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageVerificationAccept)) {
            return failOperation(StagedArtifactError::FaultInjected);
        }
        if (!revalidateStage()) {
            return failOperation(StagedArtifactError::StageIdentityMismatch);
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageFlush)) {
            return failOperation(StagedArtifactError::FaultInjected);
        }
        if (::FlushFileBuffers(handle_.get()) == 0) {
            return failOperation(StagedArtifactError::StageFlushFailed);
        }
        if (!revalidateStage()) {
            return failOperation(StagedArtifactError::StageIdentityMismatch);
        }
        phase_ = LeasePhase::Accepted;
        return {.stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactOperationResult rejectVerification() noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            if (terminalOperationError_ == StagedArtifactError::StageVerificationRejected) {
                return {.stageBytes = stageBytes_};
            }
            return terminalOperationResult(StagedArtifactError::StageNotVerifying);
        }
        if (phase_ != LeasePhase::Verifying) {
            return {.error = StagedArtifactError::StageNotVerifying, .stageBytes = stageBytes_};
        }
        terminalOperationError_ = StagedArtifactError::StageVerificationRejected;
        phase_ = LeasePhase::Terminal;
        publicationResult_ = {
            .outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
            .error = StagedArtifactError::StageVerificationRejected,
        };
        cleanupStage();
        return {.stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactPublicationResult
    publish(const PublicationDisposition disposition) noexcept override {
        if (publicationResult_.has_value()) {
            return *publicationResult_;
        }
        if (phase_ == LeasePhase::Terminal) {
            return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                    .error = terminalOperationError_ != StagedArtifactError::None
                                 ? terminalOperationError_
                                 : StagedArtifactError::StageVerificationNotAccepted};
        }
        if (disposition != PublicationDisposition::Proceed &&
            disposition != PublicationDisposition::Superseded &&
            disposition != PublicationDisposition::Cancelled) {
            return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                    .error = StagedArtifactError::InvalidPublicationDisposition};
        }
        if (disposition == PublicationDisposition::Superseded) {
            cleanupStage();
            return finishPublication({.outcome = StagedArtifactPublicationOutcome::Superseded});
        }
        if (disposition == PublicationDisposition::Cancelled) {
            cleanupStage();
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::CancelledBeforePublication});
        }
        if (phase_ != LeasePhase::Accepted) {
            return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                    .error = StagedArtifactError::StageVerificationNotAccepted};
        }

        std::lock_guard publicationLock(target_.record->publicationMutex);
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::IdentityRevalidation)) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = StagedArtifactError::FaultInjected});
        }
        if (!revalidateParent(target_)) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = StagedArtifactError::ParentIdentityMismatch});
        }
        if (!revalidateStage()) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = StagedArtifactError::StageIdentityMismatch});
        }

        const auto current = inspectTarget(target_.canonicalParentPath, target_.leaf,
                                           target_.shared->config.artifactByteLimit);
        if (current.error != StagedArtifactError::None) {
            const bool isConflictEvidence =
                current.error == StagedArtifactError::TargetLeafSymlink ||
                current.error == StagedArtifactError::TargetLeafNotRegular ||
                current.error == StagedArtifactError::ArtifactSizeLimit;
            return finishPublication(
                {.outcome = isConflictEvidence
                                ? StagedArtifactPublicationOutcome::ExternalModificationConflict
                                : StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = isConflictEvidence ? StagedArtifactError::ExternalModificationConflict
                                             : current.error});
        }
        if (current.observation != target_.expectedTarget) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::ExternalModificationConflict,
                 .error = StagedArtifactError::ExternalModificationConflict});
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::AtomicPublication)) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = StagedArtifactError::FaultInjected});
        }

        const auto renameResult = publishAtomically();
        if (renameResult != StagedArtifactError::None) {
            const auto outcome =
                renameResult == StagedArtifactError::ExternalModificationConflict
                    ? StagedArtifactPublicationOutcome::ExternalModificationConflict
                    : StagedArtifactPublicationOutcome::FailedBeforePublication;
            return finishPublication({.outcome = outcome, .error = renameResult});
        }

        ownsStageName_ = false;
        // MOVEFILE_WRITE_THROUGH already carried the rename to disk; the injected fault models a
        // directory-flush failure on the platforms that have one.
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::ParentDurability)) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning,
                 .error = StagedArtifactError::FaultInjected});
        }
        return finishPublication({.outcome = StagedArtifactPublicationOutcome::Published});
    }

  private:
    enum class LeasePhase : std::uint8_t {
        Writable,
        Verifying,
        Accepted,
        Terminal,
    };

    [[nodiscard]] StagedArtifactOperationResult
    failOperation(const StagedArtifactError error) noexcept {
        phase_ = LeasePhase::Terminal;
        terminalOperationError_ = error;
        publicationResult_ = {
            .outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
            .error = error,
        };
        cleanupStage();
        return {.error = error, .stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactOperationResult
    terminalOperationResult(const StagedArtifactError fallback) const noexcept {
        return {.error = terminalOperationError_ != StagedArtifactError::None
                             ? terminalOperationError_
                             : fallback,
                .stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactVerificationReadResult
    failRead(const StagedArtifactError error, const std::uint64_t bytesRead = 0) noexcept {
        const auto failed = failOperation(error);
        return {.error = failed.error, .bytesRead = bytesRead, .stageBytes = failed.stageBytes};
    }

    [[nodiscard]] StagedArtifactVerificationReadResult
    terminalReadResult(const StagedArtifactError fallback) const noexcept {
        return {.error = terminalOperationError_ != StagedArtifactError::None
                             ? terminalOperationError_
                             : fallback,
                .stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactPublicationResult
    finishPublication(const StagedArtifactPublicationResult result) noexcept {
        phase_ = LeasePhase::Terminal;
        publicationResult_ = result;
        if (!result.targetWasPublished()) {
            cleanupStage();
        } else {
            handle_.reset();
        }
        return result;
    }

    [[nodiscard]] std::filesystem::path stagePath() const {
        return target_.canonicalParentPath / stageName_;
    }

    [[nodiscard]] bool revalidateStage() const noexcept {
        const auto handleFacts = factsOf(handle_.get());
        if (!handleFacts.has_value() || !handleFacts->regular || handleFacts->linkCount != 1 ||
            handleFacts->size != stageBytes_ || handleFacts->identity != stageIdentity_) {
            return false;
        }
        const Handle probe =
            openLeaf(target_.canonicalParentPath, stageName_, FILE_READ_ATTRIBUTES,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
        if (!probe.isValid()) {
            return false;
        }
        const auto pathFacts = factsOf(probe.get());
        return pathFacts.has_value() && pathFacts->regular && pathFacts->linkCount == 1 &&
               pathFacts->size == stageBytes_ && pathFacts->identity == stageIdentity_;
    }

    [[nodiscard]] StagedArtifactError publishAtomically() noexcept {
        try {
            // Closing our handle first keeps the rename independent of share modes.
            handle_.reset();
            const auto source = stagePath();
            const auto destination = target_.canonicalParentPath / target_.leaf;
            if (!target_.expectedTarget.exists) {
                // No MOVEFILE_REPLACE_EXISTING: an entry that appeared since inspection fails the
                // move instead of being replaced.
                if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) !=
                    0) {
                    return StagedArtifactError::None;
                }
                const auto code = ::GetLastError();
                if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
                    return StagedArtifactError::ExternalModificationConflict;
                }
                return StagedArtifactError::AtomicPublicationFailed;
            }
            if (::MoveFileExW(source.c_str(), destination.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
                return StagedArtifactError::AtomicPublicationFailed;
            }
            return StagedArtifactError::None;
        } catch (...) {
            return StagedArtifactError::AtomicPublicationFailed;
        }
    }

    void cleanupStage() noexcept {
        handle_.reset();
        if (!ownsStageName_) {
            return;
        }
        try {
            bool present = false;
            bool deletable = false;
            bool unexpectedLinks = false;
            {
                const Handle probe =
                    openLeaf(target_.canonicalParentPath, stageName_, FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
                if (probe.isValid()) {
                    present = true;
                    const auto facts = factsOf(probe.get());
                    deletable =
                        facts.has_value() && facts->regular && facts->identity == stageIdentity_;
                    unexpectedLinks = facts.has_value() && facts->linkCount != 1;
                } else {
                    const auto code = ::GetLastError();
                    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                        target_.shared->recordCleanupFailure();
                    }
                }
            }
            if (present && !deletable) {
                target_.shared->recordCleanupFailure();
            } else if (present) {
                const auto path = stagePath();
                if (::DeleteFileW(path.c_str()) == 0 || unexpectedLinks) {
                    target_.shared->recordCleanupFailure();
                }
            }
        } catch (...) {
            target_.shared->recordCleanupFailure();
        }
        ownsStageName_ = false;
    }

    TargetData target_;
    Handle handle_;
    std::wstring stageName_;
    NativeFileIdentity stageIdentity_;
    std::uint64_t stageBytes_ = 0;
    LeasePhase phase_ = LeasePhase::Writable;
    bool ownsStageName_ = true;
    StagedArtifactError terminalOperationError_ = StagedArtifactError::None;
    std::optional<StagedArtifactPublicationResult> publicationResult_;
};

class WindowsCoordinator final : public StagedArtifactCoordinatorState {
  public:
    explicit WindowsCoordinator(std::shared_ptr<WindowsSharedState> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] PlatformPreflightResult
    preflight(const StagedArtifactPreflightRequest& request) noexcept override {
        try {
            return preflightChecked(request);
        } catch (const std::bad_alloc&) {
            return PlatformPreflightResult(StagedArtifactError::ResourceUnavailable);
        } catch (...) {
            return PlatformPreflightResult(StagedArtifactError::ParentResolutionFailed);
        }
    }

    [[nodiscard]] PlatformStageResult
    stage(std::unique_ptr<StagedArtifactTargetState> target) noexcept override {
        auto* windowsTarget = dynamic_cast<WindowsTarget*>(target.get());
        if (windowsTarget == nullptr) {
            return PlatformStageResult(StagedArtifactError::InvalidTargetPath);
        }
        if (windowsTarget->dataForValidation().shared != state_) {
            return PlatformStageResult(StagedArtifactError::CoordinatorMismatch);
        }
        if (state_->shouldFail(StagedArtifactFaultPoint::StageCreation)) {
            return PlatformStageResult(StagedArtifactError::FaultInjected);
        }
        if (!revalidateParent(windowsTarget->dataForValidation())) {
            return PlatformStageResult(StagedArtifactError::ParentIdentityMismatch);
        }

        try {
            auto data = windowsTarget->takeData();
            target.reset();
            constexpr std::size_t maximumAttempts = 32;
            for (std::size_t attempt = 0; attempt < maximumAttempts; ++attempt) {
                auto stageName = makeStageName(data.leaf);
                if (!stageName.has_value()) {
                    return PlatformStageResult(StagedArtifactError::StageCreationFailed);
                }
                // Read sharing only: nothing else may write the stage while it is being filled;
                // delete sharing lets the cleanup path remove it.
                Handle handle =
                    openLeaf(data.canonicalParentPath, *stageName, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, CREATE_NEW);
                if (!handle.isValid()) {
                    if (::GetLastError() == ERROR_FILE_EXISTS ||
                        ::GetLastError() == ERROR_ALREADY_EXISTS) {
                        continue;
                    }
                    return PlatformStageResult(StagedArtifactError::StageCreationFailed);
                }
                const auto facts = factsOf(handle.get());
                if (!facts.has_value() || !facts->regular || facts->linkCount != 1) {
                    handle.reset();
                    const auto path = data.canonicalParentPath / *stageName;
                    ::DeleteFileW(path.c_str());
                    return PlatformStageResult(StagedArtifactError::StageCreationFailed);
                }
                auto lease = std::make_unique<WindowsLease>(std::move(data), std::move(handle),
                                                            std::move(*stageName), facts->identity);
                return PlatformStageResult(std::move(lease));
            }
            return PlatformStageResult(StagedArtifactError::StageCreationFailed);
        } catch (const std::bad_alloc&) {
            return PlatformStageResult(StagedArtifactError::ResourceUnavailable);
        } catch (...) {
            return PlatformStageResult(StagedArtifactError::StageCreationFailed);
        }
    }

    [[nodiscard]] StagedArtifactCoordinatorSnapshot snapshot() const noexcept override {
        std::lock_guard lock(state_->mutex);
        return {.activeTargetCount = state_->activeTargetCount,
                .activeTargetLimit = state_->config.activeTargetLimit,
                .targetRecordCount = state_->records.size(),
                .targetRecordLimit = state_->config.targetRecordLimit,
                .cleanupFailureCount = state_->cleanupFailureCount,
                .lastIssuedTargetKey =
                    core::ArtifactTargetKey::fromRaw(state_->lastIssuedTargetKey),
                .targetIdentityExhausted = state_->targetIdentityExhausted};
    }

  private:
    [[nodiscard]] PlatformPreflightResult
    preflightChecked(const StagedArtifactPreflightRequest& request) {
        const auto leafPath = request.targetPath.filename();
        auto leaf = leafPath.native();
        if (leaf.empty() || leaf == L"." || leaf == L".." ||
            leaf.find(L'\0') != std::wstring::npos) {
            return PlatformPreflightResult(StagedArtifactError::InvalidTargetPath);
        }
        auto parentPath = request.targetPath.parent_path();
        if (parentPath.empty()) {
            parentPath = ".";
        }
        std::error_code pathError;
        auto canonicalParent = std::filesystem::canonical(parentPath, pathError);
        if (pathError) {
            return PlatformPreflightResult(StagedArtifactError::ParentResolutionFailed);
        }

        Handle parentHandle = openDirectory(canonicalParent);
        if (!parentHandle.isValid()) {
            return PlatformPreflightResult(::GetLastError() == ERROR_DIRECTORY
                                               ? StagedArtifactError::ParentNotDirectory
                                               : StagedArtifactError::ParentResolutionFailed);
        }
        const auto parentFacts = factsOf(parentHandle.get());
        if (!parentFacts.has_value() || !parentFacts->directory) {
            return PlatformPreflightResult(StagedArtifactError::ParentNotDirectory);
        }

        if (state_->shouldFail(StagedArtifactFaultPoint::TargetInspection)) {
            return PlatformPreflightResult(StagedArtifactError::FaultInjected);
        }
        const auto inspection =
            inspectTarget(canonicalParent, leaf, state_->config.artifactByteLimit);
        if (inspection.error != StagedArtifactError::None) {
            return PlatformPreflightResult(inspection.error);
        }
        if (request.expectedTarget.has_value() &&
            *request.expectedTarget != inspection.observation) {
            return PlatformPreflightResult(StagedArtifactError::ExternalModificationConflict);
        }
        if (!targetPolicyAccepts(request.overwritePolicy, inspection.observation)) {
            return PlatformPreflightResult(StagedArtifactError::OverwritePolicyConflict);
        }

        const TargetIdentity identity{parentFacts->identity, leaf};
        std::shared_ptr<TargetRecord> record;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->activeTargetCount >= state_->config.activeTargetLimit) {
                return PlatformPreflightResult(StagedArtifactError::AdmissionLimit);
            }
            const auto found = state_->records.find(identity);
            if (found != state_->records.end()) {
                record = found->second;
            } else {
                if (state_->records.size() >= state_->config.targetRecordLimit) {
                    return PlatformPreflightResult(StagedArtifactError::TargetRecordLimit);
                }
                if (state_->targetIdentityExhausted) {
                    return PlatformPreflightResult(StagedArtifactError::TargetIdentityExhausted);
                }
                const auto rawKey = state_->lastIssuedTargetKey + 1;
                if (rawKey == 0) {
                    state_->targetIdentityExhausted = true;
                    return PlatformPreflightResult(StagedArtifactError::TargetIdentityExhausted);
                }
                record = std::make_shared<TargetRecord>(core::ArtifactTargetKey::fromRaw(rawKey));
                state_->records.emplace(identity, record);
                state_->lastIssuedTargetKey = rawKey;
                if (rawKey == std::numeric_limits<std::uint64_t>::max()) {
                    state_->targetIdentityExhausted = true;
                }
            }
            ++state_->activeTargetCount;
        }

        TargetData data{state_,
                        AdmissionToken(state_),
                        std::move(record),
                        std::move(parentHandle),
                        identity.parent,
                        std::move(canonicalParent),
                        leaf,
                        inspection.observation,
                        request.overwritePolicy};
        return PlatformPreflightResult(std::make_unique<WindowsTarget>(std::move(data)));
    }

    std::shared_ptr<WindowsSharedState> state_;
};

} // namespace

PlatformCoordinatorResult
createPlatformStagedArtifactCoordinator(const StagedArtifactConfig& config) noexcept {
    if (!config.isValid()) {
        return PlatformCoordinatorResult(StagedArtifactError::InvalidConfiguration);
    }
    try {
        auto state = std::make_shared<WindowsSharedState>(config);
        return PlatformCoordinatorResult(std::make_unique<WindowsCoordinator>(std::move(state)));
    } catch (const std::bad_alloc&) {
        return PlatformCoordinatorResult(StagedArtifactError::ResourceUnavailable);
    } catch (...) {
        return PlatformCoordinatorResult(StagedArtifactError::ResourceUnavailable);
    }
}

} // namespace bloom::platform::detail
