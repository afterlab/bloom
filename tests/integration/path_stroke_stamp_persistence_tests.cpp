// #509: a Path Stroke with a turned Square stamp survives a save and open with its ids and values,
// and draws what it drew: the CPU reference frame is bit-identical. #759: so does a dash pattern,
// and a single dash and gap saved before patterns opens, saves and draws unchanged.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
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

constexpr std::array<std::string_view, 3> kStampRoles{"stamp", "stampAlign", "stampRotation"};

// A curved open path, stroked with brush dots, drawn by a Shape layer.
struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Stamps", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(96, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId stroke;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Stamps", document.snapshot().revision());
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
    [[nodiscard]] const document::NodeRecord* node(const document::NodeId id) const {
        return document.snapshot().project().findComposition(composition)->graph().findNode(id);
    }
    document::ParameterId parameter(const document::NodeId id, const std::string_view role) const {
        for (const auto& binding : node(id)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }
    void set(const document::NodeId id, const std::string_view role,
             document::ParameterValue value) {
        run<commands::SetParameterSource>(composition, parameter(id, role),
                                          document::ConstantValueSource{std::move(value)});
    }
    Fixture() {
        commands::ShapeLayerGeometry geometry;
        document::PathContour placeholder;
        placeholder.closed = true;
        placeholder.anchors = {{{1, 1}, {}, {}}, {{3, 1}, {}, {}}, {{3, 3}, {}, {}}};
        geometry.path = document::PathValue::fromContour(placeholder);
        commands::ShapeLayerStyle style;
        style.fillColor = core::Color4d{0.9, 0.6, 0.2, 1.0};
        run<commands::AddShapeLayer>(composition, document::ShapeKind::Path, geometry, style);
        document::NodeId shape;
        for (const auto& record :
             document.snapshot().project().findComposition(composition)->graph().nodes())
            if (record.typeId == document::kShapeSourceNodeType)
                shape = record.id;
        document::PathContour curve;
        curve.closed = false;
        curve.anchors = {{{8, 40}, {}, document::Vec2d{30, 4}},
                         {{50, 34}, document::Vec2d{40, 60}, {}},
                         {{88, 14}, {}, {}}};
        const auto path = add(document::kPathValueNodeType);
        set(path, document::kValueParameterRole, document::PathValue::fromContour(curve));
        stroke = add(document::kPathStrokeNodeType);
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{path, std::string(document::kValuePortName)},
            document::NodeInputRef{stroke, "path"});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{stroke, "path"},
                                    document::NodeInputRef{shape, "path"});
        set(stroke, "width", 7.0);
        set(stroke, "spacing", 9.0);
    }
};

std::vector<std::byte> save(const document::Snapshot& snapshot,
                            const document::ColorSettings& settings) {
    const auto saved = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    if (!saved)
        throw std::logic_error("archive saves");
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

std::vector<render::Rgba32f> render(const document::Snapshot& snapshot,
                                    const document::CompositionId composition) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({snapshot, composition}, {});
    if (!compiled.plan)
        throw std::runtime_error("Path Stroke fixture compiles");
    auto request =
        runtime::EvaluationRequest{.time = core::RationalTime::fromInteger(0),
                                   .output = compiled.plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = 64U << 20U};
    request.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, {});
    if (!result.frame())
        throw std::runtime_error("Path Stroke fixture evaluates");
    const auto pixels = result.frame()->processImage().pixels();
    return {pixels.begin(), pixels.end()};
}

bool sameBits(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(render::Rgba32f)) == 0);
}

// A turned Square stamp survives a save and open with its ids and values, and draws what it drew.
void squareStampRoundTrips() {
    Fixture fixture;
    const auto dots = render(fixture.document.snapshot(), fixture.composition);
    fixture.set(fixture.stroke, "stamp", std::int64_t{1});
    fixture.set(fixture.stroke, "stampAlign", std::int64_t{1});
    fixture.set(fixture.stroke, "stampRotation", 20.0);
    const auto snapshot = fixture.document.snapshot();
    const auto squares = render(snapshot, fixture.composition);
    expect(!sameBits(dots, squares), "a Square stamp draws another picture");
    auto result = project::openProjectArchive(
        save(snapshot, document::makeBloomNeutralColorSettingsV1({})), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project with a Square stamp opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(fixture.composition);
    bool kept = true;
    for (const auto role : kStampRoles) {
        const auto id = fixture.parameter(fixture.stroke, role);
        const auto* before =
            snapshot.project().findComposition(fixture.composition)->parameters().find(id);
        const auto* after = composition->parameters().find(id);
        kept = kept && before && after && before->source == after->source;
    }
    expect(kept, "the stamp, its alignment and its rotation keep their ids and values");
    expect(sameBits(squares, render(reopened, fixture.composition)),
           "the Square stamp renders bit-identically after a save and open");
}

constexpr std::array<std::string_view, 4> kPatternRoles{"dash2", "gap2", "dash3", "gap3"};

// #759: a single dash and gap saved before dash patterns -- the node has no dash2, gap2, dash3 or
// gap3 binding -- opens with nothing injected, saves back to the same bytes and draws exactly what
// the same stroke with every pattern role bound at its default draws.
void singlePairSavedBeforePatterns() {
    Fixture fixture;
    fixture.set(fixture.stroke, "spacing", 0.0);
    fixture.set(fixture.stroke, "dashLength", 12.0);
    fixture.set(fixture.stroke, "dashGap", 6.0);
    const auto bound = fixture.document.snapshot();
    const auto boundPixels = render(bound, fixture.composition);
    // The node as it was saved before the pattern roles existed.
    auto draft = fixture.document.draft(bound);
    auto* composition = draft.project().findComposition(fixture.composition);
    auto record = *composition->graph().findNode(fixture.stroke);
    std::vector<document::EdgeRecord> edges;
    for (const auto& edge : composition->graph().edges()) {
        const auto* into = std::get_if<document::NodeInputRef>(&edge.destination);
        if (edge.source.nodeId == fixture.stroke ||
            (into != nullptr && into->nodeId == fixture.stroke))
            edges.push_back(edge);
    }
    std::size_t removed = 0;
    std::erase_if(record.parameters, [&](const document::ParameterBinding& binding) {
        if (std::ranges::find(kPatternRoles, binding.role) == kPatternRoles.end())
            return false;
        expect(composition->parameters().erase(binding.parameterId), "a pattern parameter erases");
        ++removed;
        return true;
    });
    expect(removed == kPatternRoles.size(), "a new Path Stroke binds the four pattern roles");
    expect(composition->graph().eraseNode(fixture.stroke) && composition->graph().addNode(record),
           "the stroke is the node saved before patterns");
    for (const auto& edge : edges)
        expect(composition->graph().addEdge(edge), "the stroke's links are kept");
    expect(fixture.document.commit(bound.revision(), std::move(draft)).committed(),
           "a Path Stroke without the pattern roles is a valid document");
    const auto old = fixture.document.snapshot();
    expect(sameBits(boundPixels, render(old, fixture.composition)),
           "unbound pattern roles draw exactly what roles bound at 0 draw");

    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto saved = save(old, settings);
    auto result = project::openProjectArchive(saved, {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project saved before dash patterns opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto* node =
        reopened.project().findComposition(fixture.composition)->graph().findNode(fixture.stroke);
    expect(node != nullptr && std::ranges::none_of(node->parameters,
                                                   [](const auto& binding) {
                                                       return std::ranges::find(kPatternRoles,
                                                                                binding.role) !=
                                                              kPatternRoles.end();
                                                   }),
           "opening injects no pattern role");
    expect(save(reopened, settings) == saved, "it saves back to the same bytes");
    expect(sameBits(boundPixels, render(reopened, fixture.composition)),
           "and draws exactly what it drew");
}

// #759: a dash-dot pattern of three pairs survives a save and open with its ids and values, and
// draws what it drew.
void patternRoundTrips() {
    Fixture fixture;
    fixture.set(fixture.stroke, "spacing", 0.0);
    fixture.set(fixture.stroke, "cap", std::int64_t{1});
    fixture.set(fixture.stroke, "width", 4.0);
    const auto solid = render(fixture.document.snapshot(), fixture.composition);
    fixture.set(fixture.stroke, "dashLength", 10.0);
    fixture.set(fixture.stroke, "dashGap", 6.0);
    fixture.set(fixture.stroke, "dash2", 0.0);
    fixture.set(fixture.stroke, "gap2", 6.0);
    fixture.set(fixture.stroke, "dash3", 3.0);
    fixture.set(fixture.stroke, "gap3", 6.0);
    fixture.set(fixture.stroke, "dashOffset", 5.0);
    const auto snapshot = fixture.document.snapshot();
    const auto pattern = render(snapshot, fixture.composition);
    expect(!sameBits(solid, pattern), "a dash-dot pattern draws another picture");
    auto result = project::openProjectArchive(
        save(snapshot, document::makeBloomNeutralColorSettingsV1({})), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project with a dash pattern opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(fixture.composition);
    bool kept = true;
    for (const auto role : kPatternRoles) {
        const auto id = fixture.parameter(fixture.stroke, role);
        const auto* before =
            snapshot.project().findComposition(fixture.composition)->parameters().find(id);
        const auto* after = composition->parameters().find(id);
        kept = kept && before && after && before->source == after->source;
    }
    expect(kept, "the second and third dash and gap keep their ids and values");
    expect(sameBits(pattern, render(reopened, fixture.composition)),
           "the pattern renders bit-identically after a save and open");
}

} // namespace

int main() {
    try {
        squareStampRoundTrips();
        singlePairSavedBeforePatterns();
        patternRoundTrips();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: a Path Stroke stamp and dash pattern survive a save and open, and a "
                 "stroke saved before dash patterns opens and saves unchanged\n";
    return 0;
}
