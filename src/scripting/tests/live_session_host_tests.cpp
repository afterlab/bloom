// bloom.scripting.live-session-host (ADR 0034, section 10): the shared live session marshal. FIFO
// with one commit per drain, the 16-commit bound, a commit that waits behind an open gesture and is
// withdrawn at its deadline, cancellation that withdraws a queued commit and never a started one,
// revocation with ProjectReplaced, and leases that keep reading after replacement but refuse to
// commit. #744 (A2): a transaction is prepared on its client's thread and only published by the
// owner: a slow operation never runs on the owner thread, cancellation, deadline and shutdown stop
// a commit during preparation, a revision race re-bases or refuses as stale, undo keeps its place
// behind a preparing commit, and the work bound refuses before queueing.
#include <bloom/commands/operations.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

// A headless session standing in for the desktop owner: the test thread plays the UI thread.
struct Owner final {
    std::shared_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    }();
    std::shared_ptr<scripting::EventStream> events = std::make_shared<scripting::EventStream>();
    commands::CommandObserverId observer = session->commandStack()->addObserver(
        [stream = events](const commands::CommandEvent& event) { stream->publish(event); });
    std::atomic_bool gesture = false;
    std::vector<std::string> applied;
    // The threads the owner published or applied on (#744): always the test thread.
    std::vector<std::thread::id> ownerThreads;
    scripting::LiveSessionHost host;

    Owner() { bind(); }
    ~Owner() { session->commandStack()->removeObserver(observer); }
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;

    scripting::LiveSessionState state() const {
        scripting::LiveSessionState state{.renderSource = session->renderSourceSnapshot(),
                                          .context = session->operationContext(),
                                          .viewerDisplay = {},
                                          .viewerView = {},
                                          .projectPath = std::nullopt,
                                          .readOnly = false};
        state.context.layerGeometry = {};
        return state;
    }
    void bind() {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     state());
    }
    scripting::LiveSessionCommitter committer() {
        return {.execute =
                    [this](commands::Transaction transaction) {
                        applied.emplace_back(transaction.label());
                        ownerThreads.push_back(std::this_thread::get_id());
                        auto result = session->execute(std::move(transaction));
                        host.publish(state());
                        return result;
                    },
                .publish =
                    [this](commands::PreparedTransaction& prepared) {
                        auto* stack = session->commandStack();
                        ownerThreads.push_back(std::this_thread::get_id());
                        if (stack->rebaseNeeded(prepared))
                            return scripting::LivePublication{std::nullopt, session->snapshot()};
                        applied.emplace_back(prepared.label());
                        auto command = stack->publish(std::move(prepared));
                        host.publish(state());
                        return scripting::LivePublication{
                            scripting::SessionCommandResult{
                                host::ProjectSessionCommandStatus::Completed, std::move(command),
                                std::nullopt},
                            std::nullopt};
                    },
                .history =
                    [this](const bool redo, const commands::CommandOrigin origin) {
                        auto result = redo ? session->redo(origin) : session->undo(origin);
                        host.publish(state());
                        return result;
                    },
                .gestureOpen = [this] { return gesture.load(); },
                .revision = [this] { return session->snapshot().revision(); },
                .viewerLevel = {}};
    }
    // Waits until the oldest queued commit's client has prepared it (#744).
    void waitPrepared() const {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (host.preparing() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    }
    // Drains until `predicate` holds, like the adapter's event-loop slices.
    template <typename Predicate> void drainUntil(Predicate predicate) {
        const auto committing = committer();
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            if (!host.drainOne(committing))
                std::this_thread::sleep_for(1ms);
        }
    }
};

commands::Transaction rename(const std::string& name, const document::Revision revision) {
    commands::Transaction transaction(name, revision);
    transaction.emplace<commands::SetProjectName>(name);
    return transaction;
}

void fifoOnePerDrain() {
    Owner owner;
    const auto committing = owner.committer();
    std::vector<std::future<scripting::SessionCommandResult>> results;
    for (int index = 0; index < 3; ++index) {
        results.push_back(std::async(std::launch::async, [&owner, index] {
            auto lease = owner.host.acquire({});
            std::this_thread::sleep_for(std::chrono::milliseconds(20 * index));
            return lease.session().execute(
                rename("Step " + std::to_string(index), document::Revision::fromRaw(0)));
        }));
    }
    while (owner.host.queuedCommits() < 3)
        std::this_thread::sleep_for(1ms);
    owner.waitPrepared();
    expect(owner.host.drainOne(committing) && owner.applied.size() == 1,
           "one drain applies exactly one commit");
    expect(owner.host.queuedCommits() == 2, "the others stay queued");
    owner.drainUntil([&] { return owner.applied.size() == 3; });
    expect(owner.applied == std::vector<std::string>{"Step 0", "Step 1", "Step 2"},
           "commits apply in FIFO order");
    expect(results[0].get().succeeded(), "the first commit applies");
    expect(!results[1].get().succeeded(), "a later commit with a stale revision is refused");
    static_cast<void>(results[2].get());
}

void boundedAtSixteen() {
    Owner owner;
    std::stop_source stop;
    std::vector<std::future<scripting::SessionCommandResult>> queued;
    for (std::size_t index = 0; index < scripting::kLiveCommitQueueCapacity; ++index) {
        queued.push_back(std::async(std::launch::async, [&owner, &stop] {
            auto lease = owner.host.acquire({.stop = stop.get_token()});
            return lease.session().execute(rename("Queued", document::Revision::fromRaw(0)));
        }));
    }
    while (owner.host.queuedCommits() < scripting::kLiveCommitQueueCapacity)
        std::this_thread::sleep_for(1ms);
    auto lease = owner.host.acquire({});
    const auto full = lease.session().execute(rename("Overflow", document::Revision::fromRaw(0)));
    expect(!full.succeeded() && full.diagnostic &&
               full.diagnostic->code ==
                   scripting::liveCommitDiagnosticCode(scripting::LiveCommitStatus::QueueFull),
           "the seventeenth commit is refused with queue-full");
    stop.request_stop();
    for (auto& result : queued) {
        const auto refused = result.get();
        expect(refused.diagnostic && refused.diagnostic->code == "bloom.live.cancelled",
               "cancellation withdraws every queued commit");
    }
    expect(owner.host.queuedCommits() == 0 && owner.applied.empty(), "nothing was applied");
}

void gestureWaitsThenDeadline() {
    Owner owner;
    owner.gesture = true;
    auto waiting = std::async(std::launch::async, [&owner] {
        auto lease = owner.host.acquire({});
        return lease.session().execute(rename("After drag", document::Revision::fromRaw(0)));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    // #744: preparation goes on during the gesture; only publication waits for it.
    owner.waitPrepared();
    const auto committing = owner.committer();
    expect(!owner.host.drainOne(committing) && owner.host.waitingForGesture(),
           "a commit never applies while a gesture is open");
    owner.gesture = false;
    owner.drainUntil([&] { return !owner.applied.empty(); });
    expect(waiting.get().succeeded(), "the commit applies once the gesture ends");

    owner.gesture = true;
    const auto deadline = std::chrono::steady_clock::now() + 150ms;
    auto expired = std::async(std::launch::async, [&owner, deadline] {
        auto lease = owner.host.acquire({.stop = {}, .deadline = deadline});
        return lease.session().execute(rename("Never", document::Revision::fromRaw(1)));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    owner.waitPrepared();
    expect(!owner.host.drainOne(committing), "still blocked by the gesture");
    const auto result = expired.get();
    expect(result.diagnostic && result.diagnostic->code == "bloom.live.gesture-open",
           "a commit held past its deadline by a gesture is withdrawn with gesture-open");
    expect(owner.applied.size() == 1 && owner.host.queuedCommits() == 0,
           "the withdrawn commit never applies");
}

void cancellationNeverInterruptsAStartedCommit() {
    Owner owner;
    std::stop_source stop;
    std::promise<void> entered;
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    auto started = std::async(std::launch::async, [&] {
        auto lease = owner.host.acquire({.stop = stop.get_token()});
        return lease.session().execute(rename("Atomic", document::Revision::fromRaw(0)));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    auto committing = owner.committer();
    auto plain = committing.publish;
    committing.publish = [&](commands::PreparedTransaction& prepared) {
        entered.set_value();
        releaseFuture.wait();
        return plain(prepared);
    };
    owner.waitPrepared();
    std::thread drain([&] { static_cast<void>(owner.host.drainOne(committing)); });
    entered.get_future().wait();
    stop.request_stop();
    std::this_thread::sleep_for(20ms);
    release.set_value();
    drain.join();
    expect(started.get().succeeded(), "a commit that started applying finishes atomically");
    expect(owner.session->snapshot().project().name() == "Atomic", "and it landed");
}

void revocationAndLeases() {
    Owner owner;
    auto reader = owner.host.acquire({});
    expect(static_cast<bool>(reader) &&
               reader.availability() == scripting::LiveSessionAvailability::Bound,
           "a lease is available while bound");
    const auto readGeneration = reader.generation();
    auto queued = std::async(std::launch::async, [&owner] {
        auto lease = owner.host.acquire({});
        return lease.session().execute(rename("Lost", document::Revision::fromRaw(0)));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    owner.host.revoke(scripting::LiveSessionAvailability::Replacing);
    const auto lost = queued.get();
    expect(lost.diagnostic && lost.diagnostic->code == "bloom.live.project-replaced",
           "revocation withdraws a queued commit with project-replaced");
    expect(!owner.host.acquire({}) && owner.host.acquire({}).availability() ==
                                          scripting::LiveSessionAvailability::Replacing,
           "no lease while a project is being replaced");
    owner.bind();
    expect(owner.host.generation() != readGeneration, "a new generation starts");
    expect(reader.revoked() && reader.state().renderSource.snapshot.project().name() == "Untitled",
           "an old lease keeps reading the state it took");
    const auto refused = reader.session().execute(rename("Old", document::Revision::fromRaw(0)));
    // The attached session sees its revoked binding and refuses before queueing anything.
    expect(!refused.succeeded() && !refused.command && owner.host.queuedCommits() == 0,
           "an old lease refuses commits");
    auto fresh = owner.host.acquire({});
    auto history = std::async(std::launch::async, [&fresh] {
        return fresh.history(false, document::Revision::fromRaw(7));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    owner.drainUntil([&] { return owner.host.queuedCommits() == 0; });
    const auto stale = history.get();
    expect(stale.command && stale.command->status == commands::CommandStatus::StaleRevision,
           "a revision-guarded undo is checked on the owner thread");
    expect(owner.host.events() != nullptr, "the bound generation publishes its event stream");
    owner.host.revoke(scripting::LiveSessionAvailability::ShuttingDown);
    expect(owner.host.availability() == scripting::LiveSessionAvailability::ShuttingDown,
           "shutdown is terminal");
    const auto after = fresh.session().execute(rename("Late", document::Revision::fromRaw(0)));
    expect(!after.succeeded() && owner.host.queuedCommits() == 0,
           "commits after shutdown are refused");
}

void subscriptionWakesTheOwner() {
    Owner owner;
    auto subscription = owner.host.subscribeCommits();
    const auto observed = subscription.serial();
    std::stop_source stop;
    auto commit = std::async(std::launch::async, [&] {
        auto lease = owner.host.acquire({.stop = stop.get_token()});
        return lease.session().execute(rename("Wake", document::Revision::fromRaw(0)));
    });
    const auto changed = subscription.waitForChange(observed, {});
    expect(changed.has_value() && *changed != observed, "a queued commit wakes the owner");
    owner.drainUntil([&] { return !owner.applied.empty(); });
    expect(commit.get().succeeded(), "and the owner applies it");
    subscription.close();
    expect(!subscription.waitForChange(subscription.serial(), {}).has_value(),
           "a closed subscription stops waiting");
}

// #744 (A2): an operation that stays in apply() until the test releases it, and records where it
// ran and what it saw.
struct Gate final {
    std::atomic_int entered = 0;
    std::atomic_bool released = false;
    std::mutex mutex;
    std::vector<std::thread::id> threads;
    std::vector<std::string> sawNames;
    void waitEntered(const int count) const {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (entered.load() < count && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    }
};

class SlowRename final : public commands::Operation {
  public:
    SlowRename(std::string name, std::shared_ptr<Gate> gate, const std::size_t units = 1)
        : name_(std::move(name)), gate_(std::move(gate)), units_(units) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "test.slow-rename"; }
    [[nodiscard]] std::size_t workUnits() const noexcept override { return units_; }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override {
        {
            const std::lock_guard lock(gate_->mutex);
            gate_->threads.push_back(std::this_thread::get_id());
            gate_->sawNames.push_back(std::as_const(draft).project().name());
        }
        ++gate_->entered;
        while (!gate_->released.load())
            std::this_thread::sleep_for(1ms);
        return commands::SetProjectName(name_).apply(draft);
    }

  private:
    std::string name_;
    std::shared_ptr<Gate> gate_;
    std::size_t units_;
};

commands::Transaction slow(const std::string& name, const std::shared_ptr<Gate>& gate,
                           const std::optional<document::Revision> revision) {
    commands::Transaction transaction(name, revision);
    transaction.setOrigin(commands::CommandOrigin::Agent);
    transaction.emplace<SlowRename>(name, gate);
    return transaction;
}

void slowOperationNeverRunsOnTheOwner() {
    Owner owner;
    const auto committing = owner.committer();
    auto gate = std::make_shared<Gate>();
    auto client = std::async(std::launch::async, [&owner, gate] {
        auto lease = owner.host.acquire({});
        return lease.session().execute(slow("Slow", gate, document::Revision::fromRaw(0)));
    });
    gate->waitEntered(1);
    // The owner keeps turning: nothing to publish while the client prepares, and no wait.
    const auto turn = std::chrono::steady_clock::now();
    for (int index = 0; index < 50; ++index)
        expect(!owner.host.drainOne(committing), "the owner has nothing to publish yet");
    expect(std::chrono::steady_clock::now() - turn < 1s, "the owner's drain never waits");
    expect(owner.host.preparing() && owner.applied.empty(), "the commit is still preparing");
    gate->released = true;
    owner.drainUntil([&] { return !owner.applied.empty(); });
    const auto result = client.get();
    expect(result.succeeded() && owner.session->snapshot().project().name() == "Slow",
           "the prepared commit publishes");
    expect(gate->threads.size() == 1 && gate->threads.front() != std::this_thread::get_id(),
           "the operation ran once, off the owner thread");
    expect(!owner.ownerThreads.empty() &&
               std::ranges::all_of(owner.ownerThreads,
                                   [](const auto id) { return id == std::this_thread::get_id(); }),
           "publication ran on the owner thread");
    expect(owner.session->commandStack()->undoLabel() == std::optional<std::string_view>("Slow"),
           "and it is one undo step with the transaction's label");
}

void cancelDeadlineAndShutdownDuringPreparation() {
    {
        Owner owner;
        auto gate = std::make_shared<Gate>();
        std::stop_source stop;
        auto client = std::async(std::launch::async, [&owner, gate, token = stop.get_token()] {
            auto lease = owner.host.acquire({.stop = token});
            return lease.session().execute(slow("Cancelled", gate, document::Revision::fromRaw(0)));
        });
        gate->waitEntered(1);
        stop.request_stop();
        gate->released = true;
        const auto result = client.get();
        expect(result.diagnostic && result.diagnostic->code == "bloom.live.cancelled",
               "cancel during preparation withdraws the commit");
        owner.drainUntil([&] { return owner.host.queuedCommits() == 0; });
        expect(owner.applied.empty() && owner.session->snapshot().revision().value() == 0,
               "nothing was published");
    }
    {
        Owner owner;
        auto gate = std::make_shared<Gate>();
        const auto deadline = std::chrono::steady_clock::now() + 100ms;
        auto client = std::async(std::launch::async, [&owner, gate, deadline] {
            auto lease = owner.host.acquire({.stop = {}, .deadline = deadline});
            return lease.session().execute(slow("Late", gate, document::Revision::fromRaw(0)));
        });
        gate->waitEntered(1);
        std::this_thread::sleep_for(150ms);
        gate->released = true;
        const auto result = client.get();
        expect(result.diagnostic && result.diagnostic->code == "bloom.live.deadline-exceeded",
               "a deadline that passes during preparation withdraws the commit");
        expect(owner.applied.empty() && owner.host.queuedCommits() == 0, "nothing was published");
    }
    {
        // Shutdown destroys the host while a client is still preparing against it.
        auto session =
            std::shared_ptr<scripting::Session>(scripting::Session::createNew().takeSession());
        auto host = std::make_unique<scripting::LiveSessionHost>();
        host->replace({std::make_shared<scripting::EventStream>(),
                       session->publicationCoordinator(), session->artifactCoordinator()},
                      {.renderSource = session->renderSourceSnapshot(),
                       .context = {},
                       .viewerDisplay = {},
                       .viewerView = {},
                       .projectPath = std::nullopt,
                       .readOnly = false});
        auto gate = std::make_shared<Gate>();
        auto lease = host->acquire({});
        auto client = std::async(std::launch::async, [&lease, gate] {
            return lease.session().execute(slow("Never", gate, document::Revision::fromRaw(0)));
        });
        gate->waitEntered(1);
        host.reset();
        gate->released = true;
        const auto result = client.get();
        expect(result.diagnostic && result.diagnostic->code == "bloom.live.shutting-down",
               "shutdown during preparation withdraws the commit");
        expect(session->snapshot().revision().value() == 0, "nothing was published");
    }
}

void revisionRaceRebasesOrRefuses() {
    {
        // The artist edits while the agent's commit prepares; the agent expected the old revision.
        Owner owner;
        auto gate = std::make_shared<Gate>();
        auto client = std::async(std::launch::async, [&owner, gate] {
            auto lease = owner.host.acquire({});
            return lease.session().execute(slow("Agent", gate, document::Revision::fromRaw(0)));
        });
        gate->waitEntered(1);
        expect(owner.session->execute(rename("Artist", document::Revision::fromRaw(0))).succeeded(),
               "the artist's edit lands at once");
        owner.host.publish(owner.state());
        gate->released = true;
        owner.drainUntil([&] { return owner.host.queuedCommits() == 0; });
        const auto result = client.get();
        expect(result.command && result.command->status == commands::CommandStatus::StaleRevision &&
                   result.command->expectedRevision == document::Revision::fromRaw(0),
               "the agent's commit is refused with the stale-revision status");
        expect(owner.session->snapshot().project().name() == "Artist" &&
                   owner.session->commandStack()->historyLabels() ==
                       std::vector<std::string>{"Artist"},
               "the artist's edit stands alone");
    }
    {
        // A transaction that expects no revision is prepared again on the artist's revision.
        Owner owner;
        auto gate = std::make_shared<Gate>();
        auto client = std::async(std::launch::async, [&owner, gate] {
            auto lease = owner.host.acquire({});
            return lease.session().execute(slow("Agent", gate, std::nullopt));
        });
        gate->waitEntered(1);
        expect(owner.session->execute(rename("Artist", document::Revision::fromRaw(0))).succeeded(),
               "the artist's edit lands at once");
        owner.host.publish(owner.state());
        gate->released = true;
        owner.drainUntil([&] { return owner.host.queuedCommits() == 0; });
        const auto result = client.get();
        expect(result.succeeded() && owner.session->snapshot().project().name() == "Agent",
               "the re-based commit publishes on the artist's revision");
        expect(gate->sawNames == std::vector<std::string>{"Untitled", "Artist"},
               "it was prepared again on the live snapshot");
        expect(std::ranges::none_of(gate->threads,
                                    [](const auto id) { return id == std::this_thread::get_id(); }),
               "both preparations ran off the owner thread");
        expect(owner.session->commandStack()->historyLabels() ==
                   std::vector<std::string>{"Artist", "Agent"},
               "the artist's edit and the agent's keep their order in the history");
    }
}

void undoKeepsItsPlaceBehindAPreparingCommit() {
    Owner owner;
    auto gate = std::make_shared<Gate>();
    auto lease = owner.host.acquire({});
    auto edit = std::async(std::launch::async, [&lease, gate] {
        return lease.session().execute(slow("Edit", gate, document::Revision::fromRaw(0)));
    });
    gate->waitEntered(1);
    auto undo = std::async(std::launch::async, [&owner] {
        auto second = owner.host.acquire({});
        return second.session().undo();
    });
    while (owner.host.queuedCommits() < 2)
        std::this_thread::sleep_for(1ms);
    const auto committing = owner.committer();
    expect(!owner.host.drainOne(committing), "the undo waits behind the preparing commit");
    gate->released = true;
    owner.drainUntil([&] { return owner.host.queuedCommits() == 0; });
    expect(edit.get().succeeded() && undo.get().succeeded(), "both apply");
    expect(owner.session->snapshot().project().name() == "Untitled" &&
               owner.session->commandStack()->redoLabel() ==
                   std::optional<std::string_view>("Edit"),
           "the undo undid the commit queued before it");
}

void workBoundRefusesBeforeQueueing() {
    Owner owner;
    auto gate = std::make_shared<Gate>();
    gate->released = true;
    commands::Transaction transaction("Huge", document::Revision::fromRaw(0));
    transaction.emplace<SlowRename>("Huge", gate, scripting::kLiveCommitWorkLimit);
    transaction.emplace<SlowRename>("Huge", gate, 1);
    expect(transaction.workUnits() == scripting::kLiveCommitWorkLimit + 1,
           "a transaction's work is its operations' sum");
    auto lease = owner.host.acquire({});
    const auto refusedResult = lease.session().execute(std::move(transaction));
    expect(refusedResult.diagnostic &&
               refusedResult.diagnostic->code ==
                   scripting::liveCommitDiagnosticCode(scripting::LiveCommitStatus::TooLarge) &&
               refusedResult.diagnostic->code == "bloom.live.commit-too-large",
           "a transaction over the bound is refused with commit-too-large");
    expect(owner.host.queuedCommits() == 0 && gate->entered.load() == 0,
           "nothing was queued or prepared");
    commands::Transaction fits("Fits", document::Revision::fromRaw(0));
    fits.emplace<SlowRename>("Fits", gate, scripting::kLiveCommitWorkLimit);
    auto accepted = std::async(
        std::launch::async, [&lease, &fits] { return lease.session().execute(std::move(fits)); });
    owner.drainUntil([&] { return !owner.applied.empty(); });
    expect(accepted.get().succeeded(), "a transaction at the bound applies");
}

} // namespace

int main() {
    try {
        fifoOnePerDrain();
        boundedAtSixteen();
        gestureWaitsThenDeadline();
        cancellationNeverInterruptsAStartedCommit();
        revocationAndLeases();
        subscriptionWakesTheOwner();
        slowOperationNeverRunsOnTheOwner();
        cancelDeadlineAndShutdownDuringPreparation();
        revisionRaceRebasesOrRefuses();
        undoKeepsItsPlaceBehindAPreparingCommit();
        workBoundRefusesBeforeQueueing();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "live session host: FIFO, bound, gesture, cancellation, revocation, leases "
                     "and off-owner preparation passed\n";
    return failures == 0 ? 0 : 1;
}
