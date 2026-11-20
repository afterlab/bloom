#pragma once

#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::commands {

class HistoryCharge;

enum class CommandEventKind : std::uint8_t {
    RevisionChanged,
    HistoryChanged,
    Rejected,
};

struct CommandEvent final {
    CommandEventKind kind = CommandEventKind::Rejected;
    CommandResult result;
};

using CommandObserverId = std::uint64_t;
using CommandObserver = std::function<void(const CommandEvent&)>;

// #746: what an open history group has gathered (CommandStack::beginGroup).
struct HistoryGroupStats final {
    // Successful changing executes of the group's origin.
    std::uint64_t commits = 0;
    // Undo entries the group made: 1 unless something interrupted it.
    std::uint64_t entries = 0;
    // Times another origin's edit, undo or redo sealed the group's entry (the artist's own edit
    // during an agent session); the group's next commit then started a new entry.
    std::uint64_t interruptions = 0;

    friend bool operator==(const HistoryGroupStats&, const HistoryGroupStats&) = default;
};

class CommandStack;

// #744 (A2). A transaction applied to a draft of a base snapshot, its render footprint proven and
// its result validated and frozen, by CommandStack::prepare on any thread. CommandStack::publish
// then does only the owner's part: the revision check, the history step and its charge, the swap
// and the events. Move-only; it holds the base snapshot, so it never borrows the document.
class PreparedTransaction final {
  public:
    PreparedTransaction(PreparedTransaction&&) noexcept;
    PreparedTransaction& operator=(PreparedTransaction&&) noexcept;
    PreparedTransaction(const PreparedTransaction&) = delete;
    PreparedTransaction& operator=(const PreparedTransaction&) = delete;
    ~PreparedTransaction();

    [[nodiscard]] document::Revision baseRevision() const noexcept;
    [[nodiscard]] std::string_view label() const noexcept;
    [[nodiscard]] CommandOrigin origin() const noexcept;
    [[nodiscard]] std::optional<document::Revision> expectedRevision() const noexcept;
    // Preparation stopped because its cancellation check fired; nothing was prepared.
    [[nodiscard]] bool cancelled() const noexcept;
    // Gives the transaction back, to prepare it again on a newer base (CommandStack::rebaseNeeded).
    [[nodiscard]] Transaction takeTransaction() &&;

  private:
    friend class CommandStack;
    struct Detail;
    explicit PreparedTransaction(std::unique_ptr<Detail> detail) noexcept;
    std::unique_ptr<Detail> detail_;
};

class CommandStack final {
  public:
    struct HistoryLimits final {
        std::size_t maxEntries = 256;
        std::size_t maxChargedBytes = 512ULL * 1024 * 1024;
    };
    struct Work final {
        std::uint64_t operationApplies = 0;
        std::uint64_t lockedLayerChecks = 0;
        std::uint64_t chargedSnapshotsVisited = 0;
        std::uint64_t overBudgetValidations = 0;
        std::uint64_t historyEntriesRetired = 0;
        // Transactions whose parameter/animation footprint was proven by one before/after diff.
        std::uint64_t parameterFootprintDiffs = 0;
        // #399. Applied transactions whose render impact was proven by comparing the evaluation
        // inputs before and after (document/evaluation_inputs.hpp).
        std::uint64_t evaluationInputProofs = 0;
        // #502. Applied transactions the entry-layer reconcile pass ran for, the compositions it
        // skipped after one topology comparison, the ones it reconciled, and the transactions it
        // changed (adopted, returned or removed an entry layer).
        std::uint64_t entryLayerReconcilePasses = 0;
        std::uint64_t entryLayerReconcileSkips = 0;
        std::uint64_t entryLayerReconcileCompositions = 0;
        std::uint64_t entryLayerReconcileEdits = 0;
    };
    explicit CommandStack(document::Document& document);
    CommandStack(document::Document& document, HistoryLimits limits);
    CommandStack(const CommandStack&) = delete;
    CommandStack& operator=(const CommandStack&) = delete;
    CommandStack(CommandStack&&) = delete;
    CommandStack& operator=(CommandStack&&) = delete;
    ~CommandStack();

    // execute() is prepare() on the live snapshot followed by publish(), on the calling thread.
    [[nodiscard]] CommandResult execute(Transaction&& transaction);
    // #744 (A2). Any thread; touches neither this stack nor the document. Applies `transaction` to
    // a draft of `base`, reconciles entry layers, proves the render footprint and validates and
    // freezes the result. `cancelled` is polled between operations and phases; when it fires the
    // result is cancelled() and nothing else. A transaction whose expected revision is not `base`'s
    // is not applied: publish refuses it as stale, or rebaseNeeded() asks for a newer base.
    // #747: `progress`, when set, is told after each operation how many of the transaction's
    // operations have applied (applied, total), on the calling thread.
    [[nodiscard]] static PreparedTransaction
    prepare(const document::Snapshot& base, Transaction&& transaction,
            const std::function<bool()>& cancelled = {},
            const std::function<void(std::size_t, std::size_t)>& progress = {});
    // #744 (A2). Owner thread. True when the document has moved past `prepared`'s base and the
    // transaction would still apply on the live revision (it expects that revision or none), so it
    // must be prepared again on the live snapshot before publish. Answers no for a transaction
    // publish would refuse as stale anyway.
    [[nodiscard]] bool rebaseNeeded(const PreparedTransaction& prepared) const;
    // #744 (A2). Owner thread. Publishes what prepare() built, exactly as execute() would have from
    // the same base: the stale checks, the operation and validation refusals, the history step and
    // its charge, Document::publish and the events. A base the document has moved past is refused
    // as stale (call rebaseNeeded() first to re-base instead).
    [[nodiscard]] CommandResult publish(PreparedTransaction&& prepared);
    // #748. Owner thread. A project Bloom lets you build must save: when set, every change
    // execute() or publish() would make is first given to `check` with the current state and the
    // state it would publish, and a refusal answers CommandStatus::ProjectTooLarge with nothing
    // applied. The owner of the project file format supplies it (commands does not know the
    // format). Undo and redo are not checked: every state they restore was admitted.
    using CapacityCheck = std::function<std::optional<ProjectCapacityRefusal>(
        const document::Snapshot& before, const document::Snapshot& candidate)>;
    void setCapacityCheck(CapacityCheck check) { capacityCheck_ = std::move(check); }
    [[nodiscard]] CommandResult undo(CommandOrigin origin = CommandOrigin::User);
    [[nodiscard]] CommandResult redo(CommandOrigin origin = CommandOrigin::User);

    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ < history_.size(); }
    [[nodiscard]] std::optional<std::string_view> undoLabel() const noexcept;
    [[nodiscard]] std::optional<std::string_view> redoLabel() const noexcept;
    // The label of every history entry, oldest first, and how many of them are applied (the
    // entries at and after this position are the redo branch).
    [[nodiscard]] std::vector<std::string> historyLabels() const;
    [[nodiscard]] std::size_t historyPosition() const noexcept { return cursor_; }
    [[nodiscard]] document::Revision trackedRevision() const noexcept { return trackedRevision_; }
    [[nodiscard]] std::size_t size() const noexcept { return history_.size(); }
    [[nodiscard]] std::size_t historyChargedBytes() const noexcept { return historyChargedBytes_; }
    [[nodiscard]] HistoryLimits historyLimits() const noexcept { return limits_; }
    [[nodiscard]] Work work() const noexcept { return work_; }

    void clear();
    // Forgets every redo entry, as a new edit after Undo would. A withdrawn creation (for example
    // new text the author emptied) undoes and then discards, so Redo can never bring it back.
    void discardRedo();

    // #746: history grouping for an agent session. While a group is open, each successful execute
    // whose transaction origin is `origin` merges into the group's newest undo entry, labelled
    // `label`, so the whole session undoes in one step. The merged entry keeps only the state from
    // before the group's first commit and the newest state, never the ones in between, so a group
    // holds no more history than one ordinary step and merging does no more work than recording a
    // step does. Another origin's edit (the artist's own), any undo or redo, or a merged state too
    // large for the history charge seals the entry; the group's next commit starts a new entry
    // with the same label, after the artist's step, so nothing the artist did is folded into the
    // agent's step or lost. Beginning a group while one is open replaces it (sealing the entry).
    void beginGroup(std::string label, CommandOrigin origin);
    // Seals and closes the open group and returns what it gathered (zeros when none was open).
    HistoryGroupStats endGroup();
    [[nodiscard]] bool groupOpen() const noexcept { return group_.has_value(); }
    [[nodiscard]] HistoryGroupStats groupStats() const noexcept {
        return group_ ? group_->stats : HistoryGroupStats{};
    }

    [[nodiscard]] CommandObserverId addObserver(CommandObserver observer);
    void removeObserver(CommandObserverId observerId) noexcept;

  private:
    struct HistoryEntry {
        std::string label;
        document::Snapshot before;
        document::Snapshot after;
        // The render impact of the transaction that created this entry. Restoring the entry on
        // undo/redo replays it, because a restore does not re-run the operations that declared it.
        bool renderAffecting = true;
        // The proven finite changed-time footprint, when the transaction proved one; std::nullopt
        // is the conservative whole-render default. Replayed verbatim by undo/redo.
        std::optional<AffectedTimeFootprint> affectedTimes;
        // SPLIT-1. The transaction's FORWARD ordered geometry remaps, when every applied
        // render-affecting op proved a finite footprint. Undo publishes the inverted list; redo
        // publishes this stored forward list.
        std::optional<std::vector<LayerIdentityRemap>> layerIdentityRemaps;
        // #355. The composition the render effect is confined to, when known.
        std::optional<document::CompositionId> affectedComposition;
    };

    [[nodiscard]] std::optional<CommandResult> staleResult(CommandAction action, std::string label,
                                                           const document::Snapshot& current) const;
    struct OpenGroup final {
        std::string label;
        CommandOrigin origin = CommandOrigin::Agent;
        // The newest history entry is the group's and the next group commit may merge into it.
        bool entryOpen = false;
        document::Revision entryAfter;
        HistoryGroupStats stats;
    };

    [[nodiscard]] CommandResult publishStep(PreparedTransaction&& prepared);
    [[nodiscard]] CommandResult undoStep();
    [[nodiscard]] CommandResult redoStep();
    void notify(const CommandResult& result) const noexcept;

    std::vector<HistoryEntry> history_;
    std::size_t cursor_ = 0;
    HistoryLimits limits_;
    std::size_t historyChargedBytes_ = 0;
    std::unique_ptr<HistoryCharge> historyCharge_;
    Work work_;
    CapacityCheck capacityCheck_;
    document::Revision trackedRevision_;
    CommandOrigin currentOrigin_ = CommandOrigin::User;
    std::optional<OpenGroup> group_;
    document::Document& document_;
    mutable std::mutex observerMutex_;
    std::vector<std::pair<CommandObserverId, CommandObserver>> observers_;
    CommandObserverId nextObserverId_ = 1;
};

} // namespace bloom::commands
