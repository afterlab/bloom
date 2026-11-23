#include "command_test_support.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/chromatic_aberration_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/new_project.hpp>

#include <cstdlib>
#include <string>
#include <utility>

// The presentation-only layer fields: shy, and a name that follows the layer's source.

namespace bloom::commands::test {
namespace {

using document::AssetId;

template <class Operation, class... Args>
CommandResult execute(Document& document, CommandStack& stack, Args&&... args) {
    Transaction transaction("Layer presentation", document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Args>(args)...);
    return stack.execute(std::move(transaction));
}

[[nodiscard]] const document::LayerOutputBoundary&
onlyLayer(const document::Snapshot& snapshot, const document::CompositionId composition,
          const std::size_t index = 0) {
    const auto* found = snapshot.project().findComposition(composition);
    requireFixture(found != nullptr && found->graph().layerOutputs().size() > index,
                   "layer fixture");
    return found->graph().layerOutputs()[index];
}

void testShy(TestContext& test) {
    auto initial = document::makeNewProject("Shy", "Main", core::RationalTime::fromInteger(4));
    const auto composition = initial.initialCompositionId;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    test.expect(execute<AddSolidLayer>(document, stack, composition, "Solid",
                                       core::Color4d{1.0, 0.0, 0.0, 1.0})
                    .changed(),
                "solid fixture");
    const auto before = document.snapshot();
    const auto layer = onlyLayer(before, composition).layerId;
    test.expect(!onlyLayer(before, composition).shy, "a new layer is not shy");
    const auto shy = execute<SetLayerShy>(document, stack, composition, layer, true);
    test.expect(shy.changed() && onlyLayer(document.snapshot(), composition).shy,
                "set-shy marks the layer shy");
    test.expect(!shy.renderAffecting, "shy is presentation and never render-affecting");
    test.expect(execute<SetLayerShy>(document, stack, composition, layer, true).status ==
                    CommandStatus::NoChange,
                "setting the same value is no change");
    auto edited = onlyLayer(document.snapshot(), composition);
    edited.shy = false;
    test.expect(edited == onlyLayer(before, composition), "shy changes nothing else on the layer");
    test.expect(stack.undo().changed() && !onlyLayer(document.snapshot(), composition).shy,
                "undo clears shy");
    test.expect(stack.redo().changed() && onlyLayer(document.snapshot(), composition).shy,
                "redo restores shy");
    test.expect(
        execute<SetLayerShy>(document, stack, composition, document::LayerId::fromRaw(9999), true)
                .status == CommandStatus::Rejected,
        "an unknown layer is refused");
}

void testNameFollowsAsset(TestContext& test) {
    auto initial = document::makeNewProject("Follow", "Main", core::RationalTime::fromInteger(4));
    const auto composition = initial.initialCompositionId;
    document::AssetRecord asset;
    asset.id = AssetId::fromRaw(1);
    asset.locator = {"file", "project-relative", "media/plate.png", "file:///media/plate.png"};
    asset.width = 4;
    asset.height = 3;
    requireFixture(initial.project.addAsset(asset), "asset fixture");
    const auto assetName = initial.project.findAsset(asset.id)->name;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    test.expect(execute<AddImageLayer>(document, stack, composition, asset.id).changed(),
                "image layer fixture");
    const auto& added = onlyLayer(document.snapshot(), composition);
    const auto layer = added.layerId;
    test.expect(added.nameFollowsSource && added.name == assetName && assetName == "plate",
                "a layer made from an asset shows and follows the asset's name");

    const auto renamed = execute<RenameAsset>(document, stack, asset.id, "Hero plate");
    test.expect(renamed.changed() &&
                    onlyLayer(document.snapshot(), composition).name == "Hero plate",
                "renaming the asset renames the following layer");
    test.expect(stack.undo().changed() &&
                    onlyLayer(document.snapshot(), composition).name == "plate",
                "undo restores the asset and layer names together");
    test.expect(stack.redo().changed(), "asset rename redoes");

    test.expect(execute<RenameLayer>(document, stack, composition, layer, "Background").changed(),
                "renaming the layer");
    test.expect(!onlyLayer(document.snapshot(), composition).nameFollowsSource,
                "renaming the layer stops it following its source");
    test.expect(execute<RenameAsset>(document, stack, asset.id, "Final plate").changed() &&
                    onlyLayer(document.snapshot(), composition).name == "Background",
                "a renamed layer keeps its own name when the source is renamed");

    const auto follow =
        execute<SetLayerNameFollowsSource>(document, stack, composition, layer, true);
    test.expect(follow.changed() && !follow.renderAffecting &&
                    onlyLayer(document.snapshot(), composition).nameFollowsSource &&
                    onlyLayer(document.snapshot(), composition).name == "Final plate",
                "following again shows the source's current name");
    test.expect(stack.undo().changed() &&
                    onlyLayer(document.snapshot(), composition).name == "Background" &&
                    !onlyLayer(document.snapshot(), composition).nameFollowsSource,
                "undo restores the authored name and flag");
    test.expect(stack.redo().changed(), "follow redoes");
    test.expect(
        execute<SetLayerNameFollowsSource>(document, stack, composition, layer, false).changed() &&
            onlyLayer(document.snapshot(), composition).name == "Final plate",
        "turning following off keeps the current name");
    test.expect(execute<RenameLayer>(document, stack, composition, layer, "Final plate").status ==
                    CommandStatus::NoChange,
                "renaming a non-following layer to its own name is no change");

    test.expect(execute<AddSolidLayer>(document, stack, composition, "Solid",
                                       core::Color4d{1.0, 1.0, 1.0, 1.0})
                    .changed(),
                "solid fixture");
    const auto solid = onlyLayer(document.snapshot(), composition, 1);
    const auto image = onlyLayer(document.snapshot(), composition, 0);
    const auto& solidLayer = solid.name == "Solid" ? solid : image;
    test.expect(!solidLayer.nameFollowsSource, "a layer without a source never follows");
    test.expect(
        execute<SetLayerNameFollowsSource>(document, stack, composition, solidLayer.layerId, true)
                .status == CommandStatus::Rejected,
        "following is refused for a layer without a source");
}

void testNameFollowsComposition(TestContext& test) {
    auto initial = document::makeNewProject("Nest", "Main", core::RationalTime::fromInteger(4));
    const auto main = initial.initialCompositionId;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto format =
        document::CompositionFormat::create(64, 64).value_or(document::CompositionFormat{});
    const auto added = execute<AddComposition>(document, stack, "Precomp", format,
                                               core::RationalTime::fromInteger(2));
    const auto nested = added.outputId<document::CompositionId>(kAddCompositionOutput);
    test.expect(added.changed() && nested.has_value(), "nested composition fixture");
    if (!nested)
        return;
    test.expect(
        execute<AddCompositionSource>(document, stack, main, *nested, document::Vec2d{}, true)
            .changed(),
        "nested layer fixture");
    const auto& layer = onlyLayer(document.snapshot(), main);
    test.expect(layer.nameFollowsSource && layer.name == "Precomp",
                "a nested composition layer follows its composition's name");
    test.expect(execute<SetCompositionName>(document, stack, *nested, "Titles").changed() &&
                    onlyLayer(document.snapshot(), main).name == "Titles",
                "renaming the composition renames the following layer");
    test.expect(stack.undo().changed() && onlyLayer(document.snapshot(), main).name == "Precomp",
                "undo restores both names");
}

// A finishing chain after the Merge (Merge -> film grain -> chromatic aberration -> Output) does
// not stop a layer from being added, and the chain is left exactly as it was.
void testAddLayerThroughFinishingChain(TestContext& test) {
    auto initial = document::makeNewProject("Chain", "Main", core::RationalTime::fromInteger(4));
    const auto main = initial.initialCompositionId;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto format =
        document::CompositionFormat::create(64, 64).value_or(document::CompositionFormat{});
    const auto nested = execute<AddComposition>(document, stack, "Precomp", format,
                                                core::RationalTime::fromInteger(2))
                            .outputId<document::CompositionId>(kAddCompositionOutput);
    test.expect(nested.has_value(), "nested composition fixture");
    if (!nested)
        return;
    const auto beforeSnapshot = document.snapshot();
    const auto* before = beforeSnapshot.project().findComposition(main);
    const auto merge = before->graph().outputMergeId().value_or(document::NodeId{});
    const auto output = before->graph().compositionOutput().value_or(document::OutputPortRef{});
    const auto grain = execute<AddNode>(document, stack, main,
                                        std::string(document::kFilmGrainNodeType),
                                        document::Vec2d{200, 0})
                           .outputId<document::NodeId>(kAddNodeOutput)
                           .value_or(document::NodeId{});
    const auto aberration = execute<AddNode>(document, stack, main,
                                             std::string(document::kChromaticAberrationNodeType),
                                             document::Vec2d{400, 0})
                                .outputId<document::NodeId>(kAddNodeOutput)
                                .value_or(document::NodeId{});
    const auto disconnected = execute<DisconnectInput>(
        document, stack, main, document::NodeInputRef{output.nodeId, "image"});
    test.expect(disconnected.changed(), "the Output image input is disconnected");
    const auto step = [&](const document::OutputPortRef& from, const document::NodeInputRef& to) {
        return execute<ConnectPorts>(document, stack, main, from, to).changed();
    };
    test.expect(grain.isValid() && aberration.isValid(), "the two effect nodes are added");
    test.expect(step({merge, "image"}, {grain, "input"}), "the Merge feeds the film grain");
    test.expect(step({grain, "image"}, {aberration, "input"}), "the grain feeds the aberration");
    test.expect(step({aberration, "image"}, {output.nodeId, "image"}),
                "the aberration feeds Output");
    const auto chainedSnapshot = document.snapshot();
    const auto* chained = chainedSnapshot.project().findComposition(main);
    test.expect(!chained->graph().outputMergeId().has_value() &&
                    chained->graph().upstreamMergeId() == std::optional(merge),
                "the Merge is found upstream of Output through both effect nodes");

    const auto result =
        execute<AddCompositionSource>(document, stack, main, *nested, document::Vec2d{}, true);
    test.expect(result.changed(), "a layer is added to a composition with a finishing chain");
    const auto afterSnapshot = document.snapshot();
    const auto* after = afterSnapshot.project().findComposition(main);
    const auto feeds = [&](const document::NodeId from, const document::NodeId to) {
        return std::ranges::any_of(after->graph().edges(), [&](const auto& edge) {
            const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            return edge.source.nodeId == from && input != nullptr && input->nodeId == to &&
                   (input->port == "image" || input->port == "input");
        });
    };
    test.expect(after->graph().layerOutputs().size() == 1, "one layer was added");
    test.expect(after->graph().layerStack().nodeId() == merge &&
                    after->graph().layerStack().entries().size() == 1,
                "the layer lands in the Merge");
    test.expect(feeds(merge, grain), "the Merge still feeds the grain");
    test.expect(feeds(grain, aberration), "the grain still feeds the aberration");
    test.expect(feeds(aberration, output.nodeId), "the aberration still feeds Output");

    // No Merge upstream: the error names what was looked for.
    execute<DisconnectInput>(document, stack, main, document::NodeInputRef{grain, "input"});
    const auto refused =
        execute<AddCompositionSource>(document, stack, main, *nested, document::Vec2d{}, true);
    test.expect(refused.status == CommandStatus::Rejected &&
                    !refused.operationFailures.empty() &&
                    refused.operationFailures.front().issue.message.find(
                        "layer Merge upstream of Output") != std::string::npos,
                "with no Merge upstream the refusal names what was searched");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testShy(test);
        bloom::commands::test::testNameFollowsAsset(test);
        bloom::commands::test::testNameFollowsComposition(test);
        bloom::commands::test::testAddLayerThroughFinishingChain(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
