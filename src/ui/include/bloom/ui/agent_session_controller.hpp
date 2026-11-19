#pragma once

// An agent's session mode in the desktop application (issue #746; docs/user-guide/mcp.md, "Agent
// sessions"). MCP's session.begin and session.end reach it through McpLiveUiPort, on the UI
// thread. While a session is open:
// - the agent's commits group into one undo step labelled with the session (`groupUndo`,
//   commands::CommandStack::beginGroup): the artist's own edit inside the session is a step of
//   its own and the session's next commit starts a new step after it;
// - the panels catch up with the agent's commits at most once per kPanelCadence, and with
//   `deferViewer` the Viewer re-renders only when the session ends, on demand (an agent's Viewer
//   tool) or when the artist presses a key or a mouse button anywhere in Bloom
//   (CompositionSession::setAgentRefreshHold);
// - with `quiet` the agent's renders and the compositions it breaks are told once, when the
//   session ends (UiFeedback::holdAgentActivity); without it they are summarized per quiet
//   interval as any agent work is.
// One session is open at a time. It closes on session.end, when the project is replaced (here, on
// CompositionSession::documentRebound), and when the MCP server closes it for its client's
// disconnect, its idle timeout or the server's stop. Nothing here opens a dialog.

#include <bloom/commands/command_stack.hpp>
#include <bloom/mcp/live_ui_port.hpp>

#include <QObject>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace bloom::ui {

class CompositionSession;
class UiFeedback;

class AgentSessionController final : public QObject {
    Q_OBJECT

  public:
    // How often the panels catch up with an open session's commits.
    static constexpr std::chrono::milliseconds kPanelCadence{1000};
    // Closed sessions remembered per client, so a late session.end names why its session closed.
    static constexpr std::size_t kRememberedClosed = 16;

    // `stack` answers the live project's command stack (null while a project opens).
    AgentSessionController(CompositionSession& session,
                           std::function<commands::CommandStack*()> stack,
                           QObject* parent = nullptr);
    ~AgentSessionController() override;
    AgentSessionController(const AgentSessionController&) = delete;
    AgentSessionController& operator=(const AgentSessionController&) = delete;

    // The application's feedback service; null leaves feedback alone.
    void setFeedback(UiFeedback* feedback) noexcept { feedback_ = feedback; }

    [[nodiscard]] mcp::McpAgentSessionResult begin(const mcp::McpAgentSessionBegin& request);
    [[nodiscard]] mcp::McpAgentSessionResult end(const mcp::McpAgentSessionEnd& request);
    [[nodiscard]] std::optional<mcp::McpAgentSessionView> current() const;
    [[nodiscard]] bool sessionOpen() const noexcept { return open_.has_value(); }

    // The artist's input (a press, a key, the wheel, a touch) catches the held panels and a
    // deferred Viewer up before it lands.
    bool eventFilter(QObject* watched, QEvent* event) override;

  Q_SIGNALS:
    // A session began or closed.
    void changed();

  private:
    struct Open final {
        mcp::McpAgentSessionView view;
        std::chrono::steady_clock::time_point began;
        commands::CommandStack* stack = nullptr;
        commands::CommandObserverId observer = 0;
        std::shared_ptr<int> life;
        commands::HistoryGroupStats stats;
    };

    [[nodiscard]] static QString displayName(const mcp::McpAgentSessionView& view);
    mcp::McpAgentSessionView close(mcp::McpAgentSessionClose cause, bool stackAlive);

    CompositionSession& session_;
    std::function<commands::CommandStack*()> stack_;
    UiFeedback* feedback_ = nullptr;
    std::optional<Open> open_;
    std::map<std::uint64_t, mcp::McpAgentSessionView> closed_;
    std::uint64_t next_ = 1;
    bool filtering_ = false;
};

} // namespace bloom::ui
