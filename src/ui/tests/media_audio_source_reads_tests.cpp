// #732: the decode sessions of a movie with audio read its source through the local source store.
// The Assets thumbnail pass (the verify and thumbnail session, then the audio session), the audio
// analysis and the export audio preflight each read one ProRes movie with sound; the export's
// audio decoder reads a sound file, which has no picture session beside it to share a copy with.
// The first session fills the store; a second session with empty caches (new controllers and
// evaluators, the same store folder) reads no source bytes at all and is served from the local
// copy by range. A site that is not wired opens its session without the store and captures the
// whole source itself, which the whole-read count shows.
// NotRun without the media worker or the movie fixture.

#include "settled_file.hpp"
#include "await_qt.hpp"
#include "fixed_memory_budget.hpp"

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/media/cache/source_mirror.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/output/export_audio_sources.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/platform/source_store.hpp>
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

int failures = 0;
void expect(const bool ok, const std::string& message) {
    if (ok)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

// What the store and the process have read so far: the store's reads of the source itself, the
// bytes of them, its local range hits, and the whole-source captures sessions made without it.
struct Work final {
    std::uint64_t reads = 0, readBytes = 0, rangeHits = 0, wholeReads = 0;
};
Work measure(const media::cache::SourceMirror& mirror) {
    const auto statistics = mirror.statistics();
    return {statistics.networkReads + statistics.otherSourceReads,
            statistics.networkReadBytes + statistics.otherSourceReadBytes, statistics.rangeHits,
            platform::sourceReadWork().fullReads};
}

// One site's work in one pass. The first pass fills the store: the site must have used it. The
// second pass: the site reads no source bytes, makes no whole-source read of its own, and is
// served from the local copy.
void check(const std::string& pass, const std::string& site, const Work& before, const Work& after,
           const bool first) {
    const auto reads = after.reads - before.reads;
    const auto bytes = after.readBytes - before.readBytes;
    const auto hits = after.rangeHits - before.rangeHits;
    const auto whole = after.wholeReads - before.wholeReads;
    const auto detail = pass + " " + site + " (reads " + std::to_string(reads) + ", bytes " +
                        std::to_string(bytes) + ", range hits " + std::to_string(hits) +
                        ", whole reads " + std::to_string(whole) + ")";
    if (first) {
        expect(reads + hits > 0, detail + ": the site reads through the source store");
        return;
    }
    expect(reads == 0 && bytes == 0, detail + ": the site reads no source bytes");
    expect(hits > 0, detail + ": the site is served from the local copy");
    expect(whole == 0, detail + ": the site captures no whole source of its own");
}

struct UiRig final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler};
    std::unique_ptr<ui::AssetController> controller;
    UiRig(media::cache::SourceMirror& mirror, const std::filesystem::path& movie) {
        if (session.composition() == nullptr) {
            commands::Transaction seed("Seed", session.snapshot().revision());
            seed.emplace<commands::AddComposition>("Main", document::CompositionFormat{},
                                                   core::RationalTime::fromInteger(10));
            const auto seeded = session.executeTransaction(std::move(seed));
            const auto id = seeded.succeeded() ? seeded.outputId<document::CompositionId>(
                                                     commands::kAddCompositionOutput)
                                               : std::nullopt;
            if (!id || !session.setComposition(*id))
                throw std::runtime_error("composition seed");
        }
        // The import's own probe reads the source directly (it is not a site of this test) and
        // leaves a retained copy of it in memory; the sessions under test start without it.
        commands::Transaction import("Import", session.snapshot().revision());
        import.emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{movie},
                                               movie.parent_path());
        if (!session.executeTransaction(std::move(import)).succeeded())
            throw std::runtime_error("the movie does not import");
        media::video::releaseRetainedSourceSnapshots();
        controller = std::make_unique<ui::AssetController>(
            session, host, scheduler, bridge, &mirror, nullptr,
            ui::AudioProcessingAdmission{.policy = nullptr,
                                         .standaloneByteAllowance = 256U * 1024U * 1024U,
                                         // Decoded sound is not kept after its pass, so a later
                                         // audio pass decodes it again from its source.
                                         .cacheByteCapacity = 1});
    }
    ~UiRig() {
        controller->cancel();
        bridge.beginShutdown();
        scheduler.beginShutdown();
        expect(bloom::test_support::awaitQtCondition("the UI scheduler is quiet",
                                                     [&] { return scheduler.isQuiescent(); }),
               "the UI scheduler is quiet");
    }
    UiRig(const UiRig&) = delete;
    UiRig& operator=(const UiRig&) = delete;
};

void assetsPass(const std::string& label, const std::filesystem::path& movie,
                media::cache::SourceMirror& mirror, const bool first) {
    // Assets: the thumbnail pass of an imported movie decodes its picture (the verify and
    // thumbnail session) and its sound (the audio session). The import's own probe stays direct
    // (it is not a site of this test); the pass is judged from its end: the controller's reads
    // are the store's, and its whole-source captures are those of the import alone, which the
    // second pass's import does not repeat once the source is known.
    {
        UiRig rig(mirror, movie);
        const auto& assets = rig.session.snapshot().project().assets();
        expect(assets.size() == 1, label + ": one asset");
        if (assets.size() == 1) {
            const auto id = assets.front().id;
            const auto before = measure(mirror);
            rig.controller->verifyFootage();
            expect(bloom::test_support::awaitQtCondition(
                       "the movie has a thumbnail and its sound",
                       [&] {
                           return !rig.controller->thumbnail(id).isNull() &&
                                  rig.controller->waveform(id) != nullptr &&
                                  !rig.controller->thumbnailsPending() &&
                                  !rig.controller->verifyingFootage();
                       },
                       60s),
                   label + ": the movie has a thumbnail and a waveform: " +
                       rig.controller->audioDiagnostic(id).toStdString());
            check(label, "assets thumbnail and audio sessions", before, measure(mirror), first);
            // The thumbnail is now cached, so a new audio pass opens only the audio session; the
            // retained copy of the source is gone, so that session meets the store alone.
            media::video::releaseRetainedSourceSnapshots();
            rig.controller->releaseAudioDelivery();
            const auto audioBefore = measure(mirror);
            const auto jobs = rig.controller->thumbnailJobsStarted();
            rig.controller->requestAudioDelivery();
            expect(bloom::test_support::awaitQtCondition(
                       "the audio pass finishes",
                       [&] {
                           return rig.controller->thumbnailJobsStarted() > jobs &&
                                  !rig.controller->thumbnailsPending() &&
                                  !rig.controller->busy() && rig.scheduler.isQuiescent();
                       },
                       60s),
                   label + ": the audio pass finishes");
            check(label, "assets audio session", audioBefore, measure(mirror), first);
        }
    }
    mirror.flush();
}

void enginePass(const std::string& label, const std::filesystem::path& movie,
                media::cache::SourceMirror& mirror, const bool first) {
    // The runtime and the output sites, on a project of their own.
    auto created = scripting::Session::createNew("Audio", "Main", core::RationalTime::fromInteger(1),
                                                 *document::CompositionFormat::create(64, 36));
    expect(static_cast<bool>(created), label + ": the scripting session is created");
    if (!created)
        return;
    const auto scripting = std::move(created).takeSession();
    const auto composition = scripting->snapshot().project().compositions().front().id();
    const auto imported = scripting->executeJsonOperation(
        "bloom.asset.import",
        {{"paths",
          scripting::Value{scripting::ValueArray{scripting::Value{movie.generic_string()}}}}});
    expect(imported.succeeded() && scripting->snapshot().project().assets().size() == 1,
           label + ": the movie imports into the scripting project");
    if (scripting->snapshot().project().assets().size() != 1)
        return;
    const auto asset = scripting->snapshot().project().assets().front();
    const auto layered = scripting->executeJsonOperation(
        "bloom.layer.add-from-assets",
        {{"composition", scripting::Value{scripting::StableId{composition.value()}}},
         {"time", scripting::Value{std::int64_t{0}}},
         {"assets", scripting::Value{scripting::ValueArray{
                        scripting::Value{scripting::StableId{asset.id.value()}}}}}});
    expect(layered.succeeded(), label + ": the movie becomes a layer");
    if (!layered.succeeded())
        return;
    mirror.flush();

    // Audio analysis: a fresh cache, so the source opens again.
    {
        runtime::clearAudioAnalysis();
        media::video::releaseRetainedSourceSnapshots();
        auto source = std::make_shared<runtime::AudioAnalysisSource>();
        source->asset = asset;
        source->sound = std::make_shared<runtime::AudioAnalysisSound>();
        runtime::AudioAnalysisQuery query;
        query.source = source;
        query.kind = runtime::AudioAnalysisKind::Amplitude;
        query.time = core::RationalTime::fromInteger(0);
        const auto before = measure(mirror);
        const auto status =
            runtime::prepareAudioAnalysis(query, movie.parent_path(), runtime::CancellationToken{});
        expect(status.outcome == runtime::AudioAnalysisOutcome::Ready,
               label + ": the audio analysis is ready: " + status.detail);
        runtime::clearAudioAnalysis();
        check(label, "audio analysis session", before, measure(mirror), first);
    }

    // The export's audio preflight and its audio decoder.
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({.snapshot = scripting->snapshot(),
                                            .compositionId = composition,
                                            .fontFetch = runtime::fonts::FontFetch::Wait},
                                           runtime::CancellationToken{});
    expect(compiled.plan != nullptr && !output::exportAudioSourcesV1(*compiled.plan).empty(),
           label + ": the composition's mix reads the movie's sound");
    if (compiled.plan != nullptr) {
        media::video::releaseRetainedSourceSnapshots();
        const auto before = measure(mirror);
        const auto facts = output::preflightExportAudioSourcesV1(
            scripting->snapshot(), *compiled.plan, movie.parent_path(), [] { return false; });
        expect(std::holds_alternative<output::ExportAudioSourceFactsV1>(facts),
               label + ": the export audio preflight accepts the movie");
        check(label, "export audio preflight session", before, measure(mirror), first);
    }
}

// A sound-only file: one second of a quiet tone, as 16-bit PCM in a WAV container.
std::filesystem::path writeTone(const std::filesystem::path& path) {
    constexpr std::uint32_t rate = 48000;
    constexpr std::uint32_t frames = rate;
    const auto put = [](std::string& out, const std::uint32_t value, const int bytes) {
        for (int index = 0; index < bytes; ++index)
            out.push_back(static_cast<char>((value >> (8 * index)) & 0xffU));
    };
    std::string data;
    for (std::uint32_t frame = 0; frame < frames; ++frame)
        put(data, (frame / 24U) % 2U != 0U ? 3000U : 0xffffffffU - 3000U, 2);
    std::string file = "RIFF";
    put(file, 36U + static_cast<std::uint32_t>(data.size()), 4);
    file += "WAVEfmt ";
    put(file, 16, 4);
    put(file, 1, 2);
    put(file, 1, 2);
    put(file, rate, 4);
    put(file, rate * 2U, 4);
    put(file, 2, 2);
    put(file, 16, 2);
    file += "data";
    put(file, static_cast<std::uint32_t>(data.size()), 4);
    file += data;
    std::ofstream(path, std::ios::binary) << file;
    // Settled (settled_file.hpp): the sessions count identity-only reuse of the source.
    static_cast<void>(bloom::test_support::settleFileIdentity(path));
    return path;
}

// The export's audio decoder on its own: a sound file has no picture, so no other session of the
// export holds the source for the decoder to adopt.
void soundExportPass(const std::string& label, const std::filesystem::path& sound,
                     const std::filesystem::path& workDir, media::cache::SourceMirror& mirror,
                     const bool first) {
    auto created = scripting::Session::createNew("Sound", "Main", core::RationalTime::fromInteger(1),
                                                 *document::CompositionFormat::create(64, 36));
    expect(static_cast<bool>(created), label + ": the sound session is created");
    if (!created)
        return;
    const auto scripting = std::move(created).takeSession();
    const auto composition = scripting->snapshot().project().compositions().front().id();
    const auto imported = scripting->executeJsonOperation(
        "bloom.asset.import",
        {{"paths",
          scripting::Value{scripting::ValueArray{scripting::Value{sound.generic_string()}}}}});
    expect(imported.succeeded() && scripting->snapshot().project().assets().size() == 1,
           label + ": the sound imports");
    if (scripting->snapshot().project().assets().size() != 1)
        return;
    const auto layered = scripting->executeJsonOperation(
        "bloom.layer.add-audio",
        {{"composition", scripting::Value{scripting::StableId{composition.value()}}},
         {"asset", scripting::Value{scripting::StableId{
                       scripting->snapshot().project().assets().front().id.value()}}}});
    expect(layered.succeeded(), label + ": the sound becomes a layer");
    if (!layered.succeeded())
        return;
    mirror.flush();
    {
        runtime::TaskScheduler exportScheduler;
        const runtime::SnapshotCompiler exportCompiler(document::builtInNodeDefinitions());
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
        options.enabled = false;
        const auto provider = host::GpuExportProvider::create(options);
        provider->prepare(exportScheduler);
        const auto exported = workDir / (label + "-sound-export.wav");
        media::video::releaseRetainedSourceSnapshots();
        const auto before = measure(mirror);
        const auto encoded = scripting::Render::run(
            *scripting, exportScheduler, exportCompiler,
            {.composition = composition,
             .frame = std::nullopt,
             .range = std::pair<std::uint64_t, std::uint64_t>{0, 1},
             .preset = output::OutputPresetV1::PcmWavV1,
             .destination = exported,
             // The export's preflight leaves a retained copy of the source that the audio
             // decoder would adopt; each cancellation check drops it, as the idle time of a
             // real export does, so the decoder meets the store alone.
             .cancelled =
                 [] {
                     media::video::releaseRetainedSourceSnapshots();
                     return false;
                 }},
            workDir / ("scratch-" + label), provider);
        expect(encoded.succeeded && std::filesystem::exists(exported),
               label + ": the sound exports: " + encoded.diagnostic);
        expect(provider->shutdownAndWait(10s), label + ": the export provider retires");
        check(label, "export audio decoder session", before, measure(mirror), first);
    }
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomAudioSourceReads");
    QCoreApplication::setApplicationName("AudioSourceReads");
    std::filesystem::path fixtures;
    const auto arguments = QCoreApplication::arguments();
    for (int index = 1; index < arguments.size(); ++index)
        if (arguments[index] == "--media-fixtures" && index + 1 < arguments.size())
            fixtures = arguments[++index].toStdString();
    const auto fixture = fixtures / "numbered-prores.mov";
    if (fixtures.empty() || !std::filesystem::exists(fixture) ||
        !output::mediaWorkerAvailableV1()) {
        std::cout << "NotRun: media audio source reads: no media worker or movie fixture here\n";
        return 0;
    }
    const std::filesystem::path workDir = temporary.path().toStdString();
    const auto footage = workDir / "footage";
    std::filesystem::create_directories(footage);
    const auto movie = footage / "movie.mov";
    std::filesystem::copy_file(fixture, movie);
    static_cast<void>(bloom::test_support::settleFileIdentity(movie));

    media::cache::SourceMirrorConfig config;
    config.rootDirectory = workDir / "mirror";
    // The scratch folder is fast local storage: the preference stands in for a network share.
    config.policy.preference = media::cache::LocalCopyPreference::Always;
    config.driveSpace = media::cache::roomyDriveSpaceForTest();
    const auto mirror = std::make_shared<media::cache::SourceMirror>(config);
    mirror->flush();
    platform::setProcessSourceStore(mirror);

    // The Assets passes run first: nothing has started the process memory sampling yet, so the
    // controller's decoded-sound cache keeps nothing past its pass and the audio session below
    // opens alone, with the thumbnail already cached.
    assetsPass("first", movie, *mirror, true);
    assetsPass("second", movie, *mirror, false);
    enginePass("first", movie, *mirror, true);
    mirror->flush();
    enginePass("second", movie, *mirror, false);
    const auto sound = writeTone(footage / "tone.wav");
    soundExportPass("first", sound, workDir, *mirror, true);
    mirror->flush();
    soundExportPass("second", sound, workDir, *mirror, false);
    platform::setProcessSourceStore(nullptr);

    failures += bloom::test_support::awaitFailureCount();
    if (failures != 0) {
        std::cerr << "media audio source reads: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: media audio source reads\n";
    return 0;
}
