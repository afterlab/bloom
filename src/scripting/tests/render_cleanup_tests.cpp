// Issue #588: a render removes the scratch folder it made for itself on success, failure and
// cancellation, leaves a caller-supplied folder and another render's folder alone, and a failure
// from a media file that changed after import names the asset and says what to do without any
// internal stage or code in the user-facing sentence.

#include "fixed_memory_budget.hpp"
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

// A 1x1 RGBA PNG.
constexpr std::array<unsigned char, 70> kPixel{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0x64, 0x60, 0xf8, 0x5f, 0x0f, 0x00, 0x02, 0x87, 0x01, 0x80, 0xeb, 0x47,
    0xba, 0x92, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

[[nodiscard]] std::uint32_t crc32(const std::string& bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

// The same pixel; a non-empty comment adds a text chunk, so the file's bytes and digest change.
void writePixel(const std::filesystem::path& path, const std::string& text = {}) {
    std::string bytes(reinterpret_cast<const char*>(kPixel.data()), kPixel.size());
    if (!text.empty()) {
        const std::string body = "tEXt" + std::string("Comment") + '\0' + text;
        std::string chunk;
        const auto length = static_cast<std::uint32_t>(body.size() - 4);
        for (const int shift : {24, 16, 8, 0})
            chunk += static_cast<char>((length >> shift) & 0xffU);
        chunk += body;
        const auto crc = crc32(body);
        for (const int shift : {24, 16, 8, 0})
            chunk += static_cast<char>((crc >> shift) & 0xffU);
        bytes.insert(33, chunk);
    }
    std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<long>(bytes.size()));
}

struct Folder final {
    std::filesystem::path path;
    explicit Folder(const std::string& name) {
        std::mt19937_64 random{std::random_device{}()};
        path = std::filesystem::temp_directory_path() /
               ("bloom-render-cleanup-" + name + "-" + std::to_string(random()));
        std::filesystem::create_directories(path);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
    ~Folder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct Project final {
    std::unique_ptr<scripting::Session> session;
    document::CompositionId composition;
};

// A saved project with one image layer over a 1x1 plate in `media`.
[[nodiscard]] Project makeProject(const std::filesystem::path& media, const std::string& plate) {
    Project project;
    auto created = scripting::Session::createNew(
        "Cleanup", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(16, 16, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    if (!created)
        std::abort();
    project.session = std::move(created).takeSession();
    project.composition = project.session->snapshot().project().compositions().front().id();
    expect(project.session->saveAs(media / "cleanup.bloom").succeeded(), "the project is saved");
    writePixel(media / plate);
    const auto run = [&]<typename Operation>(auto&&... arguments) {
        commands::Transaction transaction("Fixture", project.session->snapshot().revision());
        transaction.emplace<Operation>(std::forward<decltype(arguments)>(arguments)...);
        expect(project.session->execute(std::move(transaction)).succeeded(),
               "a fixture command applies");
    };
    run.template operator()<commands::ImportAssets>(std::vector{media / plate}, media);
    const auto asset = project.session->snapshot().project().assets().front().id;
    run.template operator()<commands::AddImageLayer>(project.composition, asset);
    return project;
}

[[nodiscard]] std::shared_ptr<host::GpuExportProvider> cpuProvider() {
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.hostMemorySampler = bloom::test_support::fixedMemoryBudgetSample;
    options.enabled = false;
    return host::GpuExportProvider::create(options);
}

[[nodiscard]] std::vector<std::string> entries(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        names.push_back(entry.path().filename().string());
    return names;
}

[[nodiscard]] bool hasScratch(const std::filesystem::path& folder) {
    for (const auto& name : entries(folder))
        if (name.starts_with(".bloom-export-scratch"))
            return true;
    return false;
}

[[nodiscard]] scripting::RenderResult render(Project& project, const std::filesystem::path& target,
                                             const std::shared_ptr<host::GpuExportProvider>& gpu,
                                             std::function<bool()> cancelled = {},
                                             const std::filesystem::path& scratch = {}) {
    runtime::TaskScheduler scheduler;
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    return scripting::Render::run(*project.session, scheduler, compiler,
                                  {.composition = project.composition,
                                   .frame = std::uint64_t{0},
                                   .range = std::nullopt,
                                   .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                   .destination = target,
                                   .cancelled = std::move(cancelled)},
                                  scratch, gpu);
}

void successLeavesNothing() {
    Folder media("ok-media");
    Folder out("ok-out");
    auto project = makeProject(media.path, "plate.png");
    const auto gpu = cpuProvider();
    const auto result = render(project, out.path / "still.png", gpu);
    expect(result.succeeded, "the render publishes: " + result.diagnostic);
    expect(entries(out.path) == std::vector<std::string>{"still.png"},
           "only the published file is left in the destination folder");
    expect(gpu->shutdownAndWait(std::chrono::seconds(10)), "the provider retires");
}

void cancelLeavesNothing() {
    Folder media("cancel-media");
    Folder out("cancel-out");
    auto project = makeProject(media.path, "plate.png");
    const auto gpu = cpuProvider();
    std::atomic<int> polls{0};
    // The first check (before any work) passes; every later one asks to stop.
    const auto result =
        render(project, out.path / "still.png", gpu, [&polls] { return polls.fetch_add(1) >= 1; });
    expect(!hasScratch(out.path), "a cancelled or stopped render leaves no scratch folder");
    if (!result.succeeded)
        expect(result.diagnosticCode == scripting::RenderDiagnosticCode::Cancelled,
               "a stopped render reports cancellation: " + result.diagnostic);
    else
        expect(entries(out.path) == std::vector<std::string>{"still.png"},
               "a render that finished before the stop left only its file");
    expect(gpu->shutdownAndWait(std::chrono::seconds(10)), "the provider retires");
}

void changedMediaNamesTheAsset() {
    Folder media("changed-media");
    Folder out("changed-out");
    auto project = makeProject(media.path, "plate.png");
    // The file is replaced after import: the project still holds the old digest.
    writePixel(media.path / "plate.png", "replaced");
    const auto gpu = cpuProvider();
    const auto result = render(project, out.path / "still.png", gpu);
    expect(!result.succeeded, "a render of changed media fails");
    expect(result.diagnostic.find("\"plate\"") != std::string::npos &&
               result.diagnostic.find("Assets") != std::string::npos,
           "the failure names the asset and says where to fix it: " + result.diagnostic);
    expect(result.diagnostic.find("stage") == std::string::npos &&
               result.diagnostic.find("code") == std::string::npos &&
               result.diagnostic.find("Diagnostic") == std::string::npos,
           "the failure has no internal stage or code: " + result.diagnostic);
    expect(entries(out.path).empty(), "a failed render leaves nothing in the destination folder");
    expect(gpu->shutdownAndWait(std::chrono::seconds(10)), "the provider retires");
}

void callerFolderStays() {
    Folder media("caller-media");
    Folder out("caller-out");
    Folder scratch("caller-scratch");
    auto project = makeProject(media.path, "plate.png");
    const auto gpu = cpuProvider();
    const auto folder = scratch.path / "mine";
    const auto result = render(project, out.path / "still.png", gpu, {}, folder);
    expect(result.succeeded, "the render publishes: " + result.diagnostic);
    expect(std::filesystem::is_directory(folder), "a folder the caller passed in stays");
    expect(!hasScratch(out.path), "no second scratch folder appears beside the destination");
    expect(gpu->shutdownAndWait(std::chrono::seconds(10)), "the provider retires");
}

void concurrentRendersShareAFolder() {
    Folder out("concurrent-out");
    // A scratch folder that belongs to a render still in flight must survive another render.
    const auto foreign = out.path / ".bloom-export-scratch-foreign";
    std::filesystem::create_directories(foreign);
    std::ofstream(foreign / "in-flight.tmp") << "x";

    constexpr int kRenders = 4;
    std::deque<Folder> medias;
    std::vector<Project> projects;
    for (int i = 0; i < kRenders; ++i) {
        medias.emplace_back("concurrent-media-" + std::to_string(i));
        projects.push_back(makeProject(medias.back().path, "plate.png"));
    }
    const auto gpu = cpuProvider();
    std::vector<scripting::RenderResult> results(kRenders);
    std::vector<std::thread> threads;
    for (int i = 0; i < kRenders; ++i)
        threads.emplace_back([&, i] {
            results[static_cast<std::size_t>(i)] =
                render(projects[static_cast<std::size_t>(i)],
                       out.path / ("still-" + std::to_string(i) + ".png"), gpu);
        });
    for (auto& thread : threads)
        thread.join();
    for (int i = 0; i < kRenders; ++i)
        expect(results[static_cast<std::size_t>(i)].succeeded,
               "concurrent render " + std::to_string(i) +
                   " publishes: " + results[static_cast<std::size_t>(i)].diagnostic);
    expect(std::filesystem::exists(foreign / "in-flight.tmp"),
           "another render's scratch folder is not removed");
    std::size_t left = 0;
    for (const auto& name : entries(out.path))
        left += name.starts_with(".bloom-export-scratch") ? 1 : 0;
    expect(left == 1, "only the foreign scratch folder remains beside the destination");
    expect(gpu->shutdownAndWait(std::chrono::seconds(10)), "the provider retires");
}

} // namespace

int main() {
    successLeavesNothing();
    cancelLeavesNothing();
    changedMediaNamesTheAsset();
    callerFolderStays();
    concurrentRendersShareAFolder();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
