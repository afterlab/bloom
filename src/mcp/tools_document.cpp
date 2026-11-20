// The document tools both hosts share: query, transact, history and events (moved from
// apps/bloom-mcp/tools.cpp and server.cpp).
#include "core_internal.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/scripting/command_diagnostics.hpp>
#include <bloom/scripting/operation_reference.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/render.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <span>

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
    constexpr std::array<const char*, 3> copies{"automatic", "always", "never"};
    out.set(record, "localCopies", out.text(copies[static_cast<std::size_t>(value.localCopies)]));
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

// ADR 0028: one contour has the shape the parameter projection publishes: {"closed": bool,
// "anchors": [{"point": [x, y], "inHandle": [x, y] | null, "outHandle": [x, y] | null}]}.
document::PathContour pathContour(yyjson_val* item) {
    auto* closed = yyjson_obj_get(item, "closed");
    auto* anchors = yyjson_obj_get(item, "anchors");
    if (yyjson_obj_size(item) != 2 || !yyjson_is_bool(closed) || !yyjson_is_arr(anchors) ||
        yyjson_arr_size(anchors) > document::kMaximumPathAnchors)
        throw InvalidInput("A path contour is {closed, anchors}");
    document::PathContour path;
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

// #589: one group of a compound path: {"key": text, "firstContour": n, "contourCount": n, "color":
// [r, g, b, a] (optional)}.
document::PathGroup pathGroup(yyjson_val* item) {
    auto* key = yyjson_obj_get(item, "key");
    auto* first = yyjson_obj_get(item, "firstContour");
    auto* count = yyjson_obj_get(item, "contourCount");
    auto* color = yyjson_obj_get(item, "color");
    if (yyjson_obj_size(item) != (color != nullptr ? 4U : 3U) || !yyjson_is_str(key) ||
        !yyjson_is_uint(first) || !yyjson_is_uint(count) ||
        yyjson_get_uint(first) > std::numeric_limits<std::uint32_t>::max() ||
        yyjson_get_uint(count) > std::numeric_limits<std::uint32_t>::max())
        throw InvalidInput("A path group is {key, firstContour, contourCount, color}");
    document::PathGroup group;
    group.key.assign(yyjson_get_str(key), yyjson_get_len(key));
    group.firstContour = static_cast<std::uint32_t>(yyjson_get_uint(first));
    group.contourCount = static_cast<std::uint32_t>(yyjson_get_uint(count));
    if (color != nullptr) {
        if (!yyjson_is_arr(color) || yyjson_arr_size(color) != 4)
            throw InvalidInput("A path group color is four numbers");
        const auto channel = [&](const std::size_t index) {
            auto* value = yyjson_arr_get(color, index);
            if (!yyjson_is_num(value))
                throw InvalidInput("A path group color is four numbers");
            return yyjson_get_num(value);
        };
        group.color = core::Color4d{channel(0), channel(1), channel(2), channel(3)};
    }
    return group;
}

// #589: a path argument is one contour, or {"contours": [contour...], "fillRule": "nonZero" or
// "evenOdd" (optional), "groups": [group...] (optional)}.
document::PathValue pathValue(yyjson_val* item) {
    auto* contours = yyjson_obj_get(item, "contours");
    if (contours == nullptr)
        return document::PathValue::fromContour(pathContour(item));
    auto* fillRule = yyjson_obj_get(item, "fillRule");
    auto* groups = yyjson_obj_get(item, "groups");
    if (yyjson_obj_size(item) != 1U + static_cast<std::size_t>(fillRule != nullptr) +
                                     static_cast<std::size_t>(groups != nullptr) ||
        !yyjson_is_arr(contours) || yyjson_arr_size(contours) > document::kMaximumPathContours ||
        (groups != nullptr && !yyjson_is_arr(groups)))
        throw InvalidInput("A compound path is {contours, fillRule, groups}");
    document::PathValue path;
    yyjson_arr_iter rows = yyjson_arr_iter_with(contours);
    while (auto* row = yyjson_arr_iter_next(&rows))
        path.contours.push_back(pathContour(row));
    if (fillRule != nullptr) {
        const auto rule = yyjson_is_str(fillRule)
                              ? document::pathFillRuleFromName(std::string_view(
                                    yyjson_get_str(fillRule), yyjson_get_len(fillRule)))
                              : std::nullopt;
        if (!rule)
            throw InvalidInput("A path fill rule is inherit, nonZero or evenOdd");
        path.fillRule = *rule;
    }
    if (groups != nullptr) {
        yyjson_arr_iter members = yyjson_arr_iter_with(groups);
        while (auto* member = yyjson_arr_iter_next(&members))
            path.groups.push_back(pathGroup(member));
    }
    if (!path.isValid())
        throw InvalidInput("A compound path holds at most 65536 contours and 262144 anchors, and "
                           "its groups have unique keys and ascending, unshared contours");
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

// #627: a number list argument has the shape the parameter projection publishes:
// {"values": [n...]}.
document::NumberListValue numberListValue(yyjson_val* item) {
    auto* values = yyjson_obj_get(item, "values");
    if (yyjson_obj_size(item) != 1 || !yyjson_is_arr(values) ||
        yyjson_arr_size(values) > document::kMaximumNumberListItems)
        throw InvalidInput("A number list is {values: [numbers]}, at most 65536 of them");
    document::NumberListValue list;
    list.values.reserve(yyjson_arr_size(values));
    yyjson_arr_iter items = yyjson_arr_iter_with(values);
    while (auto* number = yyjson_arr_iter_next(&items)) {
        if (!yyjson_is_num(number) || !std::isfinite(yyjson_get_num(number)))
            throw InvalidInput("A number list holds finite numbers");
        list.values.push_back(yyjson_get_num(number));
    }
    return list;
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

scripting::Value value(yyjson_val* item, std::size_t depth = 0);

// #747 (D3): a key track of bloom.animation.set-key-tracks: an object with "parameter" and "times"
// (and "values", optional "interpolations" and "replace").
bool isKeyTrackObject(yyjson_val* item) {
    return yyjson_is_obj(item) && yyjson_obj_get(item, "parameter") != nullptr &&
           yyjson_obj_get(item, "times") != nullptr;
}

scripting::KeyTrack keyTrackValue(yyjson_val* item, const std::size_t depth) {
    constexpr std::string_view shape =
        "A key track is {\"parameter\": id or {\"layer\", \"role\"}, \"times\": [...], "
        "\"values\": [...], \"interpolations\": [...] (optional), \"replace\": true|false "
        "(optional)}";
    scripting::KeyTrack track;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(item);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (name != "parameter" && name != "times" && name != "values" &&
            name != "interpolations" && name != "replace")
            throw InvalidInput(std::string(shape) + "; \"" + std::string(name) +
                               "\" is not one of its members");
    }
    const auto column = [&](const char* name) {
        auto* field = yyjson_obj_get(item, name);
        if (!yyjson_is_arr(field))
            throw InvalidInput(std::string(shape) + "; \"" + name + "\" must be a list");
        auto converted = value(field, depth + 1);
        return std::get<scripting::ValueArray>(std::move(converted.storage));
    };
    auto* parameter = yyjson_obj_get(item, "parameter");
    if (isReferenceObject(parameter))
        track.parameter = referenceValue(parameter);
    else if (yyjson_is_uint(parameter) && yyjson_get_uint(parameter) != 0)
        track.parameter = scripting::StableId{yyjson_get_uint(parameter)};
    else
        throw InvalidInput(std::string(shape) + "; \"parameter\" is a parameter id");
    track.times = column("times");
    if (yyjson_obj_get(item, "values") == nullptr)
        throw InvalidInput(std::string(shape) + "; \"values\" is missing");
    track.values = column("values");
    if (auto* modes = yyjson_obj_get(item, "interpolations"); modes != nullptr && !yyjson_is_null(modes))
        track.interpolations = column("interpolations");
    if (auto* replace = yyjson_obj_get(item, "replace"); replace != nullptr && !yyjson_is_null(replace)) {
        if (!yyjson_is_bool(replace))
            throw InvalidInput(std::string(shape) + "; \"replace\" is true or false");
        track.replace = yyjson_get_bool(replace);
    }
    return track;
}

scripting::Value value(yyjson_val* item, const std::size_t depth) {
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
    if (isKeyTrackObject(item))
        return scripting::Value(keyTrackValue(item, depth));
    if (isReferenceObject(item))
        return scripting::Value(referenceValue(item));
    if (yyjson_is_obj(item) && yyjson_obj_get(item, "stops") != nullptr)
        return scripting::Value(gradientValue(item));
    if (yyjson_is_obj(item) && yyjson_obj_get(item, "values") != nullptr)
        return scripting::Value(numberListValue(item));
    if (yyjson_is_obj(item))
        return scripting::Value(pathValue(item));
    throw InvalidInput(
        "Operation arguments require scalars, arrays, paths, gradients or number lists");
}

// An argument written {"fromFile": "<absolute .json path>"} is read from that file by the server,
// so a large column of numbers never passes through the conversation. The file must be a regular
// JSON file; its size and value count are bounded, and nothing but the parsed JSON is used.
struct DataFiles final {
    struct Release {
        void operator()(yyjson_doc* document) const noexcept { yyjson_doc_free(document); }
    };
    std::vector<std::unique_ptr<yyjson_doc, Release>> documents;
};

[[nodiscard]] bool isFromFile(yyjson_val* item) {
    return yyjson_is_obj(item) && yyjson_obj_size(item) == 1 &&
           yyjson_obj_get(item, "fromFile") != nullptr;
}

// #745: the refusal of a data-file bound, naming it, its value and the actual count.
[[noreturn]] void refuseDataFile(const McpLimit limit, const std::uint64_t value,
                                 const std::uint64_t actual, const std::string& message) {
    throw LimitExceeded(limit, value, actual,
                        message + " (limit " + std::string(describeLimit(limit).name) +
                            "). Split the data over several calls or files; "
                            "bloom.animation.set-key-columns takes one column per component, the "
                            "fewest values per key.");
}

[[nodiscard]] yyjson_val* readDataFile(yyjson_val* reference, DataFiles& held,
                                       const std::string& at) {
    if (held.documents.size() == kMcpDataFilesPerCall)
        refuseDataFile(McpLimit::DataFiles, kMcpDataFilesPerCall, kMcpDataFilesPerCall + 1,
                       at + ": this would be data file " + grouped(kMcpDataFilesPerCall + 1) +
                           " of the call; a call reads at most " + grouped(kMcpDataFilesPerCall));
    auto* path = yyjson_obj_get(reference, "fromFile");
    if (!yyjson_is_str(path))
        throw InvalidInput(at + ": fromFile is the absolute path of a .json file");
    const std::filesystem::path file(string(path, 4096));
    std::error_code error;
    if (!file.is_absolute() || file.extension() != ".json")
        throw InvalidInput(at + ": fromFile is the absolute path of a .json file");
    if (!std::filesystem::is_regular_file(file, error))
        throw InvalidInput(at + ": fromFile is not a regular file Bloom can read");
    const auto size = std::filesystem::file_size(file, error);
    if (error || size == 0)
        throw InvalidInput(at + ": the data file is empty or cannot be measured");
    if (size > kMcpDataFileBytes)
        refuseDataFile(McpLimit::DataFileBytes, kMcpDataFileBytes, size,
                       at + ": the data file is " + grouped(size) +
                           " bytes; a data file holds at most " + grouped(kMcpDataFileBytes));
    std::string bytes(static_cast<std::size_t>(size), '\0');
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            throw InvalidInput(at + ": the data file could not be read");
    }
    std::unique_ptr<yyjson_doc, DataFiles::Release> document(
        yyjson_read(bytes.data(), bytes.size(), 0));
    if (!document || yyjson_doc_get_root(document.get()) == nullptr)
        throw InvalidInput(at + ": the data file is not valid JSON");
    auto* root = yyjson_doc_get_root(document.get());
    // Counted as the request is: the root and every array item and member value.
    std::size_t total = 0;
    std::size_t deepest = 0;
    std::vector<std::pair<yyjson_val*, std::size_t>> pending{{root, 0}};
    while (!pending.empty()) {
        const auto [item, depth] = pending.back();
        pending.pop_back();
        ++total;
        deepest = std::max(deepest, depth);
        if (yyjson_is_arr(item)) {
            yyjson_arr_iter iterator = yyjson_arr_iter_with(item);
            while (auto* child = yyjson_arr_iter_next(&iterator))
                pending.emplace_back(child, depth + 1);
        } else if (yyjson_is_obj(item)) {
            yyjson_obj_iter iterator = yyjson_obj_iter_with(item);
            while (auto* key = yyjson_obj_iter_next(&iterator))
                pending.emplace_back(yyjson_obj_iter_get_val(key), depth + 1);
        }
    }
    if (total > kMcpDataFileValues)
        refuseDataFile(McpLimit::DataFileValues, kMcpDataFileValues, total,
                       at + ": the data file holds " + grouped(total) +
                           " JSON values; a data file holds at most " +
                           grouped(kMcpDataFileValues));
    if (deepest > kMcpDataFileDepth)
        refuseDataFile(McpLimit::DataFileDepth, kMcpDataFileDepth, deepest,
                       at + ": the data file nests " + grouped(deepest) +
                           " levels deep; a data file nests at most " + grouped(kMcpDataFileDepth));
    held.documents.push_back(std::move(document));
    return root;
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

yyjson_mut_val* commandResult(Json& out, const commands::CommandResult& result,
                              const bool withOutputs, const std::size_t diagnosticLimit) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(result.succeeded()));
    out.set(value, "revision", out.number(result.afterRevision.value()));
    out.set(value, "origin", out.text(commands::commandOriginName(result.origin)));
    auto* diagnostics = out.array();
    const auto all = scripting::commandDiagnostics(result);
    // #745: a result answers with a bounded list; the rest are counted.
    if (all.size() > diagnosticLimit)
        out.set(value, "diagnosticsOmitted", out.number(all.size() - diagnosticLimit));
    for (std::size_t index = 0; index < std::min(all.size(), diagnosticLimit); ++index) {
        const auto& diagnostic = all[index];
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
    if (!withOutputs)
        return value;
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

// The live host's refusal codes, as MCP tool errors. #745: a bound names its value and the actual
// count (`work`: the transaction's work units; `waited`: how long the commit waited).
void throwLiveRefusal(const scripting::SessionCommandResult& result, const std::size_t work,
                      const std::chrono::milliseconds waited,
                      const std::optional<std::uint64_t> retryAfterMs) {
    if (!result.diagnostic)
        return;
    const auto& code = result.diagnostic->code;
    using scripting::LiveCommitStatus;
    const auto is = [&](const LiveCommitStatus status) {
        return code == scripting::liveCommitDiagnosticCode(status);
    };
    const auto bounded = [](const McpToolErrorCode error, const std::string& message,
                            const McpLimit limit, const std::uint64_t value,
                            const std::uint64_t actual) {
        ToolFailure failure(error, message);
        failure.limit = ToolFailure::Limit{.limit = limit, .value = value, .actual = actual};
        return failure;
    };
    if (is(LiveCommitStatus::QueueFull)) {
        const auto capacity = scripting::kLiveCommitQueueCapacity;
        auto failure = bounded(
            McpToolErrorCode::CommitQueueFull,
            "Bloom has " + grouped(capacity) + " edits waiting and this would make " +
                grouped(capacity + 1) + "; at most " + grouped(capacity) +
                " wait at once, shared with the Script editor (limit commitQueue). Nothing was "
                "queued; send it again after retryAfterMs, and batch edits into one transact "
                "(one bloom.animation.set-key-tracks operation with fromFile data writes many "
                "parameters' keys).",
            McpLimit::CommitQueue, capacity, capacity + 1);
        failure.retryAfterMs = retryAfterMs;
        throw failure;
    }
    if (is(LiveCommitStatus::ProjectReplaced))
        throw ToolFailure(McpToolErrorCode::ProjectReplaced,
                          "The project was replaced; nothing was applied.");
    if (is(LiveCommitStatus::Cancelled) || is(LiveCommitStatus::ShuttingDown))
        throw ToolFailure(McpToolErrorCode::Cancelled,
                          "The request was cancelled; nothing was applied.");
    const auto deadline = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(kMcpCommitDeadline).count());
    const auto waitedMs = static_cast<std::uint64_t>(std::max<std::int64_t>(waited.count(), 0));
    if (is(LiveCommitStatus::DeadlineExceeded))
        throw bounded(McpToolErrorCode::DeadlineExceeded,
                      "Bloom was busy past the request's deadline: the edit waited " +
                          grouped(waitedMs) + " ms and a commit may wait at most " +
                          grouped(deadline) +
                          " ms (limit commitDeadlineMs). Nothing was applied; split a large "
                          "edit into several transactions (bloom.animation.set-key-columns with "
                          "fromFile columns for keys).",
                      McpLimit::CommitDeadline, deadline, waitedMs);
    if (is(LiveCommitStatus::GestureOpen))
        throw bounded(McpToolErrorCode::GestureOpen,
                      "The artist kept an edit open past the deadline: the edit waited " +
                          grouped(waitedMs) + " ms of at most " + grouped(deadline) +
                          " ms (limit commitDeadlineMs). Nothing was applied.",
                      McpLimit::CommitDeadline, deadline, waitedMs);
    if (is(LiveCommitStatus::TooLarge))
        throw bounded(McpToolErrorCode::CommitTooLarge,
                      "The transaction carries " + grouped(work) +
                          " units of work (one per operation, or one per key, keyframe or edit "
                          "an operation lists); one commit takes at most " +
                          grouped(scripting::kLiveCommitWorkLimit) +
                          " (limit commitWork). Split it into several transactions, each a "
                          "bloom.animation.set-key-columns operation per parameter with fromFile "
                          "columns. Nothing was queued.",
                      McpLimit::CommitWork, scripting::kLiveCommitWorkLimit, work);
}

// #748: the session refused an edit that would take the project past what one project file can
// save and reopen.
[[noreturn]] void throwProjectTooLarge(const commands::CommandResult& result) {
    const auto& refusal = result.capacityRefusal;
    ToolFailure failure(McpToolErrorCode::ProjectTooLarge,
                        refusal ? refusal->message
                                : std::string("The edit would make the project too large to save; "
                                              "nothing was applied."));
    if (refusal) {
        for (std::size_t index = 0; index < kMcpLimitCount; ++index) {
            const auto limit = static_cast<McpLimit>(index);
            if (describeLimit(limit).name == refusal->limit)
                failure.limit = ToolFailure::Limit{
                    .limit = limit, .value = refusal->value, .actual = refusal->actual};
        }
    }
    throw failure;
}

// A command the session could not run at all.
void throwUnavailable(const McpCall& call, const scripting::SessionCommandResult& result,
                      const std::size_t work = 0,
                      const std::chrono::milliseconds waited = std::chrono::milliseconds{0},
                      const std::optional<std::uint64_t> retryAfterMs = std::nullopt) {
    throwLiveRefusal(result, work, waited, retryAfterMs);
    if (call.lease != nullptr && call.lease->revoked())
        throw ToolFailure(McpToolErrorCode::ProjectReplaced,
                          "The project was replaced; nothing was applied.");
    if (result.status == host::ProjectSessionCommandStatus::ReadOnly)
        throw ToolFailure(McpToolErrorCode::ReadOnly, "The project is read-only.");
    throw std::runtime_error("The session cannot edit");
}

// After a successful edit, every composition whose evaluation inputs it changed is compiled once
// (never rendered) so an edit that leaves one uncompilable is reported with the edit instead of
// surfacing later as a failed preview or export: compileFailures in the result, and a
// composition.compile-failed event. A composition that still compiles but degrades (a text layer
// drawn in the fallback face because its Font asset is gone) reports each warning in
// compileWarnings. #745: the scope is the commit's own evaluation-input proof
// (CommandResult::evaluationChangedCompositions); only a change beyond named compositions (an
// asset, for example) compiles every composition.
struct CompileRecord final {
    std::uint64_t composition = 0;
    std::string code;
    std::string detail;
    runtime::CompileSubject subject;
};

// At most this many records of each list in one answer; the rest are counted.
constexpr std::size_t kCompileRecordLimit = 256;

struct CompileReport final {
    bool checked = false;
    std::vector<CompileRecord> failures;
    std::vector<CompileRecord> warnings;
    std::size_t warningsOmitted = 0;
};

// #747: a long transact's progress, as notifications/progress (a request with a progress token)
// or an asynchronous job's state. Building is one unit per operation, preparing one per operation
// applied, then publishing and the compile check one each. Reported at most every 100 ms, except
// when the phase changes.
class TransactProgress final {
  public:
    TransactProgress(const McpRequestControl& control, const std::size_t operations)
        : report_(control.progress), operations_(operations) {}

    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(report_); }
    void building(const std::size_t built) {
        tell("building", static_cast<double>(built),
             "Building operation " + grouped(built) + " of " + grouped(operations_), false);
    }
    void waiting(const std::size_t ahead) {
        tell("waiting", static_cast<double>(operations_),
             ahead == 0 ? std::string("Waiting for its turn")
                        : "Waiting behind " + grouped(ahead) + (ahead == 1 ? " edit" : " edits"),
             true);
    }
    void preparing(const std::size_t applied, const std::size_t total) {
        tell("preparing", static_cast<double>(operations_ + applied),
             "Applying operation " + grouped(applied) + " of " + grouped(total), false);
    }
    void prepared() {
        tell("prepared", static_cast<double>(2 * operations_),
             "Waiting for Bloom to publish the edit", true);
    }
    void publishing() {
        tell("publishing", static_cast<double>(2 * operations_), "Publishing the edit", true);
    }
    void checking() {
        tell("checking", static_cast<double>(2 * operations_ + 1), "Checking compositions", true);
    }

  private:
    void tell(const std::string_view phase, const double completed, std::string message,
              const bool always) {
        if (!report_)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (!always && phase == phase_ && now - last_ < std::chrono::milliseconds(100))
            return;
        phase_ = phase;
        last_ = now;
        report_({.progress = completed,
                 .total = static_cast<double>(2 * operations_ + 2),
                 .message = std::move(message),
                 .phase = std::string(phase)});
    }

    std::function<void(const McpProgress&)> report_;
    std::size_t operations_ = 0;
    std::string_view phase_;
    std::chrono::steady_clock::time_point last_{};
};

CompileReport compileCheck(McpServerCore::State& core, McpCall& call,
                           const commands::CommandResult& command) {
    CompileReport report;
    report.checked = true;
    if (!command.changed() || !command.renderAffecting)
        return report;
    const auto source = call.session.renderSourceSnapshot();
    const auto& snapshot = source.snapshot;
    std::vector<document::CompositionId> edited;
    if (command.evaluationChangedCompositions) {
        edited = *command.evaluationChangedCompositions;
    } else if (command.affectedComposition) {
        edited.push_back(*command.affectedComposition);
    } else {
        for (const auto& composition : snapshot.project().compositions())
            edited.push_back(composition.id());
    }
    const auto warn = [&](CompileRecord record) {
        if (report.warnings.size() < kCompileRecordLimit)
            report.warnings.push_back(std::move(record));
        else
            ++report.warningsOmitted;
    };
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
            warn({.composition = composition.value(),
                  .code = std::string(runtime::compileDiagnosticCodeId(diagnostic.code)),
                  .detail = std::move(text),
                  .subject = diagnostic.subject});
        }
        // #610: a stored color space the project's color configuration lacks is a warning of the
        // edit that stored it (the node leaves the image unchanged).
        if (const auto* edit = snapshot.project().findComposition(composition); edit != nullptr)
            for (const auto& missing : scripting::missingEffectColorSpaces(source, *edit)) {
                runtime::CompileSubject subject;
                subject.compositionId = composition;
                subject.nodeId = missing.node;
                subject.parameterId = missing.parameter;
                warn({.composition = composition.value(),
                      .code = std::string(scripting::kMissingColorSpaceWarningCode),
                      .detail = missing.detail,
                      .subject = subject});
            }
        const auto failure = runtime::compileFailureSummary(compiled, composition);
        if (!failure)
            continue;
        runtime::CompileSubject subject;
        subject.compositionId = failure->compositionId;
        subject.nodeId = failure->nodeId;
        subject.parameterId = failure->parameterId;
        report.failures.push_back({.composition = composition.value(),
                                   .code = failure->code,
                                   .detail = failure->detail,
                                   .subject = subject});
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
    return report;
}

// `limit` records at most; the omitted ones are counted in `<name>Omitted`.
void setCompileRecords(Json& out, yyjson_mut_val* value, const std::string_view name,
                       const std::vector<CompileRecord>& records, const std::size_t limit,
                       const std::size_t alreadyOmitted) {
    auto* list = out.array();
    const auto shown = std::min(records.size(), limit);
    for (std::size_t index = 0; index < shown; ++index) {
        const auto& source = records[index];
        auto* record = out.object();
        out.set(record, "composition", out.number(source.composition));
        out.set(record, "code", out.text(source.code));
        out.set(record, "detail", out.text(source.detail));
        auto* subject = out.object();
        out.set(subject, "composition", out.number(source.subject.compositionId.value()));
        out.set(subject, "node",
                source.subject.nodeId ? out.number(source.subject.nodeId->value()) : out.null());
        out.set(subject, "parameter",
                source.subject.parameterId ? out.number(source.subject.parameterId->value())
                                           : out.null());
        out.set(record, "subject", subject);
        out.append(list, record);
    }
    out.set(value, name, list);
    if (const auto omitted = records.size() - shown + alreadyOmitted; omitted != 0)
        out.set(value, std::string(name) + "Omitted", out.number(omitted));
}

// response "summary": the created ids replaced by how many there are per output name and one
// SHA-256 digest over the full list (operation index, name and id of each, in order), so a large
// edit answers in a few lines and the full list can still be compared.
void summarizeOutputs(Json& out, yyjson_mut_val* value, const commands::CommandResult& result) {
    core::Sha256Hasher hasher;
    std::map<std::string, std::uint64_t> counts;
    for (const auto& output : result.outputs) {
        ++counts[output.output.name];
        const auto line =
            std::to_string(output.operationIndex) + ":" + output.output.name + ":" +
            std::to_string(std::visit([](const auto& id) { return id.value(); }, output.output.id)) +
            "\n";
        static_cast<void>(hasher.update(std::as_bytes(std::span(line.data(), line.size()))));
    }
    const auto hex = hasher.finalize().toLowercaseHex();
    yyjson_mut_obj_remove_str(value, "outputs");
    auto* summary = out.object();
    out.set(summary, "count", out.number(result.outputs.size()));
    auto* byName = out.object();
    for (const auto& [name, count] : counts)
        out.set(byName, name, out.number(count));
    out.set(summary, "byName", byName);
    out.set(summary, "digest", out.text("sha256:" + std::string(hex.data(), hex.size())));
    out.set(value, "outputSummary", summary);
}

// One transact answer: the command result, its ids (listed or summarized) and the compile check.
yyjson_mut_val* transactValue(Json& out, const commands::CommandResult& command,
                              const CompileReport& report, const bool listOutputs,
                              const std::size_t diagnosticLimit, const std::size_t compileLimit) {
    auto* value = commandResult(out, command, listOutputs, diagnosticLimit);
    if (!listOutputs)
        summarizeOutputs(out, value, command);
    out.set(value, "compileChecked", out.boolean(report.checked));
    if (report.checked) {
        setCompileRecords(out, value, "compileFailures", report.failures, compileLimit, 0);
        setCompileRecords(out, value, "compileWarnings", report.warnings, compileLimit,
                          report.warningsOmitted);
    } else {
        out.set(value, "compileFailures", out.null());
        out.set(value, "compileWarnings", out.null());
    }
    return value;
}
} // namespace

yyjson_mut_val* runTransact(McpServerCore::State& core, Json& out, McpCall& call,
                            yyjson_val* arguments) {
    members(arguments,
            {"expectedRevision", "operations", "label", "response", "compileCheck", "async",
             "idempotencyKey"},
            {"expectedRevision", "operations"});
    // #745: false skips the compile check (bulk key writes that cannot break a composition).
    const bool check =
        member(arguments, "compileCheck") ? boolean(member(arguments, "compileCheck")) : true;
    const auto revision = integer(member(arguments, "expectedRevision"));
    // "full" lists every created id; "summary" counts them by name and gives one digest.
    const auto response = member(arguments, "response") ? string(member(arguments, "response"), 16)
                                                         : std::string("full");
    if (response != "full" && response != "summary")
        throw InvalidInput("response is \"full\" or \"summary\"");
    const auto label = member(arguments, "label") ? string(member(arguments, "label"))
                       : call.live != nullptr     ? "Agent edit"
                                                  : "MCP transaction";
    auto* operations = member(arguments, "operations");
    if (!yyjson_is_arr(operations) || yyjson_arr_size(operations) == 0)
        throw InvalidInput("Expected operations");
    commands::Transaction transaction(label, document::Revision::fromRaw(revision));
    transaction.setOrigin(commands::CommandOrigin::Agent);
    const auto& context = call.context;
    const auto& registry = call.session.operations();
    std::string operationIds;
    std::size_t operationIndex = 0;
    DataFiles dataFiles;
    TransactProgress progress(call.control, yyjson_arr_size(operations));
    yyjson_arr_iter iterator = yyjson_arr_iter_with(operations);
    for (auto* operation = yyjson_arr_iter_next(&iterator); operation != nullptr;
         operation = yyjson_arr_iter_next(&iterator), ++operationIndex) {
        progress.building(operationIndex);
        if (operationIndex != 0 && call.control.stop.stop_requested())
            throw ToolFailure(McpToolErrorCode::Cancelled,
                              "The request was cancelled; nothing was applied.");
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
            if (isFromFile(item))
                item = readDataFile(item, dataFiles, at + " (" + id + "): argument \"" + name + "\"");
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
        throw ToolFailure(McpToolErrorCode::Cancelled,
                          "The request was cancelled; nothing was applied.");
    const auto work = transaction.workUnits();
    // #747: the commit reports where it stands in the live queue.
    if (call.lease != nullptr && progress.active()) {
        auto control = call.lease->control();
        control.progress = [&progress](const scripting::LiveCommitProgress& step) {
            using Step = scripting::LiveCommitProgress::Step;
            switch (step.step) {
            case Step::Queued:
                progress.waiting(step.ahead);
                break;
            case Step::Preparing:
                progress.preparing(step.applied, step.total);
                break;
            case Step::Prepared:
                progress.prepared();
                break;
            case Step::Publishing:
                progress.publishing();
                break;
            }
        };
        call.lease->setControl(std::move(control));
    } else {
        progress.waiting(0);
    }
    // The lease outlives this call's progress reporter, so it lets go of it on every exit.
    struct ProgressRelease final {
        scripting::LiveSessionLease* lease;
        bool held;
        ~ProgressRelease() {
            if (lease == nullptr || !held)
                return;
            auto control = lease->control();
            control.progress = {};
            lease->setControl(std::move(control));
        }
    } release{call.lease, progress.active()};
    const auto started = std::chrono::steady_clock::now();
    auto result = call.session.execute(std::move(transaction));
    if (call.lease == nullptr && result.command)
        core.recordHeadlessCommit(std::chrono::steady_clock::now() - started);
    if (!result.command) {
        const auto* live = core.liveHost();
        throwUnavailable(call, result, work,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started),
                         core.retryAfterMs(live != nullptr ? live->queuedCommits()
                                                           : scripting::kLiveCommitQueueCapacity));
    }
    // #748: an edit that would make the project too large to save is refused at the edit, typed,
    // with the limit it would pass; nothing was applied and the project still saves.
    if (result.command->status == commands::CommandStatus::ProjectTooLarge)
        throwProjectTooLarge(*result.command);
    call.committed = true;
    const auto command =
        std::make_shared<const commands::CommandResult>(std::move(*result.command));
    if (check && command->changed() && command->renderAffecting)
        progress.checking();
    const auto report = std::make_shared<const CompileReport>(
        check ? compileCheck(core, call, *command) : CompileReport{});
    // #745: the full id list up to kMcpTransactOutputLimit; past it the summary form answers, so a
    // transaction Bloom applied is never refused for the size of its answer.
    const bool listed = response == "full" && command->outputs.size() <= kMcpTransactOutputLimit;
    // Once the transaction changed the project, a compact answer stands by in case the full one
    // still exceeds the response bound.
    if (command->changed())
        call.compact = [command, report](Json& json) {
            auto* compact = transactValue(json, *command, *report, false, 16, 16);
            json.set(compact, "responseCompacted", json.boolean(true));
            return compact;
        };
    auto* value =
        transactValue(out, *command, *report, listed, kResultDiagnosticLimit, kCompileRecordLimit);
    if (response == "full" && !listed) {
        auto* limit = out.object();
        out.set(limit, "name", out.text(describeLimit(McpLimit::TransactOutputs).name));
        out.set(limit, "value", out.number(kMcpTransactOutputLimit));
        out.set(limit, "actual", out.number(command->outputs.size()));
        out.set(value, "outputsSummarized", limit);
    }
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
        stale.origin = commands::CommandOrigin::Agent;
        stale.status = commands::CommandStatus::StaleRevision;
        stale.beforeRevision = call.session.snapshot().revision();
        stale.afterRevision = stale.beforeRevision;
        stale.expectedRevision = expected;
        return commandResult(out, stale);
    } else {
        result = redo ? call.session.redo(commands::CommandOrigin::Agent)
                      : call.session.undo(commands::CommandOrigin::Agent);
    }
    if (!result.command)
        throwUnavailable(call, result);
    return commandResult(out, *result.command);
}

yyjson_mut_val* eventsTool(McpServerCore::State& core, Json& out, McpCall& call,
                           yyjson_val* arguments) {
    members(arguments,
            {"sinceRevision", "afterSequence", "limit", "kinds", "compositions", "response"},
            {"sinceRevision"});
    const auto since = integer(member(arguments, "sinceRevision"));
    const auto after =
        member(arguments, "afterSequence") ? integer(member(arguments, "afterSequence")) : 0;
    // #747 (D2): a page of at most `limit` events, the events a filter selects, or one summary.
    auto limit = kMcpEventsPageLimit;
    if (auto* value = member(arguments, "limit")) {
        limit = static_cast<std::size_t>(integer(value));
        if (limit == 0 || limit > kMcpEventsPageLimit)
            throw InvalidInput("limit is " + grouped(limit) + "; use 1 to " +
                               grouped(kMcpEventsPageLimit));
    }
    const auto filter = eventFilter(arguments);
    if (std::ranges::find(filter.kinds, kMcpJobEventKind) != filter.kinds.end())
        throw InvalidInput("kinds \"job\" is for events.subscribe; jobs.list lists the jobs");
    bool summaryForm = false;
    if (auto* response = member(arguments, "response")) {
        const auto form = string(response, 16);
        if (form != "events" && form != "summary")
            throw InvalidInput("response is \"events\" or \"summary\"");
        summaryForm = form == "summary";
    }
    const auto revision = call.session.snapshot().revision().value();
    const std::lock_guard lock(core.eventsMutex);
    // A cursor from before the live project was replaced must start again.
    const bool replaced = core.replaced && after < core.resetSequence;
    if (!replaced && (since > revision || after > core.sequence))
        throw InvalidInput("Event cursor is in the future");
    auto* result = out.object();
    out.set(result, "revision", out.number(revision));
    if (summaryForm) {
        // #747 (D2): what changed after the cursor, coalesced into revision ranges with counts,
        // the shape of a subscription's notification.
        auto cursor = after;
        static_cast<void>(summarizeEvents(core, out, result, filter, cursor, since));
        out.set(result, "sequence", out.number(cursor));
        out.set(result, "truncated", out.boolean(false));
        return result;
    }
    const auto& events = core.events;
    const auto revisionOf = [](const McpEventEntry& entry) {
        if (const auto* command = std::get_if<McpRetainedCommand>(&entry))
            return command->result.afterRevision.value();
        return std::get<McpNotice>(entry).revision;
    };
    out.set(result, "resetRequired",
            out.boolean(replaced || (!events.empty() && events.front().first > 1 &&
                                     after < events.front().first - 1 &&
                                     since <= revisionOf(events.front().second))));
    auto* list = out.array();
    constexpr std::array<std::string_view, 3> names{"revision", "history", "rejected"};
    // #745: one answer lists at most kMcpEventsOutputBudget created ids; a client continues from
    // `sequence`, which is then the last event listed. #747 (D2): and at most `limit` events.
    std::size_t listedOutputs = 0;
    std::size_t listed = 0;
    std::optional<std::uint64_t> lastListed;
    bool truncated = false;
    for (const auto& [sequence, entry] : events) {
        if ((!replaced && sequence <= after) || (!replaced && revisionOf(entry) < since))
            continue;
        const auto* event = std::get_if<McpRetainedCommand>(&entry);
        const auto kind = event ? names.at(static_cast<std::size_t>(event->kind))
                                : std::string_view(std::get<McpNotice>(entry).kind);
        if (!filter.selectsKind(kind))
            continue;
        if (!filter.compositions.empty()) {
            std::uint64_t single = 0;
            std::span<const std::uint64_t> named;
            if (event) {
                named = event->compositions;
            } else if (const auto& notice = std::get<McpNotice>(entry); notice.composition) {
                single = *notice.composition;
                named = {&single, 1};
            }
            if (!filter.selectsCompositions(named))
                continue;
        }
        if (listed == limit) {
            truncated = true;
            break;
        }
        yyjson_mut_val* value = nullptr;
        if (event) {
            const auto kept = event->result.outputs.size();
            if (lastListed && listedOutputs + kept > kMcpEventsOutputBudget) {
                truncated = true;
                break;
            }
            listedOutputs += kept;
            const bool full = kept == event->outputCount;
            value = commandResult(out, event->result, full);
            out.set(value, "outputCount", out.number(event->outputCount));
            if (!full)
                out.set(value, "outputsOmitted", out.boolean(true));
            out.set(value, "kind", out.text(kind));
        } else {
            value = noticeValue(out, std::get<McpNotice>(entry));
        }
        out.set(value, "sequence", out.number(sequence));
        out.append(list, value);
        lastListed = sequence;
        ++listed;
    }
    out.set(result, "sequence", out.number(truncated ? *lastListed : core.sequence));
    out.set(result, "truncated", out.boolean(truncated));
    out.set(result, "events", list);
    return result;
}

namespace {
// The composition omitted contextual arguments fall back to: the context's, else the project's
// first. Never 0, which names no composition.
std::optional<std::uint64_t> contextComposition(const McpCall& call,
                                                const document::Snapshot& snapshot) {
    if (call.context.composition && *call.context.composition != 0)
        return call.context.composition;
    const auto compositions = snapshot.project().compositions();
    if (compositions.empty())
        return std::nullopt;
    return compositions.front().id().value();
}

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
    const auto composition = contextComposition(call, snapshot);
    out.set(result, "composition", composition ? out.number(*composition) : out.null());
    auto* selection = out.array();
    for (const auto node : context.selection)
        out.append(selection, out.number(node));
    out.set(result, "selectedNodes", selection);
    auto* layers = out.array();
    for (const auto layer : context.layers)
        out.append(layers, out.number(layer));
    out.set(result, "selectedLayers", layers);
    // #645: the Viewer's drill level and the shape in point editing (empty headless).
    auto* level = out.array();
    for (const auto group : context.level)
        out.append(level, out.number(group));
    out.set(result, "level", level);
    out.set(result, "points", context.points ? out.number(*context.points) : out.null());
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
    if (auto* requested = member(arguments, "kind");
        yyjson_is_str(requested) &&
        isInspectionQuery(std::string_view(yyjson_get_str(requested), yyjson_get_len(requested))))
        return inspectionQuery(core, out, call, arguments, string(requested, 32));
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
    if (individualParameter && !member(arguments, "parameter"))
        throw InvalidInput("Individual parameter queries require a parameter ID");
    const auto defaultedComposition = contextComposition(call, snapshot);
    if (individualParameter && !member(arguments, "composition") && !defaultedComposition)
        throw InvalidInput("Individual parameter queries need a composition: the project has none");
    if (individualParameter && member(arguments, "expectedRevision") &&
        integer(member(arguments, "expectedRevision")) != snapshot.revision().value())
        throw InvalidInput("Stale parameter revision");
    if (kind == "parameter-value" && !member(arguments, "expectedRevision"))
        throw InvalidInput("String chunks require expectedRevision");
    if (kind == "parameter" && (member(arguments, "offset") || member(arguments, "limit")))
        throw InvalidInput("String chunk options require parameter-value");
    if (individualParameter) {
        const auto compositionId = member(arguments, "composition")
                                       ? integer(member(arguments, "composition"))
                                       : *defaultedComposition;
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
        const auto& reference = scripting::OperationReference::builtIn();
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
            // The ids the transaction result's outputs carry, and what the call changes beyond
            // its target: the same declarations help prints.
            auto* outputs = out.array();
            auto* sideEffects = out.array();
            if (const auto* entry = reference.find(descriptor.schema.typeId)) {
                for (const auto& output : entry->outputs) {
                    auto* row = out.object();
                    out.set(row, "name", out.text(output.name));
                    out.set(row, "kind", out.text(output.kind));
                    out.set(row, "repeated", out.boolean(output.repeated));
                    if (!output.note.empty())
                        out.set(row, "note", out.text(output.note));
                    out.append(outputs, row);
                }
                for (const auto& effect : entry->sideEffects)
                    out.append(sideEffects, out.text(effect));
            }
            out.set(record, "outputs", outputs);
            out.set(record, "sideEffects", sideEffects);
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
                            layerFilter(*composition, arguments), &snapshot.project());
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
                           "operations, context, enums, text-metrics, layer-bounds, history, "
                           "viewer, preview-cache, jobs, render-route, pixels");
    out.set(result, "records", records);
    projectRecords(out, result, arguments);
    return result;
}
} // namespace bloom::mcp
