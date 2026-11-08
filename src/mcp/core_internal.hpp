#pragma once

// Private to src/mcp: the shared server core's state and the tool handlers both hosts use
// (ADR 0034, section 2). Moved from apps/bloom-mcp/server.hpp.

#include "json.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/session.hpp>

#include <deque>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace bloom::mcp {

// A tool failure with a stable code: structuredContent carries
// {"succeeded": false, "diagnostic": {"code": "bloom.mcp.<...>", "message": ...}}. Uncoded
// runtime failures keep the stdio server's existing {"diagnostic": "<message>"} shape.
struct ToolFailure final : std::runtime_error {
    ToolFailure(const McpToolErrorCode errorCode, const std::string& message,
                std::string detailCode = {})
        : std::runtime_error(message), code(errorCode), detail(std::move(detailCode)) {}
    McpToolErrorCode code;
    // The refusing collaborator's own code, e.g. the export queue's "PreflightBlocked";
    // published as diagnostic.detail when present.
    std::string detail;
    // A failed transient render, export or capture: its job id and the run's log (bounded),
    // published as structuredContent.job and structuredContent.log.
    std::optional<std::uint64_t> job;
    std::shared_ptr<const std::vector<host::ExportLogEntry>> log;
};

// What one tools/call works against.
struct McpCall final {
    scripting::Session& session;
    // Contextual-argument defaults (the live editor context, or the headless session defaults).
    scripting::OperationContext context;
    // The live application's state at the start of the call; null for the stdio host.
    const scripting::LiveSessionState* live = nullptr;
    scripting::LiveSessionLease* lease = nullptr;
    // Live generation (0 for stdio); paging cursors bind it.
    std::uint64_t generation = 0;
    const McpRequestControl& control;
    // MCP content blocks placed before the text copy of structuredContent (capture's image or
    // resource_link).
    std::vector<yyjson_mut_val*> content;
    // Activity facts: operation ids and label of a transact (never argument values).
    std::string summary;
    // A tool diagnostic that did not make the call an error (capture linked instead of inline).
    std::optional<McpToolErrorCode> notice;
    // transact: the compositions the edit left unable to compile (copied to McpHandled).
    std::vector<McpCompileFailureFact> compileFailures;
    // The final-render provider this call renders through on the headless route: the stdio
    // server's own provider, or for a projectFile call the file-session provider. Null renders on
    // the CPU reference path, and the result then says so.
    std::shared_ptr<host::GpuExportProvider> gpuProvider;
    // True for a projectFile call: the result reports which route rendered it.
    bool fileSession = false;
};

// A non-command fact the events tool reports beside command events: a project lifecycle step
// (project.saved, project.opened, project.created, project.closed) or a composition that no longer
// compiles after an edit (composition.compile-failed).
struct McpNotice final {
    std::string kind;
    std::string message;
    std::uint64_t revision = 0;
    std::optional<std::uint64_t> composition;
    std::optional<std::uint64_t> node;
    std::optional<std::uint64_t> parameter;
    std::string path;
    std::string projectName;
};
using McpEventEntry = std::variant<commands::CommandEvent, McpNotice>;

struct McpProtocolSession::State {
    mutable std::mutex mutex;
    bool initialized = false;
    bool ready = false;
    std::optional<McpClientInfo> client;
};

struct McpServerCore::State {
    explicit State(McpCoreDependencies value);
    ~State();
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    McpCoreDependencies dependencies;
    // Paging cursors bind this per-process value, so a cursor never crosses servers.
    std::uint64_t cursorToken = 0;

    // Command events for the `events` tool (the newest retainedCommandEvents).
    std::mutex subscriptionMutex;
    scripting::Subscription subscription;
    std::optional<std::uint64_t> eventGeneration;
    mutable std::mutex eventsMutex;
    std::deque<std::pair<std::uint64_t, McpEventEntry>> events;
    std::uint64_t sequence = 0;
    // The sequence when the live project was last replaced; an older cursor must reset.
    std::uint64_t resetSequence = 0;
    bool replaced = false;

    // stdio only: the server-lifetime GPU final-render provider (McpCoreDependencies).
    std::shared_ptr<host::GpuExportProvider> gpuExportProvider;
    // Used ONLY for renders of projectFile sessions (McpCoreDependencies::fileSessionGpuProvider,
    // else the stdio provider). Never read by the open project's own routes.
    std::shared_ptr<host::GpuExportProvider> fileGpuProvider;

    std::mutex captureMutex;
    std::uint64_t captureCounter = 0;

    // Project files other than the open project (the `projectFile` argument), each in its own
    // headless session kept between calls so edits, undo history and unsaved changes persist
    // until project.save and project.close. One call at a time holds fileMutex.
    struct FileSession final {
        std::unique_ptr<scripting::Session> session;
        document::Revision savedRevision;
    };
    std::timed_mutex fileMutex;
    std::map<std::string, FileSession> fileSessions;

    // The newest kRetiredTransientJobs transient render/export/capture jobs, oldest first: each
    // job's final view and whole run log, kept after the call returned and the queue retired the
    // job, so export.status, export.log and export.wait still read it. Each belongs to the export
    // queue generation that ran it; a replaced project (a new generation) releases them.
    struct RetiredTransient final {
        std::uint64_t generation = 0;
        host::ExportJobView view;
        host::ExportLogBatch log;
    };
    std::mutex retiredMutex;
    std::deque<RetiredTransient> retired;
    void retire(RetiredTransient job);
    // The retired jobs of `generation` (oldest first); releases those of any other generation.
    [[nodiscard]] std::vector<RetiredTransient> retiredJobs(std::uint64_t generation);

    [[nodiscard]] scripting::LiveSessionHost* liveHost() const noexcept;
    // Follows the live session's current generation's command events (no-op for stdio).
    void syncEvents();
    void record(const commands::CommandEvent& event);
    void notice(McpNotice notice);
};

// ---- Project files other than the open project (tools_file_session.cpp)
inline constexpr std::size_t kMcpFileSessions = 4;
// The `projectFile` argument: an absolute path to an existing project file, normalised, or nullopt
// when absent.
[[nodiscard]] std::optional<std::filesystem::path> projectFileArgument(yyjson_val* arguments);
// `arguments` without projectFile, for the tools that do not know the member.
[[nodiscard]] std::string withoutProjectFile(yyjson_val* arguments);
// The headless session for `path`, opened on first use. The caller holds State::fileMutex. Refuses
// a file that is open in the application or as the server's own project, and more than
// kMcpFileSessions files at once. The open-in-Bloom check runs on every call (`recheck`; project.close
// skips it), so a
// file the artist opened after the session did is refused, and it fails closed when the
// application cannot be asked in time.
[[nodiscard]] scripting::Session& fileSessionFor(McpServerCore::State& core,
                                                 const std::filesystem::path& path,
                                                 const McpRequestControl& control,
                                                 bool recheck = true);
// True when a projectFile session holds `path` (project.open and project.save-as refuse it).
// Waits at most two seconds for a file call in progress and answers true then (fail closed).
[[nodiscard]] bool fileSessionHolds(McpServerCore::State& core, const std::filesystem::path& path);
// project.save and project.close on such a file.
[[nodiscard]] yyjson_mut_val* fileProjectTool(McpServerCore::State& core, Json& out, McpTool tool,
                                              const std::filesystem::path& path,
                                              yyjson_val* arguments,
                                              const McpRequestControl& control);

// ---- Projections (projections.cpp, tools_document.cpp)
[[nodiscard]] yyjson_mut_val* commandResult(Json& out, const commands::CommandResult& result);
[[nodiscard]] yyjson_mut_val* parameterRecord(Json& out, const document::ParameterRecord& value);
[[nodiscard]] yyjson_mut_val* parameterSummary(Json& out, const document::ParameterRecord& value);
// query kind=enums: the interpolation, ease and Easy Ease vocabularies with their stable ids.
void appendAnimationEnums(Json& out, yyjson_mut_val* records);
// An events-tool record for a notice: {kind, revision, message, composition?, subject?, path?,
// projectName?}.
[[nodiscard]] yyjson_mut_val* noticeValue(Json& out, const McpNotice& notice);
[[nodiscard]] std::string digestFile(const std::filesystem::path& path);
// Standard padded base64 (RFC 4648 section 4), for MCP image content.
[[nodiscard]] std::string base64(std::span<const std::byte> bytes);

// ---- Transient export queue jobs (tools_export.cpp)
// At most this many of a transient run's newest log entries are returned with its result.
inline constexpr std::size_t kTransientLogEntries = 64;
// At most this many finished transient jobs stay readable after their call returned.
inline constexpr std::size_t kRetiredTransientJobs = 32;

struct TransientResult final {
    std::uint64_t job = 0;
    std::vector<host::ExportLogEntry> log;
    std::uint64_t frames = 0;
    std::vector<std::filesystem::path> files;
    std::string report;
};
// Submits a transient job on the export queue's owner thread, waits on the request worker
// (reporting progress), stops the job when the request is cancelled, and acknowledges the result.
// Throws ToolFailure on refusal, failure or cancellation.
// export.log's entry rows; render, export and capture results reuse them.
[[nodiscard]] yyjson_mut_val* logEntries(Json& out,
                                         const std::vector<host::ExportLogEntry>& entries);
[[nodiscard]] TransientResult runTransientJob(McpServerCore::State& core, McpCall& call,
                                              host::ExportTransientJobRequest request);
// `path` as an export destination: absolute paths become external file URIs, relative paths stay
// project-relative. `overwrite` is the tool's optional "ask"|"replace"|"refuse" argument; absent
// means ask (nothing is replaced silently).
[[nodiscard]] document::ExportDestination transientDestination(const std::string& path,
                                                               yyjson_val* overwrite);

// ---- Tools
[[nodiscard]] yyjson_mut_val* queryTool(McpServerCore::State& core, Json& out, McpCall& call,
                                        yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* transactTool(McpServerCore::State& core, Json& out, McpCall& call,
                                           yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* historyTool(McpServerCore::State& core, Json& out, McpCall& call,
                                          yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* eventsTool(McpServerCore::State& core, Json& out, McpCall& call,
                                         yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* renderTool(McpServerCore::State& core, Json& out, McpCall& call,
                                         yyjson_val* arguments, bool compositionExport);
[[nodiscard]] yyjson_mut_val* captureTool(McpServerCore::State& core, Json& out, McpCall& call,
                                          yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* exportSessionTool(McpServerCore::State& core, Json& out,
                                                McpCall& call, McpTool tool, yyjson_val* arguments);
// project.*, editor.reveal, selection.set, composition.activate, transport, viewer.set and
// export.reveal (tools_session.cpp). They hold no session lease.
[[nodiscard]] yyjson_mut_val* sessionTool(McpServerCore::State& core, Json& out, McpTool tool,
                                          yyjson_val* arguments, const McpRequestControl& control,
                                          std::string& summary);
// An integer is a whole frame of the composition at its frame rate; [numerator, denominator] is
// exact seconds.
[[nodiscard]] core::RationalTime frameOrSecondsArgument(yyjson_val* value,
                                                        const document::Snapshot& snapshot,
                                                        std::uint64_t composition);
[[nodiscard]] yyjson_mut_val* colorSettingsTool(McpServerCore::State& core, Json& out,
                                                McpCall& call, yyjson_val* arguments);
// profiler.snapshot (tools_profiler.cpp): the ProfilerSnapshot as JSON with schemaVersion.
[[nodiscard]] yyjson_mut_val* profilerSnapshotTool(McpServerCore::State& core, Json& out,
                                                   const McpRequestControl& control);
[[nodiscard]] yyjson_mut_val* toolList(Json& out);

// ---- Reference and read-back (tools_reference.cpp, tools_readback.cpp)
// tools/list with every description and property description taken from the reference.
[[nodiscard]] yyjson_mut_val* describedToolList(Json& out, yyjson_val* schemas);
[[nodiscard]] std::string_view serverInstructions() noexcept;
[[nodiscard]] yyjson_mut_val* helpTool(McpServerCore::State& core, Json& out, McpCall& call,
                                       yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* resourceList(Json& out);
[[nodiscard]] yyjson_mut_val* resourceRead(Json& out, yyjson_val* params);
// The query kinds owned by tools_readback.cpp: layers, keyframes, node-types.
[[nodiscard]] bool isReadbackQuery(std::string_view kind) noexcept;
[[nodiscard]] yyjson_mut_val* readbackQuery(Json& out, McpCall& call, yyjson_val* arguments,
                                            std::string_view kind, std::uint64_t sessionToken);
[[nodiscard]] yyjson_mut_val* compositionRecord(Json& out, const document::Composition& value);
[[nodiscard]] yyjson_mut_val* assetRecord(Json& out, const document::AssetRecord& value);
[[nodiscard]] yyjson_mut_val* assetFolders(Json& out, const document::Project& project);
// query nodes: the records with ports, links, positions and groups, plus `links` and `groups`.
// With `layer`, only that layer's nodes: its Layer Output node (or the source of its Merge entry)
// and everything upstream of it; `links` and `groups` are narrowed to those nodes.
void appendNodeGraph(Json& out, yyjson_mut_val* result, yyjson_mut_val* records,
                     const document::Composition& composition,
                     std::optional<document::LayerId> layer = std::nullopt);
// The `layer` argument of a nodes or layers query: a layer of `composition`, or the refusal
// listing where layers are found.
[[nodiscard]] std::optional<document::LayerId> layerFilter(const document::Composition& composition,
                                                           yyjson_val* arguments);
// The `fields` argument: keeps only the named members of every record in result.records. A name no
// record has is refused, listing the members the first record carries.
void projectRecords(Json& out, yyjson_mut_val* result, yyjson_val* arguments);
// transact's refusals, naming what the operation takes and where its reference is.
[[nodiscard]] std::string unknownOperation(std::string_view id);
// ". help {"name": <operation>} shows every argument.", appended to an argument refusal.
[[nodiscard]] std::string argumentHelp(std::string_view operation);
[[nodiscard]] yyjson_mut_val* parameterMetadata(Json& out, const document::Composition& composition,
                                                const document::ParameterRecord& parameter);

} // namespace bloom::mcp
