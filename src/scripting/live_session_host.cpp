// The live GUI session for off-thread clients (ADR 0034, section 4): one mirror, one bounded commit
// marshal, one generation counter and one revocation rule, shared by the Python console and MCP.
#include <bloom/scripting/live_session_host.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>

namespace bloom::scripting {
namespace {

using Clock = std::chrono::steady_clock;
using Apply = std::function<SessionCommandResult(const LiveSessionCommitter&)>;

SessionCommandResult refused(const LiveCommitStatus status) {
    SessionCommandResult result;
    result.status = host::ProjectSessionCommandStatus::InvalidSession;
    std::string message;
    switch (status) {
    case LiveCommitStatus::Completed:
        break;
    case LiveCommitStatus::QueueFull:
        message = "The live commit queue is full; nothing was queued";
        break;
    case LiveCommitStatus::ProjectReplaced:
        message = "The project was replaced before the commit applied; nothing was applied";
        break;
    case LiveCommitStatus::Cancelled:
        message = "The commit was cancelled before it applied; nothing was applied";
        break;
    case LiveCommitStatus::DeadlineExceeded:
        message = "The commit waited past its deadline; nothing was applied";
        break;
    case LiveCommitStatus::GestureOpen:
        message = "An interactive edit stayed open past the commit's deadline; nothing was applied";
        break;
    case LiveCommitStatus::ShuttingDown:
        message = "Bloom is shutting down; nothing was applied";
        break;
    case LiveCommitStatus::TooLarge:
        message = "The transaction carries more work than one live commit takes (" +
                  std::to_string(kLiveCommitWorkLimit) +
                  " units: one per operation, or one per key, keyframe or edit an operation "
                  "lists); split it into several transactions. Nothing was queued";
        break;
    }
    result.diagnostic = OperationDiagnostic{std::string(liveCommitDiagnosticCode(status)), "", "",
                                            std::move(message)};
    return result;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Commit subscription

namespace {
struct CommitWake {
    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;
    std::uint64_t serial = 0;
    bool closed = false;
    void advance() {
        {
            const std::lock_guard lock(mutex);
            ++serial;
        }
        changed.notify_all();
    }
};
} // namespace

struct LiveCommitSubscription::State final : CommitWake {};

LiveCommitSubscription::LiveCommitSubscription(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}
LiveCommitSubscription::LiveCommitSubscription(LiveCommitSubscription&&) noexcept = default;
LiveCommitSubscription&
LiveCommitSubscription::operator=(LiveCommitSubscription&&) noexcept = default;
LiveCommitSubscription::~LiveCommitSubscription() = default;

std::uint64_t LiveCommitSubscription::serial() const noexcept {
    if (!state_)
        return 0;
    const std::lock_guard lock(state_->mutex);
    return state_->serial;
}

std::optional<std::uint64_t>
LiveCommitSubscription::waitForChange(const std::uint64_t observed,
                                      const std::stop_token& stop) const {
    if (!state_)
        return std::nullopt;
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, stop, [&] { return state_->closed || state_->serial != observed; });
    if (state_->closed || stop.stop_requested())
        return std::nullopt;
    return state_->serial;
}

void LiveCommitSubscription::close() noexcept {
    if (!state_)
        return;
    {
        const std::lock_guard lock(state_->mutex);
        state_->closed = true;
    }
    state_->changed.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Host state (shared with leases, which outlive neither the process nor their hold on it)

namespace {
struct HostCore {
    struct Commit {
        std::uint64_t generation = 0;
        LiveCallControl control;
        // An owner-thread call (undo, redo, the Viewer's level).
        Apply apply;
        // #744 (A2): a transaction its client thread prepares, waiting for its turn; then what it
        // prepared, waiting for the owner to publish it.
        std::optional<commands::Transaction> transaction;
        std::optional<commands::PreparedTransaction> prepared;
        // The live snapshot the owner handed back when the document moved past the base.
        std::optional<document::Snapshot> rebaseOn;
        enum class Step : std::uint8_t {
            Queued,
            Preparing,
            Prepared,
            Applying,
            Done
        } step = Step::Queued;
        bool waitedForGesture = false;
        // Set when the commit is completed while its client is preparing it, so preparation stops
        // at its next check.
        std::atomic_bool withdrawn = false;
        SessionCommandResult result;

        [[nodiscard]] bool prepares() const noexcept { return !apply; }
    };

    mutable std::mutex mutex;
    std::condition_variable_any finished;
    std::uint64_t generation = 0;
    LiveSessionAvailability availability = LiveSessionAvailability::Replacing;
    std::shared_ptr<const LiveSessionState> mirror;
    LiveSessionServices services;
    std::deque<std::shared_ptr<Commit>> queue;
    std::vector<std::shared_ptr<CommitWake>> subscribers;
    // The owner is publishing or applying a commit; the next one prepares once the mirror shows it.
    bool applying = false;
    // #747: the moving average of recent transactions' time from their turn to their result.
    std::optional<double> recentCommitMs;

    void recordCommitTime(const Clock::duration taken) {
        const auto ms = std::chrono::duration<double, std::milli>(taken).count();
        recentCommitMs = recentCommitMs ? *recentCommitMs * 0.75 + ms * 0.25 : ms;
    }

    void wakeOwner() {
        // Called with `mutex` held; subscriber locks are leaf locks.
        std::erase_if(subscribers, [](const auto& subscriber) {
            const std::lock_guard lock(subscriber->mutex);
            return subscriber->closed;
        });
        for (const auto& subscriber : subscribers) {
            {
                const std::lock_guard lock(subscriber->mutex);
                ++subscriber->serial;
            }
            subscriber->changed.notify_all();
        }
    }

    void complete(Commit& commit, SessionCommandResult result) {
        commit.result = std::move(result);
        commit.step = Commit::Step::Done;
        commit.withdrawn = true;
    }

    // Withdraws every queued commit (the owner keeps applying nothing from them).
    void withdrawAll(const LiveCommitStatus status) {
        for (auto& commit : queue)
            complete(*commit, refused(status));
        queue.clear();
        finished.notify_all();
        wakeOwner();
    }

    // The client's own withdrawal of a commit it is waiting for (cancelled or past its deadline).
    void withdraw(Commit& commit, const std::shared_ptr<Commit>& owned) {
        std::erase(queue, owned);
        complete(commit, refused(commit.control.stop.stop_requested() ? LiveCommitStatus::Cancelled
                                 : commit.waitedForGesture ? LiveCommitStatus::GestureOpen
                                                           : LiveCommitStatus::DeadlineExceeded));
        // The next commit may now be first in line to prepare.
        finished.notify_all();
        wakeOwner();
    }

    // Prepares `commit` on the calling client thread (#744, A2): called with `lock` held when the
    // commit is first in line and the owner is applying nothing, released while preparing.
    void prepare(std::unique_lock<std::mutex>& lock, const std::shared_ptr<Commit>& commit) {
        if (!mirror) {
            std::erase(queue, commit);
            complete(*commit, refused(LiveCommitStatus::ProjectReplaced));
            finished.notify_all();
            return;
        }
        // The newest mirror shows every earlier commit, which has published; the owner hands back
        // the live snapshot when the document moved on anyway.
        const auto base = commit->rebaseOn ? *commit->rebaseOn : mirror->renderSource.snapshot;
        commit->rebaseOn.reset();
        auto transaction = std::move(*commit->transaction);
        commit->transaction.reset();
        commit->step = Commit::Step::Preparing;
        const auto control = commit->control;
        lock.unlock();
        std::optional<commands::PreparedTransaction> prepared;
        std::optional<std::string> failure;
        try {
            std::function<void(std::size_t, std::size_t)> progress;
            if (control.progress)
                progress = [&control](const std::size_t applied, const std::size_t total) {
                    control.progress({.step = LiveCommitProgress::Step::Preparing,
                                      .ahead = 0,
                                      .applied = applied,
                                      .total = total});
                };
            prepared = commands::CommandStack::prepare(
                base, std::move(transaction),
                [&] {
                    return commit->withdrawn.load() || control.stop.stop_requested() ||
                           Clock::now() >= control.deadline;
                },
                progress);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        lock.lock();
        if (commit->step == Commit::Step::Done)
            return; // Withdrawn by revocation or shutdown while it was being prepared.
        if (failure) {
            std::erase(queue, commit);
            SessionCommandResult result;
            result.status = host::ProjectSessionCommandStatus::InvalidSession;
            result.diagnostic =
                OperationDiagnostic{"bloom.live.commit-failed", "", "", std::move(*failure)};
            complete(*commit, std::move(result));
            finished.notify_all();
            wakeOwner();
            return;
        }
        if (prepared->cancelled() || control.stop.stop_requested() ||
            Clock::now() >= control.deadline) {
            withdraw(*commit, commit);
            return;
        }
        commit->prepared = std::move(prepared);
        commit->step = Commit::Step::Prepared;
        wakeOwner();
    }

    SessionCommandResult enqueue(const std::uint64_t expected, const LiveCallControl& control,
                                 Apply apply, std::optional<commands::Transaction> transaction) {
        auto commit = std::make_shared<Commit>();
        commit->generation = expected;
        commit->control = control;
        commit->apply = std::move(apply);
        commit->transaction = std::move(transaction);
        std::unique_lock lock(mutex);
        if (availability == LiveSessionAvailability::ShuttingDown)
            return refused(LiveCommitStatus::ShuttingDown);
        if (generation != expected || availability != LiveSessionAvailability::Bound)
            return refused(LiveCommitStatus::ProjectReplaced);
        if (control.stop.stop_requested())
            return refused(LiveCommitStatus::Cancelled);
        if (queue.size() >= kLiveCommitQueueCapacity)
            return refused(LiveCommitStatus::QueueFull);
        queue.push_back(commit);
        wakeOwner();
        using Step = Commit::Step;
        const auto done = [&] { return commit->step == Step::Done; };
        // The commit's turn to be prepared: first in line, with nothing publishing before it.
        const auto turn = [&] {
            return commit->prepares() && commit->step == Step::Queued && !applying &&
                   !queue.empty() && queue.front() == commit;
        };
        // #747: tells the caller each time the commit moves on, without the lock.
        std::optional<LiveCommitProgress> told;
        const auto report = [&] {
            if (!control.progress || done())
                return;
            LiveCommitProgress now;
            if (commit->step == Step::Queued) {
                const auto at = std::ranges::find(queue, commit);
                now.ahead = static_cast<std::size_t>(at - queue.begin());
            } else if (commit->step == Step::Prepared) {
                now.step = LiveCommitProgress::Step::Prepared;
            } else if (commit->step == Step::Applying) {
                now.step = LiveCommitProgress::Step::Publishing;
            } else {
                return;
            }
            if (told && told->step == now.step && told->ahead == now.ahead)
                return;
            told = now;
            lock.unlock();
            control.progress(now);
            lock.lock();
        };
        std::optional<Clock::time_point> turnStarted;
        while (!done()) {
            report();
            if (done())
                break;
            if (commit->step == Step::Queued || commit->step == Step::Prepared) {
                const bool expired = Clock::now() >= control.deadline;
                if (control.stop.stop_requested() || expired) {
                    withdraw(*commit, commit);
                    break;
                }
                if (turn()) {
                    if (!turnStarted)
                        turnStarted = Clock::now();
                    prepare(lock, commit);
                    continue;
                }
                // #747: a caller that reports progress also wakes when its place in the queue
                // changes or publication starts.
                const auto moved = [&] {
                    if (!control.progress || !told)
                        return false;
                    if (commit->step == Step::Applying)
                        return true;
                    return commit->step == Step::Queued &&
                           static_cast<std::size_t>(std::ranges::find(queue, commit) -
                                                    queue.begin()) != told->ahead;
                };
                const auto wake = [&] { return done() || turn() || moved(); };
                if (control.deadline == Clock::time_point::max())
                    finished.wait(lock, control.stop, wake);
                else
                    finished.wait_until(lock, control.stop, control.deadline, wake);
            } else {
                // A commit that started publishing finishes atomically, whatever the caller's stop
                // token or deadline says. (A preparing commit is this thread's own work.)
                if (control.progress &&
                    (!told || told->step != LiveCommitProgress::Step::Publishing))
                    continue; // report() tells that publication started first.
                finished.wait(lock, done);
            }
        }
        if (turnStarted && commit->result.command)
            recordCommitTime(Clock::now() - *turnStarted);
        return std::move(commit->result);
    }
};
} // namespace

struct LiveSessionHost::State final : HostCore {};

// ---------------------------------------------------------------------------------------------
// Lease

struct LiveSessionLease::Hold {
    std::shared_ptr<HostCore> host;
    std::uint64_t generation = 0;
    std::shared_ptr<const LiveSessionState> taken;
    mutable std::mutex controlMutex;
    LiveCallControl control;

    [[nodiscard]] bool valid() const {
        const std::lock_guard lock(host->mutex);
        return host->generation == generation &&
               host->availability == LiveSessionAvailability::Bound;
    }
    [[nodiscard]] std::shared_ptr<const LiveSessionState> current() const {
        const std::lock_guard lock(host->mutex);
        if (host->generation != generation || host->availability != LiveSessionAvailability::Bound)
            return nullptr;
        return host->mirror;
    }
    [[nodiscard]] std::shared_ptr<const LiveSessionState> currentOrThrow() const {
        auto state = current();
        if (!state)
            throw std::runtime_error("The live project was replaced");
        return state;
    }
    [[nodiscard]] LiveCallControl callControl() const {
        const std::lock_guard lock(controlMutex);
        return control;
    }
    SessionCommandResult commit(Apply apply) const {
        return host->enqueue(generation, callControl(), std::move(apply), std::nullopt);
    }
    // #744 (A2): a transaction this client thread prepares in its turn.
    SessionCommandResult commit(commands::Transaction transaction) const {
        if (transaction.workUnits() > kLiveCommitWorkLimit)
            return refused(LiveCommitStatus::TooLarge);
        return host->enqueue(generation, callControl(), {}, std::move(transaction));
    }
};

LiveSessionLease::LiveSessionLease(LiveSessionLease&&) noexcept = default;
LiveSessionLease& LiveSessionLease::operator=(LiveSessionLease&&) noexcept = default;
LiveSessionLease::~LiveSessionLease() = default;

const LiveSessionState& LiveSessionLease::state() const noexcept { return *hold_->taken; }

OperationContext LiveSessionLease::operationContext() const {
    if (!hold_)
        return {};
    auto context = state().context;
    if (session_) {
        context.layerGeometry = session_->layerGeometryProvider();
        context.compositionFrameRate = session_->compositionFrameRateLookup();
        context.source = session_->operationSourceProvider();
    }
    return context;
}

std::shared_ptr<const LiveSessionState> LiveSessionLease::current() const {
    return hold_ ? hold_->current() : nullptr;
}

bool LiveSessionLease::revoked() const noexcept {
    try {
        return !hold_ || !hold_->valid();
    } catch (...) {
        return true;
    }
}

void LiveSessionLease::setControl(LiveCallControl control) {
    if (!hold_)
        return;
    const std::lock_guard lock(hold_->controlMutex);
    hold_->control = std::move(control);
}

LiveCallControl LiveSessionLease::control() const {
    if (!hold_)
        return {};
    return hold_->callControl();
}

SessionCommandResult LiveSessionLease::history(const bool redo,
                                               const document::Revision expectedRevision) {
    if (!hold_)
        return refused(LiveCommitStatus::ProjectReplaced);
    return hold_->commit([redo, expectedRevision](const LiveSessionCommitter& committer) {
        if (committer.revision && committer.revision() != expectedRevision) {
            commands::CommandResult stale;
            stale.action = redo ? commands::CommandAction::Redo : commands::CommandAction::Undo;
            stale.status = commands::CommandStatus::StaleRevision;
            stale.beforeRevision = committer.revision();
            stale.afterRevision = stale.beforeRevision;
            stale.expectedRevision = expectedRevision;
            return SessionCommandResult{host::ProjectSessionCommandStatus::Completed,
                                        std::move(stale), std::nullopt};
        }
        return committer.history ? committer.history(redo, commands::CommandOrigin::Agent)
                                 : SessionCommandResult{};
    });
}

// ---------------------------------------------------------------------------------------------
// Host

LiveSessionHost::LiveSessionHost() : state_(std::make_shared<State>()) {}

LiveSessionHost::~LiveSessionHost() {
    const std::lock_guard lock(state_->mutex);
    state_->availability = LiveSessionAvailability::ShuttingDown;
    state_->withdrawAll(LiveCommitStatus::ShuttingDown);
    for (const auto& subscriber : state_->subscribers) {
        {
            const std::lock_guard guard(subscriber->mutex);
            subscriber->closed = true;
        }
        subscriber->changed.notify_all();
    }
}

void LiveSessionHost::replace(LiveSessionServices services, LiveSessionState state) {
    const std::lock_guard lock(state_->mutex);
    if (state_->availability == LiveSessionAvailability::ShuttingDown)
        return;
    if (!state_->queue.empty())
        state_->withdrawAll(LiveCommitStatus::ProjectReplaced);
    ++state_->generation;
    state_->availability = LiveSessionAvailability::Bound;
    state_->services = std::move(services);
    state_->mirror = std::make_shared<const LiveSessionState>(std::move(state));
    state_->wakeOwner();
}

void LiveSessionHost::publish(LiveSessionState state) {
    const std::lock_guard lock(state_->mutex);
    if (state_->availability != LiveSessionAvailability::Bound)
        return;
    state_->mirror = std::make_shared<const LiveSessionState>(std::move(state));
}

void LiveSessionHost::revoke(const LiveSessionAvailability reason) {
    const std::lock_guard lock(state_->mutex);
    if (state_->availability == LiveSessionAvailability::ShuttingDown)
        return;
    if (state_->availability == LiveSessionAvailability::Bound)
        ++state_->generation;
    state_->availability =
        reason == LiveSessionAvailability::Bound ? LiveSessionAvailability::Replacing : reason;
    state_->mirror.reset();
    state_->services = {};
    state_->withdrawAll(reason == LiveSessionAvailability::ShuttingDown
                            ? LiveCommitStatus::ShuttingDown
                            : LiveCommitStatus::ProjectReplaced);
}

bool LiveSessionHost::drainOne(const LiveSessionCommitter& committer) {
    using Step = HostCore::Commit::Step;
    std::unique_lock lock(state_->mutex);
    while (!state_->queue.empty()) {
        auto commit = state_->queue.front();
        if (commit->generation != state_->generation ||
            state_->availability != LiveSessionAvailability::Bound) {
            state_->queue.pop_front();
            state_->complete(*commit, refused(LiveCommitStatus::ProjectReplaced));
            state_->finished.notify_all();
            return true;
        }
        // A commit being prepared is its client's to withdraw; it checks the same conditions.
        if (commit->step != Step::Preparing &&
            (commit->control.stop.stop_requested() || Clock::now() >= commit->control.deadline)) {
            state_->queue.pop_front();
            state_->complete(
                *commit, refused(commit->control.stop.stop_requested() ? LiveCommitStatus::Cancelled
                                 : commit->waitedForGesture ? LiveCommitStatus::GestureOpen
                                                            : LiveCommitStatus::DeadlineExceeded));
            state_->finished.notify_all();
            return true;
        }
        // #744 (A2): a transaction is published only once its client has prepared it.
        if (commit->prepares() && commit->step != Step::Prepared)
            return false;
        if (committer.gestureOpen && committer.gestureOpen()) {
            // The commit waits in the queue, in order, until the gesture closes.
            commit->waitedForGesture = true;
            return false;
        }
        state_->queue.pop_front();
        commit->step = Step::Applying;
        state_->applying = true;
        // #747: its client reports that publication started (it can no longer be withdrawn).
        state_->finished.notify_all();
        lock.unlock();
        SessionCommandResult result;
        std::optional<document::Snapshot> rebaseOn;
        try {
            if (!commit->prepares()) {
                result = commit->apply(committer);
            } else if (committer.publish) {
                auto publication = committer.publish(*commit->prepared);
                if (publication.rebaseOn)
                    rebaseOn = std::move(publication.rebaseOn);
                else if (publication.result)
                    result = std::move(*publication.result);
            } else if (committer.execute) {
                result = committer.execute(std::move(*commit->prepared).takeTransaction());
            }
        } catch (const std::exception& error) {
            rebaseOn.reset();
            result.status = host::ProjectSessionCommandStatus::InvalidSession;
            result.diagnostic =
                OperationDiagnostic{"bloom.live.commit-failed", "", "", error.what()};
        }
        lock.lock();
        state_->applying = false;
        if (rebaseOn) {
            // The document moved past the base and the transaction still applies on the live
            // revision: its client prepares it again on that snapshot, still first in line.
            commit->transaction = std::move(*commit->prepared).takeTransaction();
            commit->prepared.reset();
            commit->rebaseOn = std::move(rebaseOn);
            commit->step = Step::Queued;
            state_->queue.push_front(commit);
        } else {
            state_->complete(*commit, std::move(result));
        }
        state_->finished.notify_all();
        return true;
    }
    return false;
}

LiveCommitSubscription LiveSessionHost::subscribeCommits() {
    auto subscriber = std::make_shared<LiveCommitSubscription::State>();
    const std::lock_guard lock(state_->mutex);
    state_->subscribers.push_back(subscriber);
    return LiveCommitSubscription(subscriber);
}

LiveSessionLease LiveSessionHost::acquire(LiveCallControl control) {
    LiveSessionLease lease;
    auto hold = std::make_shared<LiveSessionLease::Hold>();
    hold->host = state_;
    hold->control = std::move(control);
    LiveSessionServices services;
    {
        const std::lock_guard lock(state_->mutex);
        lease.availability_ = state_->availability;
        lease.generation_ = state_->generation;
        if (state_->availability != LiveSessionAvailability::Bound || !state_->mirror)
            return lease;
        hold->generation = state_->generation;
        hold->taken = state_->mirror;
        services = state_->services;
    }
    SessionBinding binding;
    binding.valid = [hold] { return hold->valid(); };
    binding.snapshot = [hold] { return hold->currentOrThrow()->renderSource.snapshot; };
    binding.execute = [hold](commands::Transaction transaction) {
        return hold->commit(std::move(transaction));
    };
    binding.history = [hold](const bool redo, const commands::CommandOrigin origin) {
        return hold->commit([redo, origin](const LiveSessionCommitter& committer) {
            return committer.history ? committer.history(redo, origin) : SessionCommandResult{};
        });
    };
    binding.setViewerLevel = [hold](std::vector<std::uint64_t> level,
                                    const std::optional<std::uint64_t> points) {
        auto answer = std::make_shared<SessionViewerLevelResult>(
            SessionViewerLevelResult{"not-live", "This window has no Viewer"});
        const auto queued = hold->commit(
            [answer, level = std::move(level), points](const LiveSessionCommitter& committer) {
                if (committer.viewerLevel)
                    *answer = committer.viewerLevel(level, points);
                return SessionCommandResult{host::ProjectSessionCommandStatus::Completed,
                                            std::nullopt, std::nullopt};
            });
        if (queued.diagnostic)
            return SessionViewerLevelResult{queued.diagnostic->code, queued.diagnostic->message};
        return *answer;
    };
    binding.events = services.events ? services.events : std::make_shared<EventStream>();
    binding.publication = services.publication;
    binding.artifacts = services.artifacts;
    binding.renderSourceSnapshot = [hold] { return hold->currentOrThrow()->renderSource; };
    auto attached = Session::attach(std::move(binding));
    if (!attached) {
        lease.availability_ = LiveSessionAvailability::Replacing;
        return lease;
    }
    lease.session_ = std::shared_ptr<Session>(std::move(attached).takeSession());
    lease.hold_ = std::move(hold);
    lease.availability_ = LiveSessionAvailability::Bound;
    return lease;
}

LiveSessionAvailability LiveSessionHost::availability() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->availability;
}

std::uint64_t LiveSessionHost::generation() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->generation;
}

std::size_t LiveSessionHost::queuedCommits() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->queue.size();
}

std::optional<std::chrono::milliseconds> LiveSessionHost::recentCommitTime() const noexcept {
    const std::lock_guard lock(state_->mutex);
    if (!state_->recentCommitMs)
        return std::nullopt;
    return std::chrono::milliseconds(static_cast<std::int64_t>(std::ceil(*state_->recentCommitMs)));
}

std::shared_ptr<EventStream> LiveSessionHost::events() const {
    const std::lock_guard lock(state_->mutex);
    return state_->availability == LiveSessionAvailability::Bound ? state_->services.events
                                                                  : nullptr;
}

bool LiveSessionHost::waitingForGesture() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return !state_->queue.empty() && state_->queue.front()->waitedForGesture;
}

bool LiveSessionHost::preparing() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return !state_->queue.empty() && state_->queue.front()->prepares() &&
           state_->queue.front()->step != HostCore::Commit::Step::Prepared;
}

} // namespace bloom::scripting
