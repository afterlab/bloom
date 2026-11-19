// #746: an agent session's refresh hold. While an agent session is open the window catches up
// with the agent's commits at a slow cadence and the Viewer re-renders only on the session's end,
// on demand or on the artist's own interaction (docs/user-guide/mcp.md, "Agent sessions").
#include <bloom/ui/composition_session.hpp>

#include <bloom/commands/transaction.hpp>

#include <QThread>
#include <QTimer>

namespace bloom::ui {

document::Snapshot CompositionSession::liveSnapshot() const { return document_->snapshot(); }

void CompositionSession::setAgentRefreshHold(std::optional<AgentRefreshHold> hold) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (hold.has_value() && hold->panelCadence < std::chrono::milliseconds{1})
        hold->panelCadence = std::chrono::milliseconds{1};
    refreshHold_ = hold;
    if (!refreshHold_.has_value()) {
        refreshHeldViewer();
        return;
    }
    // A hold that no longer holds the panels adopts what waits; one that no longer holds the
    // Viewer refreshes it.
    if (!refreshHold_->viewer)
        refreshHeldViewer();
    else if (!refreshHold_->panels)
        refreshHeldPanels();
}

void CompositionSession::deferPanelRefresh() {
    panelsDeferred_ = true;
    if (deferredRefreshTimer_ == nullptr) {
        deferredRefreshTimer_ = new QTimer(this);
        deferredRefreshTimer_->setSingleShot(true);
        connect(deferredRefreshTimer_, &QTimer::timeout, this,
                &CompositionSession::refreshHeldPanels);
    }
    // The cadence is measured from the first commit that waits, so a steady stream of agent
    // commits still reaches the panels once per cadence instead of never.
    if (!deferredRefreshTimer_->isActive() && refreshHold_.has_value())
        deferredRefreshTimer_->start(refreshHold_->panelCadence);
}

void CompositionSession::catchUpForArtist(commands::Transaction* const transaction) {
    // Only the agent's own commits can hold the window behind (an artist's commit adopts at once),
    // so catching up shows the artist what the agent did, then the edit lands on the newest
    // revision; its operations still validate against it.
    if (!panelsDeferred_)
        return;
    const auto shown = snapshot_.revision();
    refreshHeldViewer();
    if (transaction != nullptr && transaction->expectedRevision() == shown)
        transaction->setExpectedRevision(snapshot_.revision());
}

void CompositionSession::refreshHeldPanels() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!panelsDeferred_)
        return;
    adoptDocumentSnapshot(nullptr, refreshHold_.has_value() && refreshHold_->viewer);
}

void CompositionSession::refreshHeldViewer() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!panelsDeferred_ && !evaluationDeferred_)
        return;
    adoptDocumentSnapshot(nullptr, false);
}

} // namespace bloom::ui
