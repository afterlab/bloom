// The shared MCP core's tool behaviour driven directly through McpServerCore::handle(), without a
// transport: the export queue's absence, and (live host) the session and UI seams of
// live_ui_port.hpp. One stdio transport check: it stops while its input is idle.

#include <bloom/document/project.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/testing/fake_export_runner.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/profiler_sampler.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <yyjson.h>

namespace {

using namespace bloom;

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
    int nextId = 1;

    explicit Headless(mcp::McpLiveUiPort ui = {},
                      std::function<bool(runtime::ProfilerSnapshot&)> profiler = {},
                      std::filesystem::path captures = {}) {
        std::shared_ptr<host::GpuExportProvider> renderProvider;
        if (!captures.empty()) {
            runtime::GpuProcessFrameEvaluatorOptions options;
            options.enabled = false;
            renderProvider = host::GpuExportProvider::create(std::move(options));
        }
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(*session),
            // No export queue is bound, as in a desktop window that has not created one.
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = true},
            .scheduler = scheduler,
            .gpuProvider = renderProvider,
            .captureDirectory = std::move(captures),
            .ui = std::move(ui),
            .profilerSnapshot = std::move(profiler),
            .serverVersion = "test",
            .limits = mcp::kMcpMessageLimits});
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

    std::string send(const std::string& message) {
        const mcp::McpRequestControl control{};
        const auto handled = core->handle(*protocol, message, control);
        return handled.response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    arguments + "}}");
    }
};

void exportWithoutQueue() {
    Headless server;
    const auto status = server.call("export.status", "{}");
    expect(contains(status, "Export isn't available in this window yet."),
           "a missing export queue says so plainly: " + status);
    expect(contains(status, "bloom.mcp.export-unavailable"),
           "a missing export queue keeps its typed code: " + status);
    expect(!contains(status, "while a project is opening"), "no false reason: " + status);
}

// The desktop host: a live session the test thread owns, with a fake McpLiveUiPort.
struct Live final {
    std::shared_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    }();
    std::shared_ptr<scripting::EventStream> events = std::make_shared<scripting::EventStream>();
    scripting::LiveSessionHost host;
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    int nextId = 1;

    // No export queue unless `exports` binds one (as in a window that has not created one).
    explicit Live(mcp::McpLiveUiPort ui, std::optional<mcp::McpExportAccess> exports = {},
                  std::filesystem::path captures = {},
                  std::shared_ptr<host::GpuExportProvider> fileProvider = {}) {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     {.renderSource = session->renderSourceSnapshot(),
                      .context = session->operationContext(),
                      .viewerDisplay = {},
                      .viewerView = {},
                      .projectPath = std::nullopt,
                      .readOnly = false});
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(host),
            .exports = exports ? std::move(*exports)
                               : mcp::McpExportAccess{.run =
                                                          [](const mcp::McpExportAccess::Work& work,
                                                             const std::stop_token&) {
                                                              work(nullptr, 1);
                                                              return true;
                                                          },
                                                      .callerIsOwner = false},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .fileSessionGpuProvider = std::move(fileProvider),
            .captureDirectory = std::move(captures),
            .ui = std::move(ui),
            .serverVersion = "test",
            .limits = mcp::kMcpMessageLimits});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"core","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    // Binds the live project to `path` (a new generation), as File > Open does in the application.
    void bindProject(const std::optional<std::filesystem::path>& path) {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     {.renderSource = session->renderSourceSnapshot(),
                      .context = session->operationContext(),
                      .viewerDisplay = {},
                      .viewerView = {},
                      .projectPath = path,
                      .readOnly = false});
    }
    ~Live() {
        core.reset();
        scheduler.beginShutdown();
    }
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;

    std::string send(const std::string& message) {
        const mcp::McpRequestControl control{};
        return core->handle(*protocol, message, control).response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    arguments + "}}");
    }
};

void unboundSeams() {
    Live server({});
    for (const auto& [tool, arguments] : std::vector<std::pair<std::string, std::string>>{
             {"editor.reveal", R"({"composition":1})"},
             {"selection.set", R"({"composition":1,"layers":[]})"},
             {"composition.activate", R"({"composition":1})"},
             {"transport", R"({"action":"play"})"},
             {"viewer.set", R"({"zoom":"fit"})"},
             {"project.new", "{}"},
             {"project.set-color-settings", R"({"config":"aces"})"},
             {"export.reveal", "{}"},
             {"project.cancel-open", "{}"},
             {"preferences.get", "{}"},
             {"preferences.set", R"({"preferences":{"loopPlayback":false}})"},
             {"cache.purge", R"({"cache":"preview"})"},
             {"jobs.list", "{}"},
             {"jobs.cancel", R"({"job":3})"},
             {"asset.reveal", R"({"asset":1})"},
             {"composition.presets", "{}"},
             {"composition.save-preset", R"({"composition":1,"name":"Card"})"},
             {"composition.rename-preset", R"({"name":"Card","newName":"Plate"})"},
             {"composition.delete-preset", R"({"name":"Card"})"},
             {"dialog.answer", R"({"button":"Cancel"})"},
             {"script.run", R"({"source":"x = 1"})"}}) {
        const auto result = server.call(tool, arguments);
        expect(contains(result, "bloom.mcp.ui-unbound"), tool + " unbound is typed: " + result);
    }
}

} // namespace
std::string firstMatch(const std::string& text, const std::string& pattern);
namespace {

// #389: the parity tools. Listed on both hosts; on stdio the window and application tools answer
// not-live, project.save-copy goes to the host's lifecycle, and the evaluated project edits work
// headless with one undo step each. On the live host each tool reaches its port member.
void parityTools() {
    {
        Headless server;
        const auto listed = server.send(R"({"jsonrpc":"2.0","id":90,"method":"tools/list"})");
        for (const auto* name :
             {"project.save-copy", "project.cancel-open", "preferences.get", "preferences.set",
              "cache.purge", "jobs.list", "jobs.cancel", "asset.reveal", "composition.presets",
              "composition.save-preset", "composition.rename-preset", "composition.delete-preset",
              "dialog.answer", "script.run"})
            expect(contains(listed, std::string(R"("name":")") + name + "\""),
                   std::string("tools/list lists ") + name);
        expect(contains(listed, R"("regionOfInterest")") && contains(listed, R"("gamma")"),
               "viewer.set lists its new fields");
        for (const auto& [tool, arguments] : std::vector<std::pair<std::string, std::string>>{
                 {"project.cancel-open", "{}"},
                 {"preferences.get", "{}"},
                 {"preferences.set", R"({"preferences":{"loopPlayback":false}})"},
                 {"cache.purge", R"({"cache":"media"})"},
                 {"jobs.list", "{}"},
                 {"jobs.cancel", R"({"job":3})"},
                 {"asset.reveal", R"({"asset":1})"},
                 {"composition.presets", "{}"},
                 {"composition.save-preset", R"({"composition":1,"name":"Card"})"},
                 {"composition.rename-preset", R"({"name":"Card","newName":"Plate"})"},
                 {"composition.delete-preset", R"({"name":"Card"})"},
                 {"dialog.answer", R"({"button":"Cancel"})"},
                 {"script.run", R"({"source":"x = 1"})"},
                 {"viewer.set", R"({"gamma":2.2,"regionOfInterest":null})"}}) {
            const auto result = server.call(tool, arguments);
            expect(contains(result, "bloom.mcp.not-live") && contains(result, R"("isError":true)"),
                   "stdio " + tool + " answers not-live: " + result);
        }
        auto result = server.call("project.save-copy", R"({"path":"copy.bloom"})");
        expect(contains(result, "bloom.mcp.path-invalid"),
               "save-copy refuses a relative path: " + result);
        result = server.call("cache.purge", R"({"cache":"disk"})");
        expect(contains(result, "preview or media"), "an unknown cache is refused: " + result);

        // The evaluated project edits work on the headless session.
        const auto before = server.session->snapshot().revision();
        result =
            server.call("transact", R"({"expectedRevision":)" + std::to_string(before.value()) +
                                        R"(,"operations":[{"op":"bloom.layer.add-solid","args":{)"
                                        R"("composition":1,"name":"Card","color":[1,1,1,1]}}]})");
        const auto layers = server.call("query", R"({"kind":"layers","composition":1})");
        const auto layer = firstMatch(layers, R"re("records":\[\{"id":(\d+))re");
        const auto revision = server.session->snapshot().revision().value();
        result = server.call("transact",
                             R"({"expectedRevision":)" + std::to_string(revision) +
                                 R"(,"operations":[{"op":"bloom.layer.set-anchor-preset","args":{)"
                                 R"("composition":1,"layer":)" +
                                 layer + R"(,"anchor":"bottom-right","time":0}}]})");
        expect(contains(result, R"("isError":false)") && contains(result, R"("succeeded":true)"),
               "set-anchor-preset applies headless: " + result);
        result = server.call("history", R"({"action":"undo","expectedRevision":)" +
                                            std::to_string(revision + 1) + "}");
        expect(contains(result, R"("succeeded":true)") &&
                   server.session->snapshot().revision().value() != revision + 1,
               "the preset is one undo step: " + result);
        result = server.call(
            "transact", R"({"expectedRevision":)" +
                            std::to_string(server.session->snapshot().revision().value()) +
                            R"(,"operations":[{"op":"bloom.asset.reload","args":{"asset":42}}]})");
        expect(contains(result, R"("succeeded":false)") && contains(result, "does not exist") &&
                   contains(result, R"("operationIndex":0)"),
               "reload of an unknown asset is refused with its index: " + result);
    }

    std::vector<mcp::McpPreference> set;
    std::optional<bool> purged;
    std::optional<std::uint64_t> cancelledJob;
    std::optional<document::AssetId> revealed;
    std::vector<std::string> presetCalls;
    mcp::McpViewerRequest viewer;
    mcp::McpSelectionRequest selected;
    mcp::McpLiveUiPort port;
    port.setSelection = [&](const mcp::McpSelectionRequest& request) {
        selected = request;
        return mcp::McpUiResult{};
    };
    port.setViewer = [&](const mcp::McpViewerRequest& request) {
        viewer = request;
        return mcp::McpUiResult{};
    };
    port.preferences = [] {
        return std::vector<mcp::McpPreference>{
            {.key = "loopPlayback", .value = true},
            {.key = "soundVolume", .value = 40.0},
            {.key = "timelineTimeFormat", .value = std::string("frames")}};
    };
    port.setPreferences = [&](const std::vector<mcp::McpPreference>& entries) {
        set = entries;
        return mcp::McpUiResult{};
    };
    port.purgeCache = [&](const bool media) {
        purged = media;
        return mcp::McpUiResult{.refusal = mcp::McpUiRefusal::Busy, .message = "running"};
    };
    port.jobs = [] {
        mcp::McpJob job;
        job.id = 7;
        job.name = "Import media";
        job.phase = "Importing media";
        job.state = "Running";
        job.completed = 2;
        job.total = 5;
        job.canCancel = true;
        return std::vector<mcp::McpJob>{job};
    };
    port.cancelJob = [&](const std::uint64_t job) {
        cancelledJob = job;
        return mcp::McpUiResult{};
    };
    port.cancelOpen = [] {
        return mcp::McpUiResult{.refusal = mcp::McpUiRefusal::InvalidArgument,
                                .message = "No project is opening."};
    };
    port.revealAsset = [&](const document::AssetId asset) {
        revealed = asset;
        return mcp::McpUiResult{};
    };
    port.compositionPresets = [] {
        return std::vector<mcp::McpCompositionPreset>{
            {.name = "HD 1080p",
             .builtIn = true,
             .width = 1920,
             .height = 1080,
             .pixelAspectNumerator = 1,
             .pixelAspectDenominator = 1,
             .frameRate = std::nullopt,
             .duration = std::nullopt,
             .startTimecode = std::nullopt,
             .dropFrame = std::nullopt,
             .background = std::nullopt},
            {.name = "Card",
             .builtIn = false,
             .width = 800,
             .height = 600,
             .pixelAspectNumerator = 1,
             .pixelAspectDenominator = 1,
             .frameRate = std::pair<std::uint32_t, std::uint32_t>{25, 1},
             .duration = core::RationalTime::fromInteger(4),
             .startTimecode = std::nullopt,
             .dropFrame = std::nullopt,
             .background = std::nullopt}};
    };
    port.saveCompositionPreset = [&](const document::CompositionId composition,
                                     const std::string& name) {
        presetCalls.push_back("save " + std::to_string(composition.value()) + " " + name);
        return mcp::McpUiResult{};
    };
    port.renameCompositionPreset = [&](const std::string& name, const std::string& newName) {
        presetCalls.push_back("rename " + name + " " + newName);
        return mcp::McpUiResult{};
    };
    port.deleteCompositionPreset = [&](const std::string& name) {
        presetCalls.push_back("delete " + name);
        return mcp::McpUiResult{.refusal = mcp::McpUiRefusal::UnknownTarget,
                                .message = "No saved preset."};
    };
    Live server(std::move(port));

    auto result =
        server.call("viewer.set", R"({"gamma":2.2,"resolution":"half","background":"black",)"
                                  R"("transparencyGrid":true,"look":false,)"
                                  R"("regionOfInterest":{"x":10,"y":20,"width":30,"height":40}})");
    expect(viewer.gamma == 2.2 && viewer.resolution == mcp::McpViewerResolution::Half &&
               viewer.background == mcp::McpViewerBackground::Black &&
               viewer.transparencyGrid == true && viewer.look == false && viewer.regionOfInterest &&
               *viewer.regionOfInterest && (*viewer.regionOfInterest)->width == 30.0 &&
               !viewer.exposure,
           "viewer.set passes the new fields: " + result);
    result = server.call("viewer.set", R"({"regionOfInterest":null})");
    expect(viewer.regionOfInterest && !*viewer.regionOfInterest,
           "a null region clears it: " + result);
    result =
        server.call("viewer.set", R"({"regionOfInterest":{"x":0,"y":0,"width":0,"height":4}})");
    expect(contains(result, "at least 1 x 1"), "an empty region is refused: " + result);
    result = server.call("viewer.set", R"({"resolution":"eighth"})");
    expect(contains(result, "auto, full, half or quarter"), "an unknown resolution: " + result);

    result = server.call("selection.set", R"({"composition":1,"mode":"all-nodes"})");
    const auto nodeCount = server.session->snapshot()
                               .project()
                               .findComposition(document::CompositionId::fromRaw(1))
                               ->graph()
                               .nodes()
                               .size();
    expect(contains(result, R"("succeeded":true)") && selected.mode == mcp::McpSelectionMode::Set &&
               selected.nodes.size() == nodeCount && selected.layers.empty(),
           "all-nodes selects every node of the composition: " + result);
    result = server.call("selection.set", R"({"composition":1,"mode":"linked-upstream"})");
    expect(contains(result, "bloom.mcp.invalid-argument"),
           "linked-upstream with nothing to extend from is refused: " + result);
    result = server.call("selection.set", R"({"composition":1,"mode":"everything"})");
    expect(contains(result, "linked-downstream"), "an unknown mode names the choices: " + result);

    result = server.call("preferences.get", "{}");
    expect(contains(result, R"("loopPlayback":true)") && contains(result, R"("soundVolume":40)") &&
               contains(result, R"("timelineTimeFormat":"frames")"),
           "preferences.get reads the port: " + result);
    result =
        server.call("preferences.set", R"({"preferences":{"loopPlayback":false,"soundVolume":10,)"
                                       R"("timelineTimeFormat":"timecode"}})");
    expect(contains(result, R"("succeeded":true)") && set.size() == 3,
           "preferences.set passes every entry: " + result);
    result = server.call("preferences.set", R"({"preferences":{"loopPlayback":[1]}})");
    expect(contains(result, "true/false, a number or a string"),
           "a structured value is refused: " + result);
    result = server.call("preferences.set", R"({"preferences":{}})");
    expect(contains(result, "1 to 64"), "an empty preference set is refused: " + result);

    result = server.call("cache.purge", R"({"cache":"media"})");
    expect(purged == true && contains(result, "bloom.mcp.busy"),
           "cache.purge reaches the port and returns its refusal: " + result);
    result = server.call("jobs.list", "{}");
    expect(contains(result, R"("id":7)") && contains(result, R"("total":5)") &&
               contains(result, R"("canCancel":true)") && contains(result, R"("exportJob":null)"),
           "jobs.list lists the rows: " + result);
    result = server.call("jobs.cancel", R"({"job":7})");
    expect(cancelledJob == 7u && contains(result, R"("succeeded":true)"),
           "jobs.cancel passes the job: " + result);
    result = server.call("project.cancel-open", "{}");
    expect(contains(result, "bloom.mcp.invalid-argument"),
           "cancel-open with nothing opening is refused: " + result);
    result = server.call("asset.reveal", R"({"asset":3})");
    expect(revealed && revealed->value() == 3, "asset.reveal passes the asset: " + result);
    result = server.call("composition.presets", "{}");
    expect(contains(result, R"("name":"HD 1080p","builtIn":true)") &&
               contains(result, R"("frameRate":[25,1])") && contains(result, R"("duration":[4,1])"),
           "composition.presets lists built-in and saved presets: " + result);
    static_cast<void>(server.call("composition.save-preset", R"({"composition":1,"name":"Card"})"));
    static_cast<void>(
        server.call("composition.rename-preset", R"({"name":"Card","newName":"Plate"})"));
    result = server.call("composition.delete-preset", R"({"name":"Plate"})");
    expect(presetCalls ==
                   std::vector<std::string>{"save 1 Card", "rename Card Plate", "delete Plate"} &&
               contains(result, "bloom.mcp.unknown-target"),
           "the preset tools reach the port in order: " + result);
}

void liveSeams() {
    std::optional<core::RationalTime> revealed;
    document::CompositionId revealedComposition;
    mcp::McpSelectionRequest selection;
    mcp::McpTransportRequest transport;
    mcp::McpViewerRequest viewer;
    std::optional<document::CompositionId> activated;
    std::vector<mcp::McpProjectRequest> lifecycle;
    std::optional<document::ColorSettings> colour;
    std::string answered;
    std::string ranScript;
    std::jthread later;
    mcp::McpLiveUiPort port{
        .revealComposition =
            [&](const document::CompositionId composition,
                const std::optional<core::RationalTime> time) {
                revealedComposition = composition;
                revealed = time;
                return composition.value() == 1;
            },
        .projectLifecycle =
            [&](mcp::McpProjectRequest request, std::function<void(mcp::McpProjectResult)> done) {
                lifecycle.push_back(request);
                if (request.kind == mcp::McpProjectRequestKind::New) {
                    done({.refusal = mcp::McpUiRefusal::UnsavedChanges,
                          .message = "Untitled has unsaved changes.",
                          .path = {},
                          .projectName = "Untitled"});
                    return;
                }
                // Asynchronous, as the desktop binding is: finish from another thread later.
                later = std::jthread([done, path = request.path] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    done({.refusal = mcp::McpUiRefusal::None,
                          .message = "Saved.",
                          .path = path,
                          .projectName = "Study"});
                });
            },
        .setSelection =
            [&](const mcp::McpSelectionRequest& request) {
                selection = request;
                return mcp::McpUiResult{};
            },
        .activateComposition =
            [&](const document::CompositionId composition) {
                activated = composition;
                return mcp::McpUiResult{};
            },
        .transport =
            [&](const mcp::McpTransportRequest& request) {
                transport = request;
                return mcp::McpUiResult{};
            },
        .setViewer =
            [&](const mcp::McpViewerRequest& request) {
                viewer = request;
                return mcp::McpUiResult{.refusal = mcp::McpUiRefusal::None, .message = {}};
            },
        .setColorSettings =
            [&](const document::ColorSettings& settings) {
                colour = settings;
                return mcp::McpUiResult{};
            },
        .revealExport = {},
        .preferences = {},
        .setPreferences = {},
        .purgeCache = {},
        .jobs = {},
        .cancelJob = {},
        .cancelOpen = {},
        .revealAsset = {},
        .compositionPresets = {},
        .saveCompositionPreset = {},
        .renameCompositionPreset = {},
        .deleteCompositionPreset = {},
        .answerDialog =
            [&](const std::string& button) {
                answered = button;
                return button == "Cancel"
                           ? mcp::McpUiResult{}
                           : mcp::McpUiResult{.refusal = mcp::McpUiRefusal::NotAllowed,
                                              .message = "Agents are not allowed."};
            },
        .runScript =
            [&](mcp::McpScriptRequest request, std::function<void(mcp::McpScriptResult)> done) {
                ranScript = request.source;
                mcp::McpScriptResult result;
                if (request.source == "off") {
                    result.refusal = mcp::McpUiRefusal::NotAllowed;
                    result.message = "Agents are not allowed to run scripts.";
                } else {
                    result.succeeded = request.source != "raise";
                    result.output = request.source == "raise" ? "ValueError: no" : "hello";
                }
                done(std::move(result));
            },
        .cancelScript = {},
        .undoHistory = {},
        .viewerState = {},
        .previewCache = {},
        .viewerRoute = {}};
    Live server(std::move(port));
    expect(contains(server.call("dialog.answer", R"({"button":"Cancel"})"), R"("succeeded":true)") &&
               answered == "Cancel",
           "dialog.answer reaches the port with the button text");
    auto gated = server.call("dialog.answer", R"({"button":"Discard"})");
    expect(contains(gated, "bloom.mcp.not-allowed"), "a gated dialog.answer is typed: " + gated);
    gated = server.call("script.run", R"({"source":"off"})");
    expect(contains(gated, "bloom.mcp.not-allowed"), "a gated script.run is typed: " + gated);
    auto ran = server.call("script.run", R"json({"source":"print('hello')","timeoutSeconds":5})json");
    expect(contains(ran, R"("succeeded":true)") && contains(ran, "hello") &&
               ranScript == "print('hello')",
           "script.run answers the console output: " + ran);
    ran = server.call("script.run", R"({"source":"raise"})");
    expect(contains(ran, "bloom.mcp.failed") && contains(ran, "ValueError: no"),
           "a failed script answers its error as output: " + ran);
    expect(contains(server.call("script.run", R"({"source":"x","timeoutSeconds":0})"),
                    "timeoutSeconds is 1 to 600"),
           "the timeout is bounded");

    // One time contract: an integer is a whole frame at the composition's rate (24 fps), and
    // [numerator, denominator] is exact seconds.
    auto result = server.call("editor.reveal", R"({"composition":1,"time":12})");
    expect(contains(result, R"("succeeded":true)") && revealed &&
               *revealed == *core::RationalTime::create(1, 2),
           "editor.reveal reads 12 as frame 12: " + result);
    result = server.call("editor.reveal", R"({"composition":1,"time":[3,2]})");
    expect(revealed && *revealed == *core::RationalTime::create(3, 2),
           "editor.reveal reads a pair as seconds: " + result);
    result = server.call("editor.reveal", R"({"composition":9})");
    expect(contains(result, "bloom.mcp.unknown-target"), "an unknown composition: " + result);

    result = server.call("selection.set", R"({"composition":1,"layers":[4,5],"mode":"add"})");
    expect(contains(result, R"("succeeded":true)") && selection.layers.size() == 2 &&
               selection.mode == mcp::McpSelectionMode::Add,
           "selection.set reaches the port: " + result);
    result = server.call("composition.activate", R"({"composition":1})");
    expect(activated && activated->value() == 1, "composition.activate: " + result);
    result = server.call("transport", R"({"action":"seek","time":24})");
    expect(transport.action == mcp::McpTransportAction::Seek && transport.time &&
               *transport.time == core::RationalTime::fromInteger(1),
           "transport seeks 24 frames of the active composition: " + result);
    result = server.call("transport", R"({"action":"ram-preview"})");
    expect(transport.action == mcp::McpTransportAction::RamPreview && !transport.time,
           "transport starts a RAM preview: " + result);
    result = server.call("transport", R"({"action":"preview"})");
    expect(contains(result, "ram-preview"), "an unknown action names ram-preview: " + result);
    result = server.call("viewer.set", R"({"zoom":"fit","channel":"alpha","exposure":1.5})");
    expect(viewer.zoom == mcp::McpViewerZoom::Fit &&
               viewer.channel == mcp::McpViewerChannel::Alpha && viewer.exposure == 1.5 &&
               !viewer.display,
           "viewer.set passes only the given fields: " + result);

    result = server.call("project.open", R"({"path":"relative.bloom"})");
    expect(contains(result, "bloom.mcp.path-invalid") && lifecycle.empty(),
           "a relative path never reaches the application: " + result);
    result = server.call("project.new", "{}");
    expect(contains(result, "bloom.mcp.unsaved-changes") && contains(result, "Untitled"),
           "project.new over unsaved changes is refused by name: " + result);
#ifdef _WIN32
    const std::string path = R"(C:\\study.bloom)";
#else
    const std::string path = "/tmp/study.bloom";
#endif
    result = server.call("project.save-as", R"({"path":")" + path + R"(","replace":true})");
    expect(contains(result, R"("succeeded":true)") && contains(result, "project.saved") &&
               lifecycle.size() == 2 && lifecycle.back().replace,
           "project.save-as waits for the asynchronous binding: " + result);
    // Colour settings are resolved by the core and applied by the application through the port.
    result = server.call("project.set-color-settings", R"({"config":"aces"})");
    expect(contains(result, R"("code":"updated")") && colour.has_value(),
           "project.set-color-settings reaches the port: " + result);
    expect(contains(result, R"("workingSpace":"ACEScg")") &&
               contains(result, R"("workingSpaceLabel":"ACEScg")"),
           "project.set-color-settings reads back the working space id and label: " + result);
    result = server.call("project.set-color-settings", R"({"config":"neutral"})");
    expect(contains(result, R"("workingSpace":"lin_rec709_scene")") &&
               contains(result, R"json("workingSpaceLabel":"Linear Rec.709 (sRGB)")json"),
           "Bloom Neutral reads back lin_rec709_scene labelled Linear Rec.709 (sRGB): " + result);
    result = server.call("events", R"({"sinceRevision":0})");
    expect(contains(result, R"("kind":"project.saved")"), "events list project.saved: " + result);
}

// export.reveal (#382): listed read-only, live-only, and a plain pass-through to the port, which
// owns the queue lookup and the refusals.
void exportReveal() {
    {
        Headless stdio;
        const auto listed = stdio.send(R"({"jsonrpc":"2.0","id":90,"method":"tools/list"})");
        const auto at = listed.find(R"("name":"export.reveal")");
        expect(at != std::string::npos, "tools/list lists export.reveal");
        const auto entry = listed.substr(at, listed.find(R"({"name":)", at + 1) - at);
        expect(contains(entry, R"("readOnlyHint":true)") &&
                   contains(entry, R"("openWorldHint":false)") &&
                   contains(entry, R"("job":{"type":["integer","string"],"minimum":1)"),
               "export.reveal is read-only with an optional job id: " + entry);
        // A port bound on stdio would still not be reached: there is no window.
        bool reached = false;
        mcp::McpLiveUiPort stdioPort;
        stdioPort.revealExport = [&](std::optional<document::ExportJobId>) {
            reached = true;
            return mcp::McpUiResult{};
        };
        Headless bound(std::move(stdioPort));
        for (auto* server : {&stdio, &bound})
            for (const auto* arguments : {"{}", R"({"job":3})"}) {
                const auto result = server->call("export.reveal", arguments);
                expect(contains(result, "bloom.mcp.not-live") &&
                           contains(result, R"("isError":true)"),
                       "stdio export.reveal answers not-live: " + result);
            }
        expect(!reached, "stdio never calls the port");
    }
    std::vector<std::optional<document::ExportJobId>> asked;
    mcp::McpLiveUiPort port;
    port.revealExport = [&](const std::optional<document::ExportJobId> job) {
        asked.push_back(job);
        if (job && job->value() == 9)
            return mcp::McpUiResult{.refusal = mcp::McpUiRefusal::UnknownTarget,
                                    .message = "Export job 9 is not in the queue."};
        return mcp::McpUiResult{};
    };
    Live server(std::move(port));
    const auto before = server.session->snapshot().revision();
    auto result = server.call("export.reveal", "{}");
    expect(contains(result, R"("succeeded":true)") && asked.size() == 1 && !asked.back(),
           "export.reveal without a job reveals the panel only: " + result);
    result = server.call("export.reveal", R"({"job":4})");
    expect(contains(result, R"("succeeded":true)") && asked.size() == 2 && asked.back() &&
               asked.back()->value() == 4,
           "export.reveal passes the job: " + result);
    result = server.call("export.reveal", R"({"job":9})");
    expect(contains(result, "bloom.mcp.unknown-target") && contains(result, "not in the queue"),
           "the port's UnknownTarget reaches the agent: " + result);
    result = server.call("export.reveal", R"({"job":0})");
    expect(contains(result, R"("error")") && asked.size() == 3,
           "job 0 is an invalid argument and never reaches the port: " + result);
    result = server.call("export.reveal", R"({"composition":1})");
    expect(contains(result, R"("error")") && asked.size() == 3, "unknown arguments are refused");
    expect(server.session->snapshot().revision() == before,
           "export.reveal never edits the project");
}

// The id after the first `"job":` of a response.
std::uint64_t jobOf(const std::string& response) {
    auto at = response.find(R"("job":)");
    if (at == std::string::npos)
        return 0;
    at += 6;
    if (at < response.size() && response[at] == '"') // ids are reported as decimal strings
        ++at;
    return std::stoull(response.substr(at));
}

// A finished transient render/export/capture job stays readable after its call returned and the
// queue retired it (export.log, export.status, export.wait), and capture carries its job and log.
void retiredTransientJobs() {
    const auto captures =
        std::filesystem::temp_directory_path() /
        ("bloom-mcp-core-tools-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(captures);
    auto fake = std::make_shared<host::testing::FakeExportRunBackend>();
    std::unique_ptr<host::ExportQueueService> queue;
    std::uint64_t generation = 1;
    mcp::McpExportAccess exports{
        .run =
            [&](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                work(queue.get(), generation);
                return true;
            },
        .callerIsOwner = true};
    runtime::NodeDefinitionRegistry registry;
    const runtime::SnapshotCompiler compiler{registry};
    Live server({}, std::move(exports), captures);
    // The queue borrows the server's session and scheduler: it goes first.
    struct Release final {
        std::unique_ptr<host::ExportQueueService>& queue;
        std::filesystem::path folder;
        ~Release() {
            queue.reset();
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    } release{queue, captures};
    host::ExportEncoderStatus encoders;
    encoders.probed = true;
    encoders.mediaWorker = true;
    queue = std::make_unique<host::ExportQueueService>(host::ExportQueueServiceDependencies{
        .session = *server.session->hostSession(),
        .scheduler = server.scheduler,
        .compiler = compiler,
        .publications = *server.session->publicationCoordinator(),
        .artifacts = *server.session->artifactCoordinator(),
        .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
        .gpu = nullptr,
        .retirement = nullptr,
        .presets = host::ExportPresetCatalogue::create(
            {.userDataDirectory = std::nullopt, .initialEncoders = encoders}),
        .limits = {},
        .displayProcessor = nullptr,
        .scratchDirectory = {},
        .backend = fake});

    fake->push({.outcome = host::testing::FakeExportScript::Outcome::FailRendering});
    const auto destination = (captures / "failed.png").string();
    const auto failed = server.call("render", R"({"composition":1,"frame":0,)"
                                              R"("preset":"PngRgba8SrgbV1","destination":")" +
                                                  destination + R"("})");
    const auto job = jobOf(failed);
    expect(contains(failed, R"("succeeded":false)") && job != 0 && contains(failed, R"("log":[)"),
           "a failed render carries its job and log: " + failed);
    const auto id = std::to_string(job);
    const auto log = server.call("export.log", R"({"job":)" + id + "}");
    expect(contains(log, "export.fake.started") && !contains(log, R"("lastSequence":0)"),
           "export.log reads the finished transient job after the call returned: " + log);
    const auto after = server.call("export.log", R"({"job":)" + id + R"(,"afterSequence":1})");
    expect(contains(after, R"("entries":[)") && !contains(after, R"("sequence":1,)"),
           "export.log honors afterSequence on a retired job: " + after);
    auto status = server.call("export.status", "{}");
    expect(contains(status, R"("recentTransientJobs":[{"id":")" + id + '"') &&
               contains(status, R"("transient":true)"),
           "export.status lists the retired transient job: " + status);
    // The refusal codes are named by enumerator, not by position: ADR 0035 retired value 23, and
    // the codes after it must keep their own names.
    // A finished job reports no pending destination check (#585), and no code is unnamed.
    expect(contains(status, R"("code":"NotRendering")") && contains(status, R"("code":"NotActive")") &&
               !contains(status, "DestinationCheckPending") &&
               !contains(status, R"("code":"Unknown")") && contains(status, R"("stalled":false)"),
           "export.status names the codes after the retired gap correctly: " + status);
    const auto wait = server.call("export.wait", R"({"job":)" + id + R"(,"timeoutMs":1000})");
    expect(contains(wait, R"("finished":true)"), "export.wait finds the retired job: " + wait);

    // Ids round-trip exactly through JSON (#584): transient ids sit above 2^53, so they are reported
    // as strings, and a double-based client still tells two jobs apart and targets each one.
    fake->push({.outcome = host::testing::FakeExportScript::Outcome::FailRendering});
    const auto secondFailed = server.call("render", R"({"composition":1,"frame":0,)"
                                                    R"("preset":"PngRgba8SrgbV1","destination":")" +
                                                        destination + R"("})");
    const auto secondId = std::to_string(jobOf(secondFailed));
    expect(secondId != "0" && secondId != id && contains(secondFailed, R"("job":")" + secondId + '"'),
           "a second render reports its own string job id: " + secondFailed);
    const auto asDouble = [](const std::string& text) {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.0f", std::stod(text));
        return std::string(buffer);
    };
    expect(asDouble(id) == asDouble(secondId),
           "the fixture ids really collide when parsed as doubles");
    expect(std::stoull(id) > (1ULL << 53U), "transient ids exceed 2^53");
    expect(contains(status, R"("id":")" + id + '"') &&
               contains(server.call("export.status", "{}"), R"("id":")" + secondId + '"'),
           "export.status reports both ids as exact strings");
    const auto logOf = [&](const std::string& argument) {
        return server.call("export.log", R"({"job":)" + argument + "}");
    };
    const auto firstLog = logOf('"' + id + '"');
    const auto secondLog = logOf('"' + secondId + '"');
    expect(contains(firstLog, R"("entries":[{)") && contains(secondLog, R"("entries":[{)") &&
               contains(logOf(id), R"("entries":[{)") && !contains(firstLog, R"("isError":true)"),
           "export.log targets the job named by a string or integer id: " + firstLog);
    const auto stopped = server.call("export.stop", R"({"job":")" + secondId + R"("})");
    const auto malformed = server.call("export.log", R"({"job":"abc"})");
    expect(!contains(stopped, "Expected an id") && contains(malformed, "Expected an id"),
           "export.stop accepts a string id; a malformed id is refused: " + stopped + malformed);

    fake->push({.writeDestinations = true});
    const auto capture = server.call("capture", R"({"composition":1,"frame":0})");
    const auto captureJob = jobOf(capture);
    expect(contains(capture, R"("isError":false)") && captureJob != 0 && captureJob != job &&
               contains(capture, R"("log":[{)"),
           "a capture carries its transient job and log: " + capture);
    expect(contains(server.call("export.log", R"({"job":)" + std::to_string(captureJob) + "}"),
                    R"("entries":[{)"),
           "export.log reads the capture's retired job");
    expect(contains(capture, R"("gpuFrames":1)") && contains(capture, R"("cpuReferenceFrames":0)") &&
               contains(capture, R"("provenance":"gpu")"),
           "a live capture reports the route the export queue recorded: " + capture);

    // 33 more retire the oldest: the ring keeps the newest 32.
    for (int index = 0; index < 33; ++index) {
        fake->push({.outcome = host::testing::FakeExportScript::Outcome::FailRendering});
        static_cast<void>(server.call("render", R"({"composition":1,"frame":0,)"
                                                R"("preset":"PngRgba8SrgbV1","destination":")" +
                                                    destination + R"("})"));
    }
    expect(contains(server.call("export.log", R"({"job":)" + id + "}"), R"("entries":[])"),
           "the ring is bounded: the oldest retired job is released");
    // A replaced project (a new queue generation) releases every retired job.
    generation = 2;
    status = server.call("export.status", "{}");
    expect(contains(status, R"("recentTransientJobs":[])"),
           "a new queue generation releases the retired jobs: " + status);

    // Issue #570: loudness normalization over MCP. The target reaches the run and the result carries
    // the measured and delivered level; out-of-range, lone-ceiling and still requests are refused.
    {
        const auto movie = (captures / "loudness.mov").string();
        const auto call = [&](const std::string& extra) {
            return server.call("export", R"({"composition":1,"frame":0,"preset":"ProResMovV1",)"
                                         R"("destination":")" +
                                             movie + R"(")" + extra + "}");
        };
        fake->push({});
        const auto normalized = call(R"(,"loudnessLufs":-23,"truePeakDbtp":-2)");
        expect(contains(normalized, R"("isError":false)") &&
                   contains(normalized, R"("targetLufs":-23)") &&
                   contains(normalized, R"("truePeakLimitDbtp":-2)") &&
                   contains(normalized, R"("measuredLufs":-20)") &&
                   contains(normalized, R"("finalLufs":-23)") && contains(normalized, R"("gainDb":-3)"),
               "export reports measured and delivered loudness: " + normalized);
        fake->push({});
        expect(!contains(call(""), R"("loudness":)"), "no target reports no loudness");
        for (const auto& bad : {R"(,"loudnessLufs":-3)", R"(,"loudnessLufs":-45)",
                                R"(,"loudnessLufs":-16,"truePeakDbtp":1)", R"(,"truePeakDbtp":-1)"}) {
            const auto refused = call(bad);
            expect(contains(refused, "-32602"), std::string("refused: ") + bad + " " + refused);
        }
        const auto still =
            server.call("render", R"({"composition":1,"frame":0,"preset":"PngRgba8SrgbV1",)"
                                  R"("destination":")" +
                                      (captures / "loudness.png").string() +
                                      R"(","loudnessLufs":-16})");
        expect(contains(still, "-32602"), "a still preset refuses a loudness target: " + still);
    }

    // Issue #581: worker limits and the worker memory report over MCP.
    {
        constexpr std::uint64_t mib = std::uint64_t{1} << 20U;
        const auto movie = (captures / "limits.mov").string();
        fake->push({.workerPeakBytes = 5 * mib});
        const auto limited =
            server.call("export", R"({"composition":1,"frame":0,"preset":"ProResMovV1",)"
                                  R"("destination":")" +
                                      movie + R"(","workerMemoryCapBytes":1073741824,)"
                                              R"("workerThreads":3})");
        expect(contains(limited, R"("isError":false)") &&
                   contains(limited, R"("workerMemory":{"measured":true,"peakBytes":5242880,)"
                                     R"("capBytes":1073741824,"waits":0,"waitedMs":0})"),
               "export reports the worker's peak memory and the cap it ran under: " + limited);
        expect(fake->startedLimits().back() == host::ExportWorkerLimits{1073741824, 3},
               "export passes its worker options to the run");
        fake->push({.workerPeakBytes = 5 * mib});
        const auto plain = server.call("export", R"({"composition":1,"frame":0,)"
                                                 R"("preset":"ProResMovV1","destination":")" +
                                                     movie + R"("})");
        expect(contains(plain, R"("capBytes":0)") &&
                   fake->startedLimits().back() == host::ExportWorkerLimits{},
               "with no option and no saved default an export runs with no limit: " + plain);
        queue->setWorkerLimits({2048 * mib, 6});
        fake->push({.workerPeakBytes = 5 * mib});
        const auto saved = server.call("export", R"({"composition":1,"frame":0,)"
                                                 R"("preset":"ProResMovV1","destination":")" +
                                                     movie + R"(","workerThreads":2})");
        expect(contains(saved, R"("isError":false)") &&
                   fake->startedLimits().back() == host::ExportWorkerLimits{2048 * mib, 2},
               "an option not given keeps the saved Preferences value: " + saved);
        queue->setWorkerLimits({});
        const auto tooMany =
            server.call("export", R"({"composition":1,"frame":0,"preset":"ProResMovV1",)"
                                  R"("destination":")" +
                                      movie + R"(","workerThreads":65})");
        const auto tooBig =
            server.call("export", R"({"composition":1,"frame":0,"preset":"ProResMovV1",)"
                                  R"("destination":")" +
                                      movie + R"(","workerMemoryCapBytes":1099511627777})");
        expect(contains(tooMany, "-32602") && contains(tooMany, "workerThreads") &&
                   contains(tooBig, "-32602") && contains(tooBig, "workerMemoryCapBytes"),
               "out-of-range worker options are refused: " + tooMany + tooBig);
        const auto still =
            server.call("render", R"({"composition":1,"frame":0,"preset":"PngRgba8SrgbV1",)"
                                  R"("destination":")" +
                                      (captures / "limits.png").string() +
                                      R"(","workerThreads":2})");
        expect(contains(still, "-32602") && contains(still, "worker"),
               "a still render has no worker to limit: " + still);
        const auto badStart = server.call("export.start", R"({"jobs":[1],"workerThreads":65})");
        const auto emptyStart = server.call("export.start-all", R"({"workerThreads":2})");
        expect(contains(badStart, "-32602") && contains(badStart, "workerThreads") &&
                   !contains(emptyStart, "-32602") && contains(emptyStart, "NothingToStart"),
               "export.start and export.start-all take the worker options: " + badStart +
                   emptyStart);
        // #619: a job id is accepted as an integer or as a decimal string, with the same answer.
        const auto numberStart = server.call("export.start", R"({"jobs":[1]})");
        const auto textStart = server.call("export.start", R"({"jobs":["1"]})");
        expect(!contains(numberStart, "-32602") && !contains(textStart, "-32602") &&
                   contains(numberStart, "UnknownJob") && contains(textStart, "UnknownJob"),
               "export.start answers a job id the same as an integer and as a string: " +
                   numberStart + textStart);
    }
}

// #390: the read-back queries that answer a fact an agent would otherwise render to learn, and
// the operations records' outputs and side effects, and the origin of each edit.
void inspectionQueries() {
    {
        Headless server;
        // text-metrics: advance and ink without a render.
        auto metrics = server.call(
            "query", R"({"kind":"text-metrics","text":"Hello","font":"Inter Regular","size":48})");
        expect(contains(metrics, R"("isError":false)") && contains(metrics, R"("advance":)") &&
                   contains(metrics, R"("ink":{)") && contains(metrics, R"("label":"Inter Regular")"),
               "text-metrics answers advance and ink: " + metrics);
        const auto narrow = firstMatch(metrics, R"re("advance":([0-9.]+))re");
        const auto wide = firstMatch(
            server.call("query", R"({"kind":"text-metrics","text":"Hello","size":96})"),
            R"re("advance":([0-9.]+))re");
        expect(!narrow.empty() && !wide.empty() && std::stod(wide) > std::stod(narrow) * 0.5,
               "a larger size has a larger advance");
        metrics = server.call("query", R"({"kind":"text-metrics","text":"a\nb","size":20})");
        expect(contains(metrics, R"("index":1)"), "a newline starts a second line: " + metrics);
        metrics = server.call("query", R"({"kind":"text-metrics","text":"   ","size":20})");
        expect(contains(metrics, R"("ink":null)"), "text without ink reports ink null: " + metrics);
        metrics = server.call("query", R"({"kind":"text-metrics","text":"x","size":0})");
        expect(contains(metrics, "pixels per em"), "a zero size is refused: " + metrics);
        metrics = server.call("query", R"({"kind":"text-metrics","text":"x","font":"Comic","size":9})");
        expect(contains(metrics, "built-in faces"), "an unknown font names the faces: " + metrics);

        // layer-bounds: the evaluated bounds of a layer, with the space named.
        auto before = server.session->snapshot().revision();
        auto added = server.call(
            "transact", R"({"expectedRevision":)" + std::to_string(before.value()) +
                            R"(,"label":"Add card","operations":[{"op":"bloom.layer.add-solid","args":{)"
                            R"("composition":1,"name":"Card","color":[1,1,1,1]}}]})");
        expect(contains(added, R"("origin":"agent")"), "a transact result names its origin: " + added);
        const auto layers = server.call("query", R"({"kind":"layers","composition":1})");
        const auto layer = firstMatch(layers, R"re("records":\[\{"id":(\d+))re");
        auto bounds = server.call(
            "query", R"({"kind":"layer-bounds","composition":1,"layer":)" + layer + R"(,"time":0})");
        expect(contains(bounds, R"("visible":true)") && contains(bounds, R"("corners":[[)") &&
                   contains(bounds, "composition pixels") && contains(bounds, R"("anchor":[)"),
               "layer-bounds measures a solid: " + bounds);
        const auto left = firstMatch(bounds, R"re("local":\{"left":(-?[0-9.]+))re");
        const auto right = firstMatch(bounds, R"re("local":\{"left":-?[0-9.]+,"top":-?[0-9.]+,"right":(-?[0-9.]+))re");
        expect(!left.empty() && !right.empty() && std::stod(right) - std::stod(left) > 0.0,
               "the local rectangle is the solid's own extent: " + bounds);
        bounds = server.call("query", R"({"kind":"layer-bounds","composition":1,"layer":9999})");
        expect(contains(bounds, R"("isError":true)") ||
                   contains(bounds, R"("status":"unavailable")") || contains(bounds, "does not exist"),
               "an unknown layer is not measured: " + bounds);
        bounds = server.call("query", R"({"kind":"layer-bounds","composition":1,"layer":)" + layer +
                                          R"(,"time":[1000,1]})");
        expect(contains(bounds, R"("visible":false)") && contains(bounds, R"("reason":)"),
               "a time outside the layer's range reads visible:false with a reason: " + bounds);

        // history: labels and position, kept in step with undo.
        auto history = server.call("query", R"({"kind":"history"})");
        expect(contains(history, R"("undoLabel":"Add card")") && contains(history, R"("canRedo":false)") &&
                   contains(history, R"("applied":true)"),
               "history lists the agent's step: " + history);
        const auto revision = server.session->snapshot().revision().value();
        static_cast<void>(server.call(
            "history", R"({"action":"undo","expectedRevision":)" + std::to_string(revision) + "}"));
        history = server.call("query", R"({"kind":"history"})");
        expect(contains(history, R"("canRedo":true)") && contains(history, R"("redoLabel":"Add card")") &&
                   contains(history, R"("applied":false)") && contains(history, R"("undoLabel":null)"),
               "an undo moves the position, the step stays as the redo: " + history);

        // events: the origin of an edit.
        const auto events = server.call("events", R"({"sinceRevision":0})");
        expect(contains(events, R"(\"origin\":\"agent\")"),
               "events name the agent as the origin: " + events);

        // The window-only kinds answer not-live on stdio.
        for (const auto* kind : {"viewer", "preview-cache", "jobs", "render-route"}) {
            const auto answer = server.call("query", std::string(R"({"kind":")") + kind + R"("})");
            expect(contains(answer, "bloom.mcp.not-live"), std::string(kind) + " is not-live on stdio: " + answer);
        }

        // The operations list: structured outputs and side effects, filtered by prefix.
        const auto duplicate =
            server.call("query", R"({"kind":"operations","idPrefix":"bloom.node.duplicate"})");
        expect(contains(duplicate, R"("sideEffects":["Repeated ids in `nodes` collapse)") &&
                   contains(duplicate, R"("outputs":[{"name":"node","kind":"node","repeated":true)"),
               "node.duplicate declares its outputs and that repeated ids collapse: " + duplicate);
        const auto setSource =
            server.call("query", R"({"kind":"operations","idPrefix":"bloom.parameter.set-source"})");
        expect(contains(setSource, "retires its animation curve"),
               "set-source declares what it does to an animated parameter: " + setSource);
        const auto feeding =
            server.call("query", R"({"kind":"operations","idPrefix":"bloom.node.add-feeding"})");
        expect(contains(feeding, "replaces the existing link") &&
                   contains(feeding, R"("name":"editorNode")"),
               "add-feeding declares the replaced link and its outputs: " + feeding);
        const auto curve = server.call(
            "query", R"({"kind":"operations","idPrefix":"bloom.animation.create-for-parameter"})");
        expect(contains(curve, R"("name":"animationCurve")") && contains(curve, R"("name":"keyframe")"),
               "create-for-parameter declares the curve and the key: " + curve);
        const auto none =
            server.call("query", R"({"kind":"operations","idPrefix":"bloom.project.set-name"})");
        expect(contains(none, R"("outputs":[])") && contains(none, R"("sideEffects":[])"),
               "an operation with neither says so with empty lists: " + none);
    }
    {
        // The live host: the window-only kinds read the port, and an unknown state reads null.
        mcp::McpLiveUiPort port;
        port.undoHistory = [] { return mcp::McpUndoHistory{.labels = {"One", "Two"}, .position = 1}; };
        port.viewerState = [] {
            mcp::McpViewerState state;
            state.available = true;
            state.exposure = 1.5;
            state.channel = mcp::McpViewerChannel::Alpha;
            state.fit = false;
            state.zoomFactor = 2.0;
            state.regionOfInterest = mcp::McpViewerRegion{.x = 1, .y = 2, .width = 3, .height = 4};
            return state;
        };
        port.jobs = [] {
            const auto row = [](const std::uint64_t id, std::string name, std::string kind,
                                std::string state) {
                mcp::McpJob job;
                job.id = id;
                job.name = std::move(name);
                job.kind = std::move(kind);
                job.stateToken = std::move(state);
                return job;
            };
            auto running = row(4, "Import media", "project", "running");
            running.phase = "Importing";
            running.state = "Running";
            running.completed = 2;
            running.total = 5;
            running.canCancel = true;
            auto failed = row(5, "Thumbnails", "application", "failed");
            failed.failure = "Disk full";
            auto exportRun = row(6, "Export run", "export", "running");
            exportRun.exportJob = document::ExportJobId::fromRaw(1);
            return std::vector<mcp::McpJob>{running, failed, exportRun, row(8, "Mystery", "", "")};
        };
        int routeMode = 0;
        port.viewerRoute = [&routeMode] {
            mcp::McpViewerRoute route;
            if (routeMode == 3)
                return route;
            route.available = true;
            route.freshness = "current";
            if (routeMode == 2)
                return route;
            route.hasFrame = true;
            route.time = core::RationalTime::create(1, 24);
            route.label = "label";
            route.provider = routeMode == 0 ? "gpu-neutral" : "gpu-resident";
            route.scene = routeMode == 0 ? mcp::McpRouteKind::Cpu : mcp::McpRouteKind::Gpu;
            route.display = mcp::McpRouteKind::Gpu;
            if (routeMode == 0)
                route.note = "The scene was evaluated on the CPU; only the display transform ran "
                             "on the GPU.";
            return route;
        };
        bool known = false;
        port.previewCache = [&known] {
            mcp::McpPreviewCacheState state;
            state.available = true;
            if (known) {
                state.filling = false;
                state.cachedFrames = 12;
                state.totalFrames = 48;
                state.longestRun = std::pair<std::uint64_t, std::uint64_t>{0, 12};
                state.stopReason = mcp::McpPreviewStop::Budget;
            }
            return state;
        };
        Live server(std::move(port));
        const auto history = server.call("query", R"({"kind":"history"})");
        expect(contains(history, R"("undoLabel":"One")") && contains(history, R"("redoLabel":"Two")") &&
                   contains(history, R"("position":1)"),
               "history reads the window's stack: " + history);
        const auto viewer = server.call("query", R"({"kind":"viewer"})");
        expect(contains(viewer, R"("exposure":1.5)") && contains(viewer, R"("channel":"alpha")") &&
                   contains(viewer, R"("zoom":2)") && contains(viewer, R"("regionOfInterest":{)"),
               "viewer reads the settings viewer.set writes: " + viewer);
        auto cache = server.call("query", R"({"kind":"preview-cache"})");
        expect(contains(cache, R"("state":null)") && contains(cache, R"("cachedFrames":null)") &&
                   contains(cache, R"("fillRate":null)") && contains(cache, R"("budgetBytes":null)") &&
                   contains(cache, R"("stopReason":null)") && contains(cache, R"("filledRange":null)"),
               "what the application does not know reads null, never a made-up value: " + cache);
        // Jobs that are not exports: kind, state, progress and the reason a failed job recorded.
        const auto jobs = server.call("query", R"({"kind":"jobs"})");
        expect(contains(jobs, R"("id":4,"name":"Import media","kind":"project","state":"running")") &&
                   contains(jobs, R"("progress":{"phase":"Importing","completed":2,"total":5})") &&
                   contains(jobs, R"("failure":null)"),
               "jobs lists a running import with its progress: " + jobs);
        expect(contains(jobs, R"("id":5,"name":"Thumbnails","kind":"application","state":"failed")") &&
                   contains(jobs, R"("progress":{"phase":"","completed":null,"total":null})") &&
                   contains(jobs, R"("failure":"Disk full")"),
               "jobs gives a failed job's reason and an indeterminate job's null progress: " + jobs);
        expect(!contains(jobs, "Export run") && !contains(jobs, R"("id":6)"),
               "jobs leaves export runs to export.status: " + jobs);
        expect(contains(jobs, R"("kind":null,"state":null)"),
               "a row that does not say reads null, never a made-up word: " + jobs);
        const auto jobsWithArgument = server.call("query", R"({"kind":"jobs","composition":1})");
        expect(contains(jobsWithArgument, "Unknown member: composition"),
               "jobs takes no other argument: " + jobsWithArgument);

        // The route of the Viewer's frame, as the frame recorded it.
        auto route = server.call("query", R"({"kind":"render-route"})");
        expect(contains(route, R"("available":true)") && contains(route, R"("scene":"cpu")") &&
                   contains(route, R"("display":"gpu")") &&
                   contains(route, R"("provider":"gpu-neutral")") &&
                   contains(route, R"("why":"The scene was evaluated on the CPU;)") &&
                   contains(route, R"("time":[1,24])") && contains(route, R"("cpuFallbacks":null)"),
               "render-route reports a CPU scene with a GPU display, and why: " + route);
        routeMode = 1;
        route = server.call("query", R"({"kind":"render-route"})");
        expect(contains(route, R"("scene":"gpu")") && contains(route, R"("display":"gpu")") &&
                   contains(route, R"("why":null)"),
               "a GPU frame says GPU and records no reason: " + route);
        routeMode = 2;
        route = server.call("query", R"({"kind":"render-route"})");
        expect(contains(route, R"("hasFrame":false)") && contains(route, R"("scene":null)") &&
                   contains(route, R"("display":null)") && contains(route, R"("provider":null)"),
               "with no frame nothing is reported, and an unknown reads null: " + route);
        routeMode = 3;
        route = server.call("query", R"({"kind":"render-route"})");
        expect(contains(route, R"("available":false)") && !contains(route, R"("scene")"),
               "with no Viewer there is no route to report: " + route);
        known = true;
        cache = server.call("query", R"({"kind":"preview-cache"})");
        expect(contains(cache, R"("state":"idle")") && contains(cache, R"("cachedFrames":12)") &&
                   contains(cache, R"("filledRange":{"firstFrame":0,"endFrame":12,"frames":12})") &&
                   contains(cache, R"("stopReason":"budget")"),
               "preview-cache reports the range and why the fill stopped: " + cache);
        expect(contains(cache, R"("fillRate":null)"),
               "without a profiler sample the fill rate stays unknown: " + cache);
    }
}

} // namespace

// Document 1.30 read-back: query kind=layers, kind=keyframes and kind=enums carry the easing
// equations, their parameters and the shy / nameFollowsSource layer flags the new operations write.
std::string firstMatch(const std::string& text, const std::string& pattern) {
    std::smatch match;
    return std::regex_search(text, match, std::regex(pattern)) ? match[1].str() : std::string{};
}

template <typename Server> std::string revisionOf(Server& server) {
    return firstMatch(server.call("query", R"({"kind":"project"})"), R"re("revision":(\d+))re");
}

void animationReadBack() {
    Headless server;
    const auto transact = [&](const std::string& operations) {
        return server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                           R"(,"operations":[)" + operations + "]}");
    };
    auto result = transact(R"({"op":"bloom.layer.add-solid","args":{"composition":1,)"
                           R"("name":"Solid","color":[1,0,0,1]}})");
    expect(contains(result, R"("isError":false)"), "the solid fixture applies: " + result);
    const auto layers = server.call("query", R"({"kind":"layers","composition":1})");
    const auto layer = firstMatch(layers, R"re("records":\[\{"id":(\d+))re");
    expect(!layer.empty() && contains(layers, R"("shy":false)") &&
               contains(layers, R"("nameFollowsSource":false)"),
           "layers read back with their presentation flags: " + layers);
    const auto nodes = server.call("query", R"({"kind":"nodes","composition":1})");
    const auto opacity = firstMatch(nodes, R"re("opacity":(\d+))re");
    expect(!opacity.empty(), "the layer's opacity parameter is addressable: " + nodes);
    for (const auto* at : {R"(0,"value":0.0)", R"(12,"value":1.0)"})
        static_cast<void>(transact(
            R"({"op":"bloom.animation.set-keyframe-at-time-for-parameter","args":{"composition":1,)"
            R"("parameter":)" +
            opacity + R"(,"time":)" + at + "}}"));
    const auto keyframesQuery =
        R"({"kind":"keyframes","composition":1,"parameter":)" + opacity + "}";
    auto keyframes = server.call("query", keyframesQuery);
    const auto curve = firstMatch(keyframes, R"re("curve":(\d+))re");
    const auto key = firstMatch(keyframes, R"re("keyframe":(\d+))re");
    expect(!curve.empty() && !key.empty() && contains(keyframes, R"("interpolation":"linear")"),
           "keyframes read back with their interpolation token: " + keyframes);
    result = transact(R"({"op":"bloom.animation.set-keyframes-easing","args":{"composition":1,)"
                      R"("keys":[[)" +
                      curve + "," + key +
                      R"(]],"interpolation":"elastic","ease":"out","amplitude":2.0}},)"
                      R"({"op":"bloom.layer.set-shy","args":{"composition":1,"layer":)" +
                      layer + R"(,"shy":true}})");
    keyframes = server.call("query", keyframesQuery);
    expect(contains(keyframes, R"("interpolation":"elastic")") &&
               contains(keyframes, R"("interpolationId":12)") &&
               contains(keyframes, R"("ease":"out")") && contains(keyframes, R"("amplitude":2)"),
           "set-keyframes-easing reads back mode and parameters: " + result + keyframes);
    expect(contains(server.call("query", R"({"kind":"layers","composition":1})"), R"("shy":true)"),
           "set-shy reads back");
    expect(contains(keyframes, R"("handleMode":"free")") &&
               contains(keyframes, R"("handleModeId":0)"),
           "every key reads back as Free until its mode is set: " + keyframes);
    result =
        transact(R"({"op":"bloom.animation.set-keyframes-handle-mode","args":{"composition":1,)"
                 R"("keys":[[)" +
                 curve + "," + key + R"(]],"mode":"auto"}})");
    keyframes = server.call("query", keyframesQuery);
    expect(contains(keyframes, R"("handleMode":"auto")") &&
               contains(keyframes, R"("handleModeId":2)"),
           "set-keyframes-handle-mode reads back: " + result + keyframes);
    result =
        transact(R"({"op":"bloom.animation.set-keyframes-handle-mode","args":{"composition":1,)"
                 R"("keys":[[)" +
                 curve + "," + key + R"(]],"mode":"smooth"}})");
    expect(contains(result, R"("isError":true)"), "an unknown handle mode is refused: " + result);
    const auto enums = server.call("query", R"({"kind":"enums"})");
    expect(contains(enums, R"("token":"bounce")") && contains(enums, R"("label":"Bezier")") &&
               contains(enums, R"("token":"in-out")") &&
               contains(enums, R"("label":"Easy Ease Out")"),
           "the interpolation, ease and Easy Ease tables are listed: " + enums);
    expect(contains(enums, R"("enum":"handleMode")") && contains(enums, R"("token":"aligned")") &&
               contains(enums, R"("label":"Auto")"),
           "the handle mode table is listed: " + enums);
}


// #502 Every Merge entry is a layer: an agent creates, edits and reads back entries and adjustment
// layers through transact and query, without knowing a layer id it could not know yet.
struct JsonResponse final {
    yyjson_doc* document = nullptr;
    explicit JsonResponse(const std::string& text)
        : document(yyjson_read(text.data(), text.size(), 0)) {}
    ~JsonResponse() { yyjson_doc_free(document); }
    JsonResponse(const JsonResponse&) = delete;
    JsonResponse& operator=(const JsonResponse&) = delete;
    [[nodiscard]] yyjson_val* content() const {
        return document ? yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(document), "result"),
                                         "structuredContent")
                        : nullptr;
    }
};

// The first array member named `key` anywhere under `value`.
yyjson_val* findArray(yyjson_val* value, const char* key) {
    if (yyjson_is_obj(value)) {
        if (auto* found = yyjson_obj_get(value, key); yyjson_is_arr(found))
            return found;
        yyjson_obj_iter iter = yyjson_obj_iter_with(value);
        while (auto* name = yyjson_obj_iter_next(&iter))
            if (auto* found = findArray(yyjson_obj_iter_get_val(name), key))
                return found;
    } else if (yyjson_is_arr(value)) {
        yyjson_arr_iter iter = yyjson_arr_iter_with(value);
        while (auto* item = yyjson_arr_iter_next(&iter))
            if (auto* found = findArray(item, key))
                return found;
    }
    return nullptr;
}

std::uint64_t outputId(const std::string& response, const std::string_view name) {
    const JsonResponse parsed(response);
    auto* outputs = findArray(parsed.content(), "outputs");
    yyjson_arr_iter iter = yyjson_arr_iter_with(outputs);
    while (auto* item = yyjson_arr_iter_next(&iter))
        if (const auto* text = yyjson_get_str(yyjson_obj_get(item, "name")); text && text == name)
            return yyjson_get_uint(yyjson_obj_get(item, "id"));
    return 0;
}

// The `entries` record of one slot, or null.
yyjson_val* entryOf(yyjson_val* content, const std::uint64_t slot) {
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(content, "entries"));
    while (auto* item = yyjson_arr_iter_next(&iter))
        if (yyjson_get_uint(yyjson_obj_get(item, "slot")) == slot)
            return item;
    return nullptr;
}

yyjson_val* layerRecord(yyjson_val* content, const std::uint64_t layer) {
    yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(content, "records"));
    while (auto* item = yyjson_arr_iter_next(&iter))
        if (yyjson_get_uint(yyjson_obj_get(item, "id")) == layer)
            return item;
    return nullptr;
}

double roleNumber(yyjson_val* entry, const char* group, const char* role) {
    return yyjson_get_num(yyjson_obj_get(yyjson_obj_get(entry, group), role));
}

std::string text(yyjson_val* value, const char* key) {
    const auto* found = yyjson_get_str(yyjson_obj_get(value, key));
    return found ? std::string(found) : std::string{};
}

void mergeEntryReadBack() {
    Headless server;
    const auto transact = [&](const std::string& operations) {
        return server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                           R"(,"operations":[)" + operations + "]}");
    };
    const auto layers = [&] { return server.call("query", R"({"kind":"layers","composition":1})"); };
    std::uint64_t merge = 0;
    {
        const JsonResponse nodes(server.call("query", R"({"kind":"nodes","composition":1})"));
        yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_obj_get(nodes.content(), "records"));
        while (auto* node = yyjson_arr_iter_next(&iter))
            if (text(node, "typeId") == "bloom.layer-stack")
                merge = yyjson_get_uint(yyjson_obj_get(node, "id"));
    }
    expect(merge != 0, "the composition's Merge is listed");
    const auto mergeText = std::to_string(merge);
    auto result = transact(R"({"op":"bloom.node.add","args":{"composition":1,)"
                           R"("nodeType":"bloom.solid-source"}})");
    const auto solid = outputId(result, "node");
    result = transact(R"({"op":"bloom.node.connect","args":{"composition":1,"sourceNode":)" +
                      std::to_string(solid) + R"(,"sourcePort":"image","targetNode":)" +
                      mergeText + R"(,"role":"content"}})");
    const auto slot = outputId(result, "layerSlot");
    expect(solid != 0 && slot != 0, "a plain entry is wired: " + result);
    const auto slotText = std::to_string(slot);
    {
        const auto response = layers();
        const JsonResponse read(response);
        auto* entry = entryOf(read.content(), slot);
        expect(entry != nullptr && text(entry, "layerKind") == "none" &&
                   yyjson_is_null(yyjson_obj_get(entry, "resolvedLayer")) &&
                   !yyjson_get_bool(yyjson_obj_get(entry, "entryLayer")) &&
                   roleNumber(entry, "values", "opacity") == 1.0 &&
                   yyjson_get_uint(yyjson_obj_get(entry, "sourceNode")) == solid &&
                   yyjson_get_bool(yyjson_obj_get(entry, "outputMerge")),
               "a plain entry reads back with no layer and its default values: " + response);
    }

    // One transact sets the plain entry's opacity; its layer is created in the same step.
    result = transact(R"({"op":"bloom.merge.set-entry-parameter","args":{"composition":1,)"
                      R"("merge":)" +
                      mergeText + R"(,"slot":)" + slotText +
                      R"(,"role":"opacity","value":0.5}})");
    expect(contains(result, R"("isError":false)"), "set-entry-parameter applies: " + result);
    const auto entryLayer = outputId(result, "layer");
    const auto opacityId = outputId(result, "parameter.opacity");
    {
        const auto response = layers();
        const JsonResponse read(response);
        auto* entry = entryOf(read.content(), slot);
        auto* record = layerRecord(read.content(), entryLayer);
        expect(entry != nullptr && text(entry, "layerKind") == "entry" &&
                   yyjson_get_bool(yyjson_obj_get(entry, "entryLayer")) &&
                   yyjson_get_uint(yyjson_obj_get(entry, "resolvedLayer")) == entryLayer &&
                   roleNumber(entry, "values", "opacity") == 0.5 &&
                   yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(entry, "parameters"),
                                                  "opacity")) == opacityId &&
                   text(entry, "pivot") == "frame",
               "the entry reads back with its entry layer and opacity: " + response);
        expect(record != nullptr && yyjson_get_bool(yyjson_obj_get(record, "entryLayer")) &&
                   yyjson_is_null(yyjson_obj_get(record, "layerOutputNode")) &&
                   yyjson_get_uint(yyjson_obj_get(record, "sourceNode")) == solid &&
                   yyjson_get_uint(yyjson_obj_get(record, "slot")) == slot &&
                   yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(record, "parameters"),
                                                  "opacity")) == opacityId &&
                   roleNumber(record, "values", "opacity") == 0.5 &&
                   !yyjson_get_bool(yyjson_obj_get(record, "shared")),
               "the entry layer is a layers record with its own parameters: " + response);
    }
    const auto parameter = server.call(
        "query", R"({"kind":"parameter","composition":1,"parameter":)" +
                     std::to_string(opacityId) + R"(,"expectedRevision":)" + revisionOf(server) +
                     "}");
    expect(contains(parameter, R"("layer":)" + std::to_string(entryLayer)) &&
               contains(parameter, R"("role":"opacity")") && contains(parameter, R"("maximum")"),
           "an entry layer parameter names its owner and the Layer node's metadata: " +
               parameter);

    // A layer edit by merge and slot, read back, and undone in one step.
    result = transact(R"({"op":"bloom.layer.set-solo","args":{"composition":1,"merge":)" +
                      mergeText + R"(,"slot":)" + slotText + R"(,"solo":true}})");
    expect(contains(result, R"("isError":false)"), "set-solo by entry applies: " + result);
    {
        const JsonResponse read(layers());
        auto* entry = entryOf(read.content(), slot);
        expect(entry && yyjson_get_bool(yyjson_obj_get(entry, "solo")), "solo reads back");
    }
    result = server.call("history",
                         R"({"action":"undo","expectedRevision":)" + revisionOf(server) + "}");
    {
        const JsonResponse read(layers());
        auto* entry = entryOf(read.content(), slot);
        expect(entry && !yyjson_get_bool(yyjson_obj_get(entry, "solo")),
               "undo clears the solo: " + result);
    }

    // An Adjustment layer, read back as the top entry with its marker.
    result = transact(R"({"op":"bloom.merge.add-adjustment-layer","args":{"composition":1,)"
                      R"("merge":)" +
                      mergeText + "}}");
    const auto adjustment = outputId(result, "node");
    const auto adjustmentSlot = outputId(result, "layerSlot");
    expect(adjustment != 0 && adjustmentSlot != 0, "add-adjustment-layer applies: " + result);
    {
        const auto response = layers();
        const JsonResponse read(response);
        auto* entry = entryOf(read.content(), adjustmentSlot);
        expect(entry != nullptr && yyjson_get_uint(yyjson_obj_get(entry, "index")) == 0 &&
                   yyjson_get_uint(yyjson_obj_get(entry, "adjustment")) == adjustment &&
                   text(entry, "name") == "Adjustment Layer 1" &&
                   text(entry, "layerKind") == "entry",
               "the Adjustment layer reads back at the top: " + response);
        auto* record = layerRecord(
            read.content(), yyjson_get_uint(yyjson_obj_get(entry, "resolvedLayer")));
        expect(record != nullptr &&
                   yyjson_get_uint(yyjson_obj_get(record, "adjustment")) == adjustment,
               "the adjustment layer's record carries the marker: " + response);
    }
    const auto nodes = server.call("query", R"({"kind":"nodes","composition":1})");
    expect(contains(nodes, R"("adjustment":)" + std::to_string(adjustment)) &&
               contains(nodes, R"("layerKind":"entry")"),
           "the Merge's stack reports the entries: " + nodes);

    // Insert a Layer node: it adopts the entry's opacity and becomes its layer.
    result = transact(R"({"op":"bloom.merge.insert-layer-node","args":{"composition":1,)"
                      R"("merge":)" +
                      mergeText + R"(,"slot":)" + slotText + "}}");
    const auto layerNode = outputId(result, "node");
    expect(layerNode != 0, "insert-layer-node applies: " + result);
    {
        const auto response = layers();
        const JsonResponse read(response);
        auto* entry = entryOf(read.content(), slot);
        expect(entry != nullptr && text(entry, "layerKind") == "node" &&
                   yyjson_get_uint(yyjson_obj_get(entry, "layerNode")) == layerNode &&
                   roleNumber(entry, "values", "opacity") == 0.5 &&
                   layerRecord(read.content(), entryLayer) == nullptr,
               "the Layer node adopted the entry's opacity: " + response);
    }
    result = transact(R"({"op":"bloom.layer.set-shy","args":{"composition":1,"merge":)" +
                      mergeText + R"(,"shy":true}})");
    expect(contains(result, R"("isError":true)"), "merge without slot is refused: " + result);
}

// #434 O2/O6: transact reads `null` as an omitted argument (`"parent": null` clears a parent) and
// passes a path object through a parameter `value` to the operation instead of refusing it at the
// protocol.
void fieldTestArguments() {
    Headless server;
    const auto transact = [&](const std::string& operations) {
        return server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                           R"(,"operations":[)" + operations + "]}");
    };
    for (const auto* name : {"Parent", "Child"})
        static_cast<void>(transact(R"({"op":"bloom.layer.add-solid","args":{"composition":1,)"
                                   R"("name":")" +
                                   std::string(name) + R"(","color":[1,0,0,1]}})"));
    const auto layers = server.call("query", R"({"kind":"layers","composition":1})");
    std::vector<std::string> ids;
    const std::regex id(R"re(\{"id":(\d+),)re");
    for (auto match = std::sregex_iterator(layers.begin(), layers.end(), id);
         match != std::sregex_iterator(); ++match)
        ids.push_back((*match)[1].str());
    expect(ids.size() >= 2, "two layers to parent: " + layers);
    if (ids.size() < 2)
        return;
    auto result = transact(R"({"op":"bloom.layer.set-parent","args":{"composition":1,"layer":)" +
                           ids[1] + R"(,"parent":)" + ids[0] + "}}");
    expect(contains(result, R"("succeeded":true)"), "the child is parented: " + result);
    result = transact(R"({"op":"bloom.layer.set-parent","args":{"composition":1,"layer":)" +
                      ids[1] + R"(,"parent":null}})");
    expect(contains(result, R"("isError":false)") && contains(result, R"("succeeded":true)"),
           "parent null clears the parent: " + result);
    const auto nodes = server.call("query", R"({"kind":"nodes","composition":1})");
    const auto opacity = firstMatch(nodes, R"re("opacity":(\d+))re");
    result = transact(R"({"op":"bloom.parameter.set-source","args":{"composition":1,"parameter":)" +
                      opacity +
                      R"(,"value":{"closed":true,"anchors":[{"point":[0,0]},{"point":[10,0]}]}}})");
    expect(!contains(result, "expects value") && contains(result, "cannot take a path"),
           "a path value reaches the parameter and is judged by its kind: " + result);
}

// #541: a Colorize's gradient is set as {"stops": [{position, color}]} and reads back the same way.
void gradientReadBack() {
    Headless server;
    const auto transact = [&](const std::string& operations) {
        return server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                           R"(,"operations":[)" + operations + "]}");
    };
    auto result = transact(R"({"op":"bloom.node.add","args":{"composition":1,)"
                           R"("nodeType":"bloom.colorize"}})");
    expect(outputId(result, "node") != 0, "a Colorize is added: " + result);
    const auto nodes = server.call("query", R"({"kind":"nodes","composition":1})");
    const auto gradient = firstMatch(nodes, R"re("gradient":(\d+))re");
    expect(!gradient.empty(), "the Colorize lists its gradient parameter: " + nodes);
    if (gradient.empty())
        return;
    result = transact(R"({"op":"bloom.parameter.set-source","args":{"composition":1,"parameter":)" +
                      gradient +
                      R"(,"value":{"stops":[{"position":0,"color":[0,0,0,1]},)"
                      R"({"position":0.4,"color":[1,0.5,0,1]},)"
                      R"({"position":1,"color":[1,1,1,1]}]}}})");
    expect(contains(result, R"("isError":false)") && contains(result, R"("succeeded":true)"),
           "a gradient value is accepted: " + result);
    const auto read = server.call(
        "query", R"({"kind":"parameter","composition":1,"parameter":)" + gradient + "}");
    expect(contains(read, R"("stops")") && contains(read, R"("position":0.4)") &&
               contains(read, R"("valueKind":"gradient")"),
           "the gradient reads back as its stops: " + read);
    result = transact(R"({"op":"bloom.parameter.set-source","args":{"composition":1,"parameter":)" +
                      gradient + R"(,"value":{"stops":[]}}})");
    expect(contains(result, "A gradient has 1 to 64 stops"),
           "a gradient without stops is refused, saying why: " + result);
    // #546: the blend reads back by name, defaults included, and is set by name.
    expect(contains(read, R"("interpolation":"linear")") &&
               contains(read, R"("colorMode":"rgb")") && contains(read, R"("hueDirection":"near")"),
           "the gradient reads back its blend: " + read);
    result = transact(R"({"op":"bloom.parameter.set-source","args":{"composition":1,"parameter":)" +
                      gradient +
                      R"(,"value":{"stops":[{"position":0,"color":[1,0,0,1]},)"
                      R"({"position":1,"color":[0,0,1,1]}],"interpolation":"b-spline",)"
                      R"("colorMode":"hsv","hueDirection":"counter-clockwise"}}})");
    expect(contains(result, R"("succeeded":true)"), "a gradient blend is accepted: " + result);
    const auto blended = server.call(
        "query", R"({"kind":"parameter","composition":1,"parameter":)" + gradient + "}");
    expect(contains(blended, R"("interpolation":"b-spline")") &&
               contains(blended, R"("colorMode":"hsv")") &&
               contains(blended, R"("hueDirection":"counter-clockwise")"),
           "the blend reads back as set: " + blended);
    result = transact(R"({"op":"bloom.parameter.set-source","args":{"composition":1,"parameter":)" +
                      gradient +
                      R"(,"value":{"stops":[{"position":0,"color":[1,0,0,1]}],)"
                      R"("interpolation":"bezier"}}})");
    expect(contains(result, "A gradient's interpolation is"),
           "an unknown interpolation is refused, saying which are known: " + result);
}

// #547: a Curves point list takes an auto (or corner) anchor with null handles as a plain point; an
// invalid one is refused naming the anchor, the shape a curve takes and what was received.
void curvesAutoAnchors() {
    Headless server;
    const auto transact = [&](const std::string& operations) {
        return server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                           R"(,"operations":[)" + operations + "]}");
    };
    auto result = transact(R"({"op":"bloom.node.add","args":{"composition":1,)"
                           R"("nodeType":"bloom.curves"}})");
    expect(outputId(result, "node") != 0, "a Curves node is added: " + result);
    const auto nodes = server.call("query", R"({"kind":"nodes","composition":1})");
    const auto master = firstMatch(nodes, R"re("master":(\d+))re");
    expect(!master.empty(), "the Curves node lists its master curve: " + nodes);
    if (master.empty())
        return;
    const auto set = [&](const std::string& anchors) {
        return transact(
            R"({"op":"bloom.parameter.set-source","args":{"composition":1,"parameter":)" + master +
            R"(,"value":{"closed":false,"anchors":)" + anchors + "}}}");
    };
    result = set(R"([{"point":[0,0],"mode":"auto","inHandle":null,"outHandle":null},)"
                 R"({"point":[0.5,0.7],"mode":"auto","inHandle":null,"outHandle":null},)"
                 R"({"point":[1,1],"mode":"corner"}])");
    expect(contains(result, R"("isError":false)") && contains(result, R"("succeeded":true)"),
           "auto anchors with null handles are accepted: " + result);
    const auto read = server.call(
        "query", R"({"kind":"parameter","composition":1,"parameter":)" + master + "}");
    expect(contains(read, R"("point":[0.5,0.7])") && !contains(read, R"("mode":"auto")"),
           "the points read back as plain curve points: " + read);
    result = set(R"([{"point":[0,0],"mode":"auto","inHandle":[0.1,0.1]}])");
    expect(contains(result, "anchor 0 has a handle") && contains(result, "point:[input, output]"),
           "a curve anchor with a handle is refused, saying what a curve takes: " + result);
    result = set(R"([{"point":[0,0]},{"point":[2000000,0]}])");
    expect(contains(result, "anchor 1 input 2e+06 is outside"),
           "a point outside the domain is refused, naming it: " + result);
    result = set(R"([{"point":[0,0],"mode":"free"}])");
    expect(contains(result, "anchor 0 is in free mode"),
           "a free anchor is refused, naming its mode: " + result);
}

// #470: a locked layer still takes Enable/Disable, Solo, Shy and Unlock over MCP; its timing,
// masks and other content are refused with the layer named.
void lockedLayerRule() {
    Headless server;
    const auto transact = [&](const std::string& operations) {
        return server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                           R"(,"operations":[)" + operations + "]}");
    };
    const auto added = transact(R"({"op":"bloom.layer.add-solid","args":{"composition":1,)"
                                R"("name":"Locked","color":[1,0,0,1]}})");
    const auto layer = std::to_string(outputId(added, "layer"));
    expect(layer != "0", "a layer is added: " + added);
    const auto on = [&](const std::string& op, const std::string& extra) {
        return transact(R"({"op":"bloom.layer.)" + op + R"(","args":{"composition":1,"layer":)" +
                        layer + extra + "}}");
    };
    auto result = on("set-locked", R"(,"locked":true)");
    expect(contains(result, R"("succeeded":true)"), "the layer locks: " + result);
    for (const auto& [op, extra] : {std::pair<std::string, std::string>{"set-enabled", R"(,"enabled":false)"},
                                    {"set-solo", R"(,"solo":true)"},
                                    {"set-shy", R"(,"shy":true)"}}) {
        result = on(op, extra);
        expect(contains(result, R"("succeeded":true)"),
               "a locked layer takes " + op + ": " + result);
    }
    result = on("set-range", R"(,"start":1,"end":5)");
    expect(contains(result, R"("succeeded":false)") && contains(result, "is locked"),
           "a locked layer refuses a new range, saying why: " + result);
    result = on("set-locked", R"(,"locked":false)");
    expect(contains(result, R"("succeeded":true)"), "the layer unlocks: " + result);
    result = on("set-range", R"(,"start":1,"end":5)");
    expect(contains(result, R"("succeeded":true)"), "an unlocked layer takes the range: " + result);
}

// profiler.snapshot (#423): unbound it says so; bound it returns the snapshot as JSON with
// schemaVersion, absent values as null, and the fixed histogram and pool shapes.
void profilerSnapshotTool() {
    {
        Headless unbound;
        const auto result = unbound.call("profiler.snapshot", "{}");
        expect(contains(result, R"("succeeded":false)") &&
                   contains(result, "This host has no profiler."),
               "an unbound profiler answers plainly: " + result);
    }
    runtime::TaskScheduler scheduler;
    runtime::ProfilerSampler sampler({.scheduler = &scheduler, .readGpu = nullptr});
    int calls = 0;
    Headless server({}, [&](runtime::ProfilerSnapshot& out) {
        ++calls;
        static_cast<void>(sampler.sampleNow());
        return sampler.readLatest(out, 0);
    });
    const auto refused = server.call("profiler.snapshot", R"({"unknown":1})");
    expect(contains(refused, "Unknown member"), "the tool takes no arguments: " + refused);
    const auto response = server.call("profiler.snapshot", "{}");
    expect(calls == 1, "the binding runs once per call");
    auto* document = yyjson_read(response.data(), response.size(), 0);
    expect(document != nullptr, "the response is JSON");
    if (document == nullptr) {
        scheduler.beginShutdown();
        return;
    }
    auto* root = yyjson_doc_get_root(document);
    auto* content = yyjson_obj_getn(yyjson_obj_get(root, "result"), "structuredContent", 17);
    expect(yyjson_is_true(yyjson_obj_get(content, "succeeded")), "the call succeeded: " + response);
    auto* snapshot = yyjson_obj_get(content, "snapshot");
    expect(yyjson_get_uint(yyjson_obj_get(snapshot, "schemaVersion")) ==
               runtime::kProfilerSnapshotSchemaVersion,
           "the snapshot carries its schema version");
    expect(yyjson_get_uint(yyjson_obj_get(snapshot, "serial")) >= 1, "a sample was taken");
    auto* memory = yyjson_obj_get(snapshot, "memory");
    expect(yyjson_is_arr(yyjson_obj_get(memory, "pools")), "pools are an array");
    expect(yyjson_is_uint(yyjson_obj_get(memory, "stagingLiveBytes")),
           "the staging line is always counted");
    expect(yyjson_is_null(yyjson_obj_get(memory, "admissionClaimBytes")),
           "claims without an admission source are null, not 0");
    auto* gpu = yyjson_obj_get(snapshot, "gpu");
    expect(yyjson_is_null(yyjson_obj_get(gpu, "liveBytes")) &&
               yyjson_is_null(yyjson_obj_get(gpu, "utilization")),
           "GPU values without a GPU source are null");
    auto* stages = yyjson_obj_get(yyjson_obj_get(snapshot, "workload"), "stages");
    expect(yyjson_get_uint(yyjson_obj_get(stages, "firstEdgeNs")) == 16'000,
           "the histogram's first edge is 16 us");
    for (const char* stage : {"decode", "colourConvert", "upload", "render", "transfer", "encode"})
        expect(yyjson_arr_size(yyjson_obj_get(yyjson_obj_get(stages, stage), "counts")) ==
                   runtime::kProfilerHistogramBucketCount,
               std::string("every stage has the fixed buckets: ") + stage);
    auto* executors = yyjson_obj_get(yyjson_obj_get(snapshot, "cpu"), "executorPools");
    expect(yyjson_arr_size(executors) >= 3, "executor pools are listed");
    expect(yyjson_is_arr(yyjson_obj_get(yyjson_obj_get(snapshot, "processes"), "helpers")),
           "helpers are an array");
    yyjson_doc_free(document);
    scheduler.beginShutdown();
}

// Input that never arrives: a read blocks until released, then reports end of input.
class IdleInput final : public std::streambuf {
  public:
    bool waitUntilBlocked(const std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return blocked_; });
    }
    void release() {
        {
            const std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

  protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        blocked_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_; });
        return traits_type::eof();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool blocked_ = false;
    bool released_ = false;
};

// The stdio transport returns when its stop token fires while stdin is idle, instead of waiting
// for the next line.
void stdioStopsWhileInputIdle() {
    Headless world;
    // Static: a reader left blocked on idle input outlives serveStdio() and ends when released.
    static IdleInput idle;
    static std::istream input(&idle);
    std::ostringstream output;
    std::stop_source stop;
    std::promise<void> done;
    auto returned = done.get_future();
    std::thread serving([&] {
        mcp::serveStdio(*world.core, input, output, stop.get_token());
        done.set_value();
    });
    expect(idle.waitUntilBlocked(std::chrono::seconds(10)), "the reader waits on idle input");
    stop.request_stop();
    const bool stopped = returned.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    expect(stopped, "stdio stops while input is idle");
    idle.release(); // Ends the reader; without the fix this is also what lets serveStdio return.
    serving.join();
    expect(output.str().empty(), "nothing was answered without a request");
}

// Name and role references, query filters and projection, id strings, set-keys and frame statistics.
void mcpFold() {
    const auto captures = std::filesystem::temp_directory_path() /
                          ("bloom-mcp-fold-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Headless server({}, {}, captures);
        const auto transact = [&](const std::string& operations) {
            return server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                               R"(,"operations":[)" + operations + "]}");
        };
        auto result = transact(R"({"op":"bloom.layer.add-solid","args":{"composition":1,)"
                               R"("name":"Wash","color":[0.25,0.5,0.75,1]}})");
        expect(contains(result, R"("isError":false)"), "the solid applies: " + result);
        const auto layers = server.call("query", R"({"kind":"layers","composition":1})");
        const auto layer = firstMatch(layers, R"re("records":\[\{"id":(\d+))re");

        // Ids as strings, and a layer by name.
        result = transact(R"({"op":"bloom.layer.rename","args":{"composition":"1","layer":")" + layer +
                          R"(","name":"Wash 2"}})");
        expect(contains(result, R"("isError":false)"), "string ids apply: " + result);
        result = transact(R"({"op":"bloom.layer.rename","args":{"composition":1,)"
                          R"("layer":{"name":"Wash 2"},"name":"Wash"}})");
        expect(contains(result, R"("isError":false)"), "a layer by name applies: " + result);
        result = transact(R"({"op":"bloom.layer.rename","args":{"composition":1,)"
                          R"("layer":{"name":"Missing"},"name":"Wash"}})");
        expect(contains(result, "No layer is named") && contains(result, "Wash"),
               "an unknown name lists the layers: " + result);

        // A parameter by layer and role; set-keys is one step.
        result = transact(R"({"op":"bloom.animation.set-keys","args":{"composition":1,)"
                          R"("parameter":{"layer":"Wash","role":"opacity"},)"
                          R"("keys":[[0,0.0],[12,1.0,"hold"]]}})");
        expect(contains(result, R"("isError":false)"), "set-keys by role applies: " + result);

        // #604: columns instead of rows, a column read from a file, and a summary answer.
        result = transact(R"({"op":"bloom.animation.set-key-columns","args":{"composition":1,)"
                          R"("parameter":{"layer":"Wash","role":"opacity"},"replace":true,)"
                          R"("times":[0,6,12],"values":[0.0,0.5,1.0],)"
                          R"("interpolations":["hold"]}})");
        expect(contains(result, R"("isError":false)") && contains(result, R"(\"keyframe\")"),
               "columnar set-keys applies and lists the keys it made: " + result);
        const auto dataFolder = captures / "data";
        std::filesystem::create_directories(dataFolder);
        {
            std::ofstream(dataFolder / "times.json") << "[0,4,8,12]";
            std::ofstream(dataFolder / "values.json") << "[1.0,0.25,0.75,0.0]";
            std::ofstream(dataFolder / "notes.txt") << "[1]";
        }
        const auto fromFile = [&](const std::string& name) {
            return R"({"fromFile":")" + (dataFolder / name).generic_string() + R"("})";
        };
        const auto bulk = [&](const std::string& times, const std::string& tail) {
            return R"({"op":"bloom.animation.set-key-columns","args":{"composition":1,)"
                   R"("parameter":{"layer":"Wash","role":"opacity"},"replace":true,"times":)" +
                   times + R"(,"values":)" + fromFile("values.json") + "}}" + tail;
        };
        result = server.call("transact",
                             R"({"expectedRevision":)" + revisionOf(server) +
                                 R"(,"response":"summary","operations":[)" +
                                 bulk(fromFile("times.json"), "") + "]}");
        expect(contains(result, R"("isError":false)") && contains(result, "outputSummary") &&
                   contains(result, "sha256:") && contains(result, R"("byName":{)") &&
                   !contains(result, R"("outputs":)"),
               "keys read from files answer with a summary and no id list: " + result);
        const auto washNodes = server.call("query", R"({"kind":"nodes","composition":1})");
        const auto washOpacity = firstMatch(washNodes, R"re("opacity":(\d+))re");
        const auto keysBack = server.call(
            "query", R"({"kind":"keyframes","composition":1,"parameter":)" + washOpacity + "}");
        expect(contains(keysBack, "0.25") && contains(keysBack, "0.75"),
               "the file's values are the keys: " + keysBack);
        // Back to the fixture's keys for the checks that follow.
        result = transact(R"({"op":"bloom.animation.set-keys","args":{"composition":1,)"
                          R"("parameter":{"layer":"Wash","role":"opacity"},"replace":true,)"
                          R"("keys":[[0,0.0],[12,1.0,"hold"]]}})");
        expect(contains(result, R"("isError":false)"), "the fixture keys are restored: " + result);
        result = server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                             R"(,"operations":[)" +
                                             bulk(fromFile("notes.txt"), "") + "]}");
        expect(contains(result, ".json"), "a data file must be .json: " + result);
        result = server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                             R"(,"operations":[)" +
                                             bulk(R"({"fromFile":"relative.json"})", "") + "]}");
        expect(contains(result, "absolute path"), "a data file path must be absolute: " + result);
        result = transact(R"({"op":"bloom.animation.set-key-columns","args":{"composition":1,)"
                          R"("parameter":{"layer":"Wash","role":"opacity"},)"
                          R"("times":[0,6],"values":[0.0]}})");
        expect(contains(result, "has 1 entries but times has 2"),
               "columns of different lengths are refused by name: " + result);
        result = server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                             R"(,"response":"brief","operations":[]})");
        expect(contains(result, "response is"), "an unknown response is refused: " + result);

        // Filters and projection.
        const auto operations = server.call("query", R"({"kind":"operations","idPrefix":"bloom.layer."})");
        expect(contains(operations, "bloom.layer.add-solid") &&
                   !contains(operations, "bloom.animation.set-keys"),
               "operations filter by id prefix");
        const auto projected =
            server.call("query", R"({"kind":"layers","composition":1,"fields":["id","name"]})");
        expect(contains(projected, R"("records":[{"id":1,"name":"Wash"}])"),
               "fields keeps only the named members: " + projected);
        const auto badField =
            server.call("query", R"({"kind":"layers","composition":1,"fields":["nope"]})");
        expect(contains(badField, "Unknown field nope"),
               "an unknown field is refused: " + badField);
        const auto nodes = server.call("query", R"({"kind":"nodes","composition":1,"layer":")" +
                                                    layer + R"("})");
        expect(contains(nodes, R"("isError":false)"), "nodes filter by layer: " + nodes);

        // Frame statistics on a known solid: the working-space values come back unchanged.
        const auto stats = server.call("capture", R"({"composition":1,"frame":12,"last":13,)"
                                                  R"("statistics":true,"region":[10,10,32,32],)"
                                                  R"("bins":4})");
        expect(contains(stats, R"("isError":false)"), "statistics render: " + stats);
        const auto number = R"re(([0-9.eE+-]+))re";
        const auto close = [&](const std::string& pattern, const double expected) {
            const auto text = firstMatch(stats, pattern + number);
            return !text.empty() && std::abs(std::stod(text) - expected) < 0.01;
        };
        expect(close(R"re("red":\{"mean":)re", 0.25) && close(R"re("green":\{"mean":)re", 0.5) &&
                   close(R"re("blue":\{"mean":)re", 0.75) &&
                   close(R"re("luma":\{"mean":)re", 0.46495),
               "the channel means match the solid: " + stats);
        expect(contains(stats, R"("pixels":2048)") && contains(stats, R"("colorSpace":")") &&
                   contains(stats, R"("frames":[{"frame":12)"),
               "the region, the working space and the per-frame rows are reported: " + stats);
        const auto outside = server.call("capture", R"({"composition":1,"frame":0,"statistics":true,)"
                                                    R"("region":[0,0,99999,10]})");
        expect(contains(outside, "region must be inside"), "a region outside the frame is refused: " + outside);
        const auto stray = server.call("capture", R"({"composition":1,"frame":0,"region":[0,0,1,1]})");
        expect(contains(stray, "need statistics=true"), "region needs statistics: " + stray);

        // Pixel values at a time, read through the production still route.
        const auto folderContents = [&] {
            std::error_code listing;
            std::vector<std::string> names;
            for (const auto& entry : std::filesystem::directory_iterator(captures, listing))
                names.push_back(entry.path().filename().string());
            std::ranges::sort(names);
            return names;
        };
        const auto before = folderContents();
        const auto pixels = server.call(
            "query", R"({"kind":"pixels","composition":1,"time":12,"region":[10,10,2,1]})");
        expect(contains(pixels, R"("isError":false)"), "pixels read: " + pixels);
        const auto first = firstMatch(pixels, R"re("pixels":\[\[([0-9.eE+-]+),([0-9.eE+-]+),([0-9.eE+-]+),([0-9.eE+-]+)\])re");
        expect(!first.empty() && std::abs(std::stod(first) - 0.25) < 0.01,
               "the first pixel's red matches the solid: " + pixels);
        expect(contains(pixels, R"("colorSpace":")") && contains(pixels, R"("frame":12)") &&
                   contains(pixels, R"("region":[10,10,2,1])") && contains(pixels, R"("width":2)") &&
                   contains(pixels, R"("height":1)") && contains(pixels, "premultiplied"),
               "the answer names the space, the frame and the layout of the numbers: " + pixels);
        expect(contains(pixels, R"("provenance":"cpu-reference")") &&
                   contains(pixels, R"("gpuFrames":0)") && contains(pixels, "CPU reference path"),
               "a read with no GPU device says it ran on the CPU reference path: " + pixels);
        expect(folderContents() == before, "pixels leaves nothing on disk");
        const auto many = server.call("query", R"({"kind":"pixels","composition":1,"time":0,"region":[0,0,17,16]})");
        expect(contains(many, "-32602") && contains(many, "at most 256"),
               "a region over the hard limit is refused: " + many);
        const auto beyond = server.call("query", R"({"kind":"pixels","composition":1,"time":0,"region":[0,0,99999,1]})");
        expect(contains(beyond, "region must be inside"), "a region outside the frame is refused: " + beyond);
        const auto late = server.call("query", R"({"kind":"pixels","composition":1,"time":999999,"region":[0,0,1,1]})");
        expect(contains(late, "time is outside the composition"), "a time outside the composition is refused: " + late);
        const auto noRegion = server.call("query", R"({"kind":"pixels","composition":1})");
        expect(contains(noRegion, "Missing member: region"), "pixels needs a region: " + noRegion);
    }
    std::error_code ignored;
    std::filesystem::remove_all(captures, ignored);
}

// A second project file edited, saved and exported in its own session while the server's own
// project stays untouched; two sessions on one file are refused.
void secondProjectFile() {
    const auto folder = std::filesystem::temp_directory_path() /
                        ("bloom-mcp-file-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto second = folder / "second.bloom";
    const auto own = folder / "own.bloom";
    {
        auto created = scripting::Session::createNew("Second");
        expect(static_cast<bool>(created), "the second project is created");
        auto session = std::move(created).takeSession();
        expect(session->saveAs(second).succeeded(), "the second project is saved");
    }
    {
        Headless server({}, {}, folder / "captures");
        expect(server.session->saveAs(own).succeeded(), "the server's own project is saved");
        const auto file = [&](const std::string& rest) {
            const auto path = second.generic_string();
            return R"({"projectFile":")" + path + '"' + rest + "}";
        };
        const auto ownRevision = revisionOf(server);
        const auto before = server.call("query", file(R"(,"kind":"project")"));
        const auto revision = firstMatch(before, R"re("revision":(\d+))re");
        expect(contains(before, R"("isError":false)") && !revision.empty(),
               "a second project reads through projectFile: " + before);
        const auto edited = server.call(
            "transact",
            file(R"(,"expectedRevision":)" + revision +
                 R"(,"operations":[{"op":"bloom.layer.add-solid","args":{"composition":1,)"
                 R"("name":"Other","color":[0,1,0,1]}}])"));
        expect(contains(edited, R"("isError":false)"), "the second project edits: " + edited);
        expect(revisionOf(server) == ownRevision, "the server's own project is untouched");
        const auto layers = server.call("query", file(R"(,"kind":"layers","composition":1)"));
        expect(contains(layers, R"("name":"Other")"), "the edit is in the second session: " + layers);
        expect(!contains(server.call("query", R"({"kind":"layers","composition":1})"), "Other"),
               "and not in the server's project");
        const auto undone = server.call("history", file(R"(,"action":"undo","expectedRevision":)" +
                                                        firstMatch(edited, R"re("revision":(\d+))re")));
        expect(contains(undone, R"("isError":false)") || contains(undone, "undo"),
               "history reaches the second session: " + undone);
        const auto redone = server.call(
            "history", file(R"(,"action":"redo","expectedRevision":)" +
                            firstMatch(undone, R"re("revision":(\d+))re")));
        expect(contains(redone, R"("isError":false)"), "and redoes: " + redone);

        const auto closeDirty = server.call("project.close", file(""));
        expect(contains(closeDirty, "unsaved changes"), "closing unsaved edits is refused: " + closeDirty);
        const auto exported = server.call(
            "render", file(R"(,"composition":1,"frame":0,"preset":"PngRgba8SrgbV1","destination":")" +
                           (folder / "out.png").generic_string() + R"(")"));
        expect(contains(exported, R"("isError":false)") &&
                   std::filesystem::exists(folder / "out.png"),
               "the second project exports: " + exported);
        const auto saved = server.call("project.save", file(""));
        expect(contains(saved, R"("saved":true)"), "the second project saves: " + saved);
        const auto closed = server.call("project.close", file(""));
        expect(contains(closed, R"("closed":true)"), "and closes: " + closed);

        // The file on disk carries the edit.
        auto reopened = scripting::Session::open(second);
        expect(static_cast<bool>(reopened), "the saved file opens");
        if (reopened) {
            const auto session = std::move(reopened).takeSession();
            const auto* composition = session->snapshot().project().findComposition(
                document::CompositionId::fromRaw(1));
            expect(composition != nullptr && composition->graph().layerOutputs().size() == 1,
                   "the saved file holds the new layer");
        }

        // The file the server itself has open is refused.
        const auto ownFile = server.call(
            "query", R"({"kind":"project","projectFile":")" + own.generic_string() + R"("})");
        expect(contains(ownFile, "already open"), "a file open as the project is refused: " + ownFile);
        const auto relative = server.call("query", R"({"kind":"project","projectFile":"a.bloom"})");
        expect(contains(relative, "absolute path"), "a relative path is refused: " + relative);
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// Another project file edited and exported through the desktop host path while the application's
// own project stays live: the file is refused when Bloom has it open (at first use, after it
// became live, and when Bloom cannot be asked), project.open and project.save-as refuse a file a
// session holds, and a render with no GPU device says it ran on the CPU reference path.
void fileSessionsBesideALiveProject() {
    const auto folder = std::filesystem::temp_directory_path() /
                        ("bloom-mcp-live-file-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto second = folder / "second.bloom";
    const auto third = folder / "third.bloom";
    const auto live = folder / "live.bloom";
    for (const auto& path : {second, third}) {
        auto created = scripting::Session::createNew("Other");
        expect(static_cast<bool>(created), "a project file is created");
        auto session = std::move(created).takeSession();
        expect(session->saveAs(path).succeeded(), "a project file is saved");
    }
    {
        // The provider the application passes for file sessions only; it has no device here.
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = false;
        Live server({}, {}, {}, host::GpuExportProvider::create(std::move(options)));
        server.bindProject(live);
        const auto file = [](const std::filesystem::path& path, const std::string& rest) {
            return R"({"projectFile":")" + path.generic_string() + '"' + rest + "}";
        };
        const auto liveRevision = revisionOf(server);

        // Edit and export a second project while another is live.
        const auto before = server.call("query", file(second, R"(,"kind":"project")"));
        const auto revision = firstMatch(before, R"re("revision":(\d+))re");
        expect(contains(before, R"("isError":false)") && !revision.empty(),
               "a second project reads while another is live: " + before);
        const auto edited = server.call(
            "transact", file(second, R"(,"expectedRevision":)" + revision +
                                         R"(,"operations":[{"op":"bloom.layer.add-solid","args":)"
                                         R"({"composition":1,"name":"Other","color":[0,1,0,1]}}])"));
        expect(contains(edited, R"("isError":false)"), "the second project edits: " + edited);
        expect(revisionOf(server) == liveRevision, "the live project is untouched");
        const auto exported = server.call(
            "render", file(second, R"(,"composition":1,"frame":0,"preset":"PngRgba8SrgbV1",)"
                                   R"("destination":")" +
                                       (folder / "out.png").generic_string() + R"(")"));
        expect(contains(exported, R"("isError":false)") &&
                   std::filesystem::exists(folder / "out.png"),
               "the second project exports while another is live: " + exported);
        expect(contains(exported, R"("provenance":"cpu-reference")") &&
                   contains(exported, R"("gpuFrames":0)") &&
                   contains(exported, R"("cpuReferenceFrames":1)") &&
                   contains(exported, "CPU reference path"),
               "a file render with no GPU device says it ran on the CPU reference path: " + exported);

        // The live project's own routes do not see the file-session provider: the live host
        // renders through the export queue, which this window has not bound.
        const auto own = server.call(
            "render", R"({"composition":1,"frame":0,"preset":"PngRgba8SrgbV1","destination":")" +
                          (folder / "own.png").generic_string() + R"("})");
        expect(contains(own, "bloom.mcp.export-unavailable") && !std::filesystem::exists(folder / "own.png"),
               "the live project still renders only through the export queue: " + own);

        // A file Bloom has open is refused at first use.
        server.bindProject(third);
        const auto first = server.call("query", file(third, R"(,"kind":"project")"));
        expect(contains(first, "already open in Bloom"),
               "a file open in Bloom is refused at first use: " + first);

        // A file that becomes live after its session opened is refused from then on.
        server.bindProject(second);
        const auto later = server.call("query", file(second, R"(,"kind":"project")"));
        expect(contains(later, "already open in Bloom"),
               "a file opened in Bloom after its session opened is refused: " + later);
        const auto laterSave = server.call("project.save", file(second, ""));
        expect(contains(laterSave, "already open in Bloom") && !contains(laterSave, R"("saved":true)"),
               "project.save on it is refused too: " + laterSave);
        const auto laterEdit = server.call(
            "transact", file(second, R"(,"expectedRevision":0,"operations":[{"op":)"
                                     R"("bloom.layer.add-solid","args":{"composition":1,)"
                                     R"("name":"Again","color":[0,1,0,1]}}])"));
        expect(contains(laterEdit, "already open in Bloom"),
               "and so is a later edit: " + laterEdit);

        // Closing the session writes nothing, so it still works.
        server.bindProject(live);
        const auto unsaved = server.call("project.close", file(second, ""));
        expect(contains(unsaved, "unsaved changes"), "the held session is still there: " + unsaved);

        // project.open and project.save-as refuse a file a session holds.
        const auto open = server.call("project.open", R"({"path":")" + second.generic_string() + R"("})");
        expect(contains(open, "held by a projectFile session"),
               "project.open of a held file is refused: " + open);
        const auto saveAs = server.call("project.save-as", R"({"path":")" + second.generic_string() +
                                                               R"(","replace":true})");
        expect(contains(saveAs, "held by a projectFile session"),
               "project.save-as onto a held file is refused: " + saveAs);
        const auto saveCopy = server.call("project.save-copy", R"({"path":")" + second.generic_string() +
                                                                  R"(","replace":true})");
        expect(contains(saveCopy, "held by a projectFile session"),
               "project.save-copy onto a held file is refused: " + saveCopy);
        const auto free = server.call("project.open", R"({"path":")" + third.generic_string() + R"("})");
        expect(!contains(free, "held by a projectFile session"),
               "a file no session holds is not refused for that reason: " + free);

        // Bloom busy: no lease within the deadline, so the file is not opened or used.
        server.host.revoke(scripting::LiveSessionAvailability::Replacing);
        const auto busyHeld = server.call("query", file(second, R"(,"kind":"project")"));
        expect(contains(busyHeld, "Bloom is busy") && contains(busyHeld, "not open there"),
               "an existing session fails closed when Bloom cannot be asked: " + busyHeld);
        const auto busyNew = server.call("query", file(third, R"(,"kind":"project")"));
        expect(contains(busyNew, "Bloom is busy") && contains(busyNew, "not open there"),
               "a new session fails closed when Bloom cannot be asked: " + busyNew);
        server.bindProject(live);
        const auto closed = server.call("project.close", file(second, R"(,"discardChanges":true)"));
        expect(contains(closed, R"("closed":true)"), "the session closes: " + closed);
        const auto reopened = server.call("project.open", R"({"path":")" + second.generic_string() + R"("})");
        expect(!contains(reopened, "held by a projectFile session"),
               "after close the file is no longer held: " + reopened);
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

int main() {
    try {
        exportWithoutQueue();
        profilerSnapshotTool();
        unboundSeams();
        liveSeams();
        exportReveal();
        retiredTransientJobs();
        animationReadBack();
        mcpFold();
        secondProjectFile();
        fileSessionsBesideALiveProject();
        mergeEntryReadBack();
        fieldTestArguments();
        gradientReadBack();
        curvesAutoAnchors();
        lockedLayerRule();
        parityTools();
        inspectionQueries();
        stdioStopsWhileInputIdle();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "core tools passed\n";
    return failures == 0 ? 0 : 1;
}
