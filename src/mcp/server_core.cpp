// The shared MCP server core (ADR 0034, section 2): JSON-RPC 2.0 framing and validation, the MCP
// handshake and tools/call dispatch for both hosts. Moved from apps/bloom-mcp/server.cpp.
#include "core_internal.hpp"

#include <bloom/platform/owner_only_secret.hpp>
#include <bloom/project/project_capacity.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>

namespace bloom::mcp {
namespace {

// clang-format off
constexpr std::array<McpTool, kMcpToolCount> kTools{
    McpTool::Query,
    McpTool::Transact,
    McpTool::History,
    McpTool::Events,
    McpTool::Render,
    McpTool::Export,
    McpTool::Capture,
    McpTool::ExportStart,
    McpTool::ExportStartAll,
    McpTool::ExportPause,
    McpTool::ExportResume,
    McpTool::ExportStop,
    McpTool::ExportStopAll,
    McpTool::ExportRetry,
    McpTool::ExportConfirmReplace,
    McpTool::ExportStatus,
    McpTool::ExportEvents,
    McpTool::ExportLog,
    McpTool::ExportWait,
    McpTool::ExportPresets,
    McpTool::ExportScript,
    McpTool::ProjectSave,
    McpTool::ProjectSaveAs,
    McpTool::ProjectOpen,
    McpTool::ProjectNew,
    McpTool::ProjectClose,
    McpTool::EditorReveal,
    McpTool::SelectionSet,
    McpTool::CompositionActivate,
    McpTool::Transport,
    McpTool::ViewerSet,
    McpTool::ExportReveal,
    McpTool::ProjectSetColorSettings,
    McpTool::ProfilerSnapshot,
    McpTool::ProjectSaveCopy,
    McpTool::ProjectCancelOpen,
    McpTool::PreferencesGet,
    McpTool::PreferencesSet,
    McpTool::CachePurge,
    McpTool::JobsList,
    McpTool::JobsCancel,
    McpTool::AssetReveal,
    McpTool::CompositionPresets,
    McpTool::CompositionSavePreset,
    McpTool::CompositionRenamePreset,
    McpTool::CompositionDeletePreset,
    McpTool::Help,
    McpTool::DialogAnswer,
    McpTool::ScriptRun,
    McpTool::SessionBegin,
    McpTool::SessionEnd,
    McpTool::JobsWait,
    McpTool::EventsSubscribe,
    McpTool::EventsUnsubscribe,
};
// clang-format on

std::optional<McpTool> toolFromName(const std::string_view name) {
    for (const auto tool : kTools) {
        if (toolName(tool) == name)
            return tool;
    }
    return std::nullopt;
}

std::string requestIdentity(yyjson_val* id) {
    if (yyjson_is_str(id))
        return "s:" + string(id, 128);
    if (yyjson_is_sint(id))
        return "n:" + std::to_string(yyjson_get_sint(id));
    if (yyjson_is_uint(id))
        return "n:" + std::to_string(yyjson_get_uint(id));
    throw InvalidInput("Invalid request ID");
}

// The JSON-RPC id of a canonical request identity, written without parsing it again.
yyjson_mut_val* requestId(Json& out, const std::optional<std::string_view> requestKey) {
    if (!requestKey || requestKey->size() < 2)
        return out.null();
    const auto text = requestKey->substr(2);
    if (requestKey->starts_with("s:"))
        return out.text(text);
    if (text.starts_with('-')) {
        std::int64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error == std::errc{} && end == text.data() + text.size())
            return out.signedNumber(value);
    } else {
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error == std::errc{} && end == text.data() + text.size())
            return out.number(value);
    }
    return out.null();
}

McpLimitRefusal refusalOf(const LimitExceeded& exceeded) {
    return {.limit = exceeded.limit,
            .value = exceeded.value,
            .actual = exceeded.actual,
            .message = exceeded.what()};
}

// Client identity strings shown in the panel: control characters removed, at most 256 bytes cut
// on a UTF-8 boundary.
std::string sanitised(yyjson_val* value) {
    if (!yyjson_is_str(value))
        return {};
    std::string text;
    const std::string_view raw(yyjson_get_str(value), yyjson_get_len(value));
    for (const char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x20U && byte != 0x7fU)
            text.push_back(c);
    }
    if (text.size() > 256) {
        auto end = std::size_t{256};
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
            --end;
        text.resize(end);
    }
    return text;
}

} // namespace

yyjson_mut_val* toolFailureValue(Json& out, const ToolFailure& failure) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(false));
    auto* diagnostic = out.object();
    out.set(diagnostic, "code", out.text(toolErrorCode(failure.code)));
    out.set(diagnostic, "message", out.text(failure.what()));
    if (!failure.detail.empty())
        out.set(diagnostic, "detail", out.text(failure.detail));
    if (failure.limit) {
        const auto description = describeLimit(failure.limit->limit);
        auto* limit = out.object();
        out.set(limit, "name", out.text(description.name));
        out.set(limit, "value", out.number(failure.limit->value));
        out.set(limit, "actual", out.number(failure.limit->actual));
        out.set(limit, "unit", out.text(description.unit));
        out.set(diagnostic, "limit", limit);
    }
    if (failure.retryAfterMs)
        out.set(diagnostic, "retryAfterMs", out.number(*failure.retryAfterMs));
    out.set(value, "diagnostic", diagnostic);
    if (failure.job)
        out.set(value, "job", out.text(std::to_string(*failure.job)));
    if (failure.log)
        out.set(value, "log", logEntries(out, *failure.log));
    return value;
}

namespace {

// #745, #741: every bound a request can meet, keyed by McpLimit's names. The commit bounds belong
// to the desktop application's live session; the stdio server has no commit queue.
yyjson_mut_val* limitsValue(Json& out, const McpServerCore::State& state) {
    const auto& limits = state.dependencies.limits;
    const auto& transport = state.dependencies.transport;
    const bool live = state.liveHost() != nullptr;
    auto* value = out.object();
    const auto put = [&](const McpLimit limit, const std::uint64_t amount) {
        out.set(value, describeLimit(limit).name, out.number(amount));
    };
    put(McpLimit::RequestBytes, limits.requestBytes);
    put(McpLimit::ResponseBytes, limits.responseBytes);
    put(McpLimit::JsonValues, limits.jsonValues);
    put(McpLimit::JsonDepth, limits.jsonDepth);
    put(McpLimit::JsonMemory, limits.arenaBytes);
    put(McpLimit::DataFiles, kMcpDataFilesPerCall);
    put(McpLimit::DataFileBytes, kMcpDataFileBytes);
    put(McpLimit::DataFileValues, kMcpDataFileValues);
    put(McpLimit::DataFileDepth, kMcpDataFileDepth);
    put(McpLimit::InFlightPerClient, transport.inFlightPerClient);
    put(McpLimit::PendingMessages, limits.pendingMessages);
    if (live) {
        put(McpLimit::CommitQueue, scripting::kLiveCommitQueueCapacity);
        put(McpLimit::CommitDeadline,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(kMcpCommitDeadline).count()));
        put(McpLimit::CommitWork, scripting::kLiveCommitWorkLimit);
    }
    put(McpLimit::TransactOutputs, kMcpTransactOutputLimit);
    // #748: what one project file can save and reopen; an edit past one is refused at the edit.
    constexpr project::ProjectCapacityLimits capacity;
    put(McpLimit::ProjectJsonValues, capacity.jsonValues);
    put(McpLimit::ProjectDocumentBytes, capacity.documentBytes);
    put(McpLimit::ProjectContainerEntries, capacity.containerEntries);
    put(McpLimit::ProjectStringBytes, capacity.stringBytes);
    // #747 (D1): asynchronous transact and idempotency keys (the jobs need the live session).
    if (live) {
        put(McpLimit::AsyncJobs, kMcpAsyncJobs);
        out.set(value, "retainedJobs", out.number(kMcpRetainedJobs));
    }
    out.set(value, "idempotencyKeys", out.number(kMcpIdempotencyKeys));
    out.set(value, "idempotencyKeyBytes", out.number(kMcpIdempotencyKeyBytes));
    out.set(value, "idempotencyWindowMs",
            out.number(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(kMcpIdempotencyWindow)
                    .count())));
    out.set(value, "idempotencyAnswerBytes", out.number(kMcpRememberedAnswerBytes));
    // Bounds that are not refused per request but shape what an agent can send.
    out.set(value, "requestWorkers", out.number(transport.requestWorkers));
    out.set(value, "clients", out.number(transport.clients));
    if (transport.connections != 0)
        out.set(value, "connections", out.number(transport.connections));
    out.set(value, "renderFrames", out.number(limits.renderFrames));
    out.set(value, "retainedEvents", out.number(limits.retainedCommandEvents));
    out.set(value, "eventOutputs", out.number(kMcpEventOutputLimit));
    out.set(value, "eventsOutputsPerAnswer", out.number(kMcpEventsOutputBudget));
    // #747 (D2): events pages and the change subscription. A subscriber further behind than
    // retainedEvents is told resetRequired (reason overflow); none is refused.
    const auto milliseconds = [](const std::chrono::milliseconds value) {
        return static_cast<std::uint64_t>(value.count());
    };
    out.set(value, "eventsPerAnswer", out.number(kMcpEventsPageLimit));
    out.set(value, "eventIntervalMinMs", out.number(milliseconds(kMcpEventIntervalMinimum)));
    out.set(value, "eventIntervalMaxMs", out.number(milliseconds(kMcpEventIntervalMaximum)));
    out.set(value, "eventCompositions", out.number(kMcpEventCompositions));
    out.set(value, "eventJobs", out.number(kMcpEventJobs));
    out.set(value, "eventStreamsPerClient", out.number(kMcpEventStreamsPerClient));
    out.set(value, "eventKeepAliveMs", out.number(milliseconds(kMcpEventKeepAlive)));
    return value;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Protocol sessions

McpProtocolSession::McpProtocolSession(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
McpProtocolSession::~McpProtocolSession() = default;

std::uint64_t McpProtocolSession::id() const noexcept { return state_->id; }

bool McpProtocolSession::initialized() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->initialized;
}

bool McpProtocolSession::ready() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->ready;
}

std::optional<McpClientInfo> McpProtocolSession::client() const {
    const std::lock_guard lock(state_->mutex);
    return state_->client;
}

// ---------------------------------------------------------------------------------------------
// Core state

McpServerCore::State::State(McpCoreDependencies value) : dependencies(std::move(value)) {
    std::array<std::byte, 8> random{};
    if (!platform::fillSecureRandom(random))
        throw std::runtime_error("The system random source is unavailable");
    for (const auto byte : random)
        cursorToken = (cursorToken << 8U) | std::to_integer<std::uint64_t>(byte);
    // Cursor text is decimal without leading zeros; keep the value non-zero.
    cursorToken |= 1U;
}

McpServerCore::State::~State() {
    // #747 (D2): every event stream ends (the transports have stopped theirs already).
    {
        const std::lock_guard lock(eventsMutex);
        closing = true;
    }
    eventsChanged.notify_all();
    // #747: the job runner ends before anything it reads.
    stopJobs();
    const std::lock_guard lock(subscriptionMutex);
    subscription.reset();
}

scripting::LiveSessionHost* McpServerCore::State::liveHost() const noexcept {
    const auto* live =
        std::get_if<std::reference_wrapper<scripting::LiveSessionHost>>(&dependencies.session);
    return live ? &live->get() : nullptr;
}

void McpServerCore::State::record(const commands::CommandEvent& event) {
    // Runs on the thread that published the command (the UI thread for the live host): the copy
    // leaves out a large event's ids (#745).
    const auto& source = event.result;
    McpRetainedCommand retained{
        .kind = event.kind, .result = {}, .outputCount = source.outputs.size(), .compositions = {}};
    auto& kept = retained.result;
    kept.action = source.action;
    kept.status = source.status;
    kept.origin = source.origin;
    kept.beforeRevision = source.beforeRevision;
    kept.afterRevision = source.afterRevision;
    kept.expectedRevision = source.expectedRevision;
    kept.label = source.label;
    kept.operationFailures = source.operationFailures;
    kept.capacityRefusal = source.capacityRefusal;
    kept.warnings = source.warnings;
    kept.validation = source.validation;
    kept.validationOperationIndex = source.validationOperationIndex;
    kept.renderAffecting = source.renderAffecting;
    kept.affectedComposition = source.affectedComposition;
    kept.historyEntriesDropped = source.historyEntriesDropped;
    kept.historyEntryLimitReached = source.historyEntryLimitReached;
    kept.historyMemoryLimitReached = source.historyMemoryLimitReached;
    kept.historyGrouped = source.historyGrouped;
    if (source.outputs.size() <= kMcpEventOutputLimit)
        kept.outputs = source.outputs;
    // #747 (D2): the compositions it changed, for a subscription's composition filter. A step
    // that reaches beyond named compositions names none (project-wide).
    if (source.evaluationChangedCompositions && !source.evaluationChangedCompositions->empty()) {
        if (source.evaluationChangedCompositions->size() <= kMcpEventsPageLimit) {
            retained.compositions.reserve(source.evaluationChangedCompositions->size());
            for (const auto composition : *source.evaluationChangedCompositions)
                retained.compositions.push_back(composition.value());
        }
    } else if (source.affectedComposition) {
        retained.compositions.push_back(source.affectedComposition->value());
    }
    {
        const std::lock_guard lock(eventsMutex);
        if (events.size() >= dependencies.limits.retainedCommandEvents)
            events.pop_front();
        events.emplace_back(++sequence, std::move(retained));
    }
    eventsChanged.notify_all();
}

void McpServerCore::State::notice(McpNotice value) {
    {
        const std::lock_guard lock(eventsMutex);
        if (events.size() >= dependencies.limits.retainedCommandEvents)
            events.pop_front();
        events.emplace_back(++sequence, std::move(value));
    }
    eventsChanged.notify_all();
}

void McpServerCore::State::syncEvents() {
    auto* live = liveHost();
    if (live == nullptr)
        return;
    std::optional<std::uint64_t> replacedBy;
    {
        const std::lock_guard lock(subscriptionMutex);
        const auto generation = live->generation();
        if (eventGeneration == generation)
            return;
        if (eventGeneration.has_value())
            replacedBy = generation;
        followGeneration(*live, generation);
    }
    // #747: asynchronous jobs queued for the replaced project end without applying.
    if (replacedBy)
        revokeJobs(*replacedBy);
}

void McpServerCore::State::followGeneration(scripting::LiveSessionHost& live,
                                            const std::uint64_t generation) {
    // Release the old subscription without holding eventsMutex: the stream holds its own lock
    // while it delivers, and delivery takes eventsMutex.
    subscription.reset();
    {
        const std::lock_guard guard(eventsMutex);
        if (eventGeneration.has_value()) {
            events.clear();
            // #747 (D2): the replacement takes a sequence of its own, so every cursor from the
            // old project (a caught-up one too) is told resetRequired.
            resetSequence = ++sequence;
            replaced = true;
        }
        eventGeneration = generation;
    }
    eventsChanged.notify_all();
    if (const auto stream = live.events())
        subscription =
            stream->subscribe([this](const commands::CommandEvent& event) { record(event); });
}

// ---------------------------------------------------------------------------------------------
// Core

McpServerCore::McpServerCore(McpCoreDependencies dependencies)
    : state_(std::make_unique<State>(std::move(dependencies))) {
    auto& state = *state_;
    state.gpuExportProvider = state.dependencies.gpuProvider;
    state.fileGpuProvider = state.dependencies.fileSessionGpuProvider
                                ? state.dependencies.fileSessionGpuProvider
                                : state.dependencies.gpuProvider;
    if (auto* headless =
            std::get_if<std::reference_wrapper<scripting::Session>>(&state.dependencies.session)) {
        scripting::Events events(headless->get());
        state.subscription = events.subscribe(
            [raw = &state](const commands::CommandEvent& event) { raw->record(event); });
    } else {
        state.syncEvents();
    }
}

McpServerCore::~McpServerCore() = default;

void McpServerCore::followLiveSession() { state_->syncEvents(); }

void McpServerCore::setExportWaitLimits(const std::chrono::milliseconds destinationCheckWait,
                                        const std::chrono::milliseconds encoderProbeWait) noexcept {
    state_->dependencies.limits.destinationCheckWait = destinationCheckWait;
    state_->dependencies.limits.encoderProbeWait = encoderProbeWait;
}

void McpServerCore::replaceHeadlessProject(const std::function<void()>& replace) {
    auto& state = *state_;
    auto* headless =
        std::get_if<std::reference_wrapper<scripting::Session>>(&state.dependencies.session);
    if (headless == nullptr) {
        replace();
        return;
    }
    const std::lock_guard lock(state.subscriptionMutex);
    // The subscription borrows the old project's event stream, which `replace` destroys.
    state.subscription.reset();
    replace();
    {
        const std::lock_guard guard(state.eventsMutex);
        state.events.clear();
        state.resetSequence = ++state.sequence;
        state.replaced = true;
    }
    state.eventsChanged.notify_all();
    scripting::Events events(headless->get());
    state.subscription = events.subscribe(
        [raw = &state](const commands::CommandEvent& event) { raw->record(event); });
}

std::span<const McpTool> McpServerCore::tools() const noexcept { return kTools; }

std::unique_ptr<McpProtocolSession> McpServerCore::openSession() {
    static std::atomic<std::uint64_t> nextSession{1};
    auto state = std::make_unique<McpProtocolSession::State>();
    state->id = nextSession.fetch_add(1, std::memory_order_relaxed);
    return std::unique_ptr<McpProtocolSession>(new McpProtocolSession(std::move(state)));
}

std::string McpServerCore::errorResponse(const std::optional<std::string_view> requestKey,
                                         const McpProtocolError code,
                                         const std::string_view message) {
    Json out;
    auto* root = out.object();
    out.set(root, "jsonrpc", out.text("2.0"));
    out.set(root, "id", requestId(out, requestKey));
    auto* detail = out.object();
    out.set(detail, "code", out.signedNumber(static_cast<std::int64_t>(code)));
    out.set(detail, "message", out.text(message));
    out.set(root, "error", detail);
    return out.encode(root);
}

std::string McpServerCore::limitResponse(const std::optional<std::string_view> requestKey,
                                         const McpLimitRefusal& refusal) {
    const auto description = describeLimit(refusal.limit);
    Json out;
    auto* root = out.object();
    out.set(root, "jsonrpc", out.text("2.0"));
    out.set(root, "id", requestId(out, requestKey));
    auto* detail = out.object();
    out.set(detail, "code", out.signedNumber(description.rpcCode));
    out.set(detail, "message", out.text(refusal.message));
    auto* data = out.object();
    out.set(data, "code", out.text(description.code));
    out.set(data, "limit", out.text(description.name));
    out.set(data, "value", out.number(refusal.value));
    out.set(data, "actual", out.number(refusal.actual));
    out.set(data, "unit", out.text(description.unit));
    if (refusal.retryAfterMs)
        out.set(data, "retryAfterMs", out.number(*refusal.retryAfterMs));
    out.set(detail, "data", data);
    out.set(root, "error", detail);
    return out.encode(root);
}

McpEnvelope inspectMessage(const std::string_view message, const McpMessageLimits& limits) {
    McpEnvelope envelope;
    if (message.size() > limits.requestBytes) {
        envelope.refusal = McpLimitRefusal{
            .limit = McpLimit::RequestBytes,
            .value = limits.requestBytes,
            .actual = message.size(),
            .message = "The request is " + grouped(message.size()) +
                       " bytes; Bloom reads at most " + grouped(limits.requestBytes) +
                       " (limit requestBytes). " + std::string(kMcpLimitAdvice)};
        return envelope;
    }
    std::shared_ptr<const Parsed> held;
    try {
        held = std::make_shared<const Parsed>(message, true);
    } catch (const LimitExceeded& exceeded) {
        envelope.refusal = refusalOf(exceeded);
        return envelope;
    } catch (const std::exception& error) {
        envelope.parseError = error.what();
        return envelope;
    }
    envelope.message = held;
    if (const auto& refusal = held->refusal())
        envelope.refusal = refusalOf(*refusal);
    const auto classify = [&] {
        try {
            const auto& parsed = *held;
            auto* root = parsed.root();
            if (!yyjson_is_obj(root))
                return;
            auto* method = member(root, "method");
            auto* id = member(root, "id");
            if (method == nullptr) {
                if (id != nullptr && (member(root, "result") || member(root, "error")))
                    envelope.kind = McpMessageKind::Response;
                return;
            }
            envelope.method = string(method, 128);
            if (id != nullptr && (yyjson_is_int(id) || yyjson_is_str(id))) {
                envelope.kind = McpMessageKind::Request;
                envelope.requestKey = requestIdentity(id);
            } else if (id == nullptr) {
                envelope.kind = McpMessageKind::Notification;
            } else {
                return;
            }
            auto* params = member(root, "params");
            if (envelope.method == "tools/call" && yyjson_is_obj(params)) {
                if (auto* name = member(params, "name"); yyjson_is_str(name))
                    envelope.tool =
                        toolFromName(std::string_view(yyjson_get_str(name), yyjson_get_len(name)));
                if (auto* meta = member(params, "_meta"); yyjson_is_obj(meta)) {
                    if (auto* token = member(meta, "progressToken");
                        yyjson_is_str(token) || yyjson_is_int(token)) {
                        Json out;
                        envelope.progressToken = out.encode(out.copy(token));
                    }
                }
            }
            if (envelope.method == "notifications/cancelled" && yyjson_is_obj(params)) {
                if (auto* cancelled = member(params, "requestId"))
                    envelope.cancels = requestIdentity(cancelled);
            }
        } catch (const std::exception&) {
            // Not a JSON-RPC message Bloom reads; handle() answers it from the parsed document.
            envelope.kind = McpMessageKind::Invalid;
            envelope.requestKey.reset();
            envelope.method.clear();
            envelope.tool.reset();
            envelope.progressToken.reset();
            envelope.cancels.reset();
        }
    };
    classify();
    // A refused message is answered even when it is a notification.
    if (envelope.refusal && envelope.kind != McpMessageKind::Request)
        envelope.kind = McpMessageKind::Invalid;
    return envelope;
}

McpHandled McpServerCore::handle(McpProtocolSession& protocol, const std::string_view message,
                                 const McpRequestControl& control) {
    return handle(protocol, inspectMessage(message, state_->dependencies.limits), control);
}

McpHandled McpServerCore::handle(McpProtocolSession& protocol, const McpEnvelope& message,
                                 const McpRequestControl& control) {
    auto& state = *state_;
    auto& session = *protocol.state_;
    McpHandled handled;
    const auto protocolError = [&](yyjson_val* id, const McpProtocolError code,
                                   const std::string_view text) {
        handled.outcome = McpCallOutcome::ProtocolError;
        std::optional<std::string> key;
        try {
            if (id != nullptr)
                key = requestIdentity(id);
        } catch (const std::exception&) {
            key.reset();
        }
        handled.response = errorResponse(key, code, text);
        return handled;
    };
    const auto limitError = [&](const McpLimitRefusal& refusal) {
        handled.outcome = McpCallOutcome::ProtocolError;
        handled.response = limitResponse(message.requestKey, refusal);
        return handled;
    };
    if (message.message == nullptr) {
        if (message.refusal)
            return limitError(*message.refusal);
        return protocolError(nullptr, McpProtocolError::ParseError,
                             message.parseError.empty() ? "Invalid JSON" : message.parseError);
    }
    if (message.refusal) {
        handled.kind = message.kind;
        handled.method = message.method;
        return limitError(*message.refusal);
    }
    auto* request = message.message->root();
    yyjson_val* id = nullptr;
    try {
        // A JSON-RPC response from the client: Bloom sends no requests, so acknowledge and
        // ignore it.
        if (yyjson_is_obj(request) && !member(request, "method") && member(request, "id") &&
            (member(request, "result") || member(request, "error"))) {
            handled.kind = McpMessageKind::Response;
            return handled;
        }
        members(request, {"jsonrpc", "id", "method", "params"}, {"jsonrpc", "method"});
        if (string(member(request, "jsonrpc")) != "2.0")
            throw InvalidInput("Expected JSON-RPC 2.0");
        handled.method = string(member(request, "method"), 128);
        id = member(request, "id");
        if (id && !(yyjson_is_int(id) || yyjson_is_str(id))) {
            id = nullptr;
            throw InvalidInput("Request ID must be an integer or string");
        }
        if (yyjson_is_str(id))
            static_cast<void>(string(id, 128));
    } catch (const std::exception& exception) {
        handled.kind = McpMessageKind::Invalid;
        return protocolError(nullptr, McpProtocolError::InvalidRequest, exception.what());
    }
    auto* params = member(request, "params");
    if (!id) {
        handled.kind = McpMessageKind::Notification;
        handled.summary = handled.method;
        if (handled.method == "notifications/initialized") {
            const std::lock_guard lock(session.mutex);
            if (session.initialized) {
                try {
                    if (params)
                        members(params, {});
                    session.ready = true;
                } catch (const InvalidInput&) {
                    // JSON-RPC notifications never receive a response.
                }
            }
        }
        return handled;
    }
    handled.kind = McpMessageKind::Request;
    handled.summary = handled.method;
    Json out;
    auto* root = out.object();
    out.set(root, "jsonrpc", out.text("2.0"));
    out.set(root, "id", out.copy(id));
    try {
        yyjson_mut_val* result = nullptr;
        const auto& method = handled.method;
        if (method == "initialize") {
            members(params, {"protocolVersion", "capabilities", "clientInfo"},
                    {"protocolVersion", "capabilities", "clientInfo"});
            const auto version = string(member(params, "protocolVersion"), 32);
            if (!yyjson_is_obj(member(params, "capabilities")))
                throw InvalidInput("Invalid capabilities");
            auto* client = member(params, "clientInfo");
            members(client, {"name", "version", "title", "description", "websiteUrl", "icons"},
                    {"name", "version"});
            static_cast<void>(string(member(client, "name"), 256));
            static_cast<void>(string(member(client, "version"), 256));
            {
                const std::lock_guard lock(session.mutex);
                if (session.initialized)
                    throw InvalidInput("Already initialized");
                session.initialized = true;
                session.client = McpClientInfo{sanitised(member(client, "name")),
                                               sanitised(member(client, "version")),
                                               sanitised(member(client, "title")), version};
            }
            handled.initializedClient = true;
            result = out.object();
            out.set(result, "protocolVersion", out.text(kMcpProtocolVersion));
            auto* info = out.object();
            out.set(info, "name", out.text("Bloom"));
            out.set(info, "version", out.text(state.dependencies.serverVersion));
            out.set(result, "serverInfo", info);
            auto* capabilities = out.object();
            auto* toolCapabilities = out.object();
            out.set(toolCapabilities, "listChanged", out.boolean(false));
            out.set(capabilities, "tools", toolCapabilities);
            auto* resourceCapabilities = out.object();
            out.set(resourceCapabilities, "listChanged", out.boolean(false));
            out.set(capabilities, "resources", resourceCapabilities);
            auto* experimental = out.object();
            auto* bloom = out.object();
            out.set(bloom, "facadeVersion", out.text("1.0"));
            out.set(bloom, "headless", out.boolean(state.liveHost() == nullptr));
            out.set(bloom, "requestByteLimit", out.number(state.dependencies.limits.requestBytes));
            // #744: the most work one transact carries in the desktop application.
            if (state.liveHost() != nullptr)
                out.set(bloom, "commitWorkLimit", out.number(scripting::kLiveCommitWorkLimit));
            // #747: transact async:true (with jobs.list, jobs.wait, jobs.cancel) and
            // idempotencyKey.
            out.set(bloom, "asyncTransact", out.boolean(state.liveHost() != nullptr));
            out.set(bloom, "idempotencyKeys", out.boolean(true));
            // #747 (D2): events.subscribe delivers notifications/bloom/events (on the Streamable
            // HTTP GET stream, or on stdout for stdio); events takes limit, filters and a summary.
            out.set(bloom, "eventSubscriptions", out.boolean(true));
            // #745, #741: every request limit, by the name its refusal gives.
            out.set(bloom, "limits", limitsValue(out, state));
            out.set(experimental, "bloom", bloom);
            out.set(capabilities, "experimental", experimental);
            out.set(result, "capabilities", capabilities);
            out.set(result, "instructions", out.text(serverInstructions()));
        } else if (method == "ping") {
            if (params)
                members(params, {});
            result = out.object();
        } else if (!protocol.ready()) {
            throw InvalidInput("Complete initialize and notifications/initialized first");
        } else if (method == "tools/list") {
            if (params)
                members(params, {});
            result = toolList(out);
        } else if (method == "resources/list") {
            if (params)
                members(params, {"cursor", "_meta"});
            result = resourceList(out);
        } else if (method == "resources/templates/list") {
            if (params)
                members(params, {"cursor", "_meta"});
            result = out.object();
            out.set(result, "resourceTemplates", out.array());
        } else if (method == "resources/read") {
            result = resourceRead(out, params);
        } else if (method == "tools/call") {
            members(params, {"name", "arguments", "_meta"}, {"name", "arguments"});
            const auto name = string(member(params, "name"), 64);
            const auto tool = toolFromName(name);
            if (!tool)
                throw InvalidInput("Unknown tool");
            handled.tool = tool;
            handled.summary = std::string(toolName(*tool));
            result = out.object();
            yyjson_mut_val* value = nullptr;
            bool failed = false;
            std::vector<yyjson_mut_val*> content;
            std::function<yyjson_mut_val*(Json&)> compact;
            try {
                // `projectFile` points a tool at another project file, in a headless session of
                // its own; the tool itself never sees the member.
                FrozenJson withoutFile(nullptr, yyjson_doc_free);
                yyjson_val* toolArguments = member(params, "arguments");
                const auto projectFile = projectFileArgument(toolArguments);
                if (projectFile) {
                    switch (*tool) {
                    case McpTool::Query:
                    case McpTool::Transact:
                    case McpTool::History:
                    case McpTool::Render:
                    case McpTool::Export:
                    case McpTool::Capture:
                    case McpTool::ProjectSave:
                    case McpTool::ProjectClose:
                        break;
                    default:
                        throw InvalidInput("projectFile applies to query, transact, history, "
                                           "render, export, capture, project.save and "
                                           "project.close");
                    }
                    withoutFile = withoutProjectFile(toolArguments);
                    toolArguments = yyjson_doc_get_root(withoutFile.get());
                }
                if (*tool == McpTool::JobsWait) {
                    // #747: a job outlives the request that submitted it; no lease is held.
                    value = jobsWaitTool(state, out, member(params, "arguments"), control);
                } else if (*tool == McpTool::EventsSubscribe) {
                    // #747 (D2): the subscription belongs to the protocol session; no lease.
                    value = eventsSubscribeTool(state, out, session, member(params, "arguments"));
                } else if (*tool == McpTool::EventsUnsubscribe) {
                    value = eventsUnsubscribeTool(state, out, session, member(params, "arguments"));
                } else if (*tool == McpTool::ProfilerSnapshot) {
                    // Telemetry holds no lease: it answers while a project opens.
                    if (auto* arguments = member(params, "arguments"))
                        members(arguments, {});
                    value = profilerSnapshotTool(state, out, control);
                } else if (projectFile &&
                           (*tool == McpTool::ProjectSave || *tool == McpTool::ProjectClose)) {
                    value = fileProjectTool(state, out, *tool, *projectFile, toolArguments, control);
                } else if (toolUsesLiveUi(*tool)) {
                    // Window and project-lifecycle tools hold no lease: project.open must work
                    // while the old project is being replaced.
                    std::string summary;
                    value = sessionTool(state, out, *tool, member(params, "arguments"), control,
                                        summary, protocol.id());
                    if (!summary.empty() && summary != handled.summary)
                        handled.summary += " " + summary;
                } else {
                    // Read and commit through the live lease or the headless session.
                    std::optional<scripting::LiveSessionLease> lease;
                    scripting::Session* target = nullptr;
                    scripting::OperationContext context;
                    std::uint64_t generation = 0;
                    std::unique_lock<std::timed_mutex> fileLock;
                    if (projectFile) {
                        fileLock = std::unique_lock(state.fileMutex);
                        target = &fileSessionFor(state, *projectFile, control);
                        context = target->operationContext();
                    } else if (auto* live = state.liveHost()) {
                        state.syncEvents();
                        lease.emplace(live->acquire(
                            {.stop = control.stop,
                             .deadline = std::chrono::steady_clock::now() + kMcpCommitDeadline}));
                        if (!*lease)
                            throw ToolFailure(
                                McpToolErrorCode::ProjectUnavailable,
                                lease->availability() ==
                                        scripting::LiveSessionAvailability::ShuttingDown
                                    ? "Bloom is closing."
                                    : "A project is opening.");
                        target = &lease->session();
                        context = lease->operationContext();
                        generation = lease->generation();
                    } else {
                        auto& headless = std::get<std::reference_wrapper<scripting::Session>>(
                                             state.dependencies.session)
                                             .get();
                        target = &headless;
                        context = headless.operationContext();
                    }
                    // A probe or evaluation an operation runs when it is created stops with
                    // the request.
                    context.stop = control.stop;
                    McpCall call{.session = *target,
                                 .context = std::move(context),
                                 .live = lease ? &lease->state() : nullptr,
                                 .lease = lease ? &*lease : nullptr,
                                 .generation = generation,
                                 .control = control,
                                 .content = {},
                                 .summary = {},
                                 .notice = std::nullopt,
                                 .compileFailures = {},
                                 .gpuProvider =
                                     projectFile ? state.fileGpuProvider : state.gpuExportProvider,
                                 .fileSession = projectFile.has_value(),
                                 .compact = {},
                                 .client = protocol.id(),
                                 .envelope = &message,
                                 .committed = false};
                    auto* arguments = toolArguments;
                    switch (*tool) {
                    case McpTool::Query:
                        value = queryTool(state, out, call, arguments);
                        break;
                    case McpTool::Transact:
                        value = transactTool(state, out, call, arguments);
                        break;
                    case McpTool::History:
                        value = historyTool(state, out, call, arguments);
                        break;
                    case McpTool::Events:
                        value = eventsTool(state, out, call, arguments);
                        break;
                    case McpTool::Render:
                    case McpTool::Export:
                        value = renderTool(state, out, call, arguments, *tool == McpTool::Export);
                        break;
                    case McpTool::Capture:
                        value = captureTool(state, out, call, arguments);
                        break;
                    case McpTool::ProjectSetColorSettings:
                        value = colorSettingsTool(state, out, call, arguments);
                        break;
                    case McpTool::Help:
                        value = helpTool(state, out, call, arguments);
                        break;
                    default:
                        value = exportSessionTool(state, out, call, *tool, arguments);
                        break;
                    }
                    if (!call.summary.empty())
                        handled.summary += " " + call.summary;
                    content = std::move(call.content);
                    handled.toolError = call.notice;
                    handled.compileFailures = std::move(call.compileFailures);
                    compact = std::move(call.compact);
                }
            } catch (const ToolFailure& failure) {
                failed = true;
                handled.toolError = failure.code;
                value = toolFailureValue(out, failure);
                if (failure.code == McpToolErrorCode::Cancelled)
                    handled.outcome = McpCallOutcome::Cancelled;
            } catch (const std::runtime_error& exception) {
                if (dynamic_cast<const InvalidInput*>(&exception) ||
                    dynamic_cast<const LimitExceeded*>(&exception))
                    throw;
                failed = true;
                if (control.stop.stop_requested()) {
                    handled.toolError = McpToolErrorCode::Cancelled;
                    handled.outcome = McpCallOutcome::Cancelled;
                }
                value = out.object();
                out.set(value, "diagnostic", out.text(exception.what()));
            }
            auto* succeeded = yyjson_mut_obj_get(value, "succeeded");
            if (succeeded && yyjson_mut_is_false(succeeded))
                failed = true;
            if (failed && handled.outcome == McpCallOutcome::Succeeded)
                handled.outcome = McpCallOutcome::ToolError;
            const auto respond = [&](Json& json, yyjson_mut_val* answer, yyjson_mut_val* body,
                                     const std::vector<yyjson_mut_val*>& blocksBefore) {
                auto* envelope = json.object();
                json.set(envelope, "jsonrpc", json.text("2.0"));
                json.set(envelope, "id", json.copy(id));
                json.set(answer, "isError", json.boolean(failed));
                json.set(answer, "structuredContent", body);
                auto* blocks = json.array();
                for (auto* block : blocksBefore)
                    json.append(blocks, block);
                auto* text = json.object();
                json.set(text, "type", json.text("text"));
                json.set(text, "text",
                         json.text(json.encode(body, state.dependencies.limits.responseBytes)));
                json.append(blocks, text);
                json.set(answer, "content", blocks);
                json.set(envelope, "result", answer);
                return json.encode(envelope, state.dependencies.limits.responseBytes);
            };
            try {
                handled.response = respond(out, result, value, content);
            } catch (const LimitExceeded&) {
                // #745: a transaction that applied is always answered. Its compact form (no id
                // list, bounded diagnostics) replaces a full answer that does not fit.
                if (!compact)
                    throw;
                Json small;
                auto* answer = small.object();
                handled.response = respond(small, answer, compact(small), {});
            }
            if (handled.summary.size() > 256)
                handled.summary.resize(256);
            return handled;
        } else {
            return protocolError(id, McpProtocolError::MethodNotFound, "Method not found");
        }
        out.set(root, "result", result);
        handled.response = out.encode(root, state.dependencies.limits.responseBytes);
        if (handled.summary.size() > 256)
            handled.summary.resize(256);
        return handled;
    } catch (const LimitExceeded& exceeded) {
        return limitError(refusalOf(exceeded));
    } catch (const InvalidInput& exception) {
        return protocolError(id, McpProtocolError::InvalidParams, exception.what());
    } catch (const std::exception& exception) {
        return protocolError(id, McpProtocolError::InternalError, exception.what());
    }
}

} // namespace bloom::mcp
