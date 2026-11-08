// #543: a value node drives a gradient stop's colour or position. Linking a Colour or Scalar output
// into a stop's socket binds a stop link (a driver-only parameter under the stop's socket name),
// and the gradient renders exactly as one authored with the driven value; unlinking returns the
// authored stop. Linear Gradient, Colorize's Gradient Map and Glow's tint gradient follow their
// links, a changed driver invalidates the frame, a keyed driver makes the source time dependent, an
// edit that moves or removes stops moves or removes their links, and a link survives a save and
// reopen in document 1.40 while a document claiming 1.39 that carries one is refused.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <span>
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

constexpr core::Color4d kDriven{1.5, 0.25, -0.5, 0.75};
const document::GradientValue kAuthored{{{0.0, core::Color4d{0.0, 0.0, 0.0, 1.0}},
                                         {0.6, core::Color4d{0.2, 0.9, 0.4, 1.0}},
                                         {1.0, core::Color4d{1.0, 1.0, 1.0, 1.0}}}};

// A layer whose content is `kind` (a Linear Gradient, or a Colorize or Glow over a bright Solid),
// and a Colour and a Scalar value node to drive its gradient's stops.
struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Stops", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(32, 8, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId node;
    std::string role;
    document::NodeId colour;
    document::NodeId scalar;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Stops", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    template <typename Operation, typename... Arguments>
    commands::CommandResult attempt(Arguments&&... arguments) {
        commands::Transaction transaction("Stops", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        return commands.execute(std::move(transaction));
    }
    [[nodiscard]] const document::Composition& current() const {
        return *document.snapshot().project().findComposition(composition);
    }
    [[nodiscard]] std::optional<document::ParameterId> find(const document::NodeId id,
                                                            const std::string_view binding) const {
        const auto* record = current().graph().findNode(id);
        for (const auto& candidate : record->parameters)
            if (candidate.role == binding)
                return candidate.parameterId;
        return std::nullopt;
    }
    [[nodiscard]] document::ParameterId parameter(const document::NodeId id,
                                                  const std::string_view binding) const {
        if (const auto found = find(id, binding))
            return *found;
        throw std::runtime_error("binding missing");
    }
    document::NodeId add(const std::string_view type) {
        return *run<commands::AddNode>(composition, std::string(type), document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    explicit Fixture(const std::string_view kind) {
        const auto solid = run<commands::AddSolidLayer>(
            composition, "Light", core::Color4d{0.6, 0.3, 0.1, 1.0}, document::Vec2d{16.0, 4.0});
        const auto source =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto layer =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        node = add(kind);
        if (kind == document::kLinearGradientNodeType) {
            role = "gradient";
        } else {
            run<commands::ConnectPorts>(composition, document::OutputPortRef{source, "image"},
                                        document::NodeInputRef{node, "input"});
            if (kind == document::kColorizeNodeType) {
                role = "gradient";
                run<commands::SetParameterSource>(
                    composition, parameter(node, "mode"),
                    document::ConstantValueSource{document::kColorizeGradientMap});
            } else {
                role = "tintGradient";
                run<commands::SetParameterSource>(composition, parameter(node, "gradient"),
                                                  document::ConstantValueSource{true});
                run<commands::SetParameterSource>(composition, parameter(node, "threshold"),
                                                  document::ConstantValueSource{0.1});
                run<commands::SetParameterSource>(composition, parameter(node, "octaves"),
                                                  document::ConstantValueSource{std::int64_t{3}});
            }
        }
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{node, "image"},
            document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)});
        run<commands::SetParameterSource>(composition, parameter(node, role),
                                          document::ConstantValueSource{kAuthored});
        colour = add(document::kColorValueNodeType);
        run<commands::SetParameterSource>(composition,
                                          parameter(colour, document::kValueParameterRole),
                                          document::ConstantValueSource{kDriven});
        scalar = add(document::kScalarValueNodeType);
        run<commands::SetParameterSource>(composition,
                                          parameter(scalar, document::kValueParameterRole),
                                          document::ConstantValueSource{0.25});
    }
    [[nodiscard]] document::NodeInputRef
    stop(const std::uint32_t index, const document::GradientStopComponent component) const {
        return {node, document::gradientStopPortName(role, index, component)};
    }
    void link(const document::NodeId from, const document::NodeInputRef& to) {
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{from, std::string(document::kValuePortName)}, to);
    }
};

struct Render final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    std::vector<render::Rgba32f> pixels;
};

Render render(const document::Snapshot& snapshot, const document::CompositionId composition,
              const runtime::CpuCompositionEvaluator& evaluator, const bool bypassCache = true) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({snapshot, composition}, {});
    if (!compiled.plan) {
        for (const auto& diagnostic : compiled.diagnostics)
            std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
        throw std::runtime_error("stop link fixture compiles");
    }
    auto request =
        runtime::EvaluationRequest{.time = core::RationalTime{},
                                   .output = compiled.plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = 64U << 20U};
    request.bypassOperationCache = bypassCache;
    const auto result = evaluator.evaluate(compiled.plan, request, {});
    if (!result.frame())
        throw std::runtime_error("stop link fixture evaluates");
    const auto pixels = result.frame()->processImage().pixels();
    return {compiled.plan, {pixels.begin(), pixels.end()}};
}
Render render(const Fixture& fixture) {
    return render(fixture.document.snapshot(), fixture.composition,
                  runtime::CpuCompositionEvaluator{});
}

bool sameBits(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(render::Rgba32f)) == 0);
}

// The port names, their inverse and the refusals.
void portNames() {
    using document::GradientStopComponent;
    const auto name =
        document::gradientStopPortName("gradient", 12, GradientStopComponent::Position);
    const auto parsed = document::parseGradientStopPort(name);
    expect(name == "gradient.stops[12].position" && parsed && parsed->role == "gradient" &&
               parsed->stop == 12 && parsed->component == GradientStopComponent::Position,
           "a stop port name round-trips");
    for (const auto* bad :
         {"gradient.stops[64].color", "gradient.stops[07].color", "gradient.stops[-1].color",
          "gradient.stops[].color", "gradient.stops[1].alpha", ".stops[1].color", "gradient"})
        expect(!document::parseGradientStopPort(bad), "a malformed stop port is not a stop port");
    const auto* linear = document::builtInNodeDefinitions().find(
        document::kLinearGradientNodeType, document::kGradientNodeSchemaVersion);
    const auto colour =
        linear ? document::findInputPort(*linear, "gradient.stops[3].color") : std::nullopt;
    const auto position =
        linear ? document::findInputPort(*linear, "gradient.stops[3].position") : std::nullopt;
    expect(colour && colour->valueKind == document::SocketValueKind::Color && position &&
               position->valueKind == document::SocketValueKind::Scalar &&
               !document::findInputPort(*linear, "start.stops[0].color"),
           "a gradient parameter declares a Color and a Scalar socket per stop, and nothing else "
           "does");
    const auto applied = document::applyGradientStopValues(
        kAuthored,
        std::array{document::GradientStopValue{1, GradientStopComponent::Color, kDriven},
                   document::GradientStopValue{2, GradientStopComponent::Position, 4.0},
                   document::GradientStopValue{9, GradientStopComponent::Color, kDriven}});
    expect(applied && applied->stops[1].color == kDriven && applied->stops[2].position == 1.0 &&
               applied->stops[0] == kAuthored.stops[0],
           "driven values replace their stops, a position clamps and a missing stop is untouched");
    expect(!document::applyGradientStopValues(
               kAuthored, std::array{document::GradientStopValue{
                              0, GradientStopComponent::Color, core::Color4d{0.0, 0.0, 0.0, 2.0}}}),
           "a driven colour whose alpha leaves [0, 1] is not evaluable");
}

// A linked stop renders as the same stop authored with the driven value; unlinking returns the
// authored stop.
void linkedStopsRenderAsAuthored(const std::string_view kind, const char* label) {
    using document::GradientStopComponent;
    Fixture linked(kind);
    const auto authored = render(linked);
    linked.link(linked.colour, linked.stop(1, GradientStopComponent::Color));
    linked.link(linked.scalar, linked.stop(1, GradientStopComponent::Position));
    const auto colourPort = linked.stop(1, GradientStopComponent::Color).port;
    const auto link = linked.find(linked.node, colourPort);
    const auto* record = link ? linked.current().parameters().find(*link) : nullptr;
    expect(record && record->schemaKey == document::kGradientStopColorSchemaKey &&
               std::holds_alternative<document::DriverBindingSource>(record->source),
           "linking a stop binds a stop link that holds the driver");
    const auto drivenFrame = render(linked);

    Fixture reference(kind);
    auto expected = kAuthored;
    expected.stops[1].color = kDriven;
    expected.stops[1].position = 0.25;
    reference.run<commands::SetParameterSource>(reference.composition,
                                                reference.parameter(reference.node, reference.role),
                                                document::ConstantValueSource{expected});
    expect(sameBits(drivenFrame.pixels, render(reference).pixels), label);
    expect(!sameBits(drivenFrame.pixels, authored.pixels), "the driven stop changes the picture");

    linked.run<commands::DisconnectInput>(linked.composition,
                                          linked.stop(1, GradientStopComponent::Color));
    linked.run<commands::DisconnectInput>(linked.composition,
                                          linked.stop(1, GradientStopComponent::Position));
    expect(!linked.find(linked.node, colourPort) &&
               (!link || linked.current().parameters().find(*link) == nullptr),
           "unlinking removes the stop link");
    expect(sameBits(render(linked).pixels, authored.pixels),
           "an unlinked stop renders as authored");
}

void linksAreSocketsNotEdges() {
    using document::GradientStopComponent;
    Fixture fixture(document::kLinearGradientNodeType);
    const auto text = fixture.add(document::kStringValueNodeType);
    const auto refused = fixture.attempt<commands::ConnectPorts>(
        fixture.composition, document::OutputPortRef{text, std::string(document::kValuePortName)},
        fixture.stop(0, GradientStopComponent::Color));
    expect(!refused.succeeded(), "a String output does not drive a stop colour");
    const auto integer = fixture.add(document::kIntegerValueNodeType);
    fixture.link(integer, fixture.stop(0, GradientStopComponent::Position));
    expect(fixture.find(fixture.node, "gradient.stops[0].position").has_value(),
           "an Integer output drives a stop position through the Scalar promotion");
    static_cast<void>(render(fixture));
    auto graph = fixture.current().graph();
    expect(!graph.addEdge(
               {document::EdgeId::fromRaw(999999),
                document::OutputPortRef{fixture.colour, std::string(document::kValuePortName)},
                fixture.stop(1, GradientStopComponent::Color)}),
           "a stop socket takes a link, never an edge");
    expect(fixture.current().graph().inputKind(fixture.stop(5, GradientStopComponent::Color)) ==
               document::SocketValueKind::Color,
           "a stop socket is a Color input");
}

// Moving and removing stops moves and removes their links.
void linksFollowTheirStops() {
    using document::GradientStopComponent;
    Fixture fixture(document::kLinearGradientNodeType);
    fixture.link(fixture.colour, fixture.stop(2, GradientStopComponent::Color));
    fixture.link(fixture.scalar, fixture.stop(0, GradientStopComponent::Position));
    const auto colourLink = fixture.parameter(fixture.node, "gradient.stops[2].color");
    fixture.run<commands::RemapGradientStopLinks>(
        fixture.composition, fixture.node, "gradient",
        std::vector<std::optional<std::uint32_t>>{std::nullopt, 0U, 1U});
    expect(fixture.find(fixture.node, "gradient.stops[1].color") == colourLink &&
               !fixture.find(fixture.node, "gradient.stops[2].color") &&
               !fixture.find(fixture.node, "gradient.stops[0].position"),
           "a moved stop keeps its link and a removed stop's link goes with it");
    fixture.link(fixture.colour, fixture.stop(0, GradientStopComponent::Color));
    const auto clash = fixture.attempt<commands::RemapGradientStopLinks>(
        fixture.composition, fixture.node, "gradient",
        std::vector<std::optional<std::uint32_t>>{1U});
    expect(!clash.succeeded(), "two links never land on one stop");
    expect(fixture.find(fixture.node, "gradient.stops[1].color") == colourLink,
           "a refused remap leaves the links alone");
}

// A driver whose value changes invalidates the cached frame; a keyed driver makes the gradient
// source time dependent.
void driversInvalidateAndAnimate() {
    using document::GradientStopComponent;
    Fixture fixture(document::kLinearGradientNodeType);
    fixture.link(fixture.colour, fixture.stop(0, GradientStopComponent::Color));
    const runtime::CpuCompositionEvaluator evaluator;
    const auto first = render(fixture.document.snapshot(), fixture.composition, evaluator, false);
    fixture.run<commands::SetParameterSource>(
        fixture.composition, fixture.parameter(fixture.colour, document::kValueParameterRole),
        document::ConstantValueSource{core::Color4d{0.1, 0.2, 0.3, 1.0}});
    const auto second = render(fixture.document.snapshot(), fixture.composition, evaluator, false);
    expect(!sameBits(first.pixels, second.pixels),
           "a changed driver re-renders the gradient through a warm cache");
    expect(!first.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "a constant driver leaves the gradient source time independent");
    const auto time = fixture.add(document::kTimeValueNodeType);
    fixture.run<commands::ConnectPorts>(fixture.composition,
                                        document::OutputPortRef{time, "seconds"},
                                        fixture.stop(1, GradientStopComponent::Position));
    const auto animated = render(fixture);
    expect(animated.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "a time-driven stop makes the gradient source time dependent");
}

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

// A link saves and reopens in 1.40; a document claiming 1.39 that carries one is refused.
void linksPersist() {
    using document::GradientStopComponent;
    Fixture fixture(document::kLinearGradientNodeType);
    fixture.link(fixture.colour, fixture.stop(1, GradientStopComponent::Color));
    const auto snapshot = fixture.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto saved = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(saved), "a document with a stop link saves");
    if (!saved)
        return;
    const auto bytes = saved.archive()->bytes();
    auto opened = project::openProjectArchive({bytes.begin(), bytes.end()}, {}, memory());
    expect(opened.outcome() == project::OpenArchiveOutcome::Opened,
           "a document with a stop link reopens");
    if (opened.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    const auto reopened = std::move(opened).takeOpened();
    expect(encode(*reopened.document) == encode(fixture.document), "the stop link reopens exactly");

    auto text = encode(fixture.document);
    const auto version = text.find("\"minor\": 43");
    if (version == std::string::npos)
        return expect(false, "the document declares 1.42");
    text.replace(version, std::string_view("\"minor\": 43").size(), "\"minor\": 39");
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    const auto decoded = parsed ? project::decodeDocumentEnvelope(parsed.document()->root())
                                : project::DocumentDecodeResult::failure(
                                      project::DocumentDecodeError::DomainViolation, "/");
    expect(!decoded && decoded.error() == project::DocumentDecodeError::UnsupportedParameterSource,
           "a document claiming 1.39 that carries a stop link is refused");
}

} // namespace

int main() {
    try {
        portNames();
        linkedStopsRenderAsAuthored(document::kLinearGradientNodeType,
                                    "a Linear Gradient's linked stop renders as authored");
        linkedStopsRenderAsAuthored(document::kColorizeNodeType,
                                    "a Gradient Map's linked stop renders as authored");
        linkedStopsRenderAsAuthored(document::kGlowNodeType,
                                    "a Glow tint gradient's linked stop renders as authored");
        linksAreSocketsNotEdges();
        linksFollowTheirStops();
        driversInvalidateAndAnimate();
        linksPersist();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: value nodes drive gradient stops\n";
    return 0;
}
