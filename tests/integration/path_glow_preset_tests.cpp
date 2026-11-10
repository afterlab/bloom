// #509 Glowing paths: the five ready-made Node Groups (Saber, Beam, Vegas, Write-on, Stroke) made
// by one command, through the real compiler and the CPU reference evaluator.
//
// Each preset is one undo step that adds a group definition (Group Input, Path Stroke, Shape
// Source, Glow, Group Output) and a Node Group instance pointed at it; its slots are named after
// the inner value they drive; drawn by a layer it lights pixels, and a changed Width changes them;
// Write-on draws nothing at its first key and more at its second, Vegas's dashes move between its
// keys; and the project saves, reopens and renders bit-identically, then round-trips its bytes.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/path_glow_presets.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_utility_nodes.hpp>
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
using document::Vec2d;

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

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Glowing paths", "Main", core::RationalTime::fromInteger(3),
        *document::CompositionFormat::create(320, 180, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Glowing paths", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            std::string why = "fixture command refused";
            for (const auto& failure : result.operationFailures)
                why += ": " + failure.issue.message;
            throw std::runtime_error(why);
        }
        return result;
    }
    [[nodiscard]] const document::Composition& current() const {
        return *document.snapshot().project().findComposition(composition);
    }
    document::ParameterId parameter(const document::NodeId id, const std::string_view role) const {
        for (const auto& binding : current().graph().findNode(id)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing: " + std::string(role));
    }
    // A layer whose content is the instance's image.
    void show(const document::NodeId instance) {
        const auto solid =
            run<commands::AddSolidLayer>(composition, "Glow", core::Color4d{0.0, 0.0, 0.0, 1.0});
        const auto layer =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        const document::NodeInputRef content{layer,
                                             std::string(document::kLayerOutputContentInputPort)};
        run<commands::DisconnectInput>(composition, content);
        run<commands::ConnectPorts>(composition, document::OutputPortRef{instance, "image1"},
                                    content);
    }
};

std::vector<render::Rgba32f> render(const document::Snapshot& snapshot,
                                    const document::CompositionId composition,
                                    const core::RationalTime time) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({snapshot, composition}, {});
    if (!compiled.plan)
        throw std::runtime_error(
            "preset compiles: " +
            runtime::describeCompileFailure(compiled, snapshot.project(), composition));
    auto request =
        runtime::EvaluationRequest{.time = time,
                                   .output = compiled.plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = 256U << 20U};
    request.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, {});
    if (!result.frame())
        throw std::runtime_error("preset evaluates");
    const auto pixels = result.frame()->processImage().pixels();
    return {pixels.begin(), pixels.end()};
}

bool sameBits(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(render::Rgba32f)) == 0);
}

// The light a frame carries over its black plate: the sum of its red, green and blue.
double light(const std::vector<render::Rgba32f>& pixels) {
    double sum = 0.0;
    for (const auto& pixel : pixels)
        sum += static_cast<double>(pixel.red()) + static_cast<double>(pixel.green()) +
               static_cast<double>(pixel.blue());
    return sum;
}

std::vector<std::byte> save(const document::Snapshot& snapshot,
                            const document::ColorSettings& settings) {
    const auto saved = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    if (!saved)
        throw std::logic_error("archive saves");
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

core::RationalTime seconds(const std::int64_t numerator, const std::int64_t denominator = 1) {
    return *core::RationalTime::create(numerator, denominator);
}

void preset(const commands::PathGlowPreset which) {
    const std::string name(commands::pathGlowPresetName(which));
    Fixture f;
    const auto before = f.document.snapshot();
    const auto made = f.run<commands::CreatePathGlowPreset>(f.composition, which, Vec2d{40.0, 0.0});
    const auto instance = made.outputId<document::NodeId>(commands::kPathGlowPresetNodeOutput);
    const auto group = made.outputId<document::CompositionId>(commands::kPathGlowPresetGroupOutput);
    expect(instance && group, name + ": the preset answers its instance and its group");
    if (!instance || !group)
        return;
    auto snapshot = f.document.snapshot();
    const auto* definition = snapshot.project().findComposition(*group);
    expect(definition != nullptr && document::isNodeGroupDefinition(*definition) &&
               definition->name() == name,
           name + ": a group definition named after the preset is added");
    if (definition == nullptr)
        return;
    const auto holds = [&](const std::string_view type) {
        return std::ranges::any_of(definition->graph().nodes(),
                                   [&](const auto& node) { return node.typeId == type; });
    };
    expect(holds(document::kPathStrokeNodeType) && holds(document::kShapeSourceNodeType) &&
               holds(document::kGlowNodeType),
           name + ": the group strokes a path, fills the stroke and glows it");
    const auto* node = f.current().graph().findNode(*instance);
    expect(node != nullptr &&
               document::nodeGroupReference(*node, f.current().parameters()) == *group,
           name + ": the instance reads the group");
    expect(snapshot.project().validate().ok(), name + ": the project is valid document truth");

    // The slots are named after what they drive.
    const auto target = [&](const std::string_view slot) {
        return document::nodeGroupSlotTarget(snapshot.project(), *node, f.current().parameters(),
                                             slot);
    };
    const auto width = target("scalar1");
    const auto size = target("scalar2");
    const auto intensity = target("scalar3");
    const auto color = target("color1");
    expect(width && width->typeId == document::kPathStrokeNodeType && width->role == "width" &&
               size && size->typeId == document::kGlowNodeType && size->role == "size" &&
               intensity && intensity->role == "intensity" && color &&
               (color->role == "tint" || color->role == "fillColor"),
           name + ": the width, glow size, intensity and colour slots each drive one inner value");
    expect(!target("vector1").has_value(), name + ": an unwired slot names nothing");

    // One undo step removes the group and the instance; redo brings both back.
    expect(f.commands.undo().changed() &&
               f.document.snapshot().project().findComposition(*group) == nullptr &&
               f.current().graph().findNode(*instance) == nullptr &&
               f.document.snapshot().revision() != before.revision(),
           name + ": the preset is one undo step");
    expect(f.commands.redo().changed() &&
               f.document.snapshot().project().findComposition(*group) != nullptr &&
               f.current().graph().findNode(*instance) != nullptr,
           name + ": redo restores the preset");

    f.show(*instance);
    const auto time = which == commands::PathGlowPreset::WriteOn ? seconds(2) : seconds(1, 2);
    const auto frame = render(f.document.snapshot(), f.composition, time);
    expect(light(frame) > 0.0, name + ": the preset lights the frame");
    if (which == commands::PathGlowPreset::WriteOn) {
        expect(light(render(f.document.snapshot(), f.composition, seconds(0))) == 0.0,
               name + ": nothing is written at the first key");
        expect(light(render(f.document.snapshot(), f.composition, seconds(1))) < light(frame),
               name + ": more is written later");
    }
    if (which == commands::PathGlowPreset::Vegas)
        expect(!sameBits(render(f.document.snapshot(), f.composition, seconds(0)), frame),
               name + ": the dashes move between the keys");

    // The instance's Width reaches the stroke inside the group.
    f.run<commands::SetParameterSource>(
        f.composition, f.parameter(*instance, "scalar1"),
        document::ConstantValueSource{document::ParameterValue{24.0}});
    const auto wider = render(f.document.snapshot(), f.composition, time);
    expect(light(wider) > light(frame), name + ": a wider stroke carries more light");

    // Saved and reopened, the preset is the same group and renders the same pixels.
    snapshot = f.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto bytes = save(snapshot, settings);
    auto opened = project::openProjectArchive(bytes, {}, memory());
    expect(opened.outcome() == project::OpenArchiveOutcome::Opened, name + ": the project reopens");
    if (opened.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(opened).takeOpened();
    const auto again = reopened.document->snapshot();
    expect(again.project().findComposition(*group) != nullptr &&
               sameBits(render(again, f.composition, time), wider),
           name + ": the reopened preset renders bit-identically");
    expect(save(again, reopened.colorSettings) == bytes,
           name + ": the reopened project round-trips byte-identically");
}

} // namespace

int main() {
    try {
        for (const auto which : commands::kPathGlowPresets)
            preset(which);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: the glowing path presets\n";
    return 0;
}
