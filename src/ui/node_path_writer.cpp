#include "node_path_writer.hpp"

#include <bloom/commands/path_write_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/worker_thread.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/scripting/node_path_query.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QTimer>

#include <algorithm>
#include <chrono>
#include <thread>

namespace bloom::ui {

struct NodePathWriter::Result final {
    scripting::NodePathQuery query;
};

NodePathWriter::NodePathWriter(CompositionSession& session, QObject* parent)
    : QObject(parent), session_(session), timer_(new QTimer(this)) {
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
}

NodePathWriter::~NodePathWriter() {
    if (!scheduler_)
        return;
    task_.cancel();
    scheduler_->beginShutdown();
    retire_->store(true);
    retire_->notify_one();
}

bool NodePathWriter::canWrite(const document::Composition& composition, const document::NodeId node) {
    const auto* record = composition.graph().findNode(node);
    const auto* definition =
        record != nullptr
            ? document::builtInNodeDefinitions().find(record->typeId, record->schemaVersion)
            : nullptr;
    return definition != nullptr &&
           std::ranges::any_of(definition->outputs, [](const auto& output) {
               return output.valueKind == document::SocketValueKind::Path;
           });
}

void NodePathWriter::request(const document::NodeId node,
                             const std::optional<document::NodeId> target) {
    const auto* composition = session_.composition();
    if (composition == nullptr || !canWrite(*composition, node))
        return;
    if (active_)
        task_.cancel();
    if (!scheduler_) {
        auto config = runtime::TaskSchedulerConfig::defaults();
        config.cpuWorkerCount = 1;
        config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
        scheduler_ = std::make_shared<runtime::TaskScheduler>(config);
        retire_ = std::make_shared<std::atomic_bool>(false);
        // The Qt-free reaper: closing the editor only requests cancellation and never joins.
        bloom::core::WorkerThread([scheduler = scheduler_, retire = retire_] {
            retire->wait(false);
            while (!scheduler->isQuiescent())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }).detach();
    }
    node_ = node;
    target_ = target;
    time_ = session_.currentTime();
    position_ = {0.0, 0.0};
    if (const auto found = composition->nodeLayout().find(node);
        found != composition->nodeLayout().end())
        position_ = {found->second.position.x + found->second.width + 64.0, found->second.position.y};
    const auto snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    const auto time = time_;
    const auto submission = scheduler_->submit<std::shared_ptr<Result>>(
        runtime::TaskRequest{"Write To Path",
                             {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Interactive},
        [snapshot, compositionId, node, time](runtime::TaskContext& context) {
            auto result = std::make_shared<Result>();
            result->query = scripting::queryNodePath(snapshot, compositionId, node, {}, time,
                                                     context.cancellation());
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Result>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<Result>>::succeeded(std::move(result));
        });
    task_ = submission.handle;
    active_ = submission.accepted();
    if (active_)
        timer_->start();
    else
        emit session_.commandRejected(tr("Write to a Path could not start"));
}

void NodePathWriter::poll() {
    auto result = task_.tryTakeResult();
    if (!result)
        return;
    timer_->stop();
    active_ = false;
    if (result->state() == runtime::TaskState::Cancelled)
        return;
    if (!result->value() || !(*result->value())->query.path) {
        const auto detail = result->value() ? (*result->value())->query.diagnostic : std::string{};
        emit session_.commandRejected(
            tr("Write to a Path: %1")
                .arg(detail.empty() ? tr("the node produced no path")
                                    : QString::fromStdString(detail)));
        return;
    }
    commands::Transaction transaction("Write to a Path", session_.snapshot().revision());
    transaction.emplace<commands::WriteValueToPath>(session_.compositionId(), target_,
                                                    std::move(*(*result->value())->query.path),
                                                    position_, time_);
    const auto done = session_.executeNodeTransaction(std::move(transaction));
    if (const auto written = done.outputId<document::NodeId>(commands::kWriteToPathNodeOutput))
        emit this->written(*written);
}

} // namespace bloom::ui
