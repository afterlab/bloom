#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <thread>

namespace bloom::host {
class GpuExportProvider;
}
namespace bloom::scripting {
class LiveSessionHost;
}

namespace bloom::ui {

// One console per editor registry. Python and native render work never run in a Qt callback.
// The console reads and commits through the shared scripting::LiveSessionHost (ADR 0034): one
// mirror, one 16-commit queue, one generation counter and one revocation rule for the Script editor
// and the in-app MCP server. The application-lifetime LiveSessionAdapter feeds the host and drains
// its commits on the UI thread.
class ScriptPanelRuntime final : public QObject {
    Q_OBJECT
  public:
    ScriptPanelRuntime(scripting::LiveSessionHost& live,
                       std::shared_ptr<host::GpuExportProvider> provider);
    ~ScriptPanelRuntime() override;
    [[nodiscard]] bool submit(const QString& source);
    // As submit, and `done` is called once on the UI thread (from the runtime's poll) with the
    // script's output and whether it succeeded: the agent route's completion (script.run).
    [[nodiscard]] bool submit(const QString& source,
                              std::function<void(const QString& output, bool succeeded)> done);
    void cancel();
    void shutdown();
    [[nodiscard]] bool busy() const;

  signals:
    void outputReady(QString source, QString output, bool succeeded);
    void activityChanged(QString message, bool busy);

  private:
    struct State;
    void poll();
    static void work(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
    std::jthread worker_;
};
} // namespace bloom::ui
