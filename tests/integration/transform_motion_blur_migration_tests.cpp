// #484: a project saved with a Transform 2D from before its motion blur (only the six transform
// parameters bound) opens with the motion blur's parameters bound at their defaults, off, and renders
// exactly as it did: the compiled plan is the same and the CPU reference frame is bit-identical.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
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


[[nodiscard]] bool motionRole(const std::string_view role) {
    for (const auto& spec : document::motionBlurParameters(document::kTransform2DNodeType))
        if (spec.role == role)
            return true;
    return false;
}

// A partly transparent Solid layer with a moved Transform 2D between the Solid and its Layer.
struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Transform", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(32, 24, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId transform;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Transform", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    document::ParameterId parameter(const std::string_view role) const {
        const auto* node =
            document.snapshot().project().findComposition(composition)->graph().findNode(transform);
        for (const auto& binding : node->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }
    Fixture() {
        const auto solid = run<commands::AddSolidLayer>(
            composition, "Plate", core::Color4d{0.5, 0.25, 0.125, 0.75}, document::Vec2d{16.0, 12.0});
        const auto source =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto layer =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        transform = *run<commands::AddNode>(composition, std::string(document::kTransform2DNodeType),
                                            document::Vec2d{})
                         .outputId<document::NodeId>(commands::kAddNodeOutput);
        run<commands::ConnectPorts>(composition, document::OutputPortRef{source, "image"},
                                    document::NodeInputRef{transform, "input"});
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{transform, "image"},
            document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)});
        run<commands::SetParameterSource>(composition, parameter("translate"),
                                          document::ConstantValueSource{document::Vec2d{3.0, 2.0}});
    }
    // Removes every binding (and record) a Transform 2D saved before #484 never had.
    void makeLegacy() {
        const auto base = document.snapshot();
        auto draft = document.draft(base);
        auto* target = draft.project().findComposition(composition);
        auto* node = target->graph().findNode(transform);
        std::erase_if(node->parameters, [&](const document::ParameterBinding& binding) {
            // A document from before motion blur (#484) and before skew and opacity (#524).
            if (!motionRole(binding.role) && binding.role != "skew" &&
                binding.role != "skewAxis" && binding.role != "opacity")
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

struct Render final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    std::vector<render::Rgba32f> pixels;
};

Render render(const document::Snapshot& snapshot, const document::CompositionId composition) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({snapshot, composition}, {});
    if (!compiled.plan)
        throw std::runtime_error("Transform fixture compiles");
    auto request =
        runtime::EvaluationRequest{.time = core::RationalTime{},
                                   .output = compiled.plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = 64U << 20U};
    request.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, {});
    if (!result.frame())
        throw std::runtime_error("Transform fixture evaluates");
    const auto pixels = result.frame()->processImage().pixels();
    return {compiled.plan, {pixels.begin(), pixels.end()}};
}

bool sameBits(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(render::Rgba32f)) == 0);
}

std::set<std::string> roles(const document::NodeRecord& node) {
    std::set<std::string> result;
    for (const auto& binding : node.parameters)
        result.insert(binding.role);
    return result;
}

void legacyTransformOpensWithItsMotionBlurOff() {
    Fixture fixture;
    const auto* definition =
        document::builtInNodeDefinitions().find(document::kTransform2DNodeType, 1);
    if (!definition)
        throw std::logic_error("Transform 2D definition");
    const auto newRoles = roles(*fixture.document.snapshot()
                                     .project()
                                     .findComposition(fixture.composition)
                                     ->graph()
                                     .findNode(fixture.transform));
    expect(newRoles.contains("motionBlur") && newRoles.size() == definition->parameters.size(),
           "a new Transform 2D binds its motion blur");
    const auto translate = fixture.parameter("translate");
    fixture.makeLegacy();
    const auto legacy = fixture.document.snapshot();
    expect(roles(*legacy.project()
                      .findComposition(fixture.composition)
                      ->graph()
                      .findNode(fixture.transform))
                   .size() == 6,
           "the legacy Transform 2D binds its six transform parameters");
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto before = render(legacy, fixture.composition);

    auto result = project::openProjectArchive(save(legacy, settings), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project with a pre-#484 Transform 2D opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto snapshot = opened.document->snapshot();
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    const auto* node = composition ? composition->graph().findNode(fixture.transform) : nullptr;
    expect(node && roles(*node) == newRoles, "the opened Transform 2D binds its motion blur");
    if (!node)
        return;
    bool defaults = true;
    bool translateKept = false;
    for (const auto& binding : node->parameters) {
        const auto* record = composition->parameters().find(binding.parameterId);
        const auto parameter = std::ranges::find(definition->parameters, binding.role,
                                                 &document::ParameterDefinition::role);
        if (!record || parameter == definition->parameters.end()) {
            defaults = false;
            continue;
        }
        if (binding.role == "translate")
            translateKept = binding.parameterId == translate;
        if (!motionRole(binding.role))
            continue;
        const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
        defaults = defaults && record->schemaKey == parameter->schemaKey && constant &&
                   constant->value == parameter->defaultValue;
    }
    expect(defaults, "each added parameter holds its default: motion blur off");
    expect(translateKept, "the transform's parameters keep their ids");

    const auto after = render(snapshot, fixture.composition);
    expect(std::ranges::equal(before.plan->operations(), after.plan->operations()) &&
               std::ranges::equal(before.plan->valueOperations(), after.plan->valueOperations()),
           "the opened Transform 2D compiles to the plan it had before it was saved");
    expect(sameBits(before.pixels, after.pixels),
           "the opened Transform 2D renders bit-identically");

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

} // namespace

int main() {
    try {
        legacyTransformOpensWithItsMotionBlurOff();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: a pre-#484 Transform 2D opens with its motion blur off and renders identically\n";
    return 0;
}
