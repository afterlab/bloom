// ADR 0026 persistence: Image source v2 (part, layer, the channels output) and the channel nodes
// save, reopen and decode exactly.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <regex>

#include <algorithm>
#include <array>
#include <iostream>
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
        "Channels", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId source;
    document::NodeId remove;
    document::NodeId shuffle;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Channels", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    document::NodeId add(const std::string_view type) {
        return *run<commands::AddNode>(composition, std::string(type), document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    document::ParameterId parameter(const document::NodeId node, const std::string_view role) {
        const auto* record =
            document.snapshot().project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }
    void setText(const document::NodeId node, const std::string_view role, std::string value) {
        run<commands::SetParameterSource>(
            composition, parameter(node, role),
            document::ConstantValueSource{document::ParameterValue{std::move(value)}});
    }
    Fixture() {
        source = add(document::kImageSourceNodeType);
        remove = add(document::kChannelRemoveNodeType);
        shuffle = add(document::kChannelShuffleNodeType);
        setText(source, document::kImagePartParameterRole, "beauty");
        setText(source, document::kImageLayerParameterRole, "diffuse");
        setText(remove, "select", "depth.*, @id");
        setText(shuffle, "red", "depth.Z");
        run<commands::ConnectPorts>(
            composition,
            document::OutputPortRef{source, std::string(document::kImageSourceChannelsOutputPort)},
            document::NodeInputRef{remove, std::string(document::kChannelSetPort)});
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{remove, std::string(document::kChannelSetPort)},
            document::NodeInputRef{shuffle, std::string(document::kChannelSetPort)});
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
        throw std::runtime_error("channel parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

void roundTrip() {
    Fixture fixture;
    const auto snapshot = fixture.document.snapshot();
    expect(snapshot.project().validate().ok(), "the channel project validates");
    const auto text = encode(fixture.document);
    expect(text.find("\"minor\": 0") != std::string::npos, "the document is written at 1.0");
    expect(text.find("\"bloom.channel-shuffle\"") != std::string::npos &&
               text.find("\"bloom.image.part\"") != std::string::npos,
           "the channel nodes and the image part parameter are written");
    expect(static_cast<bool>(decodeText(text)), "the document decodes");

    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(archive), "the channel archive saves and verifies");
    if (!archive)
        return;
    auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the channel archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(fixture.composition);
    const auto& after = *reopened.project().findComposition(fixture.composition);
    // Bindings are written in canonical role order, so compare them as sets.
    const auto sameNodes = std::ranges::all_of(before.graph().nodes(), [&](const auto& node) {
        const auto* match = after.graph().findNode(node.id);
        return match != nullptr && match->typeId == node.typeId &&
               match->schemaVersion == node.schemaVersion &&
               match->parameters.size() == node.parameters.size() &&
               std::ranges::all_of(node.parameters, [&](const auto& binding) {
                   return std::ranges::find(match->parameters, binding) != match->parameters.end();
               });
    });
    const auto sameEdges =
        before.graph().edges().size() == after.graph().edges().size() &&
        std::ranges::all_of(before.graph().edges(), [&](const auto& edge) {
            return std::ranges::find(after.graph().edges(), edge) != after.graph().edges().end();
        });
    const auto sameParameters =
        std::ranges::all_of(before.parameters().records(), [&](const auto& record) {
            const auto* match = after.parameters().find(record.id);
            return match != nullptr && *match == record;
        });
    expect(sameNodes, "nodes and their part/layer/select bindings equal themselves after reopen");
    expect(sameEdges, "the channel edges equal themselves after reopen");
    expect(sameParameters, "the part/layer/select parameters equal themselves after reopen");
    expect(reopened.project().validate().ok(), "the reopened project validates");
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
