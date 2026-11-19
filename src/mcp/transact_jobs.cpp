// #747 (D1): asynchronous transact jobs, idempotency keys and load-aware retry advice.
//
// transact with async:true answers with a job id at once; one runner thread takes the jobs in
// order and runs each exactly as a synchronous transact would (runTransact) on a lease of the
// generation it was submitted on, so a job never applies to a project opened after it. Its
// progress is the commit's own (building, waiting in the live queue, preparing, publishing, the
// compile check); jobs.list lists it, jobs.wait waits for it (streaming progress) and returns its
// result, and jobs.cancel stops it until its commit starts publishing. Every step is published as
// an McpJobEvent.
//
// An idempotencyKey remembers, for kMcpIdempotencyWindow, the answer of a transaction that reached
// the command stack; the same key with the same arguments answers it again instead of applying
// twice, and an answer where nothing could apply (busy, cancelled, replaced) is forgotten so a
// retry runs.
#include "core_internal.hpp"

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace bloom::mcp {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds kDefaultWait{30000};

[[nodiscard]] bool finished(const McpJobState state) noexcept {
    return state != McpJobState::Queued && state != McpJobState::Running;
}

// A failure answer for a job that never reached its transaction (or a refusal it met).
yyjson_mut_val* diagnosticValue(Json& out, const std::string_view code, const std::string& message) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(false));
    auto* diagnostic = out.object();
    out.set(diagnostic, "code", out.text(code));
    out.set(diagnostic, "message", out.text(message));
    out.set(value, "diagnostic", diagnostic);
    return value;
}

// `value` encoded and parsed back, so it outlives `out`; the compact form when it is larger than
// `limit` bytes.
std::shared_ptr<const McpStoredAnswer>
storeAnswer(Json& out, yyjson_mut_val* value, const std::function<yyjson_mut_val*(Json&)>& compact,
            const std::size_t limit) {
    std::string text;
    try {
        text = out.encode(value, limit);
    } catch (const LimitExceeded&) {
        if (compact) {
            Json small;
            auto* reduced = compact(small);
            text = small.encode(reduced);
        } else {
            // Nothing changed, so there is no compact form; the answer is bounded by its lists.
            text = out.encode(value);
        }
    }
    auto answer = std::make_shared<McpStoredAnswer>();
    answer->body = std::make_shared<const Parsed>(text, true);
    auto* succeeded = yyjson_obj_get(answer->body->root(), "succeeded");
    answer->failed = yyjson_is_bool(succeeded) && !yyjson_get_bool(succeeded);
    return answer;
}

std::shared_ptr<const McpStoredAnswer> failureAnswer(const std::string_view code,
                                                     const std::string& message) {
    Json out;
    return storeAnswer(out, diagnosticValue(out, code, message), {}, kMcpMessageLimits.responseBytes);
}

std::string bodyCode(const McpStoredAnswer& answer) {
    auto* root = answer.body->root();
    auto* diagnostic = yyjson_obj_get(root, "diagnostic");
    if (auto* code = yyjson_obj_get(diagnostic, "code"); yyjson_is_str(code))
        return {yyjson_get_str(code), yyjson_get_len(code)};
    // A transaction the document refused: its first diagnostic's code.
    auto* list = yyjson_obj_get(root, "diagnostics");
    if (auto* first = yyjson_arr_get_first(list); first != nullptr)
        if (auto* code = yyjson_obj_get(first, "code"); yyjson_is_str(code))
            return {yyjson_get_str(code), yyjson_get_len(code)};
    return {};
}

McpJobEvent eventOf(const McpTransactJob& job) {
    McpJobEvent event;
    event.job = job.id;
    event.client = job.client;
    event.state = job.state;
    event.phase = job.phase;
    event.progress = job.progress;
    event.progressToken = job.progressToken;
    event.revision = job.revision;
    event.finished = finished(job.state);
    event.cancellationRequested = job.cancellationRequested;
    event.code = job.code;
    return event;
}

// Keeps the unfinished jobs and the newest kMcpRetainedJobs finished ones. Called with the jobs
// mutex held.
void pruneJobs(McpTransactJobs& jobs) {
    std::size_t done = 0;
    for (const auto& job : jobs.jobs)
        done += finished(job->state) ? 1U : 0U;
    for (auto at = jobs.jobs.begin(); at != jobs.jobs.end() && done > kMcpRetainedJobs;) {
        if (finished((*at)->state)) {
            at = jobs.jobs.erase(at);
            --done;
        } else {
            ++at;
        }
    }
}

// ---- Idempotency keys

std::string idempotencyKeyArgument(yyjson_val* value) {
    if (!yyjson_is_str(value) || yyjson_get_len(value) == 0 ||
        yyjson_get_len(value) > kMcpIdempotencyKeyBytes)
        throw InvalidInput("idempotencyKey is 1 to " + std::to_string(kMcpIdempotencyKeyBytes) +
                           " printable ASCII characters (a UUID, for example)");
    const std::string_view key(yyjson_get_str(value), yyjson_get_len(value));
    for (const char c : key)
        if (static_cast<unsigned char>(c) < 0x21U || static_cast<unsigned char>(c) > 0x7eU)
            throw InvalidInput("idempotencyKey is printable ASCII without spaces");
    return std::string(key);
}

// The arguments without the key itself, as one SHA-256: the same request sent again has the same
// fingerprint.
std::string fingerprintOf(yyjson_val* arguments) {
    Json out;
    auto* copy = out.copy(arguments);
    yyjson_mut_obj_remove_str(copy, "idempotencyKey");
    const auto text = out.encode(copy, std::numeric_limits<std::size_t>::max());
    core::Sha256Hasher hasher;
    static_cast<void>(hasher.update(std::as_bytes(std::span(text.data(), text.size()))));
    const auto hex = hasher.finalize().toLowercaseHex();
    return {hex.data(), hex.size()};
}

struct Claim final {
    // Non-zero: this call owns a new pending entry and must remember or forget it.
    std::uint64_t ticket = 0;
    std::shared_ptr<const McpStoredAnswer> answer;
    std::shared_ptr<McpTransactJob> job;
};

void pruneKeys(McpIdempotency& keys, const Clock::time_point now) {
    std::erase_if(keys.entries, [&](const auto& entry) {
        return !entry.second.pending && now - entry.second.at >= kMcpIdempotencyWindow;
    });
}

Claim claimKey(McpServerCore::State& core, const std::string& key, const std::string& fingerprint,
               const std::stop_token& stop) {
    auto& keys = core.idempotency;
    std::unique_lock lock(keys.mutex);
    while (true) {
        const auto now = Clock::now();
        pruneKeys(keys, now);
        const auto found = keys.entries.find(key);
        if (found != keys.entries.end()) {
            auto& entry = found->second;
            if (entry.fingerprint != fingerprint)
                throw ToolFailure(McpToolErrorCode::IdempotencyConflict,
                                  "idempotencyKey \"" + key +
                                      "\" was used for a request with different arguments in "
                                      "the last " +
                                      std::to_string(kMcpIdempotencyWindow.count()) +
                                      " minutes; nothing was applied. Use a new key for a new "
                                      "edit.");
            if (entry.job)
                return {.ticket = 0, .answer = nullptr, .job = entry.job};
            if (!entry.pending)
                return {.ticket = 0, .answer = entry.answer, .job = nullptr};
            // The first request with this key is still running: answer with its result (or, for
            // an asynchronous one, its job as soon as it has one).
            const auto ticket = entry.ticket;
            const bool woke = keys.changed.wait(lock, stop, [&] {
                const auto again = keys.entries.find(key);
                return again == keys.entries.end() || again->second.ticket != ticket ||
                       !again->second.pending || again->second.job != nullptr;
            });
            if (!woke)
                throw ToolFailure(McpToolErrorCode::Cancelled,
                                  "The request was cancelled; nothing was applied.");
            continue;
        }
        if (keys.entries.size() >= kMcpIdempotencyKeys) {
            const auto oldest = std::ranges::min_element(
                keys.entries, {}, [](const auto& candidate) { return candidate.second.at; });
            keys.entries.erase(oldest);
        }
        const auto ticket = keys.nextTicket++;
        keys.entries.emplace(key, McpIdempotency::Entry{.fingerprint = fingerprint,
                                                        .at = now,
                                                        .ticket = ticket,
                                                        .pending = true,
                                                        .answer = nullptr,
                                                        .job = nullptr});
        return {.ticket = ticket, .answer = nullptr, .job = nullptr};
    }
}

// The claim's outcome: an answer to give again, a job to describe again, or (both null) nothing,
// which forgets the key so a retry runs.
void settleKey(McpServerCore::State& core, const std::string& key, const std::uint64_t ticket,
               std::shared_ptr<const McpStoredAnswer> answer,
               std::shared_ptr<McpTransactJob> job) {
    auto& keys = core.idempotency;
    {
        const std::lock_guard lock(keys.mutex);
        const auto found = keys.entries.find(key);
        if (found == keys.entries.end() || found->second.ticket != ticket)
            return;
        if (!answer && !job) {
            keys.entries.erase(found);
        } else {
            found->second.answer = std::move(answer);
            found->second.job = std::move(job);
            found->second.pending = static_cast<bool>(found->second.job);
            found->second.at = Clock::now();
        }
    }
    keys.changed.notify_all();
}

// A finished asynchronous job's key: kept with its result when the transaction reached the command
// stack, forgotten otherwise.
void settleJobKey(McpServerCore::State& core, const McpTransactJob& job, const bool committed,
                  std::shared_ptr<const McpStoredAnswer> answer) {
    if (!job.idempotencyKey)
        return;
    auto& keys = core.idempotency;
    {
        const std::lock_guard lock(keys.mutex);
        const auto found = keys.entries.find(*job.idempotencyKey);
        if (found == keys.entries.end() || found->second.job.get() != &job)
            return;
        if (committed) {
            found->second.pending = false;
            found->second.answer = std::move(answer);
            found->second.at = Clock::now();
        } else {
            keys.entries.erase(found);
        }
    }
    keys.changed.notify_all();
}

// ---- Jobs

// One job as jobs.list, jobs.wait and a submission describe it, written into `value`.
void jobFields(Json& out, yyjson_mut_val* value, const McpTransactJob& job, const bool withResult) {
    out.set(value, "kind", out.text("transact"));
    out.set(value, "name", out.text(job.label));
    out.set(value, "state", out.text(jobStateName(job.state)));
    out.set(value, "phase", out.text(job.phase));
    out.set(value, "completed",
            out.number(static_cast<std::uint64_t>(std::max(job.progress.progress, 0.0))));
    out.set(value, "total",
            job.progress.total
                ? out.number(static_cast<std::uint64_t>(std::max(*job.progress.total, 0.0)))
                : out.null());
    out.set(value, "message", out.text(job.progress.message));
    out.set(value, "canCancel", out.boolean(!finished(job.state) && job.phase != "publishing" &&
                                            job.phase != "checking"));
    out.set(value, "cancellationRequested", out.boolean(job.cancellationRequested));
    out.set(value, "exportJob", out.null());
    out.set(value, "revision", job.revision ? out.number(*job.revision) : out.null());
    out.set(value, "code", job.code.empty() ? out.null() : out.text(job.code));
    out.set(value, "finished", out.boolean(finished(job.state)));
    if (withResult)
        out.set(value, "result", job.result ? storedAnswerValue(out, *job.result) : out.null());
}

yyjson_mut_val* jobValue(Json& out, const McpTransactJob& job, const bool withResult) {
    auto* value = out.object();
    out.set(value, "id", out.text(job.id));
    jobFields(out, value, job, withResult);
    return value;
}

// Ends `job` with `answer`. Called with the jobs mutex held; returns the event to publish once it
// is released.
McpJobEvent finishLocked(McpTransactJobs& jobs, McpTransactJob& job,
                         std::shared_ptr<const McpStoredAnswer> answer, const bool committed) {
    job.result = std::move(answer);
    job.code = bodyCode(*job.result);
    const bool cancelled = job.code == toolErrorCode(McpToolErrorCode::Cancelled);
    job.state = !job.result->failed ? McpJobState::Succeeded
                : cancelled         ? McpJobState::Cancelled
                                    : McpJobState::Failed;
    if (!job.result->failed)
        job.code.clear();
    if (committed) {
        if (auto* revision = yyjson_obj_get(job.result->body->root(), "revision");
            yyjson_is_uint(revision))
            job.revision = yyjson_get_uint(revision);
    }
    job.phase = "done";
    if (job.progress.total)
        job.progress.progress = *job.progress.total;
    job.progress.message = job.state == McpJobState::Succeeded ? "Done"
                           : cancelled                         ? "Cancelled"
                                                               : "Failed";
    ++job.serial;
    std::erase_if(jobs.queue, [&](const auto& queued) { return queued.get() == &job; });
    pruneJobs(jobs);
    jobs.changed.notify_all();
    auto event = eventOf(job);
    event.duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - job.submitted);
    return event;
}

void runJob(McpServerCore::State& core, const std::shared_ptr<McpTransactJob>& job) {
    Json out;
    yyjson_mut_val* value = nullptr;
    bool committed = false;
    std::function<yyjson_mut_val*(Json&)> compact;
    std::vector<McpCompileFailureFact> compileFailures;
    std::string summary;
    McpRequestControl control;
    control.stop = job->stop.get_token();
    control.progress = [&core, job](const McpProgress& progress) {
        McpJobEvent event;
        {
            const std::lock_guard lock(core.jobs.mutex);
            if (finished(job->state))
                return;
            job->progress = progress;
            if (!progress.phase.empty())
                job->phase = progress.phase;
            ++job->serial;
            event = eventOf(*job);
        }
        core.jobs.changed.notify_all();
        core.publishJob(event);
    };
    try {
        auto* live = core.liveHost();
        if (live == nullptr)
            throw ToolFailure(McpToolErrorCode::NotLive, "This server has no application window.");
        core.syncEvents();
        auto lease = live->acquire({.stop = control.stop, .deadline = Clock::now() + kMcpCommitDeadline});
        if (!lease)
            throw ToolFailure(McpToolErrorCode::ProjectUnavailable,
                              lease.availability() == scripting::LiveSessionAvailability::ShuttingDown
                                  ? "Bloom is closing; nothing was applied."
                                  : "A project is opening; nothing was applied.");
        if (lease.generation() != job->generation)
            throw ToolFailure(McpToolErrorCode::ProjectReplaced,
                              "The project was replaced before the job ran; nothing was applied.");
        auto context = lease.operationContext();
        context.stop = control.stop;
        McpCall call{.session = lease.session(),
                     .context = std::move(context),
                     .live = &lease.state(),
                     .lease = &lease,
                     .generation = lease.generation(),
                     .control = control,
                     .content = {},
                     .summary = {},
                     .notice = std::nullopt,
                     .compileFailures = {},
                     .gpuProvider = nullptr,
                     .fileSession = false,
                     .compact = {},
                     .client = job->client,
                     .envelope = nullptr,
                     .committed = false};
        value = runTransact(core, out, call, job->arguments);
        committed = call.committed;
        compact = std::move(call.compact);
        compileFailures = std::move(call.compileFailures);
        summary = std::move(call.summary);
    } catch (const ToolFailure& failure) {
        value = toolFailureValue(out, failure);
    } catch (const LimitExceeded& exceeded) {
        ToolFailure failure(McpToolErrorCode::OperationRejected, exceeded.what());
        value = toolFailureValue(out, failure);
        auto* diagnostic = yyjson_mut_obj_get(value, "diagnostic");
        yyjson_mut_obj_remove_str(diagnostic, "code");
        out.set(diagnostic, "code", out.text(describeLimit(exceeded.limit).code));
        auto* limit = out.object();
        out.set(limit, "name", out.text(describeLimit(exceeded.limit).name));
        out.set(limit, "value", out.number(exceeded.value));
        out.set(limit, "actual", out.number(exceeded.actual));
        out.set(limit, "unit", out.text(describeLimit(exceeded.limit).unit));
        out.set(diagnostic, "limit", limit);
    } catch (const InvalidInput& error) {
        value = diagnosticValue(out, uiRefusalCode(McpUiRefusal::InvalidArgument), error.what());
    } catch (const std::exception& error) {
        value = diagnosticValue(out, uiRefusalCode(McpUiRefusal::Failed), error.what());
    }
    std::shared_ptr<const McpStoredAnswer> answer;
    try {
        answer = storeAnswer(out, value, compact, kMcpMessageLimits.responseBytes);
    } catch (const std::exception& error) {
        answer = failureAnswer(uiRefusalCode(McpUiRefusal::Failed), error.what());
    }
    McpJobEvent event;
    {
        const std::lock_guard lock(core.jobs.mutex);
        event = finishLocked(core.jobs, *job, answer, committed);
    }
    settleJobKey(core, *job, committed, answer);
    event.summary = "transact " + job->id + (summary.empty() ? "" : " " + summary);
    event.compileFailures = std::move(compileFailures);
    core.publishJob(event);
}

void runJobs(McpServerCore::State& core) {
    auto& jobs = core.jobs;
    while (true) {
        std::shared_ptr<McpTransactJob> job;
        McpJobEvent event;
        {
            std::unique_lock lock(jobs.mutex);
            jobs.changed.wait(lock, [&] { return jobs.stopping || !jobs.queue.empty(); });
            if (jobs.stopping)
                return;
            job = jobs.queue.front();
            jobs.queue.pop_front();
            job->state = McpJobState::Running;
            job->phase = "building";
            ++job->serial;
            event = eventOf(*job);
        }
        jobs.changed.notify_all();
        core.publishJob(event);
        runJob(core, job);
    }
}

std::shared_ptr<McpTransactJob> findJob(McpTransactJobs& jobs, const std::string& id) {
    const auto found = std::ranges::find(jobs.jobs, id, &McpTransactJob::id);
    return found == jobs.jobs.end() ? nullptr : *found;
}

std::string jobArgument(yyjson_val* arguments) {
    auto* job = member(arguments, "job");
    if (!yyjson_is_str(job))
        throw InvalidInput("job is a transact job id such as \"transact-3\" (transact with "
                           "async:true answers it; jobs.list lists them)");
    return string(job, 64);
}

// A described job, or the answer of an idempotent replay of an asynchronous submission.
yyjson_mut_val* submissionValue(McpServerCore::State& core, Json& out, const McpTransactJob& job,
                                const bool replay) {
    auto* answer = out.object();
    out.set(answer, "succeeded", out.boolean(true));
    out.set(answer, "async", out.boolean(true));
    out.set(answer, "job", out.text(job.id));
    {
        const std::lock_guard lock(core.jobs.mutex);
        jobFields(out, answer, job, finished(job.state));
    }
    if (replay)
        out.set(answer, "idempotentReplay", out.boolean(true));
    return answer;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// State

void McpServerCore::State::recordHeadlessCommit(const Clock::duration taken) {
    const auto ms = std::chrono::duration<double, std::milli>(taken).count();
    const std::lock_guard lock(loadMutex);
    headlessCommitMs = headlessCommitMs ? *headlessCommitMs * 0.75 + ms * 0.25 : ms;
}

std::uint64_t McpServerCore::State::retryAfterMs(const std::size_t queuedAhead) const {
    auto recent = static_cast<double>(kMcpDefaultCommitTime.count());
    if (const auto* live = liveHost()) {
        if (const auto measured = live->recentCommitTime())
            recent = static_cast<double>(measured->count());
    } else {
        const std::lock_guard lock(loadMutex);
        if (headlessCommitMs)
            recent = *headlessCommitMs;
    }
    const auto ceiling = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(kMcpCommitDeadline).count());
    const auto estimate = std::clamp(recent * static_cast<double>(queuedAhead + 1),
                                     static_cast<double>(kMcpRetryAfterFloor.count()), ceiling);
    return static_cast<std::uint64_t>(std::ceil(estimate));
}

void McpServerCore::State::publishJob(const McpJobEvent& event) const {
    if (!dependencies.jobEvents)
        return;
    try {
        dependencies.jobEvents(event);
    } catch (...) {
        // A listener's failure never stops a job.
    }
}

void McpServerCore::State::revokeJobs(const std::uint64_t generation) {
    std::vector<std::pair<std::shared_ptr<McpTransactJob>, McpJobEvent>> ended;
    const auto answer = failureAnswer(toolErrorCode(McpToolErrorCode::ProjectReplaced),
                                      "The project was replaced before the job ran; nothing was "
                                      "applied.");
    {
        const std::lock_guard lock(jobs.mutex);
        const auto queued = jobs.queue;
        for (const auto& job : queued)
            if (job->generation != generation) {
                job->stop.request_stop();
                ended.emplace_back(job, finishLocked(jobs, *job, answer, false));
            }
    }
    for (auto& [job, event] : ended) {
        settleJobKey(*this, *job, false, nullptr);
        event.summary = "transact " + job->id;
        publishJob(event);
    }
}

void McpServerCore::State::stopJobs() {
    {
        const std::lock_guard lock(jobs.mutex);
        jobs.stopping = true;
        for (const auto& job : jobs.jobs)
            if (!finished(job->state))
                job->stop.request_stop();
    }
    jobs.changed.notify_all();
    if (jobs.runner)
        jobs.runner->join();
    jobs.runner.reset();
}

std::uint64_t McpServerCore::retryAfterMs(const std::size_t queuedAhead) const {
    return state_->retryAfterMs(queuedAhead);
}

// ---------------------------------------------------------------------------------------------
// Tools

yyjson_mut_val* storedAnswerValue(Json& out, const McpStoredAnswer& answer) {
    return out.copy(answer.body->root());
}

yyjson_mut_val* transactTool(McpServerCore::State& core, Json& out, McpCall& call,
                             yyjson_val* arguments) {
    members(arguments,
            {"expectedRevision", "operations", "label", "response", "compileCheck", "async",
             "idempotencyKey"},
            {"expectedRevision", "operations"});
    const bool async = member(arguments, "async") ? boolean(member(arguments, "async")) : false;
    std::optional<std::string> key;
    if (auto* given = member(arguments, "idempotencyKey"))
        key = idempotencyKeyArgument(given);
    if (async && core.liveHost() == nullptr)
        throw ToolFailure(McpToolErrorCode::NotLive,
                          "transact async:true runs in the desktop application only; on this "
                          "server transact answers when it has applied, so send it without async.");
    if (async && call.fileSession)
        throw InvalidInput("async applies to the open project; a projectFile edit runs "
                           "synchronously, so send it without async");
    if (!key)
        return async ? submitTransactJob(core, out, call, arguments)
                     : runTransact(core, out, call, arguments);
    call.summary = "idempotencyKey";
    const auto claim = claimKey(core, *key, fingerprintOf(arguments), call.control.stop);
    if (claim.job) {
        call.summary += " replay " + claim.job->id;
        return submissionValue(core, out, *claim.job, true);
    }
    if (claim.answer) {
        call.summary += " replay";
        auto* value = storedAnswerValue(out, *claim.answer);
        out.set(value, "idempotentReplay", out.boolean(true));
        return value;
    }
    // This call owns the key until it has an answer to remember (or none).
    struct Settle final {
        McpServerCore::State& core;
        const std::string& key;
        std::uint64_t ticket;
        bool settled = false;
        ~Settle() {
            if (!settled)
                settleKey(core, key, ticket, nullptr, nullptr);
        }
    } settle{core, *key, claim.ticket};
    if (async) {
        // The key is bound to the job before it is queued; a refused submission forgets it.
        auto* value = submitTransactJob(core, out, call, arguments, &*key, claim.ticket);
        settle.settled = true;
        return value;
    }
    auto* value = runTransact(core, out, call, arguments);
    if (call.committed) {
        // An answer that cannot be kept is forgotten (the guard), never a failed request.
        try {
            settleKey(core, *key, claim.ticket,
                      storeAnswer(out, value, call.compact, kMcpRememberedAnswerBytes), nullptr);
            settle.settled = true;
        } catch (const std::exception&) {
        }
    }
    return value;
}

yyjson_mut_val* submitTransactJob(McpServerCore::State& core, Json& out, McpCall& call,
                                  yyjson_val* arguments, const std::string* key,
                                  const std::uint64_t ticket) {
    if (call.envelope == nullptr || call.envelope->message == nullptr)
        throw std::runtime_error("The request cannot be kept for an asynchronous job");
    auto* operations = member(arguments, "operations");
    if (!yyjson_is_arr(operations) || yyjson_arr_size(operations) == 0)
        throw InvalidInput("Expected operations");
    static_cast<void>(integer(member(arguments, "expectedRevision")));
    if (auto* response = member(arguments, "response")) {
        const auto text = string(response, 16);
        if (text != "full" && text != "summary")
            throw InvalidInput("response is \"full\" or \"summary\"");
    }
    if (auto* check = member(arguments, "compileCheck"))
        static_cast<void>(boolean(check));
    auto job = std::make_shared<McpTransactJob>();
    job->client = call.client;
    job->generation = call.generation;
    job->label = member(arguments, "label") ? string(member(arguments, "label")) : "Agent edit";
    job->operations = yyjson_arr_size(operations);
    job->progressToken = call.envelope->progressToken;
    job->message = call.envelope->message;
    job->arguments = arguments;
    job->submitted = Clock::now();
    job->progress = {.progress = 0.0,
                     .total = static_cast<double>(2 * job->operations + 2),
                     .message = "Queued",
                     .phase = "queued"};
    if (key != nullptr) {
        job->idempotencyKey = *key;
        settleKey(core, *key, ticket, nullptr, job);
    }
    std::size_t ahead = 0;
    McpJobEvent event;
    {
        const std::lock_guard lock(core.jobs.mutex);
        if (core.jobs.stopping)
            throw ToolFailure(McpToolErrorCode::Cancelled, "Bloom is closing; nothing was queued.");
        const auto unfinished = static_cast<std::size_t>(std::ranges::count_if(
            core.jobs.jobs, [](const auto& held) { return !finished(held->state); }));
        if (unfinished >= kMcpAsyncJobs) {
            const auto* live = core.liveHost();
            ToolFailure failure(
                McpToolErrorCode::JobsFull,
                grouped(unfinished) + " asynchronous transact jobs are queued or running and "
                    "this would make " + grouped(unfinished + 1) + "; at most " +
                    grouped(kMcpAsyncJobs) + " run at once (limit asyncJobs). Nothing was "
                    "queued; send it again after retryAfterMs, or wait for one with jobs.wait.");
            failure.limit = ToolFailure::Limit{
                .limit = McpLimit::AsyncJobs, .value = kMcpAsyncJobs, .actual = unfinished + 1};
            failure.retryAfterMs = core.retryAfterMs(
                unfinished + (live != nullptr ? live->queuedCommits() : 0U));
            throw failure;
        }
        job->number = core.jobs.next++;
        job->id = "transact-" + std::to_string(job->number);
        ahead = core.jobs.queue.size();
        core.jobs.queue.push_back(job);
        core.jobs.jobs.push_back(job);
        pruneJobs(core.jobs);
        if (!core.jobs.runner)
            core.jobs.runner.emplace([raw = &core] { runJobs(*raw); });
        event = eventOf(*job);
    }
    core.jobs.changed.notify_all();
    core.publishJob(event);
    call.summary = "async " + job->id + " \"" + job->label.substr(0, 64) + "\"";
    auto* value = submissionValue(core, out, *job, false);
    out.set(value, "queuedAhead", out.number(ahead));
    out.set(value, "baseRevision", out.number(call.session.snapshot().revision().value()));
    return value;
}

yyjson_mut_val* jobsWaitTool(McpServerCore::State& core, Json& out, yyjson_val* arguments,
                             const McpRequestControl& control) {
    members(arguments, {"job", "timeoutMs"}, {"job"});
    const auto id = jobArgument(arguments);
    auto wait = kDefaultWait;
    if (auto* timeout = member(arguments, "timeoutMs")) {
        const auto ms = integer(timeout);
        if (ms == 0 || ms > 86'400'000)
            throw InvalidInput("timeoutMs is 1 to 86,400,000 milliseconds");
        wait = std::chrono::milliseconds(ms);
    }
    if (core.liveHost() == nullptr)
        throw ToolFailure(McpToolErrorCode::NotLive,
                          "This server runs no asynchronous jobs: transact async:true runs in the "
                          "desktop application only.");
    auto& jobs = core.jobs;
    std::shared_ptr<McpTransactJob> job;
    {
        const std::lock_guard lock(jobs.mutex);
        job = findJob(jobs, id);
    }
    if (!job)
        throw ToolFailure(McpToolErrorCode::UnknownJob,
                          "Job " + id + " is not listed (it never existed, or finished more than " +
                              std::to_string(kMcpRetainedJobs) +
                              " jobs ago); jobs.list lists the jobs Bloom holds.");
    const auto deadline = Clock::now() + wait;
    std::uint64_t told = 0;
    bool timedOut = false;
    std::unique_lock lock(jobs.mutex);
    while (!finished(job->state)) {
        if (control.progress && job->serial != told) {
            told = job->serial;
            const auto progress = job->progress;
            lock.unlock();
            control.progress(progress);
            lock.lock();
            continue;
        }
        const bool changed = jobs.changed.wait_until(lock, control.stop, deadline, [&] {
            return finished(job->state) || (control.progress && job->serial != told);
        });
        if (control.stop.stop_requested())
            throw ToolFailure(McpToolErrorCode::Cancelled,
                              "The wait was cancelled; job " + id + " goes on.");
        if (!changed) {
            timedOut = true;
            break;
        }
    }
    auto* value = jobValue(out, *job, true);
    lock.unlock();
    out.set(value, "succeeded", out.boolean(true));
    out.set(value, "timedOut", out.boolean(timedOut));
    return value;
}

void appendTransactJobRows(McpServerCore::State& core, Json& out, yyjson_mut_val* list) {
    const std::lock_guard lock(core.jobs.mutex);
    for (const auto& job : core.jobs.jobs)
        out.append(list, jobValue(out, *job, false));
}

yyjson_mut_val* cancelTransactJob(McpServerCore::State& core, Json& out, const std::string& id) {
    auto& jobs = core.jobs;
    std::optional<McpJobEvent> event;
    std::shared_ptr<McpTransactJob> job;
    std::string message;
    {
        const std::lock_guard lock(jobs.mutex);
        job = findJob(jobs, id);
        if (!job)
            throw ToolFailure(McpToolErrorCode::UnknownJob,
                              "Job " + id + " is not listed; jobs.list lists the jobs Bloom holds.");
        if (finished(job->state)) {
            auto* value = diagnosticValue(out, uiRefusalCode(McpUiRefusal::InvalidArgument),
                                          "Job " + id + " has finished (" +
                                              std::string(jobStateName(job->state)) +
                                              "); there is nothing to cancel.");
            out.set(value, "job", jobValue(out, *job, false));
            return value;
        }
        job->cancellationRequested = true;
        job->stop.request_stop();
        if (job->state == McpJobState::Queued) {
            event = finishLocked(jobs, *job,
                                 failureAnswer(toolErrorCode(McpToolErrorCode::Cancelled),
                                               "The job was cancelled before it started; nothing "
                                               "was applied."),
                                 false);
            message = "Job " + id + " was cancelled before it started; nothing was applied.";
        } else {
            ++job->serial;
            event = eventOf(*job);
            message = job->phase == "publishing" || job->phase == "checking"
                          ? "Cancellation of job " + id +
                                " was requested, but its edit may already be publishing; an edit "
                                "that started publishing finishes. jobs.wait tells which."
                          : "Cancellation of job " + id +
                                " was requested; it stops before its edit publishes, and nothing "
                                "is applied. jobs.wait tells when it has stopped.";
        }
    }
    jobs.changed.notify_all();
    if (event && event->finished) {
        settleJobKey(core, *job, false, nullptr);
        event->summary = "transact " + job->id;
    }
    if (event)
        core.publishJob(*event);
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(true));
    out.set(value, "message", out.text(message));
    {
        const std::lock_guard lock(jobs.mutex);
        out.set(value, "job", jobValue(out, *job, false));
    }
    return value;
}

} // namespace bloom::mcp
