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

#include <bloom/core/worker_thread.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
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
    // #745: the bound the call met, published as diagnostic.limit {name, value, actual, unit}.
    struct Limit final {
        McpLimit limit = McpLimit::CommitQueue;
        std::uint64_t value = 0;
        std::uint64_t actual = 0;
    };
    std::optional<Limit> limit;
    // #747: a busy refusal's advice: when to try again (diagnostic.retryAfterMs).
    std::optional<std::uint64_t> retryAfterMs;
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
    // #745: set by a tool whose call already changed the project (transact): its result in a
    // compact form that always fits the response bound, used when the full one does not.
    std::function<yyjson_mut_val*(Json&)> compact;
    // #747: the calling protocol session (McpProtocolSession::id) and the request itself (its
    // parsed message and progress token), for transact async:true.
    std::uint64_t client = 0;
    const McpEnvelope* envelope = nullptr;
    // #747: set by transact once its transaction reached the command stack (applied, refused by
    // the document or stale): only such an answer is remembered for an idempotencyKey.
    bool committed = false;
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
// #745: a command event as the events tool keeps it. Its created ids are copied only when there
// are at most kMcpEventOutputLimit of them; a larger event keeps the count, so recording one on
// the UI thread never copies hundreds of thousands of ids.
struct McpRetainedCommand final {
    commands::CommandEventKind kind = commands::CommandEventKind::Rejected;
    commands::CommandResult result;
    std::size_t outputCount = 0;
    // #747 (D2): the compositions the step changed (its evaluation-input footprint, else the one
    // composition it was confined to); empty when it names none, which a composition filter
    // treats as reaching every composition (project-wide).
    std::vector<std::uint64_t> compositions;
};
using McpEventEntry = std::variant<McpRetainedCommand, McpNotice>;

// #747 (D2): which events an events call or a subscription selects.
struct McpEventFilter final {
    // Empty selects every kind; otherwise names from kMcpEventKinds.
    std::vector<std::string> kinds;
    // Empty selects every composition. Otherwise an event is selected when it names one of these
    // or names none (a project-wide change, which may reach any composition).
    std::vector<std::uint64_t> compositions;
    [[nodiscard]] bool selectsKind(std::string_view kind) const;
    [[nodiscard]] bool selectsCompositions(std::span<const std::uint64_t> named) const;
};
// Every kind an event or a notification can carry: the command events, the notices and `job`
// (an asynchronous transact job's progress and outcome, subscriptions only).
inline constexpr std::array<std::string_view, 9> kMcpEventKinds{
    "revision",          "history",        "rejected",        "composition.compile-failed",
    "project.saved",     "project.opened", "project.created", "project.closed",
    "project.copy-saved"};
inline constexpr std::string_view kMcpJobEventKind = "job";
// The `kinds` and `compositions` members of an events or events.subscribe call (absent: all).
[[nodiscard]] McpEventFilter eventFilter(yyjson_val* arguments);

// #747 (D2): one protocol session's events subscription (events.subscribe). Created by the call,
// shared with the stream that delivers it; changed in place by a later events.subscribe.
struct McpSubscription final {
    std::uint64_t id = 0;
    // The protocol session (McpProtocolSession::id) it belongs to: its own jobs are `mine`.
    std::uint64_t client = 0;
    // Guarded by McpServerCore::State::eventsMutex.
    McpEventFilter filter;
    std::chrono::milliseconds interval = kMcpEventIntervalDefault;
    // The last event sequence and job change delivered (or skipped as not selected).
    std::uint64_t cursor = 0;
    std::uint64_t jobCursor = 0;
    std::chrono::steady_clock::time_point lastSent{};
    // The newest stream's number; an older stream ends.
    std::uint64_t streamEpoch = 0;
    // events.unsubscribe: every stream ends.
    bool ended = false;
};

// #747 (D2): the newest step of an asynchronous transact job, as a subscription reports it.
struct McpJobLine final {
    std::uint64_t version = 0;
    std::uint64_t client = 0;
    McpJobState state = McpJobState::Queued;
    std::string phase;
    McpProgress progress;
    std::optional<std::uint64_t> revision;
    std::string code;
    bool finished = false;
    bool cancellationRequested = false;
};

struct McpProtocolSession::State {
    // Process-unique, from 1 (#746: an agent session names the protocol session that owns it).
    std::uint64_t id = 0;
    mutable std::mutex mutex;
    bool initialized = false;
    bool ready = false;
    std::optional<McpClientInfo> client;
    // #747 (D2): its events subscription, when it has one.
    std::shared_ptr<McpSubscription> subscription;
};

// ---- #747 (D1): asynchronous transact jobs and idempotency keys (transact_jobs.cpp)

// An answer kept to be given again: a finished job's transact result, or the result an
// idempotencyKey remembers. `body` is the structuredContent.
struct McpStoredAnswer final {
    std::shared_ptr<const Parsed> body;
    bool failed = false;
};

struct McpTransactJob final {
    std::uint64_t number = 0;
    std::string id;
    std::uint64_t client = 0;
    // The live generation the job was submitted on; it never applies to another project.
    std::uint64_t generation = 0;
    std::string label;
    std::size_t operations = 0;
    std::optional<std::string> progressToken;
    std::optional<std::string> idempotencyKey;
    // Owns `arguments` (the request's own parsed message).
    std::shared_ptr<const Parsed> message;
    yyjson_val* arguments = nullptr;
    std::stop_source stop;
    std::chrono::steady_clock::time_point submitted;
    // Guarded by McpTransactJobs::mutex.
    McpJobState state = McpJobState::Queued;
    std::string phase = "queued";
    McpProgress progress;
    bool cancellationRequested = false;
    std::optional<std::uint64_t> revision;
    std::string code;
    std::shared_ptr<const McpStoredAnswer> result;
    // Advances with every change, so a waiter forwards each one once.
    std::uint64_t serial = 0;
};

struct McpTransactJobs final {
    std::mutex mutex;
    std::condition_variable_any changed;
    // Waiting for the runner, oldest first.
    std::deque<std::shared_ptr<McpTransactJob>> queue;
    // Every job still readable, oldest first: the unfinished ones and the newest kMcpRetainedJobs
    // finished ones.
    std::deque<std::shared_ptr<McpTransactJob>> jobs;
    std::uint64_t next = 1;
    bool stopping = false;
    // Started with the first job; one runner, since commits publish one at a time anyway.
    std::optional<core::WorkerThread> runner;
};

struct McpIdempotency final {
    struct Entry final {
        std::string fingerprint;
        std::chrono::steady_clock::time_point at;
        // Distinguishes this claim from a later one on the same key.
        std::uint64_t ticket = 0;
        // Still running (a synchronous transact), or the job of an asynchronous one.
        bool pending = true;
        std::shared_ptr<const McpStoredAnswer> answer;
        std::shared_ptr<McpTransactJob> job;
    };
    std::mutex mutex;
    std::condition_variable_any changed;
    std::map<std::string, Entry> entries;
    std::uint64_t nextTicket = 1;
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
    // #747 (D2): woken (with eventsMutex) whenever an event, a notice, a job step or a
    // subscription change arrives, so an event stream builds its next notification.
    std::condition_variable_any eventsChanged;
    // The newest step of each followed transact job (at most kMcpEventJobs; the oldest finished
    // goes first), and a counter that advances with every step.
    std::map<std::string, McpJobLine> jobLines;
    std::uint64_t jobVersion = 0;
    std::uint64_t nextSubscription = 1;
    // The core is going away: every stream ends.
    bool closing = false;

    // stdio only: the server-lifetime GPU final-render provider (McpCoreDependencies).
    std::shared_ptr<host::GpuExportProvider> gpuExportProvider;
    // Used ONLY for renders of projectFile sessions (McpCoreDependencies::fileSessionGpuProvider,
    // else the stdio provider). Never read by the open project's own routes.
    std::shared_ptr<host::GpuExportProvider> fileGpuProvider;

    std::mutex captureMutex;
    std::uint64_t captureCounter = 0;

    // The route the newest render, export or capture job took, read by query kind render-route
    // (the Viewer's own frame records nothing about a job that ran apart from it).
    struct JobRoute final {
        std::uint64_t composition = 0;
        std::uint64_t job = 0;
        std::uint64_t gpuFrames = 0;
        std::uint64_t cpuReferenceFrames = 0;
        // The first CPU-reference reason the job's log recorded; empty when it recorded none.
        std::string why;
    };
    std::mutex jobRouteMutex;
    std::optional<JobRoute> lastJobRoute;

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

    // #747 (D1): asynchronous transact jobs, idempotency keys and the stdio server's own measure
    // of recent commit time (the live host measures its own).
    McpTransactJobs jobs;
    McpIdempotency idempotency;
    mutable std::mutex loadMutex;
    std::optional<double> headlessCommitMs;
    void recordHeadlessCommit(std::chrono::steady_clock::duration taken);
    [[nodiscard]] std::uint64_t retryAfterMs(std::size_t queuedAhead) const;
    // Publishes a job step to McpCoreDependencies::jobEvents and to event subscriptions (called
    // without jobs.mutex held).
    void publishJob(const McpJobEvent& event);
    // Fails the queued jobs of a generation other than `generation` (the project was replaced).
    void revokeJobs(std::uint64_t generation);
    // Stops the job runner and cancels every unfinished job (the core is going away).
    void stopJobs();

    [[nodiscard]] scripting::LiveSessionHost* liveHost() const noexcept;
    // Follows the live session's current generation's command events (no-op for stdio).
    void syncEvents();
    // Called with subscriptionMutex held: follows `generation`'s command events from its start.
    void followGeneration(scripting::LiveSessionHost& live, std::uint64_t generation);
    void record(const commands::CommandEvent& event);
    void notice(McpNotice notice);
};

// ---- Project files other than the open project (tools_file_session.cpp)
inline constexpr std::size_t kMcpFileSessions = 4;
// The `projectFile` argument: an absolute path to an existing project file, normalised, or nullopt
// when absent.
[[nodiscard]] std::optional<std::filesystem::path> projectFileArgument(yyjson_val* arguments);
// `arguments` without projectFile, for the tools that do not know the member.
using FrozenJson = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
[[nodiscard]] FrozenJson withoutProjectFile(yyjson_val* arguments);
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
// Issue #737/#748: the answer to a save that ran and did not write the file: bloom.mcp.save-failed,
// `message` as its message and {stage, cause} as diagnostic.detail.
[[nodiscard]] yyjson_mut_val* saveFailedAnswer(Json& out, const std::string& message,
                                               const std::string& stage, const std::string& cause);

[[nodiscard]] yyjson_mut_val* fileProjectTool(McpServerCore::State& core, Json& out, McpTool tool,
                                              const std::filesystem::path& path,
                                              yyjson_val* arguments,
                                              const McpRequestControl& control);

// ---- Projections (projections.cpp, tools_document.cpp)
// `withOutputs` false leaves out the `outputs` list (transact's "summary" response replaces it, so
// a transaction that creates hundreds of thousands of ids never builds them as JSON, #744).
// #745: a result lists at most this many diagnostics; the rest are counted in diagnosticsOmitted.
inline constexpr std::size_t kResultDiagnosticLimit = 256;
[[nodiscard]] yyjson_mut_val* commandResult(Json& out, const commands::CommandResult& result,
                                            bool withOutputs = true,
                                            std::size_t diagnosticLimit = kResultDiagnosticLimit);
[[nodiscard]] yyjson_mut_val* parameterRecord(Json& out, const document::ParameterRecord& value);
// ---- The node reference's values written straight into a response (reference_values.cpp, #745):
// each equals parsing the matching scripting/node_reference.hpp text (a number that is whole and
// below 1e15 is an integer; a non-finite one is null).
[[nodiscard]] yyjson_mut_val* referenceNumber(Json& out, double value);
// A number as its shortest decimal text (std::to_chars) reads back: digits alone are an integer
// when they fit 64 bits, anything else a real; non-finite is null.
[[nodiscard]] yyjson_mut_val* shortestNumber(Json& out, double value);
[[nodiscard]] yyjson_mut_val* referenceValue(Json& out, const document::ParameterValue& value);
[[nodiscard]] yyjson_mut_val* referenceTime(Json& out, core::RationalTime time,
                                            const document::FrameRate& rate);
[[nodiscard]] yyjson_mut_val* referenceMetadata(Json& out, std::string_view schemaKey,
                                                document::ParameterValueKind kind,
                                                const document::ParameterDefinition* definition,
                                                std::string_view role = {});
[[nodiscard]] yyjson_mut_val*
referenceNodeType(Json& out, const document::NodeDefinition& definition, bool full);
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

// Issue #581: the optional worker limits of export, export.start and export.start-all. An argument
// that is not given leaves the saved Preferences value in force for that call.
struct WorkerLimitArguments final {
    std::optional<std::uint64_t> memoryCapBytes;
    std::optional<std::uint32_t> threads;
    [[nodiscard]] bool any() const noexcept { return memoryCapBytes || threads; }
};
// Reads and validates workerMemoryCapBytes (0 = no cap, to the largest cap) and workerThreads (0 =
// automatic, to the largest thread count). Throws InvalidInput.
[[nodiscard]] WorkerLimitArguments workerLimitArguments(yyjson_val* arguments);
[[nodiscard]] host::ExportWorkerLimits applyWorkerLimits(host::ExportWorkerLimits base,
                                                         const WorkerLimitArguments& given);
// {measured, peakBytes, capBytes, waits, waitedMs}
[[nodiscard]] yyjson_mut_val* workerMemoryValue(Json& out, const host::ExportWorkerMemory& memory);

// Reads loudnessLufs (-40..-5) and truePeakDbtp (-12..0, default -1) of an audio-carrying export
// (#570). Absent when loudnessLufs is not given; truePeakDbtp alone is refused. Throws InvalidInput.
[[nodiscard]] std::optional<document::ExportLoudness> loudnessArguments(yyjson_val* arguments);
// The optional `encoder` argument of render and export: "automatic", "software" or "hardware".
[[nodiscard]] std::optional<document::ExportEncoderPreference>
encoderArgument(yyjson_val* arguments);
// {targetLufs, truePeakLimitDbtp, measured, measuredLufs, measuredTruePeakDbtp, gainDb, finalMeasured,
// finalLufs, finalTruePeakDbtp, limiterReductionDb}; a level that was not measurable (silence) is
// null.
[[nodiscard]] yyjson_mut_val* loudnessValue(Json& out, const host::ExportLoudnessReport& report);

struct TransientResult final {
    std::optional<host::ExportWorkerMemory> workerMemory;
    std::optional<host::ExportLoudnessReport> loudness;
    std::uint64_t job = 0;
    std::vector<host::ExportLogEntry> log;
    std::uint64_t frames = 0;
    std::vector<std::filesystem::path> files;
    std::string report;
    // What the export queue recorded about the route the frames took (the outputs' device
    // provenance). `routeKnown` is false when no output reported any; the counts are then unused.
    bool routeKnown = false;
    std::uint64_t gpuFrames = 0;
    std::uint64_t cpuReferenceFrames = 0;
};
// Submits a transient job on the export queue's owner thread, waits on the request worker
// (reporting progress), stops the job when the request is cancelled, and acknowledges the result.
// Throws ToolFailure on refusal, failure or cancellation.
// export.log's entry rows; render, export and capture results reuse them.
[[nodiscard]] yyjson_mut_val* logEntries(Json& out,
                                         const std::vector<host::ExportLogEntry>& entries);
[[nodiscard]] TransientResult runTransientJob(McpServerCore::State& core, McpCall& call,
                                              host::ExportTransientJobRequest request,
                                              const WorkerLimitArguments& limits = {});
// `path` as an export destination: absolute paths become external file URIs, relative paths stay
// project-relative. `overwrite` is the tool's optional "ask"|"replace"|"refuse" argument; absent
// means ask (nothing is replaced silently).
[[nodiscard]] document::ExportDestination transientDestination(const std::string& path,
                                                               yyjson_val* overwrite);

// ---- #747 (D1) tools and helpers (transact_jobs.cpp)
// A ToolFailure as structuredContent {succeeded:false, diagnostic:{code, message, limit?,
// retryAfterMs?}, job?, log?}.
[[nodiscard]] yyjson_mut_val* toolFailureValue(Json& out, const ToolFailure& failure);
// transact itself, synchronously, after its #747 arguments (async, idempotencyKey) were read.
[[nodiscard]] yyjson_mut_val* runTransact(McpServerCore::State& core, Json& out, McpCall& call,
                                          yyjson_val* arguments);
// transact async:true: queues a job and answers with its id at once.
// With `key`, the job is bound to that idempotency key's claim (`ticket`) before it is queued.
[[nodiscard]] yyjson_mut_val* submitTransactJob(McpServerCore::State& core, Json& out,
                                                McpCall& call, yyjson_val* arguments,
                                                const std::string* key = nullptr,
                                                std::uint64_t ticket = 0);
// jobs.wait for a transact job.
[[nodiscard]] yyjson_mut_val* jobsWaitTool(McpServerCore::State& core, Json& out,
                                           yyjson_val* arguments, const McpRequestControl& control);
// jobs.list's rows for the transact jobs, appended to `list`.
void appendTransactJobRows(McpServerCore::State& core, Json& out, yyjson_mut_val* list);
// jobs.cancel for a transact job id ("transact-<n>").
[[nodiscard]] yyjson_mut_val* cancelTransactJob(McpServerCore::State& core, Json& out,
                                                const std::string& job);
// The structuredContent of `answer`, copied into `out`.
[[nodiscard]] yyjson_mut_val* storedAnswerValue(Json& out, const McpStoredAnswer& answer);

// ---- Tools
[[nodiscard]] yyjson_mut_val* queryTool(McpServerCore::State& core, Json& out, McpCall& call,
                                        yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* transactTool(McpServerCore::State& core, Json& out, McpCall& call,
                                           yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* historyTool(McpServerCore::State& core, Json& out, McpCall& call,
                                          yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* eventsTool(McpServerCore::State& core, Json& out, McpCall& call,
                                         yyjson_val* arguments);
// #747 (D2): events.subscribe and events.unsubscribe for the calling protocol session (no lease).
[[nodiscard]] yyjson_mut_val* eventsSubscribeTool(McpServerCore::State& core, Json& out,
                                                  McpProtocolSession::State& session,
                                                  yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* eventsUnsubscribeTool(McpServerCore::State& core, Json& out,
                                                    McpProtocolSession::State& session,
                                                    yyjson_val* arguments);
// #747 (D2): what changed after `after`, coalesced: written into `into` as {fromSequence,
// toSequence, resetRequired, reason?, count, fromRevision?, toRevision?, kinds, origins,
// compositions, compositionsOmitted, project?}. Called with core.eventsMutex held. Moves `after`
// to the newest event (or to the replacement's start when the project was replaced) and returns
// the number of events `filter` selected; `resetRequired` is set when events after `after` are
// gone (reason "overflow" or "project-replaced"; "cursor" for a cursor from the future).
struct McpEventSummary final {
    std::size_t count = 0;
    bool resetRequired = false;
};
[[nodiscard]] McpEventSummary summarizeEvents(const McpServerCore::State& core, Json& out,
                                              yyjson_mut_val* into, const McpEventFilter& filter,
                                              std::uint64_t& after, std::uint64_t sinceRevision);
[[nodiscard]] yyjson_mut_val* renderTool(McpServerCore::State& core, Json& out, McpCall& call,
                                         yyjson_val* arguments, bool compositionExport);
[[nodiscard]] yyjson_mut_val* captureTool(McpServerCore::State& core, Json& out, McpCall& call,
                                          yyjson_val* arguments);
[[nodiscard]] yyjson_mut_val* exportSessionTool(McpServerCore::State& core, Json& out,
                                                McpCall& call, McpTool tool, yyjson_val* arguments);
// project.*, editor.reveal, selection.set, composition.activate, transport, viewer.set and
// export.reveal (tools_session.cpp). They hold no session lease.
// `client` is the calling protocol session's id (session.begin and session.end, #746).
[[nodiscard]] yyjson_mut_val* sessionTool(McpServerCore::State& core, Json& out, McpTool tool,
                                          yyjson_val* arguments, const McpRequestControl& control,
                                          std::string& summary, std::uint64_t client);
// The read-back queries text-metrics, layer-bounds, history, viewer and preview-cache
// (tools_inspection.cpp).
[[nodiscard]] bool isInspectionQuery(std::string_view kind) noexcept;
[[nodiscard]] yyjson_mut_val* inspectionQuery(McpServerCore::State& core, Json& out, McpCall& call,
                                              yyjson_val* arguments, std::string_view kind);
// query kind=pixels (tools_render.cpp): pixel values of a frame read back from the production
// still route.
[[nodiscard]] yyjson_mut_val* pixelsQuery(McpServerCore::State& core, Json& out, McpCall& call,
                                          yyjson_val* arguments);
// query kind=scopes (tools_scopes.cpp): the Scopes panel's count tables, a summary of them or a
// picture of a scope, for one frame or a range.
[[nodiscard]] yyjson_mut_val* scopesQuery(McpServerCore::State& core, Json& out, McpCall& call,
                                          yyjson_val* arguments);
// Where a scope picture is written, and its pruning: the capture folder, shared with capture.
[[nodiscard]] std::filesystem::path nextScopePicturePath(McpServerCore::State& core);
void pruneScopePictures(const std::filesystem::path& folder);
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
                     std::optional<document::LayerId> layer = std::nullopt,
                     const document::Project* project = nullptr);
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
