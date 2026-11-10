#include <bloom/document/vector_artwork_nodes.hpp>

#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

namespace bloom::document {

NodeDefinition vectorArtworkDefinition() {
    return {{std::string(kVectorArtworkNodeType), kVectorArtworkNodeSchemaVersion},
            NodeLoweringKind::VectorArtwork,
            {},
            {{std::string(kVectorArtworkOutputPort), SocketValueKind::Image}},
            {{std::string(kVectorArtworkBlockParameterRole), std::string(kVectorArtworkBlockParameterSchemaKey),
              ParameterValueKind::Integer, true, false, std::int64_t{0}},
             {std::string(kVectorArtworkFitParameterRole), std::string(kVectorArtworkFitParameterSchemaKey),
              ParameterValueKind::Boolean, true, false, true}},
            std::nullopt,
            NodeCardinality::Many,
            NodeCategory::Sources};
}

bool isVectorArtworkParameterValueValid(const std::string_view schemaKey, const ParameterValue& value) noexcept {
    if (schemaKey == kVectorArtworkBlockParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&value);
        return stored != nullptr && *stored >= 0;
    }
    if (schemaKey == kVectorArtworkFitParameterSchemaKey)
        return std::holds_alternative<bool>(value);
    return false;
}

bool isVectorArtworkBlock(const DataBlockRecord& block) noexcept {
    return std::holds_alternative<DataBlockRecordId>(block.id) && block.kind == DataBlockKind::Opaque &&
           block.typeId == kVectorArtworkBlockTypeId &&
           std::holds_alternative<OpaqueExtensionPayload>(block.payload);
}

namespace {

const ConstantValueSource* constantOf(const NodeRecord& node, const ParameterStore& parameters,
                                      const std::string_view role) {
    const auto binding = std::ranges::find(node.parameters, role, &ParameterBinding::role);
    if (binding == node.parameters.end())
        return nullptr;
    const auto* record = parameters.find(binding->parameterId);
    return record == nullptr ? nullptr : std::get_if<ConstantValueSource>(&record->source);
}

std::optional<DataBlockRecordId> namedBlock(const NodeRecord& node, const ParameterStore& parameters) {
    if (node.typeId != kVectorArtworkNodeType)
        return std::nullopt;
    const auto* constant = constantOf(node, parameters, kVectorArtworkBlockParameterRole);
    const auto* stored = constant == nullptr ? nullptr : std::get_if<std::int64_t>(&constant->value);
    if (stored == nullptr || *stored <= 0)
        return std::nullopt;
    return DataBlockRecordId::fromRaw(static_cast<std::uint64_t>(*stored));
}

} // namespace

std::optional<DataBlockRecordId> vectorArtworkBlockId(const NodeRecord& node, const ParameterStore& parameters) {
    return namedBlock(node, parameters);
}

const DataBlockRecord* vectorArtworkBlock(const Project& project, const NodeRecord& node,
                                          const ParameterStore& parameters) {
    const auto id = namedBlock(node, parameters);
    if (!id)
        return nullptr;
    const auto* block = project.findDataBlock(*id);
    return block != nullptr && isVectorArtworkBlock(*block) ? block : nullptr;
}

bool vectorArtworkFits(const NodeRecord& node, const ParameterStore& parameters) {
    const auto* constant = constantOf(node, parameters, kVectorArtworkFitParameterRole);
    const auto* fit = constant == nullptr ? nullptr : std::get_if<bool>(&constant->value);
    return fit == nullptr || *fit;
}

const AssetRecord* vectorArtworkSourceAsset(const Project& project, const DataBlockRecord& block) {
    const auto* locator = std::get_if<AssetLocator>(&block.provenance.source);
    if (locator == nullptr)
        return nullptr;
    const auto found = std::ranges::find_if(project.assets(), [&](const AssetRecord& asset) {
        return asset.locator.path == locator->path && asset.contentDigest == block.provenance.contentDigest;
    });
    return found == project.assets().end() ? nullptr : &*found;
}

bool isSvgSourceAsset(const AssetRecord& asset) noexcept {
    if (asset.kind != AssetKind::Image)
        return false;
    const std::string_view path = asset.locator.path;
    constexpr std::string_view suffix = ".svg";
    if (path.size() < suffix.size())
        return false;
    const auto tail = path.substr(path.size() - suffix.size());
    return std::ranges::equal(tail, suffix, [](const char a, const char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    });
}

namespace {
std::string svgAssetDisplayName(const AssetRecord& asset) {
    if (!asset.name.empty())
        return asset.name;
    const auto slash = asset.locator.path.find_last_of('/');
    return slash == std::string::npos ? asset.locator.path : asset.locator.path.substr(slash + 1);
}
} // namespace

std::optional<std::string> svgFootageWarning(const Project& project, const Composition& composition,
                                             const NodeRecord& node) {
    if (node.typeId != "bloom.image-source" && node.typeId != "bloom.video-source")
        return std::nullopt;
    for (const auto& binding : node.parameters) {
        if (binding.role != "asset")
            continue;
        const auto* parameter = composition.parameters().find(binding.parameterId);
        const auto* constant = parameter ? std::get_if<ConstantValueSource>(&parameter->source) : nullptr;
        const auto* text = constant ? std::get_if<std::string>(&constant->value) : nullptr;
        if (text == nullptr || text->empty())
            return std::nullopt;
        std::uint64_t raw = 0;
        for (const char c : *text) {
            if (c < '0' || c > '9' || raw > (std::numeric_limits<std::uint64_t>::max() - 9) / 10)
                return std::nullopt;
            raw = raw * 10 + static_cast<std::uint64_t>(c - '0');
        }
        const auto* asset = project.findAsset(AssetId::fromRaw(raw));
        if (asset == nullptr || !isSvgSourceAsset(*asset))
            return std::nullopt;
        return "'" + svgAssetDisplayName(*asset) +
               "' is an SVG source, not footage: add it again from Assets to rebuild the artwork";
    }
    return std::nullopt;
}

std::optional<std::string> layerSvgFootageWarning(const Project& project, const Composition& composition,
                                                  const LayerId layerId) {
    const auto& graph = composition.graph();
    const auto* layer = graph.findLayer(layerId);
    if (layer == nullptr)
        return std::nullopt;
    const auto reading = [&](const auto& matches) -> const EdgeRecord* {
        for (const auto& edge : graph.edges())
            if (matches(edge.destination))
                return &edge;
        return nullptr;
    };
    const EdgeRecord* edge = reading([&](const InputPortRef& destination) {
        if (layer->entryOwned()) {
            const auto* slot = std::get_if<LayerStackInputRef>(&destination);
            return slot != nullptr && slot->stackNodeId == layer->nodeId && slot->slotId == *layer->entrySlot &&
                   slot->role == kLayerStackContentInputRole;
        }
        const auto* input = std::get_if<NodeInputRef>(&destination);
        return input != nullptr && input->nodeId == layer->nodeId && input->port == kLayerOutputContentInputPort;
    });
    for (std::size_t step = 0; edge != nullptr && step <= graph.nodes().size(); ++step) {
        const auto* node = graph.findNode(edge->source.nodeId);
        if (node == nullptr)
            return std::nullopt;
        if (auto warning = svgFootageWarning(project, composition, *node))
            return warning;
        const auto* definition = builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
        if (definition == nullptr || definition->inputs.empty())
            return std::nullopt;
        const auto port = definition->inputs.front().name;
        edge = reading([&](const InputPortRef& destination) {
            const auto* input = std::get_if<NodeInputRef>(&destination);
            return input != nullptr && input->nodeId == node->id && input->port == port;
        });
    }
    return std::nullopt;
}

const DataBlockRecord* findVectorArtworkBlock(const Project& project, const core::Sha256Digest& digest) {
    for (const auto& block : project.typedDataBlocks())
        if (isVectorArtworkBlock(block) && block.provenance.contentDigest == digest)
            return &block;
    return nullptr;
}

ValidationResult validateVectorArtworkReferences(const Project& project, const Composition& composition) {
    ValidationResult result;
    for (const auto& node : composition.graph().nodes()) {
        if (node.typeId != kVectorArtworkNodeType)
            continue;
        const auto id = namedBlock(node, composition.parameters());
        if (!id)
            continue;
        const auto* block = project.findDataBlock(*id);
        if (block == nullptr || !isVectorArtworkBlock(*block))
            result.add(ValidationCode::MissingReference,
                       "compositions[" + std::to_string(composition.id().value()) + "].graph.nodes[" +
                           std::to_string(node.id.value()) + "].artwork",
                       "Vector Artwork names an SVG data block the project does not hold");
    }
    return result;
}

} // namespace bloom::document
