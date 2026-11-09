#pragma once

// #513: runs a Stabilize node's tracking job. The UI thread reads what the job needs from the
// document (the node's point, pattern and search, the frame range), submits one cancellable task to
// the scheduler and returns; the task compiles the picture at the node's input, evaluates it on the
// CPU reference path frame by frame and follows the feature (runtime/point_tracker_job.hpp). Nothing
// of that runs on the UI thread. The finished track is stored as one undoable edit: a data block of
// the positions and the keys on the node's position (commands/track_operations.hpp).
//
// One job runs at a time. Progress, the reason a track ended and a cancel are all surfaced through
// status() and changed().

#include <bloom/document/ids.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::document {
class NodeDefinitionRegistry;
}

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

class TrackController final : public QObject {
    Q_OBJECT

  public:
    enum class State : std::uint8_t { Idle, Running, Stored, Stopped, Failed, Cancelled };

    struct Status final {
        State state = State::Idle;
        document::NodeId node;
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        // What the last job did, in plain words (empty while idle).
        QString message;
    };

    TrackController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                    TaskUiBridge& bridge, const document::NodeDefinitionRegistry& registry,
                    QObject* parent = nullptr);
    ~TrackController() override;

    [[nodiscard]] Status status() const;
    [[nodiscard]] bool running() const noexcept { return state_ == State::Running; }

    // Starts tracking the picture at the input of `node` from the current frame to the end of the
    // composition. False, with the reason in status(), when it cannot start.
    bool start(document::NodeId node);
    // Asks the running job to stop; the track found so far is not stored.
    void cancel();

  signals:
    void changed();

  private:
    struct Outcome;
    struct Shared;
    void consume();
    void finish(State state, QString message);

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    const document::NodeDefinitionRegistry& registry_;
    State state_ = State::Idle;
    document::NodeId node_;
    QString message_;
    std::shared_ptr<Shared> shared_;
    runtime::TaskHandle<std::shared_ptr<Outcome>> handle_;
    document::ParameterId position_;
    std::uint32_t rateNumerator_ = 24;
    std::uint32_t rateDenominator_ = 1;
    std::string settings_;
};

} // namespace bloom::ui
