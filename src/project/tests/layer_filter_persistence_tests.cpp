// #742: the Layer node's optional resample filter in a saved project.
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

Project makeProject(const std::optional<std::int64_t> filter) {
    CanonicalGraph graph{NodeId::fromRaw(1)};
    std::vector<ParameterBinding> layerBindings{
        {"anchor", ParameterId::fromRaw(8)},    {"blendMode", ParameterId::fromRaw(11)},
        {"opacity", ParameterId::fromRaw(3)},   {"position", ParameterId::fromRaw(5)},
        {"rotation", ParameterId::fromRaw(10)}, {"scale", ParameterId::fromRaw(9)}};
    // #742: the optional filter binding, bound like any Layer node parameter.
    if (filter)
        layerBindings.insert(layerBindings.begin() + 2,
                             {std::string(kLayerFilterParameterRole), ParameterId::fromRaw(14)});
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
        throw std::logic_error("layer filter graph");
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
    if (filter && !composition.parameters().insert({ParameterId::fromRaw(14),
                                                    std::string(kLayerFilterParameterSchemaKey),
                                                    ConstantValueSource{*filter}}))
        throw std::logic_error("layer filter parameter");
    Project project{ProjectId::fromRaw(1), "Layer Filter"};
    if (!parameters || !project.addComposition(std::move(composition)))
        throw std::logic_error("layer filter project");
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
        throw std::runtime_error("layer filter parse");
    return decodeDocumentEnvelope(parsed.document()->root());
}

// The filter binding of the decoded or reopened Layer node, if any.
std::optional<std::int64_t> filterOf(const Composition& composition) {
    const auto* node = composition.graph().findNode(NodeId::fromRaw(3));
    const auto binding = node ? std::ranges::find(node->parameters, kLayerFilterParameterRole,
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

// A project saved before #742 has no filter binding: it writes none, and it saves and reopens
// without one, so its layers stay Bilinear.
void absentFilterStaysAbsent() {
    Document document{makeProject(std::nullopt)};
    const auto text = encode(document);
    expect(text.find(std::string(kLayerFilterParameterSchemaKey)) == std::string::npos,
           "a layer without a filter writes no filter parameter");
    const auto decoded = decodeText(text);
    expect(static_cast<bool>(decoded), "a layer without a filter decodes");
    const auto snapshot = document.snapshot();
    const auto settings = makeBloomNeutralColorSettingsV1({});
    auto archive = buildVerifiedSaveArchive({}, {.snapshot = &snapshot, .colorSettings = &settings},
                                            {}, memory());
    expect(static_cast<bool>(archive), "a layer without a filter saves and verifies");
    if (!archive)
        return;
    auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "a layer without a filter opens");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    expect(!filterOf(opened.document->snapshot().project().compositions().front()).has_value() &&
               sameLayer(opened.document->snapshot().project(), snapshot.project()),
           "it reopens without a filter binding, the same project");
}

// Every filter value saves and reopens as itself.
void everyFilterRoundTrips() {
    for (const std::int64_t filter : {0, 1, 2, 3, 4}) {
        Document document{makeProject(filter)};
        const auto snapshot = document.snapshot();
        expect(snapshot.project().validate().ok(), "a layer with a filter validates");
        const auto text = encode(document);
        expect(text.find(std::string(kLayerFilterParameterSchemaKey)) != std::string::npos,
               "a layer with a filter writes its filter parameter");
        expect(static_cast<bool>(decodeText(text)), "a layer with a filter decodes");
        const auto settings = makeBloomNeutralColorSettingsV1({});
        auto archive = buildVerifiedSaveArchive(
            {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
        expect(static_cast<bool>(archive), "a layer with a filter saves and verifies");
        if (!archive)
            continue;
        auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
        expect(result.outcome() == OpenArchiveOutcome::Opened, "a layer with a filter opens");
        if (result.outcome() != OpenArchiveOutcome::Opened)
            continue;
        auto opened = std::move(result).takeOpened();
        const auto& reopened = opened.document->snapshot().project();
        expect(filterOf(reopened.compositions().front()) == filter &&
                   sameLayer(reopened, snapshot.project()),
               "the filter reopens as itself, in the same project");
    }
}

// A value outside the five filters is not a valid document.
void invalidFilterIsRefused() {
    for (const std::int64_t filter : {-1, 5}) {
        bool refused = false;
        try {
            (void)makeProject(filter);
        } catch (const std::logic_error&) {
            refused = true;
        }
        expect(refused, "the parameter store refuses a filter outside 0..4");
    }
}

} // namespace

int main() try {
    absentFilterStaysAbsent();
    everyFilterRoundTrips();
    invalidFilterIsRefused();
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
