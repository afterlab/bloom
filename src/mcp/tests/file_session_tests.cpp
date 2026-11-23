// #775, #783 and #784: projectFile sessions driven through McpServerCore::handle().
//
// #783: a projectFile session's render or export runs on that file's own export queue, so the
// call reports progress, export.status shows the job, export.stop stops it and export.wait reads
// it, with the same projectFile; a request that is cancelled stops its job; project.close does not
// stop a running job silently.
// #784: the export-queue route reports the same preservation report as the scripted route
// (scripting::Render::run) for a movie and an image sequence of the same project.
// #775: a call whose projectFile is the project open in the window attaches to that live project:
// the edit lands in the window's document, and the answer says whether the window's copy differs
// from the file (a stale window included) and how to reload it.

#include "unique_temp_dir.hpp"
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace bloom;
using test_support::UniqueTempDir;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

bool contains(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::string firstMatch(const std::string& text, const std::string& pattern) {
    std::smatch match;
    return std::regex_search(text, match, std::regex(pattern)) && match.size() > 1 ? match[1].str()
                                                                                   : std::string{};
}

std::string quoted(const std::filesystem::path& path) { return '"' + path.generic_string() + '"'; }

// The server core over a headless session of its own (the stdio host's shape). The file sessions'
// GPU provider is left out, so their queues render on the CPU reference path.
struct Headless final {
    std::unique_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::move(created).takeSession();
    }();
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    std::atomic<int> nextId{1};

    Headless() {
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(*session),
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = true},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .captureDirectory = {},
            .ui = {},
            .profilerSnapshot = {},
            .serverVersion = "test"});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"core","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    ~Headless() {
        core.reset();
        scheduler.beginShutdown();
    }
    Headless(const Headless&) = delete;
    Headless& operator=(const Headless&) = delete;

    std::string send(const std::string& message, const mcp::McpRequestControl& control = {}) {
        const auto handled = core->handle(*protocol, message, control);
        return handled.response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments,
                     const mcp::McpRequestControl& control = {}) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                        R"(,"method":"tools/call","params":{"name":")" + tool +
                        R"(","arguments":)" + arguments + "}}",
                    control);
    }
};

// A saved project file with one composition of `frames` frames at 64x36 and one solid layer.
void makeProjectFile(Headless& server, const std::filesystem::path& path, const int frames) {
    {
        auto created = scripting::Session::createNew("Shot");
        if (!created)
            throw std::runtime_error("project");
        auto session = std::move(created).takeSession();
        if (!session->saveAs(path).succeeded())
            throw std::runtime_error("save");
    }
    const auto file = R"("projectFile":)" + quoted(path);
    const auto read = server.call("query", "{" + file + R"(,"kind":"project"})");
    const auto revision = firstMatch(read, R"re("revision":(\d+))re");
    const auto edited = server.call(
        "transact", "{" + file + R"(,"expectedRevision":)" + revision +
                        R"(,"operations":[{"op":"bloom.composition.set-duration","args":{)"
                        R"("composition":1,"duration":)" +
                        std::to_string(frames) +
                        R"(}},{"op":"bloom.composition.set-format","args":{"composition":1,)"
                        R"("width":64,"height":36}},{"op":"bloom.layer.add-solid","args":{)"
                        R"("composition":1,"name":"Card","color":[0.8,0.2,0.1,1]}}]})");
    expect(contains(edited, R"("isError":false)"), "the project file is built: " + edited);
    const auto saved = server.call("project.save", "{" + file + "}");
    expect(contains(saved, R"("saved":true)"), "the project file is saved: " + saved);
    const auto closed = server.call("project.close", "{" + file + "}");
    expect(contains(closed, R"("closed":true)"), "and closed: " + closed);
}

// #783: progress, export.status, export.stop, export.wait and request cancellation for a render
// that runs on a projectFile session's own queue, and project.close with a running job.
void fileSessionJobs() {
    const UniqueTempDir folder("bloom-mcp-file-jobs");
    Headless server;
    const auto path = folder / "long.bloom";
    makeProjectFile(server, path, 4000);
    const auto file = R"("projectFile":)" + quoted(path);
    std::filesystem::create_directories(folder / "stopped");

    std::atomic<int> progress{0};
    mcp::McpRequestControl control;
    control.progress = [&](const mcp::McpProgress&) { ++progress; };
    auto rendering = std::async(std::launch::async, [&] {
        return server.call(
            "render",
            "{" + file + R"(,"composition":1,"preset":"builtin:png-sequence","destination":)" +
                quoted(folder / "stopped" / "seq.png") + "}",
            control);
    });
    // The file's export.status answers while its render runs, and shows the job.
    std::string job;
    std::string status;
    for (int attempt = 0; attempt < 6000 && job.empty(); ++attempt) {
        status = server.call("export.status", "{" + file + "}");
        job = firstMatch(status, R"re("activeJob":"(\d+)")re");
        if (job.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(!job.empty(), "export.status with the projectFile shows the running job: " + status);
    const auto open = server.call("export.status", "{}");
    expect(!contains(open, job.empty() ? std::string("-") : job),
           "the open project's queue does not hold the file's job: " + open);
    const auto stopped = server.call("export.stop", "{" + file + R"(,"job":")" + job + R"("})");
    expect(contains(stopped, R"("accepted":true)"),
           "export.stop with the projectFile stops the file's job: " + stopped);
    const auto rendered = rendering.get();
    expect(contains(rendered, R"("isError":true)") && contains(rendered, "the job stopped"),
           "the render reports that its job stopped: " + rendered.substr(0, 600));
    expect(progress.load() > 0, "the render reported progress while it ran");
    const auto waited = server.call("export.wait", "{" + file + R"(,"job":")" + job + R"("})");
    expect(contains(waited, R"("finished":true)") && contains(waited, R"("state":"stopped")"),
           "export.wait with the projectFile reads the stopped job: " + waited.substr(0, 600));
    const auto log = server.call("export.log", "{" + file + R"(,"job":")" + job + R"("})");
    expect(contains(log, R"("entries":[{)"), "export.log reads its run: " + log.substr(0, 400));
    const auto unknown = server.call("export.wait", R"({"job":")" + job + R"("})");
    expect(contains(unknown, R"("isError":true)") && !contains(unknown, R"("finished":true)"),
           "without the projectFile the job is not the open project's: " + unknown);

    // A cancelled request stops its job.
    std::stop_source cancel;
    mcp::McpRequestControl cancellable;
    cancellable.stop = cancel.get_token();
    auto cancelled = std::async(std::launch::async, [&] {
        return server.call(
            "render",
            "{" + file +
                R"(,"composition":1,"preset":"builtin:png-sequence","overwrite":"replace","destination":)" +
                quoted(folder / "stopped" / "seq.png") + "}",
            cancellable);
    });
    std::string second;
    for (int attempt = 0; attempt < 6000 && second.empty(); ++attempt) {
        second = firstMatch(server.call("export.status", "{" + file + "}"),
                            R"re("activeJob":"(\d+)")re");
        if (second.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    cancel.request_stop();
    const auto answer = cancelled.get();
    expect(contains(answer, "cancelled"),
           "a cancelled request stops its job: " + answer.substr(0, 400));
    const auto idle = server.call("export.status", "{" + file + "}");
    expect(contains(idle, R"("activeJob":null)"),
           "and the file's queue is idle: " + idle.substr(0, 400));

    // A saved job runs through export.start; project.close does not stop it silently.
    const auto read = server.call("query", "{" + file + R"(,"kind":"project"})");
    const auto added = server.call(
        "transact", "{" + file + R"(,"expectedRevision":)" +
                        firstMatch(read, R"re("revision":(\d+))re") +
                        R"(,"operations":[{"op":"bloom.export.add-job","args":{"composition":1,)"
                        R"("outputs":[{"preset":"builtin:png-sequence","destination":{)"
                        R"("portability":"project-relative","path":"saved/seq.png",)"
                        R"("overwrite":"replace"}}]}}]})");
    const auto saved = firstMatch(added, R"re("name":"exportJob","id":"?(\d+))re");
    expect(!saved.empty(), "the job is added: " + added.substr(0, 400));
    const auto started =
        server.call("export.start", "{" + file + R"(,"jobs":[")" + saved + R"("]})");
    expect(contains(started, R"("accepted":true)"),
           "export.start with the projectFile starts the file's saved job: " + started);
    const auto refused = server.call("project.close", "{" + file + R"(,"discardChanges":true})");
    expect(contains(refused, "are running") && contains(refused, "stopJobs"),
           "project.close does not stop a running job silently: " + refused);
    const auto closed =
        server.call("project.close", "{" + file + R"(,"discardChanges":true,"stopJobs":true})");
    expect(contains(closed, R"("closed":true)"), "with stopJobs it stops and closes: " + closed);
}

// #784: the queue route and Render::run report the same for a movie and an image sequence.
void preservationReportParity() {
    const UniqueTempDir folder("bloom-mcp-report-parity");
    Headless server;
    const auto path = folder / "shot.bloom";
    makeProjectFile(server, path, 3);
    const auto file = R"("projectFile":)" + quoted(path);
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false;
    auto provider = host::GpuExportProvider::create(std::move(options));
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto opened = scripting::Session::open(path);
    if (!opened)
        throw std::runtime_error("open");
    auto scripted = std::move(opened).takeSession();
    struct Case final {
        const char* name;
        const char* preset;
        output::OutputPresetV1 renderPreset;
        const char* destination;
    };
    for (const auto& item : {Case{"movie", "builtin:prores-422hq-mov",
                                  output::OutputPresetV1::ProResMovV1, "clip.mov"},
                             Case{"sequence", "builtin:png-sequence",
                                  output::OutputPresetV1::PngRgba8SrgbV1, "seq.png"}}) {
        std::filesystem::create_directories(folder / "queued" / item.name);
        std::filesystem::create_directories(folder / "scripted" / item.name);
        const auto queued = server.call(
            "export", "{" + file + R"(,"composition":1,"first":0,"last":2,"preset":")" +
                          item.preset + R"(","destination":)" +
                          quoted(folder / "queued" / item.name / item.destination) + "}");
        const auto report = firstMatch(queued, R"re("preservationReport":"([^"]*)")re");
        const auto run = scripting::Render::run(
            *scripted, server.scheduler, compiler,
            {.composition = document::CompositionId::fromRaw(1),
             .frame = std::nullopt,
             .range = std::pair<std::uint64_t, std::uint64_t>(0, 2),
             .preset = item.renderPreset,
             .destination = folder / "scripted" / item.name / item.destination,
             .replaceExisting = true},
            {}, provider);
        expect(run.succeeded,
               std::string(item.name) + " renders on the scripted route: " + run.diagnostic);
        expect(!report.empty() && report == run.preservationReport,
               std::string(item.name) + ": the queue route reports what Render::run reports: '" +
                   report + "' and '" + run.preservationReport + "' from " + queued.substr(0, 400));
        expect(report.starts_with("preset=") && contains(report, "; exact-facets="),
               std::string(item.name) +
                   ": the report names the preset and the exact facets: " + report);
    }
    while (!provider->shutdownAndWait(std::chrono::seconds(10))) {
    }
}

// The desktop host: a live session the test thread owns as the window does.
struct Live final {
    std::shared_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew("Window");
        if (!created)
            throw std::runtime_error("session");
        return std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    }();
    std::shared_ptr<scripting::EventStream> events = std::make_shared<scripting::EventStream>();
    scripting::LiveSessionHost host;
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    std::optional<std::filesystem::path> projectPath;
    std::atomic<int> nextId{1};

    Live() {
        bind(std::nullopt);
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(host),
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = false},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .captureDirectory = {},
            .ui = {},
            .profilerSnapshot = {},
            .serverVersion = "test"});
        protocol = core->openSession();
        static_cast<void>(core->handle(*protocol,
                                       R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                                       R"("protocolVersion":"2025-11-25","capabilities":{},)"
                                       R"("clientInfo":{"name":"core","version":"1"}}})",
                                       {}));
        static_cast<void>(core->handle(
            *protocol, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", {}));
    }
    ~Live() {
        core.reset();
        scheduler.beginShutdown();
    }
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;

    scripting::LiveSessionState mirror() const {
        return {.renderSource = session->renderSourceSnapshot(),
                .context = session->operationContext(),
                .viewerDisplay = {},
                .viewerView = {},
                .projectPath = projectPath,
                .readOnly = false};
    }
    // Binds the window's project to `path` (a new generation), as File > Open does.
    void bind(const std::optional<std::filesystem::path>& path) {
        projectPath = path;
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     mirror());
    }
    // One request on a worker while this thread serves the window's commits.
    std::string call(const std::string& tool, const std::string& arguments) {
        const scripting::LiveSessionCommitter committer{
            .execute =
                [&](commands::Transaction transaction) {
                    auto result = session->execute(std::move(transaction));
                    host.publish(mirror());
                    return result;
                },
            .publish =
                [&](commands::PreparedTransaction& prepared) {
                    auto* stack = session->commandStack();
                    if (stack->rebaseNeeded(prepared))
                        return scripting::LivePublication{std::nullopt, session->snapshot()};
                    auto command = stack->publish(std::move(prepared));
                    host.publish(mirror());
                    return scripting::LivePublication{
                        scripting::SessionCommandResult{
                            host::ProjectSessionCommandStatus::Completed, std::move(command),
                            std::nullopt},
                        std::nullopt};
                },
            .history =
                [&](const bool redo, const commands::CommandOrigin origin) {
                    auto result = redo ? session->redo(origin) : session->undo(origin);
                    host.publish(mirror());
                    return result;
                },
            .gestureOpen = [] { return false; },
            .revision = [&] { return session->snapshot().revision(); },
            .viewerLevel = {}};
        const auto message = R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                             R"(,"method":"tools/call","params":{"name":")" + tool +
                             R"(","arguments":)" + arguments + "}}";
        auto result = std::async(std::launch::async, [&] {
            return core->handle(*protocol, message, {}).response.value_or(std::string{});
        });
        while (result.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
            static_cast<void>(host.drainOne(committer));
        return result.get();
    }
};

std::size_t windowLayers(const Live& window) {
    const auto snapshot = window.session->snapshot();
    const auto* composition =
        snapshot.project().findComposition(document::CompositionId::fromRaw(1));
    return composition == nullptr ? 0 : composition->graph().layerOutputs().size();
}

// #775: open the project in the window, edit it over MCP with its projectFile, and check the
// window's own document; a stale window is reported with the reload, never refused.
void attachToTheWindowsProject() {
    const UniqueTempDir folder("bloom-mcp-attach");
    Live window;
    const auto path = folder / "shot.bloom";
    expect(window.session->saveAs(path).succeeded(), "the window's project is saved");
    window.bind(path);
    const auto file = R"("projectFile":)" + quoted(path);

    const auto read = window.call("query", "{" + file + R"(,"kind":"project"})");
    expect(contains(read, R"("isError":false)") &&
               contains(read, R"("attachedToOpenProject":true)") &&
               contains(read, R"("fileDiffers":false)"),
           "a session for the window's file attaches to the live project: " + read);
    expect(!contains(read, "already open"), "and is not refused: " + read);
    const auto before = windowLayers(window);
    const auto edited = window.call(
        "transact", "{" + file + R"(,"expectedRevision":)" +
                        firstMatch(read, R"re("revision":(\d+))re") +
                        R"(,"operations":[{"op":"bloom.layer.add-solid","args":{"composition":1,)"
                        R"("name":"Attached","color":[0,1,0,1]}}]})");
    expect(contains(edited, R"("isError":false)") && contains(edited, R"("succeeded":true)"),
           "the edit applies: " + edited.substr(0, 400));
    // The window's own document holds the edit: there is one project, not a second copy.
    expect(windowLayers(window) == before + 1, "the window shows the edit made over MCP");
    const auto live = window.call("query", R"({"kind":"layers","composition":1})");
    expect(contains(live, "Attached"), "the live session reads it without a projectFile: " + live);
    const auto differs = window.call("query", "{" + file + R"(,"kind":"project"})");
    expect(contains(differs, R"("fileDiffers":true)") &&
               contains(differs, R"("reload":{"tool":"project.open","arguments":{"path":)" +
                                     quoted(path) + R"(,"discardChanges":true})"),
           "an unsaved edit in the window is reported with the reload: " + differs);
    expect(window.session->save().succeeded(), "the window saves");
    const auto same = window.call("query", "{" + file + R"(,"kind":"project"})");
    expect(contains(same, R"("fileDiffers":false)"), "after the save they agree: " + same);

    // A stale window: the window shows the file's name, but its copy is not the file's.
    const auto stale = folder / "stale.bloom";
    std::filesystem::copy_file(path, stale);
    window.bind(stale);
    const auto emptied =
        window.call("history", R"({"action":"undo","expectedRevision":)" +
                                   firstMatch(window.call("query", R"({"kind":"project"})"),
                                              R"re("revision":(\d+))re") +
                                   "}");
    expect(contains(emptied, R"("isError":false)"), "the window's copy goes stale: " + emptied);
    const auto staleFile = R"("projectFile":)" + quoted(stale);
    const auto staleRead = window.call("query", "{" + staleFile + R"(,"kind":"project"})");
    expect(contains(staleRead, R"("isError":false)") &&
               contains(staleRead, R"("attachedToOpenProject":true)") &&
               contains(staleRead, R"("fileDiffers":true)") &&
               contains(staleRead, "stale.bloom on disk") &&
               contains(staleRead, R"("tool":"project.open")"),
           "a stale window is reported with the reload, not refused: " + staleRead);
    const auto closed = window.call("project.close", "{" + staleFile + "}");
    expect(contains(closed, R"("closed":false)") && contains(closed, "project open in Bloom"),
           "closing the attached file leaves the window's project: " + closed);
    expect(window.projectPath == stale, "the window keeps its project");
}

} // namespace

int main() {
    try {
        fileSessionJobs();
        preservationReportParity();
        attachToTheWindowsProject();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "file sessions passed\n";
    return failures == 0 ? 0 : 1;
}
