#include <bloom/document/vector_artwork_nodes.hpp>

#include <bloom/document/parameter.hpp>

#include <algorithm>
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
