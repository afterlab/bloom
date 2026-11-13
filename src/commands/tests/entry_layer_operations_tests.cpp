// #502 Every Merge entry is a live layer: entry-layer creation on first edit, Layer node insert and
// dissolve (adopt / return), shared Layer nodes, Adjustment layers, the layer commands on entry
// layers, the reconcile pass's cleanup and its work counters, lock refusal and exact undo/redo.

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_stack_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/evaluation_inputs.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bloom;
using namespace bloom::commands;
using namespace bloom::document;

int failures = 0;
void expect(const bool condition, const std::string_view message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

struct Fixture {
    NewProject initial = makeNewProject(
        "Entries", "Main", core::RationalTime::fromInteger(4),
        *CompositionFormat::create(64, 32, core::PixelAspectRatio::square(),
                                   FrameRate::framesPerSecond24()));
    Document document{std::move(initial.project)};
    CommandStack stack{document};
    CompositionId composition = initial.initialCompositionId;

    [[nodiscard]] const Composition& comp() const {
        return *document.snapshot().project().findComposition(composition);
    }
    [[nodiscard]] NodeId mainMerge() const { return comp().graph().layerStack().nodeId(); }

    template <typename Operation, typename... Arguments>
    CommandResult run(Arguments&&... arguments) {
        Transaction transaction("Entry edit", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        return stack.execute(std::move(transaction));
    }
    template <typename Operation, typename... Arguments>
    CommandResult must(Arguments&&... arguments) {
        auto result = run<Operation>(std::forward<Arguments>(arguments)...);
        if (!result.succeeded()) {
            for (const auto& failure : result.operationFailures)
                std::cerr << "  refusal: " << failure.issue.message << '\n';
            for (const auto& issue : result.validation.issues())
                std::cerr << "  invalid: " << issue.path << ": " << issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
        return result;
    }
    CommandResult edit(const NodeId merge, const LayerSlotId slot, EntryLayerOperationBuilder build) {
        Transaction transaction("Entry control", document.snapshot().revision());
        transaction.emplace<EntryLayerEdit>(composition, merge, slot, std::move(build));
        return stack.execute(std::move(transaction));
    }

    NodeId addSolid() {
        return *must<AddNode>(composition, std::string(kSolidSourceNodeType), Vec2d{0, 0})
                    .outputId<NodeId>(kAddNodeOutput);
    }
    NodeId addNode(const std::string_view type) {
        return *must<AddNode>(composition, std::string(type), Vec2d{0, 0})
                    .outputId<NodeId>(kAddNodeOutput);
    }
    LayerSlotId connect(const NodeId source, const NodeId merge,
                        const std::string_view port = "image") {
        return *must<ConnectPorts>(composition, OutputPortRef{source, std::string(port)},
                                   InputPortRef{LayerStackInputRef{merge, {}, "content"}})
                    .outputId<LayerSlotId>(kConnectPortsSlotOutput);
    }
    void link(const NodeId source, const NodeId target, const std::string_view port = "image") {
        must<ConnectPorts>(composition, OutputPortRef{source, "image"},
                           InputPortRef{NodeInputRef{target, std::string(port)}});
    }
};

const LayerOutputBoundary* entryLayer(const Composition& composition, const NodeId merge,
                                      const LayerSlotId slot) {
    return mergeEntryLayer(composition.graph(), merge, slot);
}

const ParameterValue* constant(const Composition& composition, const LayerOutputBoundary& layer,
                               const std::string_view role) {
    const auto* binding = layerParameterBinding(composition.graph(), layer, role);
    const auto* record = binding ? composition.parameters().find(binding->parameterId) : nullptr;
    const auto* value = record ? std::get_if<ConstantValueSource>(&record->source) : nullptr;
    return value ? &value->value : nullptr;
}

std::size_t parameterCount(const Composition& composition) {
    return composition.parameters().records().size();
}

// Every parameter in the store is bound by a node, an entry layer or a mask.
bool noOrphanParameters(const Composition& composition) {
    std::set<ParameterId> bound;
    for (const auto& node : composition.graph().nodes())
        for (const auto& binding : node.parameters)
            bound.insert(binding.parameterId);
    for (const auto& layer : composition.graph().layerOutputs()) {
        for (const auto& binding : layer.parameters)
            bound.insert(binding.parameterId);
        for (const auto& mask : layer.masks)
            for (const auto id : {mask.path, mask.feather, mask.expansion, mask.opacity})
                bound.insert(id);
    }
    return std::ranges::all_of(composition.parameters().records(),
                               [&](const auto& record) { return bound.contains(record.id); });
}

bool sameContent(const Composition& a, const Composition& b) {
    if (!std::ranges::equal(a.parameters().records(), b.parameters().records()) ||
        !std::ranges::equal(a.animationCurves().records(), b.animationCurves().records()) ||
        !std::ranges::equal(a.graph().nodes(), b.graph().nodes()) ||
        !std::ranges::equal(a.graph().edges(), b.graph().edges()) ||
        !std::ranges::equal(a.graph().layerOutputs(), b.graph().layerOutputs()) ||
        a.graph().merges().size() != b.graph().merges().size() || a.nodeLayout() != b.nodeLayout())
        return false;
    for (const auto& merge : a.graph().merges()) {
        const auto* other = b.graph().merge(merge.nodeId());
        if (other == nullptr || !std::ranges::equal(merge.entries(), other->entries()))
            return false;
    }
    return true;
}

// Undo restores `before` exactly and redo restores `after` exactly.
void expectExactUndoRedo(Fixture& fixture, const Snapshot& before, const std::string_view what) {
    const auto after = fixture.document.snapshot();
    const auto id = fixture.composition;
    expect(fixture.stack.undo().changed(), std::string(what) + ": undoes");
    expect(sameContent(*fixture.document.snapshot().project().findComposition(id),
                       *before.project().findComposition(id)),
           std::string(what) + ": undo restores every record exactly");
    expect(fixture.stack.redo().changed(), std::string(what) + ": redoes");
    expect(sameContent(*fixture.document.snapshot().project().findComposition(id),
                       *after.project().findComposition(id)),
           std::string(what) + ": redo restores every record exactly");
}

void firstEditCreatesTheEntryLayerInOneStep() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto source = fixture.addSolid();
    const auto slot = fixture.connect(source, merge);
    expect(entryLayer(fixture.comp(), merge, slot) == nullptr, "a plain entry has no layer");
    const auto before = fixture.document.snapshot();
    const auto history = fixture.stack.size();
    const auto parameters = parameterCount(fixture.comp());
    const auto result = fixture.edit(merge, slot, [&](const LayerId layer) {
        return std::make_unique<SetLayerSolo>(fixture.composition, layer, true);
    });
    expect(result.changed() && fixture.stack.size() == history + 1,
           "the first edit of an entry control is one undo step");
    const auto* layer = entryLayer(fixture.comp(), merge, slot);
    expect(layer != nullptr && layer->entryOwned() && layer->nodeId == merge &&
               layer->entrySlot == slot && layer->pivot == LayerPivot::Frame && layer->solo,
           "the edit created the entry's own layer and applied to it");
    if (layer == nullptr)
        return;
    expect(result.outputId<LayerId>(kEntryLayerOutput) == layer->layerId,
           "the edit outputs the entry's layer");
    expect(fixture.comp().graph().merge(merge)->find(slot)->layerId == layer->layerId,
           "the slot names its entry layer");
    expect(layer->parameters.size() == 6 && parameterCount(fixture.comp()) == parameters + 6,
           "the entry layer owns six parameters; Time Remap is not bound");
    const auto* position = constant(fixture.comp(), *layer, kPositionParameterRole);
    const auto* opacity = constant(fixture.comp(), *layer, kOpacityParameterRole);
    const auto* scale = constant(fixture.comp(), *layer, kScaleParameterRole);
    expect(position && *position == ParameterValue{Vec2d{32.0, 16.0}} && opacity &&
               *opacity == ParameterValue{1.0} && scale && *scale == ParameterValue{kDefaultScale},
           "the entry layer starts at the identity: centred position, full opacity, unit scale");
    expect(layer->name == "Solid" && !layer->nameFollowsSource,
           "a source without a name of its own names the layer by its kind");
    expect(fixture.document.snapshot().project().validate().ok(), "the project validates");
    expectExactUndoRedo(fixture, before, "first entry edit");

    // A second edit reuses the layer.
    const auto layers = fixture.comp().graph().layerOutputs().size();
    const auto again = fixture.edit(merge, slot, [&](const LayerId id) {
        return std::make_unique<SetLayerSolo>(fixture.composition, id, false);
    });
    expect(again.changed() && fixture.comp().graph().layerOutputs().size() == layers,
           "a later edit edits the same entry layer");
    // A no-op edit of a plain entry changes nothing at all.
    const auto other = fixture.connect(fixture.addSolid(), merge);
    const auto revision = fixture.document.snapshot().revision();
    const auto noop = fixture.edit(merge, other, [&](const LayerId id) {
        return std::make_unique<SetLayerSolo>(fixture.composition, id, false);
    });
    expect(noop.status == CommandStatus::NoChange &&
               fixture.document.snapshot().revision() == revision &&
               entryLayer(fixture.comp(), merge, other) == nullptr,
           "an edit that changes nothing creates no entry layer");
    // An entry layer parameter is set through layerParameterBinding.
    const auto* own = entryLayer(fixture.comp(), merge, slot);
    const auto opacityId =
        layerParameterBinding(fixture.comp().graph(), *own, kOpacityParameterRole)->parameterId;
    fixture.must<SetParameterSource>(fixture.composition, opacityId,
                                     ParameterSource{ConstantValueSource{0.5}});
    expect(*constant(fixture.comp(), *entryLayer(fixture.comp(), merge, slot),
                     kOpacityParameterRole) == ParameterValue{0.5},
           "an entry layer's parameter takes a constant");
    const auto driver = fixture.addNode(kScalarValueNodeType);
    const auto driven =
        fixture.run<SetParameterSource>(fixture.composition, opacityId,
                                        ParameterSource{DriverBindingSource{driver, "value"}});
    expect(driven.status == CommandStatus::Rejected,
           "an entry layer's parameter refuses a driver: insert a Layer node to drive it");
}

void ensureReportsALayerNodeEntry() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto source = fixture.addSolid();
    const auto added = fixture.must<AddNode>(fixture.composition,
                                             std::string(kLayerOutputNodeType), Vec2d{0, 0});
    const auto node = *added.outputId<NodeId>(kAddNodeOutput);
    const auto layer = *added.outputId<LayerId>("layer");
    fixture.link(source, node);
    const auto slot = fixture.connect(node, merge);
    const auto ensured = fixture.run<EnsureEntryLayer>(fixture.composition, merge, slot);
    expect(ensured.status == CommandStatus::NoChange &&
               ensured.outputId<LayerId>(kEntryLayerOutput) == layer,
           "an entry fed by a Layer node already has that layer");
    // SetLayerEnabled on an entry layer never mutes the Merge; on a Layer node it mutes the node.
    const auto plain = fixture.connect(fixture.addSolid(), merge);
    fixture.edit(merge, plain, [&](const LayerId id) {
        return std::make_unique<SetLayerEnabled>(fixture.composition, id, false);
    });
    const auto* own = entryLayer(fixture.comp(), merge, plain);
    expect(own != nullptr && own->entryOwned() && !own->enabled &&
               !nodeMuted(fixture.comp().nodeLayout(), merge),
           "disabling an entry layer leaves its Merge unmuted");
}

void timeRemapBindsOnTheEntryLayer() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto slot = fixture.connect(fixture.addSolid(), merge);
    const auto before = fixture.document.snapshot();
    const auto enabled = fixture.edit(merge, slot, [&](const LayerId id) {
        return std::make_unique<EnableLayerTimeRemap>(fixture.composition, id);
    });
    const auto* layer = entryLayer(fixture.comp(), merge, slot);
    expect(enabled.changed() && layer != nullptr &&
               layerParameterBinding(fixture.comp().graph(), *layer, kTimeRemapParameterRole),
           "Time Remap binds on the entry layer itself");
    expect(fixture.document.snapshot().project().validate().ok(),
           "an entry layer with Time Remap validates");
    expectExactUndoRedo(fixture, before, "entry Time Remap");
    fixture.must<DisableLayerTimeRemap>(fixture.composition, layer->layerId);
    layer = entryLayer(fixture.comp(), merge, slot);
    expect(!layerParameterBinding(fixture.comp().graph(), *layer, kTimeRemapParameterRole) &&
               noOrphanParameters(fixture.comp()),
           "disabling Time Remap erases its binding and parameter");
}

void insertingALayerNodeAdoptsTheEntrysValues() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto source = fixture.addSolid();
    const auto slot = fixture.connect(source, merge);
    const auto matteSlot = fixture.connect(fixture.addSolid(), merge);
    fixture.edit(merge, slot, [&](const LayerId id) {
        return std::make_unique<SetLayerSolo>(fixture.composition, id, true);
    });
    const auto entry = *entryLayer(fixture.comp(), merge, slot);
    fixture.must<SetParameterSource>(
        fixture.composition,
        layerParameterBinding(fixture.comp().graph(), entry, kOpacityParameterRole)->parameterId,
        ParameterSource{ConstantValueSource{0.25}});
    // A matte target pointing at the entry layer follows it into the Layer node.
    fixture.edit(merge, matteSlot, [&](const LayerId id) {
        return std::make_unique<SetTrackMatte>(fixture.composition, id,
                                               TrackMatte{entry.layerId, TrackMatteChannel::Alpha});
    });
    fixture.must<RenameLayer>(fixture.composition, entry.layerId, std::string("Hero"));
    const auto before = fixture.document.snapshot();
    const auto history = fixture.stack.size();
    const auto inserted = fixture.must<InsertLayerNode>(fixture.composition, merge, slot);
    expect(fixture.stack.size() == history + 1, "inserting a Layer node is one undo step");
    const auto node = inserted.outputId<NodeId>(kEntryLayerNodeOutput);
    const auto* layer = entryLayer(fixture.comp(), merge, slot);
    expect(node && layer != nullptr && !layer->entryOwned() && layer->nodeId == *node &&
               inserted.outputId<LayerId>(kEntryLayerOutput) == layer->layerId,
           "the new Layer node is the entry's layer");
    if (layer == nullptr)
        return;
    expect(fixture.comp().graph().findLayer(entry.layerId) == nullptr,
           "the entry layer it adopted is gone");
    expect(layer->solo && layer->name == "Hero" && layer->pivot == LayerPivot::Frame &&
               *constant(fixture.comp(), *layer, kOpacityParameterRole) == ParameterValue{0.25},
           "the Layer node adopted the entry's flags, name, pivot and values");
    const auto* matted = entryLayer(fixture.comp(), merge, matteSlot);
    expect(matted && matted->trackMatte && matted->trackMatte->source == layer->layerId,
           "a matte naming the entry layer now names the Layer node");
    const auto* slotEdge = mergeSlotImageEdge(fixture.comp().graph(), merge, slot);
    expect(slotEdge && slotEdge->source.nodeId == *node &&
               fixture.comp().graph().merge(merge)->find(slot)->layerId == layer->layerId,
           "the slot is fed by and names the Layer node");
    expect(noOrphanParameters(fixture.comp()) &&
               fixture.document.snapshot().project().validate().ok(),
           "no entry-layer parameter is left behind and the project validates");
    expectExactUndoRedo(fixture, before, "insert Layer node");

    // Dissolving the Layer node returns its values to the entry, in the same slot.
    const auto layerId = layer->layerId;
    const auto beforeDissolve = fixture.document.snapshot();
    fixture.must<DissolveNode>(fixture.composition, *node);
    const auto* returned = entryLayer(fixture.comp(), merge, slot);
    expect(returned && returned->entryOwned() && returned->solo && returned->name == "Hero" &&
               returned->pivot == LayerPivot::Frame &&
               *constant(fixture.comp(), *returned, kOpacityParameterRole) == ParameterValue{0.25},
           "dissolving the Layer node keeps the entry and returns its values");
    const auto* edge = mergeSlotImageEdge(fixture.comp().graph(), merge, slot);
    expect(edge && edge->source.nodeId == source, "the slot takes the Layer node's content source");
    const auto* stillMatted = entryLayer(fixture.comp(), merge, matteSlot);
    expect(returned && stillMatted && stillMatted->trackMatte &&
               stillMatted->trackMatte->source == returned->layerId &&
               fixture.comp().graph().findLayer(layerId) == nullptr,
           "the matte follows the values back to the entry layer");
    expect(noOrphanParameters(fixture.comp()) &&
               fixture.document.snapshot().project().validate().ok(),
           "the returned entry validates with no orphan");
    expectExactUndoRedo(fixture, beforeDissolve, "dissolve Layer node");
}

void anExistingLayerNodeWinsUnlessDefault() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto slot = fixture.connect(fixture.addSolid(), merge);
    fixture.edit(merge, slot, [&](const LayerId id) {
        return std::make_unique<SetLayerSolo>(fixture.composition, id, true);
    });
    // A Layer node with its own values, connected into the entry over its own layer: it wins.
    const auto added = fixture.must<AddNode>(fixture.composition,
                                             std::string(kLayerOutputNodeType), Vec2d{0, 0});
    const auto node = *added.outputId<NodeId>(kAddNodeOutput);
    const auto layer = *added.outputId<LayerId>("layer");
    fixture.link(fixture.addSolid(), node);
    fixture.must<SetLayerShy>(fixture.composition, layer, true);
    fixture.must<SetParameterSource>(fixture.composition,
                                     *added.outputId<ParameterId>("parameter.opacity"),
                                     ParameterSource{ConstantValueSource{0.75}});
    fixture.must<ConnectPorts>(fixture.composition, OutputPortRef{node, "image"},
                               InputPortRef{LayerStackInputRef{merge, slot, "content"}});
    const auto* now = entryLayer(fixture.comp(), merge, slot);
    expect(now && now->layerId == layer && !now->solo &&
               *constant(fixture.comp(), *now, kOpacityParameterRole) == ParameterValue{0.75},
           "an existing Layer node with its own values wins over the entry layer");
    expect(std::ranges::none_of(fixture.comp().graph().layerOutputs(),
                                [](const auto& boundary) { return boundary.entryOwned(); }) &&
               noOrphanParameters(fixture.comp()) &&
               fixture.document.snapshot().project().validate().ok(),
           "the replaced entry layer and its parameters are gone");
}

// The first input and output of `node` that carry `kind`.
std::string inputOfKind(const Composition& composition, const NodeId node, const SocketValueKind kind) {
    const auto* record = composition.graph().findNode(node);
    const auto* definition = builtInNodeDefinitions().find(record->typeId, record->schemaVersion);
    const auto found = std::ranges::find(definition->inputs, kind, &InputPortDefinition::valueKind);
    if (found == definition->inputs.end())
        throw std::runtime_error("fixture node has no such input");
    return found->name;
}
std::string outputOfKind(const Composition& composition, const NodeId node, const SocketValueKind kind) {
    const auto* record = composition.graph().findNode(node);
    const auto* definition = builtInNodeDefinitions().find(record->typeId, record->schemaVersion);
    const auto found = std::ranges::find(definition->outputs, kind, &OutputPortDefinition::valueKind);
    if (found == definition->outputs.end())
        throw std::runtime_error("fixture node has no such output");
    return found->name;
}
bool feeds(const Composition& composition, const NodeId source, const InputPortRef& destination) {
    return std::ranges::any_of(composition.graph().edges(), [&](const EdgeRecord& edge) {
        return edge.source.nodeId == source && edge.destination == destination;
    });
}

// #723: dissolving a node whose output feeds a Merge Content slot puts its upstream into that same
// slot. The entry keeps its place, its own layer and that layer's settings; one undo restores the
// node and every link. With a plain image source upstream, and with an Adjustment.
void dissolveReconnectsIntoTheSameMergeSlot() {
    for (const bool adjustment : {false, true}) {
        const std::string what = adjustment ? "Adjustment upstream" : "plain source upstream";
        Fixture fixture;
        const auto merge = fixture.mainMerge();
        (void)fixture.connect(fixture.addSolid(), merge);
        NodeId upstream;
        LayerSlotId slot;
        const auto curves = fixture.addNode(kCurvesNodeType);
        const auto curvesIn = inputOfKind(fixture.comp(), curves, SocketValueKind::Image);
        const auto curvesOut = outputOfKind(fixture.comp(), curves, SocketValueKind::Image);
        if (adjustment) {
            const auto added = fixture.must<AddAdjustmentLayer>(fixture.composition, merge);
            upstream = *added.outputId<NodeId>(kEntryLayerNodeOutput);
            slot = *added.outputId<LayerSlotId>(kEntryLayerSlotOutput);
            fixture.link(upstream, curves, curvesIn);
            fixture.must<ConnectPorts>(
                fixture.composition, OutputPortRef{curves, curvesOut},
                InputPortRef{LayerStackInputRef{merge, slot, "content"}});
        } else {
            upstream = fixture.addSolid();
            fixture.link(upstream, curves, curvesIn);
            slot = fixture.connect(curves, merge, curvesOut);
        }
        (void)fixture.connect(fixture.addSolid(), merge);
        // A setting of the entry's own layer, so there is something to keep.
        expect(fixture
                   .edit(merge, slot,
                         [&](const LayerId layer) {
                             return std::make_unique<SetLayerSolo>(fixture.composition, layer, true);
                         })
                   .changed(),
               what + ": the entry has a layer setting");
        const auto before = fixture.document.snapshot();
        const auto& was = *before.project().findComposition(fixture.composition);
        const auto* layerBefore = entryLayer(was, merge, slot);
        const std::vector<LayerStackEntry> entriesBefore(was.graph().merge(merge)->entries().begin(),
                                                         was.graph().merge(merge)->entries().end());
        const InputPortRef content = LayerStackInputRef{merge, slot, "content"};
        expect(layerBefore != nullptr && layerBefore->solo && feeds(was, curves, content),
               what + ": the chain is source, Curves, Content slot");
        expect(dissolveRefusals(was, curves).empty(), what + ": every link can be kept");
        const auto history = fixture.stack.size();
        fixture.must<DissolveNode>(fixture.composition, curves);
        const auto& now = fixture.comp();
        expect(now.graph().findNode(curves) == nullptr && fixture.stack.size() == history + 1,
               what + ": Dissolve removes the node as one step");
        expect(feeds(now, upstream, content), what + ": the upstream feeds the same slot");
        expect(std::ranges::equal(now.graph().merge(merge)->entries(), entriesBefore),
               what + ": the Merge keeps its entries, in the same order");
        const auto* layerNow = entryLayer(now, merge, slot);
        expect(layerNow != nullptr && layerBefore != nullptr && *layerNow == *layerBefore,
               what + ": the entry keeps its own layer and every setting of it");
        if (adjustment)
            expect(mergeEntryAdjustment(now.graph(), merge, slot) == upstream,
                   what + ": the Adjustment serves the entry directly");
        expect(noOrphanParameters(now) && fixture.document.snapshot().project().validate().ok(),
               what + ": the project validates");
        expectExactUndoRedo(fixture, before, "dissolve into a Merge slot (" + what + ")");
    }
}

// #723: several consumers at once, each on its own. A Layer node feeds one entry of the Merge
// directly and a Curves that feeds a second entry and another node. Dissolving the Curves keeps
// the link into the other node; the second entry cannot take the Layer node (the Merge already
// holds it), so that one link is not kept, and the refusal says which and why.
void dissolveKeepsWhatItCanAndNamesWhatItCannot() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto added =
        fixture.must<AddNode>(fixture.composition, std::string(kLayerOutputNodeType), Vec2d{0, 0});
    const auto layerNode = *added.outputId<NodeId>(kAddNodeOutput);
    fixture.link(fixture.addSolid(), layerNode);
    const auto direct = fixture.connect(layerNode, merge);
    const auto curves = fixture.addNode(kCurvesNodeType);
    const auto curvesIn = inputOfKind(fixture.comp(), curves, SocketValueKind::Image);
    const auto curvesOut = outputOfKind(fixture.comp(), curves, SocketValueKind::Image);
    fixture.link(layerNode, curves, curvesIn);
    const auto second = fixture.connect(curves, merge, curvesOut);
    const auto other = fixture.addNode(kCurvesNodeType);
    fixture.must<ConnectPorts>(fixture.composition, OutputPortRef{curves, curvesOut},
                               InputPortRef{NodeInputRef{other, curvesIn}});
    const auto before = fixture.document.snapshot();
    const auto refusals = dissolveRefusals(fixture.comp(), curves);
    const InputPortRef refusedSlot = LayerStackInputRef{merge, second, "content"};
    expect(refusals.size() == 1 && refusals.front().destination == refusedSlot &&
               refusals.front().reason == DissolveRefusalReason::LayerAlreadyInMerge,
           "the one link that cannot be kept is named with its reason");
    fixture.must<DissolveNode>(fixture.composition, curves);
    const auto& now = fixture.comp();
    expect(now.graph().findNode(curves) == nullptr, "the node is removed");
    expect(feeds(now, layerNode, InputPortRef{NodeInputRef{other, curvesIn}}),
           "the consumer that can take the upstream is reconnected");
    expect(feeds(now, layerNode, InputPortRef{LayerStackInputRef{merge, direct, "content"}}) &&
               !feeds(now, layerNode, refusedSlot),
           "the Layer node keeps its own entry and is not given a second one");
    expect(fixture.document.snapshot().project().validate().ok(), "the project validates");
    expectExactUndoRedo(fixture, before, "dissolve with a refusing consumer");
}

// #723: a value node's upstream arrives as the driver of its operand, and what it drove is a
// parameter. Dissolving it moves that driver to the upstream; undo restores it.
void dissolveMovesAParameterDriverToTheUpstream() {
    Fixture fixture;
    const auto value = fixture.addNode(kScalarValueNodeType);
    const auto middle = fixture.addNode(kScalarMathNodeType);
    const auto consumer = fixture.addNode(kScalarMathNodeType);
    const auto operand = inputOfKind(fixture.comp(), middle, SocketValueKind::Scalar);
    const auto result = outputOfKind(fixture.comp(), middle, SocketValueKind::Scalar);
    const auto valueOut = outputOfKind(fixture.comp(), value, SocketValueKind::Scalar);
    fixture.must<ConnectPorts>(fixture.composition, OutputPortRef{value, valueOut},
                               InputPortRef{NodeInputRef{middle, operand}});
    fixture.must<ConnectPorts>(fixture.composition, OutputPortRef{middle, result},
                               InputPortRef{NodeInputRef{consumer, operand}});
    const auto driverOf = [&](const Composition& composition) -> const DriverBindingSource* {
        const auto* node = composition.graph().findNode(consumer);
        const auto binding = std::ranges::find(node->parameters, operand, &ParameterBinding::role);
        const auto* record = binding != node->parameters.end()
                                 ? composition.parameters().find(binding->parameterId)
                                 : nullptr;
        return record ? std::get_if<DriverBindingSource>(&record->source) : nullptr;
    };
    expect(driverOf(fixture.comp()) != nullptr && driverOf(fixture.comp())->sourceNodeId == middle,
           "the consumer's operand is driven by the middle node");
    const auto before = fixture.document.snapshot();
    fixture.must<DissolveNode>(fixture.composition, middle);
    const auto* driver = driverOf(fixture.comp());
    expect(fixture.comp().graph().findNode(middle) == nullptr && driver != nullptr &&
               driver->sourceNodeId == value && driver->outputPort == valueOut,
           "after Dissolve the operand is driven by the upstream value");
    expect(fixture.document.snapshot().project().validate().ok(), "the project validates");
    expectExactUndoRedo(fixture, before, "dissolve a value node");
}

void aSharedLayerNodeReturnsToEveryEntry() {
    Fixture fixture;
    const auto main = fixture.mainMerge();
    const auto nested = fixture.addNode(kLayerStackNodeType);
    const auto added = fixture.must<AddNode>(fixture.composition,
                                             std::string(kLayerOutputNodeType), Vec2d{0, 0});
    const auto node = *added.outputId<NodeId>(kAddNodeOutput);
    const auto layer = *added.outputId<LayerId>("layer");
    fixture.link(fixture.addSolid(), node);
    const auto first = fixture.connect(node, main);
    const auto second = fixture.connect(node, nested);
    fixture.connect(nested, main);
    expect(mergeEntriesOfLayer(fixture.comp().graph(), layer).size() == 2,
           "a Layer node reaching two entries is shared");
    fixture.must<SetLayerSolo>(fixture.composition, layer, true);
    fixture.must<DissolveNode>(fixture.composition, node);
    const auto* a = entryLayer(fixture.comp(), main, first);
    const auto* b = entryLayer(fixture.comp(), nested, second);
    expect(a && b && a->entryOwned() && b->entryOwned() && a->layerId != b->layerId && a->solo &&
               b->solo && a->parameters.size() == 6 && b->parameters.size() == 6,
           "each entry of a shared Layer node gets its own copy of its values");
    expect(noOrphanParameters(fixture.comp()) &&
               fixture.document.snapshot().project().validate().ok(),
           "both returned entries validate");
}

void layerCommandsWorkOnEntryLayers() {
    Fixture fixture;
    const auto main = fixture.mainMerge();
    const auto nested = fixture.addNode(kLayerStackNodeType);
    const auto source = fixture.addSolid();
    const auto slot = fixture.connect(source, nested);
    const auto top = fixture.connect(fixture.addSolid(), nested);
    fixture.connect(nested, main);
    fixture.edit(nested, slot, [&](const LayerId id) {
        PathValue path;
        path.editableContour().closed = true;
        path.editableContour().anchors = {{{0.0, 0.0}, std::nullopt, std::nullopt},
                                          {{8.0, 0.0}, std::nullopt, std::nullopt},
                                          {{8.0, 8.0}, std::nullopt, std::nullopt}};
        return std::make_unique<AddLayerMask>(fixture.composition, id, std::move(path));
    });
    const auto entry = entryLayer(fixture.comp(), nested, slot)->layerId;

    // Move within the nested Merge.
    fixture.must<MoveLayerBefore>(fixture.composition, slot, std::optional{top});
    expect(fixture.comp().graph().merge(nested)->entries().front().slotId == slot,
           "MoveLayerBefore moves a slot of a nested Merge");

    // Duplicate: a new slot below with the same source and a copy of the entry layer.
    auto before = fixture.document.snapshot();
    const auto duplicated =
        fixture.must<DuplicateLayers>(fixture.composition, std::vector<LayerId>{entry});
    const auto copy = duplicated.outputId<LayerId>("layer");
    const auto* copied = copy ? fixture.comp().graph().findLayer(*copy) : nullptr;
    expect(copied && copied->entryOwned() && copied->entrySlot != std::optional{slot} &&
               copied->masks.size() == 1 &&
               copied->masks.front().id != entryLayer(fixture.comp(), nested, slot)->masks.front().id,
           "duplicating an entry layer copies it into a new slot with fresh ids");
    if (copied != nullptr) {
        const auto entries = fixture.comp().graph().merge(nested)->entries();
        expect(entries.size() == 3 && entries[1].slotId == *copied->entrySlot &&
                   mergeSlotImageEdge(fixture.comp().graph(), nested, *copied->entrySlot)
                           ->source.nodeId == source,
               "the copy sits directly below and shows the same source");
        const auto* a = layerParameterBinding(fixture.comp().graph(), *copied, "opacity");
        const auto* b = layerParameterBinding(fixture.comp().graph(),
                                              *entryLayer(fixture.comp(), nested, slot), "opacity");
        expect(a && b && a->parameterId != b->parameterId, "the copy owns its own parameters");
    }
    expect(fixture.document.snapshot().project().validate().ok(), "the duplicate validates");
    expectExactUndoRedo(fixture, before, "duplicate entry layer");

    // Split: the tail is a copy in a new slot from the split time.
    before = fixture.document.snapshot();
    const auto split = fixture.must<SplitLayerAtTime>(fixture.composition, entry,
                                                      core::RationalTime::fromInteger(2));
    const auto tail = split.outputId<LayerId>("layer");
    const auto* tailLayer = tail ? fixture.comp().graph().findLayer(*tail) : nullptr;
    expect(tailLayer && tailLayer->entryOwned() &&
               tailLayer->inPoint == core::RationalTime::fromInteger(2) &&
               fixture.comp().graph().findLayer(entry)->outPoint ==
                   core::RationalTime::fromInteger(2),
           "splitting an entry layer splits its range across two slots");
    expect(fixture.document.snapshot().project().validate().ok(), "the split validates");
    expectExactUndoRedo(fixture, before, "split entry layer");

    // Delete: the slot leaves, the Merge and the source stay, no parameter is left.
    before = fixture.document.snapshot();
    fixture.must<DeleteLayers>(fixture.composition, std::vector<LayerId>{entry});
    expect(fixture.comp().graph().findLayer(entry) == nullptr &&
               fixture.comp().graph().merge(nested) != nullptr &&
               fixture.comp().graph().merge(nested)->find(slot) == nullptr &&
               fixture.comp().graph().findNode(source) != nullptr &&
               noOrphanParameters(fixture.comp()),
           "deleting an entry layer removes its slot and parameters, never the Merge");
    expect(fixture.document.snapshot().project().validate().ok(), "the delete validates");
    expectExactUndoRedo(fixture, before, "delete entry layer");
}

void duplicatingAndRemovingAMergeCarriesItsEntryLayers() {
    Fixture fixture;
    const auto main = fixture.mainMerge();
    const auto nested = fixture.addNode(kLayerStackNodeType);
    const auto slot = fixture.connect(fixture.addSolid(), nested);
    fixture.connect(nested, main);
    fixture.edit(nested, slot, [&](const LayerId id) {
        return std::make_unique<SetLayerSolo>(fixture.composition, id, true);
    });
    const auto duplicated =
        fixture.must<DuplicateNodes>(fixture.composition, std::set<NodeId>{nested}, Vec2d{40, 0});
    const auto copy = duplicated.outputId<NodeId>("node." + std::to_string(nested.value()));
    const auto* stack = copy ? fixture.comp().graph().merge(*copy) : nullptr;
    const auto* layer =
        stack && !stack->entries().empty()
            ? entryLayer(fixture.comp(), *copy, stack->entries().front().slotId)
            : nullptr;
    const auto* original = entryLayer(fixture.comp(), nested, slot);
    expect(layer && original && layer->entryOwned() && layer->nodeId == *copy && layer->solo &&
               layer->layerId != original->layerId &&
               layer->parameters.front().parameterId != original->parameters.front().parameterId,
           "a duplicated Merge copies its entry layers with fresh ids");
    expect(fixture.document.snapshot().project().validate().ok(), "the duplicate validates");
    const auto before = fixture.document.snapshot();
    fixture.must<RemoveNodes>(fixture.composition, std::set<NodeId>{nested});
    expect(fixture.comp().graph().findLayer(original ? original->layerId : LayerId{}) == nullptr &&
               noOrphanParameters(fixture.comp()) &&
               fixture.document.snapshot().project().validate().ok(),
           "removing a Merge drops its entry layers and their parameters");
    expectExactUndoRedo(fixture, before, "remove Merge");
}

void disconnectingAnEntryRetiresItsLayer() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto slot = fixture.connect(fixture.addSolid(), merge);
    const auto other = fixture.connect(fixture.addSolid(), merge);
    fixture.edit(merge, slot, [&](const LayerId id) {
        return std::make_unique<SetLayerSolo>(fixture.composition, id, true);
    });
    const auto entry = entryLayer(fixture.comp(), merge, slot)->layerId;
    fixture.edit(merge, other, [&](const LayerId id) {
        return std::make_unique<SetLayerParent>(fixture.composition, id, std::optional{entry});
    });
    const auto before = fixture.document.snapshot();
    fixture.must<DisconnectInput>(fixture.composition,
                                  InputPortRef{LayerStackInputRef{merge, slot, "content"}});
    const auto* child = entryLayer(fixture.comp(), merge, other);
    expect(fixture.comp().graph().findLayer(entry) == nullptr && child && !child->parent &&
               noOrphanParameters(fixture.comp()) &&
               fixture.document.snapshot().project().validate().ok(),
           "a disconnected entry's layer leaves with its parameters and its references");
    expectExactUndoRedo(fixture, before, "disconnect entry");
}

void lockedEntryLayersRefuseEdits() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto slot = fixture.connect(fixture.addSolid(), merge);
    fixture.edit(merge, slot, [&](const LayerId id) {
        return std::make_unique<SetLayerLocked>(fixture.composition, id, true);
    });
    const auto* layer = entryLayer(fixture.comp(), merge, slot);
    expect(layer && layer->locked, "an entry layer locks");
    const auto opacity =
        layerParameterBinding(fixture.comp().graph(), *layer, kOpacityParameterRole)->parameterId;
    expect(fixture.comp().parameterLocked(opacity), "a locked entry layer locks its parameters");
    expect(fixture
                   .run<SetParameterSource>(fixture.composition, opacity,
                                            ParameterSource{ConstantValueSource{0.5}})
                   .status == CommandStatus::Rejected,
           "a locked entry layer's parameter refuses an edit");
    expect(fixture.run<InsertLayerNode>(fixture.composition, merge, slot).status ==
               CommandStatus::Rejected,
           "a Layer node is not inserted into a locked entry");
}

void adjustmentLayersAreAddedAboveOrAtTheTop() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto bottom = fixture.connect(fixture.addSolid(), merge);
    const auto middle = fixture.connect(fixture.addSolid(), merge);
    // ConnectPorts appends at the bottom: entries are [bottom, middle] topmost first.
    const auto before = fixture.document.snapshot();
    const auto added = fixture.must<AddAdjustmentLayer>(fixture.composition, merge);
    const auto node = added.outputId<NodeId>(kEntryLayerNodeOutput);
    const auto slot = added.outputId<LayerSlotId>(kEntryLayerSlotOutput);
    const auto layer = added.outputId<LayerId>(kEntryLayerOutput);
    expect(node && slot && layer, "Add Adjustment Layer outputs node, slot and layer");
    if (!node || !slot || !layer)
        return;
    const auto entries = fixture.comp().graph().merge(merge)->entries();
    expect(entries.front().slotId == *slot, "with no row selected it lands at the top");
    expect(fixture.comp().graph().findNode(*node)->typeId == kAdjustmentNodeType &&
               mergeEntryAdjustment(fixture.comp().graph(), merge, *slot) == *node,
           "the Adjustment node serves its new entry");
    const auto* boundary = fixture.comp().graph().findLayer(*layer);
    expect(boundary && boundary->entryOwned() && boundary->name == "Adjustment Layer 1",
           "the entry layer is named Adjustment Layer 1");
    expect(fixture.document.snapshot().project().validate().ok(), "the project validates");
    expectExactUndoRedo(fixture, before, "add adjustment layer");
    const auto second =
        fixture.must<AddAdjustmentLayer>(fixture.composition, merge, std::optional{middle});
    const auto secondSlot = second.outputId<LayerSlotId>(kEntryLayerSlotOutput);
    const auto after = fixture.comp().graph().merge(merge)->entries();
    const auto at = std::ranges::find(after, *secondSlot, &LayerStackEntry::slotId);
    expect(at != after.end() && std::next(at) != after.end() && std::next(at)->slotId == middle &&
               fixture.comp().graph().findLayer(*second.outputId<LayerId>(kEntryLayerOutput))->name ==
                   "Adjustment Layer 2",
           "with a row selected it lands directly above it");
    static_cast<void>(bottom);
}

void reconcileSkipsParameterOnlyEdits() {
    Fixture fixture;
    const auto merge = fixture.mainMerge();
    const auto slot = fixture.connect(fixture.addSolid(), merge);
    fixture.edit(merge, slot, [&](const LayerId id) {
        return std::make_unique<SetLayerSolo>(fixture.composition, id, true);
    });
    const auto* layer = entryLayer(fixture.comp(), merge, slot);
    const auto opacity =
        layerParameterBinding(fixture.comp().graph(), *layer, kOpacityParameterRole)->parameterId;
    const auto work = fixture.stack.work();
    fixture.must<SetParameterSource>(fixture.composition, opacity,
                                     ParameterSource{ConstantValueSource{0.5}});
    const auto next = fixture.stack.work();
    expect(next.entryLayerReconcilePasses == work.entryLayerReconcilePasses + 1 &&
               next.entryLayerReconcileSkips == work.entryLayerReconcileSkips + 1 &&
               next.entryLayerReconcileCompositions == work.entryLayerReconcileCompositions &&
               next.entryLayerReconcileEdits == work.entryLayerReconcileEdits,
           "a parameter edit is skipped by the reconcile pass after one comparison");
    fixture.must<InsertLayerNode>(fixture.composition, merge, slot);
    const auto inserted = fixture.stack.work();
    expect(inserted.entryLayerReconcileCompositions == next.entryLayerReconcileCompositions + 1 &&
               inserted.entryLayerReconcileEdits == next.entryLayerReconcileEdits + 1,
           "a topology edit runs the pass, which adopts");
}

} // namespace

// #714: deleting a layer deletes every node only that layer uses, as one undo step; a node
// another layer (or anything else that stays) still reads is kept.
void deletingALayerTakesItsOwnSourcesAndKeepsSharedOnes() {
    Fixture fixture;
    const auto main = fixture.mainMerge();
    // Layer A: Solid -> Grade -> slot. Both nodes are its own.
    const auto ownSolid = fixture.addSolid();
    const auto ownGrade = fixture.addNode(kGradeNodeType);
    fixture.link(ownSolid, ownGrade, "input");
    const auto slotA = fixture.connect(ownGrade, main);
    // Layers B and C read one Solid.
    const auto sharedSolid = fixture.addSolid();
    const auto slotB = fixture.connect(sharedSolid, main);
    const auto slotC = fixture.connect(sharedSolid, main);
    // Layer D reads a Grade of its own that reads the shared Solid.
    const auto gradeD = fixture.addNode(kGradeNodeType);
    fixture.link(sharedSolid, gradeD, "input");
    const auto slotD = fixture.connect(gradeD, main);
    // A node nothing reads: it was never a layer's, so no layer's deletion takes it.
    const auto loose = fixture.addSolid();
    const auto layerOf = [&](const LayerSlotId slot) {
        const auto layer = fixture.must<EnsureEntryLayer>(fixture.composition, main, slot)
                               .outputId<LayerId>(kEntryLayerOutput);
        if (!layer)
            throw std::runtime_error("fixture entry layer");
        return *layer;
    };
    const auto a = layerOf(slotA);
    const auto b = layerOf(slotB);
    const auto c = layerOf(slotC);
    const auto d = layerOf(slotD);
    const auto has = [&](const NodeId id) { return fixture.comp().graph().findNode(id) != nullptr; };

    auto before = fixture.document.snapshot();
    fixture.must<DeleteLayers>(fixture.composition, std::vector<LayerId>{a});
    expect(fixture.comp().graph().findLayer(a) == nullptr && !has(ownSolid) && !has(ownGrade),
           "deleting a layer deletes the nodes only it uses, the whole chain");
    expect(has(sharedSolid) && has(gradeD) && has(loose) && has(main),
           "nodes the layer did not use stay");
    expect(noOrphanParameters(fixture.comp()) &&
               fixture.document.snapshot().project().validate().ok(),
           "the delete leaves no parameter behind and validates");
    expectExactUndoRedo(fixture, before, "delete a layer with its own sources");

    before = fixture.document.snapshot();
    fixture.must<DeleteLayers>(fixture.composition, std::vector<LayerId>{d});
    expect(!has(gradeD) && has(sharedSolid),
           "a layer's own node goes, the shared source it read stays");
    expectExactUndoRedo(fixture, before, "delete a layer reading a shared source");

    before = fixture.document.snapshot();
    fixture.must<DeleteLayers>(fixture.composition, std::vector<LayerId>{b});
    expect(fixture.comp().graph().findLayer(b) == nullptr && has(sharedSolid),
           "a source another layer still reads stays");
    expectExactUndoRedo(fixture, before, "delete one of two layers sharing a source");

    before = fixture.document.snapshot();
    fixture.must<DeleteLayers>(fixture.composition, std::vector<LayerId>{c});
    expect(!has(sharedSolid) && has(loose),
           "the source goes with the last layer that read it");
    expect(fixture.stack.undo().changed() && has(sharedSolid) &&
               sameContent(fixture.comp(), *before.project().findComposition(fixture.composition)),
           "one undo brings the layer and its node back");

    // Two layers sharing a source, deleted together: the source is theirs alone and goes.
    expect(fixture.stack.undo().changed() && fixture.comp().graph().findLayer(b) != nullptr,
           "the fixture has both sharing layers again");
    before = fixture.document.snapshot();
    fixture.must<DeleteLayers>(fixture.composition, std::vector<LayerId>{b, c});
    expect(!has(sharedSolid) && has(loose) && has(main),
           "a source only the deleted layers share goes with them");
    expectExactUndoRedo(fixture, before, "delete two layers sharing a source");
}

int main() try {
    deletingALayerTakesItsOwnSourcesAndKeepsSharedOnes();
    firstEditCreatesTheEntryLayerInOneStep();
    ensureReportsALayerNodeEntry();
    timeRemapBindsOnTheEntryLayer();
    insertingALayerNodeAdoptsTheEntrysValues();
    anExistingLayerNodeWinsUnlessDefault();
    aSharedLayerNodeReturnsToEveryEntry();
    layerCommandsWorkOnEntryLayers();
    duplicatingAndRemovingAMergeCarriesItsEntryLayers();
    disconnectingAnEntryRetiresItsLayer();
    lockedEntryLayersRefuseEdits();
    adjustmentLayersAreAddedAboveOrAtTheTop();
    dissolveReconnectsIntoTheSameMergeSlot();
    dissolveKeepsWhatItCanAndNamesWhatItCannot();
    dissolveMovesAParameterDriverToTheUpstream();
    reconcileSkipsParameterOnlyEdits();
    if (failures != 0) {
        std::cerr << failures << " entry layer expectation(s) failed\n";
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "entry layer test aborted: " << error.what() << '\n';
    return 1;
}
