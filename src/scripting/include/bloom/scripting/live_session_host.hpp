#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/session.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace bloom::host {
class PublicationCoordinator;
} // namespace bloom::host
namespace bloom::platform {
class StagedArtifactCoordinator;
} // namespace bloom::platform

// The live GUI session as seen by off-thread clients (ADR 0034, section 4). Qt-free; owned by
// src/scripting.
//
// Today the Script editor's ScriptPanelRuntime (src/ui) privately mirrors the live snapshot and
// context and marshals authoring onto the UI thread. The in-app MCP server needs exactly the same
// thing, so that mechanism moves here once and both clients use it: one mirror, one bounded marshal
// queue, one generation counter, one revocation rule.
//
// Owner thread (desktop: the UI thread, through one application-lifetime adapter in src/ui):
// publish() mirrors the session after every snapshot, composition, selection, time, colour
// settings, display/view and save change; replace()/revoke() bracket project replacement;
// drainOne() applies at most one queued commit per event-loop slice through the owner's committer.
// None of these block or perform I/O.
//
// Client threads (the Python scripting thread, MCP request workers): acquire() returns a lease on
// the current generation -- an attached scripting::Session whose snapshot and render-source reads
// come from the mirror and whose execute()/history() enqueue onto the owner thread and wait for the
// result. The UI thread never calls acquire() or waits on a lease.
//
// Ordering and atomicity: queued commits publish in FIFO order, one per slice. #744 (A2): a
// transaction is built, applied, proven and validated off the owner thread, on the client thread
// that commits it (commands::CommandStack::prepare), against the newest mirror, and only when every
// earlier commit has published; the owner thread only publishes it (the revision check, the swap,
// the undo history and the events). A commit whose base the document moved past in the meantime
// (the artist's own edit) is prepared again on the live snapshot when its expected revision is
// still the live one, and refused as stale otherwise. A commit that has started publishing
// finishes atomically (it lands in the undo history exactly like a panel edit, with the
// transaction's label). A commit still queued or preparing when its caller's stop token fires, its
// deadline passes or its generation is revoked is withdrawn and never applied. A commit never
// publishes while the artist holds an open interactive gesture (ADR 0017): it waits in the queue
// until the gesture closes, bounded by the caller's deadline (GestureOpen). The work one commit
// carries is bounded (kLiveCommitWorkLimit, TooLarge).
namespace bloom::scripting {

// Everything a client may read about the live session, captured together on the owner thread so
// no read can mix two projects or two revisions.
struct LiveSessionState final {
    SessionRenderSourceSnapshot renderSource;
    // The editor context contextual arguments fall back to (project, composition, selected nodes,
    // selected layers, current time). layerGeometry is supplied by the lease, not the mirror.
    OperationContext context;
    // The Viewer's display and view for the context composition; empty when no Viewer is open (the
    // project's default display and view then apply).
    std::string viewerDisplay;
    std::string viewerView;
    // The project file, absent for a never-saved project.
    std::optional<std::filesystem::path> projectPath;
    bool readOnly = false;
};

// Why no lease is available.
enum class LiveSessionAvailability : std::uint8_t {
    Bound = 0,
    // A project is opening, closing or being replaced; leases resume on the next generation.
    Replacing = 1,
    // The application is shutting down.
    ShuttingDown = 2,
};

enum class LiveCommitStatus : std::uint8_t {
    Completed = 0,
    // The bounded queue is full (kLiveCommitQueueCapacity); nothing was queued.
    QueueFull = 1,
    // The lease's generation was revoked before the commit started; nothing was applied.
    ProjectReplaced = 2,
    // The caller's stop token fired before the commit started; nothing was applied.
    Cancelled = 3,
    // The caller's deadline passed before the commit started; nothing was applied.
    DeadlineExceeded = 4,
    // The deadline passed while an interactive gesture stayed open; nothing was applied.
    GestureOpen = 5,
    ShuttingDown = 6,
    // #744 (A2): the transaction carries more work than one live commit takes
    // (kLiveCommitWorkLimit); nothing was queued.
    TooLarge = 7,
};

// A refused commit reaches the client as SessionCommandResult::diagnostic with this stable code,
// so Python and MCP report the same reason.
[[nodiscard]] constexpr std::string_view liveCommitDiagnosticCode(const LiveCommitStatus status) {
    switch (status) {
    case LiveCommitStatus::Completed:
        return {};
    case LiveCommitStatus::QueueFull:
        return "bloom.live.queue-full";
    case LiveCommitStatus::ProjectReplaced:
        return "bloom.live.project-replaced";
    case LiveCommitStatus::Cancelled:
        return "bloom.live.cancelled";
    case LiveCommitStatus::DeadlineExceeded:
        return "bloom.live.deadline-exceeded";
    case LiveCommitStatus::GestureOpen:
        return "bloom.live.gesture-open";
    case LiveCommitStatus::ShuttingDown:
        return "bloom.live.shutting-down";
    case LiveCommitStatus::TooLarge:
        return "bloom.live.commit-too-large";
    }
    return "bloom.live.unknown";
}

// Shared with the Script editor's existing bound: at most 16 queued commits across all clients.
inline constexpr std::size_t kLiveCommitQueueCapacity = 16;

// #744 (A2): the most work one live commit carries, in commands::Transaction::workUnits() (one per
// operation, or one per key, keyframe or edit an operation lists). A larger transaction is refused
// before it is queued (TooLarge, bloom.live.commit-too-large); split it into several. Its
// preparation runs off the owner thread and stops at the caller's deadline.
inline constexpr std::size_t kLiveCommitWorkLimit = 1'000'000;

// #744 (A2): what the owner made of a prepared transaction. Exactly one is set: the published (or
// refused) result, or the live snapshot to prepare the transaction again on.
struct LivePublication final {
    std::optional<SessionCommandResult> result;
    std::optional<document::Snapshot> rebaseOn;
};

// How the owner applies a queued commit. Set by the src/ui adapter; each function runs on the owner
// thread inside drainOne() only.
struct LiveSessionCommitter final {
    // Applies a transaction on the owner thread. Used only without `publish` (simple owners and
    // tests); the desktop sets `publish`.
    std::function<SessionCommandResult(commands::Transaction)> execute;
    // #744 (A2): publishes a transaction a client thread prepared, or asks for a re-base when the
    // document moved past its base and it would still apply (commands::CommandStack::rebaseNeeded).
    std::function<LivePublication(commands::PreparedTransaction&)> publish;
    // false = undo, true = redo; the origin is who asked.
    std::function<SessionCommandResult(bool, commands::CommandOrigin)> history;
    // True while an interactive gesture (drag, scrub-edit, text edit) is open.
    std::function<bool()> gestureOpen;
    // The live document revision, so a revision-guarded history step (MCP `history` with
    // expectedRevision) is checked on the owner thread in the same slice that applies it.
    std::function<document::Revision()> revision;
    // #645: sets the Viewer's drill level (Session::setViewerLevel). Absent: "not-live".
    std::function<SessionViewerLevelResult(const std::vector<std::uint64_t>&,
                                           std::optional<std::uint64_t>)>
        viewerLevel;
};

// #747: where a transaction stands on its way to the document, for a caller that reports
// progress (an asynchronous MCP transact, or one that carries a progress token).
struct LiveCommitProgress final {
    enum class Step : std::uint8_t {
        // Waiting in the queue behind `ahead` commits.
        Queued = 0,
        // Being prepared on the client thread: `applied` of `total` operations so far.
        Preparing = 1,
        // Prepared, waiting for the owner thread (and for an open gesture to close).
        Prepared = 2,
        // The owner thread is publishing it; from here it finishes whatever the stop token says.
        Publishing = 3,
    };
    Step step = Step::Queued;
    std::size_t ahead = 0;
    std::size_t applied = 0;
    std::size_t total = 0;
};

// The calling request's cancellation and deadline, carried into the marshal so a queued commit
// can be withdrawn.
struct LiveCallControl final {
    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    // #747: told, on the client thread and without the host's lock, each time the commit moves on
    // (and after each operation it prepares). Optional.
    std::function<void(const LiveCommitProgress&)> progress{};
};

// One client call's hold on one generation. Move-only. While a lease is alive its Session stays
// valid for reads; commits through it fail with ProjectReplaced once its generation is revoked.
class LiveSessionLease final {
  public:
    LiveSessionLease() = default;
    LiveSessionLease(LiveSessionLease&&) noexcept;
    LiveSessionLease& operator=(LiveSessionLease&&) noexcept;
    LiveSessionLease(const LiveSessionLease&) = delete;
    LiveSessionLease& operator=(const LiveSessionLease&) = delete;
    ~LiveSessionLease();

    [[nodiscard]] explicit operator bool() const noexcept { return session_ != nullptr; }
    [[nodiscard]] LiveSessionAvailability availability() const noexcept { return availability_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    // The mirror as it was when the lease was taken. Only for a bound lease (operator bool).
    [[nodiscard]] const LiveSessionState& state() const noexcept;
    // An attached session (Session::attach) for this generation: reads come from the mirror, and
    // execute()/undo()/redo() are queued commits under this lease's call control.
    [[nodiscard]] Session& session() const noexcept { return *session_; }
    // The same session as a shared owner, for a client that binds it for the whole generation (the
    // Python console). Its reads and commits stay governed by this lease's hold.
    [[nodiscard]] std::shared_ptr<Session> sharedSession() const noexcept { return session_; }
    // state().context plus a layer-geometry provider that measures on the calling client thread.
    [[nodiscard]] OperationContext operationContext() const;
    // The newest mirror of this lease's generation (published after the lease was taken), or null
    // once the generation is revoked. state() stays the mirror as it was when the lease was taken.
    [[nodiscard]] std::shared_ptr<const LiveSessionState> current() const;
    [[nodiscard]] bool revoked() const noexcept;
    // Replaces the call control later commits through this lease carry. A long-lived lease (the
    // Python console binds one per generation) gives each script its own cancellation. Call it on
    // the client thread that commits through the lease.
    void setControl(LiveCallControl control);
    // #747: the call control later commits carry (to extend it, for example with progress).
    [[nodiscard]] LiveCallControl control() const;
    // A revision-guarded undo or redo: queued like any commit, and refused with a stale-revision
    // result on the owner thread when the live revision is not `expectedRevision`.
    [[nodiscard]] SessionCommandResult history(bool redo, document::Revision expectedRevision);

  private:
    struct Hold;
    friend class LiveSessionHost;
    std::shared_ptr<Session> session_;
    std::shared_ptr<Hold> hold_;
    std::uint64_t generation_ = 0;
    LiveSessionAvailability availability_ = LiveSessionAvailability::Replacing;
};

// A neutral wake for the owner adapter: the serial advances whenever a commit is queued or
// withdrawn, so the adapter drains promptly instead of polling. Same shape as
// runtime::TaskCompletionSubscription; close() is concurrent and idempotent.
class LiveCommitSubscription final {
  public:
    LiveCommitSubscription() = default;
    LiveCommitSubscription(LiveCommitSubscription&&) noexcept;
    LiveCommitSubscription& operator=(LiveCommitSubscription&&) noexcept;
    LiveCommitSubscription(const LiveCommitSubscription&) = delete;
    LiveCommitSubscription& operator=(const LiveCommitSubscription&) = delete;
    ~LiveCommitSubscription();

    [[nodiscard]] std::uint64_t serial() const noexcept;
    // Blocks the calling (non-UI) waiter thread until the serial differs from `observed`; nullopt
    // when closed or stopped.
    [[nodiscard]] std::optional<std::uint64_t> waitForChange(std::uint64_t observed,
                                                             const std::stop_token& stop) const;
    void close() noexcept;

  private:
    struct State;
    friend class LiveSessionHost;
    explicit LiveCommitSubscription(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> state_;
};

// The borrowed host collaborators of one generation, as SessionBinding takes them today.
struct LiveSessionServices final {
    std::shared_ptr<EventStream> events;
    host::PublicationCoordinator* publication = nullptr;
    platform::StagedArtifactCoordinator* artifacts = nullptr;
};

class LiveSessionHost final {
  public:
    LiveSessionHost();
    ~LiveSessionHost();
    LiveSessionHost(const LiveSessionHost&) = delete;
    LiveSessionHost& operator=(const LiveSessionHost&) = delete;

    // ---- Owner thread. Non-blocking, no I/O.

    // Starts a new generation for a newly bound project. Revokes the previous generation first.
    void replace(LiveSessionServices services, LiveSessionState state);
    // Refreshes the mirror within the current generation.
    void publish(LiveSessionState state);
    // Revokes the current generation (project closing, opening, or shutdown): queued commits are
    // withdrawn with ProjectReplaced/ShuttingDown and new leases report `reason`.
    void revoke(LiveSessionAvailability reason);
    // Publishes or applies at most one queued commit through `committer`; returns true if one was
    // published, applied, handed back for a re-base or withdrawn. A commit waiting for a gesture,
    // or for its client to prepare it, stays queued and returns false; the commit subscription
    // wakes the owner when it is ready.
    bool drainOne(const LiveSessionCommitter& committer);
    [[nodiscard]] LiveCommitSubscription subscribeCommits();

    // ---- Any thread except the UI thread.

    [[nodiscard]] LiveSessionLease acquire(LiveCallControl control);

    // ---- Any thread. Non-blocking.
    [[nodiscard]] LiveSessionAvailability availability() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::size_t queuedCommits() const noexcept;
    // #747: a moving average of how long recent transactions took from their turn (the start of
    // their preparation) to their result; nullopt before the first one. Back-pressure answers
    // derive retryAfterMs from it and the queue depth.
    [[nodiscard]] std::optional<std::chrono::milliseconds> recentCommitTime() const noexcept;
    // The current generation's command event stream (null while no project is bound), so a client
    // can follow command events from the moment a generation starts.
    [[nodiscard]] std::shared_ptr<EventStream> events() const;
    // True while the oldest queued commit is waiting for an open gesture to close; the owner
    // drains again when the gesture ends.
    [[nodiscard]] bool waitingForGesture() const noexcept;
    // #744 (A2): true while the oldest queued commit is a transaction its client has not finished
    // preparing; the commit subscription wakes the owner when it is ready to publish.
    [[nodiscard]] bool preparing() const noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace bloom::scripting
