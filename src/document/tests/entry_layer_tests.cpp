// #502 document rules for a Merge entry's own layer (document::validateEntryLayers and the graph
// rules it relies on): an entry layer names a slot that names it, binds exactly the entry roles to
// parameters of their schema owned by nothing else, never a driver, and has no Layer node directly
// feeding its slot. mergeEntryLayer finds the nearest Layer node up the chain first.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <functional>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace bloom;
using namespace bloom::document;

int failures = 0;
void expect(const bool condition, const std::string_view message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

constexpr auto kMerge = NodeId::fromRaw(1);
constexpr auto kOutput = NodeId::fromRaw(2);
constexpr auto kSolid = NodeId::fromRaw(3);
constexpr auto kLayerNode = NodeId::fromRaw(4);
constexpr auto kSlot = LayerSlotId::fromRaw(1);
constexpr auto kEntryLayer = LayerId::fromRaw(1);
constexpr auto kNodeLayer = LayerId::fromRaw(2);

struct Options {
    // The slot is fed by a Layer node instead of the solid directly.
    bool throughLayerNode = false;
    std::function<void(LayerOutputBoundary&)> editEntry;
    std::function<void(Composition&)> editComposition;
};

std::vector<ParameterBinding> entryBindings(const std::uint64_t first) {
    return {{"position", ParameterId::fromRaw(first)},     {"anchor", ParameterId::fromRaw(first + 1)},
            {"scale", ParameterId::fromRaw(first + 2)},    {"rotation", ParameterId::fromRaw(first + 3)},
            {"opacity", ParameterId::fromRaw(first + 4)},  {"blendMode", ParameterId::fromRaw(first + 5)}};
}

void insertLayerParameters(Composition& composition, const std::uint64_t first) {
    const bool inserted =
        composition.parameters().insert({ParameterId::fromRaw(first),
                                         std::string(kPositionParameterSchemaKey),
                                         ConstantValueSource{Vec2d{32.0, 16.0}}}) &&
        composition.parameters().insert({ParameterId::fromRaw(first + 1),
                                         std::string(kAnchorParameterSchemaKey),
                                         ConstantValueSource{kDefaultAnchor}}) &&
        composition.parameters().insert({ParameterId::fromRaw(first + 2),
                                         std::string(kScaleParameterSchemaKey),
                                         ConstantValueSource{kDefaultScale}}) &&
        composition.parameters().insert({ParameterId::fromRaw(first + 3),
                                         std::string(kRotationParameterSchemaKey),
                                         ConstantValueSource{kDefaultRotationDegrees}}) &&
        composition.parameters().insert({ParameterId::fromRaw(first + 4),
                                         std::string(kOpacityParameterSchemaKey),
                                         ConstantValueSource{0.5}}) &&
        composition.parameters().insert({ParameterId::fromRaw(first + 5),
                                         std::string(kBlendModeParameterSchemaKey),
                                         ConstantValueSource{kDefaultBlendModeValue}});
    if (!inserted)
        throw std::logic_error("layer parameters");
}

Composition makeComposition(const Options& options = {}) {
    CanonicalGraph graph{kMerge};
    bool ok = graph.addNode({kMerge, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}) &&
              graph.addNode({kOutput, std::string(kCompositionOutputNodeType), {},
                             kCompositionOutputNodeSchemaVersion}) &&
              graph.addNode({kSolid,
                             std::string(kSolidSourceNodeType),
                             {{"color", ParameterId::fromRaw(1)},
                              {"height", ParameterId::fromRaw(3)},
                              {"width", ParameterId::fromRaw(2)}},
                             kSolidSourceNodeSchemaVersion}) &&
              graph.addEdge({EdgeId::fromRaw(1),
                             {kMerge, std::string(kLayerStackOutputPort)},
                             NodeInputRef{kOutput, std::string(kCompositionOutputInputPort)}});
    if (options.throughLayerNode) {
        ok = ok && graph.addNode({kLayerNode, std::string(kLayerOutputNodeType),
                                  entryBindings(20), kLayerOutputNodeSchemaVersion}) &&
             graph.addEdge({EdgeId::fromRaw(2),
                            {kSolid, std::string(kSolidSourceOutputPort)},
                            NodeInputRef{kLayerNode, std::string(kLayerOutputContentInputPort)}}) &&
             graph.addEdge({EdgeId::fromRaw(3),
                            {kLayerNode, std::string(kLayerOutputOutputPort)},
                            LayerStackInputRef{kMerge, kSlot,
                                               std::string(kLayerStackContentInputRole)}}) &&
             graph.addLayerOutput({kLayerNode, kNodeLayer, "Layer",
                                   std::string(kLayerOutputOutputPort)}) &&
             graph.layerStack().append({kSlot, kNodeLayer});
    } else {
        ok = ok && graph.addEdge({EdgeId::fromRaw(3),
                                  {kSolid, std::string(kSolidSourceOutputPort)},
                                  LayerStackInputRef{kMerge, kSlot,
                                                     std::string(kLayerStackContentInputRole)}}) &&
             graph.layerStack().append({kSlot, kEntryLayer});
    }
    LayerOutputBoundary entry{kMerge, kEntryLayer, "Solid", std::string(kLayerStackOutputPort)};
    entry.entrySlot = kSlot;
    entry.pivot = LayerPivot::Frame;
    entry.parameters = entryBindings(10);
    if (options.editEntry)
        options.editEntry(entry);
    ok = ok && graph.addLayerOutput(entry);
    if (!ok)
        throw std::logic_error("entry layer graph");
    graph.setCompositionOutput({kOutput, std::string(kCompositionOutputOutputPort)});
    Composition composition{CompositionId::fromRaw(1), "Main", core::RationalTime::fromInteger(2),
                            std::move(graph),
                            *CompositionFormat::create(64, 32, core::PixelAspectRatio::square(),
                                                       FrameRate::framesPerSecond24())};
    const bool solid =
        composition.parameters().insert({ParameterId::fromRaw(1),
                                         std::string(kSolidColorParameterSchemaKey),
                                         ConstantValueSource{core::Color4d{1.0, 0.0, 0.0, 1.0}}}) &&
        composition.parameters().insert({ParameterId::fromRaw(2),
                                         std::string(kSolidWidthParameterSchemaKey),
                                         ConstantValueSource{64.0}}) &&
        composition.parameters().insert({ParameterId::fromRaw(3),
                                         std::string(kSolidHeightParameterSchemaKey),
                                         ConstantValueSource{32.0}});
    if (!solid)
        throw std::logic_error("solid parameters");
    insertLayerParameters(composition, 10);
    if (options.throughLayerNode)
        insertLayerParameters(composition, 20);
    if (options.editComposition)
        options.editComposition(composition);
    return composition;
}

bool valid(const Composition& composition) { return composition.validate().ok(); }

Options entryEdit(std::function<void(LayerOutputBoundary&)> edit) {
    Options options;
    options.editEntry = std::move(edit);
    return options;
}
Options compositionEdit(std::function<void(Composition&)> edit, const bool throughLayerNode = false) {
    Options options;
    options.throughLayerNode = throughLayerNode;
    options.editComposition = std::move(edit);
    return options;
}
Options viaLayerNode() {
    Options options;
    options.throughLayerNode = true;
    return options;
}

void aWellFormedEntryLayerValidates() {
    const auto composition = makeComposition();
    expect(valid(composition), "an entry layer owning its six parameters validates");
    const auto* layer = mergeEntryLayer(composition.graph(), kMerge, kSlot);
    expect(layer != nullptr && layer->layerId == kEntryLayer && layer->entryOwned(),
           "mergeEntryLayer answers the entry's own layer");
    const auto* opacity = layerParameterBinding(composition.graph(), *layer, kOpacityParameterRole);
    expect(opacity && opacity->parameterId == ParameterId::fromRaw(14),
           "layerParameterBinding reads an entry layer's own binding");
    expect(entryOwnedLayer(composition.graph(), kMerge, kSlot) == layer,
           "entryOwnedLayer answers the same record");
    // The Merge node itself owns no parameters; the entry layer's are not node-bound.
    expect(composition.graph().findNode(kMerge)->parameters.empty(),
           "the Merge keeps no parameter for its entry");
}

void malformedEntryLayersAreRefused() {
    expect(!valid(makeComposition(entryEdit([](LayerOutputBoundary& entry) {
               entry.parameters.pop_back();
           }))),
           "an entry layer missing a role is refused");
    expect(!valid(makeComposition(entryEdit([](LayerOutputBoundary& entry) {
               entry.parameters.push_back({"timeRemap", ParameterId::fromRaw(11)});
           }))),
           "an entry layer binding a parameter of the wrong schema is refused");
    expect(!valid(makeComposition(entryEdit([](LayerOutputBoundary& entry) {
               entry.parameters[0].parameterId = ParameterId::fromRaw(1);
           }))),
           "an entry layer sharing a node's parameter is refused");
    expect(!valid(makeComposition(entryEdit([](LayerOutputBoundary& entry) {
               entry.parameters[0].parameterId = ParameterId::fromRaw(99);
           }))),
           "an entry layer naming a missing parameter is refused");
    expect(!valid(makeComposition(entryEdit([](LayerOutputBoundary& entry) {
                                           entry.entrySlot = LayerSlotId::fromRaw(7);
                                       }))),
           "an entry layer naming a slot its Merge does not have is refused");
    expect(!valid(makeComposition(compositionEdit([](Composition& composition) {
                                           if (!composition.parameters().setSource(
                                                   ParameterId::fromRaw(14),
                                                   DriverBindingSource{kSolid, "image"}))
                                               throw std::logic_error("driver fixture");
                                       }))),
           "an entry layer parameter driven by a node is refused");
    expect(!valid(makeComposition(entryEdit([](LayerOutputBoundary& entry) {
               entry.pivot = static_cast<LayerPivot>(5);
           }))),
           "an unknown pivot is refused");
    // A Layer node does not own parameters on its boundary.
    expect(!valid(makeComposition(compositionEdit([](Composition& composition) {
                                           auto* layer =
                                               composition.graph().findLayer(kNodeLayer);
                                           layer->parameters = entryBindings(10);
                                           (void)composition.graph().eraseLayerOutput(kEntryLayer);
                                       }, true))),
           "a Layer node boundary owning parameters is refused");
}

void aLayerNodeFeedingTheSlotShadowsTheEntryLayer() {
    // The slot is fed directly by a Layer node and names it; an entry layer for that slot is
    // refused (the reconcile pass never leaves one).
    const auto shadowed = makeComposition(viaLayerNode());
    expect(!valid(shadowed), "an entry layer on a slot fed by a Layer node is refused");
    const auto* layer = mergeEntryLayer(shadowed.graph(), kMerge, kSlot);
    expect(layer != nullptr && layer->layerId == kNodeLayer,
           "the nearest Layer node is the entry's layer");
    expect(mergeEntriesOfLayer(shadowed.graph(), kNodeLayer).size() == 1,
           "the Layer node serves one entry");
    auto clean = makeComposition(viaLayerNode());
    expect(clean.graph().eraseLayerOutput(kEntryLayer), "the stray entry layer erases");
    for (const auto& binding : entryBindings(10))
        expect(clean.parameters().erase(binding.parameterId), "its parameter erases");
    expect(valid(clean), "without the entry layer the Layer node entry validates");
    // A Layer node may carry the Frame pivot it adopted.
    clean.graph().findLayer(kNodeLayer)->pivot = LayerPivot::Frame;
    expect(valid(clean), "a Layer node with the Frame pivot validates");
}

void erasingTheSourceRemovesTheSlotAndItsEntryLayer() {
    auto composition = makeComposition();
    expect(composition.graph().eraseNode(kSolid), "the source erases");
    expect(composition.graph().findLayer(kEntryLayer) == nullptr &&
               composition.graph().layerStack().find(kSlot) == nullptr,
           "the slot and its entry layer leave with the source");
}

void adjustmentTargetsTheEntryItsChainReaches() {
    auto composition = makeComposition();
    auto& graph = composition.graph();
    const auto adjustment = NodeId::fromRaw(9);
    const auto slot = LayerSlotId::fromRaw(2);
    const bool built =
        graph.addNode({adjustment, std::string(kAdjustmentNodeType), {},
                       kAdjustmentNodeSchemaVersion}) &&
        graph.addEdge({EdgeId::fromRaw(9),
                       {adjustment, std::string(kAdjustmentOutputPort)},
                       LayerStackInputRef{kMerge, slot, std::string(kLayerStackContentInputRole)}}) &&
        graph.layerStack().append({slot, {}}) && graph.layerStack().moveBefore(slot, kSlot);
    expect(built, "an Adjustment entry wires");
    expect(mergeEntryAdjustment(graph, kMerge, slot) == adjustment &&
               !mergeEntryAdjustment(graph, kMerge, kSlot),
           "the Adjustment serves the entry its chain lands in");
    expect(valid(composition), "a plain Adjustment entry validates");
}

} // namespace

int main() try {
    aWellFormedEntryLayerValidates();
    malformedEntryLayersAreRefused();
    aLayerNodeFeedingTheSlotShadowsTheEntryLayer();
    erasingTheSourceRemovesTheSlotAndItsEntryLayer();
    adjustmentTargetsTheEntryItsChainReaches();
    if (failures != 0) {
        std::cerr << failures << " entry layer document expectation(s) failed\n";
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "entry layer document test aborted: " << error.what() << '\n';
    return 1;
}
