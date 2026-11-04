// #534: a project saved with a Glow from before the #506 modes (only its Classic parameters bound)
// opens with every Glow control bound at its default, so Properties and the node card present the
// same full control set as a new Glow, and it renders exactly as it did before it was saved: the
// compiled plan is the same and the CPU reference frame is bit-identical.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/document.hpp>
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

// The Glow roles bound before #506.
const std::set<std::string> kClassicRoles{"threshold", "knee", "intensity", "size",
                                          "falloff",   "tint", "octaves",   "glowOnly"};

// A bright, partly transparent Solid layer with a Glow between the Solid and its Layer.
struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Glow", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(32, 24, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId glow;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Glow", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    document::ParameterId parameter(const std::string_view role) const {
        const auto* node =
            document.snapshot().project().findComposition(composition)->graph().findNode(glow);
        for (const auto& binding : node->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }
    Fixture() {
        const auto solid = run<commands::AddSolidLayer>(
            composition, "Light", core::Color4d{4.0, 2.0, 0.5, 0.75}, document::Vec2d{16.0, 12.0});
        const auto source =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto layer =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        glow = *run<commands::AddNode>(composition, std::string(document::kGlowNodeType),
                                       document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
        run<commands::ConnectPorts>(composition, document::OutputPortRef{source, "image"},
                                    document::NodeInputRef{glow, "input"});
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{glow, "image"},
            document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)});
        // Non-default Classic values, so the identity is not of an all-default glow.
        run<commands::SetParameterSource>(composition, parameter("threshold"),
                                          document::ConstantValueSource{1.5});
        run<commands::SetParameterSource>(composition, parameter("size"),
                                          document::ConstantValueSource{6.0});
        run<commands::SetParameterSource>(composition, parameter("octaves"),
                                          document::ConstantValueSource{std::int64_t{3}});
    }
    // Removes every binding (and record) a Glow saved before #506 never had.
    void makeLegacy() {
        const auto base = document.snapshot();
        auto draft = document.draft(base);
        auto* target = draft.project().findComposition(composition);
        auto* node = target->graph().findNode(glow);
        std::erase_if(node->parameters, [&](const document::ParameterBinding& binding) {
            if (kClassicRoles.contains(binding.role))
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
        throw std::runtime_error("Glow fixture compiles");
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
        throw std::runtime_error("Glow fixture evaluates");
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

void legacyGlowOpensWithFullControls() {
    Fixture fixture;
    const auto* definition = document::builtInNodeDefinitions().find(document::kGlowNodeType, 1);
    if (!definition)
        throw std::logic_error("Glow definition");
    const auto newRoles = roles(*fixture.document.snapshot()
                                     .project()
                                     .findComposition(fixture.composition)
                                     ->graph()
                                     .findNode(fixture.glow));
    expect(newRoles.size() == definition->parameters.size(),
           "a new Glow binds every definition parameter");
    std::map<std::string, document::ParameterId> classicIds;
    for (const auto& role : kClassicRoles)
        classicIds[role] = fixture.parameter(role);
    fixture.makeLegacy();
    const auto legacy = fixture.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto before = render(legacy, fixture.composition);
    bool lit = false;
    for (const auto& pixel : before.pixels)
        lit = lit || pixel.alpha() > 0.0F;
    expect(lit, "the legacy Glow renders a picture");

    auto result = project::openProjectArchive(save(legacy, settings), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project with a pre-#506 Glow opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto snapshot = opened.document->snapshot();
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    const auto* node = composition ? composition->graph().findNode(fixture.glow) : nullptr;
    expect(node && roles(*node) == newRoles,
           "the opened Glow binds the full control set a new Glow does");
    if (!node)
        return;
    bool defaults = true;
    bool classicKept = true;
    for (const auto& binding : node->parameters) {
        const auto* record = composition->parameters().find(binding.parameterId);
        const auto parameter = std::ranges::find(definition->parameters, binding.role,
                                                 &document::ParameterDefinition::role);
        if (!record || parameter == definition->parameters.end()) {
            defaults = false;
            continue;
        }
        if (kClassicRoles.contains(binding.role)) {
            classicKept = classicKept && binding.parameterId == classicIds.at(binding.role);
            continue;
        }
        const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
        defaults = defaults && record->schemaKey == parameter->schemaKey && constant &&
                   constant->value == parameter->defaultValue;
    }
    expect(defaults, "each added control holds its definition default");
    expect(classicKept, "the Classic controls keep their ids");

    const auto after = render(snapshot, fixture.composition);
    expect(std::ranges::equal(before.plan->operations(), after.plan->operations()) &&
               std::ranges::equal(before.plan->valueOperations(), after.plan->valueOperations()),
           "the opened Glow compiles to the plan it had before it was saved");
    expect(sameBits(before.pixels, after.pixels),
           "the opened Glow renders bit-identically to the pre-#506 Glow");

    // The migrated project saves and reopens as itself: a complete Glow is not migrated again.
    const auto bytes = save(snapshot, opened.colorSettings);
    auto again = project::openProjectArchive(bytes, {}, memory());
    expect(again.outcome() == project::OpenArchiveOutcome::Opened, "the migrated project reopens");
    if (again.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(again).takeOpened();
    expect(save(reopened.document->snapshot(), reopened.colorSettings) == bytes,
           "the migrated project round-trips byte-identically");
}

// A Glow that already binds some #506 controls keeps them (ids and values) and gains the rest.
void partialGlowKeepsItsControls() {
    Fixture fixture;
    fixture.run<commands::SetParameterSource>(fixture.composition, fixture.parameter("rollOff"),
                                              document::ConstantValueSource{0.5});
    const auto rollOff = fixture.parameter("rollOff");
    {
        const auto base = fixture.document.snapshot();
        auto draft = fixture.document.draft(base);
        auto* target = draft.project().findComposition(fixture.composition);
        auto* node = target->graph().findNode(fixture.glow);
        std::erase_if(node->parameters, [&](const document::ParameterBinding& binding) {
            if (kClassicRoles.contains(binding.role) || binding.role == "rollOff")
                return false;
            if (!target->parameters().erase(binding.parameterId))
                throw std::logic_error("partial record erases");
            return true;
        });
        if (!fixture.document.commit(base.revision(), std::move(draft)).committed())
            throw std::logic_error("partial commit");
    }
    const auto partial = fixture.document.snapshot();
    const auto before = render(partial, fixture.composition);
    auto result = project::openProjectArchive(
        save(partial, document::makeBloomNeutralColorSettingsV1({})), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project with a partly bound Glow opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto snapshot = opened.document->snapshot();
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    const auto* node = composition->graph().findNode(fixture.glow);
    const auto binding =
        std::ranges::find(node->parameters, "rollOff", &document::ParameterBinding::role);
    const auto* record =
        binding == node->parameters.end() ? nullptr : composition->parameters().find(rollOff);
    const auto* constant =
        record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    expect(binding != node->parameters.end() && binding->parameterId == rollOff && constant &&
               constant->value == document::ParameterValue{0.5},
           "a bound #506 control keeps its id and value");
    expect(
        node->parameters.size() ==
            document::builtInNodeDefinitions().find(document::kGlowNodeType, 1)->parameters.size(),
        "the partly bound Glow gains the rest of its controls");
    expect(sameBits(before.pixels, render(snapshot, fixture.composition).pixels),
           "the partly bound Glow renders bit-identically");
}

} // namespace

int main() {
    try {
        legacyGlowOpensWithFullControls();
        partialGlowKeepsItsControls();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: a pre-#506 Glow opens with the full controls and renders identically\n";
    return 0;
}
