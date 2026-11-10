// #642: the Vector Artwork layer's expansion. See svg_artwork.hpp.

#include <bloom/document/svg_artwork.hpp>

#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/media/svg_scene.hpp>

#include <algorithm>
#include <array>
#include <unordered_set>

namespace bloom::document {

std::shared_ptr<const media::SvgScene> parseVectorArtworkBlock(const DataBlockRecord& block,
                                                              std::string& diagnostic,
                                                              const std::function<bool()>& cancelled) {
    if (!isVectorArtworkBlock(block)) {
        diagnostic = "The data block is not an SVG";
        return nullptr;
    }
    const auto& payload = std::get<OpaqueExtensionPayload>(block.payload);
    if (payload.size() > media::kMaxSvgFileBytes) {
        diagnostic = "The SVG is larger than 32 MiB, the most an SVG import reads";
        return nullptr;
    }
    auto read = media::parseSvgScene(payload.bytes(), cancelled ? media::SvgCancel(cancelled) : media::SvgCancel{});
    if (!read.scene) {
        diagnostic = read.diagnostic.empty() ? std::string("The SVG could not be read") : read.diagnostic;
        return nullptr;
    }
    return read.scene;
}

namespace {

// The derivation node groups use (node_group_nodes.cpp): a mixed 64-bit hash with the top bit set,
// so a derived id never meets one the project allocator hands out.
[[nodiscard]] std::uint64_t mixIds(const std::uint64_t base, const std::uint64_t inner) noexcept {
    std::uint64_t value = base * 0x9E3779B97F4A7C15ULL + inner + 0x7F4A7C15ULL;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value | (std::uint64_t{1} << 63);
}

// Ids derived from the Vector Artwork node's own id, distinct from every id the composition holds.
class ExpansionTarget final : public SvgArtworkTarget {
  public:
    ExpansionTarget(Composition& work, const NodeId base, std::unordered_set<std::uint64_t>& nodes,
                    std::unordered_set<std::uint64_t>& edges, std::unordered_set<std::uint64_t>& parameters,
                    std::unordered_set<std::uint64_t>& slots, std::unordered_set<std::uint64_t>& layers)
        : work_(work), base_(base.value()), nodes_(nodes), edges_(edges), parameters_(parameters),
          slots_(slots), layers_(layers) {}

    std::optional<NodeId> allocateNode() override { return NodeId::fromRaw(derive(nodes_, 1)); }
    std::optional<ParameterId> allocateParameter() override { return ParameterId::fromRaw(derive(parameters_, 2)); }
    std::optional<LayerSlotId> allocateLayerSlot() override { return LayerSlotId::fromRaw(derive(slots_, 3)); }
    std::optional<EdgeId> allocateEdge() override { return EdgeId::fromRaw(derive(edges_, 4)); }
    // Node frames are layout; an expansion makes none.
    std::optional<NodeGroupId> allocateNodeGroup() override { return std::nullopt; }
    // The carrier's own layer id (composeLayerTransform).
    LayerId allocateLayer() { return LayerId::fromRaw(derive(layers_, 5)); }

    // The entry layer every new Merge entry gets (commands' createEntryLayer): a frame-pivot layer
    // with every entry role bound at the composition's defaults (Time Remap only when enabled).
    Entry createEntryLayer(const NodeId merge, const LayerSlotId slot) override {
        const auto layerId = LayerId::fromRaw(derive(layers_, 5));
        // Named for now; the writer names it after the element at once.
        LayerOutputBoundary boundary{merge, layerId, "Layer", std::string(kLayerStackOutputPort)};
        boundary.entrySlot = slot;
        boundary.pivot = LayerPivot::Frame;
        for (const auto& role : entryLayerRoles()) {
            if (role.role == kTimeRemapParameterRole)
                continue;
            const auto value = defaultValue(role.role);
            if (!value)
                return {std::nullopt, "Entry layer default violates its parameter schema"};
            const auto parameterId = ParameterId::fromRaw(derive(parameters_, 2));
            if (!work_.parameters().insert({parameterId, std::string(role.schemaKey), ConstantValueSource{*value}}))
                return {std::nullopt, "Entry layer default violates its parameter schema"};
            boundary.parameters.push_back({std::string(role.role), parameterId});
        }
        std::ranges::sort(boundary.parameters, {}, &ParameterBinding::role);
        auto& graph = work_.graph();
        auto* stack = graph.merge(merge);
        if (stack == nullptr || !graph.addLayerOutput(std::move(boundary)) || !stack->setLayerId(slot, layerId))
            return {std::nullopt, "Entry layer could not be inserted"};
        return {layerId, {}};
    }

  private:
    std::optional<ParameterValue> defaultValue(const std::string_view role) const {
        if (role == kPositionParameterRole)
            return Vec2d{static_cast<double>(work_.format().width()) / 2.0,
                         static_cast<double>(work_.format().height()) / 2.0};
        const auto* definition = builtInNodeDefinitions().find(kLayerOutputNodeType, kLayerOutputNodeSchemaVersion);
        if (definition == nullptr)
            return std::nullopt;
        for (const auto& parameter : definition->parameters)
            if (parameter.role == role)
                return parameter.defaultValue;
        return std::nullopt;
    }

    std::uint64_t derive(std::unordered_set<std::uint64_t>& used, const std::uint64_t space) {
        auto raw = mixIds(base_, (space << 56) ^ ++counter_);
        while (!used.insert(raw).second)
            raw = (raw + 1) | (std::uint64_t{1} << 63);
        return raw;
    }

    Composition& work_;
    std::uint64_t base_;
    std::uint64_t counter_ = 0;
    std::unordered_set<std::uint64_t>& nodes_;
    std::unordered_set<std::uint64_t>& edges_;
    std::unordered_set<std::uint64_t>& parameters_;
    std::unordered_set<std::uint64_t>& slots_;
    std::unordered_set<std::uint64_t>& layers_;
};

constexpr std::array<std::string_view, 4> kTransformRoles{kPositionParameterRole, kAnchorParameterRole,
                                                          kScaleParameterRole, kRotationParameterRole};

template <typename Value>
[[nodiscard]] const Value* constantOf(const ParameterStore& parameters, const ParameterBinding* binding) {
    const auto* record = binding ? parameters.find(binding->parameterId) : nullptr;
    const auto* constant = record ? std::get_if<ConstantValueSource>(&record->source) : nullptr;
    return constant ? std::get_if<Value>(&constant->value) : nullptr;
}

// A group's entry layer the artwork's geometry can be drawn through: a frame-pivot layer whose
// matrix is exactly the identity (the import pivots each group on its own centre that way), with
// no parent and no masks. Its opacity and track matte apply to its picture as they always do.
[[nodiscard]] bool passesGeometryThrough(const Composition& work, const LayerOutputBoundary& layer) {
    if (layer.pivot != LayerPivot::Frame || layer.parent || !layer.masks.empty() ||
        !layer.timing.isIdentityMapping())
        return false;
    const auto& graph = work.graph();
    const auto bound = [&](const std::string_view role) { return layerParameterBinding(graph, layer, role); };
    if (bound(kTimeRemapParameterRole) != nullptr)
        return false;
    const auto* position = constantOf<Vec2d>(work.parameters(), bound(kPositionParameterRole));
    const auto* anchor = constantOf<Vec2d>(work.parameters(), bound(kAnchorParameterRole));
    const auto* scale = constantOf<Vec2d>(work.parameters(), bound(kScaleParameterRole));
    const auto* rotation = constantOf<double>(work.parameters(), bound(kRotationParameterRole));
    const double cx = static_cast<double>(work.format().width()) / 2.0;
    const double cy = static_cast<double>(work.format().height()) / 2.0;
    return position && anchor && scale && rotation && position->x == cx + anchor->x &&
           position->y == cy + anchor->y && scale->x == 1.0 && scale->y == 1.0 && *rotation == 0.0;
}

// Composes the layer that reads the artwork's root through `reader` (see VectorArtworkComposedLayer).
// Returns why it is not composed, or nothing once it is.
[[nodiscard]] std::optional<std::string> composeLayerTransform(Composition& work, ExpansionTarget& target,
                                                               const std::vector<EdgeRecord>& readers,
                                                               const NodeId root,
                                                               const std::vector<NodeId>& artworkNodes,
                                                               VectorArtworkExpansion& result) {
    if (readers.size() != 1)
        return std::string(readers.empty() ? "nothing reads the artwork" : "more than one node reads the artwork");
    auto& graph = work.graph();
    const auto& reader = readers.front();
    LayerOutputBoundary* layer = nullptr;
    if (const auto* slot = std::get_if<LayerStackInputRef>(&reader.destination)) {
        const auto* stack = graph.merge(slot->stackNodeId);
        const auto* entry = stack ? stack->find(slot->slotId) : nullptr;
        if (slot->role == kLayerStackContentInputRole && entry != nullptr)
            layer = graph.findLayer(entry->layerId);
        if (layer != nullptr && (!layer->entryOwned() || layer->nodeId != slot->stackNodeId))
            layer = nullptr;
    } else if (const auto* input = std::get_if<NodeInputRef>(&reader.destination)) {
        const auto* node = graph.findNode(input->nodeId);
        if (node != nullptr && node->typeId == kLayerOutputNodeType && input->port == kLayerOutputContentInputPort)
            for (const auto& boundary : graph.layerOutputs())
                if (!boundary.entryOwned() && boundary.nodeId == node->id)
                    layer = graph.findLayer(boundary.layerId);
    }
    if (layer == nullptr)
        return std::string("the artwork is read through another node before its layer");
    if (layer->pivot != LayerPivot::Frame)
        return std::string("the layer pivots on its content rather than the frame");
    if (!layer->masks.empty())
        return std::string("the layer has masks, which are drawn in its content space");
    if (!layer->timing.isIdentityMapping() || layerParameterBinding(graph, *layer, kTimeRemapParameterRole) != nullptr)
        return std::string("the layer has its own timing");
    // Copied: the parameter store grows below.
    std::array<ParameterRecord, kTransformRoles.size()> transform{};
    for (std::size_t i = 0; i < kTransformRoles.size(); ++i) {
        const auto* binding = layerParameterBinding(graph, *layer, kTransformRoles[i]);
        const auto* record = binding ? work.parameters().find(binding->parameterId) : nullptr;
        if (record == nullptr)
            return std::string("the layer's transform is incomplete");
        transform[i] = *record;
    }

    // The carrier: a Layer node with nothing to draw, holding the layer's own transform parameters
    // (the same ids, so keys, drivers and a Viewer gesture's overrides move it) and its parent.
    const auto* definition = builtInNodeDefinitions().find(kLayerOutputNodeType, kLayerOutputNodeSchemaVersion);
    const auto carrierNode = target.allocateNode();
    if (definition == nullptr || !carrierNode)
        throw SvgArtworkWriteError("The artwork's transform carrier could not be made");
    NodeRecord carrier{*carrierNode, std::string(kLayerOutputNodeType), {}, kLayerOutputNodeSchemaVersion};
    const auto newParameter = [&](std::string schemaKey, ParameterValue value) {
        const auto id = target.allocateParameter();
        if (!id || !work.parameters().insert({*id, std::move(schemaKey), ConstantValueSource{std::move(value)}}))
            throw SvgArtworkWriteError("The artwork's transform carrier could not be made");
        return *id;
    };
    for (std::size_t i = 0; i < kTransformRoles.size(); ++i)
        carrier.parameters.push_back({std::string(kTransformRoles[i]), transform[i].id});
    for (const auto& parameter : definition->parameters) {
        if (std::ranges::find(kTransformRoles, parameter.role) != kTransformRoles.end() ||
            parameter.role == kTimeRemapParameterRole)
            continue;
        const auto* schema = layerParameterBinding(graph, *layer, parameter.role);
        const auto* record = schema ? work.parameters().find(schema->parameterId) : nullptr;
        if (record == nullptr)
            throw SvgArtworkWriteError("The artwork's transform carrier could not be made");
        auto schemaKey = record->schemaKey;
        carrier.parameters.push_back({parameter.role, newParameter(std::move(schemaKey), parameter.defaultValue)});
    }
    std::ranges::sort(carrier.parameters, {}, &ParameterBinding::role);
    LayerOutputBoundary carrierLayer{carrier.id, target.allocateLayer(), layer->name,
                                     std::string(kLayerOutputOutputPort)};
    carrierLayer.pivot = LayerPivot::Frame;
    carrierLayer.parent = layer->parent;
    const auto carrierId = carrierLayer.layerId;
    const auto layerId = layer->layerId;
    if (!graph.addNode(std::move(carrier)))
        throw SvgArtworkWriteError("The artwork's transform carrier could not be made");
    // addNode may move the boundaries; the layer is found again below.
    if (!graph.addLayerOutput(std::move(carrierLayer)))
        throw SvgArtworkWriteError("The artwork's transform carrier could not be made");

    // The layer itself draws at the identity: its frame centre, no anchor, unit scale, no turn.
    layer = graph.findLayer(layerId);
    auto& bindings = layer->entryOwned() ? layer->parameters : graph.findNode(layer->nodeId)->parameters;
    const Vec2d centre{static_cast<double>(work.format().width()) / 2.0,
                       static_cast<double>(work.format().height()) / 2.0};
    for (std::size_t i = 0; i < kTransformRoles.size(); ++i) {
        ParameterValue identity = i == 0   ? ParameterValue{centre}
                                  : i == 1 ? ParameterValue{Vec2d{0.0, 0.0}}
                                  : i == 2 ? ParameterValue{Vec2d{1.0, 1.0}}
                                           : ParameterValue{0.0};
        const auto id = newParameter(transform[i].schemaKey, std::move(identity));
        for (auto& binding : bindings)
            if (binding.role == kTransformRoles[i])
                binding.parameterId = id;
    }
    layer->parent.reset();

    // Whatever was parented to the layer follows the carrier, which moves as the layer did.
    std::vector<LayerId> children;
    for (const auto& boundary : graph.layerOutputs())
        if (boundary.parent == layerId)
            children.push_back(boundary.layerId);
    for (const auto child : children)
        graph.findLayer(child)->parent = carrierId;

    // Every drawn layer of the artwork, through its groups whose geometry passes through.
    VectorArtworkComposedLayer composed{carrierId, {}};
    const std::unordered_set<NodeId> ours(artworkNodes.begin(), artworkNodes.end());
    std::vector<NodeId> pending{root};
    while (!pending.empty()) {
        const auto merge = pending.back();
        pending.pop_back();
        const auto* stack = graph.merge(merge);
        if (stack == nullptr)
            continue;
        for (const auto& entry : stack->entries()) {
            auto* entryLayer = graph.findLayer(entry.layerId);
            if (entryLayer == nullptr)
                continue;
            const auto* edge = mergeSlotImageEdge(graph, merge, entry.slotId);
            const auto* source = edge ? graph.findNode(edge->source.nodeId) : nullptr;
            if (source != nullptr && source->typeId == kLayerStackNodeType && ours.contains(source->id) &&
                passesGeometryThrough(work, *entryLayer)) {
                pending.push_back(source->id);
                continue;
            }
            if (entryLayer->parent)
                throw SvgArtworkWriteError("An artwork layer already has a parent");
            entryLayer->parent = carrierId;
            composed.leaves.push_back(entry.layerId);
        }
    }
    result.composed.emplace(layerId, std::move(composed));
    return std::nullopt;
}

} // namespace

VectorArtworkExpansion expandVectorArtwork(const Project& project, const Composition& composition,
                                           const VectorArtworkExpansionContext& context) {
    VectorArtworkExpansion result;
    std::vector<NodeId> artwork;
    for (const auto& node : composition.graph().nodes())
        if (node.typeId == kVectorArtworkNodeType)
            artwork.push_back(node.id);
    if (artwork.empty())
        return result;

    Composition work(composition);
    std::unordered_set<std::uint64_t> nodes, edges, parameters, slots, layers;
    for (const auto& node : work.graph().nodes())
        nodes.insert(node.id.value());
    for (const auto& edge : work.graph().edges())
        edges.insert(edge.id.value());
    for (const auto& record : work.parameters().records())
        parameters.insert(record.id.value());
    for (const auto& stack : work.graph().merges())
        for (const auto& entry : stack.entries())
            slots.insert(entry.slotId.value());
    for (const auto& boundary : work.graph().layerOutputs())
        layers.insert(boundary.layerId.value());

    auto fonts = context.fonts;
    for (const auto id : artwork) {
        if (context.cancelled && context.cancelled()) {
            result.cancelled = true;
            return result;
        }
        const auto* node = work.graph().findNode(id);
        if (node == nullptr)
            continue;
        const NodeRecord source = *node;
        std::shared_ptr<const media::SvgScene> scene;
        if (vectorArtworkBlockId(source, work.parameters())) {
            const auto* block = vectorArtworkBlock(project, source, work.parameters());
            if (block == nullptr) {
                result.unresolved.emplace(id, "The SVG this layer holds is missing from the project");
                continue;
            }
            std::string diagnostic;
            scene = context.scene ? context.scene(*block) : parseVectorArtworkBlock(*block, diagnostic, context.cancelled);
            if (context.cancelled && context.cancelled()) {
                result.cancelled = true;
                return result;
            }
            if (!scene) {
                result.unresolved.emplace(id, diagnostic.empty() ? std::string("The SVG this layer holds could not be read")
                                                                 : diagnostic);
                continue;
            }
        }
        // An empty node (no SVG named) is an empty Merge: it draws nothing.
        SvgArtworkDescription description;
        if (scene) {
            SvgArtworkOptions options;
            options.width = work.format().width();
            options.height = work.format().height();
            options.fit = vectorArtworkFits(source, work.parameters());
            options.fonts = fonts;
            description = buildSvgArtwork(*scene, options);
        } else {
            description.nodes.push_back({std::string(kLayerStackNodeType), {}, {}, true});
        }
        try {
            ExpansionTarget target(work, id, nodes, edges, parameters, slots, layers);
            SvgArtworkWriter writer(work, target, description, false);
            const auto root = writer.addRoot();
            writer.writeSteps();
            // Every reader of the node's image now reads the artwork's root.
            std::vector<EdgeRecord> readers;
            for (const auto& edge : work.graph().edges())
                if (edge.source.nodeId == id)
                    readers.push_back(edge);
            for (auto edge : readers) {
                (void)work.graph().eraseEdge(edge.id);
                edge.source = {root, std::string(kLayerStackOutputPort)};
                if (!work.graph().addEdge(std::move(edge)))
                    throw SvgArtworkWriteError("The artwork could not be connected where the layer reads it");
            }
            for (const auto& binding : source.parameters)
                (void)work.parameters().erase(binding.parameterId);
            (void)work.graph().eraseNode(id);
            bool muted = false;
            if (const auto layout = work.nodeLayout().find(id); layout != work.nodeLayout().end()) {
                // A muted Vector Artwork node is a muted root.
                muted = layout->second.muted;
                if (muted)
                    work.nodeLayout()[root].muted = true;
                work.nodeLayout().erase(id);
            }
            if (!context.composeLayerTransform)
                result.resampled.emplace(id, "the expansion was asked not to compose");
            else if (muted)
                result.resampled.emplace(id, "the artwork is muted");
            else if (auto why = composeLayerTransform(work, target, readers, root, writer.nodeIds(), result))
                result.resampled.emplace(id, std::move(*why));
            result.roots.emplace(id, root);
            result.notes.emplace(id, std::move(description.notes));
        } catch (const SvgArtworkWriteError& failure) {
            // The composition is half written: nothing of it is used.
            result.roots.clear();
            result.notes.clear();
            result.composed.clear();
            result.resampled.clear();
            for (const auto other : artwork)
                result.unresolved.emplace(other, failure.what());
            return result;
        }
    }
    result.composition = std::move(work);
    return result;
}

} // namespace bloom::document
