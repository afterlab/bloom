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

// Why the artwork cannot be drawn larger through `node` (read at `port`), or nothing.
[[nodiscard]] std::optional<std::string> betweenNodeRefusal(const Composition& work, const NodeRecord& node,
                                                            const std::string_view port) {
    const auto& graph = work.graph();
    const auto* definition = builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
    const std::string name = "the " + node.typeId + " node between the artwork and its layer";
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

// The lengths an oversampled layer's expansion multiplies: those of the nodes between and its masks.
[[nodiscard]] std::variant<std::vector<ParameterId>, std::string>
scaledLengths(const Composition& work, const LayerOutputBoundary& layer, const std::vector<NodeId>& between) {
    std::vector<ParameterId> lengths;
    for (const auto id : between) {
        const auto* node = work.graph().findNode(id);
        if (node == nullptr || nodeMuted(work.nodeLayout(), id))
            continue;
        const auto* scaled = scaledBetweenNode(node->typeId);
        for (const auto& binding : node->parameters)
            if (scaled != nullptr &&
                std::ranges::find(scaled->lengths, std::string_view(binding.role)) != scaled->lengths.end())
                lengths.push_back(binding.parameterId);
    }
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

// Draws the artwork of `reader` at its layer's output scale (VectorArtworkOversampledLayer).
// Returns why it is not, or nothing once it is (or once the layer needs no more pixels than the
// composition's: it neither scales up nor turns).
[[nodiscard]] std::optional<std::string> oversampleLayer(Composition& work, ExpansionTarget& target,
                                                         const ArtworkReader& reader, const NodeId root,
                                                         const std::vector<NodeId>& artworkNodes,
                                                         const OverrideLookup& override,
                                                         VectorArtworkExpansion& result) {
    auto& graph = work.graph();
    const auto layerId = reader.layer->layerId;
    // The band: the layer's largest output scale through its parents, over its whole animation.
    double largest = 1.0;
    bool turned = false;
    std::unordered_set<LayerId> seen;
    for (const auto* layer = reader.layer; layer != nullptr;
         layer = layer->parent ? graph.findLayer(*layer->parent) : nullptr) {
        if (!seen.insert(layer->layerId).second)
            break;
        const auto bound = scaleBound(work, *layer, override);
        if (!bound)
            return std::string(layer == reader.layer ? "the layer's scale is driven by the graph, so its largest scale "
                                                       "is not known when compiling"
                                                     : "a parent's scale is driven by the graph, so the layer's largest "
                                                       "scale is not known when compiling");
        largest *= *bound;
        turned = turned || turns(work, *layer, override);
    }
    if (!std::isfinite(largest))
        return std::string("the layer's scale is not finite");
    if (largest <= 1.0 && !turned)
        return std::nullopt;
    double band = 1.0;
    while (band < largest && band < kVectorArtworkMaxOversample)
        band *= 2.0;
    if (turned)
        band = std::max(band, 2.0);
    double factor = band;
    std::string limited;
    const double edge = static_cast<double>(std::max(work.format().width(), work.format().height()));
    double cap = kVectorArtworkMaxOversample;
    while (cap > 1.0 && cap * edge > kVectorArtworkMaxOversampledEdge)
        cap /= 2.0;
    if (factor > cap) {
        factor = cap;
        limited = "drawn at " + std::to_string(static_cast<int>(cap)) + "x, the most a " +
                  std::to_string(work.format().width()) + " x " + std::to_string(work.format().height()) +
                  " composition oversamples";
    }
    if (largest > kVectorArtworkMaxOversample && limited.empty())
        limited = "drawn at " + std::to_string(static_cast<int>(factor)) + "x, the most any layer oversamples";

    auto lengths = scaledLengths(work, *reader.layer, reader.between);
    if (const auto* refusal = std::get_if<std::string>(&lengths))
        return *refusal;
    // Halved while a scaled length would leave its domain (a blur radius past its largest).
    std::vector<ScaledParameter> scaled;
    for (; factor > 1.0; factor /= 2.0) {
        scaled.clear();
        bool fits = true;
        for (const auto id : std::get<std::vector<ParameterId>>(lengths)) {
            const auto* record = work.parameters().find(id);
            auto next = record ? scaledParameter(work, *record, factor) : std::nullopt;
            if (!next) {
                fits = false;
                if (record != nullptr)
                    limited = "the " + record->schemaKey + " length would leave its range at a larger scale";
                break;
            }
            scaled.push_back(std::move(*next));
        }
        if (fits)
            break;
    }
    if (factor <= 1.0)
        return limited.empty() ? std::string("the layer's lengths cannot be scaled") : limited;

    const auto carrier = addScaleCarrier(work, target, reader.layer->name, factor);
    const auto leaves = parentLeaves(work, root, artworkNodes, carrier);
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
    result.oversampled.emplace(layerId, VectorArtworkOversampledLayer{carrier, leaves, factor, std::move(limited)});
    return std::nullopt;
}

// Draws the artwork of the layer reading `root` sharp at its output scale: composed through its
// geometry when nothing can change it, oversampled otherwise. Returns why neither, or nothing.
[[nodiscard]] std::optional<std::string> drawSharp(Composition& work, ExpansionTarget& target,
                                                   const std::vector<EdgeRecord>& readers, const NodeId root,
                                                   const std::vector<NodeId>& artworkNodes,
                                                   const OverrideLookup& override, VectorArtworkExpansion& result) {
    auto found = readerOf(work, readers);
    if (auto* refusal = std::get_if<std::string>(&found))
        return std::move(*refusal);
    const auto& reader = std::get<ArtworkReader>(found);
    const auto& graph = work.graph();
    auto* layer = reader.layer;
    const bool plain = reader.between.empty() && layer->pivot == LayerPivot::Frame && layer->masks.empty() &&
                       layer->timing.isIdentityMapping() &&
                       layerParameterBinding(graph, *layer, kTimeRemapParameterRole) == nullptr;
    if (plain)
        return composeLayerTransform(work, target, layer, root, artworkNodes, result);
    return oversampleLayer(work, target, reader, root, artworkNodes, override, result);
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
            else if (auto why = drawSharp(work, target, readers, root, writer.nodeIds(), context.override, result))
                result.resampled.emplace(id, std::move(*why));
            result.roots.emplace(id, root);
            result.notes.emplace(id, std::move(description.notes));
        } catch (const SvgArtworkWriteError& failure) {
            // The composition is half written: nothing of it is used.
            result.roots.clear();
            result.notes.clear();
            result.composed.clear();
            result.oversampled.clear();
            result.scaledParameters.clear();
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
