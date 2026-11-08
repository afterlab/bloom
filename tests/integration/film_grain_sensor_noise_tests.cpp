// #565: a project saved with a Film Grain from before Sensor Noise (no binding for the mode or any
// of its values) opens with them bound at their defaults, in Film mode, and renders exactly as it
// did before it was saved: the compiled plan is the same and the CPU reference frame is
// bit-identical. A node set to Sensor Noise keeps its mode, values and ids through a save and an
// open and draws the same picture after it, and Sensor Noise draws another picture than Film.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
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

// A bright, partly transparent Solid layer with a Film Grain between the Solid and its Layer.
struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Grain", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(32, 24, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId grain;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Grain", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    document::ParameterId parameter(const std::string_view role) const {
        const auto* node =
            document.snapshot().project().findComposition(composition)->graph().findNode(grain);
        for (const auto& binding : node->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }
    Fixture() {
        const auto solid = run<commands::AddSolidLayer>(
            composition, "Plate", core::Color4d{4.0, 2.0, 0.5, 0.75}, document::Vec2d{16.0, 12.0});
        const auto source =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto layer =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        grain = *run<commands::AddNode>(composition, std::string(document::kFilmGrainNodeType),
                                        document::Vec2d{})
                     .outputId<document::NodeId>(commands::kAddNodeOutput);
        run<commands::ConnectPorts>(composition, document::OutputPortRef{source, "image"},
                                    document::NodeInputRef{grain, "input"});
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{grain, "image"},
            document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)});
        // Non-default values, so the identity is not of an all-default grain.
        run<commands::SetParameterSource>(composition, parameter("intensity"),
                                          document::ConstantValueSource{0.1});
        run<commands::SetParameterSource>(composition, parameter("blueAmount"),
                                          document::ConstantValueSource{1.5});
        run<commands::SetParameterSource>(composition, parameter("seed"),
                                          document::ConstantValueSource{std::int64_t{42}});
    }
    // Removes the bindings (and records) a Film Grain saved before Sensor Noise never had.
    void makeLegacy() {
        const auto base = document.snapshot();
        auto draft = document.draft(base);
        auto* target = draft.project().findComposition(composition);
        auto* node = target->graph().findNode(grain);
        std::erase_if(node->parameters, [&](const document::ParameterBinding& binding) {
            if (binding.role != "mode" && binding.role != "shadowWeighting" &&
                binding.role != "chromaBlotches" && binding.role != "blotchSize" &&
                binding.role != "fixedPattern")
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
        throw std::runtime_error("Film Grain fixture compiles");
    auto request =
        runtime::EvaluationRequest{.time = *core::RationalTime::create(3, 24),
                                   .output = compiled.plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = 64U << 20U};
    request.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, {});
    if (!result.frame())
        throw std::runtime_error("Film Grain fixture evaluates");
    const auto pixels = result.frame()->processImage().pixels();
    return {compiled.plan, {pixels.begin(), pixels.end()}};
}

bool sameBits(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(render::Rgba32f)) == 0);
}

void legacyGrainOpensUnchanged() {
    Fixture fixture;
    const auto newSize = fixture.document.snapshot()
                             .project()
                             .findComposition(fixture.composition)
                             ->graph()
                             .findNode(fixture.grain)
                             ->parameters.size();
    const auto intensity = fixture.parameter("intensity");
    fixture.makeLegacy();
    const auto legacy = fixture.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto before = render(legacy, fixture.composition);
    bool lit = false;
    for (const auto& pixel : before.pixels)
        lit = lit || pixel.alpha() > 0.0F;
    expect(lit, "the legacy Film Grain renders a picture");

    auto result = project::openProjectArchive(save(legacy, settings), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project with a pre-Sensor Noise Film Grain opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto snapshot = opened.document->snapshot();
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    const auto* node = composition ? composition->graph().findNode(fixture.grain) : nullptr;
    expect(node && node->parameters.size() == newSize,
           "the opened Film Grain binds the controls a new one does");
    if (!node)
        return;
    const auto constantOf = [&](const char* role) -> const document::ConstantValueSource* {
        const auto binding = std::ranges::find(node->parameters, role,
                                               &document::ParameterBinding::role);
        const auto* record = binding == node->parameters.end()
                                 ? nullptr
                                 : composition->parameters().find(binding->parameterId);
        return record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    };
    const auto* mode = constantOf("mode");
    expect(mode && mode->value == document::ParameterValue{document::kFilmGrainFilm},
           "the added mode is Film");
    const auto* fixed = constantOf("fixedPattern");
    expect(fixed && fixed->value == document::ParameterValue{0.0}, "the added fixed pattern is off");
    for (const char* role : {"shadowWeighting", "chromaBlotches", "blotchSize"})
        expect(constantOf(role) != nullptr, "the added Sensor Noise value is bound");
    const auto kept =
        std::ranges::find(node->parameters, "intensity", &document::ParameterBinding::role);
    expect(kept != node->parameters.end() && kept->parameterId == intensity,
           "the saved controls keep their ids");

    const auto after = render(snapshot, fixture.composition);
    expect(std::ranges::equal(before.plan->operations(), after.plan->operations()) &&
               std::ranges::equal(before.plan->valueOperations(), after.plan->valueOperations()),
           "the opened Film Grain compiles to the plan it had before it was saved");
    expect(sameBits(before.pixels, after.pixels),
           "the opened Film Grain renders bit-identically to the pre-Sensor Noise node");
    // Moving a Sensor Noise value on a Film node changes neither its plan nor its picture.
    Fixture moved;
    moved.run<commands::SetParameterSource>(moved.composition, moved.parameter("fixedPattern"),
                                            document::ConstantValueSource{2.0});
    moved.run<commands::SetParameterSource>(moved.composition, moved.parameter("chromaBlotches"),
                                            document::ConstantValueSource{3.0});
    const auto movedRender = render(moved.document.snapshot(), moved.composition);
    expect(sameBits(before.pixels, movedRender.pixels) &&
               std::ranges::equal(before.plan->operations(), movedRender.plan->operations()),
           "Film mode ignores the Sensor Noise values");

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

// Sensor Noise survives a save and open with its ids and values, and draws what it drew.
void sensorNoiseRoundTrips() {
    Fixture fixture;
    const auto mode = fixture.parameter("mode");
    const auto fixedPattern = fixture.parameter("fixedPattern");
    const auto film = render(fixture.document.snapshot(), fixture.composition);
    fixture.run<commands::SetParameterSource>(fixture.composition, mode,
                                              document::ConstantValueSource{
                                                  document::kFilmGrainSensorNoise});
    fixture.run<commands::SetParameterSource>(fixture.composition, fixedPattern,
                                              document::ConstantValueSource{0.5});
    const auto snapshot = fixture.document.snapshot();
    const auto sensor = render(snapshot, fixture.composition);
    expect(!sameBits(film.pixels, sensor.pixels), "Sensor Noise draws another picture than Film");
    expect(!std::ranges::equal(film.plan->operations(), sensor.plan->operations()),
           "switching to Sensor Noise compiles another plan");
    auto result = project::openProjectArchive(
        save(snapshot, document::makeBloomNeutralColorSettingsV1({})), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened,
           "a project with Sensor Noise opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(fixture.composition);
    for (const auto& [id, expected] :
         {std::pair{mode, document::ParameterValue{document::kFilmGrainSensorNoise}},
          std::pair{fixedPattern, document::ParameterValue{0.5}}}) {
        const auto* record = composition->parameters().find(id);
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        expect(constant && constant->value == expected, "a Sensor Noise value keeps its id and value");
    }
    expect(sameBits(sensor.pixels, render(reopened, fixture.composition).pixels),
           "Sensor Noise renders bit-identically after a save and open");
}

} // namespace

int main() {
    try {
        legacyGrainOpensUnchanged();
        sensorNoiseRoundTrips();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: a pre-Sensor Noise Film Grain opens in Film mode and renders identically\n";
    return 0;
}
