// bloom.ui.mcp-script-run (#603): script.run end to end. An MCP client calls script.run over
// Streamable HTTP against the real in-app server, which hands the program to the Script editor's
// embedded Python in the running session. With the Preferences switch off the call is refused and
// nothing runs; with it on the script runs, its edits arrive through the normal transactions (one
// undo step with the script's label, reverted by the window's Undo), a failing script reports its
// error and changes nothing, and `events` names the edit's origin. Real window and session,
// offscreen.
#include "mcp_http_client.hpp"
#include "window_fixture.hpp"

#include <QAction>
#include <QSettings>
#include <QTimer>
#include <bloom/mcp/server_service.hpp>
#include <bloom/mcp/testing/server_service_driver.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/live_session_adapter.hpp>
#include <bloom/ui/mcp_live_ui_binding.hpp>
#include <bloom/ui/playback_controller.hpp>
#include "script_panel_runtime.hpp"

#include <future>
#include <iostream>
#include <string>

namespace {

using namespace bloom;
using ui::test::mcp_http::call;
using ui::test::mcp_http::post;

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Predicate> bool waitFor(Predicate predicate, const int milliseconds = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds)
        QTest::qWait(5);
    return predicate();
}

[[nodiscard]] bool has(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

void setSwitch(const bool on) {
    QSettings settings;
    auto preferences = ui::loadApplicationPreferences(settings);
    preferences.agentsRunScripts = on;
    ui::saveApplicationPreferences(settings, preferences);
}

void exercise() {
    [[maybe_unused]] const ui::test::mcp_http::Startup startup;
    ui::test::WindowFixture fixture([](auto& fixture) {
        require(fixture.session.addSolidLayer("Background", {0.1, 0.5, 0.8, 1}), "Solid layer");
        QObject::connect(
            &fixture.projectHost, &ui::ProjectHost::sessionReplaced, &fixture.session, [&fixture] {
                const auto [document, stack] = fixture.projectHost.liveDocumentAndStack();
                if (document && stack)
                    fixture.session.rebind(*document, *stack,
                                           fixture.projectHost.lowestCompositionId());
            });
    });
    scripting::LiveSessionHost live;
    ui::LiveSessionAdapter adapter(live, fixture.session, fixture.projectHost);
    // The Script editor's runtime: the same object the application shares between its Script
    // editors and the agent route.
    auto runtime = std::make_shared<ui::ScriptPanelRuntime>(live, nullptr);
    ui::McpLiveUiBinding binding(fixture.projectHost, fixture.session,
                                 fixture.preview->playbackController());
    binding.setWindow(fixture.window.get());
    binding.setScriptRuntime(runtime);
    runtime::TaskScheduler scheduler;
    QTemporaryDir folder;
    require(folder.isValid(), "Scratch folder");
    const auto port = mcp::testing::McpServerServiceDriver::freePort(46100);
    mcp::McpServerService service({.live = live,
                                   .exportQueue = {},
                                   .scheduler = scheduler,
                                   .fileSessionGpuProvider = {},
                                   .liveUi = binding.port(),
                                   .profilerSnapshot = {},
                                   .tokenFile = folder.filePath("mcp/token").toStdString(),
                                   .captureDirectory = folder.filePath("captures").toStdString(),
                                   .serverVersion = "test",
                                   .preferences = {.port = port, .autoStart = false},
                                   .limits = {}});
    QTimer pump;
    QObject::connect(&pump, &QTimer::timeout, [&service] { service.pump(); });
    pump.start(5);
    require(waitFor([&] { return service.snapshot()->token.state == mcp::McpTokenState::Ready; }),
            "Token ready");
    require(service.start().accepted, "Server started");
    const auto token = service.revealToken().token;

    const auto async = [&](auto work) {
        auto future = std::async(std::launch::async, std::move(work));
        require(waitFor([&] {
                    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                }),
                "HTTP exchange finished");
        return future.get();
    };
    const auto init = async([&] {
        return post(
            port, token, {},
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"script-run-test","version":"1"}}})");
    });
    require(init.status == 200 && !init.sessionId.empty(), "Initialized over HTTP");
    const auto session = init.sessionId;
    require(async([&] {
                return post(port, token, session,
                            R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
            }).status == 202,
            "Initialized notification");
    int nextId = 2;
    const auto tool = [&](const std::string& name, const std::string& arguments) {
        const auto id = nextId++;
        return async([&, id] { return post(port, token, session, call(id, name, arguments)); }).body;
    };

    // The artist edits first, so the events later show all three origins.
    require(fixture.session.addSolidLayer("Artist layer", {0.9, 0.1, 0.1, 1}), "Artist edit");

    // 1. Off by default: refused, and nothing runs.
    require(!ui::loadApplicationPreferences(QSettings{}).agentsRunScripts,
            "The switch is off by default");
    const auto before = fixture.session.snapshot().revision().value();
    const auto refused = tool(
        "script.run",
        R"j({"source":"with bloom.transactions.group('Refused script'):\n    bloom.ops.project.set_name(name='Refused')\nprint('RAN-WHILE-OFF')"})j");
    require(has(refused, "bloom.mcp.not-allowed"), "script.run is refused with the switch off");
    require(!has(refused, "RAN-WHILE-OFF"), "A refused script does not run");
    require(fixture.session.snapshot().revision().value() == before &&
                fixture.session.snapshot().project().name() != "Refused",
            "A refused script changes nothing");

    // 2. On: the script runs in the live session and its edit is one normal undo step.
    setSwitch(true);
    const auto ran = tool(
        "script.run",
        R"j({"source":"with bloom.transactions.group('Script rename'):\n    bloom.ops.project.set_name(name='From script')\nprint('SCRIPT-RAN', int(bloom.context.composition))"})j");
    require(has(ran, "\"isError\":false") && has(ran, "SCRIPT-RAN"),
            "script.run runs a multi-line program and answers its output");
    require(fixture.session.snapshot().project().name() == "From script" &&
                fixture.session.snapshot().revision().value() == before + 1,
            "The script's edit is one revision in the live project");
    auto* undo = fixture.window->findChild<QAction*>("undoAction");
    require(undo && undo->isEnabled() && undo->text().contains("Script rename"),
            "The window offers the script's edit as one undo step with its label");
    const auto history = tool("query", R"({"kind":"history"})");
    require(has(history, "Script rename") && has(history, "canUndo"),
            "query history lists the script's step");
    undo->trigger();
    require(fixture.session.snapshot().project().name() != "From script",
            "The window's Undo reverts the script's edit");

    // 3. A script that fails reports the error and changes nothing.
    const auto afterUndo = fixture.session.snapshot().revision().value();
    const auto failed = tool(
        "script.run",
        R"j({"source":"with bloom.transactions.group('Doomed'):\n    bloom.ops.project.set_name(name='Doomed')\n    raise ValueError('boom')"})j");
    require(has(failed, "bloom.mcp.failed") && has(failed, "boom"),
            "A failing script answers bloom.mcp.failed with the error text");
    require(fixture.session.snapshot().revision().value() == afterUndo &&
                fixture.session.snapshot().project().name() != "Doomed",
            "A failing script group leaves the project as it was");

    // 4. Switching it off again closes the route.
    setSwitch(false);
    const auto closed = tool("script.run", R"j({"source":"print('RAN-AFTER-OFF')"})j");
    require(has(closed, "bloom.mcp.not-allowed") && !has(closed, "RAN-AFTER-OFF"),
            "Switching the route off refuses the next call");

    // 5. Who made each edit: the artist, the script and an agent's own transaction.
    const auto revision = fixture.session.snapshot().revision().value();
    const auto transact = tool(
        "transact",
        R"({"expectedRevision":)" + std::to_string(revision) +
            R"(,"label":"Agent rename","operations":[{"op":"bloom.project.set-name","args":{"name":"From agent"}}]})");
    require(has(transact, "\"isError\":false"), "An agent transaction succeeds");
    const auto events = tool("events", R"({"sinceRevision":0})");
    require(has(events, "\\\"origin\\\":\\\"user\\\"") && has(events, "\\\"origin\\\":\\\"script\\\"") &&
                has(events, "\\\"origin\\\":\\\"agent\\\""),
            "events name the artist, the script and the agent as the origin of each edit");

    service.beginShutdown();
    require(waitFor([&] { return service.quiescent(); }), "Server shut down");
    pump.stop();
    runtime->shutdown();
    adapter.shutdown();
    scheduler.beginShutdown();
    std::cout << "MCP script.run: refused when off, runs in the live session when on, undoable, "
                 "failing script harmless, origins reported\n";
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomTests");
    app.setApplicationName("McpScriptRun");
    ui::kit::installKinetikTheme(app);
    try {
        exercise();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
