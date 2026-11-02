// The shared MCP core's tool behaviour driven directly through McpServerCore::handle(), without a
// transport: the export queue's absence, and (live host) the session and UI seams of
// live_ui_port.hpp. One stdio transport check: it stops while its input is idle.

#include <bloom/host/export_queue.hpp>
#include <bloom/host/testing/fake_export_runner.hpp>
#include <bloom/document/project.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/profiler_sampler.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/session.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
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
                      std::function<bool(runtime::ProfilerSnapshot&)> profiler = {}) {
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
            .gpuProvider = nullptr,
            .captureDirectory = {},
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
                  std::filesystem::path captures = {}) {
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
             {"composition.delete-preset", R"({"name":"Card"})"}}) {
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
              "composition.save-preset", "composition.rename-preset",
              "composition.delete-preset"})
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
        result = server.call("transact",
                             R"({"expectedRevision":)" + std::to_string(before.value()) +
                                 R"(,"operations":[{"op":"bloom.layer.add-solid","args":{)"
                                 R"("composition":1,"name":"Card","color":[1,1,1,1]}}]})");
        const auto layers = server.call("query", R"({"kind":"layers","composition":1})");
        const auto layer = firstMatch(layers, R"re("records":\[\{"id":(\d+))re");
        const auto revision = server.session->snapshot().revision().value();
        result = server.call(
            "transact", R"({"expectedRevision":)" + std::to_string(revision) +
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
        return std::vector<mcp::McpPreference>{{.key = "loopPlayback", .value = true},
                                               {.key = "soundVolume", .value = 40.0},
                                               {.key = "timelineTimeFormat",
                                                .value = std::string("frames")}};
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
        return std::vector<mcp::McpJob>{{.id = 7,
                                         .name = "Import media",
                                         .phase = "Importing media",
                                         .state = "Running",
                                         .completed = 2,
                                         .total = 5,
                                         .canCancel = true,
                                         .cancellationRequested = false,
                                         .exportJob = std::nullopt}};
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

    auto result = server.call(
        "viewer.set", R"({"gamma":2.2,"resolution":"half","background":"black",)"
                      R"("transparencyGrid":true,"look":false,)"
                      R"("regionOfInterest":{"x":10,"y":20,"width":30,"height":40}})");
    expect(viewer.gamma == 2.2 && viewer.resolution == mcp::McpViewerResolution::Half &&
               viewer.background == mcp::McpViewerBackground::Black &&
               viewer.transparencyGrid == true && viewer.look == false &&
               viewer.regionOfInterest && *viewer.regionOfInterest &&
               (*viewer.regionOfInterest)->width == 30.0 && !viewer.exposure,
           "viewer.set passes the new fields: " + result);
    result = server.call("viewer.set", R"({"regionOfInterest":null})");
    expect(viewer.regionOfInterest && !*viewer.regionOfInterest,
           "a null region clears it: " + result);
    result = server.call("viewer.set", R"({"regionOfInterest":{"x":0,"y":0,"width":0,"height":4}})");
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
    result = server.call("preferences.set",
                         R"({"preferences":{"loopPlayback":false,"soundVolume":10,)"
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
    expect(presetCalls == std::vector<std::string>{"save 1 Card", "rename Card Plate",
                                                   "delete Plate"} &&
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
        .deleteCompositionPreset = {}};
    Live server(std::move(port));

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
                   contains(entry, R"("job":{"type":"integer","minimum":1})"),
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
                expect(contains(result, "bloom.mcp.not-live") && contains(result, R"("isError":true)"),
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
    expect(server.session->snapshot().revision() == before, "export.reveal never edits the project");
}

// The id after the first `"job":` of a response.
std::uint64_t jobOf(const std::string& response) {
    const auto at = response.find(R"("job":)");
    return at == std::string::npos ? 0 : std::stoull(response.substr(at + 6));
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
    expect(contains(status, R"("recentTransientJobs":[{"id":)" + id) &&
               contains(status, R"("transient":true)"),
           "export.status lists the retired transient job: " + status);
    // The refusal codes are named by enumerator, not by position: ADR 0035 retired value 23, and
    // the codes after it must keep their own names.
    expect(contains(status, R"("code":"DestinationCheckPending")") &&
               !contains(status, R"("code":"Unknown")"),
           "export.status names the codes after the retired gap correctly: " + status);
    const auto wait = server.call("export.wait", R"({"job":)" + id + R"(,"timeoutMs":1000})");
    expect(contains(wait, R"("finished":true)"), "export.wait finds the retired job: " + wait);

    fake->push({.writeDestinations = true});
    const auto capture = server.call("capture", R"({"composition":1,"frame":0})");
    const auto captureJob = jobOf(capture);
    expect(contains(capture, R"("isError":false)") && captureJob != 0 && captureJob != job &&
               contains(capture, R"("log":[{)"),
           "a capture carries its transient job and log: " + capture);
    expect(contains(server.call("export.log", R"({"job":)" + std::to_string(captureJob) + "}"),
                    R"("entries":[{)"),
           "export.log reads the capture's retired job");

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
    auto keyframes = server.call("query", R"({"kind":"keyframes","composition":1})");
    const auto curve = firstMatch(keyframes, R"re("curve":(\d+))re");
    const auto key = firstMatch(keyframes, R"re("keys":\[\{"id":(\d+))re");
    expect(!curve.empty() && !key.empty() && contains(keyframes, R"("interpolation":"linear")"),
           "keyframes read back with their interpolation token: " + keyframes);
    result = transact(R"({"op":"bloom.animation.set-keyframes-easing","args":{"composition":1,)"
                      R"("keys":[[)" +
                      curve + "," + key +
                      R"(]],"interpolation":"elastic","ease":"out","amplitude":2.0}},)"
                      R"({"op":"bloom.layer.set-shy","args":{"composition":1,"layer":)" +
                      layer + R"(,"shy":true}})");
    keyframes = server.call("query", R"({"kind":"keyframes","composition":1})");
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
    keyframes = server.call("query", R"({"kind":"keyframes","composition":1})");
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

int main() {
    try {
        exportWithoutQueue();
        profilerSnapshotTool();
        unboundSeams();
        liveSeams();
        exportReveal();
        retiredTransientJobs();
        animationReadBack();
        parityTools();
        stdioStopsWhileInputIdle();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "core tools passed\n";
    return failures == 0 ? 0 : 1;
}
