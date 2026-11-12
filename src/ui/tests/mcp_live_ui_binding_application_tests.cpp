// bloom.ui.mcp-live-ui-application (#389, "every artist action as an agent tool"): the live-UI
// port members that act on the application rather than the project -- the Viewer's gamma,
// resolution, background, transparency grid, look and region of interest; Preferences; Edit >
// Purge; the Jobs panel; Cancel Open; Reveal in File Manager; the composition presets; and Save a
// Copy's refusal for an editable project -- through the real McpLiveUiBinding over the real window
// fixture. None of them may dirty the project or enter its undo history. Offscreen.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "window_fixture.hpp"

#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/composition_presets.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/mcp_live_ui_binding.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTest>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace bloom;
using mcp::McpUiRefusal;

int gFailures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        ++gFailures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

std::optional<mcp::McpPreferenceValue> preference(const std::vector<mcp::McpPreference>& values,
                                                  const std::string& key) {
    const auto found = std::ranges::find(values, key, &mcp::McpPreference::key);
    return found == values.end() ? std::nullopt : std::optional(found->value);
}

void viewerSettings(ui::test::WindowFixture& fixture, const mcp::McpLiveUiPort& port) {
    auto* viewer = fixture.window->displayedViewer();
    expect(viewer != nullptr, "a Viewer is displayed");
    if (viewer == nullptr)
        return;
    auto* gamma = fixture.window->findChild<ui::kit::KValueField*>("viewerGamma");
    auto* resolution = fixture.window->findChild<ui::kit::KDropdown*>("viewerResolutionDropdown");
    auto* background = fixture.window->findChild<ui::kit::KDropdown*>("viewerBackgroundDropdown");
    auto* roi = fixture.window->findChild<QAction*>("viewerRoiAction");
    expect(gamma && resolution && background && roi, "the footer controls exist");
    if (!gamma || !resolution || !background || !roi)
        return;
    auto result = port.setViewer({.gamma = 2.2});
    expect(result.ok() && gamma->value() == 2.2, "gamma moves the footer field");
    result = port.setViewer({.gamma = 40.0});
    expect(result.refusal == McpUiRefusal::InvalidArgument, "gamma out of range is refused");
    (void)port.setViewer({.gamma = 1.0});
    result = port.setViewer({.resolution = mcp::McpViewerResolution::Quarter});
    expect(result.ok() && resolution->currentIndex() == 3, "resolution picks Quarter");
    (void)port.setViewer({.resolution = mcp::McpViewerResolution::Auto});
    result = port.setViewer({.background = mcp::McpViewerBackground::White,
                             .transparencyGrid = true});
    expect(result.ok() && background->currentIndex() == 3 && viewer->transparencyGridForTest(),
           "background and transparency grid follow");
    (void)port.setViewer({.background = mcp::McpViewerBackground::Solid,
                          .transparencyGrid = false});
    expect(!viewer->transparencyGridForTest(), "the transparency grid turns off");

    result = port.setViewer(
        {.regionOfInterest = std::optional(mcp::McpViewerRegion{10.0, 20.0, 100.0, 50.0})});
    expect(result.ok() && roi->isChecked(), "a region of interest is set and enabled");
    result = port.setViewer(
        {.regionOfInterest = std::optional(mcp::McpViewerRegion{1e6, 1e6, 10.0, 10.0})});
    expect(result.refusal == McpUiRefusal::InvalidArgument && roi->isChecked(),
           "a region outside the frame is refused and changes nothing");
    result = port.setViewer({.regionOfInterest = std::optional<mcp::McpViewerRegion>{}});
    expect(result.ok() && !roi->isChecked(), "an empty region clears it");

    // The fixture's composition has no look-tagged effect: the look cannot change.
    auto* look = fixture.window->findChild<QAction*>("viewerLookAction");
    if (look != nullptr && !look->isEnabled()) {
        result = port.setViewer({.look = !look->isChecked()});
        expect(result.refusal == McpUiRefusal::InvalidArgument,
               "the look cannot toggle without a look-tagged effect");
        result = port.setViewer({.look = look->isChecked()});
        expect(result.ok(), "asking for the look it already has is accepted");
    }
}

void preferences(const mcp::McpLiveUiPort& port) {
    auto values = port.preferences();
    expect(preference(values, "loopPlayback").has_value() &&
               preference(values, "timelineTimeFormat").has_value() &&
               preference(values, "importSequenceFrameRate").has_value() &&
               !preference(values, "timelineLayerColumnWidth").has_value(),
           "preferences.get lists the Preferences window's settings, not layout");
    expect(preference(values, "crashRecovery") == mcp::McpPreferenceValue(false),
           "preferences.get lists crashRecovery, off by default");
    auto result = port.setPreferences({{.key = "crashRecovery", .value = true},
                                       {.key = "loopPlayback", .value = false},
                                       {.key = "timelineTimeFormat",
                                        .value = std::string("timecode")},
                                       {.key = "importSequenceFrameRate",
                                        .value = std::string("25")},
                                       {.key = "soundVolume", .value = 15.0}});
    expect(result.ok(), "preferences.set applies valid entries");
    const auto stored = ui::loadApplicationPreferences(QSettings{});
    expect(!stored.loopPlayback && stored.crashRecoveryEnabled &&
               stored.timelineTimeFormat == ui::TimelineTimeFormat::Timecode &&
               stored.importSequenceFrameRate &&
               stored.importSequenceFrameRate->numerator() == 25 &&
               stored.soundVolumePercent == 15,
           "the preferences are saved as the Preferences window saves them");
    values = port.preferences();
    expect(preference(values, "loopPlayback") == mcp::McpPreferenceValue(false) &&
               preference(values, "importSequenceFrameRate") ==
                   mcp::McpPreferenceValue(std::string("25/1")),
           "preferences.get reads them back");

    // Issue #581: the export worker limits are preferences too.
    expect(preference(values, "exportWorkerMemoryCapBytes") == mcp::McpPreferenceValue(0.0) &&
               preference(values, "exportWorkerThreads") == mcp::McpPreferenceValue(0.0),
           "preferences.get lists the export worker cap and threads, off by default");
    result = port.setPreferences({{.key = "exportWorkerMemoryCapBytes", .value = 2147483648.0},
                                  {.key = "exportWorkerThreads", .value = 4.0}});
    const auto workers = ui::loadApplicationPreferences(QSettings{});
    expect(result.ok() && workers.exportWorkerMemoryCapBytes == 2147483648ULL &&
               workers.exportWorkerThreads == 4,
           "preferences.set saves the export worker cap and threads");
    result = port.setPreferences({{.key = "exportWorkerThreads", .value = 65.0}});
    expect(result.refusal == McpUiRefusal::InvalidArgument &&
               ui::loadApplicationPreferences(QSettings{}).exportWorkerThreads == 4,
           "more than 64 worker threads is refused and changes nothing");
    result = port.setPreferences({{.key = "exportWorkerMemoryCapBytes", .value = 1.5}});
    expect(result.refusal == McpUiRefusal::InvalidArgument,
           "a fractional worker memory cap is refused");
    result = port.setPreferences({{.key = "exportWorkerMemoryCapBytes", .value = 0.0},
                                  {.key = "exportWorkerThreads", .value = 0.0}});
    expect(result.ok() && ui::loadApplicationPreferences(QSettings{}).exportWorkerMemoryCapBytes == 0,
           "zero turns the worker limits back off");

    result = port.setPreferences({{.key = "loopPlayback", .value = true},
                                  {.key = "noSuchPreference", .value = true}});
    expect(result.refusal == McpUiRefusal::InvalidArgument &&
               result.message.find("noSuchPreference") != std::string::npos &&
               !ui::loadApplicationPreferences(QSettings{}).loopPlayback,
           "an unknown key refuses the whole call and changes nothing");
    result = port.setPreferences({{.key = "soundVolume", .value = 150.0}});
    expect(result.refusal == McpUiRefusal::InvalidArgument, "a value out of range is refused");
    result = port.setPreferences({{.key = "timelineTimeFormat", .value = true}});
    expect(result.refusal == McpUiRefusal::InvalidArgument &&
               result.message.find("frames, timecode") != std::string::npos,
           "a wrong kind names the choices");
    result = port.setPreferences({{.key = "mediaDiskCacheDirectory",
                                   .value = std::string("relative/cache")}});
    expect(result.refusal == McpUiRefusal::InvalidArgument, "a relative cache folder is refused");
}

// #579 and #603: the two opt-in agent routes are off by default, preferences.set cannot turn them
// on, and only the saved Preferences switches (written here as the Preferences window writes them)
// open them. A real modal dialog is answered by its button.
void agentRoutes(const mcp::McpLiveUiPort& port) {
    const auto save = [](const ui::ApplicationPreferences& preferences) {
        QSettings settings;
        ui::saveApplicationPreferences(settings, preferences);
    };
    auto values = port.preferences();
    expect(preference(values, "agentsAnswerDialogs") == mcp::McpPreferenceValue(false) &&
               preference(values, "agentsRunScripts") == mcp::McpPreferenceValue(false),
           "both agent routes are listed and off by default");
    auto result = port.setPreferences({{.key = "agentsAnswerDialogs", .value = true}});
    expect(result.refusal == McpUiRefusal::InvalidArgument &&
               result.message.find("only be changed by the artist") != std::string::npos &&
               !ui::loadApplicationPreferences(QSettings{}).agentsAnswerDialogs,
           "preferences.set cannot switch the dialog route on");
    result = port.setPreferences({{.key = "agentsRunScripts", .value = true}});
    expect(result.refusal == McpUiRefusal::InvalidArgument &&
               !ui::loadApplicationPreferences(QSettings{}).agentsRunScripts,
           "preferences.set cannot switch the script route on");

    QMessageBox box;
    box.setWindowTitle(QStringLiteral("Unsaved changes"));
    box.setText(QStringLiteral("Save the changes before closing?"));
    auto* discard = box.addButton(QStringLiteral("&Discard"), QMessageBox::DestructiveRole);
    auto* cancel = box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    bool discarded = false;
    bool cancelled = false;
    QObject::connect(discard, &QAbstractButton::clicked, [&] { discarded = true; });
    QObject::connect(cancel, &QAbstractButton::clicked, [&] { cancelled = true; });
    box.setWindowModality(Qt::ApplicationModal);
    box.show();
    QTest::qWait(50);

    // Off: the dialog is untouched and the refusal says who can allow it.
    result = port.answerDialog("Cancel");
    QTest::qWait(30);
    expect(result.refusal == McpUiRefusal::NotAllowed && !cancelled && box.isVisible(),
           "dialog.answer with the switch off presses nothing");
    expect(port.setPreferences({{.key = "loopPlayback", .value = true}}).refusal ==
               McpUiRefusal::Busy,
           "the busy refusal still describes the dialog");

    auto preferences = ui::loadApplicationPreferences(QSettings{});
    preferences.agentsAnswerDialogs = true;
    save(preferences);
    result = port.answerDialog("No such button");
    expect(result.refusal == McpUiRefusal::InvalidArgument &&
               result.message.find("Discard (discards or overwrites data)") != std::string::npos,
           "an unknown button lists the buttons and marks the discarding one");
    result = port.answerDialog("cancel");
    QTest::qWait(50);
    expect(result.ok() && cancelled && !discarded && !box.isVisible(),
           "dialog.answer presses the named button, case-insensitively, and the dialog closes");
    result = port.answerDialog("Cancel");
    expect(result.refusal == McpUiRefusal::InvalidArgument,
           "with no dialog open there is nothing to answer");

    // A discarding button is pressed on request and the answer says what it did.
    QMessageBox again;
    again.setText(QStringLiteral("Discard?"));
    auto* second = again.addButton(QStringLiteral("&Discard"), QMessageBox::DestructiveRole);
    bool pressed = false;
    QObject::connect(second, &QAbstractButton::clicked, [&] { pressed = true; });
    again.setWindowModality(Qt::ApplicationModal);
    again.show();
    QTest::qWait(50);
    result = port.answerDialog("Discard");
    QTest::qWait(50);
    expect(result.ok() && pressed && result.message.find("discards or overwrites") != std::string::npos,
           "a discarding button is pressed only on request and named as discarding");

    // Scripts: off by default, then the switch alone admits the request.
    preferences.agentsAnswerDialogs = false;
    save(preferences);
    std::optional<mcp::McpScriptResult> script;
    port.runScript({.source = "print(1)"}, [&](mcp::McpScriptResult answer) { script = answer; });
    expect(script && script->refusal == McpUiRefusal::NotAllowed,
           "script.run with the switch off is refused, not run");
    script.reset();
    preferences.agentsRunScripts = true;
    save(preferences);
    port.runScript({.source = "print(1)"}, [&](mcp::McpScriptResult answer) { script = answer; });
    expect(script && script->refusal == McpUiRefusal::Unbound,
           "with the switch on but no Script runtime bound, script.run answers Unbound");
    preferences.agentsRunScripts = false;
    save(preferences);
}

void compositionPresets(ui::test::WindowFixture& fixture, const mcp::McpLiveUiPort& port) {
    const auto composition = fixture.session.compositionId();
    auto presets = port.compositionPresets();
    expect(!presets.empty() && presets.front().builtIn, "built-in presets are listed first");
    auto result = port.saveCompositionPreset(composition, "Agent Card");
    expect(result.ok(), "a composition is saved as a preset");
    presets = port.compositionPresets();
    const auto saved = std::ranges::find(presets, std::string("Agent Card"),
                                         &mcp::McpCompositionPreset::name);
    expect(saved != presets.end() && !saved->builtIn && saved->frameRate && saved->duration,
           "the saved preset carries the portable settings");
    result = port.saveCompositionPreset(composition, "Agent Card");
    expect(result.refusal == McpUiRefusal::InvalidArgument, "a used name is refused");
    result = port.saveCompositionPreset(document::CompositionId::fromRaw(98765), "Other");
    expect(result.refusal == McpUiRefusal::UnknownTarget, "an unknown composition is refused");
    result = port.renameCompositionPreset("Agent Card", "Agent Plate");
    expect(result.ok(), "a saved preset is renamed");
    result = port.renameCompositionPreset(presets.front().name, "Mine");
    expect(result.refusal == McpUiRefusal::UnknownTarget, "a built-in preset cannot be renamed");
    result = port.deleteCompositionPreset("Agent Plate");
    expect(result.ok(), "a saved preset is deleted");
    QSettings settings;
    expect(ui::loadUserCompositionPresets(settings).empty(), "the store holds none afterwards");
    result = port.deleteCompositionPreset("Agent Plate");
    expect(result.refusal == McpUiRefusal::UnknownTarget, "deleting it again is refused");
}

void testApplication() {
    ui::test::WindowFixture fixture;
    auto& session = fixture.session;
    auto& host = fixture.projectHost;
    const auto revision = session.snapshot().revision();
    const bool dirty = host.isDirty();
    ui::McpLiveUiBinding binding(host, session, fixture.preview->playbackController());
    ui::TaskMonitorModel tasks(fixture.bridge);
    {
        const auto unboundPort = binding.port();
        expect(unboundPort.cancelJob(1).refusal == McpUiRefusal::Unbound,
               "jobs.cancel without the Jobs model -> Unbound");
        expect(unboundPort.setPreferences({{.key = "loopPlayback", .value = true}}).refusal ==
                   McpUiRefusal::Unbound,
               "preferences.set without a window -> Unbound");
    }
    binding.setWindow(fixture.window.get());
    binding.setTaskMonitor(&tasks);
    const auto port = binding.port();

    viewerSettings(fixture, port);
    preferences(port);
    agentRoutes(port);
    compositionPresets(fixture, port);

    auto result = port.purgeCache(false);
    expect(result.ok(), "cache.purge preview runs the Edit > Purge body");
    result = port.purgeCache(true);
    expect(result.ok(), "cache.purge media runs the Edit > Purge body");
    const auto rows = port.jobs();
    expect(std::ranges::all_of(rows, [](const mcp::McpJob& job) { return job.id != 0; }),
           "every Jobs row has its task id");
    expect(std::ranges::all_of(rows,
                               [](const mcp::McpJob& job) {
                                   return !job.kind.empty() && !job.stateToken.empty() &&
                                          (job.stateToken == "failed" || job.failure.empty());
                               }),
           "every Jobs row names its owner and state, and only a failed job carries a reason");
    expect(port.cancelJob(987654).refusal == McpUiRefusal::UnknownTarget,
           "jobs.cancel of an unknown job -> UnknownTarget");
    expect(port.cancelOpen().refusal == McpUiRefusal::InvalidArgument,
           "project.cancel-open with nothing opening -> InvalidArgument");
    expect(port.revealAsset(document::AssetId::fromRaw(987654)).refusal ==
               McpUiRefusal::UnknownTarget,
           "asset.reveal of an unknown asset -> UnknownTarget");

    std::optional<mcp::McpProjectResult> copied;
    port.projectLifecycle({.kind = mcp::McpProjectRequestKind::SaveCopy,
                           .path = std::filesystem::path(
                               fixture.settingsDirectory.filePath("copy.bloom").toStdString())},
                          [&](mcp::McpProjectResult answer) { copied = std::move(answer); });
    expect(copied && copied->refusal == McpUiRefusal::InvalidArgument,
           "Save a Copy of an editable project is refused toward Save As");

    expect(host.isDirty() == dirty && session.snapshot().revision() == revision,
           "no application tool dirtied the project or entered its history");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomMcpLiveUiApplicationTests"));
    QApplication::setApplicationName(QStringLiteral("McpLiveUiApplication"));
    ui::kit::installAfterlabTheme(application);
    try {
        testApplication();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: exception: %s\n", error.what());
        ++gFailures;
    }
    if (gFailures != 0)
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
