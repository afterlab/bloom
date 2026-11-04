// Verify footage (#495): the background pass that hashes stills and unverified sequence members
// once at idle priority. The work list skips what is already proven, progress reaches the total,
// cancellation stops between and within files, a changed file is reported with the relink
// suggestion, and the Assets controller runs the pass as a "Verify footage" job that purges the
// media caches (footageChanged) when bytes changed since import.
#include "footage_verification.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <bloom/core/file_uri.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {
using namespace bloom;
namespace fs = std::filesystem;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}
template <typename Predicate> bool waitUntil(Predicate predicate, const int timeoutMs = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(5);
    return predicate();
}

void writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
core::Sha256Digest digestOf(const std::string& bytes) {
    return *core::Sha256Hasher::hash(std::as_bytes(std::span(bytes)));
}
document::AssetFileStamp stampOf(const fs::path& path) {
    const auto identity = platform::observeFileIdentity(path);
    if (!identity)
        throw std::logic_error("stamp fixture");
    return {identity->size, identity->modifiedNanoseconds};
}
document::AssetLocator locatorOf(const fs::path& directory, const std::string& name) {
    document::AssetLocator locator;
    locator.path = name;
    locator.relinkHint = *core::fileUriFromAbsolutePath(directory / name);
    return locator;
}

// A still, a video and a six-member sequence: pending, legacy, verified (still stamped), verified
// whose file was touched, pending whose file left its stamp, and pending already in the memo.
document::Project fixture(const fs::path& directory) {
    document::Project project(document::ProjectId::fromRaw(1), "Verify");
    document::AssetRecord still;
    still.id = document::AssetId::fromRaw(10);
    still.kind = document::AssetKind::Image;
    still.name = "still";
    still.locator = locatorOf(directory, "still.bin");
    writeFile(directory / "still.bin", "still bytes");
    still.contentDigest = digestOf("still bytes");
    still.width = still.height = 8;
    if (!project.addAsset(still))
        throw std::logic_error("still fixture");

    document::AssetRecord video;
    video.id = document::AssetId::fromRaw(11);
    video.kind = document::AssetKind::Video;
    video.name = "clip";
    video.locator = locatorOf(directory, "clip.bin");
    writeFile(directory / "clip.bin", "video bytes");
    video.contentDigest = digestOf("other video bytes");
    video.width = video.height = 8;
    static_cast<void>(project.addAsset(video)); // listed or not, a video is never hashed

    document::AssetRecord sequence;
    sequence.id = document::AssetId::fromRaw(12);
    sequence.kind = document::AssetKind::Sequence;
    sequence.name = "plate";
    sequence.width = sequence.height = 8;
    sequence.manifest.pattern = "plate.####.bin";
    sequence.manifest.padding = 4;
    sequence.manifest.first = 1;
    sequence.manifest.last = 6;
    for (int frame = 1; frame <= 6; ++frame) {
        const auto name = "plate.000" + std::to_string(frame) + ".bin";
        const auto bytes = "plate frame " + std::to_string(frame);
        writeFile(directory / name, bytes);
        document::AssetSequenceMember member;
        member.frame = frame;
        member.locator = locatorOf(directory, name);
        if (frame != 2)
            member.stamp = stampOf(directory / name);
        if (frame == 2 || frame == 3 || frame == 4)
            member.contentDigest = digestOf(bytes);
        if (frame == 4 || frame == 5)
            member.stamp->modifiedNanoseconds -= 1'000'000'000;
        if (frame == 6 &&
            platform::hashSourceFile(directory / name).error != platform::SourceHashError::None)
            throw std::logic_error("memo fixture");
        sequence.manifest.members.push_back(std::move(member));
    }
    sequence.locator = sequence.manifest.members.front().locator;
    if (!sequence.validate().ok() || !project.addAsset(sequence))
        throw std::logic_error("sequence fixture");
    return project;
}

void workListSkipsWhatIsProven(const fs::path& directory) {
    const auto project = fixture(directory);
    const auto work = ui::footageVerificationWork(project, directory);
    std::vector<std::string> names;
    for (const auto& item : work)
        names.push_back(item.path.filename().string());
    expect((names == std::vector<std::string>{"still.bin", "plate.0001.bin", "plate.0002.bin",
                                              "plate.0004.bin"}),
           "the work list holds the still, the pending and legacy members and the touched "
           "verified member, and skips video, a stamped verified member, a pending member that "
           "left its stamp and a file the memo knows");
    expect(work.size() == 4 && work[0].recorded && !work[1].recorded && work[1].frame == 1 &&
               work[2].recorded && work[3].recorded,
           "a pending member carries no recorded digest; the others carry theirs");
}

void passHashesReportsAndRecords(const fs::path& directory) {
    const auto project = fixture(directory);
    // The still's bytes change after import: same length, different content.
    writeFile(directory / "still.bin", "STILL bytes");
    const auto work = ui::footageVerificationWork(project, directory);
    std::size_t lastCompleted = 0;
    std::size_t lastTotal = 0;
    const auto report = ui::verifyFootageFiles(
        work, {}, [&](const std::size_t completed, const std::size_t total, const fs::path&) {
            lastCompleted = completed;
            lastTotal = total;
        });
    expect(report.files == work.size() && report.hashed == work.size() && !report.cancelled,
           "every listed file is hashed");
    expect(lastCompleted == work.size() && lastTotal == work.size(),
           "progress reaches the total number of files");
    expect(report.changed == 1 && report.diagnostics.size() == 1 &&
               report.diagnostics.front().severity == runtime::DiagnosticSeverity::Warning &&
               report.diagnostics.front().code == "bloom.media.footage-changed" &&
               report.diagnostics.front().summary == "still changed since it was imported" &&
               report.diagnostics.front().suggestedAction.find("Relink") != std::string::npos,
           "a changed still is reported as a warning naming the asset, with the relink advice");
    expect(platform::observeSource(directory / "still.bin").digest == digestOf("STILL bytes"),
           "the memo now holds the changed file's actual digest");
    expect(platform::observeSource(directory / "plate.0001.bin").digest ==
               digestOf("plate frame 1"),
           "a pending member's digest is now in the memo for the next save to record");
    expect(ui::footageVerificationWork(project, directory).empty(),
           "a second pass finds nothing left to read");
}

void cancellationStopsEarly(const fs::path& directory) {
    const auto project = fixture(directory);
    const auto work = ui::footageVerificationWork(project, directory);
    bool stop = false;
    const auto report = ui::verifyFootageFiles(
        work, [&] { return stop; },
        [&](const std::size_t completed, std::size_t, const fs::path&) { stop = completed >= 1; });
    expect(report.cancelled && report.hashed == 1 && report.hashed < work.size(),
           "cancellation stops the pass before the remaining files");
}

struct Harness final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler};
    ui::AssetController controller{session, host, scheduler, bridge};
    ~Harness() {
        waitUntil([this] { return !controller.busy() && !controller.verifyingFootage(); });
    }
};

void controllerRunsTheJob(const QTemporaryDir& temporary) {
    Harness harness;
    const QString path = temporary.filePath(QStringLiteral("job.png"));
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    expect(image.save(path), "write the PNG fixture");
    QSignalSpy changed(&harness.controller, &ui::AssetController::footageChanged);
    harness.controller.importFiles({path});
    expect(waitUntil([&] { return !harness.controller.busy(); }), "the import finishes");
    expect(waitUntil([&] { return !harness.controller.verifyingFootage(); }),
           "the verification pass after the import finishes");
    expect(harness.controller.lastFootageVerification().has_value(),
           "a verification pass ran after the import");
    // Different pixels: the bytes no longer match the digest import recorded.
    image.fill(Qt::black);
    expect(image.save(path), "rewrite the PNG fixture");
    harness.controller.verifyFootage();
    expect(waitUntil([&] { return !harness.controller.verifyingFootage(); }),
           "the requested pass finishes");
    const auto& summary = harness.controller.lastFootageVerification();
    expect(summary && summary->files == 1 && summary->hashed == 1 && summary->changed == 1,
           "the pass hashes the changed still and counts it changed");
    expect(changed.count() == 1, "changed bytes ask for the media cache purge");
    bool found = false;
    for (const auto& task : harness.scheduler.snapshots()) {
        if (task.name != "Verify footage" || task.state != runtime::TaskState::Succeeded ||
            task.diagnostics.empty())
            continue;
        found = task.owner.kind == runtime::TaskOwnerKind::Project &&
                task.priority == runtime::TaskPriority::Background &&
                task.executor == runtime::TaskExecutor::BlockingIo &&
                task.progress.phase == "Verifying footage" && task.progress.total == 1 &&
                task.progress.completed == 1 &&
                task.diagnostics.front().code == "bloom.media.footage-changed";
    }
    expect(found, "Jobs shows a background Verify footage job at 1 of 1 with the warning");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    try {
        const fs::path root = temporary.path().toStdString();
        for (const auto* name : {"list", "pass", "cancel"})
            fs::create_directories(root / name);
        workListSkipsWhatIsProven(root / "list");
        passHashesReportsAndRecords(root / "pass");
        cancellationStopsEarly(root / "cancel");
        controllerRunsTheJob(temporary);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
