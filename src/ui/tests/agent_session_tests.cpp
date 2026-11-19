// #746: an agent's session mode in the desktop application. AgentSessionController over a real
// document, command stack and CompositionSession, with UiFeedback over fakes: one undo step for a
// session's commits, the artist's own edit inside one, the panels' slow catch-up and the deferred
// Viewer, the artist's interaction, the quiet summary, and every way a session closes.
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/agent_session_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ui_feedback.hpp>

#include <QApplication>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QWidget>
#include <QWindow>

#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace commands = bloom::commands;
namespace document = bloom::document;
namespace mcp = bloom::mcp;
namespace ui = bloom::ui;
using bloom::core::Color4d;
using bloom::core::RationalTime;

int failures = 0;

void expect(const bool condition, const std::string_view message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

class FakePlayer final : public ui::SoundPlayer {
  public:
    void play(const ui::FeedbackSound sound, float) noexcept override { played.push_back(sound); }
    void shutdown() noexcept override {}
    std::vector<ui::FeedbackSound> played;
};

class FakeNotifier final : public ui::Notifier {
  public:
    void post(const ui::SystemNotification& notification) override {
        posted.push_back(notification);
    }
    void setActivationHandler(std::function<void()>) override {}
    std::vector<ui::SystemNotification> posted;
};

struct Rig final {
    document::NewProject seed =
        document::makeNewProject("Session Test", "Main", RationalTime::fromInteger(10));
    document::Document doc{std::move(seed.project)};
    std::unique_ptr<commands::CommandStack> stack = std::make_unique<commands::CommandStack>(doc);
    ui::CompositionSession session{doc, *stack, seed.initialCompositionId};
    FakePlayer* player = nullptr;
    FakeNotifier* notifier = nullptr;
    bool active = false;
    std::unique_ptr<ui::UiFeedback> feedback;
    ui::AgentSessionController controller{session, [this] { return stack.get(); }};

    Rig() {
        auto fakePlayer = std::make_unique<FakePlayer>();
        auto fakeNotifier = std::make_unique<FakeNotifier>();
        player = fakePlayer.get();
        notifier = fakeNotifier.get();
        feedback = std::make_unique<ui::UiFeedback>(std::move(fakePlayer), std::move(fakeNotifier),
                                                    [this] { return active; });
        controller.setFeedback(feedback.get());
    }

    // An agent's commit, as the live session marshal applies it.
    bool agentAddsLayer(const std::string& name) {
        commands::Transaction transaction("Agent edit", doc.snapshot().revision());
        transaction.setOrigin(commands::CommandOrigin::Agent);
        transaction.emplace<commands::AddSolidLayer>(seed.initialCompositionId, name,
                                                     Color4d{1.0, 0.0, 0.0, 1.0});
        return session.executeTransaction(std::move(transaction)).changed();
    }
    std::size_t layers() const {
        return doc.snapshot()
            .project()
            .findComposition(seed.initialCompositionId)
            ->graph()
            .layerOutputs()
            .size();
    }
    mcp::McpAgentSessionResult begin(const std::uint64_t client = 7,
                                     mcp::McpAgentSessionOptions options = {}) {
        return controller.begin({.client = client,
                                 .label = "Shot 12",
                                 .options = options,
                                 .idleTimeout = mcp::kMcpAgentSessionDefaultIdle});
    }
    mcp::McpAgentSessionResult
    end(const std::uint64_t client = 7,
        const mcp::McpAgentSessionClose cause = mcp::McpAgentSessionClose::Ended) {
        return controller.end({.client = client, .session = 0, .cause = cause});
    }
};

// A session's commits are one undo step labelled with the session; undoing it takes all of them.
void groupedUndo() {
    Rig rig;
    const auto before = rig.layers();
    expect(rig.begin().ok(), "session.begin opens a session");
    for (int index = 0; index < 5; ++index)
        expect(rig.agentAddsLayer("Agent " + std::to_string(index)), "an agent commit lands");
    expect(rig.stack->size() == 1 &&
               rig.stack->undoLabel() == std::string_view("Agent session “Shot 12”"),
           "five commits are one undo step labelled with the session");
    const auto ended = rig.end();
    expect(ended.ok() && ended.session.commits == 5 && ended.session.undoSteps == 1 &&
               ended.session.closed == mcp::McpAgentSessionClose::Ended,
           "session.end reports the commits and the one undo step");
    expect(!rig.stack->groupOpen(), "the undo group closes with the session");
    expect(rig.stack->undo().changed() && rig.layers() == before,
           "one undo takes back the whole session");
}

// The artist's own edit inside a session is its own step; nothing is folded or lost.
void artistEditInsideSession() {
    Rig rig;
    expect(rig.begin().ok(), "a session opens");
    expect(rig.agentAddsLayer("Agent A"), "the agent commits");
    expect(rig.session.snapshot().revision() != rig.session.liveSnapshot().revision(),
           "the window is held behind the agent's commit");
    // Built against what the window shows: it catches the window up and lands, never stale.
    expect(rig.session.addSolidLayer(QStringLiteral("Artist"), Color4d{0.0, 1.0, 0.0, 1.0}),
           "the artist edits inside the session");
    expect(rig.agentAddsLayer("Agent B"), "the agent commits again");
    const auto ended = rig.end();
    expect(ended.session.commits == 2 && ended.session.undoSteps == 2 &&
               ended.session.artistEdits == 1,
           "the artist's edit split the session into two steps around its own");
    expect(rig.stack->size() == 3, "three steps: agent, artist, agent");
    expect(rig.stack->undo().changed() && rig.layers() == 2,
           "undo takes the session's second step back");
    expect(rig.stack->undo().changed() && rig.layers() == 1,
           "then the artist's own edit, separately");
}

// The panels catch up at a slow cadence and the Viewer is not re-rendered per commit; it refreshes
// on the session's end, on demand and on the artist's interaction.
void deferredRefresh() {
    Rig rig;
    QSignalSpy snapshots(&rig.session, &ui::CompositionSession::snapshotChanged);
    QSignalSpy evaluations(&rig.session, &ui::CompositionSession::documentEvaluationChanged);
    expect(rig.begin().ok(), "a session opens");
    const auto shown = rig.session.snapshot().revision();
    expect(rig.agentAddsLayer("A") && rig.agentAddsLayer("B") && rig.agentAddsLayer("C"),
           "three agent commits");
    expect(snapshots.count() == 0 && rig.session.snapshot().revision() == shown,
           "the panels do not rebuild for each agent commit");
    expect(rig.session.liveSnapshot().revision() > shown,
           "the agent reads and commits against the newest revision");
    rig.session.refreshHeldPanels();
    expect(snapshots.count() == 1 &&
               rig.session.snapshot().revision() == rig.session.liveSnapshot().revision(),
           "the panels catch up once for all three commits");
    expect(evaluations.count() == 0 && rig.session.viewerRefreshPending(),
           "the Viewer still shows what it showed");

    // The artist presses a key: the Viewer refreshes before the key lands.
    // Input sent to a widget directly is not the window's delivery and changes nothing.
    QWidget widget;
    widget.show();
    QKeyEvent toWidget(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &toWidget);
    expect(evaluations.count() == 0, "only the window's delivery of the input refreshes");
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(widget.windowHandle(), &press);
    expect(evaluations.count() == 1 && !rig.session.viewerRefreshPending(),
           "the artist's interaction refreshes the Viewer");

    expect(rig.agentAddsLayer("D"), "another agent commit");
    expect(rig.session.viewerRefreshPending(), "it waits again");
    static_cast<void>(rig.end());
    expect(!rig.session.viewerRefreshPending() && evaluations.count() == 2 &&
               rig.session.snapshot().revision() == rig.session.liveSnapshot().revision(),
           "the session's end refreshes the panels and the Viewer");
    expect(!rig.session.agentRefreshHold().has_value(), "the hold is gone");

    // An artist edit adopts at once, whatever the session holds.
    expect(rig.begin(8).ok(), "a second session opens");
    expect(rig.agentAddsLayer("E"), "an agent commit waits");
    expect(rig.session.addSolidLayer(QStringLiteral("Artist"), Color4d{0.0, 0.0, 1.0, 1.0}) &&
               rig.session.snapshot().revision() == rig.session.liveSnapshot().revision() &&
               !rig.session.viewerRefreshPending(),
           "the artist's own edit shows everything at once");
    static_cast<void>(rig.end(8));

    // deferViewer false: the panels still catch up slowly, but the Viewer follows each adoption.
    expect(rig.begin(9, {.quiet = true, .deferViewer = false, .groupUndo = true}).ok(),
           "a session without a deferred Viewer opens");
    const auto evaluationsBefore = evaluations.count();
    expect(rig.agentAddsLayer("F"), "an agent commit");
    rig.session.refreshHeldPanels();
    expect(evaluations.count() == evaluationsBefore + 1 && !rig.session.viewerRefreshPending(),
           "the Viewer refreshes with the panels");
    static_cast<void>(rig.end(9));
}

// quiet: the agent's activity is told once, when the session ends, under its name.
void quietSummary() {
    Rig rig;
    rig.active = false;
    expect(rig.begin().ok(), "a quiet session opens");
    expect(rig.feedback->agentActivityHeld(), "the session holds agent feedback");
    rig.feedback->noteAgentRender(true);
    rig.feedback->noteAgentRender(false);
    rig.feedback->flushAgentActivity(true);
    expect(rig.notifier->posted.empty() && rig.player->played.empty(),
           "nothing is told while the session is open");
    static_cast<void>(rig.end());
    expect(rig.notifier->posted.size() == 1 &&
               rig.notifier->posted[0].body ==
                   QStringLiteral("Agent session “Shot 12”: 2 renders, 1 failed"),
           "one summary when the session ends");
    expect(!rig.feedback->agentActivityHeld(), "the hold ends with the session");

    // Not quiet: C1's summaries apply as for any agent work.
    expect(rig.begin(8, {.quiet = false, .deferViewer = true, .groupUndo = true}).ok(),
           "a session that is not quiet opens");
    expect(!rig.feedback->agentActivityHeld(), "it holds nothing");
    static_cast<void>(rig.end(8));
}

// Every way a session closes, and the refusals around them.
void closes() {
    // The project the session's project is replaced with; it outlives the rig bound to it.
    auto replacement = document::makeNewProject("Other", "Main", RationalTime::fromInteger(10));
    document::Document other(std::move(replacement.project));
    Rig rig;
    expect(rig.begin(7).ok(), "client 7 opens a session");
    const auto busy = rig.begin(8);
    expect(!busy.ok() && busy.refusal == mcp::McpUiRefusal::Busy,
           "another client cannot open a second session");
    expect(!rig.begin(7).ok(), "nor can the same client open two");
    const auto wrong = rig.end(8);
    expect(!wrong.ok() && wrong.refusal == mcp::McpUiRefusal::InvalidArgument,
           "another client cannot end it");

    // Ended by the server for its client's disconnect, idle timeout and the server's stop.
    for (const auto cause :
         {mcp::McpAgentSessionClose::ClientDisconnected, mcp::McpAgentSessionClose::IdleTimeout,
          mcp::McpAgentSessionClose::ServerStopped}) {
        if (!rig.controller.sessionOpen())
            expect(rig.begin(7).ok(), "a session opens");
        expect(rig.agentAddsLayer("x"), "the agent commits");
        const auto closed = rig.end(7, cause);
        expect(closed.ok() && closed.session.closed == cause && !rig.stack->groupOpen() &&
                   !rig.session.agentRefreshHold(),
               "the server closes the session with its cause, sealing the undo step");
        const auto late = rig.end(7);
        expect(!late.ok() && late.message.find("already closed") != std::string::npos,
               "a later session.end says it already closed and why");
    }

    // Project replacement: the session closes on the rebind and never touches the old stack.
    expect(rig.begin(7).ok(), "a session opens before the project is replaced");
    expect(rig.agentAddsLayer("y"), "the agent commits");
    auto otherStack = std::make_unique<commands::CommandStack>(other);
    rig.session.rebind(other, *otherStack, replacement.initialCompositionId);
    rig.stack.reset();
    expect(!rig.controller.sessionOpen(), "a replaced project closes the session");
    const auto after = rig.end(7);
    expect(!after.ok() && after.message.find("the project was replaced") != std::string::npos,
           "session.end afterwards names the replacement");
    rig.stack = std::move(otherStack);
    expect(rig.begin(7).ok(), "a new session opens on the new project");
    static_cast<void>(rig.end(7));
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    groupedUndo();
    artistEditInsideSession();
    deferredRefresh();
    quietSummary();
    closes();
    if (failures == 0)
        std::cout << "agent sessions: grouped undo, artist edits, deferred refresh, quiet summary "
                     "and every close passed\n";
    return failures == 0 ? 0 : 1;
}
