// Import options and preferences (#381): ImportAssets imports numbered images as a sequence or as
// stills, reads only a frame range, skips missing frames and writes the Import preferences into
// new assets; Relink and Reload keep a still a still and a sequence its range; a still with a
// duration makes a layer that ends there and a composition that long.
#include "command_test_support.hpp"
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/document/new_project.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bloom::commands::test {
namespace {
using document::AssetKind;

// A 1x1 RGBA PNG; every frame of the fixtures has these bytes.
constexpr std::array<unsigned char, 70> kPixel{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0x64, 0x60, 0xf8, 0x5f, 0x0f, 0x00, 0x02, 0x87, 0x01, 0x80, 0xeb, 0x47,
    0xba, 0x92, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

void writePixel(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(kPixel.data()), kPixel.size());
}

struct Fixture final {
    std::filesystem::path directory = std::filesystem::current_path() / "import-options-fixtures";
    Fixture() {
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        // Frames 1, 2, 4 and 5: frame 3 is missing.
        for (const auto frame : {1, 2, 4, 5})
            writePixel(frame_(frame));
    }
    ~Fixture() { std::filesystem::remove_all(directory); }
    [[nodiscard]] std::filesystem::path frame_(const int frame) const {
        return directory /
               ("shot." + std::string(frame < 10 ? "000" : "00") + std::to_string(frame) + ".png");
    }
};

const document::AssetRecord& only(const Document& document) {
    const auto& assets = document.snapshot().project().assets();
    requireFixture(!assets.empty(), "an imported asset");
    return assets.front();
}

CommandResult run(Document& document, CommandStack& stack, std::unique_ptr<Operation> operation) {
    Transaction transaction("Import", document.snapshot().revision());
    requireFixture(transaction.add(std::move(operation)), "operation");
    return stack.execute(std::move(transaction));
}

void testImportOptions(TestContext& test) {
    const Fixture fixture;
    const auto rate = *document::FrameRate::create(24000, 1001);
    const auto five = core::RationalTime::fromInteger(5);
    {
        // Single images: each numbered file is its own still with the still duration.
        auto seed = document::makeNewProject("Stills", "Main", core::RationalTime::fromInteger(10));
        Document document(std::move(seed.project));
        CommandStack stack(document);
        ImportOptions options;
        options.sequences = false;
        options.stillDuration = five;
        options.sequenceFrameRate = rate;
        const auto result =
            run(document, stack,
                std::make_unique<ImportAssets>(std::vector{fixture.frame_(1), fixture.frame_(2)},
                                               fixture.directory, std::function<bool()>{},
                                               ImportProgressCallback{}, options));
        const auto& assets = document.snapshot().project().assets();
        test.expect(result.changed() && assets.size() == 2 && assets[0].kind == AssetKind::Image &&
                        assets[1].kind == AssetKind::Image &&
                        assets[0].interpretation.stillDuration == five &&
                        !assets[0].interpretation.frameRate,
                    "single images import each file as a still with the still duration");
    }
    {
        // A sequence with the preference rate that skips its missing frame.
        auto seed = document::makeNewProject("Skip", "Main", core::RationalTime::fromInteger(10));
        Document document(std::move(seed.project));
        CommandStack stack(document);
        ImportOptions options;
        options.skipMissingFrames = true;
        options.sequenceFrameRate = rate;
        options.stillDuration = five;
        (void)run(document, stack,
                  std::make_unique<ImportAssets>(std::vector{fixture.frame_(1)}, fixture.directory,
                                                 std::function<bool()>{}, ImportProgressCallback{},
                                                 options));
        const auto& asset = only(document);
        test.expect(asset.kind == AssetKind::Sequence &&
                        asset.interpretation.missingFrames == document::AssetMissingFrames::Skip &&
                        asset.interpretation.frameRate == rate &&
                        !asset.interpretation.stillDuration && !asset.manifest.range,
                    "a sequence takes the rate and Skip, not the still duration");
        test.expect(document::footageFrameCount(asset) == 4 &&
                        document::footageSequenceFrame(asset, 2).member->frame == 4,
                    "the skipping sequence plays its four frames back to back");
    }
    {
        // A frame range reads only its frames and is kept on the asset.
        auto seed = document::makeNewProject("Range", "Main", core::RationalTime::fromInteger(10));
        Document document(std::move(seed.project));
        CommandStack stack(document);
        ImportOptions options;
        options.frameRange = document::AssetSequenceRange{2, 4};
        (void)run(document, stack,
                  std::make_unique<ImportAssets>(std::vector{fixture.frame_(1)}, fixture.directory,
                                                 std::function<bool()>{}, ImportProgressCallback{},
                                                 options));
        const auto asset = only(document);
        test.expect(asset.kind == AssetKind::Sequence && asset.manifest.members.size() == 2 &&
                        asset.manifest.first == 2 && asset.manifest.last == 4 &&
                        asset.manifest.range == options.frameRange &&
                        asset.manifest.gaps == std::vector<std::int64_t>{3},
                    "a frame range imports frames 2 and 4 and keeps the range");
        test.expect(!asset.interpretation.frameRate,
                    "without a preference rate the sequence matches the composition");

        // Frame 3 appears and 6 is rendered: Reload reads the range again, so 3 joins and 6 does
        // not, with or without the relink options (an agent's relink has none).
        writePixel(fixture.frame_(3));
        writePixel(fixture.directory / "shot.0006.png");
        for (const bool withOptions : {true, false}) {
            const auto relink = withOptions ? relinkOptions(asset) : ImportOptions{};
            const auto relinked =
                run(document, stack,
                    std::make_unique<RelinkAsset>(asset.id, fixture.frame_(2), fixture.directory,
                                                  std::function<bool()>{}, relink));
            const auto& after = only(document);
            test.expect(after.manifest.members.size() == 3 && after.manifest.first == 2 &&
                            after.manifest.last == 4 && after.manifest.gaps.empty() &&
                            after.manifest.range == asset.manifest.range,
                        withOptions ? "Reload keeps the imported frame range"
                                    : "a relink without options keeps the frame range too");
            if (relinked.changed())
                (void)stack.undo();
        }
        std::filesystem::remove(fixture.frame_(3));
        std::filesystem::remove(fixture.directory / "shot.0006.png");
    }
    {
        // A range holding one frame imports it as a still; a range holding none is refused.
        auto seed = document::makeNewProject("One", "Main", core::RationalTime::fromInteger(10));
        Document document(std::move(seed.project));
        CommandStack stack(document);
        ImportOptions options;
        options.frameRange = document::AssetSequenceRange{4, 4};
        (void)run(document, stack,
                  std::make_unique<ImportAssets>(std::vector{fixture.frame_(1)}, fixture.directory,
                                                 std::function<bool()>{}, ImportProgressCallback{},
                                                 options));
        const auto& asset = only(document);
        test.expect(asset.kind == AssetKind::Image && asset.locator.path.ends_with("shot.0004.png"),
                    "a one-frame range imports that frame as a still");
        options.frameRange = document::AssetSequenceRange{3, 3};
        const ImportAssets none({fixture.frame_(1)}, fixture.directory, {}, {}, options);
        test.expect(none.prepared().empty() && none.failures().size() == 1 &&
                        none.failures().front().diagnostic == "No frames in the frame range",
                    "a range without frames says so");
    }
    {
        // A still relinked to a numbered file stays that one still.
        auto seed = document::makeNewProject("Still", "Main", core::RationalTime::fromInteger(10));
        Document document(std::move(seed.project));
        CommandStack stack(document);
        ImportOptions single;
        single.sequences = false;
        (void)run(document, stack,
                  std::make_unique<ImportAssets>(std::vector{fixture.frame_(1)}, fixture.directory,
                                                 std::function<bool()>{}, ImportProgressCallback{},
                                                 single));
        const auto id = only(document).id;
        (void)run(document, stack,
                  std::make_unique<RelinkAsset>(id, fixture.frame_(5), fixture.directory));
        const auto& after = only(document);
        test.expect(after.kind == AssetKind::Image &&
                        after.locator.path.ends_with("shot.0005.png") &&
                        after.manifest.members.empty(),
                    "a still relinked to a numbered file stays a still");
    }
}

void testStillDuration(TestContext& test) {
    auto seed = document::makeNewProject("Still", "Main", core::RationalTime::fromInteger(10));
    const auto composition = seed.initialCompositionId;
    document::AssetRecord logo;
    logo.id = document::AssetId::fromRaw(1);
    logo.kind = AssetKind::Image;
    logo.locator = {"file", "project-relative", "logo.png", "file:///logo.png"};
    logo.width = 4;
    logo.height = 4;
    logo.name = "logo";
    logo.interpretation.stillDuration = *core::RationalTime::create(5, 2);
    auto plain = logo;
    plain.id = document::AssetId::fromRaw(2);
    plain.name = "plain";
    plain.interpretation.stillDuration.reset();
    requireFixture(seed.project.addAsset(logo) && seed.project.addAsset(plain), "stills");
    Document document(std::move(seed.project));
    CommandStack stack(document);
    (void)run(document, stack, std::make_unique<AddImageLayer>(composition, logo.id));
    (void)run(document, stack, std::make_unique<AddImageLayer>(composition, plain.id));
    const auto* made = document.snapshot().project().findComposition(composition);
    std::vector<core::RationalTime> outs;
    for (const auto& layer : made->graph().layerOutputs())
        outs.push_back(layer.outPoint);
    test.expect(outs.size() == 2 &&
                    std::ranges::count(outs, *core::RationalTime::create(5, 2)) == 1 &&
                    std::ranges::count(outs, core::RationalTime{}) == 1,
                "a still with a duration ends there; one without spans the composition");
    CompositionSettings base;
    base.duration = core::RationalTime::fromInteger(10);
    test.expect(compositionSettingsForAsset(logo, base).duration ==
                        *core::RationalTime::create(5, 2) &&
                    compositionSettingsForAsset(plain, base).duration == base.duration,
                "New Composition from a still is as long as the still");
}
// #432: importing a file the project already holds adds nothing and names the asset that holds
// it, as bloom.asset.ensure-font does for a face, so a caller always gets one asset id per file.
void testReimportReturnsExistingAsset(TestContext& test) {
    const Fixture fixture;
    auto seed = document::makeNewProject("Again", "Main", core::RationalTime::fromInteger(10));
    Document document(std::move(seed.project));
    CommandStack stack(document);
    ImportOptions options;
    options.sequences = false;
    const auto import = [&](std::vector<std::filesystem::path> files) {
        return run(document, stack,
                   std::make_unique<ImportAssets>(std::move(files), fixture.directory,
                                                  std::function<bool()>{}, ImportProgressCallback{},
                                                  options));
    };
    const auto assetOutputs = [](const CommandResult& result) {
        std::vector<document::AssetId> ids;
        for (const auto& output : result.outputs)
            if (const auto* id = std::get_if<document::AssetId>(&output.output.id);
                id != nullptr && output.output.name == "asset")
                ids.push_back(*id);
        return ids;
    };
    const auto first = import({fixture.frame_(1)});
    const auto firstIds = assetOutputs(first);
    test.expect(first.changed() && firstIds.size() == 1, "the first import adds one asset");
    const auto history = stack.size();

    const auto again = import({fixture.frame_(1)});
    test.expect(again.status == CommandStatus::NoChange && assetOutputs(again) == firstIds &&
                    document.snapshot().project().assets().size() == 1 && stack.size() == history,
                "re-importing the same file adds nothing and returns the existing asset id");

    const auto mixed = import({fixture.frame_(1), fixture.frame_(2)});
    const auto mixedIds = assetOutputs(mixed);
    test.expect(mixed.changed() && mixedIds.size() == 2 && mixedIds.front() == firstIds.front() &&
                    mixedIds.back() != firstIds.front() &&
                    document.snapshot().project().assets().size() == 2,
                "a batch names the held asset and the new one, one id per file in order");
}
} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testImportOptions(test);
        bloom::commands::test::testStillDuration(test);
        bloom::commands::test::testReimportReturnsExistingAsset(test);
    } catch (const std::exception& error) {
        test.fail(error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
