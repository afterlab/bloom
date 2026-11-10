#include "driven_value_resolver.hpp"
#include "composition_driver_probe.hpp"
#include <QTimer>
#include <algorithm>
#include "properties_value_outputs.hpp"
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/image_statistics.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

namespace bloom::ui {
namespace {
runtime::CompiledValue promotedValue(const runtime::CompiledValue& value, std::string_view schema) {
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        for (const auto& parameter : definition.parameters) {
            if (parameter.schemaKey != schema)
                continue;
            if (parameter.valueKind == document::ParameterValueKind::Integer) {
                if (const auto* boolean = std::get_if<bool>(&value))
                    return static_cast<std::int64_t>(*boolean);
            }
            if (parameter.valueKind == document::ParameterValueKind::Float64) {
                if (const auto* integer = std::get_if<std::int64_t>(&value))
                    return static_cast<double>(*integer);
                if (const auto* boolean = std::get_if<bool>(&value))
                    return static_cast<double>(*boolean);
            }
            if (const auto* scalar = std::get_if<double>(&value)) {
                if (parameter.valueKind == document::ParameterValueKind::Vec2d)
                    return document::Vec2d{*scalar, *scalar};
                if (parameter.valueKind == document::ParameterValueKind::Vec3d)
                    return document::Vec3d{*scalar, *scalar, *scalar};
            }
            return value;
        }
    }
    return value;
}
// The slot of `port` among its operation's outputs: the port's place among the definition's
// outputs, except for Camera Shake, whose operation publishes the offset, the roll and the scale
// while its definition lists the image first.
std::optional<std::size_t> valueSlot(const document::NodeRecord& node,
                                     const document::NodeDefinition& definition,
                                     const std::string_view port) {
    if (node.typeId == document::kCameraShakeNodeType) {
        if (port == document::kCameraShakeOffsetPort)
            return 0;
        if (port == document::kCameraShakeRollPort)
            return 1;
        return std::nullopt;
    }
    const auto found =
        std::ranges::find(definition.outputs, port, &document::OutputPortDefinition::name);
    if (found == definition.outputs.end())
        return std::nullopt;
    return static_cast<std::size_t>(found - definition.outputs.begin());
}

// Every (node, port) a readout was asked for, by the same predicate the Properties rows use.
std::vector<DrivenValueResolver::ValueOutputKey>
readoutOutputKeys(const document::Snapshot& snapshot, const document::CompositionId compositionId,
                  const std::vector<document::NodeId>& nodes) {
    std::vector<DrivenValueResolver::ValueOutputKey> keys;
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr)
        return keys;
    // The selected nodes, and every Layer Bounds and Camera Shake node whatever is selected: node
    // cards show those two nodes' readouts, so each of their outputs gets a value or a reason.
    std::vector<document::NodeId> asked = nodes;
    for (const auto& node : composition->graph().nodes())
        if ((node.typeId == document::kLayerBoundsNodeType ||
             node.typeId == document::kCameraShakeNodeType) &&
            std::ranges::find(asked, node.id) == asked.end())
            asked.push_back(node.id);
    for (const auto id : asked) {
        const auto* node = composition->graph().findNode(id);
        const auto* definition =
            node ? document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)
                 : nullptr;
        if (definition == nullptr)
            continue;
        for (auto& port : readableValueOutputs(*definition))
            keys.emplace_back(id, std::move(port));
    }
    return keys;
}

QString valueText(const runtime::CompiledValue& value) {
    return std::visit(
        [](const auto& held) -> QString {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, core::Color4d>)
                return exactColorText(held);
            else if constexpr (std::is_same_v<T, document::Vec2d>)
                return QString("%1, %2").arg(held.x, 0, 'g', 12).arg(held.y, 0, 'g', 12);
            else if constexpr (std::is_same_v<T, document::Vec3d>)
                return QString("%1, %2, %3")
                    .arg(held.x, 0, 'g', 12)
                    .arg(held.y, 0, 'g', 12)
                    .arg(held.z, 0, 'g', 12);
            else if constexpr (std::is_same_v<T, std::string>)
                // An empty text is an answer, not a missing one (an empty row reads as unresolved).
                return held.empty() ? QObject::tr("(empty)") : QString::fromStdString(held);
            else if constexpr (std::is_same_v<T, bool>)
                return held ? QObject::tr("True") : QObject::tr("False");
            else if constexpr (std::is_same_v<T, double>)
                return QString::number(held, 'g', 12);
            else if constexpr (std::is_same_v<T, runtime::CompiledPathValue>)
                return QObject::tr("%1 contours, %2 anchors")
                    .arg(held.value->contours.size())
                    .arg(held.value->anchorCount());
            else
                return QString::number(static_cast<qlonglong>(held));
        },
        value);
}
// What Properties and the card show for an output: Camera Shake's offset and roll as its own fields
// show them (two decimals, with their units); everything else keeps the exact text.
QString readoutText(const document::NodeRecord& node, const std::string_view port,
                    const runtime::CompiledValue& value) {
    if (node.typeId == document::kCameraShakeNodeType) {
        if (const auto* offset = std::get_if<document::Vec2d>(&value))
            return QObject::tr("X %1 px, Y %2 px")
                .arg(offset->x, 0, 'f', 2)
                .arg(offset->y, 0, 'f', 2);
        if (const auto* roll = std::get_if<double>(&value); roll != nullptr && port == "roll")
            return QObject::tr("%1\u00b0").arg(*roll, 0, 'f', 2);
    }
    return valueText(value);
}
} // namespace
struct DrivenValueResolver::ProbeCache final {
    std::mutex mutex;
    // The entry keeps its snapshot alive, so its project address can never be reused by another
    // document state while the key is compared.
    std::optional<document::Snapshot> snapshot;
    document::CompositionId composition;
    std::vector<document::ParameterId> parameters;
    std::vector<document::NodeId> readouts;
    // What one inspection answered: the compile, and why the readouts were left out of it when
    // the compile that included them failed (empty when they were included or none were asked).
    struct Entry final {
        runtime::SnapshotCompileResult compiled;
        QString readoutReason;
    };
    Entry entry;

    [[nodiscard]] std::optional<Entry> find(const document::Snapshot& state,
                                            const document::CompositionId id,
                                            const std::vector<document::ParameterId>& asked,
                                            const std::vector<document::NodeId>& readoutNodes) {
        const std::lock_guard lock(mutex);
        if (snapshot && snapshot->revision() == state.revision() &&
            &snapshot->project() == &state.project() && composition == id && parameters == asked &&
            readouts == readoutNodes)
            return entry;
        return std::nullopt;
    }
    void store(const document::Snapshot& state, const document::CompositionId id,
               std::vector<document::ParameterId> asked, std::vector<document::NodeId> readoutNodes,
               Entry result) {
        const std::lock_guard lock(mutex);
        snapshot = state;
        composition = id;
        parameters = std::move(asked);
        readouts = std::move(readoutNodes);
        entry = std::move(result);
    }
};
DrivenValueResolver::DrivenValueResolver(CompositionSession& session, QObject* parent)
    : QObject(parent), session_(session), probeCache_(std::make_shared<ProbeCache>()),
      timer_(new QTimer(this)) {
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
}
DrivenValueResolver::~DrivenValueResolver() {
    if (!scheduler_)
        return;
    task_.cancel();
    scheduler_->beginShutdown();
    retire_->store(true);
    retire_->notify_one();
}
void DrivenValueResolver::request(std::vector<document::ParameterId> parameters,
                                  std::vector<document::NodeId> readoutNodes) {
    parameters_ = std::move(parameters);
    readoutNodes_ = std::move(readoutNodes);
    ++generation_;
    if (active_)
        task_.cancel();
    else
        start();
}
void DrivenValueResolver::start() {
    if (!scheduler_) {
        auto config = runtime::TaskSchedulerConfig::defaults();
        config.cpuWorkerCount = 1;
        config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
        scheduler_ = std::make_shared<runtime::TaskScheduler>(config);
        retire_ = std::make_shared<std::atomic_bool>(false);
        // Start the Qt-free reaper while construction can report a thread-start failure. Closing
        // the panel only requests cancellation; it never creates or joins a thread.
        std::thread([scheduler = scheduler_, retire = retire_] {
            retire->wait(false);
            while (!scheduler->isQuiescent())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }).detach();
    }
    const auto snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    const auto time = session_.currentTime();
    const auto parameters = parameters_;
    const auto readoutNodes = readoutNodes_;
    activeGeneration_ = generation_;
    const auto submission = scheduler_->submit<std::shared_ptr<Values>>(
        runtime::TaskRequest{
            "Resolve Properties Drivers",
            {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Interactive},
        [snapshot, compositionId, time, parameters, readoutNodes, cache = probeCache_,
         work = work_](runtime::TaskContext& context) {
            auto values = std::make_shared<Values>();
            for (auto parameter : parameters)
                values->parameters[parameter] = QObject::tr("Unavailable");
            const auto readoutKeys = readoutOutputKeys(snapshot, compositionId, readoutNodes);
            for (const auto& key : readoutKeys)
                values->outputs[key] = QObject::tr("Unavailable");
            auto cached = cache->find(snapshot, compositionId, parameters, readoutNodes);
            if (!cached) {
                ++work->probeCompiles;
                ProbeCache::Entry fresh;
                fresh.compiled = compileDriverProbe(snapshot, compositionId, parameters,
                                                    readoutNodes, context.cancellation());
                if (context.isCancellationRequested())
                    return runtime::TaskResult<std::shared_ptr<Values>>::cancelled();
                // One node that cannot compile must not take the driven values down with it:
                // inspect again without the readouts and keep the reason for them.
                if (!fresh.compiled.plan && !readoutNodes.empty()) {
                    fresh.readoutReason =
                        fresh.compiled.diagnostics.empty()
                            ? QObject::tr("Unavailable")
                            : QString::fromStdString(fresh.compiled.diagnostics.front().summary);
                    fresh.compiled = compileDriverProbe(snapshot, compositionId, parameters, {},
                                                        context.cancellation());
                    if (context.isCancellationRequested())
                        return runtime::TaskResult<std::shared_ptr<Values>>::cancelled();
                }
                cache->store(snapshot, compositionId, parameters, readoutNodes, fresh);
                cached = std::move(fresh);
            }
            const auto& compiled = cached->compiled;
            if (!cached->readoutReason.isEmpty())
                for (auto& [key, text] : values->outputs)
                    text = cached->readoutReason;
            ++work->evaluations;
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Values>>::cancelled();
            const auto* composition = snapshot.project().findComposition(compositionId);
            if (compiled.plan && composition) {
                const auto& plan = *compiled.plan;
                std::vector<runtime::CompiledValue> evaluatedOutputs;
                std::vector<runtime::ValueGraphDiagnostic> valueDiagnostics;
                // Why the pictures' statistics are not a measurement, when the evaluation says so.
                std::string statisticsReason;
                // A value that needs the rendered picture (a layer's bounds) or the statistics of one
                // (Image Statistics, measured through the evaluator's preparation on this worker)
                // is read from an evaluation; every other value comes from the value graph alone.
                const bool hasPostImageValues =
                    runtime::planReadsImageStatistics(plan) ||
                    std::ranges::any_of(plan.valueOperations(), [](const auto& operation) {
                        return operation.requiresPostImage;
                    });
                if (hasPostImageValues) {
                    runtime::CpuCompositionEvaluator evaluator;
                    const auto evaluation = evaluator.evaluate(
                        compiled.plan,
                        {.time = time,
                         .output = plan.output(),
                         .resolution = runtime::CompositionFormatResolution{},
                         .quality = runtime::EvaluationQuality::Reference,
                         .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                         .pixelStorageByteLimit = std::size_t{512} * 1024U * 1024U},
                        context.cancellation());
                    if (evaluation.frame() != nullptr) {
                        const auto outputSpan = evaluation.frame()->valueOutputs();
                        evaluatedOutputs.assign(outputSpan.begin(), outputSpan.end());
                    }
                    for (const auto& diagnostic : evaluation.diagnostics())
                        if (diagnostic.summary.starts_with("Image Statistics") &&
                            statisticsReason.empty())
                            statisticsReason = diagnostic.summary;
                } else {
                    runtime::ValueGraphMemoization memoization;
                    memoization.cancellation = &context.cancellation();
                    const auto evaluated =
                        runtime::evaluateValueGraph(plan.valueOperations(), plan.valueOutputCount(),
                                                    time, composition->format().frameRate(),
                                                    {plan.scalarCurves(), plan.vec2Curves(),
                                                     plan.color4Curves(), plan.vec3Curves()},
                                                    memoization);
                    evaluatedOutputs = evaluated.outputs;
                    valueDiagnostics = evaluated.diagnostics;
                }
                for (const auto& diagnostic : valueDiagnostics) {
                    if (!diagnostic.nodeId.isValid())
                        continue;
                    auto& text = values->nodeDiagnostics[diagnostic.nodeId];
                    const auto line = QString::fromStdString(diagnostic.summary) +
                                      (diagnostic.detail.empty()
                                           ? QString{}
                                           : QStringLiteral(": ") +
                                                 QString::fromStdString(diagnostic.detail));
                    if (!text.contains(line))
                        text += (text.isEmpty() ? QString{} : QStringLiteral("\n")) + line;
                }
                for (const auto& operation : plan.valueOperations()) {
                    if (!operation.sourceNodeId.isValid())
                        continue;
                    const auto* node = composition->graph().findNode(operation.sourceNodeId);
                    const auto* definition = node ? document::builtInNodeDefinitions().find(
                                                        node->typeId, node->schemaVersion)
                                                  : nullptr;
                    if (node == nullptr || definition == nullptr)
                        continue;
                    for (const auto& output : definition->outputs) {
                        const auto slot = valueSlot(*node, *definition, output.name);
                        if (!slot)
                            continue;
                        const auto index = operation.firstOutput.value() + *slot;
                        if (index >= evaluatedOutputs.size())
                            continue;
                        auto text = readoutText(*node, output.name, evaluatedOutputs[index]);
                        // #598: why the node answered what it did (a Warp Path whose size is 0
                        // answers the empty path), on the node's own readouts, in the words
                        // MCP and scripts get.
                        for (const auto& diagnostic : valueDiagnostics)
                            if (diagnostic.nodeId == node->id)
                                text += QString(" · %1").arg(QString::fromStdString(diagnostic.summary));
                        values->outputs[{node->id, output.name}] = std::move(text);
                    }
                }
                // An Image Statistics that produced nothing (cancelled, or its image never
                // rendered) says so rather than leaving its rows on the placeholder.
                for (auto& [key, text] : values->outputs) {
                    const auto* node = composition->graph().findNode(key.first);
                    const auto* definition = node ? document::builtInNodeDefinitions().find(
                                                        node->typeId, node->schemaVersion)
                                                  : nullptr;
                    if (definition == nullptr ||
                        definition->lowering != document::NodeLoweringKind::ValueImageStatistics)
                        continue;
                    if (!statisticsReason.empty())
                        text = QString::fromStdString(statisticsReason);
                    else if (text == QObject::tr("Unavailable"))
                        text = QObject::tr("Image Statistics are not ready");
                }
                for (auto id : parameters) {
                    const auto* parameter = composition->parameters().find(id);
                    const auto* driver =
                        parameter ? std::get_if<document::DriverBindingSource>(&parameter->source)
                                  : nullptr;
                    if (!driver)
                        continue;
                    const auto* node = composition->graph().findNode(driver->sourceNodeId);
                    const auto* definition = node ? document::builtInNodeDefinitions().find(
                                                        node->typeId, node->schemaVersion)
                                                  : nullptr;
                    if (!definition)
                        continue;
                    const auto slot = valueSlot(*node, *definition, driver->outputPort);
                    if (!slot)
                        continue;
                    for (const auto& operation : plan.valueOperations()) {
                        if (operation.sourceNodeId != node->id)
                            continue;
                        const auto index = operation.firstOutput.value() + *slot;
                        if (index < evaluatedOutputs.size()) {
                            auto value =
                                promotedValue(evaluatedOutputs[index], parameter->schemaKey);
                            values->parameters[id] = valueText(value);
                            values->values[id] = std::move(value);
                        }
                    }
                    for (const auto& diagnostic : valueDiagnostics)
                        if (diagnostic.nodeId == driver->sourceNodeId)
                            values->parameters[id] +=
                                QString(" · %1").arg(QString::fromStdString(diagnostic.summary));
                }
            } else if (!compiled.diagnostics.empty()) {
                for (auto& [id, text] : values->parameters) {
                    (void)id;
                    text = QString::fromStdString(compiled.diagnostics.front().summary);
                }
            }
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Values>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<Values>>::succeeded(std::move(values));
        });
    task_ = submission.handle;
    active_ = submission.accepted();
    if (active_) {
        timer_->start();
        return;
    }
    // A refused submission would otherwise leave the rows waiting forever: no task exists to poll.
    if (ready) {
        const auto message = submission.diagnostic.has_value()
                                 ? QString::fromStdString(submission.diagnostic->summary)
                                 : tr("Unavailable");
        Values unavailable;
        for (auto id : parameters_)
            unavailable.parameters[id] = message;
        for (const auto& key :
             readoutOutputKeys(session_.snapshot(), session_.compositionId(), readoutNodes_))
            unavailable.outputs[key] = message;
        ready(unavailable);
    }
}
void DrivenValueResolver::poll() {
    if (auto result = task_.tryTakeResult()) {
        active_ = false;
        if (activeGeneration_ == generation_) {
            if (ready && result->value())
                ready(**result->value());
            else if (ready) {
                Values unavailable;
                const auto message =
                    result->diagnostics().empty()
                        ? tr("Unavailable")
                        : QString::fromStdString(result->diagnostics().front().summary);
                for (auto id : parameters_)
                    unavailable.parameters[id] = message;
                for (const auto& key : readoutOutputKeys(session_.snapshot(), session_.compositionId(),
                                                         readoutNodes_))
                    unavailable.outputs[key] = message;
                ready(unavailable);
            }
            timer_->stop();
        } else
            start();
    }
}
} // namespace bloom::ui
