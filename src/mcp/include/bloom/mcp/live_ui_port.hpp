#pragma once

// The desktop application's session and UI seams for MCP (ADR 0034, "Owner decisions" item 9):
// the things an artist does in the application that are not project-truth edits -- opening and
// saving projects, choosing what is selected and which composition is active, moving the
// playhead, the Viewer's display settings, bringing the Export editor into view, Preferences,
// Edit > Purge, the Jobs panel, Reveal in File Manager and the composition presets -- offered to
// agents as live-only tools.
//
// One struct of std::function members so the composition root binds them in one place and a test
// fakes them as a group. Every member is called on the OWNER thread (the UI thread), marshalled
// there by McpServerService; a member left empty answers a typed refusal (McpUiRefusal::Unbound)
// instead of hiding the tool. Session-state members never dirty the project or touch its undo
// history.
//
// Qt-free: the bindings live in apps/bloom over the UI's own controllers.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/ids.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::mcp {

// Why a session or UI call was refused. Each has a stable diagnostic code (uiRefusalCode).
enum class McpUiRefusal : std::uint8_t {
    None = 0,
    // stdio: there is no application window to act on.
    NotLive = 1,
    // The application did not bind this seam.
    Unbound = 2,
    // A composition, layer or node id that the open project does not have.
    UnknownTarget = 3,
    // An argument the application cannot honour (a display/view this configuration lacks, a
    // zoom outside its range, a time outside the composition).
    InvalidArgument = 4,
    // The application is in the middle of something that must finish first (a modal dialog, a
    // project opening, another lifecycle request).
    Busy = 5,
    // Open, New or Close over unsaved changes without discardChanges.
    UnsavedChanges = 6,
    // Open, New or Close while export jobs run, without stopJobs.
    JobsRunning = 7,
    // Save As onto an existing file without replace.
    DestinationExists = 8,
    // A path that is empty, relative where an absolute one is needed, not a .bloom file, or in a
    // folder that does not exist.
    PathInvalid = 9,
    // Save of a project that has never been saved: use Save As with a path.
    NoPath = 10,
    // The project is read-only (a newer or preserved document).
    ReadOnly = 11,
    // The operation ran and failed (an I/O error, a document that would not open); the message
    // says why.
    Failed = 12,
    // The artist has not allowed this: an opt-in agent capability whose Preferences switch is off.
    NotAllowed = 13,
};

inline constexpr std::size_t kMcpUiRefusalCount = 14;

[[nodiscard]] constexpr std::string_view uiRefusalCode(const McpUiRefusal refusal) {
    constexpr std::array<std::string_view, kMcpUiRefusalCount> codes{
        "",
        "bloom.mcp.not-live",
        "bloom.mcp.ui-unbound",
        "bloom.mcp.unknown-target",
        "bloom.mcp.invalid-argument",
        "bloom.mcp.busy",
        "bloom.mcp.unsaved-changes",
        "bloom.mcp.jobs-running",
        "bloom.mcp.destination-exists",
        "bloom.mcp.path-invalid",
        "bloom.mcp.no-path",
        "bloom.mcp.read-only",
        "bloom.mcp.failed",
        "bloom.mcp.not-allowed",
    };
    return codes[static_cast<std::size_t>(refusal)];
}

// A synchronous seam's answer. `message` is plain text for the agent and the activity log; for a
// refusal it says why and what to do instead.
struct McpUiResult final {
    McpUiRefusal refusal = McpUiRefusal::None;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return refusal == McpUiRefusal::None; }
};

// ---- Project lifecycle (project.save, project.save-as, project.open, project.new, project.close)

enum class McpProjectRequestKind : std::uint8_t {
    Save = 0,
    SaveAs = 1,
    Open = 2,
    New = 3,
    Close = 4,
    // File > Save a Copy: a byte-for-byte copy of a read-only (newer or preserved) project's file
    // at `path`; the open project stays where it is.
    SaveCopy = 5,
};

struct McpProjectRequest final {
    McpProjectRequestKind kind = McpProjectRequestKind::Save;
    // SaveAs, SaveCopy and Open: an absolute path to a .bloom file.
    std::filesystem::path path;
    // SaveAs and SaveCopy: replace an existing file.
    bool replace = false;
    // Open, New and Close: discard unsaved changes instead of refusing with UnsavedChanges.
    bool discardChanges = false;
    // Open, New and Close: stop running export jobs instead of refusing with JobsRunning.
    bool stopJobs = false;
};

struct McpProjectResult final {
    McpUiRefusal refusal = McpUiRefusal::None;
    // Plain text; a refusal names the project ("Untitled has unsaved changes ...").
    std::string message;
    // The saved or opened file; empty after New, Close or an untitled project.
    std::filesystem::path path;
    // The project now open (after Close: the empty project the application shows).
    std::string projectName;

    [[nodiscard]] bool ok() const noexcept { return refusal == McpUiRefusal::None; }
};

// Asynchronous: the binding starts the request on the owner thread and calls `done` exactly once,
// on the owner thread, when it has finished or been refused. It must not call `done` from inside
// the call when it can avoid it, but the service copes either way.
using McpProjectLifecycle =
    std::function<void(McpProjectRequest, std::function<void(McpProjectResult)> done)>;

// ---- Selection (selection.set)

enum class McpSelectionMode : std::uint8_t {
    // Replace the selection with the given layers and nodes.
    Set = 0,
    Add = 1,
    Remove = 2,
    // Select nothing (layers and nodes are ignored).
    Clear = 3,
};

struct McpSelectionRequest final {
    document::CompositionId composition;
    std::vector<document::LayerId> layers;
    std::vector<document::NodeId> nodes;
    McpSelectionMode mode = McpSelectionMode::Set;
};

// ---- Transport (transport)

enum class McpTransportAction : std::uint8_t {
    // Move the playhead to `time`.
    Seek = 0,
    Play = 1,
    Stop = 2,
    StepForward = 3,
    StepBackward = 4,
    GoToStart = 5,
    GoToEnd = 6,
    // RAM Preview, as the Viewer button and Numpad 0 start it: cache the range, then play it. A
    // run already caching is left running; Stop ends it.
    RamPreview = 7,
};

struct McpTransportRequest final {
    McpTransportAction action = McpTransportAction::Seek;
    // Seek: composition time in seconds (MCP converts a whole frame at the composition's rate).
    std::optional<core::RationalTime> time;
};

// ---- Viewer (viewer.set)

enum class McpViewerChannel : std::uint8_t {
    Rgba = 0,
    Rgb = 1,
    Red = 2,
    Green = 3,
    Blue = 4,
    Alpha = 5,
};

enum class McpViewerZoom : std::uint8_t {
    // Fit the composition in the Viewer.
    Fit = 0,
    // 100 percent.
    ActualSize = 1,
    // `zoomFactor`.
    Factor = 2,
};

// The Viewer's preview resolution (the footer's Resolution dropdown).
enum class McpViewerResolution : std::uint8_t {
    Auto = 0,
    Full = 1,
    Half = 2,
    Quarter = 3,
};

// The surround outside the composition frame (View > Background).
enum class McpViewerBackground : std::uint8_t {
    // The composition's own background colour.
    Solid = 0,
    Gray = 1,
    Black = 2,
    White = 3,
};

// A region of interest in composition pixels.
struct McpViewerRegion final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// Only the fields that are present change.
struct McpViewerRequest final {
    // OCIO display and view names; an empty string returns to the project default.
    std::optional<std::string> display;
    std::optional<std::string> view;
    // Viewer exposure in stops (display-only, never rendered).
    std::optional<double> exposure;
    std::optional<McpViewerChannel> channel;
    std::optional<McpViewerZoom> zoom;
    // With zoom Factor: 1.0 is 100 percent.
    std::optional<double> zoomFactor;
    // Viewer gamma (display-only, never rendered); 1.0 is neutral.
    std::optional<double> gamma;
    std::optional<McpViewerResolution> resolution;
    std::optional<McpViewerBackground> background;
    // The checkerboard inside the composition frame.
    std::optional<bool> transparencyGrid;
    // The Viewer's look toggle (the OCIO look of the display and view).
    std::optional<bool> look;
    // Present with a value: evaluate only that region (clipped to the composition frame); present
    // and empty: clear the region of interest.
    std::optional<std::optional<McpViewerRegion>> regionOfInterest;
};

// ---- Preferences (preferences.get, preferences.set)

// One application preference by its key (the vocabulary is the binding's: docs/user-guide/mcp.md
// lists it). A number is a double; a choice or a path is a string.
using McpPreferenceValue = std::variant<bool, double, std::string>;
struct McpPreference final {
    std::string key;
    McpPreferenceValue value;
};

// ---- Python in the live session (script.run), behind its own Preferences switch

struct McpScriptRequest final {
    // Python source, run by the Script editor's interpreter against the live session: the same
    // transactions, undo history and cancellation as the Script editor. At most 65536 characters.
    std::string source;
};

struct McpScriptResult final {
    McpUiRefusal refusal = McpUiRefusal::None;
    // A refusal's reason; otherwise empty.
    std::string message;
    // The script's console output (an error's text when it failed).
    std::string output;
    bool succeeded = false;

    [[nodiscard]] bool ok() const noexcept { return refusal == McpUiRefusal::None; }
};

// ---- Jobs (jobs.list, jobs.cancel): the Jobs panel's rows

struct McpJob final {
    std::uint64_t id = 0;
    // The Jobs panel's own words for the row.
    std::string name;
    std::string phase;
    std::string state;
    std::optional<std::uint64_t> completed;
    std::optional<std::uint64_t> total;
    bool canCancel = false;
    bool cancellationRequested = false;
    // Set for an export run's row: export.stop stops it (export.reveal shows it).
    std::optional<document::ExportJobId> exportJob;
};

// ---- Composition presets (composition.presets, composition.save-preset, ...)

struct McpCompositionPreset final {
    std::string name;
    bool builtIn = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t pixelAspectNumerator = 1;
    std::uint64_t pixelAspectDenominator = 1;
    // User presets only.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> frameRate;
    std::optional<core::RationalTime> duration;
    std::optional<core::RationalTime> startTimecode;
    std::optional<bool> dropFrame;
    std::optional<std::array<double, 4>> background;
};

// ---- Read-back (query kinds history, viewer and preview-cache)

// The project's undo history, oldest entry first. `position` entries are applied; the ones after
// it are the redo branch.
struct McpUndoHistory final {
    std::vector<std::string> labels;
    std::size_t position = 0;
};

// The Viewer's display settings as viewer.set writes them. A string that is empty is the project
// default; `zoomFactor` is meaningful only when `fit` is false.
struct McpViewerState final {
    // False when no Viewer is showing.
    bool available = false;
    std::string display;
    std::string view;
    double exposure = 0.0;
    double gamma = 1.0;
    McpViewerChannel channel = McpViewerChannel::Rgba;
    bool fit = true;
    double zoomFactor = 1.0;
    McpViewerResolution resolution = McpViewerResolution::Auto;
    McpViewerBackground background = McpViewerBackground::Solid;
    bool transparencyGrid = false;
    bool look = false;
    std::optional<McpViewerRegion> regionOfInterest;
};

// How the last RAM Preview fill ended.
enum class McpPreviewStop : std::uint8_t {
    Complete = 0,
    // The range outgrew the memory budget; the frames that fit stay cached.
    Budget = 1,
    Cancelled = 2,
    Failed = 3,
};

// The RAM Preview controller's state. Every field the application does not know stays empty: an
// unknown state is reported as unknown, never as a made-up value.
struct McpPreviewCacheState final {
    // False when this window has no RAM Preview controller.
    bool available = false;
    std::optional<bool> filling;
    std::optional<std::uint64_t> cachedFrames;
    std::optional<std::uint64_t> totalFrames;
    // The longest run of consecutive cached frames, as half-open composition frame indices.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> longestRun;
    // Why the last fill stopped; empty while filling or before any fill ended.
    std::optional<McpPreviewStop> stopReason;
};

struct McpLiveUiPort final {
    // editor.reveal: show the composition in the Viewer (making it active) and, when given, move
    // the playhead to `time` (seconds). False when the composition or time cannot be shown.
    std::function<bool(document::CompositionId, std::optional<core::RationalTime>)>
        revealComposition;
    McpProjectLifecycle projectLifecycle;
    // selection.set
    std::function<McpUiResult(const McpSelectionRequest&)> setSelection;
    // composition.activate: the composition the Viewer, Timeline and contextual arguments use.
    std::function<McpUiResult(document::CompositionId)> activateComposition;
    // transport
    std::function<McpUiResult(const McpTransportRequest&)> transport;
    // viewer.set
    std::function<McpUiResult(const McpViewerRequest&)> setViewer;
    // project.set-color-settings: apply already-resolved colour settings to the open project, as
    // Project Settings > Colour does (not undoable; marks the project dirty). Refuse ReadOnly for a
    // read-only project and Busy while a project is opening.
    std::function<McpUiResult(const document::ColorSettings&)> setColorSettings;
    // export.reveal: bring the Export editor into view (WorkspaceHost's reveal policy) and, when
    // given, select the job, as Jobs > Show in Export does. UnknownTarget for a job the queue does
    // not hold; Failed when the panel could not be shown. Window state only: never changes the
    // project or its undo history. Export queue edits never call it (an agent reveals explicitly).
    std::function<McpUiResult(std::optional<document::ExportJobId>)> revealExport;
    // preferences.get: every preference the agent may read and set, with its current value.
    std::function<std::vector<McpPreference>()> preferences;
    // preferences.set: validates every entry first (InvalidArgument names the key), then commits
    // them together as the Preferences window's OK does. Busy while a modal dialog is open.
    std::function<McpUiResult(const std::vector<McpPreference>&)> setPreferences;
    // cache.purge: Edit > Purge (media = false: the preview cache; true: the media cache). Busy
    // while a purge runs. Never touches the project or its undo history.
    std::function<McpUiResult(bool media)> purgeCache;
    // jobs.list and jobs.cancel: the Jobs panel's rows and its Cancel. UnknownTarget for a job the
    // panel does not list; InvalidArgument for a row the panel cannot cancel (an export run).
    std::function<std::vector<McpJob>()> jobs;
    std::function<McpUiResult(std::uint64_t)> cancelJob;
    // project.cancel-open: File > Cancel Open. InvalidArgument when no project is opening.
    std::function<McpUiResult()> cancelOpen;
    // asset.reveal: Reveal in File Manager. UnknownTarget for an unknown asset; InvalidArgument
    // for an asset without a file.
    std::function<McpUiResult(document::AssetId)> revealAsset;
    // The Composition Settings dialog's preset list and its Save, Rename and Delete preset
    // buttons (per-user settings, never project truth or undo).
    std::function<std::vector<McpCompositionPreset>()> compositionPresets;
    std::function<McpUiResult(document::CompositionId, const std::string& name)>
        saveCompositionPreset;
    std::function<McpUiResult(const std::string& name, const std::string& newName)>
        renameCompositionPreset;
    std::function<McpUiResult(const std::string& name)> deleteCompositionPreset;
    // dialog.answer: press the named button of the modal dialog that blocks the window. NotAllowed
    // while the Preferences switch is off; InvalidArgument when no dialog is open or no visible,
    // enabled button has that text (the message lists the buttons).
    std::function<McpUiResult(const std::string& button)> answerDialog;
    // script.run: asynchronous like projectLifecycle; `done` is called exactly once on the owner
    // thread. NotAllowed while the Preferences switch is off; Busy when the Script editor's queue
    // is full; Unbound when this build has no embedded Python. cancelScript is the Script editor's
    // Cancel: it stops the running script and the commits it queued.
    std::function<void(McpScriptRequest, std::function<void(McpScriptResult)> done)> runScript;
    std::function<void()> cancelScript;
    // query kinds history, viewer and preview-cache: owner-thread reads, never project edits.
    std::function<McpUndoHistory()> undoHistory;
    std::function<McpViewerState()> viewerState;
    std::function<McpPreviewCacheState()> previewCache;
};

} // namespace bloom::mcp
