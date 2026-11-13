// #609: "OCIO File Transform" is named Apply LUT and "OCIO Color Space Transform" is named Color
// Space Transform. Only the names changed: a project saved with the two nodes (the checked-in
// tests/fixtures/color/ocio-nodes.bloom) still opens, validates, holds both nodes under their
// stored type ids, and shows them under the new names.
//
//   bloom_node_rename_fixture_test                 checks the fixture
//   bloom_node_rename_fixture_test --write <file>  writes the fixture project (a tool mode)

#include "node_editor_items.hpp"
#include "node_interaction_test_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#ifndef BLOOM_COLOR_FIXTURE_DIR
#error "BLOOM_COLOR_FIXTURE_DIR must name the checked-in color fixtures"
#endif

namespace bloom::ui::test {
namespace {
constexpr std::string_view kFileTransform = "bloom.ocio-file-transform";
constexpr std::string_view kColorSpaceTransform = "bloom.ocio-colour-space-transform";

project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

std::vector<std::byte> readFixture() {
    std::ifstream file(std::filesystem::path{BLOOM_COLOR_FIXTURE_DIR} / "ocio-nodes.bloom",
                       std::ios::binary);
    if (!file)
        throw std::runtime_error("missing fixture ocio-nodes.bloom");
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    const auto view = std::as_bytes(std::span(bytes));
    return {view.begin(), view.end()};
}

int writeFixture(const char* path) {
    Fixture f;
    f.add(kColorSpaceTransform, {200, 100});
    f.add(kFileTransform, {600, 100});
    const auto snapshot = f.document.snapshot();
    const auto settings =
        document::makeBloomNeutralColorSettingsV1(color::kBloomNeutralV1ConfigDigest);
    auto saved = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    if (!saved)
        throw std::runtime_error("the fixture project did not save");
    const auto bytes = saved.archive()->bytes();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file ? 0 : 1;
}

void testSavedProjectKeepsItsNodes() {
    auto opening = project::openProjectArchive(readFixture(), {}, memory());
    expect(opening.outcome() == project::OpenArchiveOutcome::Opened, "the saved project opens");
    if (opening.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    const auto opened = std::move(opening).takeOpened();
    const auto snapshot = opened.document->snapshot();
    expect(snapshot.project().validate().ok(), "the opened project validates");
    bool fileTransform = false;
    bool colorSpaceTransform = false;
    for (const auto& composition : snapshot.project().compositions())
        for (const auto& node : composition.graph().nodes()) {
            fileTransform = fileTransform || node.typeId == kFileTransform;
            colorSpaceTransform = colorSpaceTransform || node.typeId == kColorSpaceTransform;
        }
    expect(fileTransform, "the saved bloom.ocio-file-transform node is still there");
    expect(colorSpaceTransform, "the saved bloom.ocio-colour-space-transform node is still there");
    expect(node_editor::nodeTypeDisplayName(kFileTransform) == QStringLiteral("Apply LUT"),
           "bloom.ocio-file-transform is shown as Apply LUT");
    expect(node_editor::nodeTypeDisplayName(kColorSpaceTransform) ==
               QStringLiteral("Color Space Transform"),
           "bloom.ocio-colour-space-transform is shown as Color Space Transform");
}
} // namespace
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--write")
            return bloom::ui::test::writeFixture(argv[2]);
        bloom::ui::test::testSavedProjectKeepsItsNodes();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
