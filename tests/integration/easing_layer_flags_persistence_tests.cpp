// Persistence of Robert Penner's easing equations with their per-key parameters, and of the
// presentation-only layer flags `shy` and `nameFollowsSource`: they save and reopen exactly.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>

#include "zip_container_test_support.hpp"

#include <algorithm>
#include <iostream>
#include <regex>
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
        "Easing", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::CompositionId composition = initial.initialCompositionId;
    document::Document document{withAsset(std::move(initial.project))};
    commands::CommandStack commands{document};

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
        commands::Transaction transaction("Easing", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }

    const document::Composition& main() const {
        return *document.snapshot().project().findComposition(composition);
    }

    Fixture() {
        run<commands::AddImageLayer>(composition, document::AssetId::fromRaw(1));
        run<commands::AddImageLayer>(composition, document::AssetId::fromRaw(1));
        const auto layers = main().graph().layerOutputs();
        run<commands::RenameLayer>(composition, layers[1].layerId, std::string("Renamed"));
        run<commands::SetLayerShy>(composition, layers[0].layerId, true);
        // Animate the first layer's opacity: three keys, the first two eased by equations.
        const auto* node = main().graph().findNode(layers[0].nodeId);
        document::ParameterId opacity;
        for (const auto& binding : node->parameters)
            if (binding.role == document::kOpacityParameterRole)
                opacity = binding.parameterId;
        run<commands::PasteKeyframes>(
            composition, std::vector<commands::KeyframePaste>{{opacity, time(0, 1), 0.0},
                                                              {opacity, time(1, 1), 1.0},
                                                              {opacity, time(2, 1), 0.5}});
        const auto& curve = *main().animationCurves().findScalar(
            std::get<document::AnimationCurveSource>(main().parameters().find(opacity)->source)
                .curveId);
        run<commands::SetKeyframesEasing>(
            composition, std::vector<commands::KeyframeAddress>{{curve.id, curve.keyframes[0].id}},
            commands::KeyframeEasingEdit{document::KeyframeEasing::InOut, std::nullopt, 0.5, 0.25},
            document::KeyframeInterpolation::Elastic);
        run<commands::SetKeyframesInterpolation>(
            composition, std::vector<commands::KeyframeAddress>{{curve.id, curve.keyframes[1].id}},
            document::KeyframeInterpolation::Bounce);
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

void roundTrip() {
    Fixture fixture;
    const auto snapshot = fixture.document.snapshot();
    const auto archive = save(snapshot);
    const auto text = documentText(archive);
    expect(text.find("\"minor\": 0") != std::string::npos, "the document is written at 1.0");
    expect(text.find("\"elastic\"") != std::string::npos &&
               text.find("\"bounce\"") != std::string::npos,
           "the easing equations are written with their tokens");
    expect(occurrences(text, "\"easing\": {") == 1 && text.find("\"in-out\"") != std::string::npos,
           "only the key with non-default parameters carries an easing object");
    expect(occurrences(text, "\"shy\": true") == 1 &&
               occurrences(text, "\"nameFollowsSource\": true") == 1,
           "the shy layer and the following layer carry their flags; defaults are omitted");

    auto result = project::openProjectArchive(archive, {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(fixture.composition);
    const auto& after = *reopened.project().findComposition(fixture.composition);
    expect(std::ranges::equal(before.graph().layerOutputs(), after.graph().layerOutputs()),
           "layers, their shy and nameFollowsSource flags included, reopen exactly");
    expect(
        std::ranges::equal(before.animationCurves().records(), after.animationCurves().records()),
        "modes and easing parameters reopen bit for bit");
    expect(save(reopened) == archive, "a reopened project re-saves byte-identically");
}

} // namespace

int main() {
    try {
        roundTrip();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
