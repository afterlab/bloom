// #342 Node Group, Group Input and Group Output through the real archive writer and reader.
//
// A project built with commands holds a group definition (Group Input wired through an Exposure and
// an Image Mix to Group Output, with the Mask and second image entering at Group Input, the
// exposure and the mix factor read from published scalar slots) and an instance whose slots carry
// its own values: `scalar2` is a constant, `scalar1` is driven by a Scalar value node. The project
// is saved, reopened and compared record for record (the definition's nodes, links, parameters and
// layout, the instance, and every slot parameter); it compiles to the same operations as before
// saving and as the flat graph of the same nodes, renders the same pixels on the CPU reference, and
// writes the same bytes again. Editing the definition after reopening still reaches the instance.
//
// The document format did not move for groups: a definition is an ordinary composition holding a
// Group Output, and an instance is an ordinary node, so there is no version-specific behaviour to
// check beyond what src/project/tests/node_group_persistence_tests.cpp covers.

#include "node_group_document_support.hpp"

#include <bloom/document/color_settings.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace bloom;
using namespace bloom::test::node_group;

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

std::vector<std::byte> save(const document::Snapshot& snapshot,
                            const document::ColorSettings& settings) {
    const auto saved = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    if (!saved)
        throw std::logic_error("archive saves");
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

struct Compiled final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    std::vector<render::Rgba32f> pixels;
};

Compiled compileAndRender(const document::Snapshot& snapshot, const document::CompositionId id,
                          const core::RationalTime time) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({snapshot, id}, {});
    if (!compiled.plan)
        throw std::runtime_error("compiles: " + runtime::describeCompileFailure(
                                                    compiled, snapshot.project(), id));
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
        throw std::runtime_error("evaluates");
    const auto pixels = result.frame()->processImage().pixels();
    return {compiled.plan, {pixels.begin(), pixels.end()}};
}

bool sameBits(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b) {
    return !a.empty() && a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(render::Rgba32f)) == 0;
}

// Operations by kind, in order: the shape of a plan with the node ids taken out.
std::vector<std::size_t> shapeOf(const runtime::CompiledCompositionPlan& plan) {
    std::vector<std::size_t> shape;
    for (const auto& operation : plan.operations()) {
        shape.push_back(operation.index());
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
            shape.push_back(effect->kernel.index());
    }
    return shape;
}

// Every record of a composition that a reopen must return exactly; empty when they match, else
// which kind of record differs first.
// The archive writes records in canonical order, so records compare by identity, not position.
template <typename Span, typename Key>
bool sameRecords(const Span left, const Span right, Key key) {
    auto a = std::vector(left.begin(), left.end());
    auto b = std::vector(right.begin(), right.end());
    if constexpr (requires { a.front().parameters; }) {
        for (auto* records : {&a, &b})
            for (auto& record : *records)
                std::ranges::sort(record.parameters, {}, &document::ParameterBinding::role);
    }
    const auto order = [&](const auto& x, const auto& y) { return key(x) < key(y); };
    std::ranges::sort(a, order);
    std::ranges::sort(b, order);
    return std::ranges::equal(a, b);
}

std::string compositionDifference(const document::Composition& a, const document::Composition& b) {
    if (a.name() != b.name())
        return "name";
    if (!sameRecords(a.graph().nodes(), b.graph().nodes(), [](const auto& n) { return n.id.value(); }))
        return "nodes";
    if (!sameRecords(a.graph().edges(), b.graph().edges(), [](const auto& e) { return e.id.value(); }))
        return "edges";
    if (!sameRecords(a.parameters().records(), b.parameters().records(), [](const auto& r) { return r.id.value(); }))
        return "parameters";
    if (!(a.nodeLayout() == b.nodeLayout()))
        return "layout";
    if (!(a.graph().compositionOutput() == b.graph().compositionOutput()))
        return "composition output";
    return {};
}

void saveAndReopen() {
    auto fixture = buildTone({.grouped = true, .stops = 0.75, .factor = 0.6, .drivenStops = true});
    auto flat = buildTone({.grouped = false, .stops = 0.75, .factor = 0.6, .drivenStops = true});
    const auto& parts = fixture->parts;
    const auto before = fixture->document.snapshot();
    const auto* definitionBefore = before.project().findComposition(parts.group);
    const auto* mainBefore = before.project().findComposition(fixture->main);
    expect(definitionBefore != nullptr && document::isNodeGroupDefinition(*definitionBefore) &&
               document::findGroupInput(*definitionBefore) != nullptr &&
               mainBefore != nullptr && before.project().validate().ok(),
           "the fixture holds a valid group definition and its instance");
    if (definitionBefore == nullptr || mainBefore == nullptr)
        return;
    const core::RationalTime time = core::RationalTime::fromInteger(0);
    const auto original = compileAndRender(before, fixture->main, time);
    const auto flatRendered = compileAndRender(flat->document.snapshot(), flat->main, time);
    expect(shapeOf(*original.plan) == shapeOf(*flatRendered.plan) &&
               sameBits(original.pixels, flatRendered.pixels),
           "before saving, the group compiles to the flat graph's operations and pixels");

    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto bytes = save(before, settings);
    auto opened = project::openProjectArchive(bytes, {}, memory());
    expect(opened.outcome() == project::OpenArchiveOutcome::Opened, "the grouped project reopens");
    if (opened.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(opened).takeOpened();
    const auto after = reopened.document->snapshot();
    expect(after.project().validate().ok(), "the reopened project is valid document truth");

    const auto* definition = after.project().findComposition(parts.group);
    const auto* main = after.project().findComposition(fixture->main);
    expect(definition != nullptr && main != nullptr, "both compositions survive");
    if (definition == nullptr || main == nullptr)
        return;
    expect(document::isNodeGroupDefinition(*definition) &&
               document::findGroupInput(*definition) != nullptr &&
               document::findGroupInput(*definition)->id == parts.groupInput &&
               document::findGroupOutput(*definition)->id == parts.groupOutput,
           "the reopened definition still holds Group Input and Group Output, same ids");
    expect(compositionDifference(*definition, *definitionBefore).empty(),
           "the definition's nodes, links, parameters and layout reopen record for record: " +
               compositionDifference(*definition, *definitionBefore));
    expect(compositionDifference(*main, *mainBefore).empty(),
           "the main composition (instance, links, driver and slot values) reopens record for "
           "record: " + compositionDifference(*main, *mainBefore));

    // The instance: its group reference and every slot parameter.
    const auto* instance = main->graph().findNode(parts.instance);
    expect(instance != nullptr && instance->typeId == document::kNodeGroupNodeType &&
               document::nodeGroupReference(*instance, main->parameters()) == parts.group,
           "the instance still names the group");
    if (instance != nullptr) {
        const auto* instanceBefore = mainBefore->graph().findNode(parts.instance);
        bool everySlot = instanceBefore != nullptr &&
                         sameRecords(std::span(instance->parameters),
                                     std::span(instanceBefore->parameters),
                                     [](const auto& b) { return b.parameterId.value(); });
        std::size_t slots = 0;
        for (const auto& binding : instance->parameters) {
            const auto* now = main->parameters().find(binding.parameterId);
            const auto* was = mainBefore->parameters().find(binding.parameterId);
            everySlot = everySlot && now != nullptr && was != nullptr && *now == *was;
            ++slots;
        }
        expect(everySlot && slots == 1 + 8,
               "the group reference and all eight value slots reopen with their sources");
        const auto* stops = main->parameters().find(
            fixture->parameter(fixture->main, parts.instance, "scalar1"));
        const auto* factor = main->parameters().find(
            fixture->parameter(fixture->main, parts.instance, "scalar2"));
        const auto* driver = stops ? std::get_if<document::DriverBindingSource>(&stops->source)
                                   : nullptr;
        const auto* constant = factor ? std::get_if<document::ConstantValueSource>(&factor->source)
                                      : nullptr;
        expect(driver != nullptr && driver->sourceNodeId == parts.driver && constant != nullptr &&
                   std::get_if<double>(&constant->value) != nullptr &&
                   *std::get_if<double>(&constant->value) == 0.6,
               "scalar1 is still driven by the Scalar node and scalar2 still holds its override");
    }

    const auto reopenedRender = compileAndRender(after, fixture->main, time);
    expect(shapeOf(*reopenedRender.plan) == shapeOf(*original.plan) &&
               std::ranges::equal(reopenedRender.plan->operations(), original.plan->operations()),
           "the reopened project compiles to the same plan, node ids included");
    expect(sameBits(reopenedRender.pixels, original.pixels) &&
               sameBits(reopenedRender.pixels, flatRendered.pixels),
           "the reopened project renders the same CPU pixels as before saving and as the flat graph");
    expect(save(after, reopened.colorSettings) == bytes,
           "the reopened project saves the same bytes again");

    // The reopened definition is still live: one edit inside it reaches the instance, and matches
    // the flat graph with the same edit.
    commands::CommandStack stack(*reopened.document);
    commands::Transaction edit("Edit group", after.revision());
    edit.emplace<commands::SetParameterSource>(
        parts.group,
        [&] {
            for (const auto& binding : definition->graph().findNode(parts.mix)->parameters)
                if (binding.role == "factor")
                    return binding.parameterId;
            throw std::runtime_error("mix factor binding");
        }(),
        document::ConstantValueSource{document::ParameterValue{0.2}});
    expect(stack.execute(std::move(edit)).succeeded(), "an edit inside the reopened group runs");
    // The mix factor read scalar2 (0.6); the edit made it a constant 0.2, so the picture moves.
    const auto edited = compileAndRender(reopened.document->snapshot(), fixture->main, time);
    expect(!sameBits(edited.pixels, original.pixels),
           "editing the reopened definition changes what the instance draws");
}

// The commands refuse a wrong-kind link to a pixel slot with the documented message.
void wrongKindsAreRefused() {
    auto fixture = buildTone({.grouped = true});
    const auto& parts = fixture->parts;
    const auto before = fixture->document.snapshot();
    const auto refuse = [&](const document::NodeId from, const std::string_view output,
                            const std::string_view input) {
        commands::Transaction transaction("Wrong kind", fixture->document.snapshot().revision());
        transaction.emplace<commands::ConnectPorts>(
            fixture->main, document::OutputPortRef{from, std::string(output)},
            document::NodeInputRef{parts.instance, std::string(input)});
        const auto result = fixture->commands.execute(std::move(transaction));
        return !result.succeeded() && !result.operationFailures.empty() &&
               result.operationFailures.front().issue.message.find(
                   "their socket kinds do not match") != std::string::npos;
    };
    expect(refuse(parts.plate, "image", "mask1"), "an Image wired to mask1 is refused by ConnectPorts");
    expect(refuse(parts.plate, "image", "channels1"),
           "an Image wired to channels1 is refused by ConnectPorts");
    expect(refuse(parts.key, "mask", "image2"), "a Mask wired to image2 is refused by ConnectPorts");
    expect(fixture->document.snapshot().revision() == before.revision(),
           "the refused links change nothing");
}

} // namespace

int main() {
    try {
        wrongKindsAreRefused();
        saveAndReopen();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: a Node Group, its definition, Group Input and Group Output save, reopen and "
                 "render identically\n";
    return 0;
}
