#include <bloom/ui/mcp_live_ui_binding.hpp>

#include "composition_editor_support.hpp"

#include <bloom/document/project.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <set>
#include <system_error>

namespace bloom::ui {

using mcp::McpUiRefusal;

struct McpLiveUiBinding::Pending {
    mcp::McpProjectRequestKind kind = mcp::McpProjectRequestKind::Save;
    std::function<void(mcp::McpProjectResult)> done;
    // Set while the export queue's jobs are stopping before the request goes on.
    std::function<void()> afterJobs;
    std::chrono::steady_clock::time_point jobsDeadline{};
};

namespace {

constexpr std::chrono::seconds kJobsStopTimeout{15};

mcp::McpUiResult uiResult(const McpUiRefusal refusal, const QString& message) {
    return {.refusal = refusal, .message = message.toStdString()};
}

mcp::McpUiResult uiOk(const QString& message) { return uiResult(McpUiRefusal::None, message); }

bool isBloomFile(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".bloom";
}

QString quoted(const QString& name) { return QStringLiteral("'%1'").arg(name); }

} // namespace

McpLiveUiBinding::McpLiveUiBinding(ProjectHost& project, CompositionSession& session,
                                   PlaybackController& playback, QObject* const parent)
    : QObject(parent), project_(project), session_(session), playback_(playback),
      jobsTimer_(new QTimer(this)) {
    jobsTimer_->setInterval(25);
    connect(jobsTimer_, &QTimer::timeout, this, &McpLiveUiBinding::pollJobs);
    connect(&project_, &ProjectHost::saveFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                onSaveFinished(outcome, message);
            });
    connect(&project_, &ProjectHost::openFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                onOpenFinished(outcome, message);
            });
    connect(&project_, &ProjectHost::copyFinished, this,
            [this](const ProjectHostOperationOutcome outcome, const QString& message) {
                onCopyFinished(outcome, message);
            });
}

McpLiveUiBinding::~McpLiveUiBinding() = default;

void McpLiveUiBinding::setWindow(MainWindow* const window) { window_ = window; }

void McpLiveUiBinding::setRamPreview(RamPreviewController* const ramPreview) {
    ramPreview_ = ramPreview;
}

void McpLiveUiBinding::setExportQueue(ExportQueueAdapter* const exportQueue) {
    exportQueue_ = exportQueue;
}

void McpLiveUiBinding::setTaskMonitor(TaskMonitorModel* const tasks) { tasks_ = tasks; }

mcp::McpLiveUiPort McpLiveUiBinding::port() {
    return {
        .revealComposition =
            [this](const document::CompositionId composition,
                   const std::optional<core::RationalTime> time) {
                return revealComposition(composition, time);
            },
        .projectLifecycle =
            [this](mcp::McpProjectRequest request,
                   std::function<void(mcp::McpProjectResult)> done) {
                projectLifecycle(std::move(request), std::move(done));
            },
        .setSelection = [this](const mcp::McpSelectionRequest& request) {
            return setSelection(request);
        },
        .activateComposition =
            [this](const document::CompositionId composition) {
                return activateComposition(composition);
            },
        .transport = [this](const mcp::McpTransportRequest& request) {
            return transport(request);
        },
        .setViewer = [this](const mcp::McpViewerRequest& request) { return setViewer(request); },
        .setColorSettings =
            [this](const document::ColorSettings& settings) { return setColorSettings(settings); },
        .revealExport =
            [this](const std::optional<document::ExportJobId> job) { return revealExport(job); },
        .preferences = [this] { return preferences(); },
        .setPreferences = [this](const std::vector<mcp::McpPreference>& entries) {
            return setPreferences(entries);
        },
        .purgeCache = [this](const bool media) { return purgeCache(media); },
        .jobs = [this] { return jobs(); },
        .cancelJob = [this](const std::uint64_t job) { return cancelJob(job); },
        .cancelOpen = [this] { return cancelOpen(); },
        .revealAsset = [this](const document::AssetId asset) { return revealAsset(asset); },
        .compositionPresets = [this] { return compositionPresets(); },
        .saveCompositionPreset =
            [this](const document::CompositionId composition, const std::string& name) {
                return saveCompositionPreset(composition, name);
            },
        .renameCompositionPreset =
            [this](const std::string& name, const std::string& newName) {
                return renameCompositionPreset(name, newName);
            },
        .deleteCompositionPreset =
            [this](const std::string& name) { return deleteCompositionPreset(name); },
        .answerDialog = [this](const std::string& button) { return answerDialog(button); },
        .runScript =
            [this](mcp::McpScriptRequest request,
                   std::function<void(mcp::McpScriptResult)> done) {
                runScript(std::move(request), std::move(done));
            },
        .cancelScript = [this] { cancelScript(); },
        .undoHistory = [this] { return undoHistory(); },
        .viewerState = [this] { return viewerState(); },
        .previewCache = [this] { return previewCache(); },
    };
}

// ---- editor.reveal and composition.activate ----------------------------------------------------

bool McpLiveUiBinding::revealComposition(const document::CompositionId composition,
                                         const std::optional<core::RationalTime> time) {
    if (!window_ || window_->workspaceHost() == nullptr)
        return false;
    const auto* found = session_.snapshot().project().findComposition(composition);
    if (found == nullptr)
        return false;
    if (time && (*time < core::RationalTime::fromInteger(0) || !(*time < found->duration())))
        return false;
    if (!session_.setComposition(composition))
        return false;
    auto* workspace = window_->workspaceHost();
    if (workspace->revealEditor("bloom.viewer") == nullptr)
        return false;
    // The Timeline is where a new area is split off, so reveal it last: the Viewer keeps its place.
    if (workspace->revealEditor("bloom.timeline") == nullptr)
        return false;
    if (time) {
        playback_.pause();
        (void)session_.setCurrentTime(*time);
    }
    return true;
}

mcp::McpUiResult McpLiveUiBinding::activateComposition(const document::CompositionId composition) {
    const auto* found = session_.snapshot().project().findComposition(composition);
    if (found == nullptr)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("This project has no composition with that id."));
    if (!session_.setComposition(composition))
        return uiResult(McpUiRefusal::Failed, tr("The composition could not be made active."));
    return uiOk(tr("%1 is the active composition.").arg(quoted(QString::fromStdString(found->name()))));
}

// ---- selection.set -----------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::setSelection(const mcp::McpSelectionRequest& request) {
    const auto* target = session_.snapshot().project().findComposition(request.composition);
    if (target == nullptr)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("This project has no composition with that id."));
    const bool clearing = request.mode == mcp::McpSelectionMode::Clear;
    // A selection is of the active composition, as every editor shows it; making it active first is
    // what choosing it in the Assets panel does.
    if (request.composition != session_.compositionId() &&
        !session_.setComposition(request.composition))
        return uiResult(McpUiRefusal::Failed, tr("The composition could not be made active."));
    if (clearing) {
        session_.clearSelection();
        return uiOk(tr("Nothing is selected."));
    }

    const auto& graph = target->graph();
    std::vector<document::NodeId> layerNodes;
    for (const auto layer : request.layers) {
        const auto boundary = session_.boundaryNodeForLayer(layer);
        if (graph.findLayer(layer) == nullptr || !boundary)
            return uiResult(McpUiRefusal::UnknownTarget,
                            tr("The composition has no layer %1.").arg(layer.value()));
        layerNodes.push_back(*boundary);
    }
    for (const auto node : request.nodes)
        if (graph.findNode(node) == nullptr)
            return uiResult(McpUiRefusal::UnknownTarget,
                            tr("The composition has no node %1.").arg(node.value()));

    using Mode = mcp::McpSelectionMode;
    const bool layersOnly = request.nodes.empty() && !request.layers.empty();
    if (layersOnly && request.mode != Mode::Remove) {
        // Exactly the Timeline's own clicks: the first layer replaces, the rest extend.
        bool extend = request.mode == Mode::Add;
        for (const auto layer : request.layers) {
            session_.selectLayer(layer, extend);
            extend = true;
        }
    } else {
        std::set<document::NodeId> wanted(layerNodes.begin(), layerNodes.end());
        wanted.insert(request.nodes.begin(), request.nodes.end());
        std::set<document::NodeId> next;
        if (request.mode == Mode::Set) {
            next = wanted;
        } else if (request.mode == Mode::Add) {
            next = session_.selectedNodes();
            next.insert(wanted.begin(), wanted.end());
        } else {
            next = session_.selectedNodes();
            for (const auto node : wanted)
                next.erase(node);
        }
        if (next.empty()) {
            session_.clearSelection();
        } else {
            // The newest request is the primary one, as the last click is; otherwise the current
            // primary stays while it is still selected.
            document::NodeId primary = *next.begin();
            if (const auto* current = session_.selectedNode(); current && next.contains(current->id))
                primary = current->id;
            if (request.mode != Mode::Remove) {
                if (!request.nodes.empty())
                    primary = request.nodes.back();
                else if (!layerNodes.empty())
                    primary = layerNodes.back();
            }
            session_.selectNodes(std::move(next), primary);
        }
    }
    const auto count = static_cast<int>(session_.selectedNodes().size());
    return uiOk(count == 0 ? tr("Nothing is selected.") : tr("%n item(s) selected.", nullptr, count));
}

// ---- transport ---------------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::transport(const mcp::McpTransportRequest& request) {
    using Action = mcp::McpTransportAction;
    const auto context = frameContextFor(session_);
    if (!context)
        return uiResult(McpUiRefusal::InvalidArgument,
                        tr("There is no active composition to play. Activate one first."));
    const auto seekTo = [&](const std::uint64_t frame) {
        const auto time = frameTimeForIndex(context->frameRate, context->duration, frame);
        if (!time)
            return uiResult(McpUiRefusal::Failed, tr("That frame cannot be shown."));
        // The footer pauses first, then writes the exact time.
        playback_.pause();
        (void)session_.setCurrentTime(*time);
        return uiOk(tr("The playhead is at frame %1.").arg(frame));
    };
    switch (request.action) {
    case Action::Seek: {
        if (!request.time)
            return uiResult(McpUiRefusal::InvalidArgument, tr("Seek needs a time."));
        const auto time = *request.time;
        if (time < core::RationalTime::fromInteger(0) || !(time < context->duration))
            return uiResult(McpUiRefusal::InvalidArgument,
                            tr("That time is outside the composition (0 up to %1 seconds).")
                                .arg(context->duration.toSeconds(), 0, 'f', 3));
        playback_.pause();
        (void)session_.setCurrentTime(time);
        return uiOk(tr("The playhead moved."));
    }
    case Action::Play:
        playback_.play();
        return uiOk(tr("Playing."));
    case Action::Stop:
        // Stop also ends a RAM preview that is caching, keeping what it cached (as Escape does).
        if (ramPreview_ != nullptr && ramPreview_->isCaching())
            ramPreview_->cancel();
        playback_.pause();
        return uiOk(tr("Stopped."));
    case Action::RamPreview:
        if (ramPreview_ == nullptr)
            return uiResult(McpUiRefusal::Unbound, tr("RAM Preview is not available here."));
        if (ramPreview_->isCaching())
            return uiOk(tr("A RAM preview is already caching."));
        ramPreview_->start();
        if (!ramPreview_->isCaching())
            return uiResult(McpUiRefusal::Failed,
                            tr("RAM Preview could not start for this composition."));
        return uiOk(tr("Caching a RAM preview; it plays when the range is cached."));
    case Action::StepForward:
    case Action::StepBackward: {
        const auto nearest = nearestFrameIndexForTime(context->frameRate, context->duration,
                                                      session_.currentTime());
        if (!nearest)
            return uiResult(McpUiRefusal::Failed, tr("The playhead has no frame to step from."));
        std::uint64_t target = *nearest;
        if (request.action == Action::StepBackward)
            target = target > 0 ? target - 1 : 0;
        else
            target = std::min(target + 1, context->maxFrameIndexValue);
        return seekTo(target);
    }
    case Action::GoToStart:
        return seekTo(0);
    case Action::GoToEnd:
        return seekTo(context->maxFrameIndexValue);
    }
    return uiResult(McpUiRefusal::InvalidArgument, tr("Unknown transport action."));
}

// ---- viewer.set --------------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::setViewer(const mcp::McpViewerRequest& request) {
    auto* viewer = window_ ? window_->displayedViewer() : nullptr;
    if (viewer == nullptr)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("There is no Viewer to change. Show one with editor.reveal."));
    ViewerEditor::RemoteChange change;
    change.display = request.display;
    change.view = request.view;
    change.exposure = request.exposure;
    if (request.channel)
        change.channel = static_cast<ViewerChannel>(*request.channel);
    if (request.zoom) {
        switch (*request.zoom) {
        case mcp::McpViewerZoom::Fit:
            change.zoom = ViewerEditor::RemoteChange::Zoom::Fit;
            break;
        case mcp::McpViewerZoom::ActualSize:
            change.zoom = ViewerEditor::RemoteChange::Zoom::ActualSize;
            break;
        case mcp::McpViewerZoom::Factor:
            if (!request.zoomFactor)
                return uiResult(McpUiRefusal::InvalidArgument, tr("A zoom factor needs a value."));
            change.zoom = ViewerEditor::RemoteChange::Zoom::Factor;
            change.zoomFactor = *request.zoomFactor;
            break;
        }
    }
    change.gamma = request.gamma;
    if (request.resolution)
        change.resolution = static_cast<int>(*request.resolution);
    if (request.background)
        change.background = static_cast<ViewerBackground>(*request.background);
    change.transparencyGrid = request.transparencyGrid;
    change.look = request.look;
    if (request.regionOfInterest) {
        change.regionOfInterest = std::optional<QRectF>{};
        if (const auto& region = *request.regionOfInterest)
            *change.regionOfInterest = QRectF(region->x, region->y, region->width, region->height);
    }
    const auto refusal = viewer->applyRemoteChange(change);
    if (!refusal.isEmpty())
        return uiResult(McpUiRefusal::InvalidArgument, refusal);
    return uiOk(tr("The Viewer was updated."));
}

// ---- query kinds: history, viewer, preview-cache ------------------------------------------------

mcp::McpUndoHistory McpLiveUiBinding::undoHistory() const {
    mcp::McpUndoHistory history;
    const auto* stack = project_.liveDocumentAndStack().second;
    if (stack == nullptr)
        return history;
    history.labels = stack->historyLabels();
    history.position = stack->historyPosition();
    return history;
}

mcp::McpViewerState McpLiveUiBinding::viewerState() {
    mcp::McpViewerState state;
    auto* viewer = window_ ? window_->displayedViewer() : nullptr;
    if (viewer == nullptr)
        return state;
    const auto shown = viewer->remoteState();
    state.available = true;
    state.display = shown.display.toStdString();
    state.view = shown.view.toStdString();
    state.exposure = shown.exposure;
    state.gamma = shown.gamma;
    state.channel = static_cast<mcp::McpViewerChannel>(shown.channel);
    state.fit = shown.fitToWindow;
    state.zoomFactor = shown.zoom;
    state.resolution = static_cast<mcp::McpViewerResolution>(shown.resolution);
    state.background = static_cast<mcp::McpViewerBackground>(shown.background);
    state.transparencyGrid = shown.transparencyGrid;
    state.look = shown.look;
    if (shown.regionOfInterest)
        state.regionOfInterest = mcp::McpViewerRegion{.x = shown.regionOfInterest->x(),
                                                      .y = shown.regionOfInterest->y(),
                                                      .width = shown.regionOfInterest->width(),
                                                      .height = shown.regionOfInterest->height()};
    return state;
}

mcp::McpPreviewCacheState McpLiveUiBinding::previewCache() const {
    mcp::McpPreviewCacheState state;
    if (ramPreview_ == nullptr)
        return state;
    state.available = true;
    state.filling = ramPreview_->isCaching();
    state.cachedFrames = ramPreview_->cachedFrameCount();
    state.totalFrames = ramPreview_->totalFrameCount();
    state.longestRun = ramPreview_->longestRetainedRun();
    if (!ramPreview_->isCaching()) {
        if (const auto last = ramPreview_->lastCompletion()) {
            switch (*last) {
            case RamPreviewController::Completion::Complete:
                state.stopReason = mcp::McpPreviewStop::Complete;
                break;
            case RamPreviewController::Completion::PartialBudget:
                state.stopReason = mcp::McpPreviewStop::Budget;
                break;
            case RamPreviewController::Completion::Cancelled:
                state.stopReason = mcp::McpPreviewStop::Cancelled;
                break;
            case RamPreviewController::Completion::Failed:
                state.stopReason = mcp::McpPreviewStop::Failed;
                break;
            }
        }
    }
    return state;
}

// ---- project.set-color-settings ----------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::setColorSettings(const document::ColorSettings& settings) {
    using Status = host::ProjectSessionColorSettingsStatus;
    if (project_.isBusy() || pending_)
        return uiResult(McpUiRefusal::Busy,
                        tr("Bloom is busy with a project operation. Try again in a moment."));
    switch (project_.setColorSettings(settings)) {
    case Status::Updated:
        return uiOk(tr("The project color settings changed."));
    case Status::NoChange:
        return uiOk(tr("The project already uses these color settings."));
    case Status::ReadOnly:
        return uiResult(McpUiRefusal::ReadOnly,
                        tr("%1 is read-only, so its color settings cannot change.")
                            .arg(quoted(projectName())));
    case Status::InvalidSettings:
        return uiResult(McpUiRefusal::InvalidArgument,
                        tr("Those color settings cannot be used."));
    case Status::InvalidSession:
        break;
    }
    return uiResult(McpUiRefusal::Failed, tr("There is no project to change."));
}

// ---- export.reveal -----------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::revealExport(const std::optional<document::ExportJobId> job) {
    if (!exportQueue_)
        return uiResult(McpUiRefusal::Unbound, tr("This Bloom window has no export queue."));
    if (job) {
        // The queue as the Export editor shows it: saved jobs and the running transient ones.
        const auto snapshot = exportQueue_->snapshot();
        const bool queued = snapshot != nullptr &&
                            std::ranges::any_of(snapshot->jobs, [&](const host::ExportJobView& view) {
                                return view.id == *job;
                            });
        if (!queued)
            return uiResult(McpUiRefusal::UnknownTarget,
                            tr("Export job %1 is not in the queue; export.status lists the jobs.")
                                .arg(job->value()));
    }
    // The menus' reveal policy: focus an open Export editor, else take the Timeline's panel, else a
    // new panel along the bottom.
    auto* workspace = window_ ? window_->workspaceHost() : nullptr;
    if (workspace == nullptr || workspace->revealEditor("bloom.export") == nullptr)
        return uiResult(McpUiRefusal::Failed, tr("The Export panel could not be shown"));
    if (!job)
        return uiOk(tr("The Export panel is shown."));
    // Jobs > Show in Export: the panel (now open) selects the job; its reveal finds it in view.
    exportQueue_->showJob(*job);
    return uiOk(tr("The Export panel shows job %1.").arg(job->value()));
}

// ---- project lifecycle -------------------------------------------------------------------------

QString McpLiveUiBinding::projectName() const {
    const auto path = project_.displayPath();
    return path ? QString::fromStdString(path->stem().string()) : tr("Untitled");
}

mcp::McpProjectResult McpLiveUiBinding::projectResult(const McpUiRefusal refusal,
                                                      const QString& message) const {
    mcp::McpProjectResult result;
    result.refusal = refusal;
    result.message = message.toStdString();
    if (const auto path = project_.displayPath())
        result.path = *path;
    result.projectName = projectName().toStdString();
    return result;
}

std::optional<mcp::McpProjectResult>
McpLiveUiBinding::refuseUnsettled(const mcp::McpProjectRequest& request) const {
    if (project_.isDirty() && !request.discardChanges)
        return projectResult(
            McpUiRefusal::UnsavedChanges,
            tr("%1 has unsaved changes. Save it first, or pass discardChanges to throw them away.")
                .arg(quoted(projectName())));
    const auto* queue = project_.exportQueue();
    const auto jobs = queue != nullptr ? queue->activeJobCount() : std::size_t{0};
    if (jobs > 0 && !request.stopJobs)
        return projectResult(
            McpUiRefusal::JobsRunning,
            tr("%n export job(s) are running. Wait for them, or pass stopJobs to stop them.",
               nullptr, static_cast<int>(jobs)));
    return std::nullopt;
}

namespace {

// A dialog's visible, enabled buttons as their accelerator-free labels, and whether pressing one
// discards or overwrites data (named in the busy refusal and by dialog.answer).
struct DialogButton final {
    QPointer<QAbstractButton> button;
    QString label;
    bool discards = false;
};

std::vector<DialogButton> dialogButtons(QWidget* const dialog, const bool enabledOnly) {
    std::vector<DialogButton> buttons;
    for (auto* button : dialog->findChildren<QAbstractButton*>()) {
        if (!button->isVisible() || button->text().isEmpty() ||
            (enabledOnly && !button->isEnabled()))
            continue;
        auto label = button->text();
        label.remove(QLatin1Char('&'));
        const auto lower = label.toLower();
        const bool discards = lower.contains(QStringLiteral("discard")) ||
                              lower.contains(QStringLiteral("don't save")) ||
                              lower.contains(QStringLiteral("overwrite")) ||
                              lower.contains(QStringLiteral("replace"));
        buttons.push_back({button, label, discards});
    }
    return buttons;
}

QString describeButtons(const std::vector<DialogButton>& buttons) {
    QStringList described;
    for (const auto& entry : buttons)
        described.push_back(entry.discards ? entry.label +
                                                 QCoreApplication::translate(
                                                     "bloom::ui::McpLiveUiBinding",
                                                     " (discards or overwrites data)")
                                           : entry.label);
    return described.join(QStringLiteral(", "));
}

} // namespace

QString McpLiveUiBinding::openDialogMessage() {
    auto* dialog = QApplication::activeModalWidget();
    if (dialog == nullptr)
        return tr("A dialog is open in Bloom. Close it, then try again.");
    QString text = tr("A dialog is open in Bloom");
    if (!dialog->windowTitle().isEmpty())
        text += tr(": \u201c%1\u201d").arg(dialog->windowTitle());
    // A message box carries its text as properties; any other dialog has only its title.
    const auto message = dialog->property("text").toString();
    const auto detail = dialog->property("informativeText").toString();
    if (!message.isEmpty())
        text += tr(". It says: %1").arg(message);
    if (!detail.isEmpty())
        text += tr(" %1").arg(detail);
    const auto buttons = describeButtons(dialogButtons(dialog, false));
    if (!buttons.isEmpty())
        text += tr(". Its buttons: %1").arg(buttons);
    return text + tr(". Close it, then try again, or press one of its buttons with dialog.answer "
                     "when the artist has allowed agents to answer dialogs.");
}

// ---- dialog.answer (#579) ----------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::answerDialog(const std::string& requested) {
    // The artist's switch, read from the saved settings each time: it is only ever turned on in
    // Preferences, never over MCP.
    if (!loadApplicationPreferences(QSettings{}).agentsAnswerDialogs)
        return uiResult(McpUiRefusal::NotAllowed,
                        tr("Agents are not allowed to answer dialogs. The artist can allow it in "
                           "Edit > Preferences > General > Agents; until then the dialog has to "
                           "be answered in Bloom."));
    auto* dialog = QApplication::activeModalWidget();
    if (dialog == nullptr)
        return uiResult(McpUiRefusal::InvalidArgument, tr("No dialog is open in Bloom."));
    const auto buttons = dialogButtons(dialog, true);
    auto wanted = QString::fromStdString(requested).trimmed();
    wanted.remove(QLatin1Char('&'));
    const auto found = std::ranges::find_if(buttons, [&](const DialogButton& entry) {
        return entry.label.compare(wanted, Qt::CaseInsensitive) == 0;
    });
    if (found == buttons.end())
        return uiResult(McpUiRefusal::InvalidArgument,
                        tr("The open dialog has no button \u201c%1\u201d. Its buttons: %2.")
                            .arg(wanted, describeButtons(buttons)));
    // Pressed from the event loop, after this call has answered, as a click would be.
    QTimer::singleShot(0, found->button, [button = found->button] {
        if (button != nullptr && button->isEnabled())
            button->click();
    });
    QString message = tr("Pressed \u201c%1\u201d.").arg(found->label);
    if (found->discards)
        message += tr(" That button discards or overwrites data.");
    return uiOk(message);
}

void McpLiveUiBinding::projectLifecycle(mcp::McpProjectRequest request,
                                        std::function<void(mcp::McpProjectResult)> done) {
    using Kind = mcp::McpProjectRequestKind;
    const auto refuse = [&](const McpUiRefusal refusal, const QString& message) {
        done(projectResult(refusal, message));
    };
    if (pending_ || project_.isBusy()) {
        refuse(McpUiRefusal::Busy,
               tr("Bloom is busy with another project operation. Try again in a moment."));
        return;
    }
    if (QApplication::activeModalWidget() != nullptr) {
        refuse(McpUiRefusal::Busy, openDialogMessage());
        return;
    }

    const auto checkPath = [&](const bool mustExist) -> std::optional<QString> {
        const auto& path = request.path;
        if (path.empty() || !path.is_absolute() || !isBloomFile(path))
            return tr("The path must be an absolute path to a .bloom file.");
        std::error_code error;
        if (mustExist) {
            if (!std::filesystem::is_regular_file(path, error))
                return tr("There is no project file at %1.")
                    .arg(QString::fromStdString(path.generic_string()));
        } else if (!std::filesystem::is_directory(path.parent_path(), error)) {
            return tr("The folder %1 does not exist.")
                .arg(QString::fromStdString(path.parent_path().generic_string()));
        }
        return std::nullopt;
    };
    const auto readOnly = [&] {
        refuse(McpUiRefusal::ReadOnly,
               tr("%1 is read-only, so it cannot be saved.").arg(quoted(projectName())));
    };

    switch (request.kind) {
    case Kind::Save:
        if (!project_.canSave())
            return readOnly();
        if (!project_.displayPath()) {
            refuse(McpUiRefusal::NoPath,
                   tr("%1 has never been saved. Use project.save-as with a path.")
                       .arg(quoted(projectName())));
            return;
        }
        beginPending(request.kind, std::move(done));
        project_.beginSave();
        return;
    case Kind::SaveAs: {
        if (const auto bad = checkPath(false)) {
            refuse(McpUiRefusal::PathInvalid, *bad);
            return;
        }
        if (!project_.canSave())
            return readOnly();
        std::error_code error;
        if (std::filesystem::exists(request.path, error) && !request.replace) {
            refuse(McpUiRefusal::DestinationExists,
                   tr("%1 already exists. Pass replace to overwrite it.")
                       .arg(QString::fromStdString(request.path.generic_string())));
            return;
        }
        beginPending(request.kind, std::move(done));
        project_.beginSaveAs(request.path);
        return;
    }
    case Kind::SaveCopy: {
        if (const auto bad = checkPath(false)) {
            refuse(McpUiRefusal::PathInvalid, *bad);
            return;
        }
        if (!project_.canSaveCopy()) {
            refuse(McpUiRefusal::InvalidArgument,
                   tr("%1 can be saved normally. Use project.save-as for a copy.")
                       .arg(quoted(projectName())));
            return;
        }
        std::error_code error;
        if (std::filesystem::exists(request.path, error) && !request.replace) {
            refuse(McpUiRefusal::DestinationExists,
                   tr("%1 already exists. Pass replace to overwrite it.")
                       .arg(QString::fromStdString(request.path.generic_string())));
            return;
        }
        beginPending(request.kind, std::move(done));
        project_.beginSaveCopy(request.path);
        return;
    }
    case Kind::Open:
    case Kind::New:
    case Kind::Close: {
        if (request.kind == Kind::Open)
            if (const auto bad = checkPath(true)) {
                refuse(McpUiRefusal::PathInvalid, *bad);
                return;
            }
        if (const auto refusal = refuseUnsettled(request)) {
            done(*refusal);
            return;
        }
        beginPending(request.kind, std::move(done));
        settleThenRun(request, [this, request] {
            if (request.kind == Kind::Open) {
                project_.beginOpen(request.path);
                return;
            }
            if (!project_.replaceWithBlankProject()) {
                finishPending(McpUiRefusal::Busy,
                              tr("Bloom is busy with another project operation."));
                return;
            }
            finishPending(McpUiRefusal::None,
                          request.kind == Kind::New ? tr("A new untitled project is open.")
                                                    : tr("The project was closed; Bloom shows an "
                                                         "empty project."));
        });
        return;
    }
    }
}

void McpLiveUiBinding::beginPending(const mcp::McpProjectRequestKind kind,
                                    std::function<void(mcp::McpProjectResult)> done) {
    pending_ = std::make_unique<Pending>();
    pending_->kind = kind;
    pending_->done = std::move(done);
    // The window shows no dialog for a call an agent made (ProjectHost::agentOperation()).
    project_.setAgentOperation(true);
}

void McpLiveUiBinding::settleThenRun(const mcp::McpProjectRequest&, std::function<void()> run) {
    auto* queue = project_.exportQueue();
    if (queue == nullptr || queue->activeJobCount() == 0) {
        run();
        return;
    }
    // stopJobs was asked for: stop through the queue, then go on once it has let go.
    (void)queue->stopAll();
    pending_->afterJobs = std::move(run);
    pending_->jobsDeadline = std::chrono::steady_clock::now() + kJobsStopTimeout;
    jobsTimer_->start();
}

void McpLiveUiBinding::pollJobs() {
    if (!pending_ || !pending_->afterJobs) {
        jobsTimer_->stop();
        return;
    }
    auto* queue = project_.exportQueue();
    if (queue != nullptr)
        queue->pump();
    const bool stopped = queue == nullptr || queue->activeJobCount() == 0;
    if (!stopped && std::chrono::steady_clock::now() < pending_->jobsDeadline)
        return;
    jobsTimer_->stop();
    if (!stopped) {
        finishPending(McpUiRefusal::Failed,
                      tr("The export jobs did not stop in time, so the project was left as it is."));
        return;
    }
    auto run = std::move(pending_->afterJobs);
    pending_->afterJobs = nullptr;
    run();
}

void McpLiveUiBinding::finishPending(const McpUiRefusal refusal, const QString& message) {
    if (!pending_)
        return;
    project_.setAgentOperation(false);
    const auto pending = std::move(pending_);
    pending->done(projectResult(refusal, message));
}

void McpLiveUiBinding::onSaveFinished(const ProjectHostOperationOutcome outcome,
                                      const QString& message) {
    if (!pending_ || (pending_->kind != mcp::McpProjectRequestKind::Save &&
                      pending_->kind != mcp::McpProjectRequestKind::SaveAs))
        return;
    const bool saved = outcome == ProjectHostOperationOutcome::Published ||
                       outcome == ProjectHostOperationOutcome::PublishedWithWarning;
    finishPending(saved ? McpUiRefusal::None : McpUiRefusal::Failed,
                  saved ? tr("Saved %1. %2").arg(quoted(projectName()), message) : message);
}

void McpLiveUiBinding::onCopyFinished(const ProjectHostOperationOutcome outcome,
                                      const QString& message) {
    if (!pending_ || pending_->kind != mcp::McpProjectRequestKind::SaveCopy)
        return;
    const bool saved = outcome == ProjectHostOperationOutcome::Published ||
                       outcome == ProjectHostOperationOutcome::PublishedWithWarning;
    finishPending(saved ? McpUiRefusal::None : McpUiRefusal::Failed,
                  saved ? tr("Saved a copy of %1. %2").arg(quoted(projectName()), message)
                        : message);
}

void McpLiveUiBinding::onOpenFinished(const ProjectHostOperationOutcome outcome,
                                      const QString& message) {
    if (!pending_ || pending_->kind != mcp::McpProjectRequestKind::Open)
        return;
    const bool opened = outcome == ProjectHostOperationOutcome::Published ||
                        outcome == ProjectHostOperationOutcome::PublishedWithWarning;
    finishPending(opened ? McpUiRefusal::None : McpUiRefusal::Failed,
                  opened ? tr("Opened %1.").arg(quoted(projectName())) : message);
}

} // namespace bloom::ui
