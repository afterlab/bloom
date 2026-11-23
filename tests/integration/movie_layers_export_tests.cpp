// #772: an export of a composition with sixteen movie layers completes. A linear pass (the export)
// admits the source snapshot of every movie its frames read at once, beyond the eight sources an
// idle decode context keeps, and a layer that leaves the range gives its snapshot back. Before, the
// ninth movie of a frame was refused for the video-snapshot quota.
//
// The movies are generated: the export first writes a short ProRes 4444 movie of the composition's
// Background Color, then sixteen copies of it each carry a QuickTime 'free' atom with its own
// number, so every layer is a different file (its own digest, its own snapshot). Eight layers span
// the composition; the other eight leave the range at different frames, so the frames need from
// sixteen sources down to eight.

#include "fixed_memory_budget.hpp"
#include "unique_temp_dir.hpp"
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/file_snapshot.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace bloom;
using document::ExportFormat;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

document::Snapshot currentSnapshot(const host::ProjectSession& session) {
    const auto decoded = session.decodedSnapshot();
    return decoded.snapshot();
}

host::ExportEncoderStatus linuxEncoders() {
    host::ExportEncoderStatus status;
    status.probed = true;
    status.mediaWorker = true;
    status.h264Software = output::H264EncoderV1::OpenH264;
    status.openh264 = host::ExportEncoderStatus::OpenH264::Installed;
    return status;
}

struct Fixture {
    std::filesystem::path folder;
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

    static host::ProjectSession makeSession(host::ProjectSessionIdentitySource& identity,
                                            const std::filesystem::path& folder) {
        auto format = document::CompositionFormat::create(64, 32);
        auto created =
            document::makeNewProject("Layers", "Main", core::RationalTime::fromInteger(1), *format);
        auto path = host::ProjectDisplayPath::create(folder / "layers.bloom");
        auto result = host::ProjectSession::createDecoded(
            identity, {.project = std::move(created.project),
                       .colorSettings = document::makeBloomNeutralColorSettingsV1({}),
                       .editability = host::DecodedProjectEditability::Editable,
                       .displayPath = std::move(path),
                       .persistedAllocatorHighWater = std::nullopt});
        if (!result)
            throw std::runtime_error("session fixture");
        return std::move(result).takeSession();
    }

    explicit Fixture(std::filesystem::path where)
        : folder(std::move(where)), session(makeSession(identity, folder)) {
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
        std::filesystem::create_directories(folder / "scratch");
        std::filesystem::create_directories(folder / "Exports");
    }
    ~Fixture() {
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
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    document::CompositionId composition() const {
        return currentSnapshot(session).project().compositions().front().id();
    }
    const document::Composition& main(const document::Snapshot& snapshot) const {
        return *snapshot.project().findComposition(composition());
    }
    core::RationalTime frameTime(const std::int64_t frame) const {
        const auto snapshot = currentSnapshot(session);
        const auto rate = main(snapshot).format().frameRate();
        return *core::RationalTime::create(frame * rate.denominator(), rate.numerator());
    }
    std::int64_t lastFrame() const {
        const auto snapshot = currentSnapshot(session);
        const auto rate = main(snapshot).format().frameRate();
        const auto duration = main(snapshot).duration();
        return duration.numerator() * rate.numerator() /
                   (duration.denominator() * rate.denominator()) -
               1;
    }
    void execute(commands::Transaction transaction, const std::string& what) {
        if (!session.execute(std::move(transaction)).changed())
            throw std::runtime_error(what);
    }

    document::ExportJobId addJob(document::ExportOutputSettings settings, std::string path) {
        document::ExportJob job;
        job.composition = composition();
        job.render.range = {document::ExportRangeKind::Composition, 0, 0};
        document::ExportOutput output;
        output.presetId = "test";
        output.settings = std::move(settings);
        output.destination = {"project-relative", std::move(path),
                              document::ExportOverwritePolicy::Replace};
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
    // Starts the job and waits for its end; the finished view. A failed job's log is printed.
    std::optional<host::ExportJobView> run(const document::ExportJobId job,
                                           const std::string& name) {
        expect(pumpUntil([&] { return service->canStart(job).available; }, 20),
               name + ": the job becomes startable");
        const document::ExportJobId ids[] = {job};
        expect(service->start(ids).accepted, name + ": start");
        expect(pumpUntil([&] { return host::isTerminal(view(job)->state); }, 240),
               name + ": the job finishes");
        if (const auto finished = view(job); finished && finished->failure)
            for (const auto& entry : service->log(job, 0).entries)
                std::cerr << "  " << name << " log: " << entry.code << ": " << entry.message
                          << '\n';
        return view(job);
    }
};

bool published(const std::optional<host::ExportJobView>& view) {
    return view && (view->state == host::ExportJobState::Done ||
                    view->state == host::ExportJobState::DoneWithWarnings);
}

// A copy of `source` with a QuickTime 'free' atom (ignored by every reader) carrying `number`
// appended: the same movie as another file.
void writeNumberedCopy(const std::filesystem::path& source, const std::filesystem::path& target,
                       const std::uint32_t number) {
    std::ifstream in(source, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const char atom[] = {0, 0, 0, 12, 'f', 'r', 'e', 'e'};
    bytes.insert(bytes.end(), std::begin(atom), std::end(atom));
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<char>((number >> static_cast<unsigned>(shift)) & 0xFFU));
    std::ofstream out(target, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The same export through the sequence runner, whose movie source copies this test can see: the
// copies' file limit rises with the sources the frames read and falls while the export runs as the
// trimmed layers leave the range; and twelve small files held beside the export fill its quota for the
// first captures, so they wait for a release the test announces, and the result says so (#772).
void testSnapshotWaitAndRelease(Fixture& f, const std::filesystem::path& folder,
                                const std::int64_t last) {
    const auto snapshot = currentSnapshot(f.session);
    const auto& composition = f.main(snapshot);
    host::SequenceExportRequestV1 request{
        .composition = {snapshot, f.composition()},
        .range = {folder / "Exports" / "runner.mov", 0, static_cast<std::uint64_t>(last),
                  composition.format().frameRate(), composition.duration()},
        .bwfDescription = {},
        .assetBaseDirectory = folder,
        .worker = {}};
    request.preset = output::OutputPresetV1::ProResMovV1;
    request.profile = "hq";
    request.audio = false;
    std::shared_ptr<platform::FileSnapshotQuota> quota;
    std::vector<std::size_t> limits;
    std::size_t lastSeen = 0;
    {
    host::SequenceExportRunnerV1 runner(f.scheduler, f.compiler, f.publications, f.artifacts,
                                        *f.ledger, std::move(request), *f.retirement);
    quota = runner.videoSourceSnapshotQuota();
    expect(quota != nullptr, "the runner exposes its video source quota");
    if (!quota)
        return;
    const auto heldFile = folder / "held.bin";
    {
        std::ofstream(heldFile, std::ios::binary) << "held";
    }
    std::filesystem::create_directories(folder / "held-spool");
    std::vector<std::unique_ptr<platform::ReadOnlyFileSnapshot>> held;
    quota->setFileLimit(12);
    for (int count = 0; count < 12; ++count) {
        auto made = platform::ReadOnlyFileSnapshot::capture(heldFile, folder / "held-spool", quota,
                                                            std::uint64_t{1} << 20U);
        if (auto* kept = std::get_if<std::unique_ptr<platform::ReadOnlyFileSnapshot>>(&made))
            held.push_back(std::move(*kept));
    }
    expect(held.size() == 12, "twelve snapshots hold the quota (" + std::to_string(held.size()) + ")");
    quota->expectRelease();
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(240);
    bool released = false, limitFellWhileRunning = false;
    std::size_t highestLimit = 0;
    while (!runner.result() && std::chrono::steady_clock::now() < deadline) {
        runner.poll();
        if (runner.stage() == host::SequenceExportStageV1::AwaitingApproval) {
            const auto* analysis = runner.analysis();
            const auto frame = runner.frameApprovalDigest();
            if (analysis && frame)
                expect(runner.approve(analysis->digest, *frame), "the export's captured identities");
        }
        if (!released &&
            std::chrono::steady_clock::now() - started > std::chrono::milliseconds(200)) {
            released = true;
            held.clear();
            quota->releaseExpected();
        }
        const auto limit = quota->fileLimit();
        if (limits.empty() || limits.back() != limit)
            limits.push_back(limit);
        if (runner.result() && lastSeen == 0)
            lastSeen = limit;
        highestLimit = std::max(highestLimit, limit);
        if (limit < highestLimit && !runner.result())
            limitFellWhileRunning = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(runner.result().has_value() && runner.result()->published(),
           "the runner export of sixteen movie layers publishes");
    expect(highestLimit >= 8 + 16,
           "the quota admitted all sixteen sources (" + std::to_string(highestLimit) + ")");
    expect(limitFellWhileRunning,
           "the quota's limit fell during the export as layers left the range");
    std::cout << "  limit when the export ended: " << lastSeen << '\n';
    std::cout << "  limits:";
    for (const auto value : limits)
        std::cout << ' ' << value;
    std::cout << '\n';
    if (!runner.result())
        return;
    const auto& waits = runner.result()->videoSnapshotWaits;
    expect(!waits.empty(), "the result reports a snapshot wait");
    for (const auto& wait : waits) {
        const auto text = host::describeVideoSnapshotWaitV1(wait);
        std::cout << "  " << text << '\n';
        expect(text.find("waited") != std::string::npos &&
                   text.find("for released video source snapshots to close") != std::string::npos &&
                   text.find("layer-") != std::string::npos,
               "the wait names the movie, how long and why: " + text);
    }
    } // the runner (and its pass) ends here
    // The pass ends on the retirement lane a moment after the runner is gone.
    for (int wait = 0; wait < 2000 && quota->fileLimit() != 8; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    expect(quota->fileLimit() == 8,
           "the pass's end leaves the idle limit of 8 (" + std::to_string(quota->fileLimit()) + ")");
}

void testSixteenMovieLayersExport() {
    const auto folder =
        bloom::test_support::processTempDirectory() /
        ("bloom-export-movie-layers-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Remove final {
        std::filesystem::path folder;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    } remove{folder};
    Fixture f(folder);
    // The source movie: the composition's Background Color, every frame.
    {
        const auto snapshot = currentSnapshot(f.session);
        auto settings = commands::compositionSettingsOf(f.main(snapshot));
        settings.background = core::Color4d{0.2, 0.5, 0.8, 1.0};
        commands::Transaction transaction("Background");
        transaction.emplace<commands::SetCompositionSettings>(f.composition(), settings);
        f.execute(std::move(transaction), "set the background");
    }
    document::ExportOutputSettings source;
    source.format = ExportFormat::ProResMov;
    source.codecProfile = "4444";
    source.includeAudio = false;
    source.background = document::ExportBackground::CompositionColor;
    const auto generated = f.run(f.addJob(source, "Exports/source.mov"), "source movie");
    expect(published(generated), "source movie: the generated movie publishes");
    if (!published(generated))
        return;
    std::vector<std::filesystem::path> movies;
    std::filesystem::create_directories(folder / "layers");
    for (std::uint32_t number = 0; number < 16; ++number) {
        movies.push_back(folder / "layers" / ("layer-" + std::to_string(number) + ".mov"));
        writeNumberedCopy(folder / "Exports" / "source.mov", movies.back(), number);
    }
    {
        commands::Transaction import("Import movies");
        import.emplace<commands::ImportAssets>(movies, folder);
        f.execute(std::move(import), "import the sixteen movies");
    }
    std::vector<commands::LayerDropItem> items;
    for (const auto& asset : currentSnapshot(f.session).project().assets())
        if (asset.kind == document::AssetKind::Video)
            items.emplace_back(asset.id);
    expect(items.size() == 16, "sixteen movie assets (" + std::to_string(items.size()) + ")");
    {
        commands::Transaction layers("Add movie layers");
        layers.emplace<commands::AddDroppedLayers>(f.composition(), items,
                                                   commands::LayerDropPlacement{});
        f.execute(std::move(layers), "add the sixteen movie layers");
    }
    // Eight layers span the composition; the other eight end after frames 2, 3, ... 9, so the
    // frames read sixteen sources at first and eight at the end.
    const auto last = f.lastFrame();
    std::vector<document::LayerId> layerIds;
    for (const auto& boundary : f.main(currentSnapshot(f.session)).graph().layerOutputs())
        layerIds.push_back(boundary.layerId);
    expect(layerIds.size() == 16, "sixteen layers (" + std::to_string(layerIds.size()) + ")");
    for (std::size_t index = 8; index < layerIds.size(); ++index) {
        commands::Transaction trim("Trim layer");
        trim.emplace<commands::SetLayerRange>(
            f.composition(), layerIds[index], f.frameTime(0),
            f.frameTime(static_cast<std::int64_t>(index) - 5));
        f.execute(std::move(trim), "trim a layer");
    }
    testSnapshotWaitAndRelease(f, folder, last);
    document::ExportOutputSettings output;
    output.format = ExportFormat::ProResMov;
    output.codecProfile = "hq";
    output.includeAudio = false;
    const auto finished = f.run(f.addJob(output, "Exports/layers.mov"), "sixteen movie layers");
    expect(published(finished), "sixteen movie layers: the export completes and publishes");
    if (published(finished)) {
        const auto movie = folder / "Exports" / "layers.mov";
        expect(std::filesystem::exists(movie) && std::filesystem::file_size(movie) > 0,
               "sixteen movie layers: the movie is written");
        std::cout << "  sixteen movie layers: " << last + 1 << " frames published\n";
    }
}
} // namespace

int main() {
    try {
        testSixteenMovieLayersExport();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "export movie layers: ok\n";
    return failures == 0 ? 0 : 1;
}
