// Keyframe handle mode persistence: a key's handle mode (Aligned or Auto; Free is the default and
// is never written) saves and reopens exactly together with the handles the mode produced, a
// project without any mode writes no member, and a file that misuses the member is refused.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>

#include "zip_container_test_support.hpp"

#include <algorithm>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bloom;
int failures = 0;
void expect(const bool condition, const char* message) {
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
core::RationalTime time(const std::int64_t numerator, const std::int64_t denominator) {
    return *core::RationalTime::create(numerator, denominator);
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Handles", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::CompositionId composition = initial.initialCompositionId;
    document::Document document{withAsset(std::move(initial.project))};
    commands::CommandStack commands{document};
    document::AnimationCurveId opacityCurve;

    static document::Project withAsset(document::Project project) {
        document::AssetRecord asset;
        asset.id = document::AssetId::fromRaw(1);
        asset.locator = {"file", "project-relative", "media/plate.png", "file:///media/plate.png"};
        asset.width = 4;
        asset.height = 3;
        if (!project.addAsset(asset))
            throw std::runtime_error("asset fixture");
        return project;
    }

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Handles", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }

    const document::Composition& main() const {
        return *document.snapshot().project().findComposition(composition);
    }

    explicit Fixture(const bool withModes) {
        run<commands::AddImageLayer>(composition, document::AssetId::fromRaw(1));
        const auto layers = main().graph().layerOutputs();
        const auto* node = main().graph().findNode(layers[0].nodeId);
        document::ParameterId opacity;
        for (const auto& binding : node->parameters)
            if (binding.role == document::kOpacityParameterRole)
                opacity = binding.parameterId;
        run<commands::PasteKeyframes>(
            composition, std::vector<commands::KeyframePaste>{{opacity, time(0, 1), 0.0},
                                                              {opacity, time(1, 1), 1.0},
                                                              {opacity, time(2, 1), 0.25},
                                                              {opacity, time(3, 1), 0.75}});
        opacityCurve =
            std::get<document::AnimationCurveSource>(main().parameters().find(opacity)->source)
                .curveId;
        if (!withModes)
            return;
        const auto& curve = *main().animationCurves().findScalar(opacityCurve);
        run<commands::SetKeyframesHandleMode>(
            composition,
            std::vector<commands::KeyframeAddress>{{opacityCurve, curve.keyframes[1].id},
                                                   {opacityCurve, curve.keyframes[2].id}},
            document::KeyframeHandleMode::Auto);
        run<commands::SetKeyframesHandleMode>(
            composition,
            std::vector<commands::KeyframeAddress>{{opacityCurve, curve.keyframes[3].id}},
            document::KeyframeHandleMode::Aligned);
    }
};

std::vector<std::byte> save(const document::Snapshot& snapshot) {
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    if (!archive)
        throw std::runtime_error("save");
    const auto bytes = archive.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

std::string documentText(const std::vector<std::byte>& archive) {
    auto entries = project::readZipContainer(archive, {}, memory());
    if (!entries)
        throw std::runtime_error("entries");
    const auto bytes = entries.document()->documentBytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::size_t occurrences(const std::string& text, const std::string_view needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++count;
    return count;
}

project::DocumentDecodeResult decode(const std::string& text) {
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

void roundTrip() {
    Fixture fixture(true);
    const auto snapshot = fixture.document.snapshot();
    const auto archive = save(snapshot);
    const auto text = documentText(archive);
    expect(text.find("\"minor\": 0\n  },\n  \"project\"") != std::string::npos,
           "the document is written at the current schema (1.0)");
    expect(occurrences(text, "\"handleMode\": \"auto\"") == 2 &&
               occurrences(text, "\"handleMode\": \"aligned\"") == 1,
           "each non-Free key carries its mode");
    expect(occurrences(text, "\"handleMode\"") == 3 && text.find("\"free\"") == std::string::npos,
           "Free is never written");

    auto result = project::openProjectArchive(archive, {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(fixture.composition);
    const auto& after = *reopened.project().findComposition(fixture.composition);
    expect(
        std::ranges::equal(before.animationCurves().records(), after.animationCurves().records()),
        "modes and the handles they produced reopen bit for bit");
    const auto* keys = after.animationCurves().findScalar(fixture.opacityCurve);
    expect(keys != nullptr && keys->keyframes[0].handleMode == document::KeyframeHandleMode::Free &&
               keys->keyframes[1].handleMode == document::KeyframeHandleMode::Auto &&
               keys->keyframes[3].handleMode == document::KeyframeHandleMode::Aligned,
           "the modes reopen on the keys that had them");
    expect(save(reopened) == archive, "a reopened project re-saves byte-identically");
}

void projectsWithoutModes() {
    Fixture fixture(false);
    const auto snapshot = fixture.document.snapshot();
    const auto archive = save(snapshot);
    const auto text = documentText(archive);
    expect(text.find("handleMode") == std::string::npos, "a project without modes has no member");

    auto result = project::openProjectArchive(archive, {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "the archive without modes opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& original = *snapshot.project().findComposition(fixture.composition);
    const auto& restored = *reopened.project().findComposition(fixture.composition);
    expect(std::ranges::equal(original.animationCurves().records(),
                              restored.animationCurves().records()),
           "a project without modes opens with every key Free and every handle bit for bit");
    expect(documentText(save(reopened)) == text, "re-saving it writes the same document");
}

void refusals() {
    Fixture fixture(true);
    const auto text = documentText(save(fixture.document.snapshot()));
    expect(static_cast<bool>(decode(text)), "the document decodes");

    // "free" is the default and the writer never spells it.
    auto spelled = text;
    spelled.replace(spelled.find("\"aligned\""), std::string_view("\"aligned\"").size(),
                    "\"free\"");
    expect(!decode(spelled), "a spelled-out \"free\" is refused");
    auto unknown = text;
    unknown.replace(unknown.find("\"aligned\""), std::string_view("\"aligned\"").size(),
                    "\"smooth\"");
    expect(!decode(unknown), "an unknown mode token is refused");
}

} // namespace

int main() {
    try {
        roundTrip();
        projectsWithoutModes();
        refusals();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
