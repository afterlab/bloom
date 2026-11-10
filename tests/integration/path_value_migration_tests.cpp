// #589 document 1.43: the path value becomes a list of contours. A project saved by the writer that
// came before it opens with one contour wherever it had a path (shapes, masks, data blocks, a
// field's remap curve, a Curves node), renders byte for byte what that writer's build rendered, and
// survives save and reopen unchanged. tests/fixtures/paths holds the project and the document as
// that build wrote them; the digests below are the frames it rendered.
//
// #597 changed one thing that build did: a key followed by a key of a different shape used to hold,
// and now morphs into it. The fixture's mask has such a pair, so the one frame between them is the
// morph; with that key made a Hold key, the project renders every frame exactly as that build did.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/data_block.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#ifndef BLOOM_PATH_FIXTURE_DIR
#error "BLOOM_PATH_FIXTURE_DIR must name the checked-in path fixtures"
#endif

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

std::vector<std::byte> readFixture(const std::string_view name) {
    std::ifstream file(std::filesystem::path{BLOOM_PATH_FIXTURE_DIR} / name, std::ios::binary);
    if (!file)
        throw std::runtime_error("missing fixture");
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    const auto view = std::as_bytes(std::span(bytes));
    return {view.begin(), view.end()};
}

std::string readText(const std::string_view name) {
    const auto bytes = readFixture(name);
    std::string text(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(),
                           [](const std::byte byte) { return static_cast<char>(byte); });
    return text;
}

std::string encode(const document::Snapshot& snapshot, const document::ColorSettings& settings) {
    std::array<char, 1024> payloadScratch{};
    std::array<std::size_t, 1024> sortScratch{};
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
        throw std::runtime_error("parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

void replaceOnce(std::string& text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos)
        throw std::logic_error("fixture anchor missing");
    text.replace(at, from.size(), to);
}

// The frames at q / 4 seconds, as the build before document 1.43 rendered this fixture: the mask's
// two matching keys interpolate (frames 0 to 4), the second holds up to the triangle key (5), and
// the triangle key holds (6 and 7).
constexpr std::array<std::string_view, 8> kBaselineFrames{
    "f9197cee9f0b13325c07eeb2a2b4ad8d738a6599e9c2724f6c97b914e9b87e6b",
    "c4633bdde8c3c8179f28b32b13410dc4acf1dbc8591edcf1ab5beb95006eed44",
    "48be7f830cea24a3410b88c607e0dacf10eb88c792391d21c5a1fe1ec10a26c4",
    "032e2d0a00525fb5ab0094b688d43230d0afccaf23a43c8b214895ef45bb636a",
    "d057b84f6005fcb52e4fef44132a9f8e90af21b7eb8b2ade738351184dd6311f",
    "d057b84f6005fcb52e4fef44132a9f8e90af21b7eb8b2ade738351184dd6311f",
    "f5501f3d1b040c545143a165532d7d16b2f7dc83e04d613de7c58e15ad4ddb09",
    "f5501f3d1b040c545143a165532d7d16b2f7dc83e04d613de7c58e15ad4ddb09"};

// Frame 5 (1.25 seconds) since #597: halfway from the second key's quadrilateral to the triangle.
constexpr std::string_view kMorphFrame =
    "25d81daca334631947c22c5fad4ce477a61a010a6fb84d11493ebff8fb9b7c5a";
constexpr std::size_t kMorphFrameIndex = 5;

std::vector<std::string> renderFrames(const document::Snapshot& snapshot,
                                      const document::CompositionId composition) {
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({snapshot, composition}, {});
    if (!compiled.plan)
        throw std::runtime_error("the fixture does not compile");
    runtime::CpuCompositionEvaluator evaluator;
    std::vector<std::string> digests;
    for (int quarter = 0; quarter < static_cast<int>(kBaselineFrames.size()); ++quarter) {
        const auto result = evaluator.evaluate(
            compiled.plan,
            {*core::RationalTime::create(quarter, 4), compiled.plan->output(),
             runtime::CompositionFormatResolution{}, runtime::EvaluationQuality::Reference,
             runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 26U},
            {});
        if (!result.frame())
            throw std::runtime_error("the fixture does not render");
        const auto hash =
            core::Sha256Hasher::hash(std::as_bytes(result.frame()->processImage().pixels()));
        if (!hash)
            throw std::runtime_error("frame digest");
        const auto hex = hash->toLowercaseHex();
        digests.emplace_back(hex.begin(), hex.end());
    }
    return digests;
}

// The exact digests pin this toolchain's floating point against the old build's. Every platform
// still proves the frames unchanged by save and reopen, and every path unchanged, below. `morphs`
// expects the frame between the second key and the triangle to be the morph; otherwise the
// second key is a Hold key and every frame is the old build's.
void expectBaselineFrames(const std::vector<std::string>& frames, const bool morphs) {
    std::vector<std::string_view> expected(kBaselineFrames.begin(), kBaselineFrames.end());
    if (morphs)
        expected.at(kMorphFrameIndex) = kMorphFrame;
#if defined(__linux__) && defined(__x86_64__)
    const bool same = std::ranges::equal(frames, expected);
    expect(same, morphs ? "the migrated project renders every frame byte for byte as it did before "
                          "1.43, except the one where the keys of different shapes now morph"
                        : "with that key made Hold, the migrated project renders every frame byte "
                          "for byte as it did before 1.43");
    if (!same)
        for (const auto& frame : frames)
            std::cerr << "  rendered " << frame << '\n';
#else
    expect(frames.size() == expected.size(), "every frame renders");
#endif
    if (morphs && frames.size() == kBaselineFrames.size())
        expect(frames.at(kMorphFrameIndex) != frames.at(kMorphFrameIndex - 1) &&
                   frames.at(kMorphFrameIndex) != frames.at(kMorphFrameIndex + 1),
               "the frame between the keys of different shapes is neither key: it morphs");
}

struct PathCensus final {
    std::size_t paths = 0;
    std::size_t oneContour = 0;
    std::size_t empty = 0;
    std::size_t keyedPaths = 0;
    std::size_t dataBlockPaths = 0;

    void count(const document::PathValue& path) {
        ++paths;
        if (path.fillRule != document::PathFillRule::Inherit || !path.groups.empty())
            return;
        // A path with no anchors is the empty value; a path with anchors is one contour.
        if (path.contours.size() == 1)
            ++oneContour;
        else if (path.contours.empty())
            ++empty;
    }
    void count(const document::ParameterValue& value) {
        if (const auto* path = std::get_if<document::PathValue>(&value))
            count(*path);
    }
};

PathCensus censusOf(const document::Project& project, const document::CompositionId composition) {
    PathCensus census;
    const auto& main = *project.findComposition(composition);
    for (const auto& record : main.parameters().records()) {
        const auto* constant = std::get_if<document::ConstantValueSource>(&record.source);
        if (constant)
            census.count(constant->value);
    }
    for (const auto& curve : main.animationCurves().records()) {
        const auto* keyed = std::get_if<document::PathAnimationCurve>(&curve);
        if (keyed == nullptr)
            continue;
        for (const auto& key : keyed->keyframes) {
            census.count(key.value);
            ++census.keyedPaths;
        }
    }
    for (const auto& block : project.typedDataBlocks())
        if (const auto* path = std::get_if<document::PathValue>(&block.payload)) {
            census.count(*path);
            ++census.dataBlockPaths;
        }
    return census;
}

void oldProjectOpensAndRendersIdentically() {
    const auto archive = readFixture("one-contour-paths-1.42.bloom");
    auto opening = project::openProjectArchive(archive, {}, memory());
    expect(opening.outcome() == project::OpenArchiveOutcome::Opened,
           "a project the older writer saved opens");
    if (opening.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(opening).takeOpened();
    expect(opened.documentSchemaVersion == document::SchemaVersion{1, 42},
           "the fixture really is a 1.42 document");
    const auto snapshot = opened.document->snapshot();
    expect(snapshot.project().validate().ok(), "the migrated project validates");
    const auto composition = snapshot.project().compositions().front().id();

    // Every path surface the fixture carries: a closed self-crossing shape, an open shape, the
    // mask's constant and three keys, a remap curve, a Curves node and a data block.
    const auto census = censusOf(snapshot.project(), composition);
    expect(census.paths == census.oneContour + census.empty,
           "every path in the older project loads as a compound value of exactly one contour");
    expect(census.oneContour >= 8,
           "the fixture exercises the shape, mask, field and curve surfaces");
    expect(census.dataBlockPaths == 1, "the data block's path loads");
    expect(census.keyedPaths == 3, "the mask's three keys load");

    const auto frames = renderFrames(snapshot, composition);
    expectBaselineFrames(frames, true);

    // The re-encoded document is the older document with only its minor advanced: no value on any
    // surface changed its spelling, so nothing downstream of the bytes can tell.
    auto older = readText("one-contour-paths-1.42.document.json");
    replaceOnce(older, "\"minor\": 42", "\"minor\": 43");
    expect(encode(snapshot, opened.colorSettings) == older,
           "a one-contour project re-encodes byte for byte apart from its minor");

    // Save and reopen.
    auto saved = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &opened.colorSettings}, {}, memory());
    expect(static_cast<bool>(saved), "the migrated project saves and verifies");
    if (!saved)
        return;
    auto reopening = project::openProjectArchive(saved.archive()->bytes(), {}, memory());
    expect(reopening.outcome() == project::OpenArchiveOutcome::Opened,
           "the migrated project reopens");
    if (reopening.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(reopening).takeOpened();
    expect(reopened.documentSchemaVersion == document::SchemaVersion{1, 43},
           "the migrated project saves at the current schema");
    const auto again = reopened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(composition);
    const auto& after = *again.project().findComposition(composition);
    expect(std::ranges::equal(before.parameters().records(), after.parameters().records()) &&
               std::ranges::equal(before.animationCurves().records(),
                                  after.animationCurves().records()) &&
               std::ranges::equal(snapshot.project().typedDataBlocks(),
                                  again.project().typedDataBlocks()),
           "every path equals itself after save and reopen");
    expect(renderFrames(again, composition) == frames,
           "save and reopen renders exactly the same frames");
    expect(encode(again, reopened.colorSettings) == older,
           "a second save writes the same document");
}

// The older build's hold, asked for explicitly: the second key made a Hold key renders every frame
// as that build did.
void aHoldKeyRendersAsTheOlderBuildHeld() {
    auto opening =
        project::openProjectArchive(readFixture("one-contour-paths-1.42.bloom"), {}, memory());
    expect(opening.outcome() == project::OpenArchiveOutcome::Opened, "the fixture opens again");
    if (opening.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(opening).takeOpened();
    auto& document = *opened.document;
    const auto composition = document.snapshot().project().compositions().front().id();
    std::optional<document::AnimationCurveId> curveId;
    std::optional<document::KeyframeId> second;
    for (const auto& record :
         document.snapshot().project().findComposition(composition)->animationCurves().records()) {
        const auto* keyed = std::get_if<document::PathAnimationCurve>(&record);
        if (keyed == nullptr)
            continue;
        curveId = keyed->id;
        for (const auto& key : keyed->keyframes)
            if (key.time == core::RationalTime::fromInteger(1))
                second = key.id;
    }
    expect(curveId.has_value() && second.has_value(), "the mask's second key is found");
    if (!curveId.has_value() || !second.has_value())
        return;
    commands::CommandStack stack{document};
    commands::Transaction transaction("Hold", document.snapshot().revision());
    transaction.emplace<commands::SetKeyframeInterpolation>(composition, *curveId, *second,
                                                            document::KeyframeInterpolation::Hold);
    expect(stack.execute(std::move(transaction)).succeeded(), "the second key becomes a Hold key");
    expectBaselineFrames(renderFrames(document.snapshot(), composition), false);
}

void theMigrationStepOnlyAdvancesTheVersion() {
    const auto text = readText("one-contour-paths-1.42.document.json");
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    expect(static_cast<bool>(parsed), "the older document parses");
    if (!parsed)
        return;
    const auto migrated =
        project::migrateDocumentDom(parsed.document()->root(), {1, 42}, {1, 43},
                                    project::kProductionDocumentMigrationSteps, {}, operation);
    expect(migrated.outcome() == project::MigrationOutcome::Migrated &&
               migrated.stepsApplied() == 1 &&
               static_cast<bool>(project::decodeDocumentEnvelope(*migrated.migratedRoot())),
           "1.42 -> 1.43 is one step and the result decodes");
}

void olderDocumentsRefuseTheNewSpelling() {
    auto text = readText("one-contour-paths-1.42.document.json");
    expect(static_cast<bool>(decodeText(text)), "the older document decodes");

    // The first path in the document, respelled with contours.
    const auto kind = text.find("\"kind\": \"path\"");
    const auto anchors = text.find("\"anchors\"", kind);
    expect(kind != std::string::npos && anchors != std::string::npos, "the fixture has a path");
    const auto closedAt = text.find("\"closed\"", anchors);
    const auto closedEnd = text.find_first_of(",}", closedAt);
    const auto anchorText = text.substr(anchors, closedAt - anchors);
    const auto closedText = text.substr(closedAt, closedEnd - closedAt);
    auto spelled = text;
    spelled.replace(anchors, closedEnd - anchors,
                    "\"contours\": [{" + anchorText + closedText + "}]");
    expect(!static_cast<bool>(decodeText(spelled)),
           "a 1.42 document carrying the contours spelling is refused");

    // At 1.43 the flat form stays the one spelling of a one-contour value.
    auto current = spelled;
    replaceOnce(current, "\"minor\": 42", "\"minor\": 43");
    expect(!static_cast<bool>(decodeText(current)),
           "a one-contour value spelled as contours is refused: one value has one spelling");
    auto flat = text;
    replaceOnce(flat, "\"minor\": 42", "\"minor\": 43");
    expect(static_cast<bool>(decodeText(flat)), "the flat spelling stays current");
}

// Shape Source's path could not take a driver before 1.43.
void olderDocumentsRefuseADrivenShapePath() {
    auto text = readText("one-contour-paths-1.42.document.json");
    const auto key = text.find("\"schemaKey\": \"bloom.shape.path\"");
    const auto open = text.find('{', text.find("\"source\"", key));
    auto close = open;
    for (int depth = 0; close < text.size(); ++close) {
        depth += text[close] == '{' ? 1 : text[close] == '}' ? -1 : 0;
        if (depth == 0)
            break;
    }
    text.replace(open, close - open + 1,
                 R"({"kind": "driver", "sourceNodeId": "1", "outputPort": "value"})");
    expect(!static_cast<bool>(decodeText(text)),
           "a 1.42 document with a driven shape path is refused");
    replaceOnce(text, "\"minor\": 42", "\"minor\": 43");
    expect(static_cast<bool>(decodeText(text)), "a 1.43 document may drive a shape's path");
}

} // namespace

int main() {
    try {
        oldProjectOpensAndRendersIdentically();
        aHoldKeyRendersAsTheOlderBuildHeld();
        theMigrationStepOnlyAdvancesTheVersion();
        olderDocumentsRefuseTheNewSpelling();
        olderDocumentsRefuseADrivenShapePath();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
