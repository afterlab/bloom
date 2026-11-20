// #747: asynchronous transact (D1) and multi-parameter bulk keys (D3), driven through
// McpServerCore::handle() on a live session whose owner thread the test runs, and on a headless
// (stdio) session.
//
// D1: async:true answers with a job id at once; jobs.list lists the job, jobs.wait streams its
// progress and returns its result, jobs.cancel stops it until publication (a cancel racing the
// publication either stops it with nothing applied or lets it apply whole); an idempotencyKey
// answers a repeated request with the original result; a replaced project fails its pending jobs
// without applying them; a busy answer carries retryAfterMs.
// D3: bloom.animation.set-key-tracks keys many parameters in one operation, from inline tracks or
// one data file, and applies nothing when any track is invalid.

#include "unique_temp_dir.hpp"
#include <bloom/commands/command_stack.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

bool contains(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::string firstMatch(const std::string& text, const std::string& pattern) {
    std::smatch match;
    return std::regex_search(text, match, std::regex(pattern)) ? match[1].str() : std::string{};
}

std::string jobOf(const std::string& answer) {
    return firstMatch(answer, R"re("job":"(transact-\d+)")re");
}

// The text of a call's structuredContent, unescaped enough to match plain members.
std::string plain(std::string answer) {
    std::string out;
    out.reserve(answer.size());
    for (std::size_t index = 0; index < answer.size(); ++index) {
        if (answer[index] == '\\' && index + 1 < answer.size() && answer[index + 1] == '"')
            continue;
        out.push_back(answer[index]);
    }
    return out;
}

// Waits (bounded) until `ready` holds.
bool eventually(const std::function<bool()>& ready, const std::chrono::seconds limit = 20s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A desktop host: a live session whose owner thread publishes queued commits while `draining`.
struct Live final {
    std::shared_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    }();
    std::shared_ptr<scripting::EventStream> events = std::make_shared<scripting::EventStream>();
    scripting::LiveSessionHost host;
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    std::atomic<int> nextId{1};
    // The owner thread holds `ownerMutex` while it touches the session.
    std::mutex ownerMutex;
    std::atomic<bool> draining{true};
    std::atomic<bool> running{true};
    std::mutex eventsMutex;
    std::vector<mcp::McpJobEvent> jobEvents;
    // Called by the jobEvents listener with each event it receives, before it returns. Set before
    // the first job is submitted.
    std::function<void(const mcp::McpJobEvent&)> onJobEvent;
    std::thread owner;

    scripting::LiveSessionState mirror() const {
        return {.renderSource = session->renderSourceSnapshot(),
                .context = session->operationContext(),
                .viewerDisplay = {},
                .viewerView = {},
                .projectPath = std::nullopt,
                .readOnly = false};
    }

    scripting::LiveSessionCommitter committer() {
        return {.execute =
                    [this](commands::Transaction transaction) {
                        auto result = session->execute(std::move(transaction));
                        host.publish(mirror());
                        return result;
                    },
                .publish =
                    [this](commands::PreparedTransaction& prepared) {
                        auto* stack = session->commandStack();
                        if (stack->rebaseNeeded(prepared))
                            return scripting::LivePublication{std::nullopt, session->snapshot()};
                        auto command = stack->publish(std::move(prepared));
                        host.publish(mirror());
                        return scripting::LivePublication{
                            scripting::SessionCommandResult{
                                host::ProjectSessionCommandStatus::Completed, std::move(command),
                                std::nullopt},
                            std::nullopt};
                    },
                .history =
                    [this](const bool redo, const commands::CommandOrigin origin) {
                        auto result = redo ? session->redo(origin) : session->undo(origin);
                        host.publish(mirror());
                        return result;
                    },
                .gestureOpen = [] { return false; },
                .revision = [this] { return session->snapshot().revision(); },
                .viewerLevel = {}};
    }

    Live() {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     mirror());
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(host),
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = false},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .captureDirectory = {},
            .serverVersion = "test",
            .jobEvents = [this](const mcp::McpJobEvent& event) {
                {
                    const std::lock_guard lock(eventsMutex);
                    jobEvents.push_back(event);
                }
                if (onJobEvent)
                    onJobEvent(event);
            }});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"jobs","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
        owner = std::thread([this] {
            const auto commit = committer();
            while (running) {
                if (draining) {
                    const std::lock_guard lock(ownerMutex);
                    while (host.drainOne(commit)) {
                    }
                }
                std::this_thread::sleep_for(200us);
            }
        });
    }
    ~Live() {
        running = false;
        owner.join();
        core.reset();
        scheduler.beginShutdown();
    }
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;

    // Binds a new generation (File > Open), as the application does on the owner thread, then
    // lets the core follow it, as the server's pump does.
    void replaceProject() {
        {
            const std::lock_guard lock(ownerMutex);
            host.replace(
                {events, session->publicationCoordinator(), session->artifactCoordinator()},
                mirror());
        }
        core->followLiveSession();
    }

    std::string send(const std::string& message, const mcp::McpRequestControl& control = {}) {
        return core->handle(*protocol, message, control).response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments,
                     const mcp::McpRequestControl& control = {}) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                        R"(,"method":"tools/call","params":{"name":")" + tool +
                        R"(","arguments":)" + arguments + "}}",
                    control);
    }
    std::uint64_t revision() {
        const auto text = call("query", R"({"kind":"project"})");
        const auto value = firstMatch(text, R"re("revision":(\d+))re");
        return value.empty() ? 0 : std::stoull(value);
    }
    std::string state(const std::string& job) {
        return firstMatch(plain(call("jobs.wait", R"({"job":")" + job + R"(","timeoutMs":1})")),
                          R"re("state":"([a-z]+)")re");
    }
};

std::string rename(const std::uint64_t revision, const std::string& name,
                   const std::string& extra = {}) {
    return R"({"expectedRevision":)" + std::to_string(revision) + extra +
           R"(,"operations":[{"op":"bloom.project.set-name","args":{"name":")" + name + R"("}}]})";
}

// ---- D1

void asyncAnswersAtOnceAndReportsProgress() {
    Live server;
    // Nothing publishes while the owner is paused, so the job cannot finish before it is listed.
    server.draining = false;
    const auto submitted = server.call("transact", rename(0, "Async", R"(,"async":true)"));
    const auto job = jobOf(submitted);
    expect(contains(submitted, R"(\"async\":true)") && job == "transact-1" &&
               contains(submitted, R"(\"baseRevision\":0)"),
           "async:true answers with a job id at once: " + submitted);
    const auto listed = plain(server.call("jobs.list", "{}"));
    expect(contains(listed, R"("id":"transact-1","kind":"transact")") &&
               contains(listed, R"("name":"Agent edit")") &&
               (contains(listed, R"("state":"queued")") || contains(listed, R"("state":"running")")),
           "jobs.list lists the unfinished job: " + listed);

    // jobs.wait with a progress token streams the job's progress, then its result.
    std::mutex mutex;
    std::vector<std::string> messages;
    mcp::McpRequestControl control;
    control.progress = [&](const mcp::McpProgress& progress) {
        const std::lock_guard lock(mutex);
        messages.push_back(progress.message);
    };
    auto waited = std::async(std::launch::async, [&] {
        return server.call("jobs.wait", R"({"job":"transact-1","timeoutMs":20000})", control);
    });
    expect(eventually([&] {
               const std::lock_guard lock(mutex);
               return !messages.empty();
           }),
           "jobs.wait forwards the job's progress");
    server.draining = true;
    const auto result = plain(waited.get());
    expect(contains(result, R"("state":"succeeded")") && contains(result, R"("finished":true)") &&
               contains(result, R"("result":{)") && contains(result, R"("revision":1)") &&
               contains(result, R"("timedOut":false)"),
           "jobs.wait returns the finished job with its transact result: " + result);
    {
        const std::lock_guard lock(mutex);
        bool waiting = false;
        for (const auto& message : messages)
            waiting = waiting || contains(message, "Waiting") || contains(message, "Publishing");
        expect(waiting, "the progress names where the edit waits");
    }
    expect(server.revision() == 1, "the job applied once");
    {
        const std::lock_guard lock(server.eventsMutex);
        bool queued = false;
        bool finished = false;
        for (const auto& event : server.jobEvents) {
            queued = queued || (event.job == "transact-1" && event.state == mcp::McpJobState::Queued);
            finished = finished || (event.job == "transact-1" && event.finished &&
                                    event.state == mcp::McpJobState::Succeeded &&
                                    event.revision == 1 && !event.summary.empty());
        }
        expect(queued && finished, "every job step is published as a job event");
    }

    // A synchronous transact with a progress token reports its phases too.
    std::vector<std::string> phases;
    mcp::McpRequestControl sync;
    sync.progress = [&](const mcp::McpProgress& progress) { phases.push_back(progress.phase); };
    const auto applied = server.call("transact", rename(1, "Sync"), sync);
    // (Publication can finish before the request worker looks again, so the phases seen are
    // building and waiting at least.)
    expect(contains(applied, R"("isError":false)") &&
               std::ranges::find(phases, "building") != phases.end() &&
               std::ranges::find(phases, "waiting") != phases.end(),
           "a transact with a progress token reports its phases: " + applied);

    // A job whose transaction the document refuses is listed as failed, with the reason.
    const auto stale = jobOf(server.call("transact", rename(0, "Stale", R"(,"async":true)")));
    const auto refused = plain(server.call("jobs.wait", R"({"job":")" + stale + R"("})"));
    expect(contains(refused, R"("state":"failed")") && contains(refused, "stale") &&
               server.revision() == 2,
           "a stale job fails without applying: " + refused);
    const auto unknown = server.call("jobs.wait", R"({"job":"transact-999"})");
    expect(contains(unknown, "bloom.mcp.unknown-job"), "an unknown job is named: " + unknown);
}

// #750: a job's listeners have each of its events before jobs.wait answers that step. The
// listener asks jobs.wait about the job while it still holds the finished event, the interleaving
// that let a waiter return first: that wait must not see the job finished (it times out at once),
// and the wait made before the job ran returns only once the finished event is listed.
void waitAnswersAfterTheListeners() {
    Live server;
    server.draining = false;
    std::string duringDelivery;
    server.onJobEvent = [&](const mcp::McpJobEvent& event) {
        if (event.job == "transact-1" && event.finished)
            duringDelivery =
                plain(server.call("jobs.wait", R"({"job":"transact-1","timeoutMs":1})"));
    };
    const auto job = jobOf(server.call("transact", rename(0, "Listened", R"(,"async":true)")));
    expect(job == "transact-1", "the job is submitted: " + job);
    auto waited = std::async(std::launch::async, [&] {
        const auto result =
            plain(server.call("jobs.wait", R"({"job":"transact-1","timeoutMs":20000})"));
        const std::lock_guard lock(server.eventsMutex);
        const bool listed = std::ranges::any_of(server.jobEvents, [](const auto& event) {
            return event.job == "transact-1" && event.finished &&
                   event.state == mcp::McpJobState::Succeeded;
        });
        return std::pair{result, listed};
    });
    server.draining = true;
    const auto [result, listed] = waited.get();
    expect(contains(result, R"("state":"succeeded")") && contains(result, R"("timedOut":false)"),
           "jobs.wait returns the finished job: " + result);
    expect(listed, "the finished event is listed when jobs.wait returns");
    expect(contains(duringDelivery, R"("timedOut":true)"),
           "while the listener holds the finished event, jobs.wait does not answer it: " +
               duringDelivery);
}

void cancelRacesPublication() {
    Live server;
    // Cancelled while queued: nothing applies, and the job ends at once.
    server.draining = false;
    const auto blocker = jobOf(server.call("transact", rename(0, "First", R"(,"async":true)")));
    const auto queued = jobOf(server.call("transact", rename(0, "Second", R"(,"async":true)")));
    const auto cancelled = plain(server.call("jobs.cancel", R"({"job":")" + queued + R"("})"));
    expect(contains(cancelled, "cancelled before it started") &&
               contains(cancelled, R"("state":"cancelled")"),
           "a queued job is cancelled at once: " + cancelled);
    const auto again = plain(server.call("jobs.cancel", R"({"job":")" + queued + R"("})"));
    expect(contains(again, "bloom.mcp.invalid-argument") && contains(again, "has finished"),
           "a finished job cannot be cancelled again: " + again);
    // Cancelled while its commit waits for the owner: withdrawn, nothing applied.
    expect(eventually([&] {
               const auto row = plain(server.call("jobs.list", "{}"));
               return contains(row, R"("phase":"prepared")");
           }),
           "the first job's commit waits for the owner");
    const auto stopped = plain(server.call("jobs.cancel", R"({"job":")" + blocker + R"("})"));
    expect(contains(stopped, "Cancellation of job"), "a running job's cancellation is requested");
    const auto ended = plain(server.call("jobs.wait", R"({"job":")" + blocker + R"("})"));
    expect(contains(ended, R"("state":"cancelled")") && contains(ended, "bloom.mcp.cancelled"),
           "a job cancelled before publication applies nothing: " + ended);
    server.draining = true;
    expect(server.revision() == 0, "neither cancelled job applied");

    // The race: a cancel sent while the owner publishes either stops the job (nothing applied)
    // or comes too late (the edit applies whole). Never anything in between.
    std::uint64_t revision = 0;
    int stoppedCount = 0;
    int appliedCount = 0;
    for (int round = 0; round < 40; ++round) {
        const auto job = jobOf(server.call(
            "transact", rename(revision, "Race " + std::to_string(round), R"(,"async":true)")));
        std::this_thread::sleep_for(std::chrono::microseconds(round * 25));
        static_cast<void>(server.call("jobs.cancel", R"({"job":")" + job + R"("})"));
        const auto result = plain(server.call("jobs.wait", R"({"job":")" + job + R"("})"));
        const auto now = server.revision();
        if (contains(result, R"("state":"cancelled")")) {
            ++stoppedCount;
            expect(now == revision && contains(result, "bloom.mcp.cancelled"),
                   "a cancelled job applied nothing: " + result);
        } else {
            ++appliedCount;
            expect(contains(result, R"("state":"succeeded")") && now == revision + 1 &&
                       contains(result, R"("revision":)" + std::to_string(revision + 1)),
                   "a job the cancel came too late for applied whole: " + result);
            revision = now;
        }
    }
    expect(stoppedCount + appliedCount == 40, "every raced job ended cancelled or applied");
    std::cout << "cancel race: " << stoppedCount << " stopped, " << appliedCount << " applied\n";
}

void duplicateKeys() {
    Live server;
    // A synchronous edit, then the same request again: the original answer, applied once.
    const auto first = server.call("transact", rename(0, "Once", R"(,"idempotencyKey":"key-1")"));
    const auto repeat = server.call("transact", rename(0, "Once", R"(,"idempotencyKey":"key-1")"));
    expect(contains(first, R"(\"revision\":1)") && contains(repeat, R"(\"revision\":1)") &&
               contains(repeat, R"(\"idempotentReplay\":true)") &&
               contains(repeat, R"("isError":false)") && server.revision() == 1,
           "a repeated key answers the original result without applying again: " + repeat);
    const auto conflict =
        server.call("transact", rename(1, "Other", R"(,"idempotencyKey":"key-1")"));
    expect(contains(conflict, "bloom.mcp.idempotency-conflict") && server.revision() == 1,
           "the same key with other arguments is refused: " + conflict);

    // A refused edit (stale) reached the document, so it is remembered as refused.
    const auto stale = server.call("transact", rename(0, "Stale", R"(,"idempotencyKey":"key-2")"));
    const auto staleAgain =
        server.call("transact", rename(0, "Stale", R"(,"idempotencyKey":"key-2")"));
    expect(contains(stale, R"("isError":true)") && contains(staleAgain, R"("isError":true)") &&
               contains(staleAgain, R"(\"idempotentReplay\":true)"),
           "a refused edit is answered again as refused: " + staleAgain);

    // Two identical requests at once: one applies, the other waits for it and answers its result.
    server.draining = false;
    auto one = std::async(std::launch::async, [&] {
        return server.call("transact", rename(1, "Twice", R"(,"idempotencyKey":"key-3")"));
    });
    expect(eventually([&] { return server.host.queuedCommits() == 1; }),
           "the first identical request is queued");
    auto two = std::async(std::launch::async, [&] {
        return server.call("transact", rename(1, "Twice", R"(,"idempotencyKey":"key-3")"));
    });
    std::this_thread::sleep_for(20ms);
    expect(server.host.queuedCommits() == 1, "the duplicate waits instead of queueing a commit");
    server.draining = true;
    const auto a = one.get();
    const auto b = two.get();
    expect(contains(a, R"(\"revision\":2)") && contains(b, R"(\"revision\":2)") &&
               contains(b, R"(\"idempotentReplay\":true)") && server.revision() == 2,
           "concurrent duplicates apply once: " + b);

    // Asynchronous: the duplicate names the same job.
    server.draining = false;
    const auto job = server.call("transact",
                                 rename(2, "Job", R"(,"async":true,"idempotencyKey":"key-4")"));
    const auto jobAgain = server.call(
        "transact", rename(2, "Job", R"(,"async":true,"idempotencyKey":"key-4")"));
    expect(!jobOf(job).empty() && jobOf(job) == jobOf(jobAgain) &&
               contains(jobAgain, R"(\"idempotentReplay\":true)"),
           "a repeated async key names the same job: " + jobAgain);
    // A job cancelled before it applied is forgotten, so the retry runs as a new job.
    static_cast<void>(server.call("jobs.cancel", R"({"job":")" + jobOf(job) + R"("})"));
    static_cast<void>(server.call("jobs.wait", R"({"job":")" + jobOf(job) + R"("})"));
    server.draining = true;
    const auto retried = server.call(
        "transact", rename(2, "Job", R"(,"async":true,"idempotencyKey":"key-4")"));
    expect(!jobOf(retried).empty() && jobOf(retried) != jobOf(job) &&
               !contains(retried, "idempotentReplay"),
           "a cancelled job's key is forgotten: " + retried);
    const auto done = plain(server.call("jobs.wait", R"({"job":")" + jobOf(retried) + R"("})"));
    const auto replayed = plain(server.call(
        "transact", rename(2, "Job", R"(,"async":true,"idempotencyKey":"key-4")")));
    expect(contains(done, R"("state":"succeeded")") && contains(replayed, R"("idempotentReplay":true)") &&
               contains(replayed, R"("result":{)") && server.revision() == 3,
           "a finished job's key answers the job with its result: " + replayed);
    const auto badKey = server.call("transact", rename(3, "Bad", R"(,"idempotencyKey":"a b")"));
    expect(contains(badKey, "-32602") && contains(badKey, "idempotencyKey"),
           "a key with a space is refused: " + badKey);
}

void replacedWhilePending() {
    Live server;
    server.draining = false;
    // The first job's commit waits for the owner; the second waits for the runner.
    const auto running = jobOf(server.call("transact", rename(0, "Old A", R"(,"async":true)")));
    const auto queued = jobOf(server.call("transact", rename(0, "Old B", R"(,"async":true)")));
    expect(eventually([&] {
               return contains(plain(server.call("jobs.list", "{}")), R"("phase":"prepared")");
           }),
           "the first job waits for publication");
    server.replaceProject();
    const auto queuedResult = plain(server.call("jobs.wait", R"({"job":")" + queued + R"("})"));
    expect(contains(queuedResult, R"("state":"failed")") &&
               contains(queuedResult, "bloom.mcp.project-replaced"),
           "a queued job of a replaced project fails: " + queuedResult);
    server.draining = true;
    const auto runningResult = plain(server.call("jobs.wait", R"({"job":")" + running + R"("})"));
    expect(contains(runningResult, R"("state":"failed")") &&
               contains(runningResult, "bloom.mcp.project-replaced"),
           "a job whose commit was waiting fails with the replacement: " + runningResult);
    expect(server.revision() == 0, "neither job applied to the new project");
    // The new project takes new jobs.
    const auto fresh = jobOf(server.call("transact", rename(0, "New", R"(,"async":true)")));
    const auto freshResult = plain(server.call("jobs.wait", R"({"job":")" + fresh + R"("})"));
    expect(contains(freshResult, R"("state":"succeeded")") && server.revision() == 1,
           "a job sent after the replacement applies: " + freshResult);
}

void busyAnswers() {
    Live server;
    auto fresh = server.core->openSession();
    const auto advertised = server.core
                                ->handle(*fresh,
                                         R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
                                         R"({"protocolVersion":"2025-11-25","capabilities":{},)"
                                         R"("clientInfo":{"name":"limits","version":"1"}}})",
                                         {})
                                .response.value_or("");
    expect(contains(advertised, R"("asyncJobs":16)") && contains(advertised, R"("retainedJobs":64)") &&
               contains(advertised, R"("idempotencyWindowMs":600000)") &&
               contains(advertised, R"("idempotencyKeys":1024)") &&
               contains(advertised, R"("asyncTransact":true)"),
           "the desktop host advertises the job and key bounds: " + advertised);

    expect(server.core->retryAfterMs(0) >= 100 && server.core->retryAfterMs(100000) == 30000 &&
               server.core->retryAfterMs(4) >= server.core->retryAfterMs(1),
           "retry advice grows with the queue, between 100 ms and the commit deadline");
    // Sixteen jobs fill the job bound; the seventeenth is refused with retry advice.
    server.draining = false;
    std::vector<std::string> jobs;
    for (int index = 0; index < 16; ++index)
        jobs.push_back(jobOf(server.call("transact", rename(0, "Full", R"(,"async":true)"))));
    const auto full = plain(server.call("transact", rename(0, "Full", R"(,"async":true)")));
    expect(contains(full, "bloom.mcp.jobs-full") &&
               contains(full, R"("limit":{"name":"asyncJobs","value":16,"actual":17,)") &&
               !firstMatch(full, R"re("retryAfterMs":(\d+))re").empty(),
           "a seventeenth job is refused by name with retryAfterMs: " + full);
    for (const auto& job : jobs)
        static_cast<void>(server.call("jobs.cancel", R"({"job":")" + job + R"("})"));
    server.draining = true;
    for (const auto& job : jobs)
        static_cast<void>(server.call("jobs.wait", R"({"job":")" + job + R"("})"));
}

void stdioHasNoJobs() {
    auto created = scripting::Session::createNew();
    if (!created)
        throw std::runtime_error("session");
    auto session = std::move(created).takeSession();
    runtime::TaskScheduler scheduler;
    mcp::McpServerCore core(mcp::McpCoreDependencies{
        .session = std::ref(*session),
        .exports = {.run =
                        [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                            work(nullptr, 1);
                            return true;
                        },
                    .callerIsOwner = true},
        .scheduler = scheduler,
        .gpuProvider = nullptr,
        .captureDirectory = {},
        .serverVersion = "test"});
    auto protocol = core.openSession();
    const auto send = [&](const std::string& message) {
        return core.handle(*protocol, message, {}).response.value_or("");
    };
    static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                           R"("protocolVersion":"2025-11-25","capabilities":{},)"
                           R"("clientInfo":{"name":"stdio","version":"1"}}})"));
    static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    const auto call = [&](const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":")" + tool +
                    R"(","arguments":)" + arguments + "}}");
    };
    const auto async = call("transact", rename(0, "Async", R"(,"async":true)"));
    expect(contains(async, "bloom.mcp.not-live"), "stdio refuses async:true: " + async);
    const auto keyed = call("transact", rename(0, "Keyed", R"(,"idempotencyKey":"stdio-1")"));
    const auto keyedAgain = call("transact", rename(0, "Keyed", R"(,"idempotencyKey":"stdio-1")"));
    expect(contains(keyed, R"(\"revision\":1)") && contains(keyedAgain, "idempotentReplay"),
           "stdio remembers idempotency keys too: " + keyedAgain);
    const auto wait = call("jobs.wait", R"({"job":"transact-1"})");
    expect(contains(wait, "bloom.mcp.not-live"), "stdio has no jobs to wait for: " + wait);
    scheduler.beginShutdown();
}

// ---- D3

struct Headless final {
    std::unique_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::move(created).takeSession();
    }();
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    int nextId = 1;

    Headless() {
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(*session),
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = true},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .captureDirectory = {},
            .serverVersion = "test"});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"keys","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    ~Headless() {
        core.reset();
        scheduler.beginShutdown();
    }
    Headless(const Headless&) = delete;
    Headless& operator=(const Headless&) = delete;

    std::string send(const std::string& message) {
        return core->handle(*protocol, message, {}).response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    arguments + "}}");
    }
    std::string revision() {
        return firstMatch(call("query", R"({"kind":"project"})"), R"re("revision":(\d+))re");
    }
    std::string transact(const std::string& operations) {
        return call("transact",
                    R"({"expectedRevision":)" + revision() + R"(,"operations":[)" + operations + "]}");
    }
};

std::size_t count(const std::string& text, const std::string_view part) {
    std::size_t found = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1))
        ++found;
    return found;
}

void keyTracks(const std::filesystem::path& folder) {
    Headless server;
    for (const auto* name : {"A", "B"}) {
        const auto added = server.transact(
            R"({"op":"bloom.layer.add-solid","args":{"composition":1,"name":")" + std::string(name) +
            R"(","color":[1,0,0,1]}})");
        expect(contains(added, R"("isError":false)"), "a solid applies: " + added);
    }
    // Three parameters of two layers in one operation, by name and role.
    const auto tracks =
        R"({"op":"bloom.animation.set-key-tracks","args":{"composition":1,"tracks":[)"
        R"({"parameter":{"layer":"A","role":"opacity"},"times":[0,12],"values":[0.0,1.0],)"
        R"("interpolations":["hold"]},)"
        R"({"parameter":{"layer":"B","role":"opacity"},"times":[0,6,12],"values":[1.0,0.5,0.0]},)"
        R"({"parameter":{"layer":"A","role":"position"},"times":[[0,1],[1,2]],)"
        R"("values":[[0,0],[100,50]],"replace":true}]}})";
    const auto before = server.revision();
    const auto applied = server.transact(tracks);
    expect(contains(applied, R"("isError":false)") &&
               count(applied, R"(\"name\":\"keyframe\")") >= 7 &&
               std::stoull(server.revision()) == std::stoull(before) + 1,
           "set-key-tracks keys three parameters in one undo step: " + applied);

    // One valid track and one whose parameter does not exist: the transaction is refused and the
    // valid track applies nothing either.
    const auto mixedBefore = server.revision();
    const auto mixed = server.transact(
        R"({"op":"bloom.animation.set-key-tracks","args":{"composition":1,"tracks":[)"
        R"({"parameter":{"layer":"A","role":"opacity"},"times":[24],"values":[0.25]},)"
        R"({"parameter":999999,"times":[0],"values":[1.0]}]}})");
    expect(contains(mixed, R"("isError":true)") && contains(mixed, "Track 1") &&
               server.revision() == mixedBefore,
           "an invalid track refuses the whole operation and nothing applies: " + mixed);
    // A track whose columns differ is refused before anything runs, naming it.
    const auto uneven = server.transact(
        R"({"op":"bloom.animation.set-key-tracks","args":{"composition":1,"tracks":[)"
        R"({"parameter":{"layer":"A","role":"opacity"},"times":[30],"values":[0.5]},)"
        R"({"parameter":{"layer":"B","role":"opacity"},"times":[0,6],"values":[1.0]}]}})");
    expect(contains(uneven, R"("isError":true)") && contains(uneven, "tracks[1] values has 1") &&
               server.revision() == mixedBefore,
           "uneven columns are refused by track: " + uneven);
    // A value of the wrong width in one track: nothing applies.
    const auto width = server.transact(
        R"({"op":"bloom.animation.set-key-tracks","args":{"composition":1,"tracks":[)"
        R"({"parameter":{"layer":"B","role":"opacity"},"times":[40],"values":[0.5]},)"
        R"({"parameter":{"layer":"A","role":"opacity"},"times":[40],"values":[[1,2,3]]}]}})");
    expect(contains(width, R"("isError":true)") && server.revision() == mixedBefore,
           "a value that does not fit refuses every track: " + width);
    // Two tracks for one parameter are refused.
    const auto twice = server.transact(
        R"({"op":"bloom.animation.set-key-tracks","args":{"composition":1,"tracks":[)"
        R"({"parameter":{"layer":"A","role":"opacity"},"times":[1],"values":[0.5]},)"
        R"({"parameter":{"layer":"A","role":"opacity"},"times":[2],"values":[0.5]}]}})");
    expect(contains(twice, "already keys") && server.revision() == mixedBefore,
           "one parameter has at most one track: " + twice);

    // Every track from one data file. replace returns each parameter to a constant, so making it
    // animated again keys its first time once more: 4 + 1 and 2 + 1 keyframe outputs.
    std::filesystem::create_directories(folder);
    const auto file = folder / "tracks.json";
    std::ofstream(file) << R"([{"parameter":{"layer":"A","role":"opacity"},"times":[0,1,2,3],)"
                           R"("values":[0,0.25,0.5,0.75],"replace":true},)"
                           R"({"parameter":{"layer":"B","role":"opacity"},"times":[0,1],)"
                           R"("values":[1,0],"replace":true}])";
    const auto fromFile = server.call(
        "transact", R"({"expectedRevision":)" + server.revision() +
                        R"(,"response":"summary","operations":[{"op":"bloom.animation.set-key-tracks",)"
                        R"("args":{"composition":1,"tracks":{"fromFile":")" +
                        file.generic_string() + R"("}}}]})");
    expect(contains(fromFile, R"("isError":false)") && contains(fromFile, R"(\"keyframe\":8)"),
           "one data file carries every track: " + fromFile);

    // The work bound counts every track's keys.
    Live live;
    std::ofstream big(folder / "big-tracks.json");
    big << R"([{"parameter":1,"times":[)";
    for (int key = 0; key < 500'001; ++key)
        big << (key ? "," : "") << key;
    big << R"(],"values":[)";
    for (int key = 0; key < 500'001; ++key)
        big << (key ? ",0" : "0");
    big << "]}]";
    big.close();
    const auto bigTracks = R"({"op":"bloom.animation.set-key-tracks","args":{"composition":1,)"
                           R"("tracks":{"fromFile":")" +
                           (folder / "big-tracks.json").generic_string() + R"("}}})";
    const auto tooLarge = live.call("transact", R"({"expectedRevision":0,"operations":[)" +
                                                    bigTracks + "," + bigTracks + "]}");
    expect(contains(tooLarge, "bloom.mcp.commit-too-large") &&
               contains(tooLarge, R"("actual":1000002)"),
           "set-key-tracks counts each key toward the commit bound: " + tooLarge.substr(0, 600));
}

} // namespace

int main() {
    const auto folder = bloom::test_support::processTempDirectory() / "bloom-transact-jobs-test";
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    try {
        asyncAnswersAtOnceAndReportsProgress();
        waitAnswersAfterTheListeners();
        cancelRacesPublication();
        duplicateKeys();
        replacedWhilePending();
        busyAnswers();
        stdioHasNoJobs();
        keyTracks(folder);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        failures = failures == 0 ? 1 : failures;
    }
    std::filesystem::remove_all(folder, ignored);
    if (failures == 0)
        std::cout << "transact jobs passed\n";
    return failures == 0 ? 0 : 1;
}
