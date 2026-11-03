#include <bloom/ui/channel_set_controller.hpp>

#include <bloom/document/channel_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QTimer>

#include <algorithm>
#include <utility>

namespace bloom::ui {

ChannelSetController::ChannelSetController(CompositionSession& session,
                                           runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                           ListingProvider provider, QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), bridge_(bridge),
      provider_(std::move(provider)) {
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &ChannelSetController::poll);
    session_.setChannelSetController(this);
}

ChannelSetController::~ChannelSetController() {
    cancel();
    session_.setChannelSetController(nullptr);
}

void ChannelSetController::cancel() {
    for (auto& [node, task] : tasks_)
        task.handle.cancel();
    tasks_.clear();
}

std::optional<std::string_view> ChannelSetController::inputPort(const std::string_view typeId) {
    if (typeId == document::kChannelCopyNodeType)
        return document::kChannelCopyFromPort;
    if (typeId == document::kChannelShuffleNodeType || typeId == document::kChannelRemoveNodeType)
        return document::kChannelSetPort;
    if (typeId == document::kCompositionOutputNodeType)
        return document::kCompositionOutputChannelsInputPort;
    return std::nullopt;
}

void ChannelSetController::request(const document::NodeId node) {
    const auto revision = session_.snapshot().revision();
    if (const auto known = listings_.find(node);
        known != listings_.end() && known->second.revision == revision)
        return;
    if (const auto pending = tasks_.find(node); pending != tasks_.end()) {
        if (pending->second.revision == revision)
            return;
        pending->second.handle.cancel();
        tasks_.erase(pending);
    }
    const auto* composition = session_.composition();
    const auto* record = composition != nullptr ? composition->graph().findNode(node) : nullptr;
    const auto port = record != nullptr ? inputPort(record->typeId) : std::nullopt;
    if (!port)
        return;
    const auto edges = composition->graph().edges();
    const auto edge = std::ranges::find_if(edges, [&](const document::EdgeRecord& candidate) {
        const auto* input = std::get_if<document::NodeInputRef>(&candidate.destination);
        return input != nullptr && input->nodeId == node && input->port == *port;
    });
    if (edge == edges.end()) {
        runtime::ChannelInspectionListing empty;
        empty.diagnostic = "Connect a channel set";
        listings_.insert_or_assign(node, Listing{revision, std::move(empty)});
        return;
    }
    if (!provider_ || bridge_.isShuttingDown())
        return;
    const auto compositionId = session_.compositionId();
    auto submission = scheduler_.submit<std::shared_ptr<const runtime::ChannelInspectionListing>>(
        runtime::TaskRequest("List channel set",
                             {.kind = runtime::TaskOwnerKind::PanelRequest,
                              .id = runtime::TaskOwnerId::fromRaw(compositionId.value())},
                             runtime::TaskPriority::Visible),
        [provider = provider_, snapshot = session_.snapshot(), compositionId, source = edge->source,
         time = session_.currentTime(),
         colorIntent = session_.colorIntent()](runtime::TaskContext& context) {
            using Result =
                runtime::TaskResult<std::shared_ptr<const runtime::ChannelInspectionListing>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto listing = provider(snapshot, compositionId, source.nodeId, source.port, time,
                                    colorIntent, context.cancellation());
            if (context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(
                std::make_shared<const runtime::ChannelInspectionListing>(std::move(listing)));
        });
    if (submission.accepted()) {
        tasks_.emplace(node, Pending{revision, std::move(submission.handle)});
        bridge_.wake();
    }
}

const runtime::ChannelInspectionListing*
ChannelSetController::listing(const document::NodeId node) const {
    const auto found = listings_.find(node);
    return found != listings_.end() ? &found->second.value : nullptr;
}

bool ChannelSetController::listingCurrent(const document::NodeId node) const {
    const auto found = listings_.find(node);
    return found != listings_.end() && found->second.revision == session_.snapshot().revision();
}

void ChannelSetController::poll() {
    bool any = false;
    for (auto task = tasks_.begin(); task != tasks_.end();) {
        auto result = task->second.handle.tryTakeResult();
        if (!result) {
            ++task;
            continue;
        }
        if (result->state() == runtime::TaskState::Succeeded && result->value() &&
            *result->value()) {
            listings_.insert_or_assign(task->first,
                                       Listing{task->second.revision, **result->value()});
        } else {
            // A failed read is kept as an empty listing for its revision, so a picker refresh does
            // not resubmit it until the document changes.
            runtime::ChannelInspectionListing failed;
            failed.diagnostic = "The channel set could not be listed";
            listings_.insert_or_assign(task->first,
                                       Listing{task->second.revision, std::move(failed)});
        }
        task = tasks_.erase(task);
        any = true;
    }
    // A worker that finished between the bridge wake and this poll is collected on the next one.
    if (!tasks_.empty())
        QTimer::singleShot(20, &bridge_, &TaskUiBridge::wake);
    if (any)
        emit changed();
}

} // namespace bloom::ui
