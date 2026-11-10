// #599 persistence: a Line Shape Source's animatable ends. Keyed ends and an end driven by a
// Vector2 value node save and reopen exactly (every key, the driver edge, bit for bit, a reopened
// project re-saves byte-identically); a project saved with constant ends, which is the only
// spelling an older writer knew, opens unchanged and re-saves differing only in the schema minor, so
// the schema minor does not move for this change.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

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

core::RationalTime second(const std::int64_t value) {
    return core::RationalTime::fromInteger(value);
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Line ends", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::CompositionId composition = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::NodeId shape;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Line ends", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    [[nodiscard]] const document::Composition& main() const {
        return *document.snapshot().project().findComposition(composition);
    }
    [[nodiscard]] document::ParameterId parameter(const document::NodeId node,
                                                  const std::string_view role) const {
        for (const auto& binding : main().graph().findNode(node)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("fixture parameter is missing");
    }

    // `animated`: lineEnd keyed at 0 s and 1 s and lineStart driven by a Vector2 value node.
    explicit Fixture(const bool animated) {
        commands::ShapeLayerGeometry geometry;
        geometry.lineStart = document::Vec2d{4, 6};
        geometry.lineEnd = document::Vec2d{40, 12};
        run<commands::AddShapeLayer>(composition, document::ShapeKind::Line, geometry);
        const auto nodes = main().graph().nodes();
        for (const auto& node : nodes)
            if (node.typeId == document::kShapeSourceNodeType)
                shape = node.id;
        if (!animated)
            return;
        const auto end = parameter(shape, "lineEnd");
        run<commands::CreateAnimationForParameter>(composition, end, second(0));
        run<commands::SetKeyframeAtTimeForParameter>(composition, end, second(1),
                                                    document::Vec2d{10, 50});
        const auto vector = run<commands::AddNode>(composition,
                                                  std::string(document::kVector2ValueNodeType),
                                                  document::Vec2d{0, 0})
                                .outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!vector)
            throw std::runtime_error("vector node");
        run<commands::ConnectPorts>(
            composition,
            document::OutputPortRef{*vector, std::string(document::kValuePortName)},
            document::NodeInputRef{shape, "lineStart"});
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


project::DocumentDecodeResult decode(const std::string& text) {
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

std::string withMinor(std::string text, const int from, const int to) {
    const auto from_ = "\"minor\": " + std::to_string(from) + "\n  },\n  \"project\"";
    const auto at = text.find(from_);
    if (at == std::string::npos)
        throw std::runtime_error("version anchor");
    text.replace(at, from_.size(), "\"minor\": " + std::to_string(to) + "\n  },\n  \"project\"");
    return text;
}

void roundTrip() {
    Fixture fixture(true);
    const auto snapshot = fixture.document.snapshot();
    const auto archive = save(snapshot);
    const auto text = documentText(archive);
    expect(text.find("\"minor\": 43") != std::string::npos,
           "the document is written at the current minor (1.43): the schema does not move");
    auto result = project::openProjectArchive(archive, {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(fixture.composition);
    const auto& after = *reopened.project().findComposition(fixture.composition);
    expect(std::ranges::equal(before.animationCurves().records(), after.animationCurves().records()),
           "the keyed end's curve reopens bit for bit");
    const auto* end = after.parameters().find(fixture.parameter(fixture.shape, "lineEnd"));
    expect(end != nullptr && std::holds_alternative<document::AnimationCurveSource>(end->source),
           "lineEnd reopens keyed");
    const auto* start = after.parameters().find(fixture.parameter(fixture.shape, "lineStart"));
    expect(start != nullptr && std::holds_alternative<document::DriverBindingSource>(start->source),
           "lineStart reopens driven");
    expect(save(reopened) == archive, "a reopened project re-saves byte-identically");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({reopened, fixture.composition}, {});
    expect(compiled.plan != nullptr &&
               compiled.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "the reopened project compiles and its Line is time dependent");
}

void olderFilesAreUnchanged() {
    Fixture fixture(false);
    const auto snapshot = fixture.document.snapshot();
    const auto text = documentText(save(snapshot));
    // The same document labelled 1.42 is exactly what the previous writer produced: constants.
    const auto older = withMinor(text, 43, 42);
    expect(static_cast<bool>(decode(older)), "a 1.42 document with constant ends decodes");
    const project::CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 42}};
    const auto size = project::canonicalManifestSize(manifest);
    if (!size)
        throw std::runtime_error("manifest size");
    std::vector<char> encoded(*size.value());
    if (!project::encodeCanonicalManifest(manifest, encoded))
        throw std::runtime_error("manifest encode");
    const auto archive = project::test::buildConformingArchive(
        project::test::makeStoredEntry("manifest.json",
                                       project::test::toBytes({encoded.data(), encoded.size()})),
        project::test::makeStoredEntry("document.json", project::test::toBytes(older)));
    auto result = project::openProjectArchive(archive, {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the 1.42 archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& restored = *reopened.project().findComposition(fixture.composition);
    for (const auto* role : {"lineStart", "lineEnd"}) {
        const auto* parameter = restored.parameters().find(fixture.parameter(fixture.shape, role));
        expect(parameter != nullptr &&
                   std::holds_alternative<document::ConstantValueSource>(parameter->source),
               "an older project's ends open as the constants they were");
    }
    expect(documentText(save(reopened)) == text,
           "re-saving a 1.42 project with constant ends changes only the schema minor");
}

} // namespace

int main() {
    try {
        roundTrip();
        olderFilesAreUnchanged();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
