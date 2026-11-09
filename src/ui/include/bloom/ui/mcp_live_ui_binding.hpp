#pragma once

// The desktop application's side of the MCP live-UI port (ADR 0034, "Owner decisions" item 9):
// MCP is a UI for agents, so every call here does what the artist's own action does, through the
// same controllers -- ProjectHost for open/save/new/close and colour settings, CompositionSession
// for the active composition, selection and playhead, PlaybackController for the transport, the
// WorkspaceHost for revealing editors and the displayed Viewer's own controls for its display
// settings, and the export queue adapter for selecting a job in the Export editor. Nothing here owns
// project truth or invents a second path.
//
// ONE binding, built once in apps/bloom/main.cpp and handed to McpServerServiceDependencies::liveUi
// through port(). Every member runs on the owner (UI) thread. Session-state members (selection,
// active composition, transport, Viewer) never dirty the project. A refusal is a typed
// mcp::McpUiRefusal with a plain sentence; nothing here opens a dialog.
//
// The window is created after the MCP service, so it is attached later with setWindow(); until then
// the members that need it answer a typed refusal.

#include <bloom/mcp/live_ui_port.hpp>

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QTimer;

namespace bloom::ui {

class CompositionSession;
class ExportQueueAdapter;
class MainWindow;
class RamPreviewController;
class ScriptPanelRuntime;
class TaskMonitorModel;
class PlaybackController;
class ProjectHost;
enum class ProjectHostOperationOutcome : std::uint8_t;

class McpLiveUiBinding final : public QObject {
    Q_OBJECT

  public:
    // All three are borrowed and outlive the binding.
    McpLiveUiBinding(ProjectHost& project, CompositionSession& session,
                     PlaybackController& playback, QObject* parent = nullptr);
    ~McpLiveUiBinding() override;

    // The window whose workspace and displayed Viewer the members act on. Weak: the window goes
    // before the binding.
    void setWindow(MainWindow* window);
    // The RAM Preview the Viewer button and Numpad 0 run, for transport ram-preview. Weak; without
    // one, ram-preview answers Unbound.
    void setRamPreview(RamPreviewController* ramPreview);
    // The application's one export queue adapter, for export.reveal's job lookup and selection.
    // Weak; without one, export.reveal answers Unbound.
    void setExportQueue(ExportQueueAdapter* exportQueue);
    // The Jobs panel's model, for jobs.list and jobs.cancel. Weak; without one they answer Unbound.
    void setTaskMonitor(TaskMonitorModel* tasks);
    // The Script editor's runtime, for script.run. Weak; without one (a build without embedded
    // Python) script.run answers Unbound.
    void setScriptRuntime(const std::shared_ptr<ScriptPanelRuntime>& runtime);

    // The members, bound to this object (which must outlive the service).
    [[nodiscard]] mcp::McpLiveUiPort port();

    // The members themselves, public so the port and tests call the same code.
    [[nodiscard]] bool revealComposition(document::CompositionId composition,
                                         std::optional<core::RationalTime> time);
    void projectLifecycle(mcp::McpProjectRequest request,
                          std::function<void(mcp::McpProjectResult)> done);
    [[nodiscard]] mcp::McpUiResult setSelection(const mcp::McpSelectionRequest& request);
    [[nodiscard]] mcp::McpUiResult activateComposition(document::CompositionId composition);
    [[nodiscard]] mcp::McpUiResult transport(const mcp::McpTransportRequest& request);
    [[nodiscard]] mcp::McpUiResult setViewer(const mcp::McpViewerRequest& request);
    [[nodiscard]] mcp::McpUiResult setColorSettings(const document::ColorSettings& settings);
    [[nodiscard]] mcp::McpUiResult revealExport(std::optional<document::ExportJobId> job);
    // MCP parity (#389), mcp_live_ui_binding_application.cpp: Preferences, Edit > Purge, the Jobs
    // panel, Cancel Open, Reveal in File Manager and the composition presets.
    [[nodiscard]] std::vector<mcp::McpPreference> preferences() const;
    [[nodiscard]] mcp::McpUiResult setPreferences(const std::vector<mcp::McpPreference>& entries);
    [[nodiscard]] mcp::McpUiResult purgeCache(bool media);
    [[nodiscard]] std::vector<mcp::McpJob> jobs() const;
    [[nodiscard]] mcp::McpUiResult cancelJob(std::uint64_t job);
    [[nodiscard]] mcp::McpUiResult cancelOpen();
    [[nodiscard]] mcp::McpUiResult revealAsset(document::AssetId asset);
    [[nodiscard]] std::vector<mcp::McpCompositionPreset> compositionPresets() const;
    [[nodiscard]] mcp::McpUiResult saveCompositionPreset(document::CompositionId composition,
                                                         const std::string& name);
    [[nodiscard]] mcp::McpUiResult renameCompositionPreset(const std::string& name,
                                                           const std::string& newName);
    [[nodiscard]] mcp::McpUiResult deleteCompositionPreset(const std::string& name);
    // The opt-in agent routes (#579, #603). Each is refused with NotAllowed unless the artist has
    // switched it on in Preferences (read from the saved settings on every call).
    [[nodiscard]] mcp::McpUiResult answerDialog(const std::string& button);
    void runScript(mcp::McpScriptRequest request,
                   std::function<void(mcp::McpScriptResult)> done);
    void cancelScript();
    // Read-back for the history, viewer and preview-cache queries.
    [[nodiscard]] mcp::McpUndoHistory undoHistory() const;
    [[nodiscard]] mcp::McpViewerState viewerState();
    [[nodiscard]] mcp::McpPreviewCacheState previewCache() const;
    [[nodiscard]] mcp::McpViewerRoute viewerRoute();

  private:
    struct Pending;

    // The refusal text while a modal dialog is open: its title, message and buttons, so a client
    // can tell which dialog blocks it. Buttons that discard or overwrite data say so.
    [[nodiscard]] static QString openDialogMessage();

    void onSaveFinished(ProjectHostOperationOutcome outcome, const QString& message);
    void onOpenFinished(ProjectHostOperationOutcome outcome, const QString& message);
    void onCopyFinished(ProjectHostOperationOutcome outcome, const QString& message);
    void beginPending(mcp::McpProjectRequestKind kind,
                      std::function<void(mcp::McpProjectResult)> done);
    void finishPending(mcp::McpUiRefusal refusal, const QString& message);
    void settleThenRun(const mcp::McpProjectRequest& request, std::function<void()> run);
    void pollJobs();
    [[nodiscard]] QString projectName() const;
    [[nodiscard]] std::optional<mcp::McpProjectResult>
    refuseUnsettled(const mcp::McpProjectRequest& request) const;
    [[nodiscard]] mcp::McpProjectResult projectResult(mcp::McpUiRefusal refusal,
                                                      const QString& message) const;

    ProjectHost& project_;
    CompositionSession& session_;
    PlaybackController& playback_;
    QPointer<MainWindow> window_;
    QPointer<RamPreviewController> ramPreview_;
    QPointer<ExportQueueAdapter> exportQueue_;
    QPointer<TaskMonitorModel> tasks_;
    std::weak_ptr<ScriptPanelRuntime> script_;
    std::unique_ptr<Pending> pending_;
    QTimer* jobsTimer_ = nullptr;
};

} // namespace bloom::ui
