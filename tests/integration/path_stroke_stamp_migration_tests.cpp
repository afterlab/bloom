// #509: a project saved with a Path Stroke from before stamps (no `stamp`, `stampAlign` or
// `stampRotation` binding) opens with them bound at Dot, Automatic and 0, so Properties and the
// card show the same rows as a new node, and it draws exactly the round brush dots it drew before
// it was saved: the CPU reference frame is bit-identical. A node that already has a Square stamp
// keeps its ids and values, and draws what it drew.

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
    // Removes the bindings (and records) a Path Stroke saved before stamps never had.
    void makeLegacy() {
        const auto base = document.snapshot();
        auto draft = document.draft(base);
        auto* target = draft.project().findComposition(composition);
        auto* record = target->graph().findNode(stroke);
        std::erase_if(record->parameters, [&](const document::ParameterBinding& binding) {
            if (std::ranges::find(kStampRoles, binding.role) == kStampRoles.end())
                return false;
            if (!target->parameters().erase(binding.parameterId))
                throw std::logic_error("legacy record erases");
            return true;
        });
        if (!document.commit(base.revision(), std::move(draft)).committed())
            throw std::logic_error("legacy commit");
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

void legacyStrokeOpensUnchanged() {
    Fixture fixture;
    const auto newSize = fixture.node(fixture.stroke)->parameters.size();
    const auto width = fixture.parameter(fixture.stroke, "width");
    const auto current = render(fixture.document.snapshot(), fixture.composition);
    fixture.makeLegacy();
    const auto legacy = fixture.document.snapshot();
    expect(legacy.project().validate().ok(), "a Path Stroke without the stamp bindings validates");
    const auto before = render(legacy, fixture.composition);
    bool lit = false;
    for (const auto& pixel : before)
        lit = lit || pixel.alpha() > 0.0F;
    expect(lit, "the legacy Path Stroke draws its dots");
    expect(sameBits(before, current),
           "an unbound stamp lowers at its default: the legacy node draws what a new Dot draws");

    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto result = project::openProjectArchive(save(legacy, settings), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project with a pre-stamp Path Stroke opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto snapshot = opened.document->snapshot();
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    const auto* node = composition ? composition->graph().findNode(fixture.stroke) : nullptr;
    expect(node && node->parameters.size() == newSize,
           "the opened Path Stroke binds the controls a new one does");
    if (!node)
        return;
    const auto valueOf = [&](const std::string_view role) -> const document::ParameterValue* {
        const auto binding =
            std::ranges::find(node->parameters, role, &document::ParameterBinding::role);
        const auto* record = binding == node->parameters.end()
                                 ? nullptr
                                 : composition->parameters().find(binding->parameterId);
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        return constant ? &constant->value : nullptr;
    };
    const auto* stamp = valueOf("stamp");
    const auto* align = valueOf("stampAlign");
    const auto* rotation = valueOf("stampRotation");
    expect(stamp && *stamp == document::ParameterValue{std::int64_t{0}} && align &&
               *align == document::ParameterValue{std::int64_t{0}} && rotation &&
               *rotation == document::ParameterValue{0.0},
           "the added stamp is Dot, Automatic, turned 0 degrees");
    const auto kept =
        std::ranges::find(node->parameters, "width", &document::ParameterBinding::role);
    expect(kept != node->parameters.end() && kept->parameterId == width,
           "the saved controls keep their ids");
    expect(sameBits(before, render(snapshot, fixture.composition)),
           "the opened Path Stroke renders bit-identically to the pre-stamp node");

    // The migrated project saves and reopens as itself.
    const auto bytes = save(snapshot, opened.colorSettings);
    auto again = project::openProjectArchive(bytes, {}, memory());
    expect(again.outcome() == project::OpenArchiveOutcome::Opened, "the migrated project reopens");
    if (again.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(again).takeOpened();
    expect(save(reopened.document->snapshot(), reopened.colorSettings) == bytes,
           "the migrated project round-trips byte-identically");
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

} // namespace

int main() {
    try {
        legacyStrokeOpensUnchanged();
        squareStampRoundTrips();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: a pre-stamp Path Stroke opens with Dot stamps and renders identically\n";
    return 0;
}
