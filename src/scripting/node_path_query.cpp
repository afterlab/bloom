#include <bloom/scripting/node_path_query.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::scripting {
namespace {

// A private copy of the project to add the reading nodes to.
class Probe final {
  public:
    Probe(const document::Snapshot& snapshot, const document::CompositionId composition)
        : document_(snapshot.project()), stack_(document_), composition_(composition) {}
    template <class Operation, class... Args> bool edit(Args&&... args) {
        commands::Transaction transaction("Read node", document_.snapshot().revision());
        transaction.emplace<Operation>(composition_, std::forward<Args>(args)...);
        return stack_.execute(std::move(transaction)).succeeded();
    }
    document::NodeId add(const std::string_view type) {
        commands::Transaction transaction("Read node", document_.snapshot().revision());
        transaction.emplace<commands::AddNode>(composition_, std::string(type), document::Vec2d{});
        return stack_.execute(std::move(transaction))
            .outputId<document::NodeId>(commands::kAddNodeOutput)
            .value_or(document::NodeId{});
    }
    [[nodiscard]] document::Snapshot snapshot() const { return document_.snapshot(); }

  private:
    document::Document document_;
    commands::CommandStack stack_;
    document::CompositionId composition_;
};

// Every value output of `plan` at `time`: the value pass alone, or the whole frame when a value
// reads the picture (a Layer Bounds readout).
void evaluateValueOutputs(const std::shared_ptr<const runtime::CompiledCompositionPlan>& shared,
                          const document::Composition& composition, const core::RationalTime time,
                          const runtime::CancellationToken& cancellation,
                          const std::filesystem::path& assetBaseDirectory,
                          std::vector<runtime::CompiledValue>& outputs,
                          std::vector<runtime::ValueGraphDiagnostic>& diagnostics) {
    const auto& plan = *shared;
    const bool needsImage = std::ranges::any_of(
        plan.valueOperations(), [](const auto& operation) { return operation.requiresPostImage; });
    if (needsImage) {
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(assetBaseDirectory);
        const auto evaluation = evaluator.evaluate(
            shared,
            {.time = time,
             .output = plan.output(),
             .resolution = runtime::CompositionFormatResolution{},
             .quality = runtime::EvaluationQuality::Reference,
             .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
             .pixelStorageByteLimit = std::size_t{512} * 1024U * 1024U},
            cancellation);
        if (evaluation.frame() != nullptr) {
            const auto span = evaluation.frame()->valueOutputs();
            outputs.assign(span.begin(), span.end());
        }
    } else {
        // #630: an audio analysis node prepares what it reads on this thread, as a frame does.
        const runtime::AudioAnalysisPreparation audioPreparation(shared, assetBaseDirectory,
                                                                 cancellation);
        runtime::ValueGraphMemoization memoization;
        memoization.cancellation = &cancellation;
        auto evaluated = runtime::evaluateValueGraph(
            plan.valueOperations(), plan.valueOutputCount(), time, composition.format().frameRate(),
            {plan.scalarCurves(), plan.vec2Curves(), plan.color4Curves(), plan.vec3Curves()},
            memoization);
        outputs = std::move(evaluated.outputs);
        diagnostics = std::move(evaluated.diagnostics);
    }
}

} // namespace

NodePathQuery queryNodePath(const document::Snapshot& snapshot,
                            const document::CompositionId compositionId, const document::NodeId nodeId,
                            const std::string_view requestedPort, const core::RationalTime time,
                            const runtime::CancellationToken& cancellation,
                            const std::filesystem::path& assetBaseDirectory) {
    NodePathQuery result;
    const auto fail = [&result](std::string message) {
        result.diagnostic = std::move(message);
        return result;
    };
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr)
        return fail("The composition does not exist");
    const auto* node = composition->graph().findNode(nodeId);
    const auto* definition =
        node != nullptr ? document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)
                        : nullptr;
    if (node == nullptr || definition == nullptr)
        return fail("Node " + std::to_string(nodeId.value()) + " does not exist");
    std::string port(requestedPort);
    std::size_t slot = 0;
    bool found = false;
    for (std::size_t index = 0; index < definition->outputs.size(); ++index) {
        const auto& output = definition->outputs[index];
        if (output.valueKind != document::SocketValueKind::Path)
            continue;
        if (port.empty() || port == output.name) {
            port = output.name;
            slot = index;
            found = true;
            break;
        }
    }
    if (!found)
        return fail(port.empty() ? "Node " + std::to_string(nodeId.value()) + " has no Path output"
                                 : "Node " + std::to_string(nodeId.value()) + " has no Path output named \"" + port + "\"");

    Probe probe(snapshot, compositionId);
    const auto merge = probe.add(document::kLayerStackNodeType);
    const auto sink = probe.add(document::kShapeSourceNodeType);
    if (!merge.isValid() || !sink.isValid() ||
        !probe.edit<commands::ConnectPorts>(document::OutputPortRef{nodeId, port},
                                            document::NodeInputRef{sink, "path"}) ||
        !probe.edit<commands::SetNodeMuted>(sink, true) ||
        !probe.edit<commands::ConnectPorts>(
            document::OutputPortRef{sink, "image"},
            document::LayerStackInputRef{merge, {},
                                         std::string(document::kLayerStackContentInputRole)}))
        return fail("The node could not be read");
    if (!composition->graph().compositionOutput())
        return fail("The composition has no output");
    const auto outputId = composition->graph().compositionOutput()->nodeId;
    (void)probe.edit<commands::DisconnectInput>(document::NodeInputRef{outputId, "image"});
    if (!probe.edit<commands::ConnectPorts>(document::OutputPortRef{merge, "image"},
                                            document::NodeInputRef{outputId, "image"}))
        return fail("The node could not be read");
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({probe.snapshot(), compositionId}, cancellation);
    if (cancellation.isCancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    if (!compiled.plan)
        return fail(compiled.diagnostics.empty() ? std::string("The node could not be compiled")
                                                 : compiled.diagnostics.front().summary);
    const auto& plan = *compiled.plan;
    std::vector<runtime::CompiledValue> outputs;
    std::vector<runtime::ValueGraphDiagnostic> diagnostics;
    evaluateValueOutputs(compiled.plan, *composition, time, cancellation, assetBaseDirectory, outputs,
                         diagnostics);
    if (cancellation.isCancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.nodeId == nodeId)
            return fail(diagnostic.summary + ": " + diagnostic.detail);
    for (const auto& operation : plan.valueOperations()) {
        if (operation.sourceNodeId != nodeId)
            continue;
        const auto index = operation.firstOutput.value() + slot;
        if (index >= outputs.size())
            break;
        const auto* path = std::get_if<runtime::CompiledPathValue>(&outputs[index]);
        if (path == nullptr || path->value == nullptr)
            break;
        result.path = *path->value;
        return result;
    }
    return fail("The node produced no path at this time");
}

NodeNumberListQuery queryNodeNumberList(const document::Snapshot& snapshot,
                                        const document::CompositionId compositionId,
                                        const document::NodeId nodeId,
                                        const std::string_view requestedPort,
                                        const core::RationalTime time,
                                        const runtime::CancellationToken& cancellation,
                                        const std::filesystem::path& assetBaseDirectory) {
    NodeNumberListQuery result;
    const auto fail = [&result](std::string message) {
        result.diagnostic = std::move(message);
        return result;
    };
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr)
        return fail("The composition does not exist");
    const auto* node = composition->graph().findNode(nodeId);
    const auto* definition =
        node != nullptr ? document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)
                        : nullptr;
    const auto name = "Node " + std::to_string(nodeId.value());
    if (node == nullptr || definition == nullptr)
        return fail(name + " does not exist");
    std::string port(requestedPort);
    std::size_t slot = 0;
    bool found = false;
    for (std::size_t index = 0; index < definition->outputs.size() && !found; ++index) {
        const auto& output = definition->outputs[index];
        if (output.valueKind != document::SocketValueKind::NumberList ||
            (!port.empty() && port != output.name))
            continue;
        port = output.name;
        slot = index;
        found = true;
    }
    if (!found)
        return fail(port.empty() ? name + " has no Number List output"
                                 : name + " has no Number List output named \"" + port + "\"");
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::SnapshotCompileRequest request{snapshot, compositionId};
    request.valueReadouts.push_back(nodeId);
    const auto compiled = compiler.compile(request, cancellation);
    if (cancellation.isCancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    if (!compiled.plan)
        return fail(compiled.diagnostics.empty() ? std::string("The node could not be compiled")
                                                 : compiled.diagnostics.front().summary);
    std::vector<runtime::CompiledValue> outputs;
    std::vector<runtime::ValueGraphDiagnostic> diagnostics;
    evaluateValueOutputs(compiled.plan, *composition, time, cancellation, assetBaseDirectory, outputs,
                         diagnostics);
    if (cancellation.isCancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.nodeId == nodeId)
            return fail(diagnostic.summary + ": " + diagnostic.detail);
    for (const auto& operation : compiled.plan->valueOperations()) {
        if (operation.sourceNodeId != nodeId)
            continue;
        const auto index = operation.firstOutput.value() + slot;
        if (index >= outputs.size())
            break;
        const auto* list = std::get_if<runtime::CompiledNumberList>(&outputs[index]);
        if (list == nullptr || list->values == nullptr)
            break;
        result.list = document::NumberListValue{*list->values};
        return result;
    }
    return fail("The node produced no number list at this time");
}

namespace {

// One bake's progress, published for the readout while it runs. Bakes that overlap (two clients)
// show the one that started last; the readout is advisory.
std::atomic<std::uint64_t> gBakeFramesDone{0};
std::atomic<std::uint64_t> gBakeFramesTotal{0};
std::atomic<std::uint64_t> gBakeOwner{0};
std::atomic<std::uint64_t> gBakeNextOwner{1};

class BakeProgress final {
  public:
    explicit BakeProgress(const std::uint64_t total) : owner_(gBakeNextOwner.fetch_add(1)) {
        gBakeOwner.store(owner_);
        gBakeFramesDone.store(0);
        gBakeFramesTotal.store(total);
    }
    BakeProgress(const BakeProgress&) = delete;
    BakeProgress& operator=(const BakeProgress&) = delete;
    ~BakeProgress() {
        if (gBakeOwner.load() == owner_) {
            gBakeFramesTotal.store(0);
            gBakeFramesDone.store(0);
        }
    }
    void done(const std::uint64_t frames) const {
        if (gBakeOwner.load() == owner_)
            gBakeFramesDone.store(frames);
    }

  private:
    std::uint64_t owner_;
};

} // namespace

NodeBakeActivity nodeBakeActivity() noexcept {
    return {.framesDone = gBakeFramesDone.load(), .framesTotal = gBakeFramesTotal.load()};
}

NodeScalarKeysQuery queryNodeScalarKeys(const document::Snapshot& snapshot,
                                        const document::CompositionId compositionId,
                                        const document::NodeId nodeId,
                                        const std::string_view requestedPort,
                                        const runtime::CancellationToken& cancellation,
                                        const std::filesystem::path& assetBaseDirectory) {
    NodeScalarKeysQuery result;
    const auto fail = [&result](std::string message) {
        result.keys.clear();
        result.diagnostic = std::move(message);
        return result;
    };
    const auto cancelled = [&result] {
        result.keys.clear();
        result.cancelled = true;
        return result;
    };
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr)
        return fail("The composition does not exist");
    const auto* node = composition->graph().findNode(nodeId);
    const auto* definition =
        node != nullptr ? document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)
                        : nullptr;
    const auto name = "Node " + std::to_string(nodeId.value());
    if (node == nullptr || definition == nullptr)
        return fail(name + " does not exist");
    std::string port(requestedPort);
    std::size_t slot = 0;
    bool found = false;
    for (std::size_t index = 0; index < definition->outputs.size() && !found; ++index) {
        const auto& output = definition->outputs[index];
        if (output.valueKind != document::SocketValueKind::Scalar ||
            (!port.empty() && port != output.name))
            continue;
        port = output.name;
        slot = index;
        found = true;
    }
    if (!found)
        return fail(port.empty() ? name + " has no number output"
                                 : name + " has no number output named \"" + port + "\"");
    const auto rate = composition->format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping)
        return fail("The composition has no frames to bake");
    const auto last = mapping.value()->maximumFrameIndex();
    if (last >= commands::kMaximumBakedKeys)
        return fail("The composition is longer than the " +
                    std::to_string(commands::kMaximumBakedKeys) + " frames a bake writes");
    const auto frames = last + 1;
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::SnapshotCompileRequest request{snapshot, compositionId};
    request.valueReadouts.push_back(nodeId);
    const auto compiled = compiler.compile(request, cancellation);
    if (cancellation.isCancellationRequested())
        return cancelled();
    if (!compiled.plan)
        return fail(compiled.diagnostics.empty() ? std::string("The node could not be compiled")
                                                 : compiled.diagnostics.front().summary);
    std::optional<std::size_t> index;
    for (const auto& operation : compiled.plan->valueOperations())
        if (operation.sourceNodeId == nodeId) {
            index = operation.firstOutput.value() + slot;
            break;
        }
    if (!index)
        return fail(name + " produced no number");
    const BakeProgress progress(frames);
    result.keys.reserve(static_cast<std::size_t>(frames));
    std::vector<runtime::CompiledValue> outputs;
    std::vector<runtime::ValueGraphDiagnostic> diagnostics;
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
        const auto time = mapping.value()->timeForFrame(frame);
        if (!time)
            return fail("Frame " + std::to_string(frame) + " has no time");
        outputs.clear();
        diagnostics.clear();
        evaluateValueOutputs(compiled.plan, *composition, *time.value(), cancellation,
                             assetBaseDirectory, outputs, diagnostics);
        if (cancellation.isCancellationRequested())
            return cancelled();
        for (const auto& diagnostic : diagnostics)
            if (diagnostic.nodeId == nodeId)
                return fail(diagnostic.summary + ": " + diagnostic.detail);
        const auto* value = *index < outputs.size() ? std::get_if<double>(&outputs[*index]) : nullptr;
        if (value == nullptr || !std::isfinite(*value))
            return fail(name + " produced no number at frame " + std::to_string(frame));
        result.keys.push_back({*time.value(), *value});
        progress.done(frame + 1);
    }
    return result;
}

} // namespace bloom::scripting
