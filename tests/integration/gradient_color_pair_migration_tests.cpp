// #541: a document older than 1.39 opens its schema 1 colour pairs as gradient values. A Linear or
// Radial Gradient's start and end colours become its two-stop `gradient`, and a Glow's tint and
// outer tint its `tintGradient`; constant colours convert to exactly the gradient a new node with
// those colours holds, and keyed colours to a whole-value gradient curve that samples as they did.
// A 1.39 document cannot carry a schema 1 node. #543: a colour a value node drives stays driven, as
// a link on its stop.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
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

constexpr core::Color4d kStart{0.9, -0.2, 0.4, 0.75};
constexpr core::Color4d kEnd{2.5, 1.0, 0.05, 1.0};
constexpr core::Color4d kTint{1.0, 0.8, 0.6, 1.0};
constexpr core::Color4d kOuter{0.3, 0.5, 1.5, 1.0};

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Pairs", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(32, 16, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId linear;
    document::NodeId glow;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Pairs", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    document::ParameterId parameter(const document::NodeId node, const std::string_view role) {
        const auto* record =
            document.snapshot().project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }
    Fixture() {
        linear = *run<commands::AddNode>(composition, std::string(document::kLinearGradientNodeType),
                                         document::Vec2d{})
                      .outputId<document::NodeId>(commands::kAddNodeOutput);
        glow = *run<commands::AddNode>(composition, std::string(document::kGlowNodeType),
                                       document::Vec2d{300.0, 0.0})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
        run<commands::SetParameterSource>(
            composition, parameter(linear, "gradient"),
            document::ConstantValueSource{document::GradientValue{{{0.0, kStart}, {1.0, kEnd}}}});
        run<commands::SetParameterSource>(composition, parameter(glow, "tint"),
                                          document::ConstantValueSource{kTint});
        run<commands::SetParameterSource>(composition, parameter(glow, "gradient"),
                                          document::ConstantValueSource{true});
        run<commands::SetParameterSource>(
            composition, parameter(glow, "tintGradient"),
            document::ConstantValueSource{document::GradientValue{{{0.0, kTint}, {1.0, kOuter}}}});
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
        throw std::runtime_error("parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

void replaceAfter(std::string& text, const std::string_view anchor, const std::string_view from,
                  const std::string_view to) {
    const auto at = text.find(anchor);
    const auto found = at == std::string::npos ? at : text.find(from, at);
    if (found == std::string::npos)
        throw std::logic_error("fixture anchor missing");
    text.replace(found, from.size(), to);
}

document::NodeRecord& nodeOf(project::DecodedComposition& composition, const document::NodeId id) {
    return *std::ranges::find(composition.graph.nodes, id, &document::NodeRecord::id);
}
document::ParameterRecord& recordOf(project::DecodedComposition& composition,
                                    const document::ParameterId id) {
    return *std::ranges::find(composition.parameters, id, &document::ParameterRecord::id);
}
document::ParameterBinding& bindingOf(document::NodeRecord& node, const std::string_view role) {
    return *std::ranges::find(node.parameters, role, &document::ParameterBinding::role);
}

// Rewrites a current node back to its schema 1 colour pair, as a document saved before 1.39 held
// it: the generator's gradient record becomes its start colour and a new record its end colour;
// the Glow's tint gradient record becomes its outer tint.
void makeLegacy(project::DecodedDocumentEnvelope& envelope, const Fixture& fixture,
                const document::ParameterSource& start,
                const document::ParameterSource& end) {
    auto& composition = envelope.compositions.front();
    auto& linear = nodeOf(composition, fixture.linear);
    const auto gradient = bindingOf(linear, "gradient").parameterId;
    const auto endId = document::ParameterId::fromRaw(++envelope.highWater.parameter);
    bindingOf(linear, "gradient").role = "startColor";
    linear.parameters.push_back({"endColor", endId});
    std::ranges::sort(linear.parameters, {}, &document::ParameterBinding::role);
    linear.schemaVersion = document::kLegacyGradientNodeSchemaVersion;
    auto& startRecord = recordOf(composition, gradient);
    startRecord.schemaKey = std::string(document::kGradientStartColorParameterSchemaKey);
    startRecord.source = start;
    composition.parameters.push_back(
        {endId, std::string(document::kGradientEndColorParameterSchemaKey), end});

    auto& glow = nodeOf(composition, fixture.glow);
    const auto tintGradient = bindingOf(glow, "tintGradient").parameterId;
    bindingOf(glow, "tintGradient").role = "outerTint";
    std::ranges::sort(glow.parameters, {}, &document::ParameterBinding::role);
    glow.schemaVersion = document::kLegacyGlowNodeSchemaVersion;
    auto& outer = recordOf(composition, tintGradient);
    outer.schemaKey = std::string(document::kGlowOuterTintSchemaKey);
    outer.source = document::ConstantValueSource{kOuter};
}

void constantPairsConvertExactly() {
    Fixture fixture;
    const auto current = decodeText(encode(fixture.document));
    expect(static_cast<bool>(current), "the current document decodes");
    if (!current)
        return;
    auto legacy = *current.value();
    makeLegacy(legacy, fixture, document::ConstantValueSource{kStart},
               document::ConstantValueSource{kEnd});
    project::migrateLegacyGradientColorPairs(legacy);
    project::migrateLegacyGlowNodes(legacy);
    expect(legacy.compositions == current.value()->compositions,
           "constant colour pairs open as the gradients a new node with those colours holds, on "
           "the same parameter ids");
}

// A keyed start colour (keys at 0 and 2) and a keyed end colour (keys at 0 and 1), all Linear:
// the gradient keys at 0, 1 and 2 and samples every frame between them as the colours did.
void keyedPairsKeepTheirMotion() {
    Fixture fixture;
    const auto current = decodeText(encode(fixture.document));
    if (!current)
        return expect(false, "the current document decodes");
    auto legacy = *current.value();
    const auto startCurve = document::AnimationCurveId::fromRaw(++legacy.highWater.animationCurve);
    const auto endCurve = document::AnimationCurveId::fromRaw(++legacy.highWater.animationCurve);
    const auto keyed = [&](const document::AnimationCurveId id, const core::Color4d from,
                           const core::Color4d to, const std::int64_t last) {
        document::Color4AnimationCurve curve;
        curve.id = id;
        const std::array<std::pair<double, double>, 4> channels{
            std::pair{from.red, to.red}, std::pair{from.green, to.green},
            std::pair{from.blue, to.blue}, std::pair{from.alpha, to.alpha}};
        for (std::size_t index = 0; index < 4; ++index)
            curve.components[index].keyframes = {
                {document::KeyframeId::fromRaw(++legacy.highWater.keyframe),
                 core::RationalTime::fromInteger(0), channels[index].first},
                {document::KeyframeId::fromRaw(++legacy.highWater.keyframe),
                 core::RationalTime::fromInteger(last), channels[index].second}};
        return curve;
    };
    const core::Color4d startLater{0.1, 0.4, -0.6, 0.25};
    const core::Color4d endLater{0.0, 3.0, 0.5, 0.5};
    legacy.compositions.front().animationCurves.emplace_back(
        keyed(startCurve, kStart, startLater, 2));
    legacy.compositions.front().animationCurves.emplace_back(keyed(endCurve, kEnd, endLater, 1));
    makeLegacy(legacy, fixture,
               document::AnimationCurveSource{startCurve, document::ParameterValue{kStart}},
               document::AnimationCurveSource{endCurve, document::ParameterValue{kEnd}});
    project::migrateLegacyGradientColorPairs(legacy);
    auto& composition = legacy.compositions.front();
    auto& linear = nodeOf(composition, fixture.linear);
    expect(linear.schemaVersion == document::kGradientNodeSchemaVersion &&
               std::ranges::count(linear.parameters, "gradient",
                                  &document::ParameterBinding::role) == 1 &&
               std::ranges::count(linear.parameters, "endColor",
                                  &document::ParameterBinding::role) == 0,
           "the keyed generator moves to schema 2 with one gradient binding");
    const auto& record = recordOf(composition, bindingOf(linear, "gradient").parameterId);
    const auto* source = std::get_if<document::AnimationCurveSource>(&record.source);
    const document::GradientAnimationCurve* curve = nullptr;
    for (const auto& held : composition.animationCurves) {
        if (const auto* gradient = std::get_if<document::GradientAnimationCurve>(&held);
            gradient && source && gradient->id == source->curveId)
            curve = gradient;
        expect(!std::holds_alternative<document::Color4AnimationCurve>(held),
               "the colour curves are retired");
    }
    expect(record.schemaKey == document::kGradientStopsParameterSchemaKey && curve &&
               curve->keyframes.size() == 3,
           "the keyed colours become a gradient curve keyed where either colour was");
    if (!curve)
        return;
    const auto compiled = runtime::compileAnimationCurve(*curve);
    const auto lerp = [](const core::Color4d& a, const core::Color4d& b, const double u) {
        return core::Color4d{a.red + (b.red - a.red) * u, a.green + (b.green - a.green) * u,
                             a.blue + (b.blue - a.blue) * u, a.alpha + (b.alpha - a.alpha) * u};
    };
    bool matches = true;
    for (int quarter = 0; quarter <= 10; ++quarter) {
        const auto time = *core::RationalTime::create(quarter, 4);
        const double seconds = quarter / 4.0;
        const auto sampled = runtime::sampleGradientCurve(compiled, time);
        const auto start = lerp(kStart, startLater, std::min(seconds / 2.0, 1.0));
        const auto end = lerp(kEnd, endLater, std::min(seconds, 1.0));
        const auto near = [](const core::Color4d& a, const core::Color4d& b) {
            return std::abs(a.red - b.red) < 1e-12 && std::abs(a.green - b.green) < 1e-12 &&
                   std::abs(a.blue - b.blue) < 1e-12 && std::abs(a.alpha - b.alpha) < 1e-12;
        };
        matches = matches && sampled && sampled->stops.size() == 2 &&
                  near(sampled->stops[0].color, start) && near(sampled->stops[1].color, end) &&
                  sampled->stops[0].position == 0.0 && sampled->stops[1].position == 1.0;
    }
    expect(matches, "the gradient samples every time as the keyed colours did");
}

// #543: a start colour and an end colour a Colour node drives, and a Glow tint it drives: each
// opens as a link on its stop (the generator's stops 0 and 1, the Glow gradient's stop 0) with the
// colour's driver, beside the Glow's own driven tint, and the document validates and compiles.
void drivenPairsStayDriven() {
    Fixture fixture;
    const auto value = *fixture
                            .run<commands::AddNode>(fixture.composition,
                                                    std::string(document::kColorValueNodeType),
                                                    document::Vec2d{-300.0, 0.0})
                            .outputId<document::NodeId>(commands::kAddNodeOutput);
    const auto current = decodeText(encode(fixture.document));
    if (!current)
        return expect(false, "the current document decodes");
    auto legacy = *current.value();
    const document::DriverBindingSource driver{value, std::string(document::kValuePortName)};
    makeLegacy(legacy, fixture, driver, driver);
    auto& composition = legacy.compositions.front();
    recordOf(composition, bindingOf(nodeOf(composition, fixture.glow), "tint").parameterId).source =
        driver;
    project::migrateLegacyGradientColorPairs(legacy);
    project::migrateLegacyGlowNodes(legacy);
    auto& migrated = legacy.compositions.front();
    const auto linkOf = [&](const document::NodeId node,
                            const std::string_view port) -> const document::ParameterRecord* {
        auto& record = nodeOf(migrated, node);
        const auto binding =
            std::ranges::find(record.parameters, port, &document::ParameterBinding::role);
        if (binding == record.parameters.end())
            return nullptr;
        return &recordOf(migrated, binding->parameterId);
    };
    for (const auto* port : {"gradient.stops[0].color", "gradient.stops[1].color"}) {
        const auto* link = linkOf(fixture.linear, port);
        expect(link && link->schemaKey == document::kGradientStopColorSchemaKey &&
                   link->source == document::ParameterSource{driver},
               "a driven generator colour opens as a link on its stop with the colour's driver");
    }
    const auto* tintLink = linkOf(fixture.glow, "tintGradient.stops[0].color");
    expect(tintLink && tintLink->source == document::ParameterSource{driver},
           "a driven Glow tint drives the tint gradient's first stop too");
    expect(linkOf(fixture.glow, "tintGradient.stops[1].color") == nullptr,
           "a constant outer tint opens without a link");
    const auto* tint = linkOf(fixture.glow, "tint");
    expect(tint && tint->source == document::ParameterSource{driver},
           "the Glow keeps its own driven tint");
    expect(legacy.highWater.parameter >=
               std::ranges::max(migrated.parameters, {}, &document::ParameterRecord::id).id.value(),
           "the parameter high water covers the new links");
    expect(std::ranges::is_sorted(migrated.parameters, {}, &document::ParameterRecord::id),
           "the links keep the parameters in id order");
    // The migrated document, stop links and all, passes the document model's validation.
    expect(static_cast<bool>(project::reconstructDocument(legacy)),
           "the migrated document with its stop links reconstructs");
}

// The schema 1 nodes are admitted only in a document older than 1.39.
void legacyNodesNeedAnOlderDocument() {
    Fixture fixture;
    auto text = encode(fixture.document);
    replaceAfter(text, "\"typeId\": \"bloom.linear-gradient\"", "\"schemaVersion\": 2",
                 "\"schemaVersion\": 1");
    replaceAfter(text, "\"typeId\": \"bloom.glow\"", "\"schemaVersion\": 2",
                 "\"schemaVersion\": 1");
    const auto current = decodeText(text);
    expect(!current && current.error() == project::DocumentDecodeError::UnsupportedNodeVersion,
           "a 1.39 document carrying a schema 1 gradient or Glow is refused");
    auto older = text;
    replaceAfter(older, "\"schemaVersion\": {", "\"minor\": 43", "\"minor\": 38");
    expect(static_cast<bool>(decodeText(older)),
           "a 1.38 document's schema 1 gradient and Glow decode for the migration");
}

} // namespace

int main() {
    try {
        constantPairsConvertExactly();
        keyedPairsKeepTheirMotion();
        drivenPairsStayDriven();
        legacyNodesNeedAnOlderDocument();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: schema 1 colour pairs open as gradient values\n";
    return 0;
}
