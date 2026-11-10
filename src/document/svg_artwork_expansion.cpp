// #642: the Vector Artwork layer's expansion. See svg_artwork.hpp.

#include <bloom/document/svg_artwork.hpp>

#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/media/svg_scene.hpp>

#include <algorithm>
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
            if (const auto layout = work.nodeLayout().find(id); layout != work.nodeLayout().end()) {
                // A muted Vector Artwork node is a muted root.
                if (layout->second.muted)
                    work.nodeLayout()[root].muted = true;
                work.nodeLayout().erase(id);
            }
            result.roots.emplace(id, root);
            result.notes.emplace(id, std::move(description.notes));
        } catch (const SvgArtworkWriteError& failure) {
            // The composition is half written: nothing of it is used.
            result.roots.clear();
            result.notes.clear();
            for (const auto other : artwork)
                result.unresolved.emplace(other, failure.what());
            return result;
        }
    }
    result.composition = std::move(work);
    return result;
}

} // namespace bloom::document
