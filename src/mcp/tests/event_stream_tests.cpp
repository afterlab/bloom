// #747 (D2): change subscriptions and coalesced events, driven through McpServerCore on a live
// session whose owner thread the test runs (as transact_jobs_tests does), and on a headless one.
//
// events.subscribe gives the protocol session a subscription; McpServerCore::openEventStream
// delivers it as notifications/bloom/events, coalesced per interval into revision ranges with
// counts and filtered by kind and composition. A subscriber that falls behind the retained events
// is told resetRequired (overflow), a replaced project says so (project-replaced), a reconnect
// with a cursor (Last-Event-ID) resumes after it, and asynchronous transact jobs ride along. The
// events tool gains limit, filters and a summary form.

#include <bloom/commands/command_stack.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/session.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

std::uint64_t number(const std::string& text, const std::string& name) {
    const auto value = firstMatch(text, "\"" + name + R"re(":(\d+))re");
    return value.empty() ? 0 : std::stoull(value);
}

// The text of a call's structuredContent, unescaped enough to match plain members.
std::string plain(const std::string& answer) {
    std::string out;
    out.reserve(answer.size());
    for (std::size_t index = 0; index < answer.size(); ++index) {
        if (answer[index] == '\\' && index + 1 < answer.size() && answer[index + 1] == '"')
            continue;
        out.push_back(answer[index]);
    }
    return out;
}

bool eventually(const std::function<bool()>& ready, const std::chrono::seconds limit = 20s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

mcp::McpMessageLimits limitsRetaining(const std::size_t events) {
    auto limits = mcp::kMcpMessageLimits;
    limits.retainedCommandEvents = events;
    return limits;
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
    commands::CommandObserverId observer = 0;
    scripting::LiveSessionHost host;
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    std::atomic<int> nextId{1};
    std::mutex ownerMutex;
    std::atomic<bool> draining{true};
    std::atomic<bool> running{true};
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

    explicit Live(const mcp::McpMessageLimits limits = mcp::kMcpMessageLimits) {
        // The command stack's events reach the live event stream, as the desktop adapter does.
        observer = session->commandStack()->addObserver(
            [this](const commands::CommandEvent& event) { events->publish(event); });
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
            .limits = limits});
        protocol = open();
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
        session->commandStack()->removeObserver(observer);
        scheduler.beginShutdown();
    }
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;

    std::unique_ptr<mcp::McpProtocolSession> open() {
        auto opened = core->openSession();
        static_cast<void>(core->handle(*opened,
                                       R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                                       R"("protocolVersion":"2025-11-25","capabilities":{},)"
                                       R"("clientInfo":{"name":"events","version":"1"}}})",
                                       {}));
        static_cast<void>(
            core->handle(*opened, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", {}));
        return opened;
    }

    // Binds a new generation (File > Open), then lets the core follow it, as the pump does.
    void replaceProject() {
        {
            const std::lock_guard lock(ownerMutex);
            host.replace(
                {events, session->publicationCoordinator(), session->artifactCoordinator()},
                mirror());
        }
        core->followLiveSession();
    }

    std::string call(mcp::McpProtocolSession& on, const std::string& tool,
                     const std::string& arguments) {
        return core
            ->handle(on,
                     R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                         R"(,"method":"tools/call","params":{"name":")" + tool +
                         R"(","arguments":)" + arguments + "}}",
                     {})
            .response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return call(*protocol, tool, arguments);
    }
    std::uint64_t revision() { return number(call("query", R"({"kind":"project"})"), "revision"); }
    // One applied edit of composition 1 (a solid layer).
    std::string addSolid() {
        return call("transact", R"({"expectedRevision":)" + std::to_string(revision()) +
                                    R"(,"operations":[{"op":"bloom.layer.add-solid","args":{)"
                                    R"("composition":1,"name":"Solid","color":[1,0,0,1]}}]})");
    }
    // One applied project-wide edit (the project name).
    std::string rename(const std::string& name) {
        return call("transact", R"({"expectedRevision":)" + std::to_string(revision()) +
                                    R"(,"operations":[{"op":"bloom.project.set-name","args":{)"
                                    R"("name":")" +
                                    name + R"("}}]})");
    }
};

// The next notification within `limit`, skipping keep-alive wake-ups.
std::optional<mcp::McpEventNotification> nextOf(mcp::McpEventStream& stream,
                                                const std::chrono::milliseconds limit = 10s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (auto note = stream.next(std::min(until, std::chrono::steady_clock::now() + 500ms),
                                    std::stop_token{}))
            return note;
        if (stream.ended())
            return std::nullopt;
    }
    return std::nullopt;
}

// ---- Delivery, coalescing and filters

void coalescesPerIntervalWithRangesAndCounts() {
    Live server;
    expect(!server.core->openEventStream(*server.protocol, std::nullopt),
           "a session without a subscription has no stream");
    const auto subscribed =
        plain(server.call("events.subscribe", R"({"intervalMs":400,"kinds":["revision"]})"));
    expect(contains(subscribed, R"("succeeded":true)") &&
               contains(subscribed, R"("created":true)") &&
               contains(subscribed, R"("method":"notifications/bloom/events")") &&
               contains(subscribed, R"("intervalMs":400)"),
           "events.subscribe answers its subscription: " + subscribed);
    auto stream = server.core->openEventStream(*server.protocol, std::nullopt);
    expect(stream != nullptr, "a subscribed session opens a stream");
    if (!stream)
        return;
    // The first change is told at once (nothing was sent in the interval before it).
    const auto base = server.revision();
    expect(contains(server.addSolid(), R"("isError":false)"), "the first solid applies");
    const auto first = nextOf(*stream);
    expect(first && contains(first->message, R"("method":"notifications/bloom/events")") &&
               contains(first->message, R"("count":1)") &&
               contains(first->message, R"("kinds":{"revision":1})") &&
               contains(first->message, R"("origins":{"agent":1})") &&
               contains(first->message, R"("compositions":[{"count":1,"fromRevision":)") &&
               contains(first->message, R"("composition":1})") &&
               contains(first->message, R"("resetRequired":false)"),
           "the first edit is told with its range and counts: " +
               (first ? first->message : std::string("none")));
    if (!first)
        return;
    expect(number(first->message, "fromRevision") == base + 1 &&
               number(first->message, "sequence") == first->sequence,
           "its revision range and sequence: " + first->message);
    // A burst inside one interval is coalesced into one notification.
    for (int index = 0; index < 4; ++index)
        static_cast<void>(server.addSolid());
    static_cast<void>(server.rename("Coalesced"));
    const auto burst = nextOf(*stream);
    expect(burst && contains(burst->message, R"("count":5)") &&
               contains(burst->message, R"("project":{"count":1)") &&
               contains(burst->message, R"("compositions":[{"count":4)") &&
               number(burst->message, "fromSequence") == first->sequence + 1 &&
               number(burst->message, "fromRevision") == base + 2 &&
               number(burst->message, "toRevision") == base + 6,
           "five edits in one interval are one notification with one range: " +
               (burst ? burst->message : std::string("none")));
}

void filtersByKindAndComposition() {
    Live server;
    // Composition 2 never changes here; a project-wide edit still reaches its subscriber.
    static_cast<void>(server.call("events.subscribe",
                                  R"({"compositions":[2],"kinds":["revision"],"intervalMs":50})"));
    auto stream = server.core->openEventStream(*server.protocol, std::nullopt);
    static_cast<void>(server.addSolid());
    static_cast<void>(server.rename("Project wide"));
    const auto note = stream ? nextOf(*stream) : std::nullopt;
    expect(note && contains(note->message, R"("count":1)") &&
               contains(note->message, R"("project":{"count":1)") &&
               contains(note->message, R"("compositions":[])"),
           "a composition filter skips other compositions' edits and keeps project-wide ones: " +
               (note ? note->message : std::string("none")));
    // Compile failures only: an applied edit sends nothing.
    static_cast<void>(
        server.call("events.subscribe", R"({"kinds":["composition.compile-failed"]})"));
    static_cast<void>(server.rename("Unselected"));
    const auto quiet =
        stream ? stream->next(std::chrono::steady_clock::now() + 300ms, {}) : std::nullopt;
    expect(!quiet, "an unselected kind sends nothing");
    // Changing the filter in place reaches the open stream.
    const auto changed = plain(server.call("events.subscribe", R"({"kinds":["history"]})"));
    expect(contains(changed, R"("created":false)") && contains(changed, R"("kinds":["history"])"),
           "a second events.subscribe changes the subscription: " + changed);
    const auto revision = server.revision();
    static_cast<void>(server.call("history", R"({"action":"undo","expectedRevision":)" +
                                                 std::to_string(revision) + "}"));
    const auto history = stream ? nextOf(*stream) : std::nullopt;
    expect(history && contains(history->message, R"("kinds":{"history":1})"),
           "the new filter applies to the open stream: " +
               (history ? history->message : std::string("none")));
    const auto refused = plain(server.call("events.subscribe", R"({"kinds":["nonsense"]})"));
    expect(contains(refused, "is not an event kind"), "an unknown kind is refused: " + refused);
    const auto interval = plain(server.call("events.subscribe", R"({"intervalMs":10})"));
    expect(contains(interval, "intervalMs is 10"), "a too short interval is refused: " + interval);
}

// ---- Overflow, reconnect with a cursor, project replacement

void overflowTellsResetRequired() {
    // Eight retained events: a subscriber more than eight behind cannot be caught up.
    Live server(limitsRetaining(8));
    static_cast<void>(server.call("events.subscribe", R"({"intervalMs":50,"kinds":["revision"]})"));
    const auto cursor = number(server.call("events", R"({"sinceRevision":0})"), "sequence");
    for (int index = 0; index < 12; ++index)
        static_cast<void>(server.rename("Burst " + std::to_string(index)));
    // The stream opens behind: the twelve edits overflowed the eight retained.
    auto stream = server.core->openEventStream(*server.protocol, cursor);
    const auto note = stream ? nextOf(*stream) : std::nullopt;
    expect(note && contains(note->message, R"("resetRequired":true)") &&
               contains(note->message, R"("reason":"overflow")") &&
               contains(note->message, R"("count":0)") &&
               number(note->message, "fromSequence") == cursor + 1 && note->sequence == cursor + 24,
           "a subscriber behind the retained events is told to reset at the newest event: " +
               (note ? note->message : std::string("none")));
    // After the reset it continues normally.
    static_cast<void>(server.rename("After"));
    const auto after = stream ? nextOf(*stream) : std::nullopt;
    expect(after && contains(after->message, R"("resetRequired":false)") &&
               contains(after->message, R"("count":1)"),
           "the stream continues after the reset: " +
               (after ? after->message : std::string("none")));
    // The events summary for the same old cursor says the same.
    const auto summary =
        plain(server.call("events", R"({"sinceRevision":0,"afterSequence":)" +
                                        std::to_string(cursor) + R"(,"response":"summary"})"));
    expect(contains(summary, R"("resetRequired":true)") &&
               contains(summary, R"("reason":"overflow")"),
           "the events summary reports the overflow: " + summary);
}

void reconnectResumesAfterTheCursor() {
    Live server;
    static_cast<void>(server.call("events.subscribe", R"({"intervalMs":50,"kinds":["revision"]})"));
    auto first = server.core->openEventStream(*server.protocol, std::nullopt);
    static_cast<void>(server.rename("One"));
    const auto one = first ? nextOf(*first) : std::nullopt;
    expect(one.has_value(), "the first stream delivers");
    if (!one)
        return;
    // The client misses the next two (its connection dropped), then reconnects with its cursor.
    static_cast<void>(server.rename("Two"));
    static_cast<void>(server.rename("Three"));
    auto second = server.core->openEventStream(*server.protocol, one->sequence);
    expect(first->next(std::chrono::steady_clock::now() + 200ms, {}) == std::nullopt &&
               first->ended(),
           "a newer stream for the session ends the older one");
    const auto resumed = second ? nextOf(*second) : std::nullopt;
    expect(resumed && number(resumed->message, "fromSequence") == one->sequence + 1 &&
               contains(resumed->message, R"("count":2)") &&
               contains(resumed->message, R"("resetRequired":false)"),
           "a reconnect with Last-Event-ID resumes after it: " +
               (resumed ? resumed->message : std::string("none")));
    // Without a cursor a reconnect resumes after the last notification sent.
    static_cast<void>(server.rename("Four"));
    auto third = server.core->openEventStream(*server.protocol, std::nullopt);
    const auto next = third ? nextOf(*third) : std::nullopt;
    expect(next && resumed && number(next->message, "fromSequence") == resumed->sequence + 1 &&
               contains(next->message, R"("count":1)"),
           "a reconnect without a cursor resumes after the last notification sent: " +
               (next ? next->message : std::string("none")));
    // events.unsubscribe ends the stream; a GET then finds no subscription.
    const auto ended = plain(server.call("events.unsubscribe", "{}"));
    expect(contains(ended, R"("ended":true)"), "events.unsubscribe ends it: " + ended);
    expect(third && !third->next(std::chrono::steady_clock::now() + 2s, {}) && third->ended(),
           "the stream ends with the subscription");
    expect(!server.core->openEventStream(*server.protocol, std::nullopt),
           "no stream without a subscription");
}

void projectReplacementTellsResetRequired() {
    Live server;
    static_cast<void>(server.call("events.subscribe", R"({"intervalMs":50,"kinds":["revision"]})"));
    auto stream = server.core->openEventStream(*server.protocol, std::nullopt);
    static_cast<void>(server.rename("Before"));
    const auto before = stream ? nextOf(*stream) : std::nullopt;
    expect(before.has_value(), "an edit before the replacement is told");
    server.replaceProject();
    const auto replaced = stream ? nextOf(*stream) : std::nullopt;
    expect(replaced && contains(replaced->message, R"("resetRequired":true)") &&
               contains(replaced->message, R"("reason":"project-replaced")"),
           "a replaced project is told with resetRequired: " +
               (replaced ? replaced->message : std::string("none")));
    // The subscription survives the replacement and reports the new project's edits.
    static_cast<void>(server.rename("After"));
    const auto after = stream ? nextOf(*stream) : std::nullopt;
    expect(after && contains(after->message, R"("resetRequired":false)") &&
               contains(after->message, R"("count":1)"),
           "the new project's edits follow: " + (after ? after->message : std::string("none")));
}

// ---- Asynchronous transact jobs ride along

void carriesTransactJobs() {
    Live server;
    auto other = server.open();
    static_cast<void>(server.call("events.subscribe", R"({"intervalMs":50,"kinds":["job"]})"));
    auto stream = server.core->openEventStream(*server.protocol, std::nullopt);
    server.draining = false;
    const auto submitted = server.call(
        *other, "transact",
        R"({"expectedRevision":0,"async":true,"operations":[{"op":"bloom.project.set-name",)"
        R"("args":{"name":"Job"}}]})");
    const auto job = firstMatch(submitted, R"re("job":"(transact-\d+)")re");
    const auto queued = stream ? nextOf(*stream) : std::nullopt;
    expect(!job.empty() && queued && contains(queued->message, R"("job":")" + job) &&
               contains(queued->message, R"("mine":false)") &&
               contains(queued->message, R"("finished":false)"),
           "another client's queued job is told: " + submitted + " / " +
               (queued ? queued->message : std::string("none")));
    server.draining = true;
    std::optional<mcp::McpEventNotification> done;
    expect(eventually([&] {
               auto note = stream ? stream->next(std::chrono::steady_clock::now() + 200ms, {})
                                  : std::nullopt;
               if (note && contains(note->message, R"("finished":true)"))
                   done = note;
               return done.has_value();
           }),
           "the job's completion is told");
    expect(done && contains(done->message, R"("state":"succeeded")") &&
               contains(done->message, R"("revision":1)") &&
               contains(done->message, R"("count":0)"),
           "the finished job carries its state and revision, and the kinds filter leaves out "
           "the edit itself: " +
               (done ? done->message : std::string("none")));
}

// ---- The events tool: paging and the summary form

void eventsPagesAndSummarizes() {
    Live server;
    const auto start = number(server.call("events", R"({"sinceRevision":0})"), "sequence");
    // Each applied edit is two events: revision, then history (its undo history changed).
    for (int index = 0; index < 3; ++index)
        static_cast<void>(server.addSolid());
    static_cast<void>(server.rename("Paged"));
    const auto from = [&](const std::uint64_t after, const std::string& extra) {
        return plain(server.call("events", R"({"sinceRevision":0,"afterSequence":)" +
                                               std::to_string(after) + extra + "}"));
    };
    const auto page = from(start, R"(,"limit":2)");
    expect(contains(page, R"("truncated":true)") && number(page, "sequence") == start + 2 &&
               contains(page, R"("kind":"revision","sequence":)" + std::to_string(start + 1)) &&
               contains(page, R"("kind":"history","sequence":)" + std::to_string(start + 2)) &&
               !contains(page, R"("sequence":)" + std::to_string(start + 3) + "}"),
           "limit pages the events: " + page);
    const auto rest = from(start + 2, R"(,"limit":6)");
    expect(contains(rest, R"("truncated":false)") && number(rest, "sequence") == start + 8,
           "the next page continues from the cursor to the end: " + rest);
    const auto kinds = from(start, R"(,"limit":2,"kinds":["revision"])");
    expect(contains(kinds, R"("truncated":true)") && number(kinds, "sequence") == start + 3 &&
               !contains(kinds, R"("kind":"history")"),
           "a kinds filter pages the selected events: " + kinds);
    const auto filtered = from(start, R"(,"compositions":[2],"kinds":["revision"])");
    std::size_t listed = 0;
    for (auto at = filtered.find(R"("kind":"revision")"); at != std::string::npos;
         at = filtered.find(R"("kind":"revision")", at + 1))
        ++listed;
    // structuredContent and the text block each list it once.
    expect(listed == 2 && contains(filtered, R"("revision":4,"origin")"),
           "a composition filter keeps the project-wide edit only: " + filtered);
    const auto summary = from(start, R"(,"response":"summary","kinds":["revision"])");
    expect(contains(summary, R"("count":4)") && contains(summary, R"("kinds":{"revision":4})") &&
               contains(summary, R"("origins":{"agent":4})") &&
               contains(summary, R"("project":{"count":1,"fromRevision":4,"toRevision":4})") &&
               contains(summary, R"("compositions":[{"count":3,"fromRevision":1,"toRevision":3)") &&
               !contains(summary, R"("events":)") && number(summary, "sequence") == start + 8,
           "the summary form is ranges and counts: " + summary);
    const auto badLimit = plain(server.call("events", R"({"sinceRevision":0,"limit":0})"));
    expect(contains(badLimit, "limit is 0"), "a zero limit is refused: " + badLimit);
}

void initializeAdvertisesTheBounds() {
    Live server;
    auto session = server.core->openSession();
    const auto answer = server.core
                            ->handle(*session,
                                     R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                                     R"("protocolVersion":"2025-11-25","capabilities":{},)"
                                     R"("clientInfo":{"name":"limits","version":"1"}}})",
                                     {})
                            .response.value_or(std::string{});
    expect(contains(answer, R"("eventSubscriptions":true)") &&
               contains(answer, R"("eventsPerAnswer":1024)") &&
               contains(answer, R"("eventIntervalMinMs":50)") &&
               contains(answer, R"("eventIntervalMaxMs":60000)") &&
               contains(answer, R"("eventCompositions":64)") &&
               contains(answer, R"("eventStreamsPerClient":1)") &&
               contains(answer, R"("eventKeepAliveMs":15000)") &&
               contains(answer, R"("retainedEvents":1024)"),
           "initialize advertises the subscription bounds: " + answer);
}

// ---- The headless (stdio) core delivers too

void headlessDelivers() {
    auto created = scripting::Session::createNew();
    if (!created) {
        expect(false, "headless session");
        return;
    }
    const auto session = std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    runtime::TaskScheduler scheduler;
    {
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
        static_cast<void>(core.handle(*protocol,
                                      R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                                      R"("protocolVersion":"2025-11-25","capabilities":{},)"
                                      R"("clientInfo":{"name":"stdio","version":"1"}}})",
                                      {}));
        static_cast<void>(core.handle(
            *protocol, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", {}));
        static_cast<void>(core.handle(
            *protocol,
            R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"events.subscribe",)"
            R"("arguments":{"intervalMs":50,"kinds":["revision"]}}})",
            {}));
        auto stream = core.openEventStream(*protocol, std::nullopt);
        static_cast<void>(core.handle(
            *protocol,
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"transact",)"
            R"("arguments":{"expectedRevision":0,"operations":[{"op":"bloom.project.set-name",)"
            R"("args":{"name":"Stdio"}}]}}})",
            {}));
        const auto note = stream ? nextOf(*stream) : std::nullopt;
        expect(note && contains(note->message, R"("count":1)") &&
                   contains(note->message, R"("toRevision":1)"),
               "the headless core delivers the same notifications: " +
                   (note ? note->message : std::string("none")));
    }
    scheduler.beginShutdown();
}

} // namespace

int main() {
    coalescesPerIntervalWithRangesAndCounts();
    filtersByKindAndComposition();
    overflowTellsResetRequired();
    reconnectResumesAfterTheCursor();
    projectReplacementTellsResetRequired();
    carriesTransactJobs();
    eventsPagesAndSummarizes();
    initializeAdvertisesTheBounds();
    headlessDelivers();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "event stream tests passed\n";
    return 0;
}
