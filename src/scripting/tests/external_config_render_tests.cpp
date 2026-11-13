// Issue #465: every output path resolves a project-relative archive, an external archive and an
// external loose OCIO config through the supervised helper. One composition is rendered with the
// built-in ACES CG config (resolved in-process, the reference) and with the same config as a
// loose folder, an external archive and a project-relative archive (resolved in the helper). PNG,
// TIFF, flat EXR and ProRes must come out with the reference's bytes; a changed or missing config
// stops the render with its reason instead of falling back to another config.

#include "fixed_memory_budget.hpp"
#include <bloom/core/file_uri.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/export_job_runner.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/output/flat_exr_options.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/json_script.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
namespace OCIO = OCIO_NAMESPACE;

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
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.is_regular_file())
            list.push_back(entry.path().filename().string());
    std::ranges::sort(list);
    return list;
}

document::ColorSettings settingsFor(document::OcioConfigLocator locator,
                                    const document::OcioConfigPortability portability,
                                    const core::Sha256Digest& revision) {
    return {
        .schemaVersion = document::kColorSettingsSchemaVersionV1,
        .processColorSpaceId = std::string(document::kAcesCgSceneLinearColorSpaceIdV1),
        .ocioConfig = {.schemaVersion = document::kOcioConfigReferenceSchemaVersionV1,
                       .locator = std::move(locator),
                       .expectedRevision = {.algorithm = document::OcioRevisionAlgorithm::Sha256,
                                            .digest = revision},
                       .portability = portability,
                       .contextVariables = {}}};
}

struct Rendered final {
    bool succeeded = false;
    std::string diagnostic;
    std::uint64_t frames = 0;
    std::uint64_t gpuFrames = 0;
    std::uint64_t cpuFrames = 0;
    std::uint64_t payloads = 0;
};

struct Case final {
    std::string label;
    output::OutputPresetV1 preset;
    std::string file;
    std::optional<std::uint64_t> frame;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
};

const std::vector<Case>& cases() {
    static const std::vector<Case> list{
        {"png", output::OutputPresetV1::PngRgba8SrgbV1, "frame.png", 3, std::nullopt},
        {"tiff", output::OutputPresetV1::TiffRgba16SrgbV1, "frame.tiff", 3, std::nullopt},
        {"exr", output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1, "frame.exr", 3,
         std::nullopt},
        {"prores", output::OutputPresetV1::ProResMovV1, "clip.mov", std::nullopt,
         std::pair<std::uint64_t, std::uint64_t>{0, 3}},
    };
    return list;
}

// One project under `settings`, saved in `projectFolder`, rendered once per case into
// `outputFolder/<case>`.
class Project final {
  public:
    Project(const std::filesystem::path& projectFolder, const document::ColorSettings& settings,
            const std::string& label)
        : label_(label) {
        auto created =
            scripting::Session::createNew("External", "Main", core::RationalTime::fromInteger(1),
                                          *document::CompositionFormat::create(96, 54));
        expect(static_cast<bool>(created), label + ": the session opens");
        if (!created)
            return;
        session_ = std::move(created).takeSession();
        const auto script = scripting::parseScriptJson(
            R"([{"op":"bloom.layer.add-solid","args":{"composition":1,"name":"Back",)"
            R"("color":[0.05,0.2,0.6,1],"position":[48,27]}},)"
            R"({"op":"bloom.layer.add-solid","args":{"composition":1,"name":"Card",)"
            R"("color":[0.9,0.35,0.1,0.75],"width":41,"height":23,"position":[40.5,24.25]}}])");
        expect(static_cast<bool>(script), label + ": the script parses");
        if (!script)
            return;
        for (const auto& transaction : script.transactions())
            expect(session_->executeJsonOperation(transaction.operation, transaction.arguments)
                       .succeeded(),
                   label + ": the script applies");
        const auto applied = session_->hostSession()->setColorSettings(settings);
        expect(applied == host::ProjectSessionColorSettingsStatus::Updated ||
                   applied == host::ProjectSessionColorSettingsStatus::NoChange,
               label + ": the project accepts the color settings");
        std::filesystem::create_directories(projectFolder);
        const auto saved = session_->saveAs(projectFolder / "project.bloom");
        expect(saved.succeeded(), label + ": the project saves: " + saved.message);
    }

    [[nodiscard]] Rendered render(const Case& item, const std::filesystem::path& outputFolder,
                                  runtime::TaskScheduler& scheduler,
                                  const runtime::SnapshotCompiler& compiler,
                                  const std::shared_ptr<host::GpuExportProvider>& provider) const {
        if (!session_)
            return {};
        std::filesystem::create_directories(outputFolder / item.label);
        const auto result = scripting::Render::run(
            *session_, scheduler, compiler,
            scripting::RenderRequest{.composition =
                                         session_->snapshot().project().compositions().front().id(),
                                     .frame = item.frame,
                                     .range = item.range,
                                     .preset = item.preset,
                                     .destination = outputFolder / item.label / item.file},
            outputFolder / ("scratch-" + item.label), provider);
        return {result.succeeded,          result.diagnostic,
                result.publishedFrames,    result.gpuEvaluatedFrames,
                result.cpuReferenceFrames, result.gpuTransferredPayloads};
    }

  private:
    std::string label_;
    std::unique_ptr<scripting::Session> session_;
};

// `device` renders on a real Vulkan device with the packaged shader tools: the external config's
// display transform must then run on the device too, from the program the helper reflected.
// Returns false when that environment is absent and nothing was proven.
bool run(const bool device, const bool requireDevice) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("bloom-external-config-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "loose");
    std::filesystem::create_directories(root / "relative" / "color");

    // The fixtures: the built-in ACES CG config as a loose folder and as an archive.
    try {
        // OpenColorIO names the built-in by its URI without the scheme.
        const std::string builtInName(
            document::kAcesCgConfigUriV1.substr(std::string_view("ocio://").size()));
        const auto builtIn = OCIO::Config::CreateFromBuiltinConfig(builtInName.c_str());
        std::ostringstream text;
        builtIn->serialize(text);
        std::ofstream(root / "loose" / "config.ocio", std::ios::binary) << text.str();
        const auto loose =
            OCIO::Config::CreateFromFile((root / "loose" / "config.ocio").string().c_str());
        std::ofstream archive(root / "shot.ocioz", std::ios::binary);
        loose->archive(archive);
    } catch (const std::exception& error) {
        expect(false, std::string("the fixture configs are written: ") + error.what());
        return true;
    }
    std::filesystem::copy_file(root / "shot.ocioz", root / "relative" / "color" / "shot.ocioz");

    // Through the one helper that percent-encodes (#665).
    const auto looseUri =
        bloom::core::fileUriFromAbsolutePath(root / "loose" / "config.ocio").value();
    const auto archiveUri = bloom::core::fileUriFromAbsolutePath(root / "shot.ocioz").value();
    const auto revisionOf = [](document::OcioConfigLocator locator,
                               const std::filesystem::path& projectDirectory) {
        document::OcioConfigReference reference;
        reference.locator = std::move(locator);
        return host::ocioConfigContentRevision(reference, projectDirectory);
    };
    const auto builtInRevision = revisionOf(
        document::BuiltInOcioConfigLocator{std::string(document::kAcesCgConfigUriV1)}, {});
    expect(builtInRevision.has_value(), "the built-in ACES config is available");
    if (!builtInRevision)
        return true;
    const auto looseRevision = revisionOf(document::ExternalOcioConfigLocator{looseUri}, {})
                                   .value_or(core::Sha256Digest{});
    const auto archiveRevision =
        revisionOf(document::ExternalOciozLocator{archiveUri}, {}).value_or(core::Sha256Digest{});
    const auto relativeRevision =
        revisionOf(document::ProjectRelativeOciozLocator{"color/shot.ocioz"}, root / "relative")
            .value_or(core::Sha256Digest{});
    expect(looseRevision != core::Sha256Digest{} && archiveRevision != core::Sha256Digest{} &&
               relativeRevision != core::Sha256Digest{} && archiveRevision != relativeRevision,
           "each external config reports its content revision");

    auto config = runtime::TaskSchedulerConfig::defaults();
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    runtime::TaskScheduler scheduler(config);
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
    options.enabled = false;
    const char* const loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (device) {
        if (loader == nullptr || *loader == '\0') {
            expect(!requireDevice, "device: --require-device needs BLOOM_TEST_VULKAN_LOADER");
            return false;
        }
        options.enabled = true;
        options.loaderPath = loader;
        options.ocioResolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
    }
    const auto provider = host::GpuExportProvider::create(options);
    const auto retire = [&] {
        host::releaseExternalOcioConfigs();
        expect(provider->shutdownAndWait(std::chrono::seconds(10)), "the provider retires");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    };
    if (device) {
        provider->prepare(scheduler);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!provider->prepared() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!provider->deviceAvailable() || !provider->gpuDisplayPreparationAvailable()) {
            expect(!requireDevice,
                   "device: a Vulkan device and the packaged shader tools are required");
            retire();
            return false;
        }
    }

    const Project reference(
        root / "reference",
        settingsFor(document::BuiltInOcioConfigLocator{std::string(document::kAcesCgConfigUriV1)},
                    document::OcioConfigPortability::BuiltIn, *builtInRevision),
        "built-in");
    struct External final {
        std::string label;
        std::filesystem::path folder;
        document::ColorSettings settings;
    };
    const std::vector<External> externals{
        {"loose", root / "project-loose",
         settingsFor(document::ExternalOcioConfigLocator{looseUri},
                     document::OcioConfigPortability::External, looseRevision)},
        {"archive", root / "project-archive",
         settingsFor(document::ExternalOciozLocator{archiveUri},
                     document::OcioConfigPortability::External, archiveRevision)},
        // The archive sits beside the project file, named by its project-relative path.
        {"relative", root / "relative",
         settingsFor(document::ProjectRelativeOciozLocator{"color/shot.ocioz"},
                     document::OcioConfigPortability::ProjectRelative, relativeRevision)},
    };

    std::vector<bool> available;
    std::vector<Rendered> referenceRuns;
    for (const auto& item : cases()) {
        const auto rendered =
            reference.render(item, root / "out-reference", scheduler, compiler, provider);
        available.push_back(rendered.succeeded);
        referenceRuns.push_back(rendered);
        if (!rendered.succeeded)
            std::cout << "NOTE: " << item.label << " unavailable here: " << rendered.diagnostic
                      << '\n';
    }
    expect(available[0] && available[2], "the reference renders PNG and flat EXR");

    for (const auto& external : externals) {
        const Project project(external.folder, external.settings, external.label);
        for (std::size_t index = 0; index < cases().size(); ++index) {
            const auto& item = cases()[index];
            const auto out = root / ("out-" + external.label);
            const auto rendered = project.render(item, out, scheduler, compiler, provider);
            const auto label = external.label + " " + item.label;
            if (!available[index]) {
                // TIFF and ProRes need the media worker; both configs refuse together.
                expect(!rendered.succeeded, label + ": refused where the reference is refused");
                continue;
            }
            expect(rendered.succeeded,
                   label + ": renders through the helper: " + rendered.diagnostic);
            if (device) {
                // No frame and no display transform may take the CPU reference path.
                expect(rendered.gpuFrames > 0 && rendered.cpuFrames == 0 &&
                           rendered.payloads == referenceRuns[index].payloads &&
                           rendered.gpuFrames == referenceRuns[index].gpuFrames,
                       label + ": every frame and its display transform ran on the device (gpu " +
                           std::to_string(rendered.gpuFrames) + " vs " +
                           std::to_string(referenceRuns[index].gpuFrames) + ", cpu " +
                           std::to_string(rendered.cpuFrames) + ", payloads " +
                           std::to_string(rendered.payloads) + " vs " +
                           std::to_string(referenceRuns[index].payloads) + ")");
            }
            const auto mine = names(out / item.label);
            const auto theirs = names(root / "out-reference" / item.label);
            expect(!theirs.empty() && mine == theirs, label + ": the same files are written");
            for (const auto& name : theirs)
                expect(bytes(out / item.label / name) ==
                           bytes(root / "out-reference" / item.label / name),
                       label + ": bytes equal the built-in reference for " + name);
        }
    }

    // Flat EXR to another color space of the config (the ACES interchange hand-off the export
    // queue offers): the conversion is built from the external config and equals the built-in's.
    {
        const auto convert = [](const std::string& location, const core::Sha256Digest& revision)
            -> std::optional<std::vector<std::array<float, 4>>> {
            const runtime::EvaluationColorIntent intent{
                .workingColorSpaceId = document::kAcesCgSceneLinearColorSpaceIdV1,
                .ocioConfigRevision = revision,
                .ocioConfigUri = location};
            const auto prepared = output::PreparedFlatExrOutputV1::prepare(
                intent, {.outputColorSpaceId = "ACES2065-1"});
            if (!prepared)
                return std::nullopt;
            std::vector<render::Rgba32f> source;
            for (const auto& pixel : std::vector<std::array<float, 4>>{{0.18F, 0.18F, 0.18F, 1.0F},
                                                                       {0.4F, 0.1F, 0.05F, 0.5F},
                                                                       {2.0F, 1.0F, 0.5F, 1.0F}}) {
                const auto made =
                    render::Rgba32f::fromPremultiplied(pixel[0], pixel[1], pixel[2], pixel[3]);
                if (!made)
                    return std::nullopt;
                source.push_back(*made.value());
            }
            std::vector<std::array<float, 4>> target(source.size());
            if (!prepared->apply(source, target))
                return std::nullopt;
            return target;
        };
        const auto handoff = convert(std::string(document::kAcesCgConfigUriV1), *builtInRevision);
        expect(handoff.has_value() && (*handoff)[2][1] != 1.0F,
               "flat EXR hand-off: the built-in reference converts to ACES2065-1");
        for (const auto& external : externals) {
            const auto location =
                host::ocioConfigIntentLocation(external.settings.ocioConfig, external.folder);
            const auto converted =
                convert(location, external.settings.ocioConfig.expectedRevision.digest);
            expect(converted.has_value() && handoff.has_value() && *converted == *handoff,
                   external.label + " flat EXR hand-off: the conversion equals the built-in's");
        }
    }

    // A config that changed after the project pinned it stops the render; nothing is published
    // and no other config stands in.
    {
        const Project project(root / "project-changed", externals[0].settings, "changed");
        std::ofstream(root / "loose" / "config.ocio", std::ios::binary | std::ios::app)
            << "\n# edited after the project pinned its revision\n";
        const auto rendered =
            project.render(cases()[0], root / "out-changed", scheduler, compiler, provider);
        expect(!rendered.succeeded && names(root / "out-changed" / "png").empty(),
               "changed: a config that no longer matches its revision publishes nothing (" +
                   rendered.diagnostic + ")");
        std::filesystem::remove(root / "loose" / "config.ocio");
        const auto missing =
            project.render(cases()[0], root / "out-missing", scheduler, compiler, provider);
        expect(!missing.succeeded && names(root / "out-missing" / "png").empty(),
               "missing: a config that is gone publishes nothing (" + missing.diagnostic + ")");
    }

    retire();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool device = false;
    bool requireDevice = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        device = device || argument == "--device" || argument == "--require-device";
        requireDevice = requireDevice || argument == "--require-device";
    }
    const bool ran = run(device, requireDevice);
    if (failures == 0 && !ran) {
        std::cout << "NOTE: no Vulkan device or packaged shader tools; device half not run\n";
        return 77;
    }
    if (failures == 0)
        std::cout << "external config render tests passed\n";
    return failures == 0 ? 0 : 1;
}
