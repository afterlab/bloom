// #642: the Vector Artwork layer's expansion. See svg_artwork.hpp.

#include <bloom/document/svg_artwork.hpp>

#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/evaluation_inputs.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/media/svg_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>
#include <variant>

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
                    std::unordered_set<std::uint64_t>& slots, std::unordered_set<std::uint64_t>& layers,
                    std::unordered_set<std::uint64_t>& curves)
        : work_(work), base_(base.value()), nodes_(nodes), edges_(edges), parameters_(parameters),
          slots_(slots), layers_(layers), curves_(curves) {}

    std::optional<NodeId> allocateNode() override { return NodeId::fromRaw(derive(nodes_, 1)); }
    std::optional<ParameterId> allocateParameter() override { return ParameterId::fromRaw(derive(parameters_, 2)); }
    std::optional<LayerSlotId> allocateLayerSlot() override { return LayerSlotId::fromRaw(derive(slots_, 3)); }
    std::optional<EdgeId> allocateEdge() override { return EdgeId::fromRaw(derive(edges_, 4)); }
    // Node frames are layout; an expansion makes none.
    std::optional<NodeGroupId> allocateNodeGroup() override { return std::nullopt; }
    // The carrier's own layer id (composeLayerTransform).
    LayerId allocateLayer() { return LayerId::fromRaw(derive(layers_, 5)); }
    // A scaled copy of a curve (oversampleLayer).
    AnimationCurveId allocateCurve() { return AnimationCurveId::fromRaw(derive(curves_, 6)); }

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
    std::unordered_set<std::uint64_t>& curves_;
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

// The layer that reads the artwork's root and the nodes between them, artwork side first.
struct ArtworkReader final {
    LayerOutputBoundary* layer = nullptr;
    std::vector<NodeId> between;
};

// #642: the nodes that may sit between the artwork and its layer when the artwork is drawn at a
// larger scale (VectorArtworkOversampledLayer), each with its lengths in composition pixels, which
// are scaled with the picture as a scaled pre-composition scales them. The blurs, Glow and Drop
// Shadow are the separable blur, Directional Blur, Transform 2D and pointwise primitives over their
// one image; the colour nodes have no length. Any other node keeps the resampled picture, named.
struct ScaledBetweenNode final {
    std::string_view type;
    std::array<std::string_view, 2> lengths;
};
constexpr std::array<ScaledBetweenNode, 13> kScaledBetweenNodes{{
    {"bloom.gaussian-blur", {"radius", "radiusY"}},
    {"bloom.box-blur", {"radius", {}}},
    {kGlowNodeType, {"size", "length"}},
    {kDropShadowNodeType, {"distance", "softness"}},
    {"bloom.levels", {}},
    {"bloom.curves", {}},
    {"bloom.hue-saturation", {}},
    {"bloom.grade", {}},
    {"bloom.colorize", {}},
    {"bloom.tone-map", {}},
    {"bloom.color-matrix", {}},
    {"bloom.alpha-association", {}},
    {"bloom.reroute", {}},
}};

[[nodiscard]] const ScaledBetweenNode* scaledBetweenNode(const std::string_view type) noexcept {
    const auto found = std::ranges::find(kScaledBetweenNodes, type, &ScaledBetweenNode::type);
    return found == kScaledBetweenNodes.end() ? nullptr : &*found;
}

[[nodiscard]] bool readsPixels(const SocketValueKind kind) noexcept {
    return kind == SocketValueKind::Image || kind == SocketValueKind::Mask || kind == SocketValueKind::ChannelSet ||
           kind == SocketValueKind::ScalarField || kind == SocketValueKind::Vector2Field ||
           kind == SocketValueKind::ColorField;
}

// Why the artwork cannot be drawn larger through `node` (read at `port`, `where` it sits), or
// nothing.
[[nodiscard]] std::optional<std::string> betweenNodeRefusal(const Composition& work, const NodeRecord& node,
                                                            const std::string_view port,
                                                            const std::string_view where = "between the artwork and its layer") {
    const auto& graph = work.graph();
    const auto* definition = builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
    const std::string name = "the " + node.typeId + " node " + std::string(where);
    if (definition == nullptr || definition->inputs.empty() || definition->inputs.front().name != port ||
        definition->inputs.front().valueKind != SocketValueKind::Image)
        return name + " reads it through another input than its image";
    // A muted node passes its first image through untouched (layer-graph-model.md).
    if (nodeMuted(work.nodeLayout(), node.id))
        return std::nullopt;
    const auto* scaled = scaledBetweenNode(node.typeId);
    if (scaled == nullptr)
        return name + " is not known to draw the same at a larger scale";
    for (const auto& edge : graph.edges()) {
        const auto* input = std::get_if<NodeInputRef>(&edge.destination);
        if (input == nullptr || input->nodeId != node.id || input->port == port)
            continue;
        const auto declared = findInputPort(*definition, input->port);
        if (declared && readsPixels(declared->valueKind))
            return name + " reads another image (" + input->port + ")";
        if (std::ranges::find(scaled->lengths, std::string_view(input->port)) != scaled->lengths.end())
            return name + " has its " + input->port + " driven by the graph";
    }
    for (const auto& binding : node.parameters) {
        if (std::ranges::find(scaled->lengths, std::string_view(binding.role)) == scaled->lengths.end())
            continue;
        const auto* record = work.parameters().find(binding.parameterId);
        if (record != nullptr && std::holds_alternative<DriverBindingSource>(record->source))
            return name + " has its " + binding.role + " driven by the graph";
    }
    return std::nullopt;
}

// The layer reading the artwork's root (through `readers`, the edges that read it), walking down
// through the nodes between, or why there is none that can be drawn sharp.
[[nodiscard]] std::variant<ArtworkReader, std::string> readerOf(Composition& work, std::vector<EdgeRecord> readers) {
    auto& graph = work.graph();
    ArtworkReader result;
    for (std::size_t step = 0; step <= graph.nodes().size(); ++step) {
        if (readers.size() != 1)
            return std::string(readers.empty() ? "nothing reads the artwork" : "more than one node reads the artwork");
        const auto& reader = readers.front();
        if (const auto* slot = std::get_if<LayerStackInputRef>(&reader.destination)) {
            const auto* stack = graph.merge(slot->stackNodeId);
            const auto* entry = stack ? stack->find(slot->slotId) : nullptr;
            if (slot->role == kLayerStackContentInputRole && entry != nullptr)
                result.layer = graph.findLayer(entry->layerId);
            if (result.layer != nullptr && (!result.layer->entryOwned() || result.layer->nodeId != slot->stackNodeId))
                result.layer = nullptr;
            if (result.layer == nullptr)
                return std::string("the artwork is read by a Merge entry without a layer");
            return result;
        }
        const auto* input = std::get_if<NodeInputRef>(&reader.destination);
        const auto* node = input ? graph.findNode(input->nodeId) : nullptr;
        if (node == nullptr)
            return std::string("the artwork is read through another node before its layer");
        if (node->typeId == kLayerOutputNodeType) {
            if (input->port != kLayerOutputContentInputPort)
                return std::string("the artwork is read through another node before its layer");
            for (const auto& boundary : graph.layerOutputs())
                if (!boundary.entryOwned() && boundary.nodeId == node->id)
                    result.layer = graph.findLayer(boundary.layerId);
            if (result.layer == nullptr)
                return std::string("the artwork is read through another node before its layer");
            return result;
        }
        if (auto refusal = betweenNodeRefusal(work, *node, input->port))
            return std::move(*refusal);
        result.between.push_back(node->id);
        readers.clear();
        for (const auto& edge : graph.edges())
            if (edge.source.nodeId == node->id)
                readers.push_back(edge);
    }
    return std::string("the artwork is read through a cycle");
}

// Every drawn layer of the artwork, through its groups whose geometry passes through, parented to
// `carrier`.
[[nodiscard]] std::vector<LayerId> parentLeaves(Composition& work, const NodeId root,
                                                const std::vector<NodeId>& artworkNodes, const LayerId carrier) {
    auto& graph = work.graph();
    std::vector<LayerId> leaves;
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
            entryLayer->parent = carrier;
            leaves.push_back(entry.layerId);
        }
    }
    return leaves;
}

// Composes `layer`, which reads the artwork's root with nothing between (VectorArtworkComposedLayer).
// Returns why it is not composed, or nothing once it is.
[[nodiscard]] std::optional<std::string> composeLayerTransform(Composition& work, ExpansionTarget& target,
                                                               LayerOutputBoundary* layer, const NodeId root,
                                                               const std::vector<NodeId>& artworkNodes,
                                                               VectorArtworkExpansion& result) {
    auto& graph = work.graph();
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

    result.composed.emplace(layerId, VectorArtworkComposedLayer{carrierId, parentLeaves(work, root, artworkNodes, carrierId)});
    return std::nullopt;
}

// ---- Oversampling (VectorArtworkOversampledLayer) ----

using OverrideLookup = std::function<std::optional<ParameterValue>(ParameterId)>;

// The source a parameter has for this compile: a preview's override, else its own.
[[nodiscard]] ParameterSource sourceOf(const ParameterRecord& record, const OverrideLookup& override) {
    if (override)
        if (auto value = override(record.id))
            return ConstantValueSource{std::move(*value)};
    return record.source;
}

// The largest magnitude a component's keys reach, at and between them: a hold or linear segment
// stays within its keys, an eased one within its keys and their handles, and a Back or Elastic one
// is allowed its overshoot, counted generously (the span again times its back or amplitude). Never
// below the true largest value, so the band it picks is never too small.
[[nodiscard]] double keyedBound(const std::vector<ScalarKeyframe>& keys, const double fallback) {
    if (keys.empty())
        return std::abs(fallback);
    double bound = 0.0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        bound = std::max(bound, std::abs(key.value));
        if (i + 1 == keys.size())
            break;
        const auto& next = keys[i + 1];
        const auto kind = key.outgoingInterpolation;
        if (kind == KeyframeInterpolation::Hold || kind == KeyframeInterpolation::Linear)
            continue;
        bound = std::max({bound, std::abs(key.value + key.outgoingHandle.value),
                          std::abs(next.value + next.incomingHandle.value)});
        if (kind == KeyframeInterpolation::Back || kind == KeyframeInterpolation::Elastic)
            bound = std::max(bound, std::max(std::abs(key.value), std::abs(next.value)) +
                                        std::abs(next.value - key.value) *
                                            std::max({1.0, std::abs(key.easing.back), std::abs(key.easing.amplitude)}));
    }
    return bound;
}

// The parameter `role` of `layer` (null when it has none).
[[nodiscard]] const ParameterRecord* layerRecord(const Composition& work, const LayerOutputBoundary& layer,
                                                 const std::string_view role) {
    const auto* binding = layerParameterBinding(work.graph(), layer, role);
    return binding ? work.parameters().find(binding->parameterId) : nullptr;
}

// The largest |scale| on either axis `layer` reaches over its animation; nothing when the graph
// drives it (its range is not known when compiling).
[[nodiscard]] std::optional<double> scaleBound(const Composition& work, const LayerOutputBoundary& layer,
                                               const OverrideLookup& override) {
    const auto* record = layerRecord(work, layer, kScaleParameterRole);
    if (record == nullptr)
        return 1.0;
    const auto source = sourceOf(*record, override);
    if (const auto* constant = std::get_if<ConstantValueSource>(&source)) {
        const auto* value = std::get_if<Vec2d>(&constant->value);
        return value ? std::optional{std::max(std::abs(value->x), std::abs(value->y))} : std::nullopt;
    }
    if (const auto* animated = std::get_if<AnimationCurveSource>(&source)) {
        const auto* curve = work.animationCurves().findVec2(animated->curveId);
        if (curve == nullptr)
            return std::nullopt;
        Vec2d fallback{1.0, 1.0};
        if (animated->defaultValue)
            if (const auto* value = std::get_if<Vec2d>(&*animated->defaultValue))
                fallback = *value;
        return std::max(keyedBound(curve->components[0].keyframes, fallback.x),
                        keyedBound(curve->components[1].keyframes, fallback.y));
    }
    return std::nullopt;
}

// Whether `layer` is turned at any time (its rotation is not zero throughout).
[[nodiscard]] bool turns(const Composition& work, const LayerOutputBoundary& layer, const OverrideLookup& override) {
    const auto* record = layerRecord(work, layer, kRotationParameterRole);
    if (record == nullptr)
        return false;
    const auto source = sourceOf(*record, override);
    if (const auto* constant = std::get_if<ConstantValueSource>(&source)) {
        const auto* value = std::get_if<double>(&constant->value);
        return value == nullptr || *value != 0.0;
    }
    if (const auto* animated = std::get_if<AnimationCurveSource>(&source)) {
        const auto* curve = work.animationCurves().findScalar(animated->curveId);
        return curve == nullptr || keyedBound(curve->keyframes, 0.0) != 0.0;
    }
    return true;
}

[[nodiscard]] PathValue scaledPath(PathValue path, const double factor) {
    for (auto& contour : path.contours)
        for (auto& anchor : contour.anchors) {
            anchor.point = {anchor.point.x * factor, anchor.point.y * factor};
            if (anchor.inHandle)
                anchor.inHandle = Vec2d{anchor.inHandle->x * factor, anchor.inHandle->y * factor};
            if (anchor.outHandle)
                anchor.outHandle = Vec2d{anchor.outHandle->x * factor, anchor.outHandle->y * factor};
        }
    return path;
}

[[nodiscard]] std::optional<ParameterValue> scaledValue(const ParameterValue& value, const double factor) {
    if (const auto* number = std::get_if<double>(&value))
        return ParameterValue{*number * factor};
    if (const auto* point = std::get_if<Vec2d>(&value))
        return ParameterValue{Vec2d{point->x * factor, point->y * factor}};
    if (const auto* path = std::get_if<PathValue>(&value))
        return ParameterValue{scaledPath(*path, factor)};
    return std::nullopt;
}

// A length parameter multiplied by `factor`: its new source and, for a keyed one, the scaled copy
// of its curve (whose id is given when it is written). Nothing when it cannot be: driven, of a kind
// that is not a length, or scaled past its schema's domain.
struct ScaledParameter final {
    ParameterId id;
    ParameterSource source;
    std::optional<AnimationCurveRecord> curve;
};
[[nodiscard]] std::optional<ScaledParameter> scaledParameter(const Composition& work, const ParameterRecord& record,
                                                             const double factor) {
    if (const auto* constant = std::get_if<ConstantValueSource>(&record.source)) {
        auto value = scaledValue(constant->value, factor);
        if (!value)
            return std::nullopt;
        ParameterRecord probe{record.id, record.schemaKey, ConstantValueSource{*value}};
        ParameterStore domain;
        if (!domain.insert(probe))
            return std::nullopt;
        return ScaledParameter{record.id, std::move(probe.source), std::nullopt};
    }
    const auto* animated = std::get_if<AnimationCurveSource>(&record.source);
    const auto* curve = animated ? work.animationCurves().find(animated->curveId) : nullptr;
    if (curve == nullptr)
        return std::nullopt;
    AnimationCurveSource source = *animated;
    if (source.defaultValue) {
        auto value = scaledValue(*source.defaultValue, factor);
        if (!value)
            return std::nullopt;
        source.defaultValue = std::move(*value);
    }
    if (const auto* scalar = std::get_if<ScalarAnimationCurve>(curve)) {
        auto copy = *scalar;
        for (auto& key : copy.keyframes) {
            key.value *= factor;
            key.outgoingHandle.value *= factor;
            key.incomingHandle.value *= factor;
            if (!std::isfinite(key.value) || !isScalarWithinSchemaDomain(record.schemaKey, key.value))
                return std::nullopt;
        }
        return ScaledParameter{record.id, std::move(source), AnimationCurveRecord{std::move(copy)}};
    }
    if (const auto* path = std::get_if<PathAnimationCurve>(curve)) {
        auto copy = *path;
        for (auto& key : copy.keyframes)
            key.value = scaledPath(std::move(key.value), factor);
        return ScaledParameter{record.id, std::move(source), AnimationCurveRecord{std::move(copy)}};
    }
    return std::nullopt;
}

// The lengths of `nodes` (nodes allowed between) in composition pixels, by parameter.
[[nodiscard]] std::vector<ParameterId> nodeLengths(const Composition& work, const std::vector<NodeId>& nodes) {
    std::vector<ParameterId> lengths;
    for (const auto id : nodes) {
        const auto* node = work.graph().findNode(id);
        if (node == nullptr || nodeMuted(work.nodeLayout(), id))
            continue;
        const auto* scaled = scaledBetweenNode(node->typeId);
        for (const auto& binding : node->parameters)
            if (scaled != nullptr &&
                std::ranges::find(scaled->lengths, std::string_view(binding.role)) != scaled->lengths.end())
                lengths.push_back(binding.parameterId);
    }
    return lengths;
}

// The lengths an oversampled layer's expansion multiplies: those of the nodes between and its masks.
[[nodiscard]] std::variant<std::vector<ParameterId>, std::string>
scaledLengths(const Composition& work, const LayerOutputBoundary& layer, const std::vector<NodeId>& between) {
    auto lengths = nodeLengths(work, between);
    for (const auto& mask : layer.masks) {
        if (mask.matte)
            return std::string("the layer has a mask shaped by a node's Mask, which is drawn at the composition's "
                               "scale");
        lengths.push_back(mask.path);
        lengths.push_back(mask.feather);
        lengths.push_back(mask.expansion);
    }
    return lengths;
}

// Every length of `lengths` multiplied by `factor`, or nothing (with `limited` naming the first
// that would leave its range).
[[nodiscard]] std::optional<std::vector<ScaledParameter>> scaledAll(const Composition& work,
                                                                    const std::vector<ParameterId>& lengths,
                                                                    const double factor, std::string& limited) {
    std::vector<ScaledParameter> scaled;
    for (const auto id : lengths) {
        const auto* record = work.parameters().find(id);
        auto next = record ? scaledParameter(work, *record, factor) : std::nullopt;
        if (!next) {
            if (record != nullptr)
                limited = "the " + record->schemaKey + " length would leave its range at a larger scale";
            return std::nullopt;
        }
        scaled.push_back(std::move(*next));
    }
    return scaled;
}

// Writes scaled lengths into the expanded composition (a keyed one as a new curve).
void writeScaled(Composition& work, ExpansionTarget& target, std::vector<ScaledParameter> scaled, const double factor,
                 VectorArtworkExpansion& result) {
    for (auto& parameter : scaled) {
        if (parameter.curve) {
            const auto curveId = target.allocateCurve();
            std::visit([&](auto& curve) { curve.id = curveId; }, *parameter.curve);
            if (!work.animationCurves().insert(std::move(*parameter.curve)))
                throw SvgArtworkWriteError("A scaled curve of the artwork's layer could not be written");
            std::get<AnimationCurveSource>(parameter.source).curveId = curveId;
        }
        if (!work.parameters().setSource(parameter.id, std::move(parameter.source)))
            throw SvgArtworkWriteError("A scaled length of the artwork's layer could not be written");
        result.scaledParameters[parameter.id] = factor;
    }
}

// ---- The window an oversampled picture draws (kVectorArtworkMaxOversampledPixels) ----

// The smallest |value| a component's keys reach, at and between them (zero when they cross it),
// with the same allowances keyedBound makes for handles and overshoot. Never above the true
// smallest value.
[[nodiscard]] double keyedFloor(const std::vector<ScalarKeyframe>& keys, const double fallback) {
    if (keys.empty())
        return std::abs(fallback);
    double low = keys.front().value;
    double high = low;
    const auto add = [&](const double value) {
        low = std::min(low, value);
        high = std::max(high, value);
    };
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        add(key.value);
        if (i + 1 == keys.size())
            break;
        const auto& next = keys[i + 1];
        const auto kind = key.outgoingInterpolation;
        if (kind == KeyframeInterpolation::Hold || kind == KeyframeInterpolation::Linear)
            continue;
        add(key.value + key.outgoingHandle.value);
        add(next.value + next.incomingHandle.value);
        if (kind == KeyframeInterpolation::Back || kind == KeyframeInterpolation::Elastic) {
            const double span = std::abs(next.value - key.value) *
                                std::max({1.0, std::abs(key.easing.back), std::abs(key.easing.amplitude)});
            add(std::min(key.value, next.value) - span);
            add(std::max(key.value, next.value) + span);
        }
    }
    if (low <= 0.0 && high >= 0.0)
        return 0.0;
    return std::min(std::abs(low), std::abs(high));
}

// The largest reach, in composition pixels, the lengths of `nodes` give a picture beyond its
// content (a blur's radius, a glow's size and streak, a shadow's distance and softness) over the
// animation.
[[nodiscard]] double nodeMargin(const Composition& work, const std::vector<NodeId>& nodes) {
    double margin = 0.0;
    for (const auto id : nodeLengths(work, nodes)) {
        const auto* record = work.parameters().find(id);
        if (record == nullptr)
            continue;
        if (const auto* constant = std::get_if<ConstantValueSource>(&record->source)) {
            if (const auto* number = std::get_if<double>(&constant->value))
                margin += std::abs(*number);
            continue;
        }
        if (const auto* animated = std::get_if<AnimationCurveSource>(&record->source))
            if (const auto* curve = work.animationCurves().findScalar(animated->curveId)) {
                double fallback = 0.0;
                if (animated->defaultValue)
                    if (const auto* value = std::get_if<double>(&*animated->defaultValue))
                        fallback = *value;
                margin += keyedBound(curve->keyframes, fallback);
            }
    }
    return margin;
}

// Whether the window clip (runtime::detail::visibleSupportWindows) reaches through `node`. Every
// node allowed between does (the blurs, Glow's keys, octaves and streaks, Drop Shadow's move, the
// colour nodes), except a blur that repeats its picture's edge (Clamp), whose edge is its window's.
[[nodiscard]] bool clipsThrough(const Composition& work, const NodeId id) {
    const auto* node = work.graph().findNode(id);
    if (node == nullptr)
        return false;
    if (nodeMuted(work.nodeLayout(), id))
        return true;
    if (scaledBetweenNode(node->typeId) == nullptr)
        return false;
    if (node->typeId == "bloom.gaussian-blur" || node->typeId == "bloom.box-blur") {
        const auto binding = std::ranges::find(node->parameters, std::string_view("edge"), &ParameterBinding::role);
        const auto* edge =
            binding == node->parameters.end() ? nullptr : constantOf<std::int64_t>(work.parameters(), &*binding);
        return edge != nullptr && *edge == 0;
    }
    return true;
}

[[nodiscard]] bool allClipThrough(const Composition& work, const std::vector<NodeId>& nodes) {
    return std::ranges::all_of(nodes, [&](const NodeId id) { return clipsThrough(work, id); });
}

// The largest number of full-resolution pixels an oversampled picture's drawn window holds over the
// animation, estimated when compiling: the picture is `factor` times its content (`contentArea`
// composition pixels at a scale of 1, plus `margin` on every side). Where the window is clipped to
// what reaches the frame (`clipped`), it is also at most the frame's preimage through the smallest
// scale the layers above reach (`smallest`; any turn widens the preimage's box, at worst to
// (width + height)^2 / 2 at 45 degrees).
[[nodiscard]] double estimatedOversampledPixels(const double factor, const double contentArea, const double width,
                                                const double height, const double smallest, const bool turns,
                                                const bool clipped, const double margin) {
    double area = contentArea;
    if (clipped && smallest > 0.0) {
        const double frame = turns ? (width + height) * (width + height) / 2.0 : width * height;
        area = std::min(area, frame / (smallest * smallest));
    }
    const double side = std::sqrt(std::max(area, 0.0)) + 2.0 * margin;
    return factor * factor * side * side;
}

// Halves `factor` while the window estimate is past the budget, naming it in `limited`.
[[nodiscard]] double withinPixelBudget(double factor, const std::function<double(double)>& estimate,
                                       std::string& limited) {
    while (factor > 1.0) {
        const double pixels = estimate(factor);
        if (pixels <= kVectorArtworkMaxOversampledPixels)
            break;
        limited = "drawn at " + std::to_string(static_cast<long long>(factor / 2.0)) + "x: its window at " +
                  std::to_string(static_cast<long long>(factor)) + "x is estimated at " +
                  std::to_string(static_cast<long long>(pixels)) +
                  " pixels over the animation, past the pixel budget of one oversampled picture (8192 x 4096)";
        factor /= 2.0;
    }
    return factor;
}

// ---- The layers and groups above an artwork's layer (VectorArtworkOversampledGroup) ----

// The largest and smallest output scale a chain of layers reaches over its animation, whether any
// of them turns, and whether the window can be clipped through all of them: each pivots on the
// frame, or (#642 B4) is a content pivot above the oversampled picture whose input's bounds the
// runtime works out before the frame is drawn (contentPivotClips). Any other content pivot's
// matrix follows its content's bounds, so nothing beneath it can be clipped to the frame.
struct Span final {
    double largest = 1.0;
    double smallest = 1.0;
    bool turns = false;
    bool framePivots = true;
    Span& operator*=(const Span& other) {
        largest *= other.largest;
        smallest *= other.smallest;
        turns = turns || other.turns;
        framePivots = framePivots && other.framePivots;
        return *this;
    }
};

// The smallest |scale| on either axis `layer` reaches over its animation; nothing when the graph
// drives it.
[[nodiscard]] std::optional<double> scaleFloor(const Composition& work, const LayerOutputBoundary& layer,
                                               const OverrideLookup& override) {
    const auto* record = layerRecord(work, layer, kScaleParameterRole);
    if (record == nullptr)
        return 1.0;
    const auto source = sourceOf(*record, override);
    if (const auto* constant = std::get_if<ConstantValueSource>(&source)) {
        const auto* value = std::get_if<Vec2d>(&constant->value);
        return value ? std::optional{std::min(std::abs(value->x), std::abs(value->y))} : std::nullopt;
    }
    if (const auto* animated = std::get_if<AnimationCurveSource>(&source)) {
        const auto* curve = work.animationCurves().findVec2(animated->curveId);
        if (curve == nullptr)
            return std::nullopt;
        Vec2d fallback{1.0, 1.0};
        if (animated->defaultValue)
            if (const auto* value = std::get_if<Vec2d>(&*animated->defaultValue))
                fallback = *value;
        return std::min(keyedFloor(curve->components[0].keyframes, fallback.x),
                        keyedFloor(curve->components[1].keyframes, fallback.y));
    }
    return std::nullopt;
}

// `layer` and its parents; nothing when a scale is driven by the graph. `contentPivotClips`: a
// content pivot on `first` (not on its parents) still clips (#642 B4).
[[nodiscard]] std::optional<Span> chainSpan(const Composition& work, const LayerOutputBoundary& first,
                                            const OverrideLookup& override, const bool contentPivotClips = false) {
    Span span;
    std::unordered_set<LayerId> seen;
    for (const auto* layer = &first; layer != nullptr;
         layer = layer->parent ? work.graph().findLayer(*layer->parent) : nullptr) {
        if (!seen.insert(layer->layerId).second)
            break;
        const auto largest = scaleBound(work, *layer, override);
        const auto smallest = scaleFloor(work, *layer, override);
        if (!largest || !smallest)
            return std::nullopt;
        const bool clips = layer->pivot == LayerPivot::Frame || (contentPivotClips && layer == &first);
        span *= Span{*largest, *smallest, turns(work, *layer, override), clips};
    }
    return span;
}

// How a picture reaches the next Merge: the nodes between it and the first layer that resamples it
// (in the picture's own space), the layers resampling it, each reading the one before (one
// resample), the nodes between the last of them and the Merge slot (in that Merge's space), and the
// slot; no slot when it reaches the composition's output.
struct Hop final {
    std::vector<NodeId> before;
    std::vector<LayerId> layers;
    std::vector<NodeId> after;
    std::optional<std::pair<NodeId, LayerSlotId>> slot;
};

[[nodiscard]] std::vector<EdgeRecord> imageReaders(const CanonicalGraph& graph, const NodeId node) {
    std::vector<EdgeRecord> readers;
    for (const auto& edge : graph.edges())
        if (edge.source.nodeId == node && edge.source.port == kLayerStackOutputPort)
            readers.push_back(edge);
    return readers;
}

// Walks the picture of `from` (a Merge, or the last layer of `hop.layers`) one reader at a time.
[[nodiscard]] std::variant<Hop, std::string> hopFrom(const Composition& work, NodeId from, Hop hop) {
    const auto& graph = work.graph();
    enum class Phase { Before, Layers, After };
    auto phase = hop.layers.empty() ? Phase::Before : Phase::Layers;
    for (std::size_t step = 0; step <= graph.nodes().size(); ++step) {
        const auto readers = imageReaders(graph, from);
        if (readers.size() != 1)
            return std::string(readers.empty() ? "nothing reads its picture" : "more than one node reads its picture");
        const auto& reader = readers.front();
        if (const auto* slot = std::get_if<LayerStackInputRef>(&reader.destination)) {
            if (slot->role != kLayerStackContentInputRole)
                return std::string("its picture is read as a Merge input other than an entry's image");
            hop.slot = std::pair{slot->stackNodeId, slot->slotId};
            // With no Layer node on the way, the entry's own layer resamples it (none yet: the
            // identity, made when the group is drawn larger).
            if (hop.layers.empty())
                if (const auto* own = mergeEntryLayer(graph, slot->stackNodeId, slot->slotId))
                    hop.layers.push_back(own->layerId);
            return hop;
        }
        const auto* input = std::get_if<NodeInputRef>(&reader.destination);
        const auto* node = input ? graph.findNode(input->nodeId) : nullptr;
        if (node == nullptr)
            return std::string("its picture is read by something other than a node");
        if (node->typeId == kCompositionOutputNodeType)
            return hop;
        if (node->typeId == kLayerOutputNodeType) {
            if (input->port != kLayerOutputContentInputPort)
                return std::string("its picture is read through another input of a Layer node");
            if (phase == Phase::After)
                return std::string("a node sits between two Layer nodes that resample its picture");
            const LayerOutputBoundary* layer = nullptr;
            for (const auto& boundary : graph.layerOutputs())
                if (!boundary.entryOwned() && boundary.nodeId == node->id)
                    layer = &boundary;
            if (layer == nullptr)
                return std::string("its picture is read by a Layer node without a layer");
            hop.layers.push_back(layer->layerId);
            phase = Phase::Layers;
            from = node->id;
            continue;
        }
        if (auto refusal = betweenNodeRefusal(work, *node, input->port, "between a group and the layer above it"))
            return std::move(*refusal);
        (phase == Phase::Before ? hop.before : hop.after).push_back(node->id);
        if (phase == Phase::Layers)
            phase = Phase::After;
        from = node->id;
    }
    return std::string("its picture is read through a cycle");
}

// One entry of a group: its slot, its layer (none yet: drawn as an entry layer at its defaults)
// and the nodes between that layer and the slot (in the group's space).
struct GroupEntry final {
    LayerSlotId slot;
    std::optional<LayerId> layer;
    std::vector<NodeId> after;
};

// The entries of `merge` drawn into a larger picture of the group, or why it cannot be.
[[nodiscard]] std::variant<std::vector<GroupEntry>, std::string> groupEntries(const Composition& work,
                                                                              const NodeId merge) {
    const auto& graph = work.graph();
    const auto* stack = graph.merge(merge);
    if (stack == nullptr)
        return std::string("the group is not a Merge");
    std::vector<GroupEntry> entries;
    std::unordered_set<LayerId> layers;
    for (const auto& entry : stack->entries()) {
        const auto* edge = mergeSlotImageEdge(graph, merge, entry.slotId);
        if (edge == nullptr)
            continue;
        if (mergeEntryAdjustment(graph, merge, entry.slotId))
            return std::string("an Adjustment entry applies its nodes to the group's picture at the composition's scale");
        GroupEntry item{entry.slotId, std::nullopt, {}};
        if (const auto* layer = mergeEntryLayer(graph, merge, entry.slotId)) {
            item.layer = layer->layerId;
            if (mergeEntriesOfLayer(graph, layer->layerId).size() != 1)
                return "the Layer node '" + layer->name + "' is shared with another Merge entry";
            if (layer->trackMatte && layer->trackMatte->node)
                return "the layer '" + layer->name + "' is matted by a node's Mask, which is drawn at the composition's scale";
            if (!layer->entryOwned()) {
                // The nodes after the Layer node, walking up from the slot.
                auto cursor = edge->source.nodeId;
                for (std::size_t step = 0; cursor != layer->nodeId; ++step) {
                    const auto* node = graph.findNode(cursor);
                    const auto* definition = node ? builtInNodeDefinitions().find(node->typeId, node->schemaVersion) : nullptr;
                    if (step > graph.nodes().size() || definition == nullptr || definition->inputs.empty())
                        return "the nodes after the layer '" + layer->name + "' cannot be followed";
                    const auto& port = definition->inputs.front().name;
                    if (auto refusal = betweenNodeRefusal(work, *node, port, "between a layer of the group and its slot"))
                        return std::move(*refusal);
                    item.after.push_back(cursor);
                    const EdgeRecord* into = nullptr;
                    for (const auto& candidate : graph.edges())
                        if (const auto* in = std::get_if<NodeInputRef>(&candidate.destination);
                            in != nullptr && in->nodeId == cursor && in->port == port)
                            into = &candidate;
                    if (into == nullptr)
                        return "the nodes after the layer '" + layer->name + "' cannot be followed";
                    cursor = into->source.nodeId;
                }
            }
            layers.insert(layer->layerId);
        }
        entries.push_back(std::move(item));
    }
    for (const auto& item : entries) {
        const auto* layer = item.layer ? graph.findLayer(*item.layer) : nullptr;
        if (layer != nullptr && layer->trackMatte && !layers.contains(layer->trackMatte->source))
            return "the layer '" + layer->name + "' is matted by a layer outside the group";
    }
    return entries;
}

// The entry's layer of `slot`, made at its defaults (the identity) when it has none yet.
[[nodiscard]] LayerId ensureEntryLayer(Composition& work, ExpansionTarget& target, const NodeId merge,
                                       const LayerSlotId slot) {
    if (const auto* layer = mergeEntryLayer(work.graph(), merge, slot))
        return layer->layerId;
    auto made = target.createEntryLayer(merge, slot);
    if (!made.layer)
        throw SvgArtworkWriteError("A group's entry layer could not be made: " + made.failure);
    return *made.layer;
}

// What the layers above an artwork's layer give its picture: the span its band counts (the layers
// up to the first group that is not drawn larger, which resamples what is below it), the smallest
// scale and any turn of every layer above (for the window estimate), and whether the window clip
// reaches the picture through all of them.
struct Above final {
    Span span;
    double smallest = 1.0;
    bool turns = false;
    bool clipped = true;
};

// One group above the artwork's layer: its Merge, how its picture reaches the next, its entries,
// the span of the layers resampling its picture, and whether it is already drawn larger (by
// another artwork's expansion).
struct GroupLevel final {
    NodeId merge;
    Hop hop;
    std::vector<GroupEntry> entries;
    Span span;
    bool done = false;
};

// Draws every group above the artwork's layer (reached by `first`, the hop from that layer to its
// Merge) at the output scale its picture needs (VectorArtworkOversampledGroup) and returns what is
// above the layer. A group that cannot be is named in `groupsResampled`; nothing above it counts in
// the band of what is below it. `pivotsClip`: whether a content pivot reading the artwork's picture
// still clips it (contentPivotClips).
[[nodiscard]] Above oversampleGroups(Composition& work, ExpansionTarget& target, const Hop& first,
                                     const OverrideLookup& override, const bool pivotsClip,
                                     VectorArtworkExpansion& result) {
    Above above;
    // The Layer nodes after the artwork's layer (the first of `first.layers`) resample its picture
    // in one resample with it.
    for (std::size_t i = 1; i < first.layers.size(); ++i) {
        const auto* layer = work.graph().findLayer(first.layers[i]);
        // #642 B4: each reads the layer before it, a Layer Output whose bounds the runtime works
        // out before the frame is drawn when the artwork's own picture's are, so a content pivot
        // here clips then too.
        const auto span = layer ? chainSpan(work, *layer, override, pivotsClip) : std::nullopt;
        if (!span) {
            // Its scale is not known: count nothing above, and clip nothing.
            return Above{Span{}, 0.0, true, false};
        }
        above.span *= *span;
    }
    above.smallest = above.span.smallest;
    above.turns = above.span.turns;
    above.clipped = above.span.framePivots && allClipThrough(work, first.after);
    if (!first.slot)
        return above;

    // The groups, from the artwork's Merge up, until the composition's output or one that cannot
    // be drawn larger.
    std::vector<GroupLevel> levels;
    std::unordered_set<NodeId> seen;
    bool reachedOutput = false;
    for (auto merge = first.slot->first; seen.insert(merge).second;) {
        if (result.groupsResampled.contains(merge))
            break;
        auto hop = hopFrom(work, merge, {});
        if (auto* refusal = std::get_if<std::string>(&hop)) {
            result.groupsResampled.emplace(merge, std::move(*refusal));
            break;
        }
        auto& walked = std::get<Hop>(hop);
        if (!walked.slot && walked.layers.empty()) {
            // The composition's own picture: nothing resamples it.
            reachedOutput = true;
            above.clipped = above.clipped && allClipThrough(work, walked.before);
            break;
        }
        GroupLevel level{merge, std::move(walked), {}, {}, result.groups.contains(merge)};
        bool known = true;
        for (const auto id : level.hop.layers) {
            const auto* layer = work.graph().findLayer(id);
            const auto span = layer ? chainSpan(work, *layer, override) : std::nullopt;
            if (!span) {
                known = false;
                break;
            }
            level.span *= *span;
        }
        if (!known) {
            result.groupsResampled.emplace(merge, "the scale of a layer that resamples the group is driven by the "
                                                  "graph, so its largest scale is not known when compiling");
            break;
        }
        if (!level.done) {
            auto entries = groupEntries(work, merge);
            if (auto* refusal = std::get_if<std::string>(&entries)) {
                result.groupsResampled.emplace(merge, std::move(*refusal));
                break;
            }
            level.entries = std::move(std::get<std::vector<GroupEntry>>(entries));
        }
        const auto next = level.hop.slot ? std::optional{level.hop.slot->first} : std::nullopt;
        levels.push_back(std::move(level));
        if (!next) {
            reachedOutput = true;
            break;
        }
        merge = *next;
    }
    // What is beyond the last group walked is not known: nothing is clipped through it.
    if (!reachedOutput)
        above.clipped = false;

    // From the top down: the band each group needs (the layers above it, up to a group that is not
    // drawn larger), its window over the animation (every layer above), its budget and its lengths.
    const double width = static_cast<double>(work.format().width());
    const double height = static_cast<double>(work.format().height());
    Span band;
    Span window;
    bool clipped = above.clipped;
    for (std::size_t index = levels.size(); index-- > 0;) {
        auto& level = levels[index];
        band *= level.span;
        window *= level.span;
        clipped = clipped && level.span.framePivots && allClipThrough(work, level.hop.before) &&
                  allClipThrough(work, level.hop.after);
        if (level.done)
            continue;
        if (!std::isfinite(band.largest) || (band.largest <= 1.0 && !band.turns))
            continue;
        double factor = 1.0;
        while (factor < band.largest && factor < kVectorArtworkMaxOversample)
            factor *= 2.0;
        if (band.turns)
            factor = std::max(factor, 2.0);
        std::string limited;
        if (band.largest > kVectorArtworkMaxOversample)
            limited = "drawn at " + std::to_string(static_cast<int>(factor)) + "x, the most any group oversamples";
        const double margin = nodeMargin(work, level.hop.before);
        factor = withinPixelBudget(
            factor,
            [&](const double candidate) {
                return estimatedOversampledPixels(candidate, width * height, width, height, window.smallest,
                                                  window.turns, clipped, margin);
            },
            limited);
        // The lengths in the group's space: after each entry's layer, and between the group and its reader.
        std::vector<ParameterId> lengths = nodeLengths(work, level.hop.before);
        for (const auto& entry : level.entries) {
            const auto after = nodeLengths(work, entry.after);
            lengths.insert(lengths.end(), after.begin(), after.end());
        }
        std::optional<std::vector<ScaledParameter>> scaled;
        for (; factor > 1.0; factor /= 2.0)
            if ((scaled = scaledAll(work, lengths, factor, limited)))
                break;
        if (factor <= 1.0 || !scaled) {
            // Its reader resamples its picture: nothing above it reaches the groups below.
            result.groupsResampled.emplace(level.merge, limited.empty() ? std::string("its lengths cannot be scaled")
                                                                        : limited);
            band = Span{};
            continue;
        }

        // Draw the group larger: every entry's layer into it, its reader back from it.
        VectorArtworkOversampledGroup group{level.merge, {}, {}, factor, std::move(limited)};
        for (const auto& entry : level.entries) {
            const auto layer = entry.layer ? *entry.layer : ensureEntryLayer(work, target, level.merge, entry.slot);
            result.outputScales[layer] = factor;
            group.entries.push_back(layer);
        }
        group.reader = !level.hop.layers.empty()
                           ? level.hop.layers.front()
                           : ensureEntryLayer(work, target, level.hop.slot->first, level.hop.slot->second);
        result.contentScales[group.reader] = factor;
        writeScaled(work, target, std::move(*scaled), factor, result);
        result.groups.emplace(level.merge, std::move(group));
    }
    // The artwork's layer's picture is resampled by everything above it, through the groups drawn
    // at their scale.
    above.span *= band;
    above.smallest *= window.smallest;
    above.turns = above.turns || window.turns;
    above.clipped = clipped;
    return above;
}

// A carrier holding the constant scale `factor` about the composition's origin, with nothing to
// draw and no parent.
[[nodiscard]] LayerId addScaleCarrier(Composition& work, ExpansionTarget& target, const std::string& name,
                                      const double factor) {
    auto& graph = work.graph();
    const auto* definition = builtInNodeDefinitions().find(kLayerOutputNodeType, kLayerOutputNodeSchemaVersion);
    const auto carrierNode = target.allocateNode();
    if (definition == nullptr || !carrierNode)
        throw SvgArtworkWriteError("The artwork's scale carrier could not be made");
    NodeRecord carrier{*carrierNode, std::string(kLayerOutputNodeType), {}, kLayerOutputNodeSchemaVersion};
    // A frame pivot is the frame's centre plus the anchor: an anchor of minus the centre pivots on
    // the origin, so the carrier maps a point p to factor * p.
    const Vec2d origin{-static_cast<double>(work.format().width()) / 2.0,
                       -static_cast<double>(work.format().height()) / 2.0};
    for (const auto& parameter : definition->parameters) {
        if (parameter.role == kTimeRemapParameterRole)
            continue;
        ParameterValue value = parameter.role == kPositionParameterRole ? ParameterValue{Vec2d{0.0, 0.0}}
                               : parameter.role == kAnchorParameterRole ? ParameterValue{origin}
                               : parameter.role == kScaleParameterRole  ? ParameterValue{Vec2d{factor, factor}}
                               : parameter.role == kRotationParameterRole ? ParameterValue{0.0}
                                                                          : parameter.defaultValue;
        const auto id = target.allocateParameter();
        if (!id || !work.parameters().insert({*id, parameter.schemaKey, ConstantValueSource{std::move(value)}}))
            throw SvgArtworkWriteError("The artwork's scale carrier could not be made");
        carrier.parameters.push_back({parameter.role, *id});
    }
    std::ranges::sort(carrier.parameters, {}, &ParameterBinding::role);
    LayerOutputBoundary carrierLayer{carrier.id, target.allocateLayer(), name, std::string(kLayerOutputOutputPort)};
    carrierLayer.pivot = LayerPivot::Frame;
    const auto carrierId = carrierLayer.layerId;
    if (!graph.addNode(std::move(carrier)) || !graph.addLayerOutput(std::move(carrierLayer)))
        throw SvgArtworkWriteError("The artwork's scale carrier could not be made");
    return carrierId;
}

// #642 B4: whether a content pivot on the artwork's layer still clips its picture to the window the
// frame shows. It does when the runtime can work out the bounds it pivots on before the frame is
// drawn (runtime::detail::UnclippedPictures): every source the artwork is made of but text (whose
// bounds are its rasterized words), and between the artwork and the layer the blurs, the colour
// nodes and the layer's masks, not a Glow or a Drop Shadow (their pictures' bounds follow a
// Transform 2D and a directional pass). Otherwise the whole picture is drawn, within the budget.
[[nodiscard]] bool contentPivotClips(const Composition& work, const std::vector<NodeId>& artworkNodes,
                                     const std::vector<NodeId>& between) {
    const auto& graph = work.graph();
    for (const auto id : artworkNodes)
        if (const auto* node = graph.findNode(id); node != nullptr && node->typeId == kTextSourceNodeType)
            return false;
    for (const auto id : between) {
        const auto* node = graph.findNode(id);
        if (node == nullptr)
            return false;
        if (nodeMuted(work.nodeLayout(), id))
            continue;
        if (node->typeId == kGlowNodeType || node->typeId == kDropShadowNodeType)
            return false;
    }
    return true;
}

// The span of the artwork's own layer and its parents, or why its largest scale is not known.
[[nodiscard]] std::variant<Span, std::string> holdingSpan(const Composition& work, const LayerOutputBoundary& holding,
                                                          const OverrideLookup& override, const bool contentPivotClips) {
    Span span;
    std::unordered_set<LayerId> seen;
    for (const auto* layer = &holding; layer != nullptr;
         layer = layer->parent ? work.graph().findLayer(*layer->parent) : nullptr) {
        if (!seen.insert(layer->layerId).second)
            break;
        const auto largest = scaleBound(work, *layer, override);
        const auto smallest = scaleFloor(work, *layer, override);
        if (!largest || !smallest)
            return std::string(layer == &holding ? "the layer's scale is driven by the graph, so its largest scale "
                                                   "is not known when compiling"
                                                 : "a parent's scale is driven by the graph, so the layer's largest "
                                                   "scale is not known when compiling");
        const bool clips = layer->pivot == LayerPivot::Frame || (contentPivotClips && layer == &holding);
        span *= Span{*largest, *smallest, turns(work, *layer, override), clips};
    }
    return span;
}

// Draws the artwork of `reader` at its layer's output scale (VectorArtworkOversampledLayer), with
// `own` the span of the layer and its parents and `above` what is above it. Returns why it is not,
// or nothing once it is (or once the layer needs no more pixels than the composition's: it neither
// scales up nor turns). `contentArea` is the artwork's area at a scale of 1, for the window estimate.
[[nodiscard]] std::optional<std::string> oversampleLayer(Composition& work, ExpansionTarget& target,
                                                         const ArtworkReader& reader, const NodeId root,
                                                         const std::vector<NodeId>& artworkNodes, const Span& own,
                                                         const Above& above, const double contentArea,
                                                         VectorArtworkExpansion& result) {
    const auto layerId = reader.layer->layerId;
    // The band: the layer's largest output scale through its parents and everything above it,
    // over its whole animation.
    const double largest = own.largest * above.span.largest;
    const bool turned = own.turns || above.span.turns;
    if (!std::isfinite(largest))
        return std::string("the layer's scale is not finite");
    if (largest <= 1.0 && !turned)
        return std::nullopt;
    double factor = 1.0;
    while (factor < largest && factor < kVectorArtworkMaxOversample)
        factor *= 2.0;
    if (turned)
        factor = std::max(factor, 2.0);
    std::string limited;
    if (largest > kVectorArtworkMaxOversample)
        limited = "drawn at " + std::to_string(static_cast<int>(factor)) + "x, the most any layer oversamples";

    auto lengths = scaledLengths(work, *reader.layer, reader.between);
    if (const auto* refusal = std::get_if<std::string>(&lengths))
        return *refusal;
    // Halved while its window over the animation is past the pixel budget. The window is clipped to
    // what reaches the frame only when every layer from this one up pivots on the frame and every
    // node between clips through.
    const bool clipped = own.framePivots && above.clipped && allClipThrough(work, reader.between);
    const double margin = nodeMargin(work, reader.between);
    const double width = static_cast<double>(work.format().width());
    const double height = static_cast<double>(work.format().height());
    factor = withinPixelBudget(
        factor,
        [&](const double candidate) {
            return estimatedOversampledPixels(candidate, contentArea, width, height, own.smallest * above.smallest,
                                              own.turns || above.turns, clipped, margin);
        },
        limited);
    // Halved while a scaled length would leave its domain (a blur radius past its largest).
    std::optional<std::vector<ScaledParameter>> scaled;
    for (; factor > 1.0; factor /= 2.0)
        if ((scaled = scaledAll(work, std::get<std::vector<ParameterId>>(lengths), factor, limited)))
            break;
    if (factor <= 1.0 || !scaled)
        return limited.empty() ? std::string("the layer's lengths cannot be scaled") : limited;

    const auto name = reader.layer->name;
    const auto carrier = addScaleCarrier(work, target, name, factor);
    const auto leaves = parentLeaves(work, root, artworkNodes, carrier);
    writeScaled(work, target, std::move(*scaled), factor, result);
    result.contentScales[layerId] = factor;
    result.oversampled.emplace(layerId, VectorArtworkOversampledLayer{carrier, leaves, factor, std::move(limited)});
    return std::nullopt;
}

// Draws the artwork of the layer reading `root` sharp at its output scale: composed through its
// geometry when nothing can change it, oversampled otherwise, and every group above it that scales
// its picture drawn at that group's output scale. Returns why neither, or nothing.
[[nodiscard]] std::optional<std::string> drawSharp(Composition& work, ExpansionTarget& target,
                                                   const std::vector<EdgeRecord>& readers, const NodeId root,
                                                   const std::vector<NodeId>& artworkNodes,
                                                   const OverrideLookup& override, const double contentArea,
                                                   VectorArtworkExpansion& result) {
    auto found = readerOf(work, readers);
    if (auto* refusal = std::get_if<std::string>(&found))
        return std::move(*refusal);
    auto reader = std::get<ArtworkReader>(found);
    const auto layerId = reader.layer->layerId;
    const auto& graph = work.graph();
    const bool plain = reader.between.empty() && reader.layer->pivot == LayerPivot::Frame &&
                       reader.layer->masks.empty() && reader.layer->timing.isIdentityMapping() &&
                       layerParameterBinding(graph, *reader.layer, kTimeRemapParameterRole) == nullptr;
    // What can stop the layer being oversampled is known before anything above it changes.
    const bool pivotsClip = contentPivotClips(work, artworkNodes, reader.between);
    auto own = holdingSpan(work, *reader.layer, override, pivotsClip);
    if (auto* refusal = std::get_if<std::string>(&own)) {
        if (plain)
            return composeLayerTransform(work, target, reader.layer, root, artworkNodes, result);
        return std::move(*refusal);
    }
    if (!plain)
        if (const auto lengths = scaledLengths(work, *reader.layer, reader.between);
            const auto* refusal = std::get_if<std::string>(&lengths))
            return *refusal;

    // How its picture reaches its Merge, and the groups above.
    Hop first;
    first.layers.push_back(layerId);
    bool walked = true;
    if (reader.layer->entryOwned()) {
        first.slot = std::pair{reader.layer->nodeId, *reader.layer->entrySlot};
    } else if (auto hop = hopFrom(work, reader.layer->nodeId, first); auto* next = std::get_if<Hop>(&hop)) {
        first = std::move(*next);
    } else {
        walked = false;
    }
    // Where nothing above it is known, its own band only, and no clip.
    const Above above =
        walked ? oversampleGroups(work, target, first, override, pivotsClip, result) : Above{Span{}, 0.0, true, false};
    // The groups may have added layers: find the layer again.
    reader.layer = work.graph().findLayer(layerId);
    if (reader.layer == nullptr)
        throw SvgArtworkWriteError("The artwork's layer could not be found again");
    if (plain && above.span.largest <= 1.0 && !above.span.turns && !result.outputScales.contains(layerId))
        return composeLayerTransform(work, target, reader.layer, root, artworkNodes, result);
    return oversampleLayer(work, target, reader, root, artworkNodes, std::get<Span>(own), above, contentArea, result);
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
    std::unordered_set<std::uint64_t> nodes, edges, parameters, slots, layers, curves;
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
    for (const auto& curve : work.animationCurves().records())
        curves.insert(animationCurveId(curve).value());

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
        // The artwork's area at a scale of 1, for the window estimate of an oversampled picture:
        // fitted, the composition's frame; unfitted, the file's own viewport.
        double contentArea = 0.0;
        if (scene) {
            SvgArtworkOptions options;
            options.width = work.format().width();
            options.height = work.format().height();
            options.fit = vectorArtworkFits(source, work.parameters());
            options.fonts = fonts;
            description = buildSvgArtwork(*scene, options);
            contentArea = options.fit ? options.width * options.height : scene->width * scene->height;
        } else {
            description.nodes.push_back({std::string(kLayerStackNodeType), {}, {}, true});
        }
        try {
            ExpansionTarget target(work, id, nodes, edges, parameters, slots, layers, curves);
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
            else if (auto why = drawSharp(work, target, readers, root, writer.nodeIds(), context.override, contentArea,
                                          result))
                result.resampled.emplace(id, std::move(*why));
            result.roots.emplace(id, root);
            result.notes.emplace(id, std::move(description.notes));
        } catch (const SvgArtworkWriteError& failure) {
            // The composition is half written: nothing of it is used.
            result.roots.clear();
            result.notes.clear();
            result.composed.clear();
            result.oversampled.clear();
            result.groups.clear();
            result.contentScales.clear();
            result.outputScales.clear();
            result.scaledParameters.clear();
            result.resampled.clear();
            result.groupsResampled.clear();
            for (const auto other : artwork)
                result.unresolved.emplace(other, failure.what());
            return result;
        }
    }
    result.composition = std::move(work);
    return result;
}

} // namespace bloom::document
