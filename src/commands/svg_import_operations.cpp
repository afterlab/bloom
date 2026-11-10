#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/media/svg_scene.hpp>

#include "node_operation_support.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/svg_artwork.hpp>

#include <algorithm>
#include <cmath>
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

} // namespace

OperationResult AddSvgArtwork::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return detail::missingComposition(composition_);
    std::shared_ptr<const media::SvgScene> scene = scene_;
    if (!scene) {
        auto read = media::readSvgSceneFile(path_);
        if (read.error != media::SvgError::None)
            return OperationResult::rejected(OperationIssueCode::InvalidValue, "SVG import failed: " + read.diagnostic);
        scene = std::move(read.scene);
    }
    const auto name = name_.empty() ? path_.stem().string() : name_;
    document::SvgArtworkOptions options;
    options.width = composition->format().width();
    options.height = composition->format().height();
    options.fit = fit_;
    options.name = name;
    options.layoutOriginX = document::svgArtworkLayoutOrigin(*composition);
    options.fonts = fonts_;
    const auto description = document::buildSvgArtwork(*scene, options);
    auto result = writeEditable(draft, *composition, composition_, *scene, description, name, projectDirectory_);
    if (result.status == OperationStatus::Applied) {
        // The placement is by construction; one validation of the whole composition at the end.
        if (const auto failure = detail::validateGraph(*composition, document::builtInNodeDefinitions()))
            return *failure;
    }
    return result;
}

} // namespace bloom::commands
