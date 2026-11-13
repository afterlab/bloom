// #718: a scripted or headless render reads a source the local mirror keeps exactly once. This
// test spans scripting and the media cache, so it lives here and not in either module's tests.
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/media/cache/source_mirror.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/platform/source_store.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/json_script.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting {
namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::vector<char> bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<std::string> names(const std::filesystem::path& folder) {
    std::vector<std::string> list;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        if (entry.is_regular_file())
            list.push_back(entry.path().filename().string());
    std::ranges::sort(list);
    return list;
}

// #718: a scripted or headless render reads a source the local mirror keeps exactly once. The
// render's evaluators are made inside the run, so they find the mirror as the process's source
// store; with the device disabled every frame takes the export's CPU path, whose evaluator lives
// for that export only. A second session (new evaluators, as after a restart) renders the same
// bytes without reading the source.
void testRendersReadSlowSourcesOnce() {
    namespace cache = media::cache;
    const auto root = std::filesystem::temp_directory_path() / "bloom-render-mirror";
    std::filesystem::remove_all(root);
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    runtime::TaskScheduler scheduler(config);
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false;
    const auto provider = host::GpuExportProvider::create(options);
    // The plate: one small frame rendered from a generated composition.
    std::filesystem::path plate;
    {
        auto created =
            Session::createNew("Plate", "Composition", core::RationalTime::fromInteger(1));
        expect(static_cast<bool>(created), "plate session");
        if (!created)
            return;
        auto session = std::move(created).takeSession();
        const auto script = parseScriptJson(
            R"([{"op":"bloom.layer.add-solid","args":{"composition":1,"name":"Solid","color":[0.2,0.6,0.9,1],"position":[960,540]}}])");
        expect(static_cast<bool>(script), "plate script parses");
        if (!script)
            return;
        for (const auto& transaction : script.transactions())
            expect(session->executeJsonOperation(transaction.operation, transaction.arguments)
                       .succeeded(),
                   "plate script applies");
        std::filesystem::create_directories(root / "plates");
        const auto made = Render::run(
            *session, scheduler, compiler,
            RenderRequest{.composition =
                              session->snapshot().project().compositions().front().id(),
                          .frame = 0,
                          .range = std::nullopt,
                          .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                          .destination = root / "plates" / "plate.png"},
            root / "scratch-plate", provider);
        expect(made.succeeded, "the plate renders: " + made.diagnostic);
        const auto files = names(root / "plates");
        expect(files.size() == 1, "one plate file");
        if (!made.succeeded || files.size() != 1)
            return;
        plate = root / "plates" / files.front();
    }
    cache::SourceMirrorConfig mirrorConfig;
    mirrorConfig.rootDirectory = root / "mirror";
    // The scratch folder is fast local storage: the preference stands in for a network share.
    mirrorConfig.policy.preference = cache::LocalCopyPreference::Always;
    const auto mirror = std::make_shared<cache::SourceMirror>(mirrorConfig);
    mirror->flush();
    platform::setProcessSourceStore(mirror);
    const auto sourceReads = [&] {
        const auto statistics = mirror->statistics();
        return statistics.networkReads + statistics.otherSourceReads;
    };
    // One still and one two-frame range from a new session that imports the plate.
    const auto render = [&](const std::string& label) -> std::vector<std::vector<char>> {
        auto created =
            Session::createNew("Mirror", "Composition", core::RationalTime::fromInteger(1));
        if (!created)
            return {};
        auto session = std::move(created).takeSession();
        const auto composition = session->snapshot().project().compositions().front().id();
        const auto imported = session->executeJsonOperation(
            "bloom.asset.import", {{"paths", Value{ValueArray{Value{plate.generic_string()}}}}});
        expect(imported.succeeded() && session->snapshot().project().assets().size() == 1,
               label + ": the plate imports");
        if (session->snapshot().project().assets().size() != 1)
            return {};
        const auto layer = session->executeJsonOperation(
            "bloom.layer.add-image",
            {{"composition", Value{StableId{composition.value()}}},
             {"asset",
              Value{StableId{session->snapshot().project().assets().front().id.value()}}}});
        expect(layer.succeeded(), label + ": the plate becomes a layer");
        std::vector<std::vector<char>> written;
        const auto run = [&](const std::string& folder, std::optional<std::uint64_t> frame,
                             std::optional<std::pair<std::uint64_t, std::uint64_t>> range) {
            const auto destination = root / label / folder;
            std::filesystem::create_directories(destination);
            const auto result = Render::run(
                *session, scheduler, compiler,
                RenderRequest{.composition = composition,
                              .frame = frame,
                              .range = range,
                              .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                              .destination = destination / "out.png"},
                root / ("scratch-" + label + "-" + folder), provider);
            expect(result.succeeded, label + " " + folder + " renders: " + result.diagnostic);
            for (const auto& name : names(destination))
                written.push_back(bytes(destination / name));
        };
        run("still", 0, std::nullopt);
        run("range", std::nullopt, std::pair<std::uint64_t, std::uint64_t>{0, 1});
        return written;
    };
    const auto first = render("first");
    mirror->flush();
    expect(first.size() == 3, "the first pass writes a still and a two-frame range");
    expect(sourceReads() == 1 && mirror->statistics().writes == 1,
           "the first pass reads the plate from its storage once and keeps it");
    const auto wholeReads = platform::sourceReadWork().fullReads;
    const auto hitsBefore = mirror->statistics().diskHits;
    const auto second = render("second");
    platform::setProcessSourceStore(nullptr);
    expect(second == first, "a later session renders the same bytes");
    expect(sourceReads() == 1 && platform::sourceReadWork().fullReads == wholeReads,
           "a later session reads nothing from the plate's storage");
    expect(mirror->statistics().diskHits > hitsBefore,
           "a later session is served from the local copy");
}
} // namespace
} // namespace bloom::scripting

int main() {
    bloom::scripting::testRendersReadSlowSourcesOnce();
    if (bloom::scripting::failures == 0)
        std::cout << "render source mirror: ok\n";
    return bloom::scripting::failures == 0 ? 0 : 1;
}
