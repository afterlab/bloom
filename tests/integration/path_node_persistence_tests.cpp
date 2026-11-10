// #344 #530 #509 #349: the path processing nodes (Transform Path, Trim Path, Offset Path, Round
// Corners, Boolean Paths, Merge Paths, Path Stroke with its stamps, Mask Path, Text Outline) and
// the analysis nodes (Histogram, Image Compare, Image Statistics) save, reopen and save again with
// every parameter, selector, link and layout field exactly as authored, and an edit of a selector
// or a Path operand undoes and redoes.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace bloom;
int failures = 0;
void expect(const bool condition, const std::string& message) {
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

document::PathValue twoContours(const double size) {
    document::PathValue path;
    document::PathContour outer;
    outer.closed = true;
    outer.anchors = {{{0.0, 0.0}, std::nullopt, std::nullopt},
                     {{size, 0.0}, std::nullopt, document::Vec2d{size, 5.0}},
                     {{size, size}, document::Vec2d{size - 3.0, size + 4.0}, std::nullopt},
                     {{0.0, size}, std::nullopt, std::nullopt}};
    document::PathContour open;
    open.closed = false;
    open.anchors = {{{size / 4.0, size / 4.0}, std::nullopt, std::nullopt},
                    {{size / 2.0, size / 3.0}, std::nullopt, std::nullopt}};
    path.contours = {outer, open};
    path.fillRule = document::PathFillRule::EvenOdd;
    return path;
}

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Path nodes", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Path nodes", document.snapshot().revision());
        transaction.emplace<Operation>(composition, std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    bool tryRun(commands::Transaction&& transaction) {
        return commands.execute(std::move(transaction)).succeeded();
    }
    document::NodeId add(const std::string_view type, const document::Vec2d where) {
        return *run<commands::AddNode>(std::string(type), where)
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    [[nodiscard]] const document::NodeRecord& node(const document::NodeId id) const {
        static const document::NodeRecord none{};
        const auto snapshot = document.snapshot();
        const auto* record =
            snapshot.project().findComposition(composition)->graph().findNode(id);
        return record == nullptr ? none : *record;
    }
    [[nodiscard]] document::ParameterId parameter(const document::NodeId id,
                                                  const std::string_view role) const {
        for (const auto& binding : node(id).parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("fixture role missing: " + std::string(role));
    }
    [[nodiscard]] document::ParameterSource source(const document::ParameterId id) const {
        const auto snapshot = document.snapshot();
        return snapshot.project().findComposition(composition)->parameters().find(id)->source;
    }
    // Moves a parameter off its default, trying the candidates a domain may accept.
    void change(const document::NodeId id, const document::ParameterDefinition& declared) {
        const auto parameterId = parameter(id, declared.role);
        std::vector<document::ParameterValue> candidates;
        switch (declared.valueKind) {
        case document::ParameterValueKind::Float64: {
            const double base = std::get<double>(declared.defaultValue);
            candidates = {base + 3.5, base + 0.5, base + 0.125, base * 0.5 + 0.25};
            break;
        }
        case document::ParameterValueKind::Integer:
            candidates = {std::int64_t{2}, std::int64_t{1}, std::int64_t{3}};
            break;
        case document::ParameterValueKind::Boolean:
            candidates = {!std::get<bool>(declared.defaultValue)};
            break;
        case document::ParameterValueKind::Vec2d:
            candidates = {document::Vec2d{7.5, -3.25}};
            break;
        case document::ParameterValueKind::Path:
            candidates = {twoContours(40.0)};
            break;
        default:
            return;
        }
        for (const auto& candidate : candidates) {
            commands::Transaction transaction("Set", document.snapshot().revision());
            transaction.emplace<commands::SetParameterSource>(
                composition, parameterId, document::ConstantValueSource{candidate});
            if (tryRun(std::move(transaction)) &&
                !(std::get<document::ConstantValueSource>(source(parameterId)).value ==
                  declared.defaultValue))
                return;
        }
        throw std::runtime_error("no candidate was accepted for " + declared.role);
    }
    void connect(const document::NodeId from, const std::string_view output,
                 const document::NodeId to, const std::string_view input) {
        run<commands::ConnectPorts>(document::OutputPortRef{from, std::string(output)},
                                    document::NodeInputRef{to, std::string(input)});
    }
};

std::string encode(const document::Snapshot& snapshot) {
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::vector<char> payloadScratch(1U << 16U);
    std::vector<std::size_t> sortScratch(1U << 14U);
    const project::CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = project::canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!project::encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}

// Saves through the real archive writer and opens it again.
std::optional<project::OpenedArchive> saveAndOpen(const document::Snapshot& snapshot) {
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(archive), "the archive saves and verifies");
    if (!archive)
        return std::nullopt;
    auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the archive reopens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return std::nullopt;
    return std::move(result).takeOpened();
}

void pathAndAnalysisNodesReopen() {
    Fixture fixture;
    const std::array pathNodes{document::kTransformPathNodeType, document::kTrimPathNodeType,
                               document::kOffsetPathNodeType,    document::kRoundCornersNodeType,
                               document::kBooleanPathsNodeType,  document::kMergePathsNodeType,
                               document::kPathStrokeNodeType,    document::kMaskPathNodeType,
                               document::kTextOutlineNodeType};
    const std::array analysisNodes{document::kHistogramNodeType, document::kImageCompareNodeType,
                                   document::kImageStatisticsNodeType};
    std::vector<document::NodeId> all;
    double x = 0.0;
    // Path producers, each processing node with every parameter and selector moved off its
    // default, and a Shape Source it feeds.
    for (const auto type : pathNodes) {
        const auto* definition = document::builtInNodeDefinitions().find(type, 1);
        expect(definition != nullptr, std::string("a definition exists for ") + std::string(type));
        if (definition == nullptr)
            continue;
        // #509 Mask Path and Text Outline read a node by its id and take no path in.
        const bool reader =
            type == document::kMaskPathNodeType || type == document::kTextOutlineNodeType;
        const auto processor = fixture.add(type, {x, 120.0});
        for (const auto& declared : definition->parameters)
            fixture.change(processor, declared);
        if (reader) {
            const auto shape = fixture.add(document::kShapeSourceNodeType, {x, 360.0});
            fixture.connect(processor, "path", shape, "path");
            all.insert(all.end(), {processor, shape});
            x += 240.0;
            continue;
        }
        const auto producer = fixture.add(document::kPathValueNodeType, {x, 0.0});
        fixture.change(producer, *document::builtInNodeDefinitions()
                                      .find(document::kPathValueNodeType, 1)
                                      ->parameters.data());
        fixture.connect(producer, document::kValuePortName, processor,
                        type == document::kBooleanPathsNodeType ? "first" : "path");
        if (type == document::kBooleanPathsNodeType) {
            const auto second = fixture.add(document::kPathValueNodeType, {x, 240.0});
            fixture.change(second, *document::builtInNodeDefinitions()
                                        .find(document::kPathValueNodeType, 1)
                                        ->parameters.data());
            fixture.connect(second, document::kValuePortName, processor, "second");
            all.push_back(second);
        }
        const auto shape = fixture.add(document::kShapeSourceNodeType, {x, 360.0});
        fixture.connect(processor, "path", shape, "path");
        all.insert(all.end(), {producer, processor, shape});
        x += 240.0;
    }
    // Image analysis: an image into each, and a second image into Image Compare.
    for (const auto type : analysisNodes) {
        const auto* definition = document::builtInNodeDefinitions().find(type, 1);
        expect(definition != nullptr, std::string("a definition exists for ") + std::string(type));
        if (definition == nullptr)
            continue;
        const auto image = fixture.add(document::kSolidSourceNodeType, {x, 0.0});
        const auto analyser = fixture.add(type, {x, 140.0});
        for (const auto& declared : definition->parameters)
            fixture.change(analyser, declared);
        fixture.connect(image, "image", analyser, "input");
        if (type == document::kImageCompareNodeType) {
            const auto other = fixture.add(document::kSolidSourceNodeType, {x, 280.0});
            fixture.connect(other, "image", analyser, "second");
            all.push_back(other);
        }
        all.insert(all.end(), {image, analyser});
        x += 240.0;
    }
    const auto before = fixture.document.snapshot();
    expect(before.project().validate().ok(), "the project with the path and analysis nodes validates");
    const auto text = encode(before);

    const auto opened = saveAndOpen(before);
    if (!opened)
        return;
    const auto reopened = opened->document->snapshot();
    expect(reopened.project().validate().ok(), "the reopened project validates");
    const auto& oldComposition = *before.project().findComposition(fixture.composition);
    const auto& newComposition = *reopened.project().findComposition(fixture.composition);
    int compared = 0;
    for (const auto id : all) {
        const auto* oldNode = oldComposition.graph().findNode(id);
        const auto* newNode = newComposition.graph().findNode(id);
        expect(oldNode != nullptr && newNode != nullptr, "a node survives the reopen");
        if (oldNode == nullptr || newNode == nullptr)
            continue;
        auto canonical = *oldNode;
        // Bindings are written in role order.
        std::ranges::sort(canonical.parameters, {}, &document::ParameterBinding::role);
        expect(canonical == *newNode, "node " + oldNode->typeId + " and its binding identities survive");
        for (const auto& binding : oldNode->parameters) {
            const auto* oldParameter = oldComposition.parameters().find(binding.parameterId);
            const auto* newParameter = newComposition.parameters().find(binding.parameterId);
            expect(oldParameter != nullptr && newParameter != nullptr &&
                       oldParameter->schemaKey == newParameter->schemaKey &&
                       oldParameter->source == newParameter->source,
                   "parameter " + binding.role + " of " + oldNode->typeId +
                       " keeps its value exactly");
            ++compared;
        }
        const auto oldLayout = oldComposition.nodeLayout().find(id);
        const auto newLayout = newComposition.nodeLayout().find(id);
        expect(oldLayout != oldComposition.nodeLayout().end() &&
                   newLayout != newComposition.nodeLayout().end() &&
                   oldLayout->second == newLayout->second,
               "the layout of " + oldNode->typeId + " survives");
    }
    expect(compared > 60, "many parameter values were compared (" + std::to_string(compared) + ")");
    expect(std::ranges::equal(oldComposition.graph().edges(), newComposition.graph().edges()) &&
               !oldComposition.graph().edges().empty(),
           "every link (path into the node, the node into Shape Source, images into the analysers) survives");
    // Save, reopen, save again: the canonical document does not drift.
    expect(encode(reopened) == text, "save and reopen leaves the canonical document byte identical");
    const auto again = saveAndOpen(reopened);
    if (again)
        expect(encode(again->document->snapshot()) == text,
               "a second save and reopen leaves the canonical document byte identical");
}

// An edit of a selector and of a Path operand of Trim Path and Boolean Paths undoes and redoes.
void selectorAndPathEditsUndoAndRedo() {
    Fixture fixture;
    for (const auto type : {document::kTrimPathNodeType, document::kBooleanPathsNodeType}) {
        const auto node = fixture.add(type, {0.0, 0.0});
        const auto selectorRole =
            type == document::kTrimPathNodeType ? "mode" : "operation";
        const auto operandRole = type == document::kTrimPathNodeType ? "path" : "first";
        for (const auto role : {selectorRole, operandRole}) {
            const auto parameter = fixture.parameter(node, role);
            const auto original = fixture.source(parameter);
            const document::ParameterValue edited =
                std::string_view(role) == selectorRole
                    ? document::ParameterValue{std::int64_t{1}}
                    : document::ParameterValue{twoContours(30.0)};
            fixture.run<commands::SetParameterSource>(parameter,
                                                      document::ConstantValueSource{edited});
            expect(fixture.source(parameter) == document::ParameterSource{document::ConstantValueSource{edited}},
                   std::string(type) + " " + role + ": the edit is applied");
            expect(fixture.commands.undo().succeeded(),
                   std::string(type) + " " + role + ": the edit undoes");
            expect(fixture.source(parameter) == original,
                   std::string(type) + " " + role + ": undo restores the original source");
            expect(fixture.commands.redo().succeeded(),
                   std::string(type) + " " + role + ": the edit redoes");
            expect(fixture.source(parameter) == document::ParameterSource{document::ConstantValueSource{edited}},
                   std::string(type) + " " + role + ": redo restores the edit");
        }
    }
}

// #509: the stamp controls, and the source and mask of Mask Path and Text Outline, undo and redo.
void stampAndSourceEditsUndoAndRedo() {
    Fixture fixture;
    const std::vector<std::tuple<std::string_view, std::string_view, document::ParameterValue>>
        edits{{document::kPathStrokeNodeType, "stamp", std::int64_t{3}},
              {document::kPathStrokeNodeType, "stampAlign", std::int64_t{2}},
              {document::kPathStrokeNodeType, "stampRotation", 30.0},
              {document::kMaskPathNodeType, "source", std::int64_t{7}},
              {document::kMaskPathNodeType, "mask", std::int64_t{2}},
              {document::kTextOutlineNodeType, "source", std::int64_t{9}}};
    for (const auto& [type, role, edited] : edits) {
        const auto node = fixture.add(type, {0.0, 0.0});
        const auto parameter = fixture.parameter(node, role);
        const auto original = fixture.source(parameter);
        const std::string label = std::string(type) + " " + std::string(role);
        fixture.run<commands::SetParameterSource>(parameter, document::ConstantValueSource{edited});
        const document::ParameterSource applied{document::ConstantValueSource{edited}};
        expect(fixture.source(parameter) == applied, label + ": the edit is applied");
        expect(fixture.commands.undo().succeeded() && fixture.source(parameter) == original,
               label + ": undo restores the original");
        expect(fixture.commands.redo().succeeded() && fixture.source(parameter) == applied,
               label + ": redo restores the edit");
    }
}

} // namespace

int main() {
    try {
        pathAndAnalysisNodesReopen();
        selectorAndPathEditsUndoAndRedo();
        stampAndSourceEditsUndoAndRedo();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
