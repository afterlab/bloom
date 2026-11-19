// bloom.ui.mcp-save-failure (issue #737): a Save or Save As that does not write the project file
// names the stage it stopped at and its cause -- in the agent's bloom.mcp.save-failed diagnostic,
// in the status bar, and in the artist's dialog -- and an agent's failed save never opens or leaves
// a modal dialog.
//
// The MCP binding is constructed before the window, as apps/bloom/main.cpp does, so its
// saveFinished() slot runs before the window's and ends the agent operation first. That order is
// what let the window open "The project could not be saved." over a failed agent save.
// Offscreen.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "await_qt.hpp"
#include "window_fixture.hpp"

#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/mcp_live_ui_binding.hpp>
#include <bloom/ui/playback_controller.hpp>

#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace {

constexpr std::chrono::milliseconds kWait{20000};

using namespace bloom;
namespace fs = std::filesystem;
using mcp::McpUiRefusal;

int gFailures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++gFailures;
    }
}

mcp::McpProjectResult runProject(ui::McpLiveUiBinding& binding, mcp::McpProjectRequest request) {
    std::optional<mcp::McpProjectResult> result;
    binding.projectLifecycle(std::move(request),
                             [&](mcp::McpProjectResult value) { result = std::move(value); });
    expect(bloom::test_support::awaitQtCondition(
               "the project lifecycle call answers", [&] { return result.has_value(); }, kWait),
           "the project lifecycle call answers");
    // Let every queued slot of the finished signal run: a dialog would open here.
    QTest::qWait(60);
    return result.value_or(
        mcp::McpProjectResult{.refusal = McpUiRefusal::Failed, .message = "no answer"});
}

mcp::McpProjectRequest saveAs(fs::path path) {
    mcp::McpProjectRequest value;
    value.kind = mcp::McpProjectRequestKind::SaveAs;
    value.path = std::move(path);
    value.replace = true;
    return value;
}

QString dialogText(QWidget& dialog) {
    QString text;
    for (const auto* label : dialog.findChildren<QLabel*>())
        text += label->text() + QLatin1Char('\n');
    return text;
}

void testFailedSaves() {
    QTemporaryDir projects;
    expect(projects.isValid(), "a temporary folder");
    const fs::path folder = projects.path().toStdString();
    // A folder named like a project file: preflight refuses it as not a regular file. A real
    // failure of the real pipeline, no fault injection.
    const auto notAFile = folder / "folder.bloom";
    fs::create_directory(notAFile);

    std::unique_ptr<ui::McpLiveUiBinding> binding;
    ui::test::WindowFixture fixture(
        {}, std::size_t{2} * 1024 * 1024 * 1024, [&binding](ui::test::WindowFixture& f) {
            binding = std::make_unique<ui::McpLiveUiBinding>(f.projectHost, f.session,
                                                             f.preview->playbackController());
        });
    binding->setWindow(fixture.window.get());
    auto& host = fixture.projectHost;
    QSignalSpy finished(&host, &ui::ProjectHost::saveFinished);

    // ---- an agent's pathless Save never opens the Save As dialog
    {
        expect(!host.displayPath().has_value(), "the fixture project has never been saved");
        host.setAgentOperation(true);
        host.beginSave();
        host.setAgentOperation(false);
        QTest::qWait(60);
        expect(QApplication::activeModalWidget() == nullptr,
               "an agent's pathless Save opens no Save As dialog");
        const auto& failure = host.lastSaveFailure();
        expect(failure.has_value() && failure->stage == "start" &&
                   failure->cause == "path-required",
               "and is refused as path-required");
        expect(!finished.isEmpty() &&
                   finished.last().at(2).value<ui::ProjectHostOperationOrigin>() ==
                       ui::ProjectHostOperationOrigin::Agent,
               "saveFinished names the agent as the origin");
    }

    // ---- an agent's failed Save As: a typed diagnostic, no dialog, nothing left busy
    {
        const auto result = runProject(*binding, saveAs(notAFile));
        expect(result.refusal == McpUiRefusal::SaveFailed,
               "a save that did not write the file answers bloom.mcp.save-failed, got " +
                   std::string(mcp::uiRefusalCode(result.refusal)) + ": " + result.message);
        expect(result.saveFailure.has_value() &&
                   result.saveFailure->stage == "publication.preflight" &&
                   result.saveFailure->cause == "target-leaf-not-regular",
               "its detail names the stage and the cause");
        expect(result.message == "Save failed while checking the destination file: the "
                                 "destination is not a regular file.",
               "its message names the stage and the cause: " + result.message);
        expect(QApplication::activeModalWidget() == nullptr, "a failed agent save opens no dialog");
        expect(!host.agentOperation() && !host.isBusy(), "and leaves the host free");
        const auto statusShown =
            !finished.isEmpty() && finished.last().at(1).toString().toStdString() == result.message;
        expect(statusShown, "the status bar gets the same message");
    }

    // ---- the next agent call is not Busy and saves
    {
        const auto saved = folder / "saved.bloom";
        const auto result = runProject(*binding, saveAs(saved));
        expect(result.ok() && fs::is_regular_file(saved),
               "the next agent Save As writes the file: " + result.message);
        expect(!host.lastSaveFailure().has_value(), "a successful save clears the failure");
    }

    // ---- the artist's failed Save As opens the dialog, and it names the stage and the cause
    {
        QString shown;
        QTimer poll;
        poll.setInterval(10);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            if (auto* dialog = QApplication::activeModalWidget()) {
                shown = dialogText(*dialog);
                dialog->close();
            }
        });
        poll.start();
        host.beginSaveAs(notAFile);
        expect(bloom::test_support::awaitQtCondition(
                   "the artist's failure dialog opens", [&] { return !shown.isEmpty(); }, kWait),
               "the artist's failed save opens its dialog");
        poll.stop();
        expect(shown.contains(QStringLiteral("The project could not be saved.")) &&
                   shown.contains(QStringLiteral("Save failed while checking the destination "
                                                 "file: the destination is not a regular file.")),
               "the dialog names the stage and the cause: " + shown.toStdString());
        expect(bloom::test_support::awaitQtCondition(
                   "the dialog closes", [] { return QApplication::activeModalWidget() == nullptr; },
                   kWait),
               "the dialog closes");
    }

    binding.reset();
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomMcpSaveFailureTests"));
    QApplication::setApplicationName(QStringLiteral("McpSaveFailure"));
    ui::kit::installAfterlabTheme(application);
    try {
        testFailedSaves();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: exception: %s\n", error.what());
        ++gFailures;
    }
    if (gFailures != 0)
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return gFailures + bloom::test_support::awaitFailureCount() == 0 ? 0 : 1;
}
