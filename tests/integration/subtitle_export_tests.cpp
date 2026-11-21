// #752: subtitle tracks end to end through the export queue and the media worker.
//
// The composition is subtitle_tracks_fixture.hpp's: an offset Subtitles layer, a trimmed one with
// Time Remap and a nested one at twice the speed (bloom.runtime.subtitle-tracks checks, on every
// frame, that the track pass shows exactly what the CPU picture shows for this composition and
// range). Here the export of frames 6-90 is reopened:
//
// - H.264 MP4, ProRes MOV and VP9 WebM with SRT, WebVTT and TTML sidecars and embedded tracks: each
//   sidecar parsed from the published file and each embedded track read back by FFmpeg's demuxer
//   holds, on every output frame, exactly the cues the picture shows; the results list every file
//   with its digest, and the log names each one;
// - a PNG sequence writes the same sidecars; asking it to embed is a blocking preflight issue that
//   names the sidecar formats;
// - two tracks that share a language are numbered; an existing sidecar under "never replace" stops
//   the export before it renders and leaves the file alone;
// - a sidecar whose text or frames differ from its track is a difference (the check that fails the
//   output), and so is an embedded track that differs from what was sent.
//
// Without the subtitle encoders (a dependency prefix older than the lock) the embedded cases say
// SKIP and the test exits 77 after every other case passed.

#include "export_subtitles.hpp"
#include "ffmpeg_subtitle_tracks.hpp"
#include "fixed_memory_budget.hpp"
#include "subtitle_tracks_fixture.hpp"
#include "unique_temp_dir.hpp"

#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/cue_markup.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::subtitle_tracks;
using document::ExportFormat;
using document::ExportSubtitleFormat;

int failures = 0;
bool embeddedSkipped = false;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr std::uint64_t kFirst = 6;
constexpr std::uint64_t kLast = 90;
constexpr std::uint64_t kFrames = kLast - kFirst + 1;
constexpr media::provider::Rational kRational{kRate, 1};

host::ExportEncoderStatus linuxEncoders() {
    host::ExportEncoderStatus status;
    status.probed = true;
    status.mediaWorker = true;
    status.h264Software = output::H264EncoderV1::OpenH264;
    status.openh264 = host::ExportEncoderStatus::OpenH264::Installed;
    return status;
}

// The expected cue of each exported node, in output frames.
struct Expected final {
    document::NodeId node;
    std::string language; // BCP 47
    std::string title;
    std::vector<ExpectedCue> cues;
};

std::vector<Expected> expectedTracks(const Fixture& fixture) {
    const auto shift = [](std::vector<ExpectedCue> cues) {
        for (auto& cue : cues) {
            cue.first -= kFirst;
            cue.end -= kFirst;
        }
        return cues;
    };
    return {{fixture.alpha, "en", "Alpha", shift(Fixture::expectedAlpha())},
            {fixture.bravo, "fr", "", shift(Fixture::expectedBravo())},
            {fixture.charlie, "en", "", shift(Fixture::expectedCharlie())}};
}

// What a track shows on each output frame: the texts of its active cues, in order.
using Shown = std::vector<std::vector<std::string>>;

Shown shownOf(const std::vector<ExpectedCue>& cues) {
    Shown shown(kFrames);
    for (const auto& cue : cues)
        for (auto frame = cue.first; frame < cue.end && frame < kFrames; ++frame)
            shown[frame].push_back(cue.text);
    return shown;
}

std::uint64_t frameAtOrAfter(const core::RationalTime time) {
    const auto numerator = time.numerator() * kRate;
    return static_cast<std::uint64_t>((numerator + time.denominator() - 1) / time.denominator());
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A published sidecar, parsed back: what it shows on each output frame.
std::optional<Shown> sidecarShown(const std::filesystem::path& path,
                                  const ExportSubtitleFormat format) {
    subtitles::LoadOptions options;
    options.format = format == ExportSubtitleFormat::Srt      ? subtitles::Format::Srt
                     : format == ExportSubtitleFormat::WebVtt ? subtitles::Format::WebVtt
                                                              : subtitles::Format::Ttml;
    const auto loaded = subtitles::load(readAll(path), options);
    const auto* result = std::get_if<subtitles::LoadResult>(&loaded);
    if (result == nullptr)
        return std::nullopt;
    std::vector<ExpectedCue> cues;
    for (const auto& cue : result->cues.cues)
        cues.push_back({frameAtOrAfter(cue->start), frameAtOrAfter(cue->end),
                        document::cue_markup::plainText(cue->text)});
    return shownOf(cues);
}

// An embedded track read back by FFmpeg: what it shows on each output frame. A MOV or MP4 sample
// holds the cues shown together, one per line group.
Shown embeddedShown(const media::provider::SubtitleTrackV1& track, const bool webm) {
    std::vector<ExpectedCue> cues;
    for (const auto& cue : track.cues)
        cues.push_back(
            {cue.first, cue.end, webm ? document::cue_markup::plainText(cue.text) : cue.text});
    return shownOf(cues);
}

// MOV and MP4 join cues shown together.
Shown joinedShown(const Shown& shown) {
    Shown out(shown.size());
    for (std::size_t frame = 0; frame < shown.size(); ++frame) {
        if (shown[frame].empty())
            continue;
        std::string text;
        for (const auto& cue : shown[frame])
            text += (text.empty() ? "" : "\n") + cue;
        out[frame].push_back(text);
    }
    return out;
}

std::string firstDifference(const Shown& actual, const Shown& expected) {
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
        if (actual[frame] != expected[frame])
            return "output frame " + std::to_string(frame) + " shows " +
                   (actual[frame].empty() ? std::string("nothing") : actual[frame].front()) +
                   ", the picture " +
                   (expected[frame].empty() ? std::string("nothing") : expected[frame].front());
    return {};
}

struct Rig {
    std::filesystem::path folder;
    Fixture source;
    host::ProjectSessionIdentitySource identity;
    host::ProjectSession session;
    runtime::NodeDefinitionRegistry registry;
    runtime::SnapshotCompiler compiler{registry};
    runtime::TaskScheduler scheduler;
    host::PublicationCoordinator publications = [] {
        auto result = host::PublicationCoordinator::create();
        if (!result)
            throw std::runtime_error("publication coordinator");
        return std::move(*result);
    }();
    platform::StagedArtifactCoordinator artifacts = [] {
        auto result = platform::StagedArtifactCoordinator::create({});
        if (!result)
            throw std::runtime_error("artifact coordinator");
        return std::move(result).takeCoordinator();
    }();
    std::shared_ptr<output::ExportResourceLedgerV1> ledger =
        std::make_shared<output::ExportResourceLedgerV1>();
    std::shared_ptr<host::GpuExportProvider> gpu;
    std::shared_ptr<host::SequenceExportRetirementOwnerV1> retirement =
        host::SequenceExportRetirementOwnerV1::create();
    std::shared_ptr<host::ExportPresetCatalogue> presets;
    std::unique_ptr<host::ExportQueueService> service;

    static std::filesystem::path scratch() {
        auto path = bloom::test_support::processTempDirectory() /
                    ("bloom-subtitle-export-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path / "Exports");
        std::filesystem::create_directories(path / "scratch");
        return path;
    }
    static host::ProjectSession makeSession(host::ProjectSessionIdentitySource& identity,
                                            const std::filesystem::path& folder,
                                            const Fixture& source) {
        auto path = host::ProjectDisplayPath::create(folder / "subtitles.bloom");
        auto result = host::ProjectSession::createDecoded(
            identity, {.project = source.document.snapshot().project(),
                       .colorSettings = document::makeBloomNeutralColorSettingsV1({}),
                       .editability = host::DecodedProjectEditability::Editable,
                       .displayPath = std::move(path),
                       .persistedAllocatorHighWater = std::nullopt});
        if (!result)
            throw std::runtime_error("session fixture");
        return std::move(result).takeSession();
    }

    Rig() : folder(scratch()), session(makeSession(identity, folder, source)) {
        if (!runtime::registerBuiltInNodeDefinitions(registry))
            throw std::runtime_error("registry");
        registry.freeze();
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
        options.enabled = false;
        gpu = host::GpuExportProvider::create(std::move(options));
        gpu->prepare(scheduler);
        presets = host::ExportPresetCatalogue::create(
            {.userDataDirectory = std::nullopt, .initialEncoders = linuxEncoders()});
        service = std::make_unique<host::ExportQueueService>(
            host::ExportQueueServiceDependencies{.session = session,
                                                 .scheduler = scheduler,
                                                 .compiler = compiler,
                                                 .publications = publications,
                                                 .artifacts = artifacts,
                                                 .ledger = ledger,
                                                 .gpu = gpu,
                                                 .retirement = retirement,
                                                 .presets = presets,
                                                 .limits = {},
                                                 .displayProcessor = nullptr,
                                                 .scratchDirectory = folder / "scratch",
                                                 .backend = nullptr});
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    ~Rig() {
        service->beginShutdown(host::ExportStopCause::ApplicationQuit);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!service->quiescent() && std::chrono::steady_clock::now() < deadline) {
            service->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        service.reset();
        scheduler.beginShutdown();
        retirement->beginShutdown();
        while (!scheduler.isQuiescent() || !retirement->retirementComplete())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!retirement->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        (void)gpu->shutdownAndWait(std::chrono::seconds(10));
        std::filesystem::remove_all(folder);
    }

    document::ExportJobId addJob(const ExportFormat format, std::string path,
                                 const document::ExportSubtitles subtitles,
                                 const document::ExportOverwritePolicy overwrite =
                                     document::ExportOverwritePolicy::Replace) {
        document::ExportJob job;
        job.composition = source.main;
        job.render.range = {document::ExportRangeKind::Custom, kFirst, kLast};
        document::ExportOutput output;
        output.presetId = "test";
        output.settings.format = format;
        output.settings.includeAudio = false;
        if (format == ExportFormat::Vp9Webm)
            output.settings.codecProfile = "8bit";
        if (format == ExportFormat::ProResMov) {
            output.settings.codecProfile = "hq";
            output.settings.encoder = document::ExportEncoderPreference::Software;
        }
        output.settings.subtitles = subtitles;
        output.destination = {"project-relative", std::move(path), overwrite};
        job.outputs = {output};
        commands::Transaction transaction("Add export job");
        transaction.emplace<commands::AddExportJob>(job);
        const auto result = session.execute(std::move(transaction));
        if (!result.changed())
            throw std::runtime_error("add job");
        return *result.command->outputId<document::ExportJobId>(commands::kExportJobOutput);
    }

    std::optional<host::ExportJobView> view(const document::ExportJobId id) const {
        service->pump();
        for (const auto& job : service->snapshot()->jobs)
            if (job.id == id)
                return job;
        return std::nullopt;
    }
    template <typename Predicate> bool pumpUntil(Predicate&& done, const int seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            service->pump();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    std::optional<host::ExportJobView> run(const document::ExportJobId job,
                                           const std::string& name) {
        expect(pumpUntil([&] { return service->canStart(job).available; }, 20),
               name + ": the job becomes startable");
        const document::ExportJobId ids[] = {job};
        expect(service->start(ids).accepted, name + ": start");
        expect(pumpUntil([&] { return host::isTerminal(view(job)->state); }, 180),
               name + ": the job finishes");
        return view(job);
    }
    std::vector<host::ExportLogEntry> log(const document::ExportJobId job) const {
        return service->log(job, 0).entries;
    }
};

bool published(const std::optional<host::ExportJobView>& view) {
    return view && (view->state == host::ExportJobState::Done ||
                    view->state == host::ExportJobState::DoneWithWarnings);
}

bool encoderMissing(Rig& rig, const document::ExportJobId job) {
    for (const auto& entry : rig.log(job))
        if (entry.message.find("Subtitle encoder is not in the intake") != std::string::npos)
            return true;
    return false;
}

const std::vector<ExportSubtitleFormat> kAllSidecars{
    ExportSubtitleFormat::Srt, ExportSubtitleFormat::WebVtt, ExportSubtitleFormat::Ttml};

// Every sidecar of `view` reads back as its node's track, frame by frame; the names follow the
// rule and the log lists each file with its digest.
void checkSidecars(Rig& rig, const document::ExportJobId job, const host::ExportJobView& view,
                   const std::string& base, const std::string& name) {
    const auto& verification = view.outputs.front().verification;
    expect(verification.has_value(), name + ": the output has its verification");
    if (!verification)
        return;
    const auto expected = expectedTracks(rig.source);
    std::set<std::string> names;
    std::size_t sidecars = 0;
    for (const auto& file : verification->subtitles) {
        if (!file.sidecar)
            continue;
        ++sidecars;
        names.insert(file.path.filename().string());
        const auto track = std::ranges::find(expected, file.node, &Expected::node);
        expect(track != expected.end(), name + ": a sidecar belongs to an exported node");
        if (track == expected.end())
            continue;
        const auto shown = sidecarShown(file.path, *file.sidecar);
        expect(shown.has_value(), name + ": " + file.path.filename().string() + " parses back");
        if (!shown)
            continue;
        const auto difference = firstDifference(*shown, shownOf(track->cues));
        expect(difference.empty(), name + ": " + file.path.filename().string() + ": " + difference);
        const auto bytes = readAll(file.path);
        const auto digest = core::Sha256Hasher::hash(std::as_bytes(std::span(bytes)));
        expect(file.digest && digest && *file.digest == *digest && file.bytes == bytes.size(),
               name + ": " + file.path.filename().string() + "'s digest is the published file's");
        expect(file.language == track->language && file.title == track->title &&
                   file.cues == track->cues.size(),
               name + ": " + file.path.filename().string() + " names its language and title");
        const auto hex = digest->toLowercaseHex();
        bool logged = false;
        for (const auto& entry : rig.log(job))
            logged = logged ||
                     (entry.code == "export.subtitles.sidecar" &&
                      entry.message.find(file.path.filename().string()) != std::string::npos &&
                      entry.message.find(std::string(hex.data(), hex.size())) != std::string::npos);
        expect(logged,
               name + ": the log lists " + file.path.filename().string() + " with its digest");
    }
    expect(sidecars == 9, name + ": three formats of three tracks are published");
    // Alpha and Charlie share English, so both are numbered; Bravo is French.
    for (const auto* extension : {"srt", "vtt", "ttml"})
        for (const auto& wanted :
             {base + ".en.1." + extension, base + ".en.2." + extension, base + ".fr." + extension})
            expect(names.contains(wanted) &&
                       std::filesystem::exists(rig.folder / "Exports" / wanted),
                   name + ": " + wanted + " is published");
}

void checkEmbedded(const Fixture& source, const host::ExportJobView& view,
                   const std::filesystem::path& movie, const bool webm, const std::string& name) {
    const auto& verification = *view.outputs.front().verification;
    std::vector<const host::ExportSubtitleFile*> embedded;
    for (const auto& file : verification.subtitles)
        if (!file.sidecar)
            embedded.push_back(&file);
    expect(embedded.size() == 3, name + ": three embedded tracks are listed");
    const auto reopened = media::ffmpeg::readSubtitleTracks(movie.string(), kRational);
    expect(reopened.size() == embedded.size(), name + ": FFmpeg reads back every embedded track");
    for (std::size_t index = 0; index < std::min(reopened.size(), embedded.size()); ++index) {
        const auto& file = *embedded[index];
        expect(file.path == movie, name + ": an embedded track names its movie");
        const auto& track = reopened[index];
        expect(track.language == file.language && track.title == file.title,
               name + ": embedded track " + std::to_string(index + 1) +
                   " carries its language and title");
        // Which node: by the language and title the results name.
        const auto expectedLanguage = [&](const std::string& bcp47) {
            return bcp47 == "fr" ? std::string(webm ? "fre" : "fra") : std::string("eng");
        };
        bool matched = false;
        for (const auto& candidate : expectedTracks(source)) {
            if (candidate.node != file.node)
                continue;
            matched = true;
            expect(track.language == expectedLanguage(candidate.language),
                   name + ": the ISO 639-2 code fits the container");
            const auto want = webm ? shownOf(candidate.cues) : joinedShown(shownOf(candidate.cues));
            const auto difference = firstDifference(embeddedShown(track, webm), want);
            expect(difference.empty(),
                   name + ": embedded track " + std::to_string(index + 1) + ": " + difference);
        }
        expect(matched, name + ": an embedded track belongs to an exported node");
    }
}

void testMovies() {
    struct Case {
        const char* name;
        ExportFormat format;
        const char* extension;
        bool webm;
    };
    for (const auto& c : {Case{"H.264 MP4", ExportFormat::H264Mp4, "mp4", false},
                          Case{"ProRes MOV", ExportFormat::ProResMov, "mov", false},
                          Case{"VP9 WebM", ExportFormat::Vp9Webm, "webm", true}}) {
        Rig rig;
        const std::string base = std::string("Shot-") + c.extension;
        const auto job = rig.addJob(c.format, "Exports/" + base + "." + c.extension,
                                    {kAllSidecars, true, std::nullopt});
        const auto view = rig.run(job, c.name);
        if (!published(view) && encoderMissing(rig, job)) {
            std::cout << "SKIP: " << c.name
                      << " embedded tracks — this FFmpeg has no subtitle encoder (a dependency "
                         "prefix older than the lock; rebuild it)\n";
            embeddedSkipped = true;
            continue;
        }
        expect(published(view), std::string(c.name) + ": the export publishes");
        if (!published(view)) {
            for (const auto& entry : rig.log(job))
                std::cerr << "  log: " << entry.code << ": " << entry.message << '\n';
            continue;
        }
        const auto& verification = *view->outputs.front().verification;
        expect(std::ranges::any_of(verification.checks,
                                   [](const host::ExportCheckResult& result) {
                                       return result.check == host::ExportCheck::Subtitles &&
                                              result.outcome == host::ExportCheckOutcome::Passed;
                                   }),
               std::string(c.name) + ": the Subtitles check passed");
        checkSidecars(rig, job, *view, base, c.name);
        checkEmbedded(rig.source, *view, rig.folder / "Exports" / (base + "." + c.extension),
                      c.webm, c.name);
        std::cout << c.name << ": " << verification.subtitles.size()
                  << " subtitle files and tracks verified\n";
    }
}

void testImageSequence() {
    Rig rig;
    const auto job =
        rig.addJob(ExportFormat::Png, "Exports/Plate.png", {kAllSidecars, false, std::nullopt});
    const auto view = rig.run(job, "PNG sequence");
    expect(published(view), "PNG sequence: the export publishes");
    if (published(view))
        checkSidecars(rig, job, *view, "Plate", "PNG sequence");
    // Embedding in a sequence is a blocking preflight issue naming the sidecar formats.
    const auto embedded = rig.addJob(ExportFormat::Png, "Exports/Embedded.png",
                                     {{ExportSubtitleFormat::Srt}, true, std::nullopt});
    expect(rig.pumpUntil(
               [&] {
                   const auto current = rig.view(embedded);
                   return current &&
                          std::ranges::any_of(
                              current->outputs.front().preflight,
                              [](const host::ExportPreflightIssue& issue) {
                                  return issue.code ==
                                         host::ExportPreflightCode::SubtitleEmbedUnsupported;
                              });
               },
               20),
           "embedding in a PNG sequence is a preflight issue");
    const auto current = rig.view(embedded);
    for (const auto& issue : current->outputs.front().preflight)
        if (issue.code == host::ExportPreflightCode::SubtitleEmbedUnsupported)
            expect(issue.severity == host::ExportPreflightSeverity::Blocking &&
                       issue.reason.find("SRT, WebVTT or TTML") != std::string::npos,
                   "the issue blocks and names the sidecar formats");
    expect(!rig.service->canStart(embedded).available, "the job cannot start");
}

void testExistingSidecarIsNotReplaced() {
    Rig rig;
    const auto existing = rig.folder / "Exports" / "Kept.fr.srt";
    {
        std::ofstream out(existing);
        out << "untouched";
    }
    const auto job = rig.addJob(ExportFormat::Png, "Exports/Kept.png",
                                {{ExportSubtitleFormat::Srt}, false, std::nullopt},
                                document::ExportOverwritePolicy::Refuse);
    const auto view = rig.run(job, "existing sidecar");
    expect(view && view->state == host::ExportJobState::Failed,
           "an existing sidecar under never replace fails the output");
    expect(view && view->failure && view->failure->reason.find("Kept.fr.srt") != std::string::npos,
           "the failure names the file");
    expect(readAll(existing) == "untouched", "the existing file is left alone");
    expect(!std::filesystem::exists(rig.folder / "Exports" / "Kept.000006.png") &&
               !std::filesystem::exists(rig.folder / "Exports" / "Kept.0006.png"),
           "nothing rendered");
}

void testDifferencesFail() {
    // A sidecar that does not read back as its track.
    const std::vector<media::provider::SubtitleCueV1> track{{0, 12, "One"}, {20, 30, "Two"}};
    document::CueListValue list;
    list.language = "en";
    for (std::uint64_t index = 0; index < track.size(); ++index) {
        auto cue = std::make_shared<document::Cue>();
        cue->id = index + 1;
        cue->start = at(static_cast<std::int64_t>(track[index].first), kRate);
        cue->end = at(static_cast<std::int64_t>(track[index].end), kRate);
        cue->text = track[index].text;
        list.cues.push_back(std::move(cue));
    }
    list.nextCueId = 3;
    for (const auto format : kAllSidecars) {
        const auto writer = format == ExportSubtitleFormat::Srt      ? subtitles::Format::Srt
                            : format == ExportSubtitleFormat::WebVtt ? subtitles::Format::WebVtt
                                                                     : subtitles::Format::Ttml;
        subtitles::WriteOptions options;
        options.frameRate = std::pair{static_cast<std::uint32_t>(kRate), 1U};
        const auto written = subtitles::write(list, writer, options);
        expect(written.has_value(), "the injection fixture writes");
        if (!written)
            continue;
        expect(host::detail::sidecarDifference(written->text, format, track, kRational).empty(),
               "a sidecar equal to its track has no difference");
        auto late = track;
        late[1].end += 1;
        expect(!host::detail::sidecarDifference(written->text, format, late, kRational).empty(),
               "a sidecar one frame short of its track is a difference");
        auto other = track;
        other[0].text = "Uno";
        expect(!host::detail::sidecarDifference(written->text, format, other, kRational).empty(),
               "a sidecar with other text is a difference");
        auto fewer = track;
        fewer.pop_back();
        expect(!host::detail::sidecarDifference(written->text, format, fewer, kRational).empty(),
               "a sidecar with another cue count is a difference");
    }
    // An embedded track that differs from what was sent.
    media::provider::SubtitleTrackV1 sent{"eng", "Alpha", track};
    auto reopened = sent;
    reopened.cues[0].end += 1;
    expect(!media::provider::subtitleTrackDifference({sent}, {reopened}).empty(),
           "an embedded track that reopens one frame long is a difference");
    // The language and naming rules.
    expect(host::detail::iso6392Language("fr-CA", false) == "fra" &&
               host::detail::iso6392Language("fr-CA", true) == "fre" &&
               host::detail::iso6392Language("zh-Hant", false) == "zho" &&
               host::detail::iso6392Language("yue", false) == "yue" &&
               host::detail::iso6392Language("", false) == "und" &&
               host::detail::iso6392Language("x-klingon", false) == "und",
           "BCP 47 tags map to ISO 639-2");
    expect(host::detail::sidecarPath("/out/Shot.mov", "pt-BR", std::nullopt,
                                     ExportSubtitleFormat::WebVtt) ==
                   std::filesystem::path("/out/Shot.pt-BR.vtt") &&
               host::detail::sidecarPath("/out/Shot.mov", "en", 2, ExportSubtitleFormat::Srt) ==
                   std::filesystem::path("/out/Shot.en.2.srt"),
           "sidecar names follow <base>.<lang>[.<n>].<ext>");
}

} // namespace

int main() {
    try {
        testDifferencesFail();
        testImageSequence();
        testExistingSidecarIsNotReplaced();
        testMovies();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "subtitle export: all checks passed\n";
    return embeddedSkipped ? 77 : 0;
}
