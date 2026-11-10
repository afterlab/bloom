#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/media/svg_scene.hpp>

#include "node_operation_support.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/data_block.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/svg_artwork.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/platform/font_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace bloom::commands {
namespace {

struct Failure final {
    std::string message;
};

// The editable import's ids come from the project and its entry layers are made the way every
// new Merge entry is (EnsureEntryLayer), in the Draft the operation applies to.
class DraftTarget final : public document::SvgArtworkTarget {
  public:
    DraftTarget(document::Draft& draft, const document::CompositionId composition)
        : draft_(draft), composition_(composition) {}

    std::optional<document::NodeId> allocateNode() override { return draft_.ids().allocateNode(); }
    std::optional<document::ParameterId> allocateParameter() override { return draft_.ids().allocateParameter(); }
    std::optional<document::LayerSlotId> allocateLayerSlot() override { return draft_.ids().allocateLayerSlot(); }
    std::optional<document::EdgeId> allocateEdge() override { return draft_.ids().allocateEdge(); }
    std::optional<document::NodeGroupId> allocateNodeGroup() override { return draft_.ids().allocateNodeGroup(); }
    Entry createEntryLayer(const document::NodeId merge, const document::LayerSlotId slot) override {
        auto result = EnsureEntryLayer(composition_, merge, slot).apply(draft_);
        if (result.status == OperationStatus::Rejected)
            return {std::nullopt, result.issues.empty() ? std::string{} : result.issues.front().message};
        for (const auto& output : result.outputs)
            if (output.name == "layer")
                if (const auto* layer = std::get_if<document::LayerId>(&output.id))
                    return {*layer, {}};
        return {std::nullopt, {}};
    }

  private:
    document::Draft& draft_;
    document::CompositionId composition_;
};

// The file the artwork came from, as an Assets record: its path (project-relative when the
// project folder is known) and the SHA-256 of its bytes. Nothing reads the file again, so a
// moved or missing file is not an error here or when the project opens; the record stays and
// the Assets panel shows the file as missing like any other moved footage. A file already
// recorded with the same path and digest is reused.
std::optional<document::AssetId> recordSource(document::Draft& draft, const media::SvgScene& scene,
                                              const std::filesystem::path& projectDirectory,
                                              document::SvgArtworkNotes& notes) {
    if (scene.sourcePath.empty() || !scene.hasSourceDigest)
        return std::nullopt;
    document::AssetRecord asset;
    asset.kind = document::AssetKind::Image;
    try {
        asset.locator = assetLocatorFor(scene.sourcePath, projectDirectory);
    } catch (const std::exception&) {
        notes.add("The source file's path cannot be recorded in the project");
        return std::nullopt;
    }
    asset.contentDigest = scene.sourceDigest;
    asset.width = static_cast<std::uint32_t>(std::clamp(std::ceil(scene.width), 1.0, 65535.0));
    asset.height = static_cast<std::uint32_t>(std::clamp(std::ceil(scene.height), 1.0, 65535.0));
    auto& project = draft.project();
    if (const auto found = std::ranges::find_if(project.assets(), [&](const auto& candidate) {
            return candidate.locator.path == asset.locator.path && candidate.contentDigest == asset.contentDigest;
        });
        found != project.assets().end())
        return found->id;
    const auto id = draft.ids().allocateAsset();
    if (!id)
        throw Failure{"Asset ID space is exhausted"};
    asset.id = *id;
    asset.name = document::svgArtworkName(scene.sourcePath.filename().string(), "SVG");
    for (const auto& existing : project.assets())
        if (!existing.folder)
            asset.order = std::max(asset.order, existing.order == std::numeric_limits<std::uint64_t>::max()
                                                    ? existing.order
                                                    : existing.order + 1);
    if (!project.addAsset(std::move(asset))) {
        notes.add("The source file could not be recorded in the project");
        return std::nullopt;
    }
    return *id;
}

// The warnings of an import as operation issues: each by name, then the rest counted by reason.
void appendNotes(OperationResult& result, const document::SvgArtworkNotes& notes) {
    for (const auto& note : notes.listed) {
        if (result.issues.size() >= 400)
            break;
        result.issues.push_back({OperationIssueCode::Unsupported, note});
    }
    if (!notes.omitted.empty()) {
        std::string text;
        for (const auto& [reason, count] : notes.omitted)
            text += (text.empty() ? "" : "; ") + std::to_string(count) + " more: " + reason;
        result.issues.push_back({OperationIssueCode::Unsupported, "Further items not listed by name: " + text});
    }
}

// "As editable shapes": the description written into the composition, its root group on top of
// the composition's stack.
OperationResult writeEditable(document::Draft& draft, document::Composition& composition,
                              const document::CompositionId compositionId, const media::SvgScene& scene,
                              const document::SvgArtworkDescription& description, const std::string& name,
                              const std::filesystem::path& projectDirectory) {
    try {
        DraftTarget target(draft, compositionId);
        document::SvgArtworkWriter writer(composition, target, description);
        auto& graph = composition.graph();
        const auto top = graph.layerStack().nodeId();
        const auto rootMerge = writer.addRoot();
        const auto rootSlot = writer.addSlot(top, rootMerge);
        const auto rootLayer = writer.addEntry(top, rootSlot, document::svgArtworkName(name, "SVG"));
        document::SvgArtworkNotes notes;
        const auto sourceAsset = recordSource(draft, scene, projectDirectory, notes);
        notes.append(description.notes);
        writer.writeSteps();
        writer.writeFrames();
        if (graph.findNode(rootMerge) == nullptr)
            throw Failure{"The SVG root could not be created"};
        std::vector<OperationOutput> outputs{{"layer", rootLayer}, {"slot", rootSlot}, {"merge", rootMerge}};
        if (sourceAsset)
            outputs.push_back({"asset", *sourceAsset});
        auto result = OperationResult::applied(std::move(outputs));
        appendNotes(result, notes);
        return result;
    } catch (const Failure& failure) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue, failure.message);
    } catch (const document::SvgArtworkWriteError& failure) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue, failure.what());
    }
}


// "As one layer": a Vector Artwork layer on top of the composition's stack whose node names a
// data block holding the file's bytes (one block per SHA-256, shared by every import of the same
// bytes). The description is built here as well, so the import names exactly the warnings the
// editable import names and refuses what it would refuse.
OperationResult writeLayer(document::Draft& draft, document::Composition& composition,
                           const document::CompositionId compositionId, const PreparedSvgSource& source,
                           const document::SvgArtworkDescription& description, const std::string& name,
                           const bool fit, const std::filesystem::path& projectDirectory) {
    try {
        auto& project = draft.project();
        document::SvgArtworkNotes notes;
        const auto sourceAsset = recordSource(draft, *source.scene, projectDirectory, notes);
        notes.append(description.notes);

        document::DataBlockRecordId blockId;
        if (const auto* existing = document::findVectorArtworkBlock(project, source.digest)) {
            blockId = std::get<document::DataBlockRecordId>(existing->id);
        } else {
            if (!source.bytes)
                throw Failure{"The SVG's bytes were not read, so it cannot be held in the project"};
            std::size_t held = 0;
            for (const auto& block : project.typedDataBlocks())
                held += block.payloadBytes();
            const auto size = source.bytes->size();
            if (size > document::kMaxDataBlockPayloadBytes ||
                size > document::kMaxAggregateDataBlockPayloadBytes - std::min(held, document::kMaxAggregateDataBlockPayloadBytes))
                throw Failure{"The SVG '" + document::svgArtworkName(source.path.filename().string(), "SVG") + "' (" +
                              std::to_string((size + 1023) / 1024) +
                              " KiB) does not fit in the 128 MiB a project holds for embedded data; import it "
                              "as editable shapes instead"};
            const auto id = draft.ids().allocateDataBlock();
            if (!id)
                throw Failure{"Data block ID space is exhausted"};
            document::DataBlockRecord block;
            block.id = *id;
            block.kind = document::DataBlockKind::Opaque;
            block.typeId = std::string(document::kVectorArtworkBlockTypeId);
            block.schemaVersion = document::kVectorArtworkBlockSchemaVersion;
            block.mediaType = std::string(document::kVectorArtworkBlockMediaType);
            try {
                block.provenance.source = assetLocatorFor(source.path, projectDirectory);
            } catch (const std::exception&) {
                block.provenance.source = document::DataBlockProducer{"bloom.svg-import", "1", {}};
            }
            block.provenance.contentDigest = source.digest;
            block.payload = document::OpaqueExtensionPayload(*source.bytes);
            if (!project.addDataBlock(std::move(block)))
                throw Failure{"The SVG could not be held in the project"};
            blockId = *id;
        }

        // One node, written the way the editable import writes its nodes.
        document::SvgArtworkDescription layer;
        layer.nodes.push_back({std::string(document::kVectorArtworkNodeType),
                               {{std::string(document::kVectorArtworkBlockParameterRole),
                                 document::ParameterValue{static_cast<std::int64_t>(blockId.value())}},
                                {std::string(document::kVectorArtworkFitParameterRole), document::ParameterValue{fit}}},
                               {document::svgArtworkLayoutOrigin(composition), 0.0},
                               false});
        DraftTarget target(draft, compositionId);
        document::SvgArtworkWriter writer(composition, target, layer);
        const auto top = composition.graph().layerStack().nodeId();
        const auto node = writer.addRoot();
        const auto slot = writer.addSlot(top, node);
        const auto entry = writer.addEntry(top, slot, document::svgArtworkName(name, "SVG"));
        std::vector<OperationOutput> outputs{{"layer", entry}, {"slot", slot}, {"node", node}, {"dataBlock", blockId}};
        if (sourceAsset)
            outputs.push_back({"asset", *sourceAsset});
        auto result = OperationResult::applied(std::move(outputs));
        appendNotes(result, notes);
        return result;
    } catch (const Failure& failure) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue, failure.message);
    } catch (const document::SvgArtworkWriteError& failure) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue, failure.what());
    }
}

bool hasText(const media::SvgElement& element) {
    if (element.kind == media::SvgElement::Kind::Text)
        return true;
    return std::ranges::any_of(element.children, [](const auto& child) { return hasText(child); }) ||
           (element.clip && hasText(*element.clip)) || (element.mask && hasText(*element.mask));
}

void collectWarnings(const media::SvgElement& element, std::vector<SvgSourceWarning>& out) {
    for (const auto& text : element.approximations)
        out.push_back({"bloom.svg.approximated", (element.where.empty() ? element.name : element.where) + ": " + text,
                       text});
    for (const auto& child : element.children)
        collectWarnings(child, out);
}

SvgSourceRead finishPrepare(media::SvgReadResult read, std::shared_ptr<const std::vector<std::byte>> bytes,
                            const std::filesystem::path& path, const std::function<bool()>& cancel,
                            const std::function<void(SvgPreparePhase)>& phase) {
    SvgSourceRead out;
    out.error = read.error;
    out.diagnostic = read.diagnostic;
    if (read.error != media::SvgError::None || !read.scene)
        return out;
    // Asked to stop while the file was read: nothing is returned, however small the file was.
    if (cancel && cancel()) {
        out.error = media::SvgError::Cancelled;
        out.diagnostic = "Cancelled.";
        return out;
    }
    auto prepared = std::make_shared<PreparedSvgSource>();
    prepared->path = path;
    prepared->bytes = std::move(bytes);
    core::Sha256Hasher hasher;
    if (!hasher.update(*prepared->bytes)) {
        out.error = media::SvgError::ReadFailed;
        out.diagnostic = "The file could not be hashed.";
        return out;
    }
    prepared->digest = hasher.finalize();
    // The scene is ours alone until it is returned: record which file it came from and what that
    // file held, so the imported artwork names its source.
    const auto scene = std::const_pointer_cast<media::SvgScene>(read.scene);
    scene->sourcePath = path;
    scene->sourceDigest = prepared->digest;
    scene->hasSourceDigest = true;
    prepared->scene = scene;
    for (const auto& skipped : scene->skipped)
        prepared->warnings.push_back({"bloom.svg.not-imported", skipped.name + ": " + skipped.reason, skipped.reason});
    collectWarnings(scene->root, prepared->warnings);
    if (hasText(scene->root)) {
        if (phase)
            phase(SvgPreparePhase::FindingFonts);
        prepared->fonts = std::make_shared<const platform::FontCatalogue>(
            platform::FontCatalogueProvider{}.enumerate(cancel ? cancel : std::function<bool()>{}, false));
        if (cancel && cancel()) {
            out.error = media::SvgError::Cancelled;
            out.diagnostic = "Cancelled.";
            return out;
        }
    }
    out.source = std::move(prepared);
    return out;
}

} // namespace

std::string_view svgImportModeName(const SvgImportMode mode) noexcept {
    return mode == SvgImportMode::Shapes ? "shapes" : "layer";
}

std::optional<SvgImportMode> svgImportModeFromName(const std::string_view name) noexcept {
    if (name == "layer")
        return SvgImportMode::Layer;
    if (name == "shapes")
        return SvgImportMode::Shapes;
    return std::nullopt;
}

bool SvgSourceRead::cancelled() const noexcept { return error == media::SvgError::Cancelled; }

SvgSourceRead prepareSvgSource(const std::filesystem::path& path, const std::function<bool()>& cancel,
                               const std::function<void(SvgPreparePhase)>& phase) {
    if (phase)
        phase(SvgPreparePhase::Reading);
    SvgSourceRead out;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        out.error = media::SvgError::ReadFailed;
        out.diagnostic = "The file could not be opened.";
        return out;
    }
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0) {
        out.error = media::SvgError::ReadFailed;
        out.diagnostic = "The file could not be read.";
        return out;
    }
    if (static_cast<std::uint64_t>(size) > media::kMaxSvgFileBytes) {
        out.error = media::SvgError::TooLarge;
        out.diagnostic = "The file is larger than 32 MiB, the most an SVG import reads.";
        return out;
    }
    file.seekg(0);
    auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    if (!file && !file.eof()) {
        out.error = media::SvgError::ReadFailed;
        out.diagnostic = "The file could not be read.";
        return out;
    }
    return prepareSvgSource(std::move(bytes), path, cancel, {});
}

SvgSourceRead prepareSvgSource(std::shared_ptr<const std::vector<std::byte>> bytes, const std::filesystem::path& path,
                               const std::function<bool()>& cancel, const std::function<void(SvgPreparePhase)>& phase) {
    if (phase)
        phase(SvgPreparePhase::Reading);
    if (!bytes) {
        SvgSourceRead out;
        out.error = media::SvgError::ReadFailed;
        out.diagnostic = "The file could not be read.";
        return out;
    }
    if (bytes->size() > media::kMaxSvgFileBytes) {
        SvgSourceRead out;
        out.error = media::SvgError::TooLarge;
        out.diagnostic = "The file is larger than 32 MiB, the most an SVG import reads.";
        return out;
    }
    auto read = media::parseSvgScene(*bytes, cancel ? media::SvgCancel(cancel) : media::SvgCancel{});
    return finishPrepare(std::move(read), std::move(bytes), path, cancel, phase);
}

AddSvgArtwork::AddSvgArtwork(const document::CompositionId composition, std::shared_ptr<const media::SvgScene> scene,
                             std::filesystem::path path, std::string name, const bool fit,
                             const SvgImportMode mode)
    : composition_(composition), scene_(std::move(scene)), path_(std::move(path)), name_(std::move(name)),
      fit_(fit), mode_(mode) {}

AddSvgArtwork::AddSvgArtwork(const document::CompositionId composition, std::shared_ptr<const PreparedSvgSource> source,
                             std::string name, const SvgImportMode mode, const bool fit)
    : composition_(composition), scene_(source ? source->scene : nullptr), source_(std::move(source)),
      path_(source_ ? source_->path : std::filesystem::path{}), name_(std::move(name)), fit_(fit), mode_(mode) {
    if (source_)
        fonts_ = source_->fonts;
}

OperationResult AddSvgArtwork::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return detail::missingComposition(composition_);
    if (preparationFailure_ && !source_ && !scene_)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "SVG import failed: " + *preparationFailure_);
    // One layer keeps the file's bytes; a caller that gave only a scene has the file read here,
    // as a caller that gave nothing has always had.
    std::shared_ptr<const PreparedSvgSource> source = source_;
    std::shared_ptr<const media::SvgScene> scene = scene_;
    if ((mode_ == SvgImportMode::Layer && (!source || !source->bytes)) || !scene) {
        if (mode_ == SvgImportMode::Layer || !scene) {
            auto read = prepareSvgSource(path_);
            if (!read.source)
                return OperationResult::rejected(OperationIssueCode::InvalidValue, "SVG import failed: " + read.diagnostic);
            source = read.source;
            scene = source->scene;
        }
    }
    auto fonts = fonts_ ? fonts_ : (source ? source->fonts : nullptr);
    const auto name = name_.empty() ? path_.stem().string() : name_;
    document::SvgArtworkOptions options;
    options.width = composition->format().width();
    options.height = composition->format().height();
    options.fit = fit_;
    options.name = name;
    options.layoutOriginX = document::svgArtworkLayoutOrigin(*composition);
    options.fonts = fonts;
    const auto description = document::buildSvgArtwork(*scene, options);
    auto result = mode_ == SvgImportMode::Layer
                      ? writeLayer(draft, *composition, composition_, *source, description, name, fit_, projectDirectory_)
                      : writeEditable(draft, *composition, composition_, *scene, description, name, projectDirectory_);
    if (result.status == OperationStatus::Applied) {
        // The placement is by construction; one validation of the whole composition at the end.
        if (const auto failure = detail::validateGraph(*composition, document::builtInNodeDefinitions()))
            return *failure;
    }
    return result;
}

// ---- #642: Break Apart ----

const document::NodeRecord* findLayerVectorArtwork(const document::Composition& composition,
                                                   const document::LayerId layerId) {
    const auto& graph = composition.graph();
    const auto* layer = graph.findLayer(layerId);
    if (layer == nullptr)
        return nullptr;
    // The edge into the layer's content: its Merge slot's content input, or its Layer node's image.
    const auto reading = [&](const auto& matches) -> const document::EdgeRecord* {
        for (const auto& edge : graph.edges())
            if (matches(edge.destination))
                return &edge;
        return nullptr;
    };
    const document::EdgeRecord* edge = reading([&](const document::InputPortRef& destination) {
        if (layer->entryOwned()) {
            const auto* slot = std::get_if<document::LayerStackInputRef>(&destination);
            return slot != nullptr && slot->stackNodeId == layer->nodeId && slot->slotId == *layer->entrySlot &&
                   slot->role == document::kLayerStackContentInputRole;
        }
        const auto* input = std::get_if<document::NodeInputRef>(&destination);
        return input != nullptr && input->nodeId == layer->nodeId && input->port == document::kLayerOutputContentInputPort;
    });
    for (std::size_t step = 0; edge != nullptr && step <= graph.nodes().size(); ++step) {
        const auto* node = graph.findNode(edge->source.nodeId);
        if (node == nullptr)
            return nullptr;
        if (node->typeId == document::kVectorArtworkNodeType)
            return node;
        // Walk up through the node's first image input (a blur, a colour node, a reroute).
        const auto* definition = document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
        if (definition == nullptr || definition->inputs.empty())
            return nullptr;
        const auto port = definition->inputs.front().name;
        edge = reading([&](const document::InputPortRef& destination) {
            const auto* input = std::get_if<document::NodeInputRef>(&destination);
            return input != nullptr && input->nodeId == node->id && input->port == port;
        });
    }
    return nullptr;
}

std::shared_ptr<const PreparedSvgSource>
prepareVectorArtworkSource(const document::Project& project, const document::CompositionId compositionId,
                           const document::LayerId layer, std::string& diagnostic, const std::function<bool()>& cancel,
                           const std::function<void(SvgPreparePhase)>& phase) {
    const auto* composition = project.findComposition(compositionId);
    if (composition == nullptr) {
        diagnostic = "The composition does not exist";
        return nullptr;
    }
    const auto* node = findLayerVectorArtwork(*composition, layer);
    if (node == nullptr) {
        diagnostic = "The layer does not hold an SVG as one Vector Artwork layer";
        return nullptr;
    }
    const auto* block = document::vectorArtworkBlock(project, *node, composition->parameters());
    if (block == nullptr) {
        diagnostic = "The SVG this layer holds is missing from the project";
        return nullptr;
    }
    const auto& payload = std::get<document::OpaqueExtensionPayload>(block->payload);
    auto bytes = std::make_shared<const std::vector<std::byte>>(payload.bytes().begin(), payload.bytes().end());
    std::filesystem::path path;
    if (const auto* asset = document::vectorArtworkSourceAsset(project, *block))
        path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(asset->locator.path.data()),
                                                   asset->locator.path.size()));
    auto read = prepareSvgSource(std::move(bytes), path, cancel, phase);
    if (!read.source)
        diagnostic = read.diagnostic;
    return read.source;
}

BreakApartSvgArtwork::BreakApartSvgArtwork(const document::CompositionId composition, const document::LayerId layer)
    : composition_(composition), layer_(layer) {}

OperationResult BreakApartSvgArtwork::apply(document::Draft& draft) const {
    auto& project = draft.project();
    auto* composition = project.findComposition(composition_);
    if (composition == nullptr)
        return detail::missingComposition(composition_);
    const auto* layer = composition->graph().findLayer(layer_);
    if (layer == nullptr)
        return detail::missingLayer(composition, composition_, layer_);
    const auto* found = findLayerVectorArtwork(*composition, layer_);
    if (found == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Layer " + std::to_string(layer_.value()) +
                                             " does not hold an SVG as one Vector Artwork layer, so there is nothing to "
                                             "break apart");
    const document::NodeRecord source = *found;
    const auto* block = document::vectorArtworkBlock(project, source, composition->parameters());
    if (block == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The SVG this layer holds is missing from the project");
    // The scene: the prepared one when it is this block's bytes, else the block parsed here.
    std::shared_ptr<const media::SvgScene> scene;
    std::shared_ptr<const platform::FontCatalogue> fonts = fonts_;
    if (prepared_ && prepared_->scene && prepared_->digest == block->provenance.contentDigest) {
        scene = prepared_->scene;
        if (!fonts)
            fonts = prepared_->fonts;
    } else {
        std::string diagnostic;
        scene = document::parseVectorArtworkBlock(*block, diagnostic);
        if (!scene)
            return OperationResult::rejected(OperationIssueCode::InvalidValue, "SVG break apart failed: " + diagnostic);
    }
    try {
        document::SvgArtworkOptions options;
        options.width = composition->format().width();
        options.height = composition->format().height();
        options.fit = document::vectorArtworkFits(source, composition->parameters());
        options.name = document::svgArtworkName(layer->name, "SVG");
        options.layoutOriginX = document::svgArtworkLayoutOrigin(*composition);
        options.fonts = fonts;
        const auto description = document::buildSvgArtwork(*scene, options);
        DraftTarget target(draft, composition_);
        document::SvgArtworkWriter writer(*composition, target, description);
        auto& graph = composition->graph();
        const auto root = writer.addRoot();
        writer.writeSteps();
        writer.writeFrames();
        // Every reader of the node's image reads the artwork's root group instead.
        std::vector<document::EdgeRecord> readers;
        for (const auto& edge : graph.edges())
            if (edge.source.nodeId == source.id)
                readers.push_back(edge);
        for (auto edge : readers) {
            (void)graph.eraseEdge(edge.id);
            edge.source = {root, std::string(document::kLayerStackOutputPort)};
            if (!graph.addEdge(std::move(edge)))
                throw Failure{"The artwork could not be connected where the layer reads it"};
        }
        for (const auto& binding : source.parameters)
            (void)composition->parameters().erase(binding.parameterId);
        (void)graph.eraseNode(source.id);
        if (const auto layout = composition->nodeLayout().find(source.id); layout != composition->nodeLayout().end()) {
            // A muted Vector Artwork node is a muted root.
            if (layout->second.muted)
                composition->nodeLayout()[root].muted = true;
            composition->nodeLayout().erase(source.id);
        }
        if (const auto failure = detail::validateGraph(*composition, document::builtInNodeDefinitions()))
            return *failure;
        auto result = OperationResult::applied({{"layer", layer_}, {"merge", root}});
        appendNotes(result, description.notes);
        return result;
    } catch (const Failure& failure) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue, failure.message);
    } catch (const document::SvgArtworkWriteError& failure) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue, failure.what());
    }
}

} // namespace bloom::commands
