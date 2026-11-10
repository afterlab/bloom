// #343: imported SVG artwork saves and reopens. A file with a compound shape, a blur filter, a
// gradient and text imports through AddSvgArtwork, one point of the compound shape is keyed, and
// the project saves and reopens with every layer, node, parameter and curve equal to itself, the
// pivots where they were, and the source file recorded (path and SHA-256) in the Assets. The source
// file is then moved away, and the project still opens and still validates: nothing reads the
// file again.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/platform/font_catalog.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
using namespace bloom;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

const char* kSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">
<defs><filter id="soft"><feGaussianBlur stdDeviation="2"/></filter>
<linearGradient id="ramp"><stop offset="0" stop-color="#f00"/><stop offset="1" stop-color="#00f"/></linearGradient></defs>
<path id="Ring" fill="#0f0" fill-rule="evenodd" d="M20 20H80V80H20Z M35 35H65V65H35Z"/>
<rect id="Soft" x="100" y="20" width="30" height="30" fill="#fff" filter="url(#soft)"/>
<rect id="Ramp" x="140" y="20" width="40" height="40" fill="url(#ramp)"/>
<text id="Label" x="100" y="90" font-size="10" font-family="Test Sans" font-weight="700">Hi</text>
</svg>)svg";

std::string readAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void importKeySaveAndReopen() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto folder = std::filesystem::temp_directory_path() / ("bloom-svg-persistence-" + std::to_string(stamp));
    std::filesystem::create_directories(folder);
    const auto file = folder / "art.svg";
    std::ofstream(file, std::ios::binary) << kSvg;

    auto initial = document::makeNewProject(
        "Svg", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(400, 200, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    const auto composition = initial.initialCompositionId;
    document::Document document(std::move(initial.project));
    commands::CommandStack stack(document);

    auto fonts = std::make_shared<platform::FontCatalogue>();
    fonts->status = platform::FontCatalogueStatus::Available;
    platform::FontFace bold;
    bold.family = "Test Sans";
    bold.style = "Bold";
    bold.weight = 700;
    bold.postscriptName = "TestSans-Bold";
    bold.path = folder / "missing-font.ttf";
    fonts->faces.push_back(bold);

    {
        commands::Transaction transaction("Import SVG", document.snapshot().revision());
        auto& op = transaction.emplace<commands::AddSvgArtwork>(composition, nullptr, file, "art", true,
                                                                commands::SvgImportMode::Shapes);
        op.withProjectDirectory(folder).withFonts(fonts);
        const auto result = stack.execute(std::move(transaction));
        expect(result.succeeded(), "the file imports");
    }
    const auto& imported = *document.snapshot().project().findComposition(composition);

    // The compound shape's point is keyed (a whole-value key on its Path node).
    document::NodeId ringNode;
    document::ParameterId ringValue;
    document::PathValue ringBase;
    for (const auto& node : imported.graph().nodes()) {
        if (node.typeId != document::kPathValueNodeType)
            continue;
        for (const auto& binding : node.parameters)
            if (const auto* record = imported.parameters().find(binding.parameterId))
                if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source))
                    if (const auto* value = std::get_if<document::PathValue>(&constant->value))
                        if (value->contours.size() == 2) {
                            ringNode = node.id;
                            ringValue = binding.parameterId;
                            ringBase = *value;
                        }
    }
    expect(ringNode.isValid() && ringValue.isValid(), "the compound shape has one Path node with two contours");
    auto moved = ringBase;
    moved.contours[0].anchors[1].point.x += 25.0;
    moved.contours[1].anchors[2].point.y -= 7.5;
    for (const auto& [time, value] : {std::pair{core::RationalTime::fromInteger(1), moved},
                                      std::pair{core::RationalTime{}, ringBase}}) {
        commands::Transaction transaction("Key", document.snapshot().revision());
        transaction.emplace<commands::SetKeyframeAtTimeForParameter>(composition, ringValue, time, value);
        expect(stack.execute(std::move(transaction)).succeeded(), "a whole path value keys at a time");
    }

    // The source is recorded: path under the project folder and the hash of the file's bytes.
    const auto before = document.snapshot();
    const auto assets = before.project().assets();
    expect(assets.size() == 1, "one asset records the source file");
    if (!assets.empty()) {
        core::Sha256Hasher hasher;
        const auto text = readAll(file);
        expect(hasher.update(std::as_bytes(std::span(text))), "hash");
        expect(assets.front().contentDigest == hasher.finalize(), "the record holds the file's SHA-256");
        expect(assets.front().locator.path == "art.svg" && assets.front().locator.portability == "project-relative",
               "the record holds the path relative to the project folder");
    }
    // Importing the same file again records it once.
    {
        commands::Transaction transaction("Import SVG again", document.snapshot().revision());
        auto& op = transaction.emplace<commands::AddSvgArtwork>(composition, nullptr, file, "again", true,
                                                                commands::SvgImportMode::Shapes);
        op.withProjectDirectory(folder).withFonts(fonts);
        expect(stack.execute(std::move(transaction)).succeeded() && document.snapshot().project().assets().size() == 1,
               "the same file is recorded once");
    }
    expect(stack.undo().changed() && document.snapshot().project().assets().size() == 1,
           "undoing the second import keeps the first's record");

    // Save, move the source away, reopen.
    const auto snapshot = document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(archive), "the imported artwork saves and verifies");
    std::filesystem::rename(file, folder / "moved.svg");
    std::filesystem::remove(folder / "moved.svg");
    if (!archive)
        return;
    auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "the project opens with its source file gone");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& was = *snapshot.project().findComposition(composition);
    const auto& now = *reopened.project().findComposition(composition);
    expect(std::ranges::equal(was.graph().layerOutputs(), now.graph().layerOutputs()),
           "every imported layer, its pivot and its matte equal themselves after save and open");
    // A reopened node lists its parameters in role order; the bindings are the same set.
    const auto bindingsOf = [](const document::NodeRecord& node) {
        auto bindings = node.parameters;
        std::ranges::sort(bindings, {}, &document::ParameterBinding::role);
        return bindings;
    };
    bool nodesEqual = was.graph().nodes().size() == now.graph().nodes().size();
    for (const auto& node : was.graph().nodes()) {
        const auto* other = now.graph().findNode(node.id);
        nodesEqual = nodesEqual && other != nullptr && node.typeId == other->typeId &&
                     node.schemaVersion == other->schemaVersion &&
                     bindingsOf(node) == bindingsOf(*other);
    }
    expect(nodesEqual, "every node, with its parameter bindings, equals itself");
    expect(std::ranges::equal(was.parameters().records(), now.parameters().records()) &&
               std::ranges::equal(was.animationCurves().records(), now.animationCurves().records()),
           "every parameter, the keyed compound shape and the pivots equal themselves");
    expect(std::ranges::equal(reopened.project().assets(), snapshot.project().assets()),
           "the source record survives with its path and hash");
    expect(reopened.project().validate().ok(), "the reopened project validates");

    // One blur, a text layer in the file's face, and a pivot that is the centre of the Ring.
    std::size_t blurs = 0;
    for (const auto& node : now.graph().nodes())
        blurs += node.typeId == "bloom.gaussian-blur" ? 1 : 0;
    expect(blurs == 1, "the filter is one Gaussian Blur node");
    bool fontKept = false;
    for (const auto& record : now.parameters().records())
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&record.source))
            if (const auto* text = std::get_if<std::string>(&constant->value))
                fontKept = fontKept || text->find("family=Test Sans") != std::string::npos;
    expect(fontKept, "the text keeps the file's font after reopening");
    std::filesystem::remove_all(folder);
}

} // namespace

int main() {
    try {
        importKeySaveAndReopen();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
