#include "subtitles_inspector.hpp"

#include <bloom/core/worker_thread.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/subtitles/subtitles.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>

#include <QTimer>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <variant>

namespace bloom::ui {

namespace {

using Results = std::map<document::NodeId, SubtitlesInspector::Node>;

// One node of the request, read on the interface thread.
struct NodeRequest final {
    document::NodeId node;
    // The playhead on the node's clock (the cue list's key time).
    core::RationalTime time;
    // The styling at the playhead, as the frame resolves it.
    runtime::SubtitlesStyleValues style;
    bool detail = false;
    std::optional<host::LinkedCueList> linked;
};

QRectF rectOf(const render::PathBounds& bounds) {
    return QRectF(QPointF(bounds.left, bounds.top), QPointF(bounds.right, bounds.bottom));
}

const document::CueListValue* constantCues(const document::Composition& composition,
                                           const document::ParameterId parameter) {
    const auto* record = composition.parameters().find(parameter);
    const auto* constant =
        record == nullptr ? nullptr : std::get_if<document::ConstantValueSource>(&record->source);
    return constant == nullptr ? nullptr
                               : std::get_if<document::CueListValue>(&constant->value);
}

QString plainFirstLines(const document::Cue& cue) {
    // The overlay labels a block with its text; markup tags are dropped for that reading.
    QString text;
    bool tag = false;
    for (const auto character : QString::fromStdString(cue.text)) {
        if (character == QLatin1Char('<'))
            tag = true;
        else if (character == QLatin1Char('>'))
            tag = false;
        else if (!tag)
            text += character;
    }
    return text.replace(QStringLiteral("&amp;"), QStringLiteral("&"))
        .replace(QStringLiteral("&lt;"), QStringLiteral("<"))
        .replace(QStringLiteral("&gt;"), QStringLiteral(">"))
        .replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
}

} // namespace

// The compiled plan of the newest document state, and the checks of each node at it: a later
// time at the same state reuses both, so moving the playhead only lays out the cues it shows.
struct SubtitlesInspector::Cache final {
    std::mutex mutex;
    std::optional<document::Revision> revision;
    document::CompositionId composition;
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    QString failure;
    std::map<document::NodeId, std::map<std::uint64_t, QStringList>> checks;
};

SubtitlesInspector::SubtitlesInspector(CompositionSession& session, QObject* parent)
    : QObject(parent), session_(session), cache_(std::make_shared<Cache>()),
      timer_(new QTimer(this)), debounce_(new QTimer(this)) {
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
    // Coalesce a burst (a snapshot change and a time change in one event loop turn).
    debounce_->setSingleShot(true);
    debounce_->setInterval(0);
    connect(debounce_, &QTimer::timeout, this, [this] { start(); });
    connect(&session_, &CompositionSession::snapshotChanged, this, &SubtitlesInspector::refresh);
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &SubtitlesInspector::refresh);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &SubtitlesInspector::refresh);
    connect(&session_, &CompositionSession::liveValueChanged, this, &SubtitlesInspector::refresh);
    connect(&session_, &CompositionSession::documentRebound, this, [this] {
        // A new document may reuse the old one's revision numbers: forget what was compiled.
        cache_ = std::make_shared<Cache>();
        results_.clear();
        refresh();
    });
}

SubtitlesInspector::~SubtitlesInspector() {
    if (!scheduler_)
        return;
    task_.cancel();
    scheduler_->beginShutdown();
    retire_->store(true);
    retire_->notify_one();
}

void SubtitlesInspector::setDetailNodes(const void* owner, std::set<document::NodeId> nodes) {
    auto& held = detail_[owner];
    if (held == nodes)
        return;
    held = std::move(nodes);
    refresh();
}

void SubtitlesInspector::releaseDetailNodes(const void* owner) {
    if (detail_.erase(owner) != 0)
        refresh();
}

const SubtitlesInspector::Node* SubtitlesInspector::result(const document::NodeId node) const {
    const auto found = results_.find(node);
    return found == results_.end() ? nullptr : &found->second;
}

void SubtitlesInspector::refresh() {
    ++generation_;
    pending_ = true;
    if (active_)
        task_.cancel();
    else if (!debounce_->isActive())
        debounce_->start();
}

void SubtitlesInspector::ensureScheduler() {
    if (scheduler_)
        return;
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    scheduler_ = std::make_shared<runtime::TaskScheduler>(config);
    retire_ = std::make_shared<std::atomic_bool>(false);
    // The same reaper the driven-value resolver uses: destroying the inspector only asks the
    // worker to stop; it never joins a thread on the interface thread.
    core::WorkerThread([scheduler = scheduler_, retire = retire_] {
        retire->wait(false);
        while (!scheduler->isQuiescent())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }).detach();
}

void SubtitlesInspector::read(QString name, std::function<CueRead()> work, QObject* receiver,
                              std::function<void(CueRead)> done) {
    ensureScheduler();
    auto submission = scheduler_->submit<std::shared_ptr<CueRead>>(
        runtime::TaskRequest{name.toStdString(),
                             {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Interactive},
        [work = std::move(work)](runtime::TaskContext&) {
            return runtime::TaskResult<std::shared_ptr<CueRead>>::succeeded(
                std::make_shared<CueRead>(work()));
        });
    if (!submission.accepted()) {
        CueRead refused;
        refused.failure = subtitles::LoadFailure{
            .line = 0,
            .column = 0,
            .code = std::string(subtitles::codes::kFile),
            .message = submission.diagnostic ? submission.diagnostic->summary
                                             : std::string("The read could not start"),
            .reports = {}};
        if (done)
            done(std::move(refused));
        return;
    }
    reads_.push_back({std::move(submission.handle), receiver, std::move(done)});
    timer_->start();
}

void SubtitlesInspector::start() {
    if (active_)
        return;
    pending_ = false;
    const auto* composition = session_.composition();
    std::vector<NodeRequest> requests;
    std::set<document::NodeId> detail;
    for (const auto& [owner, nodes] : detail_)
        detail.insert(nodes.begin(), nodes.end());
    if (composition != nullptr)
        for (const auto& node : composition->graph().nodes()) {
            if (node.typeId != document::kSubtitlesNodeType)
                continue;
            NodeRequest request;
            request.node = node.id;
            request.detail = detail.contains(node.id);
            const auto cues = nodeRoleParameter(session_, node.id, document::kSubtitlesCuesRole);
            request.time = cues ? session_.keyTimeAtPlayhead(*cues) : session_.currentTime();
            const auto scalar = [&](const std::string_view role, double& target) {
                if (const auto parameter = nodeRoleParameter(session_, node.id, role))
                    if (const auto value = session_.effectiveScalarValue(*parameter))
                        target = *value;
            };
            scalar("size", request.style.size);
            scalar("outlineWidth", request.style.outlineWidth);
            scalar("lineHeight", request.style.lineHeight);
            scalar("letterSpacing", request.style.letterSpacing);
            scalar("opacity", request.style.opacity);
            if (const auto parameter = nodeRoleParameter(session_, node.id, "offset"))
                if (const auto value = session_.effectiveVec2Value(*parameter))
                    request.style.offset = *value;
            // A linked file's state is read for every node (the node card shows it); a stamp
            // that did not move is not read again.
            if (cues)
                if (const auto* list = constantCues(*composition, *cues))
                    if (const auto* file =
                            std::get_if<document::CueFileProvenance>(&list->provenance))
                        request.linked = host::LinkedCueList{
                            session_.compositionId(), node.id, *file,
                            subtitles::linkedFilePath(*file, session_.projectDirectory())};
            requests.push_back(std::move(request));
        }
    if (requests.empty()) {
        if (!results_.empty()) {
            results_.clear();
            emit inspected();
        }
        return;
    }
    ensureScheduler();
    activeGeneration_ = generation_;
    const auto snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    const auto submission = scheduler_->submit<std::shared_ptr<Results>>(
        runtime::TaskRequest{"Inspect Subtitles",
                             {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Interactive},
        [snapshot, compositionId, requests = std::move(requests),
         cache = cache_](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<Results>>;
            const auto cancelled = [&context] { return context.isCancellationRequested(); };
            auto results = std::make_shared<Results>();
            std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
            QString failure;
            {
                std::lock_guard lock(cache->mutex);
                if (cache->revision == snapshot.revision() &&
                    cache->composition == compositionId) {
                    plan = cache->plan;
                    failure = cache->failure;
                }
            }
            if (!plan && failure.isEmpty()) {
                const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
                const auto compiled = compiler.compile({snapshot, compositionId}, {});
                if (cancelled())
                    return Result::cancelled();
                plan = compiled.plan;
                if (!plan)
                    failure = compiled.diagnostics.empty()
                                  ? QObject::tr("The composition does not compile")
                                  : QString::fromStdString(compiled.diagnostics.front().summary);
                std::lock_guard lock(cache->mutex);
                if (cache->revision != snapshot.revision() ||
                    cache->composition != compositionId) {
                    cache->checks.clear();
                    cache->revision = snapshot.revision();
                    cache->composition = compositionId;
                }
                cache->plan = plan;
                cache->failure = failure;
            }
            const auto* composition = snapshot.project().findComposition(compositionId);
            for (const auto& request : requests) {
                if (cancelled())
                    return Result::cancelled();
                auto& node = (*results)[request.node];
                if (request.linked) {
                    node.linked = true;
                    // Bounded by the subtitle file limit (16 MiB); the task's cancellation is checked around it.
                    node.fileState = host::linkedFileState(*request.linked);
                    if (node.fileState == host::LinkedFileState::Changed) {
                        // The desktop reloads a changed file; one that stays changed did not load.
                        subtitles::FileLoadRequest load{
                            .path = request.linked->path,
                            .projectDirectory = {},
                            .options = {},
                            .link = true};
                        load.options.format =
                            subtitles::formatFromName(request.linked->provenance.format);
                        load.options.encoding = request.linked->provenance.encoding;
                        const auto loaded = subtitles::loadFile(load);
                        if (const auto* refused = std::get_if<subtitles::LoadFailure>(&loaded)) {
                            node.fileMessage = QString::fromStdString(refused->message);
                            if (refused->line > 0)
                                node.fileMessage = QObject::tr("Line %1: %2")
                                                       .arg(refused->line)
                                                       .arg(node.fileMessage);
                        }
                    }
                }
                const runtime::CompiledSubtitles* compiled = nullptr;
                if (plan)
                    for (const auto& operation : plan->operations())
                        if (const auto* candidate =
                                std::get_if<runtime::CompiledSubtitles>(&operation);
                            candidate != nullptr && candidate->sourceNodeId == request.node)
                            compiled = candidate;
                if (compiled == nullptr) {
                    node.reason = plan ? QObject::tr("The node does not reach the output")
                                       : failure;
                    continue;
                }
                const auto& settings = compiled->settings;
                node.frame = QRectF(0.0, 0.0, settings.frameWidth, settings.frameHeight);
                node.safeArea = rectOf(runtime::subtitlesSafeArea(settings));
                node.burnIn = settings.burnIn;
                if (compiled->drivenCues) {
                    node.reason = QObject::tr("The cues come from another node");
                    continue;
                }
                const auto& list = *compiled->cues.list;
                auto placed = runtime::placeSubtitleCues(*compiled, list, request.time,
                                                         request.style, cancelled);
                if (cancelled())
                    return Result::cancelled();
                if (const auto* message = std::get_if<std::string>(&placed))
                    node.reason = QString::fromStdString(*message);
                else
                    for (const auto& block :
                         std::get<std::vector<runtime::SubtitlesPlacedBlock>>(placed))
                        node.blocks.push_back({block.id, block.index + 1, rectOf(block.block),
                                               plainFirstLines(*list.cues[block.index])});
                if (!request.detail || composition == nullptr)
                    continue;
                {
                    std::lock_guard lock(cache->mutex);
                    if (const auto found = cache->checks.find(request.node);
                        found != cache->checks.end() && cache->revision == snapshot.revision()) {
                        node.checks = found->second;
                        node.checked = true;
                        continue;
                    }
                }
                const runtime::SubtitlesCheckSettings checkSettings{
                    .maxCharactersPerLine = settings.maxCharactersPerLine,
                    .maxReadingSpeed = settings.maxReadingSpeed,
                    .minDuration = settings.minDuration,
                    .minGapFrames = settings.minGap,
                    .frameRate = composition->format().frameRate(),
                    .compositionDuration = composition->duration()};
                const auto checks = runtime::checkSubtitleCues(
                    *compiled, list, runtime::subtitlesConstantStyle(*compiled), checkSettings);
                if (cancelled())
                    return Result::cancelled();
                for (std::size_t index = 0; index < checks.size() && index < list.cues.size();
                     ++index) {
                    QStringList messages;
                    for (const auto& check : checks[index])
                        messages.push_back(QString::fromStdString(check.message));
                    if (!messages.isEmpty())
                        node.checks[list.cues[index]->id] = std::move(messages);
                }
                node.checked = true;
                std::lock_guard lock(cache->mutex);
                if (cache->revision == snapshot.revision())
                    cache->checks[request.node] = node.checks;
            }
            return Result::succeeded(std::move(results));
        });
    task_ = submission.handle;
    active_ = submission.accepted();
    if (active_)
        timer_->start();
}

void SubtitlesInspector::poll() {
    for (auto read = reads_.begin(); read != reads_.end();) {
        auto finished = read->handle.tryTakeResult();
        if (!finished) {
            ++read;
            continue;
        }
        auto pending = std::move(*read);
        read = reads_.erase(read);
        if (pending.receiver.isNull() || !pending.done)
            continue;
        CueRead outcome;
        if (finished->value() && *finished->value())
            outcome = std::move(**finished->value());
        else
            outcome.failure = subtitles::LoadFailure{.line = 0,
                                                     .column = 0,
                                                     .code = std::string(subtitles::codes::kCancelled),
                                                     .message = "The read was cancelled",
                                                     .reports = {}};
        pending.done(std::move(outcome));
        // `done` may have destroyed the inspector's owner chain; reads_ is ours, keep iterating
        // from the fresh begin to stay valid.
        read = reads_.begin();
    }
    if (!active_) {
        if (reads_.empty())
            timer_->stop();
        return;
    }
    auto result = task_.tryTakeResult();
    if (!result)
        return;
    active_ = false;
    if (reads_.empty())
        timer_->stop();
    if (activeGeneration_ == generation_ && result->value() && *result->value()) {
        results_ = std::move(**result->value());
        emit inspected();
    }
    if (pending_)
        start();
}

} // namespace bloom::ui
