#include "node_keyframe_baker.hpp"

#include <bloom/commands/keyframe_bake_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/worker_thread.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/scripting/node_path_query.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QTimer>

#include <chrono>
#include <thread>

namespace bloom::ui {

struct NodeKeyframeBaker::Result final {
    scripting::NodeScalarKeysQuery query;
};

NodeKeyframeBaker::NodeKeyframeBaker(CompositionSession& session, QObject* parent)
    : QObject(parent), session_(session), timer_(new QTimer(this)) {
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
}

NodeKeyframeBaker::~NodeKeyframeBaker() {
    if (!scheduler_)
        return;
    task_.cancel();
    scheduler_->beginShutdown();
    retire_->store(true);
    retire_->notify_one();
}

std::vector<std::pair<std::string, QString>>
NodeKeyframeBaker::outputs(const document::Composition& composition, const document::NodeId node) {
    std::vector<std::pair<std::string, QString>> found;
    const auto* record = composition.graph().findNode(node);
    const auto* definition =
        record != nullptr
            ? document::builtInNodeDefinitions().find(record->typeId, record->schemaVersion)
            : nullptr;
    if (definition == nullptr)
        return found;
    for (const auto& output : definition->outputs)
        if (output.valueKind == document::SocketValueKind::Scalar)
            found.emplace_back(output.name, QString::fromStdString(output.name));
    return found;
}

void NodeKeyframeBaker::request(const document::NodeId node, const std::string& port) {
    const auto* composition = session_.composition();
    if (composition == nullptr || outputs(*composition, node).empty())
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
    position_ = {0.0, 0.0};
    if (const auto found = composition->nodeLayout().find(node);
        found != composition->nodeLayout().end())
        position_ = {found->second.position.x + found->second.width + 64.0,
                     found->second.position.y};
    const auto snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    const auto base = session_.projectDirectory();
    const auto submission = scheduler_->submit<std::shared_ptr<Result>>(
        runtime::TaskRequest{"Bake To Keyframes",
                             {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Interactive},
        [snapshot, compositionId, node, port, base](runtime::TaskContext& context) {
            auto result = std::make_shared<Result>();
            result->query = scripting::queryNodeScalarKeys(snapshot, compositionId, node, port,
                                                           context.cancellation(), base);
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Result>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<Result>>::succeeded(std::move(result));
        });
    task_ = submission.handle;
    active_ = submission.accepted();
    if (active_)
        timer_->start();
    else
        emit session_.commandRejected(tr("Bake to keyframes could not start"));
}

void NodeKeyframeBaker::cancel() {
    if (active_)
        task_.cancel();
}

void NodeKeyframeBaker::poll() {
    auto result = task_.tryTakeResult();
    if (!result)
        return;
    timer_->stop();
    active_ = false;
    if (result->state() == runtime::TaskState::Cancelled ||
        (result->value() && (*result->value())->query.cancelled))
        return;
    if (!result->value() || (*result->value())->query.keys.empty()) {
        const auto detail = result->value() ? (*result->value())->query.diagnostic : std::string{};
        emit session_.commandRejected(
            tr("Bake to keyframes: %1")
                .arg(detail.empty() ? tr("the node produced no numbers")
                                    : QString::fromStdString(detail)));
        return;
    }
    commands::Transaction transaction("Bake to keyframes", session_.snapshot().revision());
    transaction.emplace<commands::BakeValueToKeyframes>(
        session_.compositionId(), std::move((*result->value())->query.keys), position_);
    const auto done = session_.executeNodeTransaction(std::move(transaction));
    if (const auto node = done.outputId<document::NodeId>(commands::kBakeToKeyframesNodeOutput))
        emit this->baked(*node);
}

} // namespace bloom::ui
