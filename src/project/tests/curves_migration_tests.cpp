// #493: a project saved with Curves schema 1 (five Vector 2 points per channel) opens as schema 2.
// The schema 1 file is written from a Levels node of the same shape (26 parameters: five per
// channel, then clamp) by renaming its type, roles and schema keys and rewriting each value as a
// point, so every id, the allocator high water and the canonical member order are a real file's.

#include "zip_container_test_support.hpp"

#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace bloom;
using namespace bloom::project;
using document::Vec2d;
int failures = 0;
void expect(const bool condition, const char* message) {
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

// The schema 1 points each channel is written with: a moved master, an inverted red with an HDR
// point, and the identity elsewhere.
using Channel = std::array<Vec2d, document::kLegacyCurvePointCount>;
std::array<Channel, document::kGradeChannelCount> legacyChannels() {
    std::array<Channel, document::kGradeChannelCount> channels{};
    for (std::size_t channel = 0; channel < document::kGradeChannelCount; ++channel)
        for (std::size_t point = 0; point < document::kLegacyCurvePointCount; ++point) {
            const double c = document::defaultCurvePointCoordinate(point);
            channels[channel][point] = {c, c};
        }
    channels[0] = {Vec2d{0.0, 0.0}, {0.2, 0.1}, {0.5, 0.5}, {0.8, 0.9}, {1.0, 1.0}};
    channels[1] = {Vec2d{0.0, 0.875}, {0.25, 0.625}, {0.5, 0.5}, {0.75, 0.25}, {1.5, -0.125}};
    return channels;
}

// Levels fields in their roles' sorted order (gamma, input black, input white, output black,
// output white), so renaming them to Point1..Point5 keeps the canonical binding order.
constexpr std::array<std::size_t, document::kLevelsFieldCount> kSortedLevelsFields{2, 0, 1, 3, 4};

std::string number(const double value) {
    const auto text = formatCanonicalFloat64(value);
    if (!text)
        throw std::logic_error("canonical number");
    return std::string(text.value()->view());
}

void replaceOnce(std::string& text, const std::string& from, const std::string& to,
                 const std::size_t start = 0) {
    const auto at = text.find(from, start);
    if (at == std::string::npos)
        throw std::logic_error("legacy fixture anchor: " + from);
    text.replace(at, from.size(), to);
}

struct LegacyFixture final {
    std::string documentText;
    std::vector<std::byte> archive;
    // Each schema 1 point's parameter id, by channel then point.
    std::array<std::array<document::ParameterId, document::kLegacyCurvePointCount>,
               document::kGradeChannelCount>
        ids{};
    document::ParameterId clamp;
    document::NodeId node;
    document::CompositionId composition;
};

std::vector<std::byte> archiveWith(const std::string& documentText) {
    const CanonicalManifestV1 manifest{};
    const auto size = canonicalManifestSize(manifest);
    if (!size)
        throw std::logic_error("manifest size");
    std::string manifestText(*size.value(), '\0');
    if (!encodeCanonicalManifest(manifest, manifestText))
        throw std::logic_error("manifest encode");
    return test::buildConformingArchive(
        test::makeStoredEntry("manifest.json", test::toBytes(manifestText)),
        test::makeStoredEntry("document.json", test::toBytes(documentText)));
}

LegacyFixture legacyFixture() {
    LegacyFixture fixture;
    auto initial = document::makeNewProject("Curves Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    fixture.composition = initial.initialCompositionId;
    document::Document owned(std::move(initial.project));
    const auto before = owned.snapshot();
    auto draft = owned.draft(before);
    auto* composition = draft.project().findComposition(fixture.composition);
    const auto* definition = document::builtInNodeDefinitions().find(document::kLevelsNodeType, 1);
    const auto node = draft.ids().allocateNode();
    if (!definition || !node || !composition)
        throw std::logic_error("legacy fixture node");
    fixture.node = *node;
    std::vector<document::ParameterBinding> bindings;
    std::map<std::string, document::ParameterId> byRole;
    // #383: a schema 1 Curves file never carried the effect mask Levels now has.
    for (const auto& parameter : document::withoutEffectMask(*definition).parameters) {
        const auto id = draft.ids().allocateParameter();
        if (!id)
            throw std::logic_error("legacy fixture parameter");
        auto value = parameter.defaultValue;
        if (parameter.role == "clamp")
            value = true;
        if (!composition->parameters().insert(
                {*id, parameter.schemaKey, document::ConstantValueSource{value}}))
            throw std::logic_error("legacy fixture parameter inserts");
        bindings.push_back({parameter.role, *id});
        byRole[parameter.role] = *id;
    }
    if (!composition->graph().addNode(
            {*node, std::string(document::kLevelsNodeType), std::move(bindings), 1}))
        throw std::logic_error("legacy fixture node inserts");
    if (!owned.commit(before.revision(), std::move(draft)).committed())
        throw std::logic_error("legacy fixture commit");
    const auto snapshot = owned.snapshot();
    const auto settings =
        document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes({}));
    const auto saved = buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    if (!saved)
        throw std::logic_error("legacy fixture archive");
    const auto bytes = saved.archive()->bytes();
    auto entries = readZipContainer(std::vector<std::byte>(bytes.begin(), bytes.end()), {},
                                    memory());
    if (!entries)
        throw std::logic_error("legacy fixture entries");
    const auto documentBytes = entries.document()->documentBytes();
    auto text = std::string(reinterpret_cast<const char*>(documentBytes.data()),
                            documentBytes.size());

    const auto channels = legacyChannels();
    replaceOnce(text, "\"typeId\": \"bloom.levels\"", "\"typeId\": \"bloom.curves\"");
    replaceOnce(text, "\"" + std::string(document::kLevelsClampSchemaKey) + "\"",
                "\"" + std::string(document::kCurvesClampSchemaKey) + "\"");
    fixture.clamp = byRole.at("clamp");
    for (std::size_t channel = 0; channel < document::kGradeChannelCount; ++channel)
        for (std::size_t point = 0; point < document::kLegacyCurvePointCount; ++point) {
            const auto field = kSortedLevelsFields[point];
            fixture.ids[channel][point] = byRole.at(document::levelsRole(channel, field));
            replaceOnce(text, "\"role\": \"" + document::levelsRole(channel, field) + "\"",
                        "\"role\": \"" + document::curvePointRole(channel, point) + "\"");
            const auto key = "\"schemaKey\": \"" + document::levelsSchemaKey(channel, field) + "\"";
            const auto at = text.find(key);
            if (at == std::string::npos)
                throw std::logic_error("legacy fixture schema key");
            text.replace(at, key.size(),
                         "\"schemaKey\": \"" + document::curvePointSchemaKey(channel, point) +
                             "\"");
            // "kind": "float64",<ws>"value": <number>  ->  "kind": "vec2",<ws>"x": X,<ws>"y": Y
            const std::string kind = "\"kind\": \"float64\",";
            const auto kindAt = text.find(kind, at);
            const auto valueAt = text.find("\"value\": ", kindAt);
            if (kindAt == std::string::npos || valueAt == std::string::npos)
                throw std::logic_error("legacy fixture value");
            const auto space = text.substr(kindAt + kind.size(), valueAt - kindAt - kind.size());
            const auto end = text.find_first_of(",\n}", valueAt + 9);
            const auto& p = channels[channel][point];
            text.replace(kindAt, end - kindAt,
                         "\"kind\": \"vec2\"," + space + "\"x\": " + number(p.x) + "," + space +
                             "\"y\": " + number(p.y));
        }
    fixture.documentText = text;
    fixture.archive = archiveWith(text);
    return fixture;
}

const document::ParameterRecord* recordFor(const document::Composition& composition,
                                           const document::NodeRecord& node,
                                           const std::string_view role) {
    const auto binding = std::ranges::find(node.parameters, role, &document::ParameterBinding::role);
    return binding == node.parameters.end() ? nullptr
                                            : composition.parameters().find(binding->parameterId);
}

std::optional<document::PathValue> constantPath(const document::ParameterRecord* record) {
    if (!record)
        return std::nullopt;
    const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
    const auto* path = constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
    return path ? std::optional{*path} : std::nullopt;
}

// A schema 1 file opens as schema 2: each channel's five points become its curve exactly, the hue
// curves are empty, the clamp is kept, ids are reused and no other record remains.
void openMigratesConstantPoints() {
    const auto fixture = legacyFixture();
    auto result = openProjectArchive(fixture.archive, {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "a Curves schema 1 project opens");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto snapshot = opened.document->snapshot();
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    const auto* node = composition ? composition->graph().findNode(fixture.node) : nullptr;
    expect(node && node->typeId == document::kCurvesNodeType &&
               node->schemaVersion == document::kCurvesNodeSchemaVersion &&
               node->parameters.size() == document::kGradeCurveCount + 1,
           "the node is Curves schema 2 with nine curves and clamp");
    if (!node)
        return;
    const auto channels = legacyChannels();
    for (std::size_t index = 0; index < document::kGradeCurveCount; ++index) {
        const auto curve = document::kGradeCurves[index];
        const auto* record = recordFor(*composition, *node, document::gradeCurveRole(curve));
        const auto value = constantPath(record);
        if (index < document::kGradeChannelCount) {
            expect(record && record->id == fixture.ids[index][0] &&
                       record->schemaKey == document::gradeCurveSchemaKey(curve) && value &&
                       *value == document::gradeCurvePath(channels[index]),
                   "each channel's points become its curve exactly, on its first point's id");
        } else {
            expect(record &&
                       record->id == fixture.ids[0][index - document::kGradeChannelCount + 1] &&
                       value && value->firstContour().anchors.empty(),
                   "each hue curve starts empty on a reused id");
        }
    }
    const auto* clamp = recordFor(*composition, *node, "clamp");
    const auto* clampValue =
        clamp ? std::get_if<document::ConstantValueSource>(&clamp->source) : nullptr;
    expect(clamp && clamp->id == fixture.clamp && clampValue &&
               clampValue->value == document::ParameterValue{true},
           "the clamp is kept");
    expect(composition->parameters().records().size() == document::kGradeCurveCount + 1,
           "no schema 1 point record remains");
    // The migrated project saves and reopens as itself.
    const auto settings = opened.colorSettings;
    const auto saved = buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(saved), "the migrated project saves");
    if (!saved)
        return;
    const auto bytes = saved.archive()->bytes();
    auto again = openProjectArchive(std::vector<std::byte>(bytes.begin(), bytes.end()), {},
                                    memory());
    expect(again.outcome() == OpenArchiveOutcome::Opened, "the migrated project reopens");
    if (again.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(again).takeOpened();
    const auto reopenedSnapshot = reopened.document->snapshot();
    const auto resaved = buildVerifiedSaveArchive(
        {}, {.snapshot = &reopenedSnapshot, .colorSettings = &reopened.colorSettings}, {},
        memory());
    expect(resaved && std::ranges::equal(resaved.archive()->bytes(), bytes),
           "the migrated project round-trips byte-identically");
}

// An animated point takes its first key per component and its curve is removed; a driven point
// takes the identity default its node started with.
void migrationResolvesAnimatedAndDrivenPoints() {
    const auto fixture = legacyFixture();
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(fixture.documentText)), {}, operation);
    expect(static_cast<bool>(parsed), "the schema 1 document parses");
    if (!parsed)
        return;
    auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    expect(static_cast<bool>(decoded), "a Curves schema 1 node decodes");
    if (!decoded)
        return;
    auto envelope = *decoded.value();
    auto& composition = envelope.compositions.front();
    const auto curveId = document::AnimationCurveId::fromRaw(envelope.highWater.animationCurve + 1);
    const auto keyA = document::KeyframeId::fromRaw(envelope.highWater.keyframe + 1);
    const auto keyB = document::KeyframeId::fromRaw(envelope.highWater.keyframe + 2);
    envelope.highWater.animationCurve += 1;
    envelope.highWater.keyframe += 2;
    document::Vec2AnimationCurve animated{curveId, {}};
    animated.component(document::AnimationComponent::X)->keyframes = {
        {keyA, core::RationalTime::fromInteger(0), 0.375},
        {keyB, core::RationalTime::fromInteger(2), 0.625}};
    composition.animationCurves.push_back(animated);
    const auto masterThird = fixture.ids[0][2];
    const auto redSecond = fixture.ids[1][1];
    for (auto& record : composition.parameters) {
        if (record.id == masterThird)
            record.source = document::AnimationCurveSource{
                curveId, document::ParameterValue{Vec2d{0.5, 0.7}}};
        if (record.id == redSecond)
            record.source = document::DriverBindingSource{fixture.node, "image"};
    }
    migrateLegacyCurvesNodes(envelope);
    expect(envelope.compositions.front().animationCurves.empty(),
           "a schema 1 point's animation curve is removed");
    const auto& node = *std::ranges::find(envelope.compositions.front().graph.nodes, fixture.node,
                                          &document::NodeRecord::id);
    expect(node.schemaVersion == document::kCurvesNodeSchemaVersion,
           "the decoded node migrates to schema 2");
    const auto pathOf = [&](const document::ParameterId id) -> std::optional<document::PathValue> {
        const auto record = std::ranges::find(envelope.compositions.front().parameters, id,
                                              &document::ParameterRecord::id);
        if (record == envelope.compositions.front().parameters.end())
            return std::nullopt;
        const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
        const auto* path = constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
        return path ? std::optional{*path} : std::nullopt;
    };
    auto master = legacyChannels()[0];
    master[2] = {0.375, 0.7};
    auto red = legacyChannels()[1];
    red[1] = {0.25, 0.25};
    expect(pathOf(fixture.ids[0][0]) == document::gradeCurvePath(master),
           "an animated point takes its first key, and its default for an unkeyed component");
    expect(pathOf(fixture.ids[1][0]) == document::gradeCurvePath(red),
           "a driven point takes its identity default");
    auto reconstructed = reconstructDocument(std::move(envelope));
    expect(static_cast<bool>(reconstructed), "the migrated envelope is valid document truth");
}

// A file whose Curves node claims schema 1 without schema 1 points is refused, not guessed at.
void malformedSchemaOneRefuses() {
    auto fixture = legacyFixture();
    auto text = fixture.documentText;
    replaceOnce(text, "\"role\": \"alphaPoint5\"", "\"role\": \"alphaPointX\"");
    auto result = openProjectArchive(archiveWith(text), {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Failed,
           "a schema 1 Curves node missing a point is refused");
}

} // namespace

int main() {
    try {
        openMigratesConstantPoints();
        migrationResolvesAnimatedAndDrivenPoints();
        malformedSchemaOneRefuses();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: Curves schema 1 projects open as schema 2\n";
    return 0;
}
