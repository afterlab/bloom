// bloom.ui.live-commit-preparation (#744, A2): through the real CompositionSession and
// LiveSessionAdapter, a live commit's operations run on its client's thread while the UI thread
// keeps turning, and the UI thread only publishes. An agent session's group (#746) still makes one
// undo step and the artist's own edit still splits it; the artist's edit during an agent's
// preparation lands at once and the agent's commit answers stale; shutdown during preparation
// withdraws it. #748: the agent's commit is measured for the project's size in its preparation.
#include "await_qt.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <bloom/commands/operations.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/live_session_adapter.hpp>
#include <bloom/ui/project_host.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;
using bloom::test_support::awaitQtCondition;

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Gate final {
    std::atomic_int entered = 0;
    std::atomic_bool released = false;
    std::mutex mutex;
    std::vector<std::thread::id> threads;
};

// Stays in apply() until released, as a heavy edit would, and records the thread it ran on.
class HeldRename final : public commands::Operation {
  public:
    HeldRename(std::string name, std::shared_ptr<Gate> gate)
        : name_(std::move(name)), gate_(std::move(gate)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "test.held-rename"; }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override {
        {
            const std::lock_guard lock(gate_->mutex);
            gate_->threads.push_back(std::this_thread::get_id());
        }
        ++gate_->entered;
        while (!gate_->released.load())
            std::this_thread::sleep_for(1ms);
        return commands::SetProjectName(name_).apply(draft);
    }

  private:
    std::string name_;
    std::shared_ptr<Gate> gate_;
};

struct Fixture final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost projectHost{scheduler};
    ui::CompositionSession session{*projectHost.liveDocumentAndStack().first,
                                   *projectHost.liveDocumentAndStack().second,
                                   projectHost.lowestCompositionId()};
    scripting::LiveSessionHost live;
    ui::LiveSessionAdapter adapter{live, session, projectHost};

    [[nodiscard]] commands::CommandStack& stack() const {
        return *const_cast<ui::ProjectHost&>(projectHost).liveDocumentAndStack().second;
    }
};

// Starts an agent commit on its own thread, as an MCP request worker does.
std::future<scripting::SessionCommandResult> agentCommit(Fixture& fixture, std::string name,
                                                         std::shared_ptr<Gate> gate) {
    return std::async(std::launch::async, [&fixture, name = std::move(name), gate] {
        auto lease = fixture.live.acquire({});
        if (!lease)
            return scripting::SessionCommandResult{};
        commands::Transaction transaction(name, lease.state().renderSource.snapshot.revision());
        transaction.setOrigin(commands::CommandOrigin::Agent);
        if (gate)
            transaction.emplace<HeldRename>(name, gate);
        else
            transaction.emplace<commands::SetProjectName>(name);
        return lease.session().execute(std::move(transaction));
    });
}

scripting::SessionCommandResult awaitResult(std::future<scripting::SessionCommandResult>& future,
                                            const char* name) {
    require(
        awaitQtCondition(name, [&] { return future.wait_for(0s) == std::future_status::ready; }),
        name);
    return future.get();
}

void uiThreadKeepsTurning() {
    Fixture fixture;
    auto gate = std::make_shared<Gate>();
    auto commit = agentCommit(fixture, "Held", gate);
    require(awaitQtCondition("the operation starts", [&] { return gate->entered.load() == 1; }),
            "the operation starts");
    // While the operation is applying, the UI thread turns its event loop freely.
    int ticks = 0;
    qint64 longestGap = 0;
    QElapsedTimer sinceTick;
    sinceTick.start();
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        longestGap = std::max(longestGap, sinceTick.restart());
        ++ticks;
    });
    timer.start(5);
    require(awaitQtCondition("the UI thread turns", [&] { return ticks >= 40; }),
            "the UI thread turns while the agent's edit is applied");
    timer.stop();
    require(longestGap < 50, "no event-loop turn waits on the agent's operation");
    require(fixture.session.snapshot().project().name() != "Held", "nothing published yet");
    gate->released = true;
    const auto result = awaitResult(commit, "the commit publishes");
    require(result.succeeded() && fixture.session.snapshot().project().name() == "Held",
            "the prepared commit publishes and the window adopts it");
    require(gate->threads.size() == 1 && gate->threads.front() != std::this_thread::get_id(),
            "the operation ran once, off the UI thread");
    require(fixture.session.undoLabel() == QStringLiteral("Held"),
            "it is one undo step with the transaction's label");
}

void agentGroupAndArtistSplit() {
    Fixture fixture;
    fixture.stack().beginGroup("Agent session", commands::CommandOrigin::Agent);
    auto first = agentCommit(fixture, "One", nullptr);
    require(awaitResult(first, "first agent commit").succeeded(), "the first agent commit lands");
    auto second = agentCommit(fixture, "Two", nullptr);
    require(awaitResult(second, "second agent commit").succeeded(),
            "the second agent commit lands");
    require(fixture.stack().size() == 1 && fixture.stack().groupStats().commits == 2,
            "an agent session's commits stay one undo step");
    // The artist's own edit splits the group and is never refused as stale.
    commands::Transaction artist("Artist", fixture.session.snapshot().revision());
    artist.emplace<commands::SetProjectName>("Artist");
    require(fixture.session.executeTransaction(std::move(artist)).succeeded(),
            "the artist's edit lands");
    auto third = agentCommit(fixture, "Three", nullptr);
    require(awaitResult(third, "third agent commit").succeeded(), "the third agent commit lands");
    const auto stats = fixture.stack().endGroup();
    require(stats.commits == 3 && stats.entries == 2 && stats.interruptions == 1,
            "the artist's edit split the agent's step");
    require(fixture.stack().historyLabels() ==
                std::vector<std::string>{"Agent session", "Artist", "Agent session"},
            "the steps keep their order");
}

void artistEditDuringPreparation() {
    Fixture fixture;
    auto gate = std::make_shared<Gate>();
    auto commit = agentCommit(fixture, "Agent", gate);
    require(awaitQtCondition("the operation starts", [&] { return gate->entered.load() == 1; }),
            "the operation starts");
    commands::Transaction artist("Artist", fixture.session.snapshot().revision());
    artist.emplace<commands::SetProjectName>("Artist");
    require(fixture.session.executeTransaction(std::move(artist)).succeeded(),
            "the artist's edit lands at once while the agent's is being prepared");
    gate->released = true;
    const auto result = awaitResult(commit, "the agent's commit answers");
    require(result.command && result.command->status == commands::CommandStatus::StaleRevision,
            "the agent's commit, based on the old revision, is refused as stale");
    require(fixture.session.snapshot().project().name() == "Artist" &&
                fixture.stack().historyLabels() == std::vector<std::string>{"Artist"},
            "only the artist's edit is in the history");
}

// #748: the agent's commit is measured for the project's size in its preparation, off the UI
// thread, through the mirror the adapter publishes; the artist's own edit is measured on the UI
// thread, from what it changed.
void capacityIsMeasuredInPreparation() {
    Fixture fixture;
    const auto original = fixture.stack().capacityCheck();
    require(original != nullptr, "the project has a capacity check");
    using Thread = commands::CommandStack::CapacityCheck::Thread;
    struct Calls final {
        std::mutex mutex;
        std::vector<std::pair<std::thread::id, Thread>> calls;
    };
    auto calls = std::make_shared<Calls>();
    auto counted = std::make_shared<commands::CommandStack::CapacityCheck>();
    counted->measure = [original, calls](const document::Snapshot& before,
                                         const document::Snapshot& candidate, const Thread thread) {
        {
            const std::lock_guard lock(calls->mutex);
            calls->calls.emplace_back(std::this_thread::get_id(), thread);
        }
        return original->measure(before, candidate, thread);
    };
    counted->published = original->published;
    fixture.stack().setCapacityCheck(counted);
    // The artist's edit; the mirror published after it carries the stack's check.
    commands::Transaction artist("Artist", fixture.session.snapshot().revision());
    artist.emplace<commands::SetProjectName>("Artist");
    require(fixture.session.executeTransaction(std::move(artist)).succeeded(),
            "the artist's edit applies");
    auto commit = agentCommit(fixture, "Agent", nullptr);
    const auto result = awaitResult(commit, "the agent's commit answers");
    require(result.succeeded() && fixture.session.snapshot().project().name() == "Agent",
            "the agent's commit publishes");
    const std::lock_guard lock(calls->mutex);
    require(calls->calls.size() == 2, "each edit is measured once");
    require(calls->calls[0].first == std::this_thread::get_id() &&
                calls->calls[0].second == Thread::Owner,
            "the artist's edit is measured on the UI thread");
    require(calls->calls[1].first != std::this_thread::get_id() &&
                calls->calls[1].second == Thread::Preparation,
            "the agent's commit is measured in its preparation, off the UI thread");
}

void shutdownDuringPreparation() {
    Fixture fixture;
    auto gate = std::make_shared<Gate>();
    const auto revision = fixture.session.snapshot().revision();
    auto commit = agentCommit(fixture, "Never", gate);
    require(awaitQtCondition("the operation starts", [&] { return gate->entered.load() == 1; }),
            "the operation starts");
    fixture.adapter.shutdown();
    gate->released = true;
    const auto result = awaitResult(commit, "the agent's commit answers");
    require(result.diagnostic && result.diagnostic->code == "bloom.live.shutting-down",
            "shutdown during preparation withdraws the commit");
    require(fixture.session.snapshot().revision() == revision, "nothing was published");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        uiThreadKeepsTurning();
        agentGroupAndArtistSplit();
        artistEditDuringPreparation();
        capacityIsMeasuredInPreparation();
        shutdownDuringPreparation();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "live commit preparation: off the UI thread, grouping, artist race and shutdown "
                 "passed\n";
    return 0;
}
