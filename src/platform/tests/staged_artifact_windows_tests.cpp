// Windows provider checks for the staged-artifact coordinator: the publication state machine over
// Win32 handles, non-ASCII names, replace and create-only policies, external-modification
// detection and stage cleanup. The POSIX suite (staged_artifact_tests.cpp) covers the same
// contract with descriptor-level fixtures.
#include <bloom/platform/staged_artifact.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using namespace bloom::platform;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

[[nodiscard]] std::size_t entryCount(const std::filesystem::path& directory) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(directory),
                                                  std::filesystem::directory_iterator()));
}

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<wchar_t, MAX_PATH + 1> buffer{};
        const auto length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        path_ = std::filesystem::path(std::wstring(buffer.data(), length)) /
                (L"bloom-staged-wé-" + std::to_wstring(::GetCurrentProcessId()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

// Runs one lease from stage to publication and returns the publication outcome.
[[nodiscard]] StagedArtifactPublicationResult
publishBytes(StagedArtifactCoordinator& coordinator, const std::filesystem::path& target,
             const ArtifactOverwritePolicy policy, const std::string_view contents,
             const PublicationDisposition disposition = PublicationDisposition::Proceed) {
    auto preflight = coordinator.preflight({.targetPath = target, .overwritePolicy = policy});
    if (!preflight) {
        return {.error = preflight.error()};
    }
    auto staged = coordinator.stage(std::move(preflight).takeTarget());
    if (!staged) {
        return {.error = staged.error()};
    }
    auto lease = std::move(staged).takeLease();
    if (!lease.write(bytes(contents)) || !lease.finishWriting()) {
        return {.error = StagedArtifactError::StageWriteFailed};
    }
    std::vector<std::byte> readBack(contents.size());
    const auto read = lease.readForVerification(0, readBack);
    expect(read && read.bytesRead == contents.size() && read.endOfFile,
           "verification reads the staged bytes back");
    expect(std::equal(readBack.begin(), readBack.end(), bytes(contents).begin()),
           "the staged bytes match what was written");
    if (!lease.acceptVerification()) {
        return {.error = StagedArtifactError::StageVerificationNotAccepted};
    }
    return lease.publish(disposition);
}

void testLifecycle(StagedArtifactCoordinator& coordinator, const std::filesystem::path& root) {
    const auto target = root / L"clip é中.bin";
    auto created = publishBytes(coordinator, target, ArtifactOverwritePolicy::CreateOnly, "first");
    expect(created.outcome == StagedArtifactPublicationOutcome::Published &&
               readFile(target) == "first" && entryCount(root) == 1,
           "a create-only publication creates a non-ASCII target and leaves no stage");

    auto refused = publishBytes(coordinator, target, ArtifactOverwritePolicy::CreateOnly, "second");
    expect(refused.error == StagedArtifactError::OverwritePolicyConflict &&
               readFile(target) == "first",
           "create-only refuses an existing target");

    auto replaced =
        publishBytes(coordinator, target, ArtifactOverwritePolicy::ReplaceExisting, "second");
    expect(replaced.outcome == StagedArtifactPublicationOutcome::Published &&
               readFile(target) == "second" && entryCount(root) == 1,
           "replace swaps the published bytes in place");

    auto cancelled = publishBytes(coordinator, target, ArtifactOverwritePolicy::CreateOrReplace,
                                  "third", PublicationDisposition::Cancelled);
    expect(cancelled.outcome == StagedArtifactPublicationOutcome::CancelledBeforePublication &&
               readFile(target) == "second" && entryCount(root) == 1,
           "a cancelled publication keeps the target and removes its stage");
    const auto superseded =
        publishBytes(coordinator, target, ArtifactOverwritePolicy::CreateOrReplace, "third",
                     PublicationDisposition::Superseded);
    expect(superseded.outcome == StagedArtifactPublicationOutcome::Superseded &&
               readFile(target) == "second" && entryCount(root) == 1,
           "a superseded publication keeps the target and removes its stage");
}

void testExternalModification(StagedArtifactCoordinator& coordinator,
                              const std::filesystem::path& root) {
    const auto target = root / L"watched.bin";
    writeFile(target, "original");
    auto preflight = coordinator.preflight(
        {.targetPath = target, .overwritePolicy = ArtifactOverwritePolicy::ReplaceExisting});
    expect(preflight.succeeded(), "preflight fingerprints an existing target");
    auto staged = coordinator.stage(std::move(preflight).takeTarget());
    auto lease = std::move(staged).takeLease();
    expect(lease.write(bytes("ours")) && lease.finishWriting() && lease.acceptVerification(),
           "the lease is accepted");
    writeFile(target, "someone else");
    const auto result = lease.publish(PublicationDisposition::Proceed);
    expect(result.outcome == StagedArtifactPublicationOutcome::ExternalModificationConflict &&
               readFile(target) == "someone else" && entryCount(root) == 2,
           "a target changed after preflight is a conflict and keeps the foreign bytes");
}

void testStageCleanupOnDrop(StagedArtifactCoordinator& coordinator,
                            const std::filesystem::path& root) {
    const auto before = entryCount(root);
    {
        auto preflight = coordinator.preflight({.targetPath = root / L"dropped.bin"});
        auto staged = coordinator.stage(std::move(preflight).takeTarget());
        auto lease = std::move(staged).takeLease();
        expect(static_cast<bool>(lease.write(bytes("abandoned"))), "a stage accepts bytes");
        expect(entryCount(root) == before + 1, "an active lease owns a hidden stage file");
    }
    expect(entryCount(root) == before && coordinator.snapshot().cleanupFailureCount == 0,
           "dropping a lease removes its stage without a cleanup failure");
}

void testReparsePointTargetRefused(StagedArtifactCoordinator& coordinator,
                                   const std::filesystem::path& root) {
    const auto real = root / L"real.bin";
    writeFile(real, "real");
    const auto link = root / L"link.bin";
    std::error_code error;
    std::filesystem::create_symlink(real, link, error);
    if (error) {
        std::cout << "SKIPPED symbolic-link check: creating a symlink needs a privilege ("
                  << error.message() << ")\n";
        return;
    }
    const auto preflight = coordinator.preflight({.targetPath = link});
    expect(!preflight && preflight.error() == StagedArtifactError::TargetLeafSymlink,
           "a symbolic-link target leaf is rejected without traversal");
}

void testByteLimit(const std::filesystem::path& root) {
    StagedArtifactConfig config;
    config.artifactByteLimit = 4;
    auto created = StagedArtifactCoordinator::create(config);
    expect(created.succeeded(), "a limited coordinator is created");
    if (!created) {
        return;
    }
    auto coordinator = std::move(created).takeCoordinator();
    auto preflight = coordinator.preflight({.targetPath = root / L"limited.bin"});
    auto staged = coordinator.stage(std::move(preflight).takeTarget());
    auto lease = std::move(staged).takeLease();
    const auto written = lease.write(bytes("too many bytes"));
    expect(!written && written.error == StagedArtifactError::ArtifactSizeLimit,
           "a write past the artifact limit fails");
}

} // namespace

int main() {
    TempDirectory directory;
    auto created = StagedArtifactCoordinator::create({});
    expect(created.succeeded(), "the Windows coordinator is created");
    if (!created) {
        return 1;
    }
    auto coordinator = std::move(created).takeCoordinator();
    testLifecycle(coordinator, directory.path());
    testExternalModification(coordinator, directory.path());
    testStageCleanupOnDrop(coordinator, directory.path());
    testReparsePointTargetRefused(coordinator, directory.path());
    testByteLimit(directory.path());
    return failures == 0 ? 0 : 1;
}
