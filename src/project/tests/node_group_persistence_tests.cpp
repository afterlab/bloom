// Durable node groups: the round trip through a real archive and the reopen that must return the
// exact same records.

#include "zip_container_test_support.hpp"

#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container.hpp>

#include <array>
#include <iostream>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bloom;
using namespace bloom::project;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

ProjectIoOperationMemory memory(const std::uint64_t budget = 64U << 20U) {
    auto coordinator = ProjectIoMemoryCoordinator::create(budget);
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation(budget, budget);
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

document::ColorSettings neutralColorSettings() {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes(bytes));
}

// A project whose one composition carries two groups: one over both live nodes with a deliberately
// non-default padding, and one with no members at all -- which no command produces, but which the
// model permits and the format therefore has to carry without inventing or dropping anything.
struct Authored final {
    std::unique_ptr<document::Document> document;
    document::CompositionId compositionId;
};

Authored authoredProject() {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    auto owned = std::make_unique<document::Document>(std::move(initial.project));
    const auto snapshot = owned->snapshot();
    auto draft = owned->draft(snapshot);
    auto& composition = *draft.project().findComposition(initial.initialCompositionId);
    const auto first = draft.ids().allocateNodeGroup();
    const auto second = draft.ids().allocateNodeGroup();
    if (!first || !second)
        throw std::runtime_error("group ids");
    std::set<document::NodeId> members;
    for (const auto& node : composition.graph().nodes())
        members.insert(node.id);
    // The first frame carries a colour, the second none.
    composition.nodeGroups()[*first] = {
        *first, "Lighting", members, {48.0, 12.5}, std::array<std::uint8_t, 3>{217, 124, 124}};
    composition.nodeGroups()[*second] = {*second, "Spare", {}, {}};
    if (!owned->commit(snapshot.revision(), std::move(draft)).committed())
        throw std::runtime_error("group commit");
    return {std::move(owned), initial.initialCompositionId};
}

std::vector<std::byte> archiveOf(const document::Snapshot& snapshot,
                                 const document::ColorSettings& settings) {
    const CanonicalManifestV1 manifest;
    const CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &settings};
    auto saved = buildVerifiedSaveArchive(manifest, input, {}, memory());
    if (!saved || !saved.archive()) {
        const auto* failure = saved.failure();
        if (failure) {
            std::cerr << "archive stage " << static_cast<int>(failure->stage()) << '\n';
            if (const auto* decoded = failure->payloadAs<SaveArchiveDocumentDecodeFailure>())
                std::cerr << "decode " << static_cast<int>(decoded->error) << " "
                          << decoded->path.view() << '\n';
            if (const auto* reconstructed = failure->payloadAs<ReconstructionRejected>())
                std::cerr << "reconstruct " << static_cast<int>(reconstructed->stage) << '\n';
        }
        throw std::runtime_error("group archive");
    }
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

void roundTripAndReopen() {
    const auto authored = authoredProject();
    const auto settings = neutralColorSettings();
    auto snapshot = authored.document->snapshot();
    const auto& original = snapshot.project().findComposition(authored.compositionId)->nodeGroups();
    expect(original.size() == 2, "the fixture authored two groups");

    const auto archive = archiveOf(snapshot, settings);
    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened, "a grouped project opens");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(openedResult).takeOpened();
    expect(opened.schemaMinor == kCanonicalDocumentSchemaVersionV1.minor && !opened.roundTrip,
           "a grouped project is written and read as the current schema minor");
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(authored.compositionId);
    expect(composition != nullptr, "the reopened project keeps its composition");
    if (composition == nullptr)
        return;
    expect(composition->nodeGroups() == original,
           "every group id, name, member, padding component and colour round-trips exactly");
    expect(composition->nodeGroups().begin()->second.color ==
                   std::optional(std::array<std::uint8_t, 3>{217, 124, 124}) &&
               !std::next(composition->nodeGroups().begin())->second.color,
           "a frame colour survives save and reopen, and a neutral frame stays neutral");
    expect(reopened.ids().highWater().nodeGroup == snapshot.ids().highWater().nodeGroup &&
               reopened.ids().highWater().nodeGroup == 2,
           "the group allocator high water is durable");
    expect(reopened.ids().highWater().node == snapshot.ids().highWater().node,
           "groups never advance the node watermark");

    // Byte determinism: the second write of the reopened document must be the first write again.
    expect(archiveOf(reopened, opened.colorSettings) == archive,
           "a reopened grouped document re-saves byte-identically");
}

void multipleMergeRoundTrip() {
    auto authored = authoredProject();
    auto snapshot = authored.document->snapshot();
    auto draft = authored.document->draft(snapshot);
    auto& graph = draft.project().findComposition(authored.compositionId)->graph();
    const auto nestedId = draft.ids().allocateNode();
    const auto slotId = draft.ids().allocateLayerSlot();
    const auto edgeId = draft.ids().allocateEdge();
    if (!nestedId || !slotId || !edgeId)
        throw std::logic_error("Merge ids");
    const auto nested = *nestedId;
    const auto slot = *slotId;
    const auto edge = *edgeId;
    expect(graph.addNode({nested,
                          std::string(document::kLayerStackNodeType),
                          {},
                          document::kLayerStackNodeSchemaVersion}),
           "add nested Merge");
    graph.merge(nested)->setEnabled(false);
    expect(graph.layerStack().append({slot, {}}) &&
               graph.addEdge(
                   {edge,
                    {nested, "image"},
                    document::LayerStackInputRef{graph.layerStack().nodeId(), slot, "content"}}),
           "connect nested Merge");
    expect(authored.document->commit(snapshot.revision(), std::move(draft)).committed(),
           "publish nested Merge");
    snapshot = authored.document->snapshot();
    const auto bytes = archiveOf(snapshot, neutralColorSettings());
    auto result = openProjectArchive(bytes, {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "multiple Merges reopen");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto restored = opened.document->snapshot();
    const auto& restoredGraph = restored.project().findComposition(authored.compositionId)->graph();
    expect(restoredGraph.merges().size() == 2 && restoredGraph.merge(nested) &&
               !restoredGraph.merge(nested)->enabled() && restoredGraph.layerStack().find(slot),
           "Merge ownership and plain-image slot survive");
    expect(archiveOf(restored, opened.colorSettings) == bytes,
           "multiple Merge archive is byte stable");
}

// #716: a composition whose Output node was deleted, and one whose Merge went too, save and reopen
// as they are: no output endpoint, and no layer stack.
void compositionWithoutOutputRoundTrips() {
    for (const bool alsoMerge : {false, true}) {
        auto authored = authoredProject();
        auto snapshot = authored.document->snapshot();
        auto draft = authored.document->draft(snapshot);
        auto& composition = *draft.project().findComposition(authored.compositionId);
        auto& graph = composition.graph();
        const auto output = graph.compositionOutput();
        const auto merge = graph.outputMergeId();
        if (!output || !merge)
            throw std::logic_error("a new composition has an Output and its Merge");
        for (auto& [id, group] : composition.nodeGroups())
            group.members.clear();
        expect(graph.eraseNode(output->nodeId), "the Output node is removed");
        composition.nodeLayout().erase(output->nodeId);
        if (alsoMerge) {
            expect(graph.eraseNode(*merge), "the Merge is removed");
            composition.nodeLayout().erase(*merge);
        }
        expect(!graph.compositionOutput().has_value(), "the composition has no output endpoint");
        expect(authored.document->commit(snapshot.revision(), std::move(draft)).committed(),
               "a composition without an Output is a valid document");
        snapshot = authored.document->snapshot();
        const auto bytes = archiveOf(snapshot, neutralColorSettings());
        auto result = openProjectArchive(bytes, {}, memory());
        expect(result.outcome() == OpenArchiveOutcome::Opened,
               "a composition without an Output reopens");
        if (result.outcome() != OpenArchiveOutcome::Opened)
            continue;
        auto opened = std::move(result).takeOpened();
        const auto restored = opened.document->snapshot();
        const auto& restoredGraph =
            restored.project().findComposition(authored.compositionId)->graph();
        expect(!restoredGraph.compositionOutput().has_value() &&
                   restoredGraph.nodes().size() == (alsoMerge ? 0U : 1U) &&
                   restoredGraph.merges().size() == (alsoMerge ? 0U : 1U),
               "the reopened composition has no output endpoint and the same nodes");
        expect(archiveOf(restored, opened.colorSettings) == bytes,
               "a composition without an Output re-saves byte-identically");
    }
}
} // namespace

int main() {
    try {
        multipleMergeRoundTrip();
        compositionWithoutOutputRoundTrips();
        roundTripAndReopen();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
