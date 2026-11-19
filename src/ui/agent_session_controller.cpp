#include <bloom/ui/agent_session_controller.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ui_feedback.hpp>

#include <QCoreApplication>
#include <QEvent>

#include <utility>

namespace bloom::ui {
namespace {

using mcp::McpAgentSessionClose;
using mcp::McpAgentSessionResult;
using mcp::McpUiRefusal;

std::string closedWords(const mcp::McpAgentSessionView& view) {
    switch (view.closed.value_or(McpAgentSessionClose::Ended)) {
    case McpAgentSessionClose::Ended:
        return "it was ended";
    case McpAgentSessionClose::ClientDisconnected:
        return "its client disconnected";
    case McpAgentSessionClose::IdleTimeout:
        return "its client sent nothing for " + std::to_string(view.idleTimeout.count()) +
               " seconds";
    case McpAgentSessionClose::ProjectReplaced:
        return "the project was replaced";
    case McpAgentSessionClose::ServerStopped:
        return "the server stopped";
    }
    return "it was ended";
}

} // namespace

AgentSessionController::AgentSessionController(CompositionSession& session,
                                               std::function<commands::CommandStack*()> stack,
                                               QObject* parent)
    : QObject(parent), session_(session), stack_(std::move(stack)) {
    // A replaced project ends the session: its undo group went with the old history, and the
    // rebind already dropped the refresh hold. The old stack is never touched again.
    connect(&session_, &CompositionSession::documentRebound, this, [this] {
        if (open_.has_value())
            static_cast<void>(close(McpAgentSessionClose::ProjectReplaced, false));
    });
}

AgentSessionController::~AgentSessionController() {
    if (filtering_ && QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->removeEventFilter(this);
}

QString AgentSessionController::displayName(const mcp::McpAgentSessionView& view) {
    return view.label.empty() ? tr("Agent session")
                              : tr("Agent session “%1”").arg(QString::fromStdString(view.label));
}

std::optional<mcp::McpAgentSessionView> AgentSessionController::current() const {
    if (!open_.has_value())
        return std::nullopt;
    auto view = open_->view;
    view.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - open_->began).count();
    view.undoSteps = view.options.groupUndo ? open_->stats.entries : view.commits;
    return view;
}

McpAgentSessionResult AgentSessionController::begin(const mcp::McpAgentSessionBegin& request) {
    if (open_.has_value()) {
        if (open_->view.client == request.client)
            return {McpUiRefusal::Busy,
                    "Agent session " + std::to_string(open_->view.session) +
                        " is already open for this client; end it with session.end first.",
                    *current()};
        return {McpUiRefusal::Busy,
                "Another agent's session (" + displayName(open_->view).toStdString() +
                    ") is open; try again when it ends.",
                {}};
    }
    auto* const stack = stack_ ? stack_() : nullptr;
    if (stack == nullptr)
        return {McpUiRefusal::Busy, "A project is opening; try again once it is open.", {}};

    Open open;
    open.began = std::chrono::steady_clock::now();
    open.stack = stack;
    open.view.session = next_++;
    open.view.client = request.client;
    open.view.label = request.label;
    open.view.options = request.options;
    open.view.idleTimeout = request.idleTimeout;
    open.view.open = true;
    open.life = std::make_shared<int>(0);
    // Counts what happens while the session is open. The observer runs inside the stack's own
    // call, so reading its group statistics there is safe.
    open.observer = stack->addObserver(
        [this, life = std::weak_ptr<int>(open.life), stack](const commands::CommandEvent& event) {
            if (life.expired() || !open_.has_value() ||
                event.kind != commands::CommandEventKind::RevisionChanged)
                return;
            if (event.result.origin == commands::CommandOrigin::Agent) {
                if (event.result.action == commands::CommandAction::Execute)
                    ++open_->view.commits;
            } else {
                ++open_->view.artistEdits;
            }
            open_->stats = stack->groupStats();
        });
    if (request.options.groupUndo)
        stack->beginGroup(displayName(open.view).toStdString(), commands::CommandOrigin::Agent);
    open_ = std::move(open);
    if (request.options.quiet && feedback_ != nullptr)
        feedback_->holdAgentActivity(true);
    session_.setAgentRefreshHold(CompositionSession::AgentRefreshHold{
        .panels = true, .viewer = request.options.deferViewer, .panelCadence = kPanelCadence});
    // The panels are always held, so the artist's input always catches the window up first.
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->installEventFilter(this);
        filtering_ = true;
    }
    Q_EMIT changed();
    return {McpUiRefusal::None, {}, *current()};
}

McpAgentSessionResult AgentSessionController::end(const mcp::McpAgentSessionEnd& request) {
    if (!open_.has_value() || open_->view.client != request.client ||
        (request.session != 0 && request.session != open_->view.session)) {
        if (const auto found = closed_.find(request.client);
            found != closed_.end() &&
            (request.session == 0 || request.session == found->second.session))
            return {McpUiRefusal::InvalidArgument,
                    "Agent session " + std::to_string(found->second.session) +
                        " already closed: " + closedWords(found->second) + ".",
                    found->second};
        return {McpUiRefusal::InvalidArgument,
                "This client has no open agent session; session.begin opens one.",
                {}};
    }
    auto view = close(request.cause, true);
    return {McpUiRefusal::None, {}, std::move(view)};
}

mcp::McpAgentSessionView AgentSessionController::close(const McpAgentSessionClose cause,
                                                       const bool stackAlive) {
    auto open = std::move(*open_);
    open_.reset();
    open.life.reset();
    if (stackAlive && open.stack != nullptr && stack_ && stack_() == open.stack) {
        open.stack->removeObserver(open.observer);
        if (open.view.options.groupUndo)
            open.stats = open.stack->endGroup();
    }
    auto& view = open.view;
    view.open = false;
    view.closed = cause;
    view.undoSteps = view.options.groupUndo ? open.stats.entries : view.commits;
    view.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - open.began).count();
    if (filtering_ && QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->removeEventFilter(this);
    filtering_ = false;
    // The window catches up and the Viewer refreshes; what the session held is told once.
    session_.setAgentRefreshHold(std::nullopt);
    if (view.options.quiet && feedback_ != nullptr)
        feedback_->holdAgentActivity(false, displayName(view));
    closed_[view.client] = view;
    while (closed_.size() > kRememberedClosed) {
        auto oldest = closed_.begin();
        for (auto it = closed_.begin(); it != closed_.end(); ++it)
            if (it->second.session < oldest->second.session)
                oldest = it;
        closed_.erase(oldest);
    }
    Q_EMIT changed();
    return view;
}

bool AgentSessionController::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::KeyPress:
    case QEvent::Wheel:
    case QEvent::TabletPress:
    case QEvent::TouchBegin:
        // The artist is at the window: show them what the agent did before their input lands.
        // Only on the top-level window's delivery, before Qt picks the widget that receives it,
        // so a panel that rebuilds while catching up never loses the widget the input was for.
        if (open_.has_value() && watched != nullptr && watched->isWindowType() &&
            (session_.panelRefreshPending() || session_.viewerRefreshPending()))
            session_.refreshHeldViewer();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace bloom::ui
