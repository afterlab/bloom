// #743: the Layer node's optional pixel snap and the composition's pixel-art mode in a saved
// project.
#include "zip_container_test_support.hpp"

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/resample_filter.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bloom;
using namespace bloom::project;
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}
ProjectIoOperationMemory memory() {
    auto coordinator = ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

constexpr auto kLayer = LayerId::fromRaw(1);

Project makeProject(const std::optional<std::int64_t> snap, const bool pixelArt = false) {
    CanonicalGraph graph{NodeId::fromRaw(1)};
    std::vector<ParameterBinding> layerBindings{
        {"anchor", ParameterId::fromRaw(8)},    {"blendMode", ParameterId::fromRaw(11)},
        {"opacity", ParameterId::fromRaw(3)},   {"position", ParameterId::fromRaw(5)},
        {"rotation", ParameterId::fromRaw(10)}, {"scale", ParameterId::fromRaw(9)}};
    // #743: the optional snap binding, bound like any Layer node parameter.
    if (snap)
        layerBindings.push_back({std::string(kLayerSnapParameterRole), ParameterId::fromRaw(14)});
    const bool nodes = graph.addNode({NodeId::fromRaw(3), std::string(kLayerOutputNodeType),
                                      layerBindings, kLayerOutputNodeSchemaVersion}) &&
                       graph.addNode({NodeId::fromRaw(1),
                                      std::string(kLayerStackNodeType),
                                      {},
                                      kLayerStackNodeSchemaVersion}) &&
                       graph.addNode({NodeId::fromRaw(4),
                                      std::string(kCompositionOutputNodeType),
                                      {},
                                      kCompositionOutputNodeSchemaVersion}) &&
                       graph.addNode({NodeId::fromRaw(2),
                                      std::string(kSolidSourceNodeType),
                                      {{"color", ParameterId::fromRaw(7)},
                                       {"height", ParameterId::fromRaw(13)},
                                       {"width", ParameterId::fromRaw(12)}},
                                      kSolidSourceNodeSchemaVersion});
    const bool edges =
        graph.addEdge(
            {EdgeId::fromRaw(3),
             {NodeId::fromRaw(1), std::string(kLayerStackOutputPort)},
             NodeInputRef{NodeId::fromRaw(4), std::string(kCompositionOutputInputPort)}}) &&
        graph.addEdge(
            {EdgeId::fromRaw(1),
             {NodeId::fromRaw(2), std::string(kSolidSourceOutputPort)},
             NodeInputRef{NodeId::fromRaw(3), std::string(kLayerOutputContentInputPort)}}) &&
        graph.addEdge({EdgeId::fromRaw(2),
                       {NodeId::fromRaw(3), std::string(kLayerOutputOutputPort)},
                       LayerStackInputRef{NodeId::fromRaw(1), LayerSlotId::fromRaw(1),
                                          std::string(kLayerStackContentInputRole)}});
    LayerOutputBoundary boundary{NodeId::fromRaw(3), kLayer, "Layer",
                                 std::string(kLayerOutputOutputPort)};
    if (!nodes || !edges || !graph.addLayerOutput(boundary) ||
        !graph.layerStack().append({LayerSlotId::fromRaw(1), kLayer}))
        throw std::logic_error("layer snap graph");
    graph.setCompositionOutput({NodeId::fromRaw(4), std::string(kCompositionOutputOutputPort)});
    Composition composition{CompositionId::fromRaw(1), "Main", core::RationalTime::fromInteger(10),
                            std::move(graph),
                            *CompositionFormat::create(1920, 1080, core::PixelAspectRatio::square(),
                                                       FrameRate::framesPerSecond24())};
    const bool parameters =
        composition.parameters().insert({ParameterId::fromRaw(12),
                                         std::string(kSolidWidthParameterSchemaKey),
                                         ConstantValueSource{1920.0}}) &&
        composition.parameters().insert({ParameterId::fromRaw(13),
                                         std::string(kSolidHeightParameterSchemaKey),
                                         ConstantValueSource{1080.0}}) &&
        composition.parameters().insert({ParameterId::fromRaw(7),
                                         std::string(kSolidColorParameterSchemaKey),
                                         ConstantValueSource{core::Color4d{0.0, 0.5, 1.0, 1.0}}}) &&
        composition.parameters().insert({ParameterId::fromRaw(5),
                                         std::string(kPositionParameterSchemaKey),
                                         ConstantValueSource{Vec2d{96.0, -48.0}}}) &&
        composition.parameters().insert({ParameterId::fromRaw(3),
                                         std::string(kOpacityParameterSchemaKey),
                                         ConstantValueSource{1.0}}) &&
        composition.parameters().insert({ParameterId::fromRaw(8),
                                         std::string(kAnchorParameterSchemaKey),
                                         ConstantValueSource{kDefaultAnchor}}) &&
        composition.parameters().insert({ParameterId::fromRaw(9),
                                         std::string(kScaleParameterSchemaKey),
                                         ConstantValueSource{kDefaultScale}}) &&
        composition.parameters().insert({ParameterId::fromRaw(10),
                                         std::string(kRotationParameterSchemaKey),
                                         ConstantValueSource{kDefaultRotationDegrees}}) &&
        composition.parameters().insert({ParameterId::fromRaw(11),
                                         std::string(kBlendModeParameterSchemaKey),
                                         ConstantValueSource{kDefaultBlendModeValue}});
    if (snap && !composition.parameters().insert({ParameterId::fromRaw(14),
                                                  std::string(kLayerSnapParameterSchemaKey),
                                                  ConstantValueSource{*snap}}))
        throw std::logic_error("layer snap parameter");
    composition.setPixelArt(pixelArt);
    Project project{ProjectId::fromRaw(1), "Pixel Snap"};
    if (!parameters || !project.addComposition(std::move(composition)))
        throw std::logic_error("layer snap project");
    return project;
}

std::string encode(const Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}

DocumentDecodeResult decodeText(const std::string& text) {
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("layer snap parse");
    return decodeDocumentEnvelope(parsed.document()->root());
}

// The snap binding of the decoded or reopened Layer node, if any.
std::optional<std::int64_t> snapOf(const Composition& composition) {
    const auto* node = composition.graph().findNode(NodeId::fromRaw(3));
    const auto binding = node ? std::ranges::find(node->parameters, kLayerSnapParameterRole,
                                                  &ParameterBinding::role)
                              : decltype(node->parameters.end()){};
    if (node == nullptr || binding == node->parameters.end())
        return std::nullopt;
    const auto* record = composition.parameters().find(binding->parameterId);
    const auto* constant = record ? std::get_if<ConstantValueSource>(&record->source) : nullptr;
    const auto* value = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
    return value ? std::optional(*value) : std::nullopt;
}

// The Layer node and every parameter record of the two projects' compositions are the same.
bool sameLayer(const Project& a, const Project& b) {
    const auto& left = a.compositions().front();
    const auto& right = b.compositions().front();
    const auto* leftNode = left.graph().findNode(NodeId::fromRaw(3));
    const auto* rightNode = right.graph().findNode(NodeId::fromRaw(3));
    const bool nodes = leftNode != nullptr && rightNode != nullptr && *leftNode == *rightNode;
    // The store's records in identity order: a reopened store need not keep insertion order.
    const auto sorted = [](const Composition& composition) {
        std::vector<ParameterRecord> records(composition.parameters().records().begin(),
                                             composition.parameters().records().end());
        std::ranges::sort(records, {}, [](const ParameterRecord& r) { return r.id.value(); });
        return records;
    };
    const bool records = sorted(left) == sorted(right);
    return nodes && records;
}

// Saves `document`, verifies the archive and reopens it.
std::optional<Project> roundTrip(const Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = makeBloomNeutralColorSettingsV1({});
    auto archive = buildVerifiedSaveArchive({}, {.snapshot = &snapshot, .colorSettings = &settings},
                                            {}, memory());
    expect(static_cast<bool>(archive), "the project saves and verifies");
    if (!archive)
        return std::nullopt;
    auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "the project opens");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return std::nullopt;
    auto opened = std::move(result).takeOpened();
    return opened.document->snapshot().project();
}

// A project saved before #743 has no snap binding and no pixel-art mode: it writes neither, and it
// saves and reopens without them, so its layers do not snap.
void absentSnapStaysAbsent() {
    Document document{makeProject(std::nullopt)};
    const auto text = encode(document);
    expect(text.find(std::string(kLayerSnapParameterSchemaKey)) == std::string::npos,
           "a layer without a snap writes no snap parameter");
    expect(text.find("\"pixelArt\"") == std::string::npos,
           "a composition without pixel-art mode writes no pixelArt member");
    expect(static_cast<bool>(decodeText(text)), "a layer without a snap decodes");
    const auto reopened = roundTrip(document);
    expect(reopened && !snapOf(reopened->compositions().front()).has_value() &&
               !reopened->compositions().front().pixelArt() &&
               sameLayer(*reopened, document.snapshot().project()),
           "it reopens without a snap binding or pixel-art mode, the same project");
}

// Every snap value saves and reopens as itself, with pixel-art mode on and off.
void everySnapRoundTrips() {
    for (const bool pixelArt : {false, true})
        for (const std::int64_t snap : {kLayerSnapOff, kLayerSnapOn, kLayerSnapAutomatic}) {
            Document document{makeProject(snap, pixelArt)};
            expect(document.snapshot().project().validate().ok(), "a layer with a snap validates");
            const auto text = encode(document);
            expect(text.find(std::string(kLayerSnapParameterSchemaKey)) != std::string::npos,
                   "a layer with a snap writes its snap parameter");
            expect((text.find("\"pixelArt\": true") != std::string::npos) == pixelArt,
                   "pixel-art mode is written exactly when it is on");
            const auto decoded = decodeText(text);
            expect(static_cast<bool>(decoded), "a layer with a snap decodes");
            const auto reopened = roundTrip(document);
            expect(reopened && snapOf(reopened->compositions().front()) == snap &&
                       reopened->compositions().front().pixelArt() == pixelArt &&
                       sameLayer(*reopened, document.snapshot().project()),
                   "the snap and the pixel-art mode reopen as themselves, in the same project");
        }
}

// A value outside the three snaps is not a valid document, and a pixelArt member that is not
// true is not a canonical one.
void invalidValuesAreRefused() {
    for (const std::int64_t snap : {-1, 3}) {
        bool refused = false;
        try {
            (void)makeProject(snap);
        } catch (const std::logic_error&) {
            refused = true;
        }
        expect(refused, "the parameter store refuses a snap outside 0..2");
    }
    Document document{makeProject(kLayerSnapOn, true)};
    auto text = encode(document);
    const auto at = text.find("\"pixelArt\": true");
    expect(at != std::string::npos, "the pixel-art member is written");
    if (at == std::string::npos)
        return;
    auto notBoolean = text;
    notBoolean.replace(at, std::string_view("\"pixelArt\": true").size(), "\"pixelArt\": 1");
    expect(!decodeText(notBoolean), "a pixelArt member that is not a Boolean is refused");
    auto falseMember = text;
    falseMember.replace(at, std::string_view("\"pixelArt\": true").size(), "\"pixelArt\": false");
    const auto falseResult = decodeText(falseMember);
    expect(!falseResult && falseResult.error() == DocumentDecodeError::DomainViolation,
           "a pixelArt member of false is refused: the member is present only when on");
}

} // namespace

int main() try {
    absentSnapStaysAbsent();
    everySnapRoundTrips();
    invalidValuesAreRefused();
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
