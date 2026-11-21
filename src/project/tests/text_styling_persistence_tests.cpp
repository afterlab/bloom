// #757: a Text Source's styling parameters in a saved project. A styled node is saved, reopened
// and re-encoded byte for byte; a version-2 node saved before the styling parameters opens with
// exactly the bindings it was saved with (no migration); and validation refuses a styling value
// outside its domain.
#include "zip_container_test_support.hpp"

#include <bloom/core/color.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/text_styling.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace bloom;
using namespace bloom::project;
int failures = 0;
void expect(const bool condition, const std::string& message) {
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

using Values = std::map<std::string, document::ParameterValue, std::less<>>;

// The styled values the round trip writes: markup, an accent, an outline and a background box.
Values styledValues() {
    return {{"markup", true},
            {"accent1", core::Color4d{1.0, 0.25, 0.0, 1.0}},
            {"outlineColor", core::Color4d{0.0, 0.0, 0.5, 1.0}},
            {"outlineWidth", 3.5},
            {"outlineJoin", std::int64_t{document::kTextOutlineJoinBevel}},
            {"background", std::int64_t{document::kTextBackgroundBlock}},
            {"backgroundColor", core::Color4d{0.0, 0.0, 0.0, 0.5}},
            {"backgroundPadding", 0.5},
            {"backgroundRadius", 6.0}};
}

struct Authored final {
    std::unique_ptr<document::Document> document;
    document::CompositionId compositionId;
    document::NodeId node;
    // The node's parameter ids by role.
    std::map<std::string, document::ParameterId, std::less<>> parameters;
};

// A project with one Text Source. `styling` false binds only the twelve layout parameters, as a
// version-2 node saved before #757 has; `values` overrides the defaults by role.
std::optional<Authored> authoredProject(const bool styling, const Values& values,
                                        std::string* problem = nullptr) {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    Authored authored{std::make_unique<document::Document>(std::move(initial.project)),
                      initial.initialCompositionId,
                      {},
                      {}};
    const auto snapshot = authored.document->snapshot();
    auto draft = authored.document->draft(snapshot);
    auto& composition = *draft.project().findComposition(initial.initialCompositionId);
    const auto* definition = document::builtInNodeDefinitions().find(
        document::kTextSourceNodeType, document::kTextSourceNodeSchemaVersion);
    const auto nodeId = draft.ids().allocateNode();
    if (definition == nullptr || !nodeId)
        throw std::runtime_error("Text definition");
    std::vector<document::ParameterBinding> bindings;
    for (const auto& parameter : definition->parameters) {
        if (!styling && document::isTextStylingSchemaKey(parameter.schemaKey))
            continue;
        const auto id = draft.ids().allocateParameter();
        if (!id)
            throw std::runtime_error("parameter id");
        auto value = parameter.defaultValue;
        if (const auto found = values.find(parameter.role); found != values.end())
            value = found->second;
        // The parameter table refuses a value its schema does not accept.
        if (!composition.parameters().insert(
                {*id, parameter.schemaKey, document::ConstantValueSource{value}})) {
            if (problem != nullptr)
                *problem = "refused " + parameter.role;
            return std::nullopt;
        }
        bindings.push_back({parameter.role, *id});
        authored.parameters.emplace(parameter.role, *id);
    }
    if (!composition.graph().addNode({*nodeId, std::string(document::kTextSourceNodeType),
                                      std::move(bindings), document::kTextSourceNodeSchemaVersion}))
        throw std::runtime_error("Text node");
    authored.node = *nodeId;
    const auto validation = draft.validate();
    if (!validation.ok()) {
        if (problem != nullptr)
            *problem = "invalid";
        return std::nullopt;
    }
    if (!authored.document->commit(snapshot.revision(), std::move(draft)).committed())
        throw std::runtime_error("fixture publication");
    return authored;
}

std::vector<std::byte> archiveOf(const document::Snapshot& snapshot,
                                 const document::ColorSettings& settings) {
    const CanonicalManifestV1 manifest;
    const CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &settings};
    auto saved = buildVerifiedSaveArchive(manifest, input, {}, memory());
    if (!saved || !saved.archive())
        throw std::runtime_error("text styling archive");
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

std::string documentTextOf(const std::vector<std::byte>& archive) {
    auto entries = readZipContainer(archive, {}, memory());
    if (!entries)
        throw std::runtime_error("text styling entries");
    const auto bytes = entries.document()->documentBytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

const document::ParameterValue* constantIn(const document::Composition& composition,
                                           const document::ParameterId id) {
    const auto* record = composition.parameters().find(id);
    const auto* constant =
        record == nullptr ? nullptr : std::get_if<document::ConstantValueSource>(&record->source);
    return constant == nullptr ? nullptr : &constant->value;
}

void styledRoundTrip() {
    const auto authored = authoredProject(true, styledValues());
    expect(authored.has_value(), "a styled Text Source validates");
    if (!authored)
        return;
    const auto settings = neutralColorSettings();
    const auto snapshot = authored->document->snapshot();
    const auto archive = archiveOf(snapshot, settings);
    const auto text = documentTextOf(archive);
    for (const auto key :
         {"bloom.text.markup", "bloom.text.accent1", "bloom.text.outlineWidth",
          "bloom.text.outlineJoin", "bloom.text.background", "bloom.text.backgroundRadius"})
        expect(text.find(key) != std::string::npos,
               std::string("the styling parameter ") + key + " is written");

    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened, "the styled project opens");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(openedResult).takeOpened();
    expect(!opened.roundTrip, "nothing unknown is retained");
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(authored->compositionId);
    const auto* node =
        composition == nullptr ? nullptr : composition->graph().findNode(authored->node);
    expect(node != nullptr && node->parameters.size() == 24 &&
               node->schemaVersion == document::kTextSourceNodeSchemaVersion,
           "the node reopens at version 2 with its twenty-four bindings");
    if (composition != nullptr)
        for (const auto& [role, value] : styledValues()) {
            const auto* reread = constantIn(*composition, authored->parameters.at(role));
            expect(reread != nullptr && *reread == value,
                   "the styling value " + role + " reopens equal");
        }
    const auto reencoded = documentTextOf(archiveOf(reopened, opened.colorSettings));
    expect(reencoded == text,
           "re-encoding the reopened styled document reproduces identical bytes");
}

void versionTwoWithoutStyling() {
    const auto authored = authoredProject(false, {});
    expect(authored.has_value(), "a version-2 Text Source without styling still validates");
    if (!authored)
        return;
    const auto settings = neutralColorSettings();
    const auto archive = archiveOf(authored->document->snapshot(), settings);
    const auto text = documentTextOf(archive);
    expect(text.find("bloom.text.markup") == std::string::npos &&
               text.find("bloom.text.outline") == std::string::npos,
           "a node without styling writes no styling parameter");
    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened, "the older project opens");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(openedResult).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(authored->compositionId);
    const auto* node =
        composition == nullptr ? nullptr : composition->graph().findNode(authored->node);
    expect(node != nullptr && node->parameters.size() == 12 &&
               node->schemaVersion == document::kTextSourceNodeSchemaVersion,
           "it reopens with its twelve bindings, not upgraded or migrated");
    expect(documentTextOf(archiveOf(reopened, opened.colorSettings)) == text,
           "and re-encodes to identical bytes");
}

void refusals() {
    const auto refused = [](const std::string& role, document::ParameterValue value) {
        Values values{{role, std::move(value)}};
        std::string problem;
        return !authoredProject(true, values, &problem).has_value();
    };
    expect(refused("outlineJoin", std::int64_t{3}), "an outline join past Bevel is refused");
    expect(refused("background", std::int64_t{-1}), "a negative background choice is refused");
    expect(refused("outlineWidth", -1.0), "a negative outline width is refused");
    expect(refused("outlineWidth", 300.0), "an outline width past 256 px is refused");
    expect(refused("backgroundPadding", 5.0), "a padding past 4 em is refused");
    expect(refused("markup", 1.0), "a markup value that is not a Boolean is refused");
    expect(!refused("outlineWidth", 256.0) && !refused("backgroundPadding", 4.0),
           "the domain ends are accepted");
}

} // namespace

int main() {
    try {
        styledRoundTrip();
        versionTwoWithoutStyling();
        refusals();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " text styling persistence expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: Text Source styling saves, reopens byte for byte and keeps older nodes\n";
    return 0;
}
