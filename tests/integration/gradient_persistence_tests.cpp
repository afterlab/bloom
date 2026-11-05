// #541 persistence: a Colorize gradient -- a constant and a whole-value animation curve -- saves,
// reopens and decodes exactly in document 1.38, and its keys sample stop by stop; the canonical text
// is stable; a document claiming 1.37 that carries a gradient is refused; a malformed gradient is
// refused; a 1.37 document without one migrates by version alone and opens unchanged; and a
// Colorize saved before the gradient existed binds it on its first edit.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/curve_compilation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

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

document::GradientValue fiveStops() {
    return {{{0.8, core::Color4d{1.2, 0.9, 0.1, 1.0}},
             {0.0, core::Color4d{0.0, -0.05, 0.3, 1.0}},
             {0.5, core::Color4d{0.1, 0.6, 0.2, 0.5}},
             {0.5, core::Color4d{0.9, 0.1, -0.2, 1.0}},
             {1.0, core::Color4d{3.0, 2.0, 1.5, 1.0}}}};
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Gradients", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 32, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId colorize;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Gradients", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    [[nodiscard]] const document::Composition& comp() const {
        return *document.snapshot().project().findComposition(composition);
    }
    [[nodiscard]] std::optional<document::ParameterId> parameter(const std::string_view role) const {
        const auto* node = comp().graph().findNode(colorize);
        const auto found = std::ranges::find(node->parameters, role,
                                             &document::ParameterBinding::role);
        return found == node->parameters.end() ? std::nullopt
                                               : std::optional(found->parameterId);
    }
    Fixture() {
        colorize = *run<commands::AddNode>(composition, std::string(document::kColorizeNodeType),
                                           document::Vec2d{0, 0})
                        .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
};

std::string encode(const document::Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const project::CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = project::canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!project::encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}

project::DocumentDecodeResult decodeText(const std::string& text) {
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("gradient parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

void replaceOnce(std::string& text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos)
        throw std::logic_error("fixture anchor missing");
    text.replace(at, from.size(), to);
}

void gradientsRoundTrip() {
    Fixture fixture;
    const auto gradient = fixture.parameter("gradient");
    expect(gradient.has_value(), "a new Colorize binds its gradient");
    if (!gradient)
        return;
    const auto* record = fixture.comp().parameters().find(*gradient);
    const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
    expect(constant && constant->value ==
                           document::ParameterValue{document::defaultColorizeGradient()},
           "the gradient starts as the three-colour ramp");
    fixture.run<commands::SetParameterSource>(
        fixture.composition, *gradient,
        document::ParameterSource{document::ConstantValueSource{fiveStops()}});
    // A 64-stop gradient is the most a document holds.
    document::GradientValue most;
    for (std::size_t i = 0; i < document::kMaximumGradientStops; ++i)
        most.stops.push_back({static_cast<double>(i) / 63.0, core::Color4d{0.5, 0.5, 0.5, 1.0}});
    auto tooMany = most;
    tooMany.stops.push_back({0.5, core::Color4d{0.5, 0.5, 0.5, 1.0}});
    commands::Transaction refused("Too many", fixture.document.snapshot().revision());
    refused.emplace<commands::SetParameterSource>(
        fixture.composition, *gradient,
        document::ParameterSource{document::ConstantValueSource{tooMany}});
    expect(!fixture.commands.execute(std::move(refused)).succeeded(),
           "a gradient past 64 stops is refused");

    const auto constantText = encode(fixture.document);
    expect(constantText.find("\"kind\": \"gradient\"") != std::string::npos &&
               constantText.find("\"minor\": 39") != std::string::npos,
           "the gradient constant is written at 1.39");
    const auto decodedConstant = decodeText(constantText);
    expect(static_cast<bool>(decodedConstant), "the gradient constant decodes");

    // Keyed: two keys with the same stop count interpolate stop by stop.
    fixture.run<commands::CreateAnimationForParameter>(fixture.composition, *gradient,
                                                       core::RationalTime::fromInteger(0));
    auto later = fiveStops();
    later.stops[0].position = 0.6;
    later.stops[1].color.green = 0.95;
    fixture.run<commands::SetKeyframeAtTimeForParameter>(
        fixture.composition, *gradient, core::RationalTime::fromInteger(2), later);
    const auto* keyed = fixture.comp().parameters().find(*gradient);
    const auto* source = std::get_if<document::AnimationCurveSource>(&keyed->source);
    const auto* curve =
        source ? fixture.comp().animationCurves().findGradient(source->curveId) : nullptr;
    expect(curve && curve->keyframes.size() == 2, "the gradient is a two-key whole-value curve");
    if (curve) {
        const auto compiled = runtime::compileAnimationCurve(*curve);
        const auto middle = runtime::sampleGradientCurve(compiled, core::RationalTime::fromInteger(1));
        const auto end = runtime::sampleGradientCurve(compiled, core::RationalTime::fromInteger(3));
        expect(middle && middle->stops.size() == 5 &&
                   std::abs(middle->stops[0].position - 0.7) < 1e-6 &&
                   std::abs(middle->stops[1].color.green - 0.45) < 1e-6,
               "between two keys each stop's position and colour interpolate");
        expect(end && *end == later, "past the last key the gradient holds");
    }

    const auto text = encode(fixture.document);
    expect(text.find("\"kind\": \"gradient\"") != std::string::npos, "the gradient curve is written");
    const auto decoded = decodeText(text);
    expect(static_cast<bool>(decoded), "the gradient curve decodes");

    const auto snapshot = fixture.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(archive), "the gradient archive saves and verifies");
    if (!archive)
        return;
    auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(fixture.composition);
    expect(composition &&
               std::ranges::equal(composition->parameters().records(),
                                  fixture.comp().parameters().records()) &&
               std::ranges::equal(composition->animationCurves().records(),
                                  fixture.comp().animationCurves().records()),
           "the gradient curve and its keys equal themselves after save and open");
    expect(reopened.project().validate().ok(), "the reopened project validates");
    expect(encode(*opened.document) == text, "the canonical text is stable across save and open");
}

void olderAndMalformedDocuments() {
    Fixture fixture;
    const auto current = encode(fixture.document);
    const auto kindAt = current.find("\"kind\": \"gradient\"");
    expect(kindAt != std::string::npos, "the fixture carries a gradient");
    if (kindAt == std::string::npos)
        return;
    // A document claiming 1.37 cannot carry a gradient.
    auto claimed = current;
    replaceOnce(claimed, "\"minor\": 39", "\"minor\": 37");
    expect(!static_cast<bool>(decodeText(claimed)), "a 1.37 document carrying a gradient is refused");

    // A malformed gradient is refused: a stop outside [0, 1], an unknown stop member.
    const auto stopsAt = current.find("\"stops\": [", kindAt);
    expect(stopsAt != std::string::npos, "the gradient writes its stops");
    if (stopsAt != std::string::npos) {
        auto outside = current;
        const auto positionAt = outside.find("\"position\": 0", stopsAt);
        expect(positionAt != std::string::npos, "the first stop writes its position");
        if (positionAt != std::string::npos) {
            outside.replace(positionAt, std::string_view("\"position\": 0").size(),
                            "\"position\": 7");
            expect(!static_cast<bool>(decodeText(outside)), "a stop outside [0, 1] is refused");
        }
        auto unknown = current;
        const auto alphaAt = unknown.find("\"alpha\": ", stopsAt);
        if (alphaAt != std::string::npos) {
            unknown.insert(alphaAt, "\"hue\": 0, ");
            expect(!static_cast<bool>(decodeText(unknown)), "a misplaced stop member is refused");
        }
    }

    // A 1.37 document without gradients migrates to 1.38 in one version-only step.
    document::Document bare{
        document::makeNewProject("Bare", "Main", core::RationalTime::fromInteger(4)).project};
    auto legacy = encode(bare);
    replaceOnce(legacy, "\"minor\": 39", "\"minor\": 37");
    const auto decodedLegacy = decodeText(legacy);
    expect(static_cast<bool>(decodedLegacy), "a 1.37 document without gradients decodes");
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(legacy)), {}, operation);
    expect(static_cast<bool>(parsed), "the 1.37 fixture parses");
    if (parsed) {
        const auto migrated =
            project::migrateDocumentDom(parsed.document()->root(), {1, 37}, {1, 38},
                                        project::kProductionDocumentMigrationSteps, {}, operation);
        expect(migrated.outcome() == project::MigrationOutcome::Migrated &&
                   migrated.stepsApplied() == 1,
               "1.37 -> 1.38 is one step");
        if (migrated.outcome() == project::MigrationOutcome::Migrated) {
            const auto decodedMigrated = project::decodeDocumentEnvelope(*migrated.migratedRoot());
            expect(static_cast<bool>(decodedMigrated) && decodedLegacy &&
                       decodedMigrated.value()->compositions.size() ==
                           decodedLegacy.value()->compositions.size(),
                   "the 1.37 -> 1.38 step only advances the version");
        }
    }
}

// A Colorize with no gradient binding (one saved before 1.38) is valid, saves without a gradient,
// and takes the gradient on its first edit: SetNodeParameterValue binds the optional role.
void unboundColorizeBindsOnFirstEdit() {
    auto initial = document::makeNewProject("Legacy", "Main", core::RationalTime::fromInteger(4));
    const auto compositionId = initial.initialCompositionId;
    const auto nodeId = document::NodeId::fromRaw(9001);
    {
        auto* composition = initial.project.findComposition(compositionId);
        const auto* definition =
            document::builtInNodeDefinitions().find(document::kColorizeNodeType, 1);
        if (composition == nullptr || definition == nullptr)
            return expect(false, "the legacy fixture has a composition and the definition");
        std::vector<document::ParameterBinding> bindings;
        std::uint64_t next = 9100;
        for (const auto& parameter : definition->parameters) {
            if (parameter.role == "gradient")
                continue;
            const auto id = document::ParameterId::fromRaw(next++);
            if (!composition->parameters().insert(
                    {id, parameter.schemaKey, document::ConstantValueSource{parameter.defaultValue}}))
                return expect(false, "a legacy Colorize parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        if (!composition->graph().addNode(
                {nodeId, std::string(document::kColorizeNodeType), std::move(bindings), 1}))
            return expect(false, "the legacy Colorize is accepted");
    }
    expect(initial.project.validate().ok(), "a Colorize without its gradient is valid");
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    expect(encode(document).find("\"kind\": \"gradient\"") == std::string::npos,
           "a Colorize without its gradient writes none");
    commands::Transaction transaction("Use Gradient", document.snapshot().revision());
    transaction.emplace<commands::SetNodeParameterValue>(compositionId, nodeId,
                                                         std::string("gradient"),
                                                         document::ParameterValue{fiveStops()});
    expect(stack.execute(std::move(transaction)).succeeded(),
           "setting an unbound gradient binds it");
    const auto snapshot = document.snapshot();
    const auto* composition = snapshot.project().findComposition(compositionId);
    const auto* node = composition->graph().findNode(nodeId);
    const auto binding =
        std::ranges::find(node->parameters, "gradient", &document::ParameterBinding::role);
    const auto* record = binding == node->parameters.end()
                             ? nullptr
                             : composition->parameters().find(binding->parameterId);
    const auto* constant =
        record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    expect(constant && constant->value == document::ParameterValue{fiveStops()},
           "the bound gradient holds the value given");
    expect(stack.undo().succeeded() && document.snapshot()
                               .project()
                               .findComposition(compositionId)
                               ->graph()
                               .findNode(nodeId)
                               ->parameters.size() == node->parameters.size() - 1,
           "one undo removes the binding again");
}

} // namespace

int main() {
    try {
        gradientsRoundTrip();
        olderAndMalformedDocuments();
        unboundColorizeBindsOnFirstEdit();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: gradient persistence\n";
    return 0;
}
