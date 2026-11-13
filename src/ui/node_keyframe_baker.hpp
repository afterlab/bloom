#pragma once

// #636 Bake to keyframes in the Node editor: a number output of the selected node, read at every
// frame of the composition and stored as keyframes on a new Number node beside it. The frames are
// read on a worker (the project as it was when the menu item was chosen), never on the UI thread;
// the status bar shows how far the read is, and Cancel on the same menu stops it. The stored keys
// are then one command, so one undo step, and a refusal reaches the user as the session's
// rejection message. The same read and the same command are the MCP, Python and CLI operation
// `bloom.node.bake-to-keyframes`.

#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QTimer;

namespace bloom::ui {

class CompositionSession;

class NodeKeyframeBaker final : public QObject {
    Q_OBJECT

  public:
    explicit NodeKeyframeBaker(CompositionSession& session, QObject* parent = nullptr);
    ~NodeKeyframeBaker() override;

    // The node's number outputs, as {port, label}; empty when it has none to bake.
    [[nodiscard]] static std::vector<std::pair<std::string, QString>>
    outputs(const document::Composition& composition, document::NodeId node);
    // Starts the bake; one at a time, a new request cancels the one running.
    void request(document::NodeId node, const std::string& port);
    // Stops the bake that is running; nothing is written.
    void cancel();
    [[nodiscard]] bool busy() const noexcept { return active_; }

  signals:
    // The Number node the keys were written to, after the command has been applied.
    void baked(bloom::document::NodeId numberNode);

  private:
    void poll();

    struct Result;
    CompositionSession& session_;
    QTimer* timer_;
    std::shared_ptr<runtime::TaskScheduler> scheduler_;
    std::shared_ptr<std::atomic_bool> retire_;
    runtime::TaskHandle<std::shared_ptr<Result>> task_;
    bool active_ = false;
    document::Vec2d position_;
};

} // namespace bloom::ui
