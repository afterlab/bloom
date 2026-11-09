#include <bloom/ui/track_controller.hpp>

#include <bloom/commands/track_operations.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/runtime/point_tracker_job.hpp>

#include <QDateTime>
#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <variant>

namespace bloom::ui {

struct TrackController::Shared final {
    std::atomic<std::uint64_t> done{0};
    std::atomic<std::uint64_t> total{0};
};

struct TrackController::Outcome final {
    runtime::PointTrackResult track;
    // Set when the picture could not be prepared at all.
    std::string failure;
};

namespace {

constexpr std::uint32_t kLongestEvaluatedSide = 1920;

[[nodiscard]] std::optional<document::Vec2d> constantPoint(const document::Composition& composition,
                                                           const document::NodeRecord& node,
                                                           const std::string_view role) {
    for (const auto& binding : node.parameters)
        if (binding.role == role)
            if (const auto* record = composition.parameters().find(binding.parameterId))
                if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source))
                    if (const auto* point = std::get_if<document::Vec2d>(&constant->value))
                        return *point;
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> constantChoice(const document::Composition& composition,
                                                         const document::NodeRecord& node,
                                                         const std::string_view role) {
    for (const auto& binding : node.parameters)
        if (binding.role == role)
            if (const auto* record = composition.parameters().find(binding.parameterId))
                if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source))
                    if (const auto* choice = std::get_if<std::int64_t>(&constant->value))
                        return *choice;
    return std::nullopt;
}

} // namespace

TrackController::TrackController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                                 TaskUiBridge& bridge,
                                 const document::NodeDefinitionRegistry& registry, QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), bridge_(bridge),
      registry_(registry) {
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, [this] { consume(); });
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, [this] {
        consume();
        if (state_ == State::Running)
            emit changed();
    });
    session_.setTrackController(this);
}

TrackController::~TrackController() {
    session_.setTrackController(nullptr);
    if (state_ == State::Running)
        handle_.cancel();
}

TrackController::Status TrackController::status() const {
    Status result;
    result.state = state_;
    result.node = node_;
    if (shared_) {
        result.done = shared_->done.load(std::memory_order_relaxed);
        result.total = shared_->total.load(std::memory_order_relaxed);
    }
    result.message = message_;
    return result;
}

void TrackController::finish(const State state, QString message) {
    state_ = state;
    message_ = std::move(message);
    emit changed();
}

bool TrackController::start(const document::NodeId node) {
    if (state_ == State::Running)
        return false;
    node_ = node;
    shared_.reset();
    const auto refuse = [&](QString why) {
        finish(State::Failed, std::move(why));
        return false;
    };
    const auto* composition = session_.composition();
    const auto* record = composition == nullptr ? nullptr : composition->graph().findNode(node);
    if (record == nullptr || record->typeId != document::kStabilizeNodeType)
        return refuse(tr("This node cannot be tracked."));
    const auto input = runtime::trackTapFor(*composition, node);
    if (!input)
        return refuse(tr("Connect a picture to the node's input to track it."));
    if (composition->nodeLocked(node))
        return refuse(tr("The node is locked."));
    const auto point = constantPoint(*composition, *record, "trackPoint");
    if (!point)
        return refuse(tr("The Track Point is animated. Set it to a fixed value on the frame to "
                         "start from."));
    const auto pattern = constantChoice(*composition, *record, "pattern");
    const auto search = constantChoice(*composition, *record, "search");
    if (!pattern || !search)
        return refuse(tr("The tracking settings are not valid."));
    document::ParameterId position;
    for (const auto& binding : record->parameters)
        if (binding.role == "tracked")
            position = binding.parameterId;
    if (!position.isValid())
        return refuse(tr("The node has no position to key."));

    const auto format = composition->format();
    const auto rate = format.frameRate();
    const auto rateTime = core::RationalTime::create(rate.numerator(), rate.denominator());
    if (!rateTime)
        return refuse(tr("The composition's frame rate is not valid."));
    const auto durationSeconds = composition->duration().toSeconds();
    const auto frames = static_cast<std::int64_t>(
        std::floor(durationSeconds * static_cast<double>(rate.numerator()) /
                       static_cast<double>(rate.denominator()) +
                   1.0e-9));
    const auto currentSeconds = session_.currentTime().toSeconds();
    const auto first = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(currentSeconds * static_cast<double>(rate.numerator()) /
                                               static_cast<double>(rate.denominator()))),
        0, std::max<std::int64_t>(frames - 1, 0));
    const auto last = frames - 1;
    if (last <= first)
        return refuse(tr("There is no frame after this one to track."));

    runtime::NodeTrackRequest request;
    const double longest = static_cast<double>(std::max(format.width(), format.height()));
    const double scale = std::min(1.0, static_cast<double>(kLongestEvaluatedSide) / longest);
    request.extentWidth = std::max<std::uint32_t>(
        8, static_cast<std::uint32_t>(std::llround(static_cast<double>(format.width()) * scale)));
    request.extentHeight = std::max<std::uint32_t>(
        8, static_cast<std::uint32_t>(std::llround(static_cast<double>(format.height()) * scale)));
    request.compositionWidth = static_cast<double>(format.width());
    request.compositionHeight = static_cast<double>(format.height());
    request.frameRateNumerator = rate.numerator();
    request.frameRateDenominator = rate.denominator();
    request.firstFrame = first;
    request.lastFrame = last;
    request.startX = point->x;
    request.startY = point->y;
    request.patternRadius = std::max(
        4, static_cast<int>(std::lround(document::stabilizePatternRadius(*pattern) * scale)));
    request.searchRadius = std::max(
        2, static_cast<int>(std::lround(document::stabilizeSearchRadius(*search) * scale)));

    position_ = position;
    rateNumerator_ = rate.numerator();
    rateDenominator_ = rate.denominator();
    settings_ = "pattern=" + std::to_string(document::stabilizePatternRadius(*pattern)) +
                ";search=" + std::to_string(document::stabilizeSearchRadius(*search)) +
                ";first=" + std::to_string(first) + ";last=" + std::to_string(last) +
                ";x=" + std::to_string(point->x) + ";y=" + std::to_string(point->y);

    auto shared = std::make_shared<Shared>();
    shared->total.store(static_cast<std::uint64_t>(last - first + 1));
    const auto snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    const auto* registry = &registry_;
    const auto inspection = *input;
    auto submission = scheduler_.submit<std::shared_ptr<Outcome>>(
        runtime::TaskRequest(
            "Track point",
            {.kind = runtime::TaskOwnerKind::Composition,
             .id = runtime::TaskOwnerId::fromRaw(compositionId.value())},
            runtime::TaskPriority::Interactive, runtime::TaskExecutor::Cpu),
        [snapshot, compositionId, registry, inspection, request, shared](
            runtime::TaskContext& context) mutable {
            auto outcome = std::make_shared<Outcome>();
            context.reportProgress({"Tracking", "", 0, shared->total.load()});
            runtime::SnapshotCompiler compiler(*registry);
            const auto compiled =
                compiler.compile({.snapshot = snapshot,
                                  .compositionId = compositionId,
                                  .channelInspection = inspection},
                                 context.cancellation());
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Outcome>>::cancelled();
            if (!compiled.plan) {
                outcome->failure = "The picture at the node's input could not be prepared.";
                return runtime::TaskResult<std::shared_ptr<Outcome>>::succeeded(std::move(outcome));
            }
            request.plan = compiled.plan;
            outcome->track = runtime::trackNodeInput(
                request, context.cancellation(),
                [&](const std::uint64_t done, const std::uint64_t total) {
                    shared->done.store(done, std::memory_order_relaxed);
                    context.reportProgress({"Tracking", "", done, total});
                });
            if (outcome->track.end == runtime::PointTrackEnd::Cancelled)
                return runtime::TaskResult<std::shared_ptr<Outcome>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<Outcome>>::succeeded(std::move(outcome));
        });
    if (!submission.accepted())
        return refuse(tr("Tracking is unavailable while processing capacity is full."));
    shared_ = std::move(shared);
    handle_ = std::move(submission.handle);
    bridge_.wake();
    finish(State::Running, tr("Tracking..."));
    return true;
}

void TrackController::cancel() {
    if (state_ == State::Running)
        handle_.cancel();
}

void TrackController::consume() {
    if (state_ != State::Running)
        return;
    auto result = handle_.tryTakeResult();
    if (!result)
        return;
    if (result->state() == runtime::TaskState::Cancelled) {
        finish(State::Cancelled, tr("Tracking was cancelled. Nothing was changed."));
        return;
    }
    if (result->state() != runtime::TaskState::Succeeded || !result->value().has_value() ||
        !*result->value()) {
        finish(State::Failed, tr("Tracking failed. Nothing was changed."));
        return;
    }
    const auto& outcome = **result->value();
    if (!outcome.failure.empty()) {
        finish(State::Failed, QString::fromStdString(outcome.failure));
        return;
    }
    const auto& track = outcome.track;
    if (track.samples.size() < 2) {
        finish(State::Failed, tr("The feature could not be followed from this frame: %1")
                                  .arg(QString::fromStdString(track.detail)));
        return;
    }
    // The node and its position must still be there: the document may have changed meanwhile.
    const auto* composition = session_.composition();
    const auto* record = composition == nullptr ? nullptr : composition->graph().findNode(node_);
    if (record == nullptr || composition->parameters().find(position_) == nullptr) {
        finish(State::Failed, tr("The node was removed while tracking. Nothing was changed."));
        return;
    }
    std::vector<commands::TrackRow> rows;
    rows.reserve(track.samples.size());
    for (const auto& sample : track.samples)
        rows.push_back({sample.frame, sample.x, sample.y, sample.score});
    commands::TrackStorage storage;
    storage.composition = session_.compositionId();
    storage.node = node_;
    storage.position = position_;
    storage.frameRateNumerator = rateNumerator_;
    storage.frameRateDenominator = rateDenominator_;
    storage.settings = settings_;
    storage.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    const auto stored = session_.executeTransaction(commands::makeStoreTrackTransaction(storage, rows));
    if (stored.status != commands::CommandStatus::Succeeded) {
        finish(State::Failed, tr("The track could not be stored."));
        return;
    }
    if (track.end == runtime::PointTrackEnd::Finished)
        finish(State::Stored, tr("Tracked %n frame(s).", nullptr, static_cast<int>(rows.size())));
    else
        finish(State::Stopped, tr("Tracking stopped at frame %1: %2 The track up to there was kept.")
                                   .arg(track.lostFrame)
                                   .arg(QString::fromStdString(track.detail)));
}

} // namespace bloom::ui
