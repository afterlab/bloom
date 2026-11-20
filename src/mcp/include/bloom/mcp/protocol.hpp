#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

// MCP protocol vocabulary shared by both Bloom MCP hosts (ADR 0034): the stdio server (bloom-mcp)
// and the in-app server (Streamable HTTP inside the desktop application). Qt-free; owned by
// src/mcp. Values here are the existing bloom-mcp limits, unchanged, so a client sees the same
// bounds on both transports.
namespace bloom::mcp {

// The one MCP revision Bloom implements, for both transports.
inline constexpr std::string_view kMcpProtocolVersion = "2025-11-25";

// ---------------------------------------------------------------------------------------------
// Message limits (unchanged from bloom-mcp; docs/user-guide/mcp.md)

struct McpMessageLimits final {
    // One JSON-RPC message (stdio line or HTTP request body).
    std::size_t requestBytes = std::size_t{1024} * 1024U;
    // One JSON-RPC response (JSON body or one SSE `data:` payload).
    std::size_t responseBytes = std::size_t{4} * 1024U * 1024U;
    // JSON values in one message (array items and member values, not member names) and its
    // nesting depth.
    std::size_t jsonValues = 65536;
    std::size_t jsonDepth = 32;
    // The most memory one parse or one response build may allocate (#745: a cap, not an
    // allocation; each document allocates what it needs).
    std::size_t arenaBytes = std::size_t{32} * 1024U * 1024U;
    // Messages accepted but not yet executing, across every client of one server.
    std::size_t pendingMessages = 16;
    // Frames one render or export call may request.
    std::uint32_t renderFrames = 10000;
    // Command events retained for the `events` tool.
    std::size_t retainedCommandEvents = 1024;
    // How long export.start and export.start-all wait for a destination check that is already
    // running before they answer DestinationCheckPending.
    std::chrono::milliseconds destinationCheckWait{10000};
    // How long the one-call export waits for an encoder probe that is already running (the probe
    // a host starts at launch) before it answers with the "Checking encoders" refusal (#683).
    std::chrono::milliseconds encoderProbeWait{30000};
};

inline constexpr McpMessageLimits kMcpMessageLimits{};

// #745: a transport's own bounds, advertised in initialize beside the message limits. The
// defaults are stdio's (one client, one request at a time); the in-app server passes its
// McpServerLimits.
struct McpTransportLimits final {
    std::size_t inFlightPerClient = 1;
    std::size_t requestWorkers = 1;
    std::size_t clients = 1;
    // Open connections; 0 for a transport without them (stdio).
    std::size_t connections = 0;
};

// ---------------------------------------------------------------------------------------------
// Tools

// Every tool both hosts list. The stdio host and the in-app host offer the same set with the same
// schemas; a host that cannot honour a call answers with a typed tool error, never by hiding the
// tool. Render, export and capture run through the export queue's one runner (ADR 0033 sections
// 6 and 13); the export.* tools are the queue's session calls.
enum class McpTool : std::uint8_t {
    Query = 0,
    Transact = 1,
    History = 2,
    Events = 3,
    Render = 4,
    Export = 5,
    Capture = 6,
    ExportStart = 7,
    ExportStartAll = 8,
    ExportPause = 9,
    ExportResume = 10,
    ExportStop = 11,
    ExportStopAll = 12,
    ExportRetry = 13,
    ExportConfirmReplace = 14,
    ExportStatus = 15,
    ExportEvents = 16,
    ExportLog = 17,
    ExportWait = 18,
    ExportPresets = 19,
    ExportScript = 20,
    // Project lifecycle (ADR 0034, owner decision item 9 as amended 2026-10-30): the File menu's
    // Save, Save As, Open, New and Close. Never part of the undo history.
    ProjectSave = 21,
    ProjectSaveAs = 22,
    ProjectOpen = 23,
    ProjectNew = 24,
    ProjectClose = 25,
    // Live-only session and UI state (live_ui_port.hpp); never dirties the project.
    EditorReveal = 26,
    SelectionSet = 27,
    CompositionActivate = 28,
    Transport = 29,
    ViewerSet = 30,
    // Brings the Export editor into view and optionally selects a job (#382). Window state only.
    ExportReveal = 31,
    // Project Settings > Colour (not undoable; marks the project dirty).
    ProjectSetColorSettings = 32,
    // The runtime telemetry snapshot (#423; docs/architecture/profiler-feed.md). Read-only; holds
    // no session lease.
    ProfilerSnapshot = 33,
    // MCP parity (#389). File > Save a Copy and Cancel Open, then live-only window and
    // application state (live_ui_port.hpp): Preferences, Edit > Purge, the Jobs panel, Reveal in
    // File Manager and the Composition Settings dialog's presets. None of them is a project edit.
    ProjectSaveCopy = 34,
    ProjectCancelOpen = 35,
    PreferencesGet = 36,
    PreferencesSet = 37,
    CachePurge = 38,
    JobsList = 39,
    JobsCancel = 40,
    AssetReveal = 41,
    CompositionPresets = 42,
    CompositionSavePreset = 43,
    CompositionRenamePreset = 44,
    CompositionDeletePreset = 45,
    // The reference: every tool, query kind, operation and command, described.
    Help = 46,
    // Opt-in agent routes (each behind its own Preferences switch, off by default): answer the
    // modal dialog that blocks the window (#579), and run Python in the live session through the
    // Script editor's runtime (#603).
    DialogAnswer = 47,
    ScriptRun = 48,
    // Agent session mode (#746): quiet feedback, a deferred Viewer, slow panel refresh and one
    // undo step for the session's commits. Live only; never a project edit by itself.
    SessionBegin = 49,
    SessionEnd = 50,
    // #747 (D1): wait for an asynchronous transact job (transact with async:true), with progress
    // when the request carries a progress token, and read its result.
    JobsWait = 51,
    // #747 (D2): subscribe the calling session to coalesced change notifications
    // (notifications/bloom/events on a Streamable HTTP GET stream, or on stdout for stdio), and
    // end that subscription. Neither touches the project.
    EventsSubscribe = 52,
    EventsUnsubscribe = 53,
};

inline constexpr std::size_t kMcpToolCount = 54;

// The wire name. MCP tool names allow [A-Za-z0-9_.-]; the export.* names are ADR 0033's session
// call names verbatim.
[[nodiscard]] constexpr std::string_view toolName(const McpTool tool) {
    // clang-format off
    constexpr std::array<std::string_view, kMcpToolCount> names{
        "query",
        "transact",
        "history",
        "events",
        "render",
        "export",
        "capture",
        "export.start",
        "export.start-all",
        "export.pause",
        "export.resume",
        "export.stop",
        "export.stop-all",
        "export.retry",
        "export.confirm-replace",
        "export.status",
        "export.events",
        "export.log",
        "export.wait",
        "export.presets",
        "export.script",
        "project.save",
        "project.save-as",
        "project.open",
        "project.new",
        "project.close",
        "editor.reveal",
        "selection.set",
        "composition.activate",
        "transport",
        "viewer.set",
        "export.reveal",
        "project.set-color-settings",
        "profiler.snapshot",
        "project.save-copy",
        "project.cancel-open",
        "preferences.get",
        "preferences.set",
        "cache.purge",
        "jobs.list",
        "jobs.cancel",
        "asset.reveal",
        "composition.presets",
        "composition.save-preset",
        "composition.rename-preset",
        "composition.delete-preset",
        "help",
        "dialog.answer",
        "script.run",
        "session.begin",
        "session.end",
        "jobs.wait",
        "events.subscribe",
        "events.unsubscribe",
    };
    // clang-format on
    return names[static_cast<std::size_t>(tool)];
}

// How a tool's work runs. Inline tools finish on the request worker under bounded cost (paged
// queries, one queued commit, a cursor read). Task tools submit scheduler work (export runner
// jobs) and report progress when the request carried a progress token; their duration is
// unbounded but cancellable.
enum class McpToolExecution : std::uint8_t { Inline = 0, Task = 1 };

[[nodiscard]] constexpr McpToolExecution toolExecution(const McpTool tool) {
    switch (tool) {
    case McpTool::Render:
    case McpTool::Export:
    case McpTool::Capture:
    case McpTool::ExportWait:
    case McpTool::JobsWait:
        return McpToolExecution::Task;
    default:
        return McpToolExecution::Inline;
    }
}

// Whether a successful call changes the live document (and therefore the undo history). Only
// these tools ever reach the commit marshal.
[[nodiscard]] constexpr bool toolMutatesDocument(const McpTool tool) {
    return tool == McpTool::Transact || tool == McpTool::History;
}

// #747: whether a call that carries a progress token is answered as an event stream with
// notifications/progress: the Task tools, and transact (a long edit reports where it stands).
[[nodiscard]] constexpr bool toolStreamsProgress(const McpTool tool) {
    return toolExecution(tool) == McpToolExecution::Task || tool == McpTool::Transact;
}

// Whether a tool works on the application window rather than the project: it runs on the owner
// thread through McpLiveUiPort without a session lease, so it works while a project opens.
[[nodiscard]] constexpr bool toolUsesLiveUi(const McpTool tool) {
    const auto index = static_cast<std::uint8_t>(tool);
    return (index >= static_cast<std::uint8_t>(McpTool::ProjectSave) &&
            index <= static_cast<std::uint8_t>(McpTool::ExportReveal)) ||
           (index >= static_cast<std::uint8_t>(McpTool::ProjectSaveCopy) &&
            index <= static_cast<std::uint8_t>(McpTool::CompositionDeletePreset)) ||
           tool == McpTool::DialogAnswer || tool == McpTool::ScriptRun ||
           tool == McpTool::SessionBegin || tool == McpTool::SessionEnd;
}

// ---------------------------------------------------------------------------------------------
// Errors

// JSON-RPC protocol errors. Tool failures are not protocol errors: they are successful
// tools/call responses with isError = true and a structured diagnostic (McpToolErrorCode).
enum class McpProtocolError : std::int32_t {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    // Server-defined range.
    // Server-defined range. #745: each request limit has its own code, and its error carries
    // `data` {code, limit, value, actual, unit} (McpLimit).
    ServerBusy = -32000,
    ShuttingDown = -32001,
    ClientLimitReached = -32002,
    RequestTooLarge = -32003,
    TooManyJsonValues = -32004,
    JsonTooDeep = -32005,
    ResponseTooLarge = -32006,
    JsonMemoryExceeded = -32007,
    ClientRequestLimit = -32008,
    TooManyDataFiles = -32009,
    DataFileTooLarge = -32010,
    DataFileTooManyValues = -32011,
    DataFileTooDeep = -32012,
};

// ---------------------------------------------------------------------------------------------
// Request limits (#745, #741)

// Every bound a request can meet. initialize advertises each one's value under
// capabilities.experimental.bloom.limits.<name>, and a refusal names it with its value and the
// actual count: a JSON-RPC error with its own code and `data`, or for the commit bounds a tool
// diagnostic with its own bloom.mcp code and `limit`.
enum class McpLimit : std::uint8_t {
    RequestBytes = 0,
    ResponseBytes = 1,
    JsonValues = 2,
    JsonDepth = 3,
    JsonMemory = 4,
    DataFiles = 5,
    DataFileBytes = 6,
    DataFileValues = 7,
    DataFileDepth = 8,
    InFlightPerClient = 9,
    PendingMessages = 10,
    CommitQueue = 11,
    CommitDeadline = 12,
    CommitWork = 13,
    TransactOutputs = 14,
    // #747 (D1): asynchronous transact jobs queued or running at once, across every client.
    AsyncJobs = 15,
    // #748: what one project file can save and reopen (bloom::project::ProjectCapacityLimits). An
    // edit that would take the project past one is refused at the edit
    // (bloom.mcp.project-too-large) and nothing is applied; the project as it stands still saves.
    ProjectJsonValues = 16,
    ProjectDocumentBytes = 17,
    ProjectContainerEntries = 18,
    ProjectStringBytes = 19,
};

inline constexpr std::size_t kMcpLimitCount = 20;

struct McpLimitDescription final {
    // The member of capabilities.experimental.bloom.limits.
    std::string_view name;
    // The stable refusal code (data.code of the JSON-RPC error, or diagnostic.code of the tool).
    std::string_view code;
    // The unit of the value and the actual count.
    std::string_view unit;
    // The JSON-RPC error code of a protocol refusal; 0 for a bound refused as a tool diagnostic
    // (the commit bounds) or never refused (transactOutputs answers in summary form instead).
    std::int32_t rpcCode = 0;
};

[[nodiscard]] constexpr McpLimitDescription describeLimit(const McpLimit limit) {
    // clang-format off
    constexpr std::array<McpLimitDescription, kMcpLimitCount> table{{
        {"requestBytes", "bloom.mcp.limit.request-bytes", "bytes",
         static_cast<std::int32_t>(McpProtocolError::RequestTooLarge)},
        {"responseBytes", "bloom.mcp.limit.response-bytes", "bytes",
         static_cast<std::int32_t>(McpProtocolError::ResponseTooLarge)},
        {"jsonValues", "bloom.mcp.limit.json-values", "values",
         static_cast<std::int32_t>(McpProtocolError::TooManyJsonValues)},
        {"jsonDepth", "bloom.mcp.limit.json-depth", "levels",
         static_cast<std::int32_t>(McpProtocolError::JsonTooDeep)},
        {"jsonMemoryBytes", "bloom.mcp.limit.json-memory", "bytes",
         static_cast<std::int32_t>(McpProtocolError::JsonMemoryExceeded)},
        {"dataFilesPerCall", "bloom.mcp.limit.data-files", "files",
         static_cast<std::int32_t>(McpProtocolError::TooManyDataFiles)},
        {"dataFileBytes", "bloom.mcp.limit.data-file-bytes", "bytes",
         static_cast<std::int32_t>(McpProtocolError::DataFileTooLarge)},
        {"dataFileValues", "bloom.mcp.limit.data-file-values", "values",
         static_cast<std::int32_t>(McpProtocolError::DataFileTooManyValues)},
        {"dataFileDepth", "bloom.mcp.limit.data-file-depth", "levels",
         static_cast<std::int32_t>(McpProtocolError::DataFileTooDeep)},
        {"inFlightPerClient", "bloom.mcp.limit.in-flight", "requests",
         static_cast<std::int32_t>(McpProtocolError::ClientRequestLimit)},
        {"pendingMessages", "bloom.mcp.limit.pending", "requests",
         static_cast<std::int32_t>(McpProtocolError::ServerBusy)},
        {"commitQueue", "bloom.mcp.commit-queue-full", "commits", 0},
        {"commitDeadlineMs", "bloom.mcp.deadline-exceeded", "milliseconds", 0},
        {"commitWork", "bloom.mcp.commit-too-large", "work units", 0},
        {"transactOutputs", "bloom.mcp.outputs-summarized", "outputs", 0},
        {"asyncJobs", "bloom.mcp.jobs-full", "jobs", 0},
        {"projectJsonValues", "bloom.mcp.project-too-large", "values", 0},
        {"projectDocumentBytes", "bloom.mcp.project-too-large", "bytes", 0},
        {"projectContainerEntries", "bloom.mcp.project-too-large", "entries", 0},
        {"projectStringBytes", "bloom.mcp.project-too-large", "bytes", 0},
    }};
    // clang-format on
    return table[static_cast<std::size_t>(limit)];
}

// What every refusal of a request-shape limit suggests: the columnar key form and data files.
inline constexpr std::string_view kMcpLimitAdvice =
    "Write many keys with bloom.animation.set-key-columns (one column of times, one per "
    "component), and pass a large column as {\"fromFile\": \"<absolute .json path>\"} so it never "
    "travels in the request.";

// Stable tool diagnostic codes ("diagnostic.code" in structuredContent).
enum class McpToolErrorCode : std::uint8_t {
    // A project is opening, closing or being replaced; retry after it binds.
    ProjectUnavailable = 0,
    // The project was replaced while the call ran; nothing was applied.
    ProjectReplaced = 1,
    // expectedRevision did not match; nothing was applied.
    RevisionConflict = 2,
    // The commit queue is full; nothing was queued.
    CommitQueueFull = 3,
    // An interactive gesture stayed open until the call's deadline; nothing was applied.
    GestureOpen = 4,
    // notifications/cancelled, server stop or shutdown.
    Cancelled = 5,
    DeadlineExceeded = 6,
    // The export queue refused the job (its availability code is attached).
    ExportUnavailable = 7,
    // The export runner failed (its ExportFailure is attached).
    ExportFailed = 8,
    // A capture's PNG is larger than the inline image limit; the published file is linked instead.
    CaptureTooLargeForInline = 9,
    // The document is read-only.
    ReadOnly = 10,
    // Operation validation or adapter diagnostics (the registry's own codes are attached).
    OperationRejected = 11,
    // The render or export request itself cannot run as given (a destination the format cannot
    // write, an existing file without overwrite); the message says what to change.
    PreflightBlocked = 12,
    // #744: the transaction carries more work than one live commit takes
    // (scripting::kLiveCommitWorkLimit); nothing was queued. Split it into several transactions.
    CommitTooLarge = 13,
    // #747: kMcpAsyncJobs asynchronous transact jobs are queued or running; nothing was queued.
    JobsFull = 14,
    // #747: an idempotencyKey was used before with different arguments; nothing was applied.
    IdempotencyConflict = 15,
    // #747: a call that needs the desktop application on the stdio server (transact async:true).
    NotLive = 16,
    // #747: jobs.wait or jobs.cancel for a job id Bloom does not hold.
    UnknownJob = 17,
    // #748: the edit would make the project larger than one project file can save and reopen;
    // nothing was applied. diagnostic.limit names the limit (projectJsonValues,
    // projectDocumentBytes, projectContainerEntries or projectStringBytes), its value and what the
    // project would hold.
    ProjectTooLarge = 18,
};

inline constexpr std::size_t kMcpToolErrorCodeCount = 19;

[[nodiscard]] constexpr std::string_view toolErrorCode(const McpToolErrorCode code) {
    constexpr std::array<std::string_view, kMcpToolErrorCodeCount> names{
        "bloom.mcp.project-unavailable",
        "bloom.mcp.project-replaced",
        "bloom.mcp.revision-conflict",
        "bloom.mcp.commit-queue-full",
        "bloom.mcp.gesture-open",
        "bloom.mcp.cancelled",
        "bloom.mcp.deadline-exceeded",
        "bloom.mcp.export-unavailable",
        "bloom.mcp.export-failed",
        "bloom.mcp.capture-too-large-for-inline",
        "bloom.mcp.read-only",
        "bloom.mcp.operation-rejected",
        "bloom.mcp.preflight-blocked",
        "bloom.mcp.commit-too-large",
        "bloom.mcp.jobs-full",
        "bloom.mcp.idempotency-conflict",
        "bloom.mcp.not-live",
        "bloom.mcp.unknown-job",
        "bloom.mcp.project-too-large",
    };
    return names[static_cast<std::size_t>(code)];
}

// ---------------------------------------------------------------------------------------------
// Deadlines

// How long a commit may wait in the marshal (queue position plus an open gesture) before it is
// withdrawn with GestureOpen or DeadlineExceeded. Task tools have no deadline; they are cancelled
// explicitly.
inline constexpr std::chrono::seconds kMcpCommitDeadline{30};

// #740: when a render, export or capture that failed retryably (export.capacity.stalled, a
// publication refused memory or a busy output file) may be tried again: diagnostic.retryAfterMs.
inline constexpr std::uint64_t kMcpExportRetryAfterMs = 5000;

// #747 (D1): asynchronous transact. At most kMcpAsyncJobs jobs are queued or running at once
// (limit asyncJobs, bloom.mcp.jobs-full); the newest kMcpRetainedJobs finished ones stay readable
// through jobs.list and jobs.wait.
inline constexpr std::size_t kMcpAsyncJobs = 16;
inline constexpr std::size_t kMcpRetainedJobs = 64;
// #747 (D1): an idempotencyKey (1..128 printable ASCII bytes) is remembered for this long, at most
// this many at once (the oldest finished one goes first), with an answer of at most this many
// bytes (a larger one is kept in its compact form).
inline constexpr std::chrono::minutes kMcpIdempotencyWindow{10};
inline constexpr std::size_t kMcpIdempotencyKeys = 1024;
inline constexpr std::size_t kMcpIdempotencyKeyBytes = 128;
inline constexpr std::size_t kMcpRememberedAnswerBytes = std::size_t{64} * 1024U;
// #747 (D1): a busy answer's retryAfterMs is the recent commit time (this default before any) times
// the work queued ahead plus one, between the floor and kMcpCommitDeadline.
inline constexpr std::chrono::milliseconds kMcpDefaultCommitTime{250};
inline constexpr std::chrono::milliseconds kMcpRetryAfterFloor{100};

// #745: the most created ids a transact lists in full. A transaction that creates more is
// answered in the summary form (counts by name and one digest), never refused after it applied.
inline constexpr std::size_t kMcpTransactOutputLimit = 10'000;
// The events tool keeps the created ids of a command event that has at most this many (a larger
// one keeps their count), and one events answer lists at most kMcpEventsOutputBudget ids (it then
// stops early and its `sequence` continues from the last event listed).
inline constexpr std::size_t kMcpEventOutputLimit = 1'024;
inline constexpr std::size_t kMcpEventsOutputBudget = 8'192;

// #747 (D2): events pages and the change subscription.
// - One events answer lists at most this many events (its `limit`, also the default); it then
//   stops early and its `sequence` continues from the last event listed.
inline constexpr std::size_t kMcpEventsPageLimit = 1'024;
// - A subscription coalesces what changed into one notification per interval: at least the
//   minimum apart, the default when events.subscribe gives none.
inline constexpr std::chrono::milliseconds kMcpEventIntervalDefault{250};
inline constexpr std::chrono::milliseconds kMcpEventIntervalMinimum{50};
inline constexpr std::chrono::milliseconds kMcpEventIntervalMaximum{60'000};
// - One notification (and one events summary) lists at most this many compositions; the rest are
//   counted in compositionsOmitted.
inline constexpr std::size_t kMcpEventCompositions = 64;
// - At most this many asynchronous transact jobs are followed for a subscription (the unfinished
//   ones and the newest finished ones); one notification lists each changed one once.
inline constexpr std::size_t kMcpEventJobs = kMcpAsyncJobs + kMcpRetainedJobs;
// - A session has one open stream; a newer one (a reconnect) ends the older. A quiet stream sends
//   an SSE comment this often so a dead connection is noticed.
inline constexpr std::size_t kMcpEventStreamsPerClient = 1;
inline constexpr std::chrono::milliseconds kMcpEventKeepAlive{15'000};

// A data file passed as {"fromFile": ...}: at most this many per call, each at most this many
// bytes, values and levels deep.
inline constexpr std::size_t kMcpDataFilesPerCall = 16;
inline constexpr std::size_t kMcpDataFileBytes = std::size_t{16} * 1024U * 1024U;
inline constexpr std::size_t kMcpDataFileValues = 2'000'000;
inline constexpr std::size_t kMcpDataFileDepth = 16;

// Inline capture images: PNG bytes above this are linked, not embedded, so the base64 image plus
// the JSON envelope stays within responseBytes.
inline constexpr std::size_t kMcpInlineImageBytes = std::size_t{3} * 1024U * 1024U - 64U * 1024U;

static_assert(kMcpInlineImageBytes / 3U * 4U + 64U * 1024U <= kMcpMessageLimits.responseBytes);

} // namespace bloom::mcp
