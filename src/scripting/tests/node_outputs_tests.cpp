// #591: every output of a node at a time, as values (query kind=outputs and bloom.data.outputs
// share scripting::readNodeOutputs). A Coordinate Space read by a Space Readout, To Space and From
// Space is evaluated headless, as MCP's stdio host and the Script editor read it: each readout
// equals the map computed here independently, the answers follow the space when it changes, a
// picture output says it is no value, and every refusal is typed. With a space connected, Axes and
// Axis Tick read neither their own Origin nor their Pixels Per Unit: setting or wiring one changes
// nothing in the plan, and disconnecting the space reads them again.

#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/coordinate_space.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter_activity.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/scripting/readings.hpp>
#include <bloom/scripting/session.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <source_location>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace bloom;
using document::NodeId;
using document::Vec2d;
using scripting::NodeOutputsStatus;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

[[nodiscard]] bool near(const double value, const double expected, const double tolerance = 1e-9) {
    return std::abs(value - expected) <= tolerance * std::max(1.0, std::abs(expected));
}
[[nodiscard]] bool near(const Vec2d value, const Vec2d expected) {
    return near(value.x, expected.x) && near(value.y, expected.y);
}
[[nodiscard]] std::string text(const Vec2d value) {
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ")";
}

struct Fixture final {
    std::unique_ptr<scripting::Session> session;
    document::CompositionId composition;

    Fixture() {
        auto created = scripting::Session::createNew(
            "Outputs", "Main", core::RationalTime::fromInteger(4),
            *document::CompositionFormat::create(512, 384, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)));
        if (!created)
            std::abort();
        session = std::move(created).takeSession();
        composition = session->snapshot().project().compositions().front().id();
    }

    template <typename Operation, typename... Arguments>
    scripting::SessionCommandResult run(Arguments&&... args) {
        commands::Transaction transaction("Fixture", session->snapshot().revision());
        transaction.emplace<Operation>(composition, std::forward<Arguments>(args)...);
        auto result = session->execute(std::move(transaction));
        expect(result.succeeded(), "a fixture command applies");
        return result;
    }

    NodeId add(const std::string_view type) {
        return run<commands::AddNode>(std::string(type), Vec2d{})
            .command->outputId<NodeId>(commands::kAddNodeOutput)
            .value_or(NodeId{});
    }
    void connect(const NodeId from, const std::string& output, const NodeId to,
                 const std::string& input) {
        run<commands::ConnectPorts>(document::OutputPortRef{from, output},
                                    document::NodeInputRef{to, input});
    }
    void disconnect(const NodeId to, const std::string& input) {
        run<commands::DisconnectInput>(document::NodeInputRef{to, input});
    }

    [[nodiscard]] document::ParameterId role(const NodeId node, const std::string_view name) const {
        const auto snapshot = session->snapshot();
        const auto* record = snapshot.project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == name)
                return binding.parameterId;
        return {};
    }
    void set(const NodeId node, const std::string_view name, document::ParameterValue value) {
        run<commands::SetParameterSource>(role(node, name),
                                          document::ConstantValueSource{std::move(value)});
    }

    [[nodiscard]] scripting::NodeOutputsReading read(const NodeId node, const std::int64_t frame = 0,
                                                     scripting::OperationContext context = {}) {
        if (!context.source)
            context = session->operationContext();
        return scripting::readNodeOutputs(context, composition,
                                          *core::RationalTime::create(frame, 24), node);
    }

    [[nodiscard]] bool inactive(const NodeId node, const std::string_view roleName) const {
        const auto snapshot = session->snapshot();
        const auto* owner = snapshot.project().findComposition(composition);
        const auto* record = owner->graph().findNode(node);
        const auto* parameter = owner->parameters().find(role(node, roleName));
        return document::isParameterInactive(*owner, *record, parameter->schemaKey, roleName);
    }
};

// One output's value by name; none when the output is missing or has no value.
template <typename Kind>
[[nodiscard]] const Kind* valueOf(const scripting::NodeOutputsReading& reading,
                                  const std::string_view name) {
    for (const auto& output : reading.outputs)
        if (output.name == name)
            return output.value ? std::get_if<Kind>(&*output.value) : nullptr;
    return nullptr;
}

// The map this test expects a Scale-mode space to be, built here from the definition in
// docs/architecture/coordinate-spaces.md (T(origin) * R(clockwise degrees, y down) * S(sx, -sy)),
// not from Bloom's own helpers.
struct Expected final {
    double a, b, c, d, e, f;
    [[nodiscard]] Vec2d toPixels(const Vec2d unit) const {
        return {a * unit.x + b * unit.y + c, d * unit.x + e * unit.y + f};
    }
    [[nodiscard]] Vec2d toUnits(const Vec2d pixel) const {
        const double det = a * e - b * d;
        const double x = pixel.x - c;
        const double y = pixel.y - f;
        return {(e * x - b * y) / det, (a * y - d * x) / det};
    }
};
[[nodiscard]] Expected scaleSpace(const Vec2d origin, const Vec2d perUnit, const double degrees) {
    const double radians = degrees * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    return {cosine * perUnit.x, sine * perUnit.y, origin.x,
            sine * perUnit.x,   -cosine * perUnit.y, origin.y};
}

struct SpaceScene final {
    NodeId space, readout, toSpace, fromSpace, fromVector;
};

SpaceScene buildSpaceScene(Fixture& fixture) {
    SpaceScene scene{};
    scene.space = fixture.add(document::kCoordinateSpaceNodeType);
    scene.readout = fixture.add(document::kSpaceReadoutNodeType);
    scene.toSpace = fixture.add(document::kToSpaceNodeType);
    scene.fromSpace = fixture.add(document::kFromSpaceNodeType);
    scene.fromVector = fixture.add(document::kFromSpaceNodeType);
    fixture.set(scene.space, "origin", Vec2d{256.0, 200.0});
    fixture.set(scene.space, "pixelsPerUnit", Vec2d{40.0, 30.0});
    fixture.set(scene.space, "rotation", 30.0);
    fixture.set(scene.space, "rangeMin", Vec2d{-4.0, -3.0});
    fixture.set(scene.space, "rangeMax", Vec2d{4.0, 3.0});
    fixture.set(scene.toSpace, "pixels", Vec2d{300.0, 120.0});
    fixture.set(scene.fromSpace, "units", Vec2d{1.5, 2.5});
    fixture.set(scene.fromVector, "units", Vec2d{1.0, 0.0});
    fixture.set(scene.fromVector, "conversion",
                static_cast<std::int64_t>(document::SpaceConversion::Vector));
    for (const auto consumer : {scene.readout, scene.toSpace, scene.fromSpace, scene.fromVector})
        fixture.connect(scene.space, "space", consumer, "space");
    return scene;
}

void checkReadout(const scripting::NodeOutputsReading& reading, const Expected& map,
                  const std::string& when) {
    expect(reading.status == NodeOutputsStatus::Evaluated, when + ": the readout evaluates");
    expect(reading.type == document::kSpaceReadoutNodeType && reading.outputs.size() == 12,
           when + ": every one of the readout's twelve outputs is listed");
    const auto* origin = valueOf<Vec2d>(reading, "origin");
    expect(origin != nullptr && near(*origin, map.toPixels({0.0, 0.0})),
           when + ": origin is where (0, 0) lands" + (origin ? " " + text(*origin) : ""));
    const auto* xAxis = valueOf<Vec2d>(reading, "xAxis");
    const auto* yAxis = valueOf<Vec2d>(reading, "yAxis");
    expect(xAxis != nullptr && near(*xAxis, {map.a, map.d}) && yAxis != nullptr &&
               near(*yAxis, {map.b, map.e}),
           when + ": the axes are one unit's step in pixels");
    const auto* scale = valueOf<Vec2d>(reading, "scale");
    expect(scale != nullptr && near(*scale, {std::hypot(map.a, map.d), std::hypot(map.b, map.e)}),
           when + ": scale is each axis's length");
    const auto* rotation = valueOf<double>(reading, "rotation");
    expect(rotation != nullptr && near(*rotation, std::atan2(map.d, map.a) * 180.0 / std::numbers::pi),
           when + ": rotation is the x axis's angle");
    const auto* rangeMin = valueOf<Vec2d>(reading, "rangeMin");
    const auto* rangeMax = valueOf<Vec2d>(reading, "rangeMax");
    expect(rangeMin != nullptr && near(*rangeMin, {-4.0, -3.0}) && rangeMax != nullptr &&
               near(*rangeMax, {4.0, 3.0}),
           when + ": the range is the space's");
    const auto* matrix = valueOf<runtime::CompiledNumberList>(reading, "matrix");
    const std::array<double, 9> rows{map.a, map.b, map.c, map.d, map.e, map.f, 0.0, 0.0, 1.0};
    bool same = matrix != nullptr && matrix->values->size() == 9;
    for (std::size_t index = 0; same && index < 9; ++index)
        same = near((*matrix->values)[index], rows[index]);
    expect(same, when + ": the matrix is the map, row by row");
    const auto* name = valueOf<std::string>(reading, "name");
    const auto* parent = valueOf<std::string>(reading, "parentName");
    expect(name != nullptr && *name == "graph" && parent != nullptr && parent->empty(),
           when + ": the space is named graph and has no parent");
    const auto* projective = valueOf<bool>(reading, "projective");
    const auto* valid = valueOf<bool>(reading, "valid");
    expect(projective != nullptr && !*projective && valid != nullptr && *valid,
           when + ": an affine space reads valid and not projective");
    expect(reading.nodeDiagnostics.empty(), when + ": the readout reports nothing");
}

void checkConversions(Fixture& fixture, const SpaceScene& scene, const Expected& map,
                      const std::string& when) {
    const auto to = fixture.read(scene.toSpace);
    const auto* units = valueOf<Vec2d>(to, "result");
    expect(units != nullptr && near(*units, map.toUnits({300.0, 120.0})),
           when + ": To Space answers the space's units of a pixel" +
               (units ? " " + text(*units) : ""));
    const auto from = fixture.read(scene.fromSpace);
    const auto* pixels = valueOf<Vec2d>(from, "result");
    expect(pixels != nullptr && near(*pixels, map.toPixels({1.5, 2.5})),
           when + ": From Space answers the pixel of a point");
    const auto vector = fixture.read(scene.fromVector);
    const auto* step = valueOf<Vec2d>(vector, "result");
    expect(step != nullptr && near(*step, {map.a, map.d}),
           when + ": From Space in vector mode answers the linear part");
    for (const auto* reading : {&to, &from, &vector}) {
        const auto* valid = valueOf<bool>(*reading, "valid");
        expect(valid != nullptr && *valid, when + ": every conversion is valid");
    }
}

void spaceReadoutsAreValues() {
    Fixture fixture;
    const auto scene = buildSpaceScene(fixture);
    auto map = scaleSpace({256.0, 200.0}, {40.0, 30.0}, 30.0);
    checkReadout(fixture.read(scene.readout), map, "first space");
    checkConversions(fixture, scene, map, "first space");

    // The Coordinate Space's own output: the space as values.
    const auto defined = fixture.read(scene.space);
    expect(defined.outputs.size() == 1 && defined.outputs[0].kind == document::SocketValueKind::Space,
           "a Coordinate Space has one space output");
    const auto* held = valueOf<runtime::CompiledSpaceValue>(defined, "space");
    expect(held != nullptr && held->value != nullptr, "the space output holds a space");
    if (held != nullptr && held->value != nullptr) {
        const auto& space = *held->value;
        const std::array<double, 16> embedded{map.a, map.b, 0.0, map.c, map.d, map.e, 0.0, map.f,
                                              0.0,   0.0,   1.0, 0.0,   0.0,   0.0,   0.0, 1.0};
        bool same = space.name == "graph" && space.dimensions == 2 && space.parent == nullptr &&
                    space.range && near(space.range->minimum.x, -4.0) &&
                    near(space.range->maximum.y, 3.0);
        for (std::size_t index = 0; same && index < 16; ++index)
            same = near(space.toPixels[index], embedded[index]);
        expect(same, "the space carries its name, its 4x4 map and its range");
    }

    // The answers follow the space when it changes, at a later revision.
    const auto before = fixture.read(scene.readout).revision;
    fixture.set(scene.space, "origin", Vec2d{100.0, 50.0});
    fixture.set(scene.space, "pixelsPerUnit", Vec2d{64.0, 16.0});
    fixture.set(scene.space, "rotation", -45.0);
    map = scaleSpace({100.0, 50.0}, {64.0, 16.0}, -45.0);
    const auto after = fixture.read(scene.readout);
    expect(after.revision > before, "the read names the later revision");
    checkReadout(after, map, "moved space");
    checkConversions(fixture, scene, map, "moved space");

    // Nothing connected: the readout says there is no space.
    fixture.disconnect(scene.readout, "space");
    const auto alone = fixture.read(scene.readout);
    const auto* valid = valueOf<bool>(alone, "valid");
    expect(alone.status == NodeOutputsStatus::Evaluated && valid != nullptr && !*valid,
           "a readout with no space connected reads not valid");
}

void refusalsAndNonValues() {
    Fixture fixture;
    const auto solid = fixture.add(document::kSolidSourceNodeType);
    const auto picture = fixture.read(solid);
    expect(picture.status == NodeOutputsStatus::Evaluated && !picture.outputs.empty() &&
               picture.outputs.front().kind == document::SocketValueKind::Image &&
               !picture.outputs.front().value && !picture.outputs.front().note.empty(),
           "a picture output is listed with no value and a note");

    const auto missingNode = fixture.read(NodeId::fromRaw(9999));
    expect(missingNode.status == NodeOutputsStatus::MissingNode &&
               missingNode.diagnostic.find("query kind=nodes") != std::string::npos,
           "an unknown node is refused, naming where nodes are listed: " + missingNode.diagnostic);
    const auto missingComposition = scripting::readNodeOutputs(
        fixture.session->operationContext(), document::CompositionId::fromRaw(9999),
        core::RationalTime::fromInteger(0), solid);
    expect(missingComposition.status == NodeOutputsStatus::MissingComposition,
           "an unknown composition is refused: " + missingComposition.diagnostic);
    const auto noSource = scripting::readNodeOutputs(scripting::OperationContext{},
                                                     fixture.composition,
                                                     core::RationalTime::fromInteger(0), solid);
    expect(noSource.status == NodeOutputsStatus::Unavailable && !noSource.diagnostic.empty(),
           "a client that cannot evaluate says so");
    std::stop_source stop;
    stop.request_stop();
    auto context = fixture.session->operationContext();
    context.stop = stop.get_token();
    const auto space = fixture.add(document::kCoordinateSpaceNodeType);
    const auto stopped = fixture.read(space, 0, context);
    expect(stopped.status == NodeOutputsStatus::Cancelled, "a stopped caller cancels the read");
}

// The Axis Tick's evaluated value operation in the plan (its operands carry the lowered values).
[[nodiscard]] std::optional<runtime::CompiledValueOperation> tickOperation(Fixture& fixture,
                                                                           const NodeId tick) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::SnapshotCompileRequest request{fixture.session->snapshot(), fixture.composition};
    request.valueReadouts.push_back(tick);
    const auto compiled = compiler.compile(request, runtime::CancellationToken{});
    if (!compiled.plan)
        return std::nullopt;
    for (const auto& operation : compiled.plan->valueOperations())
        if (operation.sourceNodeId == tick)
            return operation;
    return std::nullopt;
}

void placementRowsFollowTheSpace() {
    Fixture fixture;
    const auto space = fixture.add(document::kCoordinateSpaceNodeType);
    fixture.set(space, "origin", Vec2d{120.0, 300.0});
    fixture.set(space, "pixelsPerUnit", Vec2d{50.0, 25.0});
    const auto axes = fixture.add(document::kAxesNodeType);
    const auto tick = fixture.add(document::kAxisTickNodeType);
    fixture.set(tick, "index", std::int64_t{3});
    fixture.set(tick, "origin", Vec2d{10.0, 20.0});
    fixture.set(tick, "pixelsPerUnit", Vec2d{7.0, 9.0});

    // No space: every placement row acts, and the tick sits where its own origin and scale say.
    for (const auto node : {axes, tick})
        for (const auto* row : {"origin", "pixelsPerUnit"})
            expect(!fixture.inactive(node, row),
                   std::string(row) + " acts while nothing is connected to the space input");
    auto alone = fixture.read(tick);
    const auto* own = valueOf<Vec2d>(alone, "position");
    // min -5, step 1, index 3: the value -2, x = origin.x + value * pixels per unit; the label
    // sits Label Offset (0, 24 by default) from the tick.
    expect(own != nullptr && near(*own, {10.0 - 2.0 * 7.0, 20.0 + 24.0}),
           "without a space the tick is placed by its own origin and scale");

    fixture.connect(space, "space", axes, "space");
    fixture.connect(space, "space", tick, "space");
    for (const auto node : {axes, tick})
        for (const auto* row : {"origin", "pixelsPerUnit"})
            expect(fixture.inactive(node, row),
                   std::string(row) + " does nothing while a space is connected");
    expect(!fixture.inactive(tick, "labelOffset") && !fixture.inactive(tick, "min"),
           "the rows that share the placement's key or still act stay active");
    const auto inSpace = fixture.read(tick);
    const auto* placed = valueOf<Vec2d>(inSpace, "position");
    expect(placed != nullptr && near(*placed, {120.0 - 2.0 * 50.0, 300.0 + 24.0}),
           "with a space the tick is placed by the space");

    // An inactive value changes nothing in the plan, set or wired: neither is read.
    const auto reference = tickOperation(fixture, tick);
    fixture.set(tick, "origin", Vec2d{-400.0, 900.0});
    const auto moved = tickOperation(fixture, tick);
    expect(reference && moved && *reference == *moved,
           "changing an inactive origin leaves the plan's operation as it was");
    const auto readout = fixture.add(document::kSpaceReadoutNodeType);
    fixture.connect(space, "space", readout, "space");
    fixture.connect(readout, "scale", tick, "pixelsPerUnit");
    // The wire reaches the readout into the plan, so compare the placement operands themselves:
    // both are the definition's defaults, constants with no parameter and no value output behind.
    const auto wired = tickOperation(fixture, tick);
    const auto* utility =
        wired ? std::get_if<runtime::CompiledValueUtility>(&wired->kernel) : nullptr;
    const auto constantAt = [&](const std::size_t index) -> std::optional<Vec2d> {
        if (utility == nullptr || index >= utility->operands.size() ||
            utility->operands[index].id.isValid())
            return std::nullopt;
        const auto* value = std::get_if<runtime::CompiledValue>(&utility->operands[index].source);
        const auto* vector = value ? std::get_if<Vec2d>(value) : nullptr;
        return vector ? std::optional(*vector) : std::nullopt;
    };
    // Axis Tick's operands: min, max, step, index, yAxis, origin, pixelsPerUnit, ...
    const auto origin = constantAt(5);
    const auto perUnit = constantAt(6);
    expect(origin && near(*origin, {0.0, 0.0}) && perUnit && near(*perUnit, {100.0, 100.0}),
           "a value wired into an inactive input is not read: both lower to their defaults");
    const auto read = fixture.read(tick);
    const auto* position = valueOf<Vec2d>(read, "position");
    expect(read.status == NodeOutputsStatus::Evaluated && position != nullptr &&
               near(*position, {120.0 - 2.0 * 50.0, 300.0 + 24.0}),
           "neither the set origin nor the wired scale moves the tick");

    // Disconnected again: the rows act and the tick reads its own origin and the wired scale.
    fixture.disconnect(tick, "space");
    expect(!fixture.inactive(tick, "origin") && !fixture.inactive(tick, "pixelsPerUnit"),
           "disconnecting the space makes the placement rows act again");
    const auto back = fixture.read(tick);
    const auto* restored = valueOf<Vec2d>(back, "position");
    expect(restored != nullptr && near(*restored, {-400.0 - 2.0 * 50.0, 900.0 + 24.0}),
           "without the space the tick reads its own origin and its wired scale again");
}

} // namespace

int main() {
    spaceReadoutsAreValues();
    refusalsAndNonValues();
    placementRowsFollowTheSpace();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "node outputs: every check passed\n";
    return 0;
}
