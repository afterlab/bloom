// The document tools both hosts share: query, transact, history and events (moved from
// apps/bloom-mcp/tools.cpp and server.cpp).
#include "core_internal.hpp"

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/scripting/command_diagnostics.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace bloom::mcp {
namespace {
// #381: Interpret Footage, as bloom.asset.set-interpretation takes it.
[[nodiscard]] std::string ratioText(const std::uint64_t numerator,
                                    const std::uint64_t denominator) {
    return std::to_string(numerator) + "/" + std::to_string(denominator);
}
template <typename Out>
[[nodiscard]] auto* interpretationRecord(Out& out, const document::AssetRecord& asset) {
    const auto& value = asset.interpretation;
    auto* record = out.object();
    const auto rate = document::footageFrameRate(asset);
    out.set(record, "frameRate",
            out.text(value.frameRate
                         ? ratioText(value.frameRate->numerator(), value.frameRate->denominator())
                         : std::string{}));
    out.set(record, "playbackFrameRate",
            out.text(rate ? ratioText(rate->numerator(), rate->denominator()) : std::string{}));
    if (const auto start = document::footageStartTimecode(asset)) {
        auto* time = out.array();
        out.append(time, out.number(static_cast<std::uint64_t>(start->start.numerator())));
        out.append(time, out.number(static_cast<std::uint64_t>(start->start.denominator())));
        out.set(record, "startTime", time);
        out.set(record, "dropFrame", out.boolean(start->dropFrame));
    }
    out.set(record, "sourceTimecode", out.boolean(!value.startTimecode.has_value()));
    constexpr std::array<const char*, 4> alpha{"guess", "ignore", "straight", "premultiplied"};
    out.set(record, "alpha", out.text(alpha[static_cast<std::size_t>(value.alphaMode)]));
    out.set(record, "invertAlpha", out.boolean(value.invertAlpha));
    auto* matte = out.array();
    for (const auto component : value.matteColor)
        out.append(matte, out.real(component));
    out.set(record, "matteColor", matte);
    constexpr std::array<const char*, 4> fields{"file", "progressive", "upper", "lower"};
    out.set(record, "fieldOrder", out.text(fields[static_cast<std::size_t>(value.fieldOrder)]));
    out.set(record, "deinterlace", out.boolean(value.deinterlace));
    const auto aspect = document::footagePixelAspect(asset);
    out.set(record, "pixelAspect",
            out.text(value.pixelAspect ? ratioText(aspect.numerator(), aspect.denominator())
                                       : std::string("file")));
    const auto file = document::footageFilePixelAspect(asset);
    out.set(record, "filePixelAspect", out.text(ratioText(file.numerator(), file.denominator())));
    out.set(record, "loop", out.number(value.loopCount));
    constexpr std::array<const char*, 3> missing{"hold", "nothing", "skip"};
    out.set(record, "missingFrames",
            out.text(missing[static_cast<std::size_t>(value.missingFrames)]));
    if (value.stillDuration) {
        auto* still = out.array();
        out.append(still, out.number(static_cast<std::uint64_t>(value.stillDuration->numerator())));
        out.append(still,
                   out.number(static_cast<std::uint64_t>(value.stillDuration->denominator())));
        out.set(record, "stillDuration", still);
    }
    constexpr std::array<const char*, 4> legacy{"auto", "srgb", "linear", "raw"};
    out.set(record, "legacy", out.text(legacy[static_cast<std::size_t>(value.colorSpace)]));
    out.set(record, "inputColorSpaceId", out.text(value.inputColorSpaceId));
    return record;
}
constexpr std::array<std::string_view, 7> kContextSources{
    "", "project", "composition", "time", "layer", "selection", "layers"};
constexpr std::size_t kParameterPageSize = 64;
// Even a control-heavy string expands by at most 6x in JSON and 2x in the MCP text copy.
// 128 KiB therefore stays below the 4 MiB response cap with both representations present.
constexpr std::size_t kParameterStringChunkBytes = std::size_t{128} * 1024;

std::string parameterCursor(const std::uint64_t session, const std::uint64_t project,
                            const std::uint64_t composition, const std::uint64_t revision,
                            const std::size_t offset) {
    return "p1:" + std::to_string(session) + ':' + std::to_string(project) + ':' +
           std::to_string(composition) + ':' + std::to_string(revision) + ':' +
           std::to_string(offset);
}

std::size_t parseParameterCursor(const std::string_view cursor, const std::uint64_t session,
                                 const std::uint64_t project, const std::uint64_t composition,
                                 const std::uint64_t revision, const std::size_t count) {
    if (!cursor.starts_with("p1:"))
        throw InvalidInput("Malformed parameter cursor");
    std::array<std::uint64_t, 5> values{};
    auto remaining = cursor.substr(3);
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto separator = remaining.find(':');
        if ((index == values.size() - 1) != (separator == std::string_view::npos))
            throw InvalidInput("Malformed parameter cursor");
        const auto part = remaining.substr(0, separator);
        if (part.empty() || (part.size() > 1 && part.front() == '0'))
            throw InvalidInput("Malformed parameter cursor");
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), values[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size())
            throw InvalidInput("Malformed parameter cursor");
        if (index < values.size() - 1)
            remaining.remove_prefix(separator + 1);
    }
    if (values[0] != session || values[1] != project || values[2] != composition ||
        values[3] != revision)
        throw InvalidInput("Stale parameter cursor");
    if (values[4] >= count)
        throw InvalidInput("Parameter cursor is out of range");
    return static_cast<std::size_t>(values[4]);
}

document::Vec2d pathPoint(yyjson_val* item) {
    if (!yyjson_is_arr(item) || yyjson_arr_size(item) != 2 ||
        !yyjson_is_num(yyjson_arr_get(item, 0)) || !yyjson_is_num(yyjson_arr_get(item, 1)))
        throw InvalidInput("A path point is two finite numbers");
    const document::Vec2d point{yyjson_get_num(yyjson_arr_get(item, 0)),
                                yyjson_get_num(yyjson_arr_get(item, 1))};
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
        throw InvalidInput("A path point is two finite numbers");
    return point;
}

// ADR 0028: a path argument has the shape the parameter projection publishes: {"closed": bool,
// "anchors": [{"point": [x, y], "inHandle": [x, y] | null, "outHandle": [x, y] | null}]}.
document::PathValue pathValue(yyjson_val* item) {
    auto* closed = yyjson_obj_get(item, "closed");
    auto* anchors = yyjson_obj_get(item, "anchors");
    if (yyjson_obj_size(item) != 2 || !yyjson_is_bool(closed) || !yyjson_is_arr(anchors) ||
        yyjson_arr_size(anchors) > document::kMaximumPathAnchors)
        throw InvalidInput("A path is {closed, anchors}");
    document::PathValue path;
    path.closed = yyjson_get_bool(closed);
    yyjson_arr_iter rows = yyjson_arr_iter_with(anchors);
    while (auto* row = yyjson_arr_iter_next(&rows)) {
        if (!yyjson_is_obj(row))
            throw InvalidInput("A path anchor is an object");
        document::PathAnchor anchor;
        bool hasPoint = false;
        std::optional<document::PathAnchorMode> mode;
        yyjson_obj_iter fields = yyjson_obj_iter_with(row);
        while (auto* key = yyjson_obj_iter_next(&fields)) {
            const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
            auto* field = yyjson_obj_iter_get_val(key);
            if (name == "point") {
                anchor.point = pathPoint(field);
                hasPoint = true;
            } else if (name == "inHandle" || name == "outHandle") {
                if (!yyjson_is_null(field))
                    (name == "inHandle" ? anchor.inHandle : anchor.outHandle) = pathPoint(field);
            } else if (name == "mode") {
                mode = yyjson_is_str(field) ? document::pathAnchorModeFromName(std::string_view(
                                                  yyjson_get_str(field), yyjson_get_len(field)))
                                            : std::nullopt;
                if (!mode)
                    throw InvalidInput("A path anchor mode is corner, mirrored, aligned, free or "
                                       "auto");
            } else {
                throw InvalidInput("A path anchor has point, inHandle, outHandle and mode");
            }
        }
        if (!hasPoint)
            throw InvalidInput("A path anchor needs a point");
        anchor.mode = mode.value_or(document::inferPathAnchorMode(anchor));
        path.anchors.push_back(anchor);
    }
    return path;
}

// #541: a gradient argument has the shape the parameter projection publishes: {"stops":
// [{"position": p, "color": [r, g, b, a]}]}.
// #546: with an optional "interpolation", "colorMode" and "hueDirection" by name.
document::GradientValue gradientValue(yyjson_val* item) {
    auto* stops = yyjson_obj_get(item, "stops");
    document::GradientValue gradient;
    std::size_t members = 1;
    const auto named = [&](const char* key, auto parse, auto& out, const char* names) {
        auto* text = yyjson_obj_get(item, key);
        if (text == nullptr)
            return;
        ++members;
        const auto value = yyjson_is_str(text)
                               ? parse(std::string_view(yyjson_get_str(text), yyjson_get_len(text)))
                               : std::nullopt;
        if (!value)
            throw InvalidInput(std::string("A gradient's ") + key + " is " + names);
        out = *value;
    };
    named("interpolation", document::gradientInterpolationFromName, gradient.modes.interpolation,
          "\"constant\", \"linear\", \"ease\", \"cardinal\", \"b-spline\" or \"exponential\"");
    named("colorMode", document::gradientColorModeFromName, gradient.modes.colorMode,
          "\"rgb\", \"hsv\" or \"hsl\"");
    named("hueDirection", document::gradientHueDirectionFromName, gradient.modes.hueDirection,
          "\"near\", \"far\", \"clockwise\" or \"counter-clockwise\"");
    if (yyjson_obj_size(item) != members || !yyjson_is_arr(stops) ||
        yyjson_arr_size(stops) > document::kMaximumGradientStops)
        throw InvalidInput("A gradient is {stops, interpolation, colorMode, hueDirection}");
    yyjson_arr_iter rows = yyjson_arr_iter_with(stops);
    while (auto* row = yyjson_arr_iter_next(&rows)) {
        auto* position = yyjson_obj_get(row, "position");
        auto* color = yyjson_obj_get(row, "color");
        if (!yyjson_is_obj(row) || yyjson_obj_size(row) != 2 || !yyjson_is_num(position) ||
            !yyjson_is_arr(color) || yyjson_arr_size(color) != 4)
            throw InvalidInput("A gradient stop is {position, color: [r, g, b, a]}");
        std::array<double, 4> rgba{};
        for (std::size_t index = 0; index < 4; ++index) {
            auto* channel = yyjson_arr_get(color, index);
            if (!yyjson_is_num(channel))
                throw InvalidInput("A gradient stop colour is four numbers");
            rgba[index] = yyjson_get_num(channel);
        }
        gradient.stops.push_back(
            {yyjson_get_num(position), core::Color4d{rgba[0], rgba[1], rgba[2], rgba[3]}});
    }
    if (!gradient.isValid())
        throw InvalidInput("A gradient has 1 to 64 stops, positions in 0..1 and finite colours "
                           "with alpha in 0..1");
    return gradient;
}

// #574: {"name": "Shot 2"} (a layer) or {"layer": "Shot 2", "role": "opacity"} (a parameter), with
// an optional "composition". An object with any other member is a path or a gradient.
bool isReferenceObject(yyjson_val* item) {
    if (!yyjson_is_obj(item) || yyjson_obj_size(item) == 0)
        return false;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(item);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (name != "name" && name != "layer" && name != "role" && name != "composition")
            return false;
    }
    return true;
}

scripting::ObjectReference referenceValue(yyjson_val* item) {
    scripting::ObjectReference reference;
    const auto text = [](yyjson_val* field, const char* what) {
        if (!yyjson_is_str(field))
            throw InvalidInput(std::string("A reference's ") + what + " is text");
        return string(field, 1024);
    };
    if (auto* field = yyjson_obj_get(item, "name"))
        reference.name = text(field, "name");
    if (auto* field = yyjson_obj_get(item, "role"))
        reference.role = text(field, "role");
    if (auto* field = yyjson_obj_get(item, "layer")) {
        if (yyjson_is_str(field))
            reference.layerName = string(field, 1024);
        else if (yyjson_is_uint(field) && yyjson_get_uint(field) != 0)
            reference.layerId = yyjson_get_uint(field);
        else
            throw InvalidInput("A reference's layer is the layer's name (text) or its id");
    }
    if (auto* field = yyjson_obj_get(item, "composition")) {
        if (!yyjson_is_uint(field) || yyjson_get_uint(field) == 0)
            throw InvalidInput("A reference's composition is a composition id");
        reference.composition = yyjson_get_uint(field);
    }
    return reference;
}

scripting::Value value(yyjson_val* item, const std::size_t depth = 0) {
    if (depth > 16)
        throw InvalidInput("Argument exceeds nesting limit");
    if (yyjson_is_bool(item))
        return scripting::Value(yyjson_get_bool(item));
    if (yyjson_is_sint(item))
        return scripting::Value(yyjson_get_sint(item));
    if (yyjson_is_uint(item)) {
        const auto number = yyjson_get_uint(item);
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw InvalidInput("Integer exceeds signed 64-bit range");
        return scripting::Value(static_cast<std::int64_t>(number));
    }
    if (yyjson_is_real(item)) {
        const auto number = yyjson_get_real(item);
        if (!std::isfinite(number))
            throw InvalidInput("Number must be finite");
        return scripting::Value(number);
    }
    if (yyjson_is_str(item))
        return scripting::Value(string(item, kRequestLimit));
    if (yyjson_is_arr(item)) {
        scripting::ValueArray values;
        yyjson_arr_iter iterator = yyjson_arr_iter_with(item);
        while (auto* child = yyjson_arr_iter_next(&iterator))
            values.push_back(value(child, depth + 1));
        return scripting::Value(std::move(values));
    }
    if (isReferenceObject(item))
        return scripting::Value(referenceValue(item));
    if (yyjson_is_obj(item) && yyjson_obj_get(item, "stops") != nullptr)
        return scripting::Value(gradientValue(item));
    if (yyjson_is_obj(item))
        return scripting::Value(pathValue(item));
    throw InvalidInput("Operation arguments require scalars, arrays, paths or gradients");
}

bool matches(yyjson_val* value, const scripting::ValueKind kind) {
    using Kind = scripting::ValueKind;
    switch (kind) {
    case Kind::Boolean:
        return yyjson_is_bool(value);
    case Kind::Integer:
        return yyjson_is_int(value);
    case Kind::Double:
        return yyjson_is_num(value);
    case Kind::String:
        return yyjson_is_str(value);
    case Kind::Id:
        // A layer or parameter may be given by name and role instead (#574).
        if (isReferenceObject(value))
            return true;
        // An id is a positive integer or the same value as decimal text ("16").
        if (yyjson_is_str(value)) {
            const std::string_view text(yyjson_get_str(value), yyjson_get_len(value));
            std::uint64_t parsed = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), parsed);
            return !text.empty() && error == std::errc{} && end == text.data() + text.size() &&
                   parsed != 0;
        }
        return yyjson_is_uint(value) && yyjson_get_uint(value) != 0;
    case Kind::Array:
        return yyjson_is_arr(value);
    case Kind::Path:
        return yyjson_is_obj(value);
    case Kind::Time:
        return yyjson_is_int(value) || (yyjson_is_arr(value) && yyjson_arr_size(value) == 2 &&
                                        yyjson_is_int(yyjson_arr_get_first(value)));
    case Kind::Value:
        // A number, a vector or colour, true/false for a Boolean parameter, text for a String
        // parameter or an enumeration member named by its label ("screen"), or a path object for
        // a Path parameter.
        return yyjson_is_num(value) || yyjson_is_bool(value) || yyjson_is_str(value) ||
               yyjson_is_obj(value) ||
               (yyjson_is_arr(value) && yyjson_arr_size(value) >= 2 && yyjson_arr_size(value) <= 4);
    case Kind::Vec2:
    case Kind::Vec3:
    case Kind::Color4: {
        const std::size_t count = kind == Kind::Vec2 ? 2 : kind == Kind::Vec3 ? 3 : 4;
        if (!yyjson_is_arr(value) || yyjson_arr_size(value) != count)
            return false;
        yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
        while (auto* item = yyjson_arr_iter_next(&iterator)) {
            if (!yyjson_is_num(item))
                return false;
        }
        return true;
    }
    }
    return false;
}
} // namespace

yyjson_mut_val* commandResult(Json& out, const commands::CommandResult& result) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(result.succeeded()));
    out.set(value, "revision", out.number(result.afterRevision.value()));
    auto* diagnostics = out.array();
    for (const auto& diagnostic : scripting::commandDiagnostics(result)) {
        auto* detail = out.object();
        out.set(detail, "code", out.text(diagnostic.code));
        if (diagnostic.severity != "error")
            out.set(detail, "severity", out.text(diagnostic.severity));
        out.set(detail, "operationIndex",
                diagnostic.operationIndex ? out.number(*diagnostic.operationIndex) : out.null());
        if (!diagnostic.operation.empty())
            out.set(detail, "operation", out.text(diagnostic.operation));
        if (!diagnostic.path.empty())
            out.set(detail, "path", out.text(diagnostic.path));
        out.set(detail, "message", out.text(diagnostic.message));
        out.append(diagnostics, detail);
    }
    out.set(value, "diagnostics", diagnostics);
    auto* outputs = out.array();
    for (const auto& output : result.outputs) {
        auto* record = out.object();
        out.set(record, "operationIndex", out.number(output.operationIndex));
        out.set(record, "name", out.text(output.output.name));
        out.set(
            record, "id",
            out.number(std::visit([](const auto& id) { return id.value(); }, output.output.id)));
        out.append(outputs, record);
    }
    out.set(value, "outputs", outputs);
    return value;
}

namespace {

// The live host's refusal codes, as MCP tool errors.
void throwLiveRefusal(const scripting::SessionCommandResult& result) {
    if (!result.diagnostic)
        return;
    const auto& code = result.diagnostic->code;
    using scripting::LiveCommitStatus;
    const auto is = [&](const LiveCommitStatus status) {
        return code == scripting::liveCommitDiagnosticCode(status);
    };
    if (is(LiveCommitStatus::QueueFull))
        throw ToolFailure(McpToolErrorCode::CommitQueueFull,
                          "Bloom has too many edits waiting; nothing was queued.");
    if (is(LiveCommitStatus::ProjectReplaced))
        throw ToolFailure(McpToolErrorCode::ProjectReplaced,
                          "The project was replaced; nothing was applied.");
    if (is(LiveCommitStatus::Cancelled) || is(LiveCommitStatus::ShuttingDown))
        throw ToolFailure(McpToolErrorCode::Cancelled,
                          "The request was cancelled; nothing was applied.");
    if (is(LiveCommitStatus::DeadlineExceeded))
        throw ToolFailure(McpToolErrorCode::DeadlineExceeded,
                          "Bloom was busy past the request's deadline; nothing was applied.");
    if (is(LiveCommitStatus::GestureOpen))
        throw ToolFailure(McpToolErrorCode::GestureOpen,
                          "The artist kept an edit open past the deadline; nothing was applied.");
}

// A command the session could not run at all.
void throwUnavailable(const McpCall& call, const scripting::SessionCommandResult& result) {
    throwLiveRefusal(result);
    if (call.lease != nullptr && call.lease->revoked())
        throw ToolFailure(McpToolErrorCode::ProjectReplaced,
                          "The project was replaced; nothing was applied.");
    if (result.status == host::ProjectSessionCommandStatus::ReadOnly)
        throw ToolFailure(McpToolErrorCode::ReadOnly, "The project is read-only.");
    throw std::runtime_error("The session cannot edit");
}

// After a successful edit, every edited composition is compiled once (never rendered) so an edit
// that leaves one uncompilable is reported with the edit instead of surfacing later as a failed
// preview or export: compileFailures in the result, and a composition.compile-failed event. A
// composition that still compiles but degrades (a text layer drawn in the fallback face because its
// Font asset is gone) reports each warning in compileWarnings.
yyjson_mut_val* compileFailures(McpServerCore::State& core, Json& out, McpCall& call,
                                const commands::CommandResult& command,
                                yyjson_mut_val* warnings) {
    auto* failures = out.array();
    const auto subjectRecord = [&](const runtime::CompileSubject& subject) {
        auto* record = out.object();
        out.set(record, "composition", out.number(subject.compositionId.value()));
        out.set(record, "node", subject.nodeId ? out.number(subject.nodeId->value()) : out.null());
        out.set(record, "parameter",
                subject.parameterId ? out.number(subject.parameterId->value()) : out.null());
        return record;
    };
    const auto snapshot = call.session.snapshot();
    std::vector<document::CompositionId> edited;
    if (command.affectedComposition) {
        edited.push_back(*command.affectedComposition);
    } else {
        for (const auto& composition : snapshot.project().compositions())
            edited.push_back(composition.id());
    }
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    for (const auto composition : edited) {
        if (call.control.stop.stop_requested() ||
            snapshot.project().findComposition(composition) == nullptr)
            continue;
        const auto compiled =
            compiler.compile({.snapshot = snapshot, .compositionId = composition}, {});
        for (const auto& diagnostic : compiled.diagnostics) {
            if (diagnostic.severity != runtime::DiagnosticSeverity::Warning)
                continue;
            auto text = diagnostic.summary;
            if (!diagnostic.detail.empty())
                text += (text.empty() ? "" : ": ") + diagnostic.detail;
            auto* warning = out.object();
            out.set(warning, "composition", out.number(composition.value()));
            out.set(warning, "code", out.text(std::string(runtime::compileDiagnosticCodeId(
                                           diagnostic.code))));
            out.set(warning, "detail", out.text(text));
            out.set(warning, "subject", subjectRecord(diagnostic.subject));
            out.append(warnings, warning);
        }
        const auto failure = runtime::compileFailureSummary(compiled, composition);
        if (!failure)
            continue;
        auto* record = out.object();
        out.set(record, "composition", out.number(composition.value()));
        out.set(record, "code", out.text(failure->code));
        out.set(record, "detail", out.text(failure->detail));
        out.set(record, "subject",
                subjectRecord({.compositionId = failure->compositionId,
                               .nodeId = failure->nodeId,
                               .edgeId = std::nullopt,
                               .parameterId = failure->parameterId,
                               .layerId = std::nullopt,
                               .layerSlotId = std::nullopt,
                               .field = {}}));
        out.append(failures, record);
        call.compileFailures.push_back(
            {.composition = failure->compositionId.value(),
             .detail = failure->detail,
             .node = failure->nodeId ? std::optional(failure->nodeId->value()) : std::nullopt,
             .parameter = failure->parameterId ? std::optional(failure->parameterId->value())
                                               : std::nullopt});
        core.notice(
            {.kind = "composition.compile-failed",
             .message = failure->detail,
             .revision = command.afterRevision.value(),
             .composition = failure->compositionId.value(),
             .node = failure->nodeId ? std::optional(failure->nodeId->value()) : std::nullopt,
             .parameter =
                 failure->parameterId ? std::optional(failure->parameterId->value()) : std::nullopt,
             .path = {},
             .projectName = {}});
    }
    return failures;
}

} // namespace

yyjson_mut_val* transactTool(McpServerCore::State& core, Json& out, McpCall& call,
                             yyjson_val* arguments) {
    members(arguments, {"expectedRevision", "operations", "label"},
            {"expectedRevision", "operations"});
    const auto revision = integer(member(arguments, "expectedRevision"));
    const auto label = member(arguments, "label") ? string(member(arguments, "label"))
                       : call.live != nullptr     ? "Agent edit"
                                                  : "MCP transaction";
    auto* operations = member(arguments, "operations");
    if (!yyjson_is_arr(operations) || yyjson_arr_size(operations) == 0)
        throw InvalidInput("Expected operations");
    commands::Transaction transaction(label, document::Revision::fromRaw(revision));
    const auto& context = call.context;
    const auto& registry = call.session.operations();
    std::string operationIds;
    std::size_t operationIndex = 0;
    yyjson_arr_iter iterator = yyjson_arr_iter_with(operations);
    for (auto* operation = yyjson_arr_iter_next(&iterator); operation != nullptr;
         operation = yyjson_arr_iter_next(&iterator), ++operationIndex) {
        // Every argument refusal names the operation's place in the list, so an agent knows which
        // entry to fix.
        const auto at = "operations[" + std::to_string(operationIndex) + "]";
        if (!yyjson_is_obj(operation) || !member(operation, "op") || !member(operation, "args"))
            throw InvalidInput(at + ": each operation is {\"op\": <id>, \"args\": {...}}");
        members(operation, {"op", "args"}, {"op", "args"});
        const auto id = string(member(operation, "op"), 128);
        const auto* descriptor = registry.find(id);
        if (operationIds.size() < 200)
            operationIds += (operationIds.empty() ? "" : ",") + id;
        if (!descriptor)
            throw InvalidInput(at + ": " + unknownOperation(id));
        auto* args = member(operation, "args");
        if (!yyjson_is_obj(args))
            throw InvalidInput(at + " (" + id + "): args must be an object");
        scripting::Arguments values;
        yyjson_obj_iter argumentsIterator = yyjson_obj_iter_with(args);
        while (auto* key = yyjson_obj_iter_next(&argumentsIterator)) {
            const auto name = string(key, 128);
            const auto schema = std::ranges::find(descriptor->schema.arguments, name,
                                                  &scripting::ArgumentSchema::name);
            auto* item = yyjson_obj_iter_get_val(key);
            // null is the same as leaving the argument out, as None is in Python: an optional
            // argument takes its default (`"parent": null` clears a parent) and a required one is
            // reported missing.
            if (yyjson_is_null(item) && schema != descriptor->schema.arguments.end())
                continue;
            if (schema == descriptor->schema.arguments.end())
                throw InvalidInput(at + " (" + id + "): unknown argument \"" + name +
                                   "\"; the example is " +
                                   scripting::exampleCall(descriptor->schema) + argumentHelp(id));
            if (!matches(item, schema->kind))
                throw InvalidInput(at + " (" + id + "): argument \"" + name + "\" expects " +
                                   std::string(scripting::valueKindName(schema->kind)) +
                                   "; the example is " +
                                   scripting::exampleCall(descriptor->schema) + argumentHelp(id));
            // SCRIPT-0 converts these optional fields to uint32_t in its factory.
            if (id == "bloom.composition.add" &&
                (name == "frameRateNumerator" || name == "frameRateDenominator") &&
                (!yyjson_is_uint(item) || yyjson_get_uint(item) == 0 ||
                 yyjson_get_uint(item) > std::numeric_limits<std::uint32_t>::max()))
                throw InvalidInput(at + " (" + id +
                                   "): frame-rate components must be in 1..4294967295");
            values.emplace(name, schema->kind == scripting::ValueKind::Id &&
                                         !isReferenceObject(item)
                                     ? scripting::Value(scripting::StableId{identifier(item)})
                                     : value(item));
        }
        auto created = registry.create(id, std::move(values), context);
        if (!created) {
            auto* result = out.object();
            out.set(result, "succeeded", out.boolean(false));
            auto* diagnostics = out.array();
            auto* diagnostic = out.object();
            out.set(diagnostic, "code", out.text(created.diagnostic()->code));
            out.set(diagnostic, "operationIndex", out.number(operationIndex));
            out.set(diagnostic, "operation", out.text(created.diagnostic()->operationId));
            out.set(diagnostic, "argument", out.text(created.diagnostic()->argument));
            out.set(diagnostic, "message", out.text(created.diagnostic()->message));
            out.append(diagnostics, diagnostic);
            out.set(result, "diagnostics", diagnostics);
            return result;
        }
        if (!transaction.add(std::move(created).takeOperation()))
            throw InvalidInput(at + " (" + id + "): the operation could not be added");
    }
    call.summary = operationIds + " \"" + std::string(label.substr(0, 64)) + "\"";
    if (call.control.stop.stop_requested())
        throw std::runtime_error("Request cancelled");
    const auto result = call.session.execute(std::move(transaction));
    if (!result.command)
        throwUnavailable(call, result);
    auto* value = commandResult(out, *result.command);
    auto* warnings = out.array();
    out.set(value, "compileFailures",
            result.command->changed() && result.command->renderAffecting
                ? compileFailures(core, out, call, *result.command, warnings)
                : out.array());
    out.set(value, "compileWarnings", warnings);
    return value;
}

yyjson_mut_val* historyTool(McpServerCore::State&, Json& out, McpCall& call,
                            yyjson_val* arguments) {
    members(arguments, {"action", "expectedRevision"}, {"action", "expectedRevision"});
    const auto action = string(member(arguments, "action"), 8);
    if (action != "undo" && action != "redo")
        throw InvalidInput("History action is undo or redo");
    const bool redo = action == "redo";
    const auto expected =
        document::Revision::fromRaw(integer(member(arguments, "expectedRevision")));
    call.summary = action;
    scripting::SessionCommandResult result;
    if (call.lease != nullptr) {
        result = call.lease->history(redo, expected);
    } else if (call.session.snapshot().revision() != expected) {
        commands::CommandResult stale;
        stale.action = redo ? commands::CommandAction::Redo : commands::CommandAction::Undo;
        stale.status = commands::CommandStatus::StaleRevision;
        stale.beforeRevision = call.session.snapshot().revision();
        stale.afterRevision = stale.beforeRevision;
        stale.expectedRevision = expected;
        return commandResult(out, stale);
    } else {
        result = redo ? call.session.redo() : call.session.undo();
    }
    if (!result.command)
        throwUnavailable(call, result);
    return commandResult(out, *result.command);
}

yyjson_mut_val* eventsTool(McpServerCore::State& core, Json& out, McpCall& call,
                           yyjson_val* arguments) {
    members(arguments, {"sinceRevision", "afterSequence"}, {"sinceRevision"});
    const auto since = integer(member(arguments, "sinceRevision"));
    const auto after =
        member(arguments, "afterSequence") ? integer(member(arguments, "afterSequence")) : 0;
    const auto revision = call.session.snapshot().revision().value();
    const std::lock_guard lock(core.eventsMutex);
    // A cursor from before the live project was replaced must start again.
    const bool replaced = core.replaced && after < core.resetSequence;
    if (!replaced && (since > revision || after > core.sequence))
        throw InvalidInput("Event cursor is in the future");
    auto* result = out.object();
    out.set(result, "revision", out.number(revision));
    out.set(result, "sequence", out.number(core.sequence));
    const auto& events = core.events;
    const auto revisionOf = [](const McpEventEntry& entry) {
        if (const auto* command = std::get_if<commands::CommandEvent>(&entry))
            return command->result.afterRevision.value();
        return std::get<McpNotice>(entry).revision;
    };
    out.set(result, "resetRequired",
            out.boolean(replaced || (!events.empty() && events.front().first > 1 &&
                                     after < events.front().first - 1 &&
                                     since <= revisionOf(events.front().second))));
    auto* list = out.array();
    constexpr std::array<std::string_view, 3> names{"revision", "history", "rejected"};
    for (const auto& [sequence, entry] : events) {
        if ((!replaced && sequence <= after) || (!replaced && revisionOf(entry) < since))
            continue;
        yyjson_mut_val* value = nullptr;
        if (const auto* event = std::get_if<commands::CommandEvent>(&entry)) {
            value = commandResult(out, event->result);
            out.set(value, "kind", out.text(names.at(static_cast<std::size_t>(event->kind))));
        } else {
            const auto& notice = std::get<McpNotice>(entry);
            value = noticeValue(out, notice);
        }
        out.set(value, "sequence", out.number(sequence));
        out.append(list, value);
    }
    out.set(result, "events", list);
    return result;
}

namespace {
// query kind "context": the live editor context (or the headless session defaults).
yyjson_mut_val* contextProjection(Json& out, McpCall& call, const document::Snapshot& snapshot) {
    auto* result = out.object();
    out.set(result, "revision", out.number(snapshot.revision().value()));
    out.set(result, "live", out.boolean(call.live != nullptr));
    auto* project = out.object();
    out.set(project, "id", out.number(snapshot.project().id().value()));
    out.set(project, "name", out.text(snapshot.project().name()));
    out.set(result, "project", project);
    const auto& context = call.context;
    out.set(result, "composition",
            context.composition ? out.number(*context.composition) : out.null());
    auto* selection = out.array();
    for (const auto node : context.selection)
        out.append(selection, out.number(node));
    out.set(result, "selectedNodes", selection);
    auto* layers = out.array();
    for (const auto layer : context.layers)
        out.append(layers, out.number(layer));
    out.set(result, "selectedLayers", layers);
    const auto time = context.time.value_or(core::RationalTime::fromInteger(0));
    auto* timeValue = out.array();
    out.append(timeValue, out.signedNumber(time.numerator()));
    out.append(timeValue, out.signedNumber(time.denominator()));
    out.set(result, "time", timeValue);
    // The Viewer's display and view; empty means the project's default display and view.
    out.set(result, "display", out.text(call.live ? call.live->viewerDisplay : std::string{}));
    out.set(result, "view", out.text(call.live ? call.live->viewerView : std::string{}));
    out.set(result, "readOnly",
            out.boolean(call.live ? call.live->readOnly
                                  : call.session.state().contentKind ==
                                        host::ProjectSessionContentKind::PreservedReadOnly));
    return result;
}
} // namespace

yyjson_mut_val* queryTool(McpServerCore::State& core, Json& out, McpCall& call,
                          yyjson_val* arguments) {
    const auto sessionToken = core.cursorToken ^ (call.generation * 0x9e3779b97f4a7c15ULL);
    if (auto* requested = member(arguments, "kind");
        yyjson_is_str(requested) &&
        isReadbackQuery(std::string_view(yyjson_get_str(requested), yyjson_get_len(requested))))
        return readbackQuery(out, call, arguments, string(requested, 32), sessionToken);
    members(arguments,
            {"kind", "composition", "pageSize", "cursor", "parameter", "expectedRevision", "offset",
             "limit", "idPrefix", "layer", "fields"},
            {"kind"});
    const auto kind = string(member(arguments, "kind"), 32);
    if (member(arguments, "idPrefix") && kind != "operations")
        throw InvalidInput("idPrefix applies to the operations query");
    if (member(arguments, "layer") && kind != "nodes")
        throw InvalidInput("layer applies to the layers and nodes queries");
    const bool parameterPage =
        kind == "parameters" && (member(arguments, "pageSize") || member(arguments, "cursor"));
    const bool individualParameter = kind == "parameter" || kind == "parameter-value";
    if (member(arguments, "fields") &&
        (kind == "context" || kind == "project" || individualParameter))
        throw InvalidInput("fields applies to the queries that return records");
    const bool compositionRecords =
        kind == "nodes" || kind == "parameters" || kind == "layers" || kind == "keyframes";
    if (member(arguments, "composition") && !compositionRecords && !individualParameter)
        throw InvalidInput("query kind " + string(member(arguments, "kind"), 32) +
                           " does not take composition; help {\"name\":\"query\"} lists "
                           "each kind's arguments");
    if ((member(arguments, "pageSize") || member(arguments, "cursor")) && kind != "parameters")
        throw InvalidInput("pageSize and cursor apply only to parameters and keyframes");
    if (member(arguments, "parameter") != nullptr && !individualParameter)
        throw InvalidInput("A parameter ID requires an individual parameter query");
    if ((member(arguments, "expectedRevision") || member(arguments, "offset") ||
         member(arguments, "limit")) &&
        !individualParameter)
        throw InvalidInput("expectedRevision, offset and limit apply only to parameter and "
                           "parameter-value queries");
    const auto snapshot = call.session.snapshot();
    if (kind == "context") {
        if (member(arguments, "fields"))
            throw InvalidInput("fields applies to the queries that return records");
        return contextProjection(out, call, snapshot);
    }
    auto* result = out.object();
    out.set(result, "revision", out.number(snapshot.revision().value()));
    if (kind == "project") {
        auto* project = out.object();
        out.set(project, "id", out.number(snapshot.project().id().value()));
        out.set(project, "name", out.text(snapshot.project().name()));
        out.set(result, "project", project);
        return result;
    }
    if (individualParameter &&
        (!member(arguments, "composition") || !member(arguments, "parameter")))
        throw InvalidInput("Individual parameter queries require composition and parameter IDs");
    if (individualParameter && member(arguments, "expectedRevision") &&
        integer(member(arguments, "expectedRevision")) != snapshot.revision().value())
        throw InvalidInput("Stale parameter revision");
    if (kind == "parameter-value" && !member(arguments, "expectedRevision"))
        throw InvalidInput("String chunks require expectedRevision");
    if (kind == "parameter" && (member(arguments, "offset") || member(arguments, "limit")))
        throw InvalidInput("String chunk options require parameter-value");
    if (individualParameter) {
        const auto compositionId = integer(member(arguments, "composition"));
        const auto* composition =
            snapshot.project().findComposition(document::CompositionId::fromRaw(compositionId));
        if (!composition)
            throw InvalidInput("Stale composition ID at revision " +
                               std::to_string(snapshot.revision().value()));
        const auto parameterId = integer(member(arguments, "parameter"));
        const auto* parameter =
            composition->parameters().find(document::ParameterId::fromRaw(parameterId));
        if (!parameter)
            throw InvalidInput("Stale parameter ID at revision " +
                               std::to_string(snapshot.revision().value()));
        out.set(result, "composition", out.number(compositionId));
        out.set(result, "parameter", out.number(parameterId));
        if (kind == "parameter") {
            const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
            const auto* text = constant ? std::get_if<std::string>(&constant->value) : nullptr;
            out.set(result, "record",
                    text && text->size() > kParameterStringChunkBytes
                        ? parameterSummary(out, *parameter)
                        : parameterRecord(out, *parameter));
            out.set(result, "metadata", parameterMetadata(out, *composition, *parameter));
            return result;
        }
        const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
        const auto* text = constant ? std::get_if<std::string>(&constant->value) : nullptr;
        if (!text)
            throw InvalidInput("Parameter is not a constant string");
        const auto offset = member(arguments, "offset") ? integer(member(arguments, "offset")) : 0;
        const auto limit = member(arguments, "limit") ? integer(member(arguments, "limit"))
                                                      : kParameterStringChunkBytes;
        if (limit < 4 || limit > kParameterStringChunkBytes || offset > text->size() ||
            (offset < text->size() &&
             (static_cast<unsigned char>((*text)[offset]) & 0xc0U) == 0x80U))
            throw InvalidInput("Invalid UTF-8 string chunk range");
        auto end = static_cast<std::size_t>(offset) +
                   std::min<std::size_t>(static_cast<std::size_t>(limit), text->size() - offset);
        while (end < text->size() && (static_cast<unsigned char>((*text)[end]) & 0xc0U) == 0x80U)
            --end;
        out.set(result, "offset", out.number(offset));
        out.set(result, "nextOffset", out.number(end));
        out.set(result, "totalBytes", out.number(text->size()));
        out.set(result, "complete", out.boolean(end == text->size()));
        out.set(result, "chunk", out.text(std::string_view(*text).substr(offset, end - offset)));
        return result;
    }
    auto* records = out.array();
    if (kind == "operations") {
        const auto prefix = member(arguments, "idPrefix") ? string(member(arguments, "idPrefix"), 128)
                                                          : std::string{};
        for (const auto& descriptor : call.session.operations().descriptors()) {
            if (!descriptor.schema.typeId.starts_with(prefix))
                continue;
            auto* record = out.object();
            out.set(record, "id", out.text(descriptor.schema.typeId));
            out.set(record, "description", out.text(descriptor.schema.description));
            auto* schema = out.array();
            for (const auto& argument : descriptor.schema.arguments) {
                auto* item = out.object();
                out.set(item, "name", out.text(argument.name));
                out.set(item, "kind", out.text(scripting::valueKindName(argument.kind)));
                out.set(item, "required", out.boolean(argument.required));
                out.set(item, "contextual", out.boolean(argument.contextual()));
                out.set(item, "context",
                        out.text(kContextSources.at(static_cast<std::size_t>(argument.context))));
                out.set(item, "summary", out.text(argument.summary));
                out.set(item, "unit", out.text(argument.unit));
                out.set(item, "minimum",
                        argument.minimum ? out.real(*argument.minimum) : out.null());
                out.set(item, "maximum",
                        argument.maximum ? out.real(*argument.maximum) : out.null());
                out.set(item, "default", out.text(argument.defaultText));
                auto* choices = out.array();
                for (const auto& choice : argument.choices) {
                    auto* row = out.object();
                    const auto& stored = choice.value.storage;
                    const auto* label = std::get_if<std::string>(&stored);
                    const auto* whole = std::get_if<std::int64_t>(&stored);
                    out.set(row, "value",
                            label   ? out.text(*label)
                            : whole ? out.signedNumber(*whole)
                                    : out.null());
                    out.set(row, "label", out.text(choice.label));
                    out.append(choices, row);
                }
                out.set(item, "choices", choices);
                out.append(schema, item);
            }
            out.set(record, "arguments", schema);
            out.set(record, "example", out.text(scripting::exampleCall(descriptor.schema)));
            out.append(records, record);
        }
    } else if (kind == "compositions") {
        for (const auto& composition : snapshot.project().compositions())
            out.append(records, compositionRecord(out, composition));
    } else if (kind == "assets") {
        for (const auto& asset : snapshot.project().assets()) {
            auto* record = assetRecord(out, asset);
            if (asset.kind == document::AssetKind::Image ||
                asset.kind == document::AssetKind::Sequence ||
                asset.kind == document::AssetKind::Video)
                out.set(record, "interpretation", interpretationRecord(out, asset));
            out.append(records, record);
        }
        out.set(result, "folders", assetFolders(out, snapshot.project()));
    } else if (kind == "enums") {
        appendAnimationEnums(out, records);
    } else if (kind == "nodes" || kind == "parameters") {
        const auto id = integer(member(arguments, "composition"));
        const auto* composition =
            snapshot.project().findComposition(document::CompositionId::fromRaw(id));
        if (!composition)
            throw InvalidInput("Stale composition ID at revision " +
                               std::to_string(snapshot.revision().value()));
        if (kind == "nodes") {
            appendNodeGraph(out, result, records, *composition,
                            layerFilter(*composition, arguments));
        } else if (parameterPage) {
            // A summary only carries bounded structural strings (at most 256 bytes each),
            // never a value. Even worst-case JSON escaping and the MCP text copy fit 4 MiB.
            const auto pageSize = member(arguments, "pageSize")
                                      ? integer(member(arguments, "pageSize"))
                                      : kParameterPageSize;
            if (pageSize == 0 || pageSize > kParameterPageSize)
                throw InvalidInput("Parameter pageSize must be in 1..64");
            const auto parameters = composition->parameters().records();
            const auto offset =
                member(arguments, "cursor")
                    ? parseParameterCursor(string(member(arguments, "cursor"), 160), sessionToken,
                                           snapshot.project().id().value(), id,
                                           snapshot.revision().value(), parameters.size())
                    : 0;
            const auto end = offset + std::min<std::size_t>(pageSize, parameters.size() - offset);
            for (auto index = offset; index < end; ++index)
                out.append(records, parameterSummary(out, parameters[index]));
            out.set(result, "nextCursor",
                    end < parameters.size()
                        ? out.text(parameterCursor(sessionToken, snapshot.project().id().value(),
                                                   id, snapshot.revision().value(), end))
                        : out.null());
            out.set(result, "totalRecords", out.number(parameters.size()));
        } else {
            for (const auto& parameter : composition->parameters().records()) {
                out.append(records, parameterRecord(out, parameter));
            }
        }
    } else
        throw InvalidInput("Unknown query kind " + kind +
                           ". Kinds: project, compositions, layers, nodes, node-types, "
                           "parameters, parameter, parameter-value, keyframes, assets, "
                           "operations, context");
    out.set(result, "records", records);
    projectRecords(out, result, arguments);
    return result;
}
} // namespace bloom::mcp
