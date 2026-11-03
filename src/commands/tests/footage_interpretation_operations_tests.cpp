// Interpret Footage (#381): SetAssetInterpretation applies one change to a multi-selection as one
// undo entry, skips members a kind does not have, refuses bad values, and refits layers whose out
// point ended with the footage.
#include "command_test_support.hpp"
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/footage_interpretation_operations.hpp>
#include <bloom/document/new_project.hpp>

namespace bloom::commands::test {
namespace {
using document::AssetId;

document::AssetRecord video(const AssetId id) {
    document::AssetRecord asset;
    asset.id = id;
    asset.kind = document::AssetKind::Video;
    asset.locator = {"file", "project-relative", "clip.mov", "file:///clip.mov"};
    asset.width = 64;
    asset.height = 48;
    asset.frames = 48;
    asset.duration = core::RationalTime::fromInteger(2);
    asset.name = "clip";
    document::AssetVideoStream stream;
    stream.codec = "h264";
    stream.timebase = *core::RationalTime::create(1, 24);
    stream.framePeriod = *core::RationalTime::create(1, 24);
    stream.duration = core::RationalTime::fromInteger(2);
    stream.width = 64;
    stream.height = 48;
    asset.videoStreams = {stream};
    return asset;
}

document::AssetRecord sequence(const AssetId id) {
    document::AssetRecord asset;
    asset.id = id;
    asset.kind = document::AssetKind::Sequence;
    asset.locator = {"file", "project-relative", "shot.0001.png", "file:///shot.0001.png"};
    asset.width = 4;
    asset.height = 4;
    asset.name = "shot";
    asset.manifest.pattern = "shot.####.png";
    asset.manifest.padding = 4;
    asset.manifest.first = 1;
    asset.manifest.last = 12;
    for (std::int64_t frame = 1; frame <= 12; ++frame)
        asset.manifest.members.push_back(
            {frame,
             {"file", "project-relative", "shot." + std::to_string(frame) + ".png",
              "file:///shot.png"},
             bloom::core::Sha256Digest{},
             std::nullopt});
    return asset;
}

document::AssetRecord still(const AssetId id) {
    document::AssetRecord asset;
    asset.id = id;
    asset.kind = document::AssetKind::Image;
    asset.locator = {"file", "project-relative", "logo.png", "file:///logo.png"};
    asset.width = 4;
    asset.height = 4;
    asset.name = "logo";
    return asset;
}

CommandResult apply(Document& document, CommandStack& stack, std::vector<AssetId> assets,
                    AssetInterpretationChange change) {
    Transaction transaction("Interpret Footage", document.snapshot().revision());
    transaction.emplace<SetAssetInterpretation>(std::move(assets), std::move(change));
    return stack.execute(std::move(transaction));
}

void testMultiSelection(TestContext& test) {
    auto initial =
        document::makeNewProject("Interpret", "Main", core::RationalTime::fromInteger(10));
    const auto composition = initial.initialCompositionId;
    const auto clip = AssetId::fromRaw(1), shot = AssetId::fromRaw(2), logo = AssetId::fromRaw(3);
    requireFixture(initial.project.addAsset(video(clip)), "video");
    requireFixture(initial.project.addAsset(sequence(shot)), "sequence");
    requireFixture(initial.project.addAsset(still(logo)), "still");
    Document document(std::move(initial.project));
    CommandStack stack(document);
    {
        Transaction transaction("Add", document.snapshot().revision());
        transaction.emplace<AddImageLayer>(composition, clip);
        requireFixture(stack.execute(std::move(transaction)).changed(), "video layer");
    }
    const auto layerOut = [&] {
        return document.snapshot()
            .project()
            .findComposition(composition)
            ->graph()
            .layerOutputs()
            .front()
            .outPoint;
    };
    test.expect(layerOut() == core::RationalTime::fromInteger(2),
                "a 48-frame 24 fps clip makes a 2 s layer");
    const auto before = stack.size();
    AssetInterpretationChange change;
    change.frameRate = document::FrameRate::create(12, 1);
    change.alphaMode = document::AssetAlphaMode::Ignore;
    change.missingFrames = document::AssetMissingFrames::Nothing;
    test.expect(apply(document, stack, {clip, shot, logo}, change).changed(),
                "one change applies to a mixed selection");
    test.expect(stack.size() == before + 1, "the whole selection is one undo entry");
    const auto& project = document.snapshot().project();
    const auto& clipValue = project.findAsset(clip)->interpretation;
    const auto& shotValue = project.findAsset(shot)->interpretation;
    const auto& logoValue = project.findAsset(logo)->interpretation;
    test.expect(clipValue.frameRate == document::FrameRate::create(12, 1) &&
                    clipValue.alphaMode == document::AssetAlphaMode::Ignore &&
                    clipValue.missingFrames == document::AssetMissingFrames::HoldPrevious,
                "the video takes the rate and alpha but has no missing frames");
    test.expect(shotValue.frameRate == document::FrameRate::create(12, 1) &&
                    shotValue.missingFrames == document::AssetMissingFrames::Nothing,
                "the sequence takes the rate and the missing-frame behavior");
    test.expect(!logoValue.frameRate && logoValue.alphaMode == document::AssetAlphaMode::Ignore,
                "the still takes only the alpha");
    test.expect(layerOut() == core::RationalTime::fromInteger(4),
                "a layer that ended with its footage follows the conformed (longer) length");
    test.expect(stack.undo().changed() &&
                    !document.snapshot().project().findAsset(clip)->interpretation.frameRate &&
                    layerOut() == core::RationalTime::fromInteger(2),
                "one undo restores every asset and the layer");
    test.expect(stack.redo().changed(), "redo");

    AssetInterpretationChange loop;
    loop.loopCount = 3;
    loop.frameRate = std::optional<document::FrameRate>{};
    test.expect(apply(document, stack, {clip}, loop).changed() &&
                    layerOut() == core::RationalTime::fromInteger(6),
                "back at 24 fps, Loop 3 makes the 2 s clip a 6 s layer");

    AssetInterpretationChange bad;
    bad.loopCount = 0;
    test.expect(apply(document, stack, {clip}, bad).status == CommandStatus::Rejected,
                "a zero loop count is refused");
    bad = {};
    bad.frameRate = document::FrameRate::create(5000, 1);
    test.expect(apply(document, stack, {shot}, bad).status == CommandStatus::Rejected,
                "a frame rate above 1000 fps is refused");
    bad = {};
    bad.matteColor = std::array<double, 3>{2.0, 0.0, 0.0};
    test.expect(apply(document, stack, {logo}, bad).status == CommandStatus::Rejected,
                "a matte color outside 0 to 1 is refused");
    test.expect(apply(document, stack, {AssetId::fromRaw(99)}, change).status ==
                    CommandStatus::Rejected,
                "a missing asset is refused");
    test.expect(apply(document, stack, {clip}, {}).status == CommandStatus::Rejected,
                "an empty change is refused");
}

void testPasteEverything(TestContext& test) {
    auto initial = document::makeNewProject("Paste", "Main", core::RationalTime::fromInteger(4));
    const auto a = AssetId::fromRaw(1), b = AssetId::fromRaw(2);
    auto source = sequence(a);
    source.interpretation.frameRate = document::FrameRate::create(24000, 1001);
    source.interpretation.loopCount = 2;
    source.interpretation.pixelAspect = core::PixelAspectRatio::create(2, 1);
    source.interpretation.inputColorSpaceId = "ACEScg";
    requireFixture(initial.project.addAsset(source), "source");
    requireFixture(initial.project.addAsset(sequence(b)), "target");
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto copied = AssetInterpretationChange::everything(
        document.snapshot().project().findAsset(a)->interpretation);
    test.expect(apply(document, stack, {b}, copied).changed() &&
                    document.snapshot().project().findAsset(b)->interpretation ==
                        document.snapshot().project().findAsset(a)->interpretation,
                "Paste Interpretation copies every member");
    test.expect(apply(document, stack, {b}, copied).status == CommandStatus::NoChange,
                "pasting the same interpretation changes nothing");
}
} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testMultiSelection(test);
        bloom::commands::test::testPasteEverything(test);
    } catch (const std::exception& error) {
        test.fail(error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
