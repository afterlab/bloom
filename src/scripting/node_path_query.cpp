#include <bloom/scripting/node_path_query.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include <algorithm>
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

} // namespace

NodePathQuery queryNodePath(const document::Snapshot& snapshot,
                            const document::CompositionId compositionId, const document::NodeId nodeId,
                            const std::string_view requestedPort, const core::RationalTime time,
                            const runtime::CancellationToken& cancellation) {
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
    const bool needsImage = std::ranges::any_of(
        plan.valueOperations(), [](const auto& operation) { return operation.requiresPostImage; });
    if (needsImage) {
        const runtime::CpuCompositionEvaluator evaluator;
        const auto evaluation = evaluator.evaluate(
            compiled.plan,
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
        runtime::ValueGraphMemoization memoization;
        memoization.cancellation = &cancellation;
        auto evaluated = runtime::evaluateValueGraph(
            plan.valueOperations(), plan.valueOutputCount(), time, composition->format().frameRate(),
            {plan.scalarCurves(), plan.vec2Curves(), plan.color4Curves(), plan.vec3Curves()},
            memoization);
        outputs = std::move(evaluated.outputs);
        diagnostics = std::move(evaluated.diagnostics);
    }
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

} // namespace bloom::scripting
