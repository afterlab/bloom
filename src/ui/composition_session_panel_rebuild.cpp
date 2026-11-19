// #744: the panels that rebuild from the snapshot do so at most once per display frame for a burst
// of agent or script commits, and the artist hears and reads a retirement of undo steps once per
// burst, as information rather than a refusal.
#include <bloom/ui/composition_session.hpp>

#include <QThread>
#include <QTimer>

namespace bloom::ui {

void CompositionSession::publishSnapshotChanged(const bool coalescePanels) {
    emit snapshotChanged();
    if (coalescePanels) {
        panelRebuildPending_ = true;
        if (panelRebuildTimer_ == nullptr) {
            panelRebuildTimer_ = new QTimer(this);
            panelRebuildTimer_->setSingleShot(true);
            panelRebuildTimer_->setInterval(kPanelRebuildInterval);
            connect(panelRebuildTimer_, &QTimer::timeout, this,
                    &CompositionSession::flushPanelRebuild);
        }
        // Measured from the first commit that waits, so a steady stream still rebuilds the panels
        // once per frame instead of never.
        if (!panelRebuildTimer_->isActive())
            panelRebuildTimer_->start();
        return;
    }
    // Anything that does not wait takes a waiting rebuild with it.
    panelRebuildPending_ = false;
    if (panelRebuildTimer_ != nullptr)
        panelRebuildTimer_->stop();
    emit panelRebuildDue();
}

void CompositionSession::flushPanelRebuild() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (panelRebuildTimer_ != nullptr)
        panelRebuildTimer_->stop();
    if (!panelRebuildPending_)
        return;
    panelRebuildPending_ = false;
    emit panelRebuildDue();
}

namespace {
[[nodiscard]] QString byteSize(const std::size_t bytes) {
    constexpr std::size_t kMiB = 1024ULL * 1024;
    if (bytes >= kMiB && bytes % kMiB == 0)
        return CompositionSession::tr("%1 MiB").arg(static_cast<qulonglong>(bytes / kMiB));
    return CompositionSession::tr("%1 KiB").arg(static_cast<qulonglong>((bytes + 1023) / 1024));
}
} // namespace

QString CompositionSession::undoRetirementNotice(const commands::CommandResult& result,
                                                 const commands::CommandStack::HistoryLimits limits) {
    const auto steps = static_cast<qulonglong>(limits.maxEntries);
    if (result.historyEntryLimitReached && result.historyMemoryLimitReached)
        return tr("Undo history is at its %1-step limit and its %2 memory limit: the oldest undo "
                  "steps are removed to make room for new edits")
            .arg(steps)
            .arg(byteSize(limits.maxChargedBytes));
    if (result.historyEntryLimitReached)
        return tr("Undo history is at its %1-step limit: the oldest undo steps are removed to make "
                  "room for new edits")
            .arg(steps);
    return tr("Undo history is at its %1 memory limit: the oldest undo steps are removed to make "
              "room for new edits")
        .arg(byteSize(limits.maxChargedBytes));
}

void CompositionSession::noteUndoRetirement(const commands::CommandResult& result) {
    // #746: an agent's edit is answered over MCP; the undo steps it retires are not the artist's
    // news, and do not open or extend a burst either.
    if (result.historyEntriesDropped == 0 || result.origin == commands::CommandOrigin::Agent ||
        commandStack_ == nullptr)
        return;
    if (undoRetirementBurst_ == nullptr) {
        undoRetirementBurst_ = new QTimer(this);
        undoRetirementBurst_->setSingleShot(true);
    }
    undoRetirementBurst_->setInterval(undoRetirementBurstGap_);
    const bool withinBurst = undoRetirementBurst_->isActive();
    // Every retirement extends the burst: it ends after a gap with none.
    undoRetirementBurst_->start();
    if (!withinBurst)
        emit noticePosted(undoRetirementNotice(result, commandStack_->historyLimits()));
}

} // namespace bloom::ui
