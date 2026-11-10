#pragma once

// #637 Write to a Path in the Node editor: the selected Path-producing node's result at the
// playhead, stored as points in a Path node. The node is evaluated on a worker (the playhead's time
// and the project as they were when the menu item was chosen), never on the UI thread; the stored
// result is then one command, so one undo step, and a refusal reaches the user as the session's
// rejection message. The same evaluation and the same command are the MCP, Python and CLI operation
// `bloom.node.write-to-path`.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>

#include <atomic>
#include <memory>
#include <optional>

class QTimer;

namespace bloom::ui {

class CompositionSession;

class NodePathWriter final : public QObject {
    Q_OBJECT

  public:
    explicit NodePathWriter(CompositionSession& session, QObject* parent = nullptr);
    ~NodePathWriter() override;

    // Whether the node has a Path output to write.
    [[nodiscard]] static bool canWrite(const document::Composition& composition,
                                       document::NodeId node);
    // Starts the write; one at a time, a new request cancels the one running.
    void request(document::NodeId node, std::optional<document::NodeId> target = std::nullopt);
    [[nodiscard]] bool busy() const noexcept { return active_; }

  signals:
    // The Path node written, after the command has been applied.
    void written(bloom::document::NodeId pathNode);

  private:
    void poll();

    struct Result;
    CompositionSession& session_;
    QTimer* timer_;
    std::shared_ptr<runtime::TaskScheduler> scheduler_;
    std::shared_ptr<std::atomic_bool> retire_;
    runtime::TaskHandle<std::shared_ptr<Result>> task_;
    bool active_ = false;
    document::NodeId node_;
    std::optional<document::NodeId> target_;
    core::RationalTime time_;
    document::Vec2d position_;
};

} // namespace bloom::ui
