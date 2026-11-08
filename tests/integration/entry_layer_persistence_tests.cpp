// #502 persistence: a Merge entry's own layer (slotId, its own parameters, the frame pivot, Time
// Remap) and an Adjustment entry save, reopen and decode exactly in the current document (entry
// layers arrived in 1.37); the canonical
// text is stable; a document claiming 1.36 that carries an entry-layer member is refused; a 1.36
// document without them migrates by version alone and opens unchanged.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

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

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Entries", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 32, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Entries", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    [[nodiscard]] const document::Composition& comp() const {
        return *document.snapshot().project().findComposition(composition);
    }
    void withEntryLayers() {
        const auto merge = comp().graph().layerStack().nodeId();
        const auto solid = *run<commands::AddNode>(composition,
                                                   std::string(document::kSolidSourceNodeType),
                                                   document::Vec2d{0, 0})
                                .outputId<document::NodeId>(commands::kAddNodeOutput);
        const auto slot =
            *run<commands::ConnectPorts>(
                 composition, document::OutputPortRef{solid, "image"},
                 document::InputPortRef{document::LayerStackInputRef{merge, {}, "content"}})
                 .outputId<document::LayerSlotId>(commands::kConnectPortsSlotOutput);
        const auto edited = run<commands::EntryLayerEdit>(
            composition, merge, slot, [this](const document::LayerId layer) {
                return std::make_unique<commands::EnableLayerTimeRemap>(composition, layer);
            });
        const auto layer = *edited.outputId<document::LayerId>(commands::kEntryLayerOutput);
        run<commands::SetLayerSolo>(composition, layer, true);
        const auto* boundary = comp().graph().findLayer(layer);
        run<commands::SetParameterSource>(
            composition,
            document::layerParameterBinding(comp().graph(), *boundary,
                                            document::kOpacityParameterRole)
                ->parameterId,
            document::ParameterSource{document::ConstantValueSource{0.5}});
        run<commands::AddAdjustmentLayer>(composition, merge);
    }
};

std::string encode(const document::Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const project::CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = project::canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!project::encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}

project::DocumentDecodeResult decodeText(const std::string& text) {
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("entry layer parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

void replaceOnce(std::string& text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos)
        throw std::logic_error("fixture anchor missing");
    text.replace(at, from.size(), to);
}

bool sameComposition(const document::Composition& a, const document::Composition& b) {
    if (!std::ranges::equal(a.graph().layerOutputs(), b.graph().layerOutputs()) ||
        // A node made by AddNode binds in definition order and reopens in canonical role order.
        !std::ranges::equal(a.graph().nodes(), b.graph().nodes(),
                            [](document::NodeRecord left, document::NodeRecord right) {
                                std::ranges::sort(left.parameters, {},
                                                  &document::ParameterBinding::role);
                                std::ranges::sort(right.parameters, {},
                                                  &document::ParameterBinding::role);
                                return left == right;
                            }) ||
        !std::ranges::equal(a.graph().edges(), b.graph().edges()) ||
        !std::ranges::equal(a.parameters().records(), b.parameters().records()) ||
        !std::ranges::equal(a.animationCurves().records(), b.animationCurves().records()) ||
        a.graph().merges().size() != b.graph().merges().size())
        return false;
    for (const auto& merge : a.graph().merges()) {
        const auto* other = b.graph().merge(merge.nodeId());
        if (other == nullptr || !std::ranges::equal(merge.entries(), other->entries()))
            return false;
    }
    return true;
}

void entryLayersRoundTrip() {
    Fixture fixture;
    fixture.withEntryLayers();
    const auto snapshot = fixture.document.snapshot();
    expect(snapshot.project().validate().ok(), "the entry-layer project validates");
    const auto text = encode(fixture.document);
    expect(
        text.find("\"slotId\"") != std::string::npos &&
            text.find("\"pivot\": \"frame\"") != std::string::npos &&
            text.find("\"role\": \"timeRemap\"") != std::string::npos &&
            text.find("\"bloom.adjustment\"") != std::string::npos &&
            text.find("\"minor\": 43") != std::string::npos,
        "entry layers, the frame pivot, their Time Remap and the Adjustment are written at 1.42");
    const auto decoded = decodeText(text);
    expect(static_cast<bool>(decoded), "the 1.38 document decodes");
    if (decoded) {
        const auto& layers = decoded.value()->compositions.front().graph.layerOutputs;
        expect(std::ranges::count_if(layers, [](const auto& layer) { return layer.entryOwned(); }) ==
                       2 &&
                   std::ranges::all_of(layers,
                                       [](const auto& layer) {
                                           return !layer.entryOwned() ||
                                                  (layer.pivot == document::LayerPivot::Frame &&
                                                   layer.parameters.size() >= 6);
                                       }),
               "both entry layers decode with their slot, pivot and parameters");
    }

    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(archive), "the entry-layer archive saves and verifies");
    if (!archive)
        return;
    auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    expect(sameComposition(*snapshot.project().findComposition(fixture.composition),
                           *reopened.project().findComposition(fixture.composition)),
           "every entry layer, slot, parameter and node equals itself after save and open");
    expect(reopened.project().validate().ok(), "the reopened project validates");
    expect(encode(*opened.document) == text, "the canonical text is stable across save and open");
}

void olderDocumentsOpenUnchangedAndRefuseEntryMembers() {
    // A 1.36 document has no entry layer: it migrates by version alone and opens unchanged.
    Fixture plain;
    const auto merge = plain.comp().graph().layerStack().nodeId();
    const auto solid = *plain.run<commands::AddSolidLayer>(plain.composition, "Solid",
                                                           core::Color4d{1, 0, 0, 1})
                            .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    static_cast<void>(solid);
    static_cast<void>(merge);
    const auto current = encode(plain.document);
    auto legacy = current;
    replaceOnce(legacy, "\"minor\": 43", "\"minor\": 36");
    const auto decodedLegacy = decodeText(legacy);
    expect(static_cast<bool>(decodedLegacy), "a 1.36 document without entry layers decodes");
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(legacy)), {}, operation);
    expect(static_cast<bool>(parsed), "the 1.36 fixture parses");
    if (parsed) {
        const auto migrated =
            project::migrateDocumentDom(parsed.document()->root(), {1, 36}, {1, 38},
                                        project::kProductionDocumentMigrationSteps, {}, operation);
        expect(migrated.outcome() == project::MigrationOutcome::Migrated &&
                   migrated.stepsApplied() == 2,
               "1.36 -> 1.38 is two steps");
        if (migrated.outcome() == project::MigrationOutcome::Migrated) {
            const auto decodedMigrated = project::decodeDocumentEnvelope(*migrated.migratedRoot());
            expect(static_cast<bool>(decodedMigrated) && decodedLegacy &&
                       decodedMigrated.value()->compositions.front().graph.layerOutputs ==
                           decodedLegacy.value()->compositions.front().graph.layerOutputs,
                   "the 1.36 -> 1.38 steps only advance the version");
        }
    }

    // A document claiming 1.36 cannot carry an entry layer or a pivot.
    Fixture fixture;
    fixture.withEntryLayers();
    auto text = encode(fixture.document);
    replaceOnce(text, "\"minor\": 43", "\"minor\": 36");
    expect(!static_cast<bool>(decodeText(text)), "a 1.36 document carrying entry layers is refused");

    // The pivot takes only "frame".
    auto content = encode(fixture.document);
    replaceOnce(content, "\"pivot\": \"frame\"", "\"pivot\": \"content\"");
    expect(!static_cast<bool>(decodeText(content)), "a written content pivot is refused");
    auto bogus = encode(fixture.document);
    replaceOnce(bogus, "\"pivot\": \"frame\"", "\"pivot\": \"elsewhere\"");
    expect(!static_cast<bool>(decodeText(bogus)), "an unknown pivot is refused");
}

} // namespace

int main() try {
    entryLayersRoundTrip();
    olderDocumentsOpenUnchangedAndRefuseEntryMembers();
    if (failures != 0) {
        std::cerr << failures << " entry layer persistence expectation(s) failed\n";
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "entry layer persistence test aborted: " << error.what() << '\n';
    return 1;
}
