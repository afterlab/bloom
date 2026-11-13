#include "native.hpp"
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>

#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bloom::scripting::python {
namespace {

std::shared_ptr<Session> makeSession(const std::string& project) {
    const nb::gil_scoped_release release;
    auto result = project.empty() ? Session::createNew() : Session::open(project);
    if (!result) {
        throw std::runtime_error(result.diagnostic().message);
    }
    return std::move(result).takeSession();
}

nb::dict failure(const OperationDiagnostic& diagnostic) {
    nb::dict row;
    row["succeeded"] = false;
    nb::dict detail;
    detail["code"] = diagnostic.code;
    detail["operation_id"] = diagnostic.operationId;
    detail["argument"] = diagnostic.argument;
    detail["message"] = diagnostic.message;
    nb::list diagnostics;
    diagnostics.append(detail);
    row["diagnostics"] = diagnostics;
    return row;
}

constexpr std::array<const char*, 7> kContextSources{"",      "project",   "composition", "time",
                                                     "layer", "selection", "layers"};

} // namespace

NativeHost::NativeHost(const std::string& project, std::shared_ptr<PythonRootRetirements> roots)
    : NativeHost(makeSession(project), {}, std::move(roots)) {}

NativeHost::NativeHost(std::shared_ptr<Session> session,
                       std::shared_ptr<host::GpuExportProvider> provider,
                       std::shared_ptr<PythonRootRetirements> roots)
    : session_(std::move(session)), rootRetirements_(std::move(roots)),
      gpuExportProvider_(std::move(provider)) {
    if (!rootRetirements_)
        throw std::invalid_argument("Python hosts require the interpreter retirement registry");
    if (!session_ || !session_->isValid()) {
        throw std::invalid_argument("A live Bloom session is required");
    }
    ownsGpuExportProvider_ = gpuExportProvider_ == nullptr;
    if (ownsGpuExportProvider_) {
        gpuExportProvider_ = rootRetirements_->sharedProvider();
    }
    facade_ = std::make_unique<Facade>(*session_);
    subscription_ = facade_->events.subscribe([this](const commands::CommandEvent& event) {
        const std::lock_guard lock(eventMutex_);
        if (pendingEvents_.size() == 1024) {
            pendingEvents_.pop_front();
            eventsDropped_ = true;
        }
        pendingEvents_.push_back(event);
    });
    // This is the final potentially failing construction step. Every created scheduler is
    // registered before its threads start; failure cleanup/joins run outside the GIL.
    const nb::gil_scoped_release release;
    retirement_ = rootRetirements_->registerHost(gpuExportProvider_, ownsGpuExportProvider_);
}

NativeHost::~NativeHost() {
    // Allocation-free GC handoff precedes provider release; no native/host/scheduler join here.
    if (retirement_)
        retirement_->release(gpuExportProvider_);
}

std::unique_lock<std::mutex> NativeHost::lockSession() const {
    // A native render/save may hold this lock while the GIL is released. Waiting readers
    // must also release the GIL so progress and cancellation remain callable.
    const nb::gil_scoped_release release;
    return std::unique_lock(sessionMutex_);
}

nb::dict NativeHost::snapshot() const {
    const auto access = lockSession();
    return projectSnapshot(facade_->query.snapshot());
}

OperationContext NativeHost::operationContext() const {
    OperationContext context;
    if (contextProvider) {
        const auto live = contextProvider();
        context.project = live.project;
        if (live.composition != 0) {
            context.composition = live.composition;
        }
        context.selection = live.selection;
        context.time = live.time;
        if (context.composition.has_value()) {
            context.layers =
                selectedLayers(facade_->query.snapshot(), *context.composition, context.selection);
        }
        context.layerGeometry = session_->layerGeometryProvider();
        context.compositionFrameRate = session_->compositionFrameRateLookup();
        // Name references (#574) read the project the script edits.
        context.source = session_->operationSourceProvider();
    } else {
        context = session_->operationContext();
    }
    // ADR 0031: align and distribute evaluate one frame; other Python threads keep running.
    if (context.layerGeometry) {
        context.layerGeometry =
            [measure = std::move(context.layerGeometry)](const document::CompositionId composition,
                                                         const core::RationalTime time) {
                const nb::gil_scoped_release release;
                return measure(composition, time);
            };
    }
    return context;
}

nb::dict NativeHost::context() const {
    const auto access = lockSession();
    if (contextProvider) {
        const auto context = contextProvider();
        nb::dict result;
        result["project"] = context.project;
        result["composition"] = context.composition ? nb::cast(context.composition) : nb::none();
        nb::list selection;
        for (const auto id : context.selection)
            selection.append(id);
        result["selection"] = nb::tuple(selection);
        nb::list layers;
        for (const auto id :
             selectedLayers(facade_->query.snapshot(), context.composition, context.selection)) {
            layers.append(id);
        }
        result["layers"] = nb::tuple(layers);
        result["time"] = nb::make_tuple(context.time.numerator(), context.time.denominator());
        nb::list level;
        for (const auto id : context.level)
            level.append(id);
        result["level"] = nb::tuple(level);
        result["points"] = context.points ? nb::cast(*context.points) : nb::none();
        return result;
    }
    const auto snapshot = facade_->query.snapshot();
    nb::dict result;
    result["project"] = snapshot.project().id().value();
    result["composition"] = snapshot.project().compositions().empty()
                                ? nb::none()
                                : nb::cast(snapshot.project().compositions().front().id().value());
    result["selection"] = nb::tuple();
    result["layers"] = nb::tuple();
    result["time"] = nb::make_tuple(0, 1);
    result["level"] = nb::tuple();
    result["points"] = nb::none();
    return result;
}

nb::list NativeHost::schemas() const {
    nb::list result;
    for (const auto& descriptor : facade_->operations.descriptors()) {
        nb::dict row;
        row["id"] = descriptor.schema.typeId;
        row["description"] = descriptor.schema.description;
        nb::list arguments;
        for (const auto& argument : descriptor.schema.arguments) {
            nb::dict value;
            value["name"] = argument.name;
            value["kind"] = std::string(valueKindName(argument.kind));
            value["required"] = argument.required;
            value["contextual"] = argument.contextual();
            value["context"] = kContextSources.at(static_cast<std::size_t>(argument.context));
            value["summary"] = argument.summary;
            value["example"] = argument.example.has_value()
                                   ? nb::cast(pythonLiteral(*argument.example))
                                   : nb::none();
            value["unit"] = argument.unit;
            value["minimum"] = argument.minimum ? nb::cast(*argument.minimum) : nb::none();
            value["maximum"] = argument.maximum ? nb::cast(*argument.maximum) : nb::none();
            value["default"] = argument.defaultText;
            nb::list choices;
            for (const auto& choice : argument.choices) {
                const auto* text = std::get_if<std::string>(&choice.value.storage);
                const auto* whole = std::get_if<std::int64_t>(&choice.value.storage);
                choices.append(nb::make_tuple(text    ? nb::cast(*text)
                                              : whole ? nb::cast(*whole)
                                                      : nb::none(),
                                              choice.label));
            }
            value["choices"] = choices;
            arguments.append(value);
        }
        row["arguments"] = arguments;
        row["example"] = exampleCall(descriptor.schema);
        result.append(row);
    }
    return result;
}

document::Vec2d pathPoint(const nb::handle value) {
    if (!nb::isinstance<nb::tuple>(value) && !nb::isinstance<nb::list>(value)) {
        throw nb::type_error("A path point is (x, y)");
    }
    const auto sequence = nb::borrow<nb::sequence>(value);
    if (nb::len(sequence) != 2) {
        throw nb::type_error("A path point is (x, y)");
    }
    const document::Vec2d point{nb::cast<double>(sequence[0]), nb::cast<double>(sequence[1])};
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        throw nb::value_error("Path points must be finite");
    }
    return point;
}

// #541: a gradient argument is the mapping bloom.data projects for a gradient value: {"stops":
// [{"position": p, "color": (r, g, b, a)}]}.
// #546: with an optional 'interpolation', 'color_mode' and 'hue_direction' by name.
Value gradientValue(const nb::dict& mapping) {
    document::GradientValue gradient;
    std::size_t members = 1;
    const auto named = [&](const char* key, auto parse, auto& out, const char* names) {
        if (!mapping.contains(key))
            return;
        ++members;
        const nb::handle item = mapping[key];
        const auto value = nb::isinstance<nb::str>(item)
                               ? parse(std::string_view(nb::cast<std::string>(item)))
                               : std::nullopt;
        if (!value)
            throw nb::value_error((std::string("A gradient's ") + key + " is " + names).c_str());
        out = *value;
    };
    named("interpolation", document::gradientInterpolationFromName, gradient.modes.interpolation,
          "'constant', 'linear', 'ease', 'cardinal', 'b-spline' or 'exponential'");
    named("color_mode", document::gradientColorModeFromName, gradient.modes.colorMode,
          "'rgb', 'hsv' or 'hsl'");
    named("hue_direction", document::gradientHueDirectionFromName, gradient.modes.hueDirection,
          "'near', 'far', 'clockwise' or 'counter-clockwise'");
    if (nb::len(mapping) != members)
        throw nb::type_error(
            "A gradient is {'stops': [...], 'interpolation', 'color_mode', 'hue_direction'}");
    const auto stops = nb::borrow<nb::sequence>(mapping["stops"]);
    if (nb::len(stops) > document::kMaximumGradientStops)
        throw nb::value_error("A gradient has at most 64 stops");
    for (const auto item : stops) {
        if (!nb::isinstance<nb::dict>(item))
            throw nb::type_error("A gradient stop is a mapping with a position and a color");
        const auto row = nb::borrow<nb::dict>(item);
        if (nb::len(row) != 2 || !row.contains("position") || !row.contains("color"))
            throw nb::type_error("A gradient stop is {'position': p, 'color': (r, g, b, a)}");
        const auto color = nb::borrow<nb::sequence>(row["color"]);
        if (nb::len(color) != 4)
            throw nb::type_error("A gradient stop colour is four numbers");
        gradient.stops.push_back(
            {nb::cast<double>(row["position"]),
             core::Color4d{nb::cast<double>(color[0]), nb::cast<double>(color[1]),
                           nb::cast<double>(color[2]), nb::cast<double>(color[3])}});
    }
    if (!gradient.isValid())
        throw nb::value_error("A gradient has 1 to 64 stops, positions in 0..1 and finite colours "
                              "with alpha in 0..1");
    return Value(std::move(gradient));
}

// ADR 0028: a path argument is the mapping bloom.data projects for a path value: {"closed": bool,
// "anchors": [{"point": (x, y), "in_handle": (x, y) or None, "out_handle": (x, y) or None}]}.
document::PathContour pathContour(const nb::dict& mapping) {
    if (nb::len(mapping) != 2 || !mapping.contains("closed") || !mapping.contains("anchors") ||
        !nb::isinstance<nb::bool_>(mapping["closed"])) {
        throw nb::type_error("A path contour is {'closed': bool, 'anchors': [...]}");
    }
    const auto anchors = nb::borrow<nb::sequence>(mapping["anchors"]);
    if (nb::len(anchors) > document::kMaximumPathAnchors) {
        throw nb::value_error("A path has at most 4096 anchors");
    }
    document::PathContour path;
    path.closed = nb::cast<bool>(mapping["closed"]);
    for (const auto item : anchors) {
        if (!nb::isinstance<nb::dict>(item)) {
            throw nb::type_error("A path anchor is a mapping with a point");
        }
        const auto row = nb::borrow<nb::dict>(item);
        if (!row.contains("point")) {
            throw nb::type_error("A path anchor needs a point");
        }
        document::PathAnchor anchor;
        anchor.point = pathPoint(row["point"]);
        std::optional<document::PathAnchorMode> mode;
        for (const auto [key, handle] : row) {
            const auto name = nb::cast<std::string>(key);
            if (name == "point") {
                continue;
            }
            if (name == "mode") {
                mode = nb::isinstance<nb::str>(handle)
                           ? document::pathAnchorModeFromName(nb::cast<std::string>(handle))
                           : std::nullopt;
                if (!mode) {
                    throw nb::value_error("A path anchor mode is corner, mirrored, aligned, free "
                                          "or auto");
                }
                continue;
            }
            const bool incoming = name == "in_handle" || name == "inHandle";
            if (!incoming && name != "out_handle" && name != "outHandle") {
                throw nb::type_error("A path anchor has point, in_handle, out_handle and mode");
            }
            if (!handle.is_none()) {
                (incoming ? anchor.inHandle : anchor.outHandle) = pathPoint(handle);
            }
        }
        anchor.mode = mode.value_or(document::inferPathAnchorMode(anchor));
        path.anchors.push_back(anchor);
    }
    return path;
}

// #589: one contour, or {'contours': [contour...], 'fill_rule': 'nonZero' or 'evenOdd' (optional),
// 'groups': [{'key': text, 'first_contour': n, 'contour_count': n, 'color': (r, g, b, a)
// (optional)}] (optional)}.
Value pathValue(const nb::dict& mapping) {
    if (!mapping.contains("contours"))
        return Value(document::PathValue::fromContour(pathContour(mapping)));
    if (nb::len(mapping) != 1U + static_cast<std::size_t>(mapping.contains("fill_rule")) +
                                static_cast<std::size_t>(mapping.contains("groups")))
        throw nb::type_error("A compound path is {'contours': [...], 'fill_rule': text, "
                             "'groups': [...]}");
    document::PathValue path;
    for (const auto item : nb::borrow<nb::sequence>(mapping["contours"])) {
        if (!nb::isinstance<nb::dict>(item))
            throw nb::type_error("A path contour is {'closed': bool, 'anchors': [...]}");
        path.contours.push_back(pathContour(nb::borrow<nb::dict>(item)));
    }
    if (mapping.contains("fill_rule")) {
        const auto rule =
            nb::isinstance<nb::str>(mapping["fill_rule"])
                ? document::pathFillRuleFromName(nb::cast<std::string>(mapping["fill_rule"]))
                : std::nullopt;
        if (!rule)
            throw nb::value_error("A path fill rule is inherit, nonZero or evenOdd");
        path.fillRule = *rule;
    }
    if (mapping.contains("groups")) {
        for (const auto item : nb::borrow<nb::sequence>(mapping["groups"])) {
            if (!nb::isinstance<nb::dict>(item))
                throw nb::type_error("A path group is {'key': text, 'first_contour': n, "
                                     "'contour_count': n, 'color': (r, g, b, a)}");
            const auto row = nb::borrow<nb::dict>(item);
            if (!row.contains("key") || !row.contains("first_contour") ||
                !row.contains("contour_count") || nb::len(row) != (row.contains("color") ? 4U : 3U))
                throw nb::type_error("A path group is {'key': text, 'first_contour': n, "
                                     "'contour_count': n, 'color': (r, g, b, a)}");
            document::PathGroup group;
            group.key = nb::cast<std::string>(row["key"]);
            group.firstContour = nb::cast<std::uint32_t>(row["first_contour"]);
            group.contourCount = nb::cast<std::uint32_t>(row["contour_count"]);
            if (row.contains("color")) {
                const auto color = nb::borrow<nb::sequence>(row["color"]);
                if (nb::len(color) != 4)
                    throw nb::type_error("A path group color is four numbers");
                group.color = core::Color4d{nb::cast<double>(color[0]), nb::cast<double>(color[1]),
                                            nb::cast<double>(color[2]), nb::cast<double>(color[3])};
            }
            path.groups.push_back(std::move(group));
        }
    }
    if (!path.isValid())
        throw nb::value_error("A compound path holds at most 65536 contours and 262144 anchors, "
                              "and its groups have unique keys and ascending, unshared contours");
    return Value(std::move(path));
}

// #574: {'name': 'Shot 2'} (a layer) or {'layer': 'Shot 2' or an id, 'role': 'opacity'} (a
// parameter), with an optional 'composition'. Any other member makes the mapping a path or a
// gradient.
bool isReferenceMapping(const nb::dict& mapping) {
    if (nb::len(mapping) == 0)
        return false;
    for (const auto [key, unused] : mapping) {
        static_cast<void>(unused);
        if (!nb::isinstance<nb::str>(key))
            return false;
        const auto name = nb::cast<std::string>(key);
        if (name != "name" && name != "layer" && name != "role" && name != "composition")
            return false;
    }
    return true;
}

Value referenceValue(const nb::dict& mapping) {
    ObjectReference reference;
    const auto text = [&](const char* key) -> std::optional<std::string> {
        if (!mapping.contains(key))
            return std::nullopt;
        if (!nb::isinstance<nb::str>(mapping[key]))
            throw nb::type_error((std::string("A reference's ") + key + " is text").c_str());
        return nb::cast<std::string>(mapping[key]);
    };
    reference.name = text("name");
    reference.role = text("role");
    if (mapping.contains("layer")) {
        const auto layer = mapping["layer"];
        if (nb::isinstance<nb::str>(layer)) {
            reference.layerName = nb::cast<std::string>(layer);
        } else if (nb::isinstance<nb::int_>(layer) && !nb::isinstance<nb::bool_>(layer) &&
                   nb::cast<std::int64_t>(layer) > 0) {
            reference.layerId = static_cast<std::uint64_t>(nb::cast<std::int64_t>(layer));
        } else {
            throw nb::type_error("A reference's layer is the layer's name (text) or its id");
        }
    }
    if (mapping.contains("composition")) {
        const auto composition = mapping["composition"];
        if (!nb::isinstance<nb::int_>(composition) || nb::cast<std::int64_t>(composition) <= 0)
            throw nb::type_error("A reference's composition is a composition id");
        reference.composition = static_cast<std::uint64_t>(nb::cast<std::int64_t>(composition));
    }
    return Value(std::move(reference));
}

Value scriptValue(const nb::handle value, const std::size_t depth) {
    if (depth > 16) {
        throw nb::value_error("Argument nesting exceeds 16 levels");
    }
    if (nb::isinstance<nb::bool_>(value)) {
        return Value(nb::cast<bool>(value));
    }
    if (nb::isinstance<nb::int_>(value)) {
        return Value(nb::cast<std::int64_t>(value));
    }
    if (nb::isinstance<nb::float_>(value)) {
        const auto number = nb::cast<double>(value);
        if (!std::isfinite(number)) {
            throw nb::value_error("Arguments must be finite");
        }
        return Value(number);
    }
    if (nb::isinstance<nb::str>(value)) {
        auto text = nb::cast<std::string>(value);
        if (text.size() > std::size_t{1024} * 1024U) {
            throw nb::value_error("Argument string exceeds 1 MiB");
        }
        return Value(std::move(text));
    }
    if (nb::isinstance<nb::tuple>(value) || nb::isinstance<nb::list>(value)) {
        const auto sequence = nb::borrow<nb::sequence>(value);
        if (nb::len(sequence) > 4096) {
            throw nb::value_error("Argument array exceeds 4096 entries");
        }
        ValueArray array;
        for (const auto item : sequence) {
            array.push_back(scriptValue(item, depth + 1));
        }
        return Value(std::move(array));
    }
    if (nb::isinstance<nb::dict>(value)) {
        const auto mapping = nb::borrow<nb::dict>(value);
        if (isReferenceMapping(mapping))
            return referenceValue(mapping);
        if (mapping.contains("stops"))
            return gradientValue(mapping);
        return pathValue(mapping);
    }
    throw nb::type_error("Expected a scalar, an array, a path or a gradient of Bloom argument values");
}

namespace {

// #442: after an edit that changed the project, every edited composition is compiled once (never
// rendered) so a composition that still compiles but degrades, such as a text layer drawn in the
// fallback face because its Font asset is gone, tells the caller, as MCP transact does in
// compileWarnings. Each row has composition, code, detail and subject (composition, node and
// parameter, None when the warning is not about one).
nb::list compileWarnings(const SessionRenderSourceSnapshot& source,
                         const commands::CommandResult& command) {
    const auto& snapshot = source.snapshot;
    nb::list warnings;
    if (!command.changed())
        return warnings;
    std::vector<document::CompositionId> edited;
    if (command.affectedComposition) {
        edited.push_back(*command.affectedComposition);
    } else {
        for (const auto& composition : snapshot.project().compositions())
            edited.push_back(composition.id());
    }
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    for (const auto composition : edited) {
        if (snapshot.project().findComposition(composition) == nullptr)
            continue;
        const auto compiled = [&] {
            const nb::gil_scoped_release release;
            return compiler.compile({.snapshot = snapshot, .compositionId = composition}, {});
        }();
        for (const auto& diagnostic : compiled.diagnostics) {
            if (diagnostic.severity != runtime::DiagnosticSeverity::Warning)
                continue;
            auto text = diagnostic.summary;
            if (!diagnostic.detail.empty())
                text += (text.empty() ? "" : ": ") + diagnostic.detail;
            nb::dict row;
            row["composition"] = composition.value();
            row["code"] = std::string(runtime::compileDiagnosticCodeId(diagnostic.code));
            row["detail"] = text;
            nb::dict subject;
            subject["composition"] = diagnostic.subject.compositionId.value();
            subject["node"] = diagnostic.subject.nodeId ? nb::cast(diagnostic.subject.nodeId->value())
                                                        : nb::none();
            subject["parameter"] = diagnostic.subject.parameterId
                                       ? nb::cast(diagnostic.subject.parameterId->value())
                                       : nb::none();
            row["subject"] = subject;
            warnings.append(row);
        }
        // #610: a stored color space the project's color configuration lacks.
        for (const auto& missing : missingEffectColorSpaces(
                 source, *snapshot.project().findComposition(composition))) {
            nb::dict row;
            row["composition"] = composition.value();
            row["code"] = std::string(kMissingColorSpaceWarningCode);
            row["detail"] = missing.detail;
            nb::dict subject;
            subject["composition"] = composition.value();
            subject["node"] = missing.node.value();
            subject["parameter"] = missing.parameter.value();
            row["subject"] = subject;
            warnings.append(row);
        }
    }
    return warnings;
}

} // namespace

nb::dict NativeHost::transact(const nb::list& operations, const std::string& label,
                              const std::uint64_t revision) {
    if (std::this_thread::get_id() != owner_) {
        throw std::runtime_error("Transactions must run on the host authoring thread");
    }
    if (operations.empty() || operations.size() > 4096 || label.size() > 4096) {
        throw nb::value_error("A transaction needs 1..4096 operations and a bounded label");
    }
    const auto access = lockSession();
    const auto context = operationContext();
    commands::Transaction transaction(label, document::Revision::fromRaw(revision));
    for (const auto item : operations) {
        const auto request = nb::cast<nb::dict>(item);
        const auto id = nb::cast<std::string>(request["op"]);
        const auto args = nb::cast<nb::dict>(request["args"]);
        const auto* descriptor = facade_->operations.find(id);
        if (descriptor == nullptr) {
            return failure({"bloom.scripting.unknown-operation", id, "", "Unknown operation ID"});
        }
        Arguments arguments;
        for (const auto [key, value] : args) {
            const auto name = nb::cast<std::string>(key);
            const auto schema =
                std::ranges::find(descriptor->schema.arguments, name, &ArgumentSchema::name);
            if (schema == descriptor->schema.arguments.end()) {
                return failure({"bloom.scripting.invalid-argument", id, name, "Unknown argument"});
            }
            auto converted = scriptValue(value);
            // SCRIPT-0 converts these optional fields to uint32_t in its factory.
            if (id == "bloom.composition.add" &&
                (name == "frameRateNumerator" || name == "frameRateDenominator")) {
                const auto* component = std::get_if<std::int64_t>(&converted.storage);
                if (!component || *component <= 0 ||
                    static_cast<std::uint64_t>(*component) >
                        std::numeric_limits<std::uint32_t>::max()) {
                    return failure({"bloom.scripting.invalid-argument", id, name,
                                    "Frame-rate components must be in 1..4294967295"});
                }
            }
            arguments.emplace(name, std::move(converted));
        }
        auto created = facade_->operations.create(id, std::move(arguments), context);
        if (!created) {
            return failure(*created.diagnostic());
        }
        if (!transaction.add(std::move(created).takeOperation())) {
            throw nb::value_error("The transaction rejected an operation");
        }
    }
    const auto result = [&] {
        const nb::gil_scoped_release release;
        return session_->execute(std::move(transaction));
    }();
    if (!result.command) {
        // A live commit the marshal withdrew names why (bloom.live.*, ADR 0034).
        return failure(result.diagnostic
                           ? *result.diagnostic
                           : OperationDiagnostic{"bloom.scripting.session-unavailable", "", "",
                                                 "Session cannot edit"});
    }
    auto row = commandResult(*result.command);
    row["compile_warnings"] = compileWarnings(session_->renderSourceSnapshot(), *result.command);
    return row;
}

nb::dict NativeHost::history(const bool redo) {
    if (std::this_thread::get_id() != owner_) {
        throw std::runtime_error("History must run on the host authoring thread");
    }
    const auto access = lockSession();
    const auto result = [&] {
        const nb::gil_scoped_release release;
        return redo ? session_->redo() : session_->undo();
    }();
    if (!result.command) {
        // A live commit the marshal withdrew names why (bloom.live.*, ADR 0034).
        return failure(result.diagnostic
                           ? *result.diagnostic
                           : OperationDiagnostic{"bloom.scripting.session-unavailable", "", "",
                                                 "Session cannot edit"});
    }
    return commandResult(*result.command);
}

void NativeHost::save(const std::string& path) {
    if (std::this_thread::get_id() != owner_) {
        throw std::runtime_error("Saving must start on the host authoring thread");
    }
    const auto access = lockSession();
    const nb::gil_scoped_release release;
    const auto result = session_->saveAs(path);
    if (!result.succeeded()) {
        throw std::runtime_error(result.message);
    }
}

nb::dict NativeHost::setViewerLevel(std::vector<std::uint64_t> level,
                                   const std::optional<std::uint64_t> points) {
    if (std::this_thread::get_id() != owner_)
        throw std::runtime_error("The Viewer's level changes on the host authoring thread");
    const auto access = lockSession();
    SessionViewerLevelResult result;
    {
        const nb::gil_scoped_release release;
        result = session_->setViewerLevel(std::move(level), points);
    }
    if (!result.succeeded())
        throw std::runtime_error(result.code + ": " + result.message);
    nb::dict row;
    row["code"] = result.code;
    row["message"] = result.message;
    return row;
}

nb::dict NativeHost::setColorSettings(const std::string& config,
                                      std::optional<std::string> workingSpace,
                                      std::optional<std::string> file, const bool projectRelative,
                                      std::optional<std::map<std::string, std::string>> context) {
    if (std::this_thread::get_id() != owner_) {
        throw std::runtime_error("Color settings change on the host authoring thread");
    }
    const auto access = lockSession();
    SessionColorSettingsResult result;
    {
        const nb::gil_scoped_release release;
        std::optional<host::ProjectColorConfigFile> chosen;
        if (file) {
            chosen.emplace();
            chosen->path = std::move(*file);
            chosen->projectRelative = projectRelative;
            if (context)
                for (auto& [name, value] : *context)
                    chosen->contextVariables.push_back({name, std::move(value)});
        }
        result = session_->setColorSettings(config, std::move(workingSpace), chosen);
    }
    if (!result.succeeded()) {
        throw std::runtime_error(result.code + ": " + result.message);
    }
    nb::dict row;
    row["code"] = result.code;
    if (result.settings) {
        row["config"] =
            std::string(host::projectColorConfigName(host::projectColorConfigOf(*result.settings)));
        row["working_space"] = result.settings->processColorSpaceId;
        row["working_space_label"] =
            std::string(host::workingColorSpaceLabel(result.settings->processColorSpaceId));
        // What the configuration is, to read it back.
        const auto description = host::describeProjectColorConfig(*result.settings);
        row["config_kind"] = description.kind;
        row["config_location"] = description.location;
        row["config_revision"] = description.revision;
        nb::dict variables;
        for (const auto& variable : description.contextVariables)
            variables[nb::str(variable.name.c_str())] = variable.value;
        row["context"] = variables;
    }
    return row;
}

nb::list NativeHost::events() {
    std::deque<commands::CommandEvent> pending;
    bool dropped = false;
    {
        const std::lock_guard lock(eventMutex_);
        pending.swap(pendingEvents_);
        dropped = std::exchange(eventsDropped_, false);
    }
    nb::list result;
    if (dropped) {
        nb::dict row;
        row["kind"] = "reset";
        result.append(row);
    }
    constexpr std::array<const char*, 3> names{"revision", "history", "rejected"};
    for (const auto& event : pending) {
        auto row = commandResult(event.result);
        row["kind"] = names.at(static_cast<std::size_t>(event.kind));
        result.append(row);
    }
    return result;
}

nb::list NativeHost::tasks() const {
    auto call = retirement_->acquire();
    nb::list result;
    for (const auto& task : call.scheduler().snapshots()) {
        nb::dict row;
        row["id"] = task.id.value();
        row["name"] = task.name;
        row["state"] = static_cast<int>(task.state);
        row["completed"] = task.progress.completed;
        row["total"] = task.progress.total ? nb::cast(*task.progress.total) : nb::none();
        row["phase"] = task.progress.phase;
        row["cancellation_requested"] = task.cancellationRequested;
        result.append(row);
    }
    return result;
}

bool NativeHost::cancel(const std::uint64_t id) {
    auto call = retirement_->acquire();
    return call.scheduler().cancel(runtime::TaskId::fromRaw(id));
}

void NativeHost::requestCancellation() {
    ++cancellationEpoch_;
    cancelled_.store(true);
    auto call = retirement_->tryAcquire();
    if (!call)
        return; // Closing already cancels the scheduler; never dereference a collected handle.
    for (const auto& task : call->scheduler().snapshots()) {
        if (!runtime::isTerminal(task.state))
            static_cast<void>(call->scheduler().cancel(task.id));
    }
}

} // namespace bloom::scripting::python
