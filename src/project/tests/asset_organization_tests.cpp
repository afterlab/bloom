#include "zip_container_test_support.hpp"
#include <array>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <iostream>
#include <stdexcept>

namespace {

using namespace bloom;
using namespace bloom::project;
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) {
        ++failures;
        std::cerr << message << '\n';
    }
}
ProjectIoOperationMemory memory() {
    auto coordinator = ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
document::AssetRecord plate() {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(7);
    asset.locator = {"file", "project-relative", "media/shot.v02.exr",
                     "file:///media/shot.v02.exr"};
    asset.width = 2;
    asset.height = 3;
    asset.name = "Plate";
    return asset;
}
void roundTripAndValidation() {
    auto initial = document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
    const auto root = document::AssetFolderId::fromRaw(3);
    const auto child = document::AssetFolderId::fromRaw(8);
    expect(initial.project.addAssetFolder({child, "Plates", root}),
           "child record accepts later parent declaration");
    expect(initial.project.addAssetFolder({root, "Shots", {}}), "root folder added");
    auto asset = plate();
    asset.folder = child;
    asset.tags = {"approved", "hero"};
    asset.order = 42;
    asset.width = 6000;
    asset.height = 4000;
    expect(initial.project.addAsset(asset), "organized asset added");
    auto lut = asset;
    lut.id = document::AssetId::fromRaw(78);
    lut.kind = document::AssetKind::Lut;
    lut.locator.path = "looks/show.cube";
    lut.name = "Show LUT";
    lut.width = lut.height = 0;
    expect(initial.project.addAsset(lut), "LUT asset is accepted");
    auto sequence = plate();
    sequence.id = document::AssetId::fromRaw(79);
    sequence.kind = document::AssetKind::Sequence;
    sequence.locator = {"file", "project-relative", "media/wide.0001.exr",
                        "file:///media/wide.0001.exr"};
    sequence.width = 20000;
    sequence.height = 2;
    sequence.manifest.pattern = "wide.####.exr";
    sequence.manifest.padding = 4;
    sequence.manifest.first = 1;
    sequence.manifest.last = 2;
    auto second = sequence.locator;
    second.path = "media/wide.0002.exr";
    second.relinkHint = "file:///media/wide.0002.exr";
    sequence.manifest.members = {{1, sequence.locator, bloom::core::Sha256Digest{}, std::nullopt},
                                 {2, second, bloom::core::Sha256Digest{}, std::nullopt}};
    expect(initial.project.addAsset(sequence), "wide EXR sequence metadata added");
    document::Document document(std::move(initial.project));
    auto snapshot = document.snapshot();
    expect(snapshot.ids().highWater().assetFolder == 8,
           "folder namespace participates in allocator inventory");
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = buildVerifiedSaveArchive({}, {.snapshot = &snapshot, .colorSettings = &settings},
                                            {}, memory());
    expect(static_cast<bool>(archive), "organized project saves at 1.0");
    if (!archive)
        return;
    auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "organized project opens");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    auto restored = opened.document->snapshot();
    expect(opened.schemaMinor == 0 && *restored.project().findAsset(asset.id) == asset,
           "name, tags, folder, order and media identity round-trip at 1.0");
    expect(restored.project().findAsset(lut.id) && *restored.project().findAsset(lut.id) == lut,
           "LUT locator, kind and digest round-trip at 1.0");
    expect(restored.project().findAsset(sequence.id) &&
               *restored.project().findAsset(sequence.id) == sequence,
           "wide EXR sequence metadata round-trips without pixel allocation");
    expect(restored.project().assetFolders().size() == 2 &&
               *restored.project().findAssetFolder(child) ==
                   *snapshot.project().findAssetFolder(child),
           "folder hierarchy round-trips");
    auto draft = document.draft(snapshot);
    draft.project().findAssetFolder(root)->parent = child;
    expect(!draft.validate().ok(), "folder cycle rejected");
    expect(!document.commit(snapshot.revision(), std::move(draft)).committed(),
           "cycle cannot publish");
    draft = document.draft(snapshot);
    draft.project().findAsset(asset.id)->folder = document::AssetFolderId::fromRaw(999);
    expect(!draft.validate().ok(), "dangling asset folder rejected");
    draft = document.draft(snapshot);
    draft.project().findAssetFolder(root)->parent = document::AssetFolderId::fromRaw(999);
    expect(!draft.validate().ok(), "dangling parent rejected");
    draft = document.draft(snapshot);
    expect(draft.project().addAssetFolder({document::AssetFolderId::fromRaw(9), "Plates", root}),
           "duplicate name fixture");
    expect(!draft.validate().ok(), "duplicate sibling folder names rejected");
    draft = document.draft(snapshot);
    draft.project().findAsset(asset.id)->tags = {"hero", "approved"};
    expect(!draft.validate().ok(), "unsorted tags rejected");
    draft.project().findAsset(asset.id)->tags = {"hero", "hero"};
    expect(!draft.validate().ok(), "duplicate tags rejected");
}
void lexicalNames() {
    auto asset = plate();
    for (const auto& [path, expected] :
         std::vector<std::pair<std::string, std::string>>{{"media/shot.v02.exr", "shot.v02"},
                                                          {"media/.hidden", ".hidden"},
                                                          {"C:\\media\\frame.png", "frame"},
                                                          {"media/..", ".."},
                                                          {"media/élan.png", "élan"}}) {
        asset.locator.path = path;
        expect(document::defaultAssetName(asset) == expected, "portable lexical file-stem name");
    }
    asset.locator.path.clear();
    asset.fontFamily = "Embedded face";
    expect(document::defaultAssetName(asset) == "Embedded face",
           "font family fallback for empty locator");
    asset.kind = document::AssetKind::Font;
    asset.locator = {"font", "builtin", "embedded/0", "font:fixture"};
    asset.fontStyle = "Book";
    expect(document::defaultAssetName(asset) == "Embedded face Book",
           "builtin font has no file stem and retains its captured face label");
}
} // namespace
// #378: a composition may sit in an Assets folder. The member is written only when set and
// round-trips through save and open.
std::string encodeSnapshot(const document::Snapshot& snapshot) {
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("composition folder canonical size");
    std::string text(*size.value(), '\0');
    if (!encodeCanonicalDocument(input, text))
        throw std::runtime_error("composition folder canonical encode");
    return text;
}
void compositionFolders() {
    auto initial = document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
    const auto folder = document::AssetFolderId::fromRaw(5);
    expect(initial.project.addAssetFolder({folder, "Comps", {}}), "composition folder added");
    const auto compositionId = initial.project.compositions().front().id();
    document::Document document(std::move(initial.project));
    auto rootSnapshot = document.snapshot();
    const auto rootText = encodeSnapshot(rootSnapshot);
    expect(rootText.find("\"folder\"") == std::string::npos,
           "a root composition writes no folder member");

    auto draft = document.draft(rootSnapshot);
    draft.project().findComposition(compositionId)->setFolder(folder);
    expect(draft.validate().ok(), "a composition in an existing folder validates");
    expect(document.commit(rootSnapshot.revision(), std::move(draft)).committed(),
           "the foldered composition publishes");
    auto snapshot = document.snapshot();
    const auto text = encodeSnapshot(snapshot);
    expect(text.find("\"folder\": \"5\"") != std::string::npos,
           "the composition writes its folder id");

    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = buildVerifiedSaveArchive({}, {.snapshot = &snapshot, .colorSettings = &settings},
                                            {}, memory());
    expect(static_cast<bool>(archive), "a foldered composition saves");
    if (archive) {
        auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
        expect(result.outcome() == OpenArchiveOutcome::Opened, "the foldered project opens");
        if (result.outcome() == OpenArchiveOutcome::Opened) {
            auto opened = std::move(result).takeOpened();
            const auto* restored =
                opened.document->snapshot().project().findComposition(compositionId);
            expect(opened.schemaMinor == 0 && restored && restored->folder() == folder,
                   "the composition folder round-trips at 1.0");
        }
    }

    draft = document.draft(snapshot);
    draft.project().findComposition(compositionId)->setFolder(document::AssetFolderId::fromRaw(99));
    expect(!draft.validate().ok(), "a dangling composition folder is rejected");
    draft = document.draft(snapshot);
    expect(draft.project().removeAssetFolder(folder), "fixture folder removal");
    expect(!draft.validate().ok(), "removing a folder that still holds a composition is rejected");
}

int main() {
    try {
        roundTripAndValidation();
        compositionFolders();
        lexicalNames();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
