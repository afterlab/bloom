// #745: the node reference's values (scripting/node_reference.hpp) written straight into a
// response, so a projection never parses a value's JSON text into a document of its own. Each
// writer produces exactly what parsing the matching scripting text gives (reference_values_tests
// compares them over every node type, every parameter default and a set of values).
#include "core_internal.hpp"

#include <bloom/document/gradient_interpolation.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/scripting/node_reference.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <type_traits>
#include <variant>

namespace bloom::mcp {
namespace {

yyjson_mut_val* vector(Json& out, const std::initializer_list<double> values) {
    auto* result = out.array();
    for (const auto value : values)
        out.append(result, referenceNumber(out, value));
    return result;
}

yyjson_mut_val* contour(Json& out, const document::PathContour& path) {
    auto* value = out.object();
    out.set(value, "closed", out.boolean(path.closed));
    auto* anchors = out.array();
    const auto handle = [&](const std::optional<document::Vec2d>& point) {
        return point ? vector(out, {point->x, point->y}) : out.null();
    };
    for (const auto& anchor : path.anchors) {
        auto* row = out.object();
        out.set(row, "point", vector(out, {anchor.point.x, anchor.point.y}));
        out.set(row, "inHandle", handle(anchor.inHandle));
        out.set(row, "outHandle", handle(anchor.outHandle));
        out.set(row, "mode", out.text(document::pathAnchorModeName(anchor.mode)));
        out.append(anchors, row);
    }
    out.set(value, "anchors", anchors);
    return value;
}

yyjson_mut_val* path(Json& out, const document::PathValue& item) {
    if (document::pathValueHasFlatForm(item))
        return contour(out, item.firstContour());
    auto* value = out.object();
    auto* contours = out.array();
    for (const auto& each : item.contours)
        out.append(contours, contour(out, each));
    out.set(value, "contours", contours);
    if (item.fillRule != document::PathFillRule::Inherit)
        out.set(value, "fillRule", out.text(document::pathFillRuleName(item.fillRule)));
    if (!item.groups.empty()) {
        auto* groups = out.array();
        for (const auto& group : item.groups) {
            auto* row = out.object();
            out.set(row, "key", out.text(group.key));
            out.set(row, "firstContour", out.number(group.firstContour));
            out.set(row, "contourCount", out.number(group.contourCount));
            if (group.color)
                out.set(row, "color",
                        vector(out, {group.color->red, group.color->green, group.color->blue,
                                     group.color->alpha}));
            out.append(groups, row);
        }
        out.set(value, "groups", groups);
    }
    return value;
}

yyjson_mut_val* gradient(Json& out, const document::GradientValue& item) {
    auto* value = out.object();
    auto* stops = out.array();
    for (const auto& stop : item.stops) {
        auto* row = out.object();
        out.set(row, "position", referenceNumber(out, stop.position));
        out.set(row, "color",
                vector(out, {stop.color.red, stop.color.green, stop.color.blue, stop.color.alpha}));
        out.append(stops, row);
    }
    out.set(value, "stops", stops);
    const auto& modes = item.modes;
    if (modes.interpolation != document::GradientInterpolation::Linear)
        out.set(value, "interpolation",
                out.text(document::gradientInterpolationName(modes.interpolation)));
    if (modes.colorMode != document::GradientColorMode::Rgb)
        out.set(value, "colorMode", out.text(document::gradientColorModeName(modes.colorMode)));
    if (modes.hueDirection != document::GradientHueDirection::Near)
        out.set(value, "hueDirection",
                out.text(document::gradientHueDirectionName(modes.hueDirection)));
    return value;
}

} // namespace

yyjson_mut_val* shortestNumber(Json& out, const double value) {
    if (!std::isfinite(value))
        return out.null();
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    const std::string_view text(buffer.data(), converted.ptr);
    // Digits alone read back as an integer when it fits 64 bits (2^63 does, unsigned).
    if (text.find_first_of(".eE") == std::string_view::npos) {
        if (text.starts_with('-')) {
            std::int64_t whole = 0;
            if (std::from_chars(text.data(), text.data() + text.size(), whole).ec == std::errc{})
                return out.signedNumber(whole);
        } else {
            std::uint64_t whole = 0;
            if (std::from_chars(text.data(), text.data() + text.size(), whole).ec == std::errc{})
                return out.number(whole);
        }
    }
    return out.real(value);
}

yyjson_mut_val* referenceNumber(Json& out, const double value) {
    if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 1e15)
        return out.signedNumber(static_cast<std::int64_t>(value));
    return shortestNumber(out, value);
}

yyjson_mut_val* referenceValue(Json& out, const document::ParameterValue& value) {
    return std::visit(
        [&out](const auto& held) -> yyjson_mut_val* {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>)
                return out.boolean(held);
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return out.signedNumber(held);
            else if constexpr (std::is_same_v<T, double>)
                return referenceNumber(out, held);
            else if constexpr (std::is_same_v<T, std::string>)
                return out.text(held);
            else if constexpr (std::is_same_v<T, document::Vec2d>)
                return vector(out, {held.x, held.y});
            else if constexpr (std::is_same_v<T, document::Vec3d>)
                return vector(out, {held.x, held.y, held.z});
            else if constexpr (std::is_same_v<T, core::Color4d>)
                return vector(out, {held.red, held.green, held.blue, held.alpha});
            else if constexpr (std::is_same_v<T, core::RationalTime>) {
                auto* pair = out.array();
                out.append(pair, out.signedNumber(held.numerator()));
                out.append(pair, out.signedNumber(held.denominator()));
                return pair;
            } else if constexpr (std::is_same_v<T, document::GradientValue>)
                return gradient(out, held);
            else if constexpr (std::is_same_v<T, document::NumberListValue>) {
                auto* list = out.object();
                auto* values = out.array();
                for (const double each : held.values)
                    out.append(values, referenceNumber(out, each));
                out.set(list, "values", values);
                return list;
            } else
                return path(out, held);
        },
        value);
}

yyjson_mut_val* referenceTime(Json& out, const core::RationalTime time,
                              const document::FrameRate& rate) {
    const auto count = scripting::framesAt(time, rate);
    auto* value = out.object();
    out.set(value, "frames",
            count.exact ? out.signedNumber(count.frames) : referenceNumber(out, count.approximate));
    auto* seconds = out.array();
    out.append(seconds, out.signedNumber(time.numerator()));
    out.append(seconds, out.signedNumber(time.denominator()));
    out.set(value, "seconds", seconds);
    return value;
}

yyjson_mut_val* referenceMetadata(Json& out, const std::string_view schemaKey,
                                  const document::ParameterValueKind kind,
                                  const document::ParameterDefinition* definition,
                                  const std::string_view role) {
    const auto presentation = document::parameterPresentation(schemaKey, kind, role);
    auto* value = out.object();
    out.set(value, "valueKind", out.text(scripting::parameterKindName(kind)));
    out.set(value, "animatable",
            out.boolean(definition != nullptr && definition->supportsAnimation));
    out.set(value, "default",
            definition != nullptr ? referenceValue(out, definition->defaultValue) : out.null());
    out.set(value, "unit", out.text(presentation.unit));
    if (presentation.kind != document::ParameterPresentationKind::Other) {
        const auto bound = [&](const double limit) {
            return std::fabs(limit) >= std::numeric_limits<double>::max() / 2
                       ? out.null()
                       : referenceNumber(out, limit);
        };
        out.set(value, "displayScale", referenceNumber(out, presentation.displayScale));
        out.set(value, "minimum", bound(presentation.minimum));
        out.set(value, "maximum", bound(presentation.maximum));
        out.set(value, "softMinimum", referenceNumber(out, presentation.softMinimum));
        out.set(value, "softMaximum", referenceNumber(out, presentation.softMaximum));
    }
    auto* choices = out.array();
    for (const auto& choice : scripting::parameterChoices(schemaKey)) {
        auto* row = out.object();
        out.set(row, "value", out.signedNumber(choice.value));
        out.set(row, "label", out.text(choice.label));
        out.append(choices, row);
    }
    out.set(value, "choices", choices);
    return value;
}

yyjson_mut_val* referenceNodeType(Json& out, const document::NodeDefinition& definition,
                                  const bool full) {
    auto* value = out.object();
    out.set(value, "id", out.text(definition.key.typeId));
    out.set(value, "name", out.text(scripting::nodeTypeDisplayName(definition.key.typeId)));
    out.set(value, "category", out.text(scripting::nodeCategoryName(definition)));
    out.set(value, "schemaVersion", out.number(definition.key.schemaVersion));
    out.set(value, "cardinality",
            out.text(definition.cardinality == document::NodeCardinality::OnePerComposition
                         ? "one-per-composition"
                         : "many"));
    auto* inputs = out.array();
    for (const auto& port : definition.inputs) {
        auto* row = out.object();
        out.set(row, "port", out.text(port.name));
        out.set(row, "kind", out.text(scripting::socketKindName(port.valueKind)));
        out.set(row, "required", out.boolean(port.required));
        // #490: an inspection input (the Image Preview's) also takes these kinds.
        if (port.acceptsPixelKinds) {
            auto* also = out.array();
            out.append(also, out.text(scripting::socketKindName(document::SocketValueKind::Mask)));
            out.append(also,
                       out.text(scripting::socketKindName(document::SocketValueKind::ChannelSet)));
            out.set(row, "alsoAccepts", also);
        }
        out.append(inputs, row);
    }
    out.set(value, "inputs", inputs);
    auto* outputs = out.array();
    for (const auto& port : definition.outputs) {
        auto* row = out.object();
        out.set(row, "port", out.text(port.name));
        out.set(row, "kind", out.text(scripting::socketKindName(port.valueKind)));
        out.append(outputs, row);
    }
    out.set(value, "outputs", outputs);
    auto* slots = out.array();
    for (const auto* slot :
         {definition.layerSlotInput ? &*definition.layerSlotInput : nullptr,
          definition.audioLayerSlotInput ? &*definition.audioLayerSlotInput : nullptr}) {
        if (slot == nullptr)
            continue;
        auto* row = out.object();
        out.set(row, "role", out.text(slot->role));
        out.set(row, "kind", out.text(scripting::socketKindName(slot->valueKind)));
        out.append(slots, row);
    }
    out.set(value, "layerSlots", slots);
    auto* parameters = out.array();
    for (const auto& parameter : definition.parameters) {
        auto* row = out.object();
        out.set(row, "role", out.text(parameter.role));
        out.set(row, "schemaKey", out.text(parameter.schemaKey));
        if (full) {
            out.set(row, "metadata",
                    referenceMetadata(out, parameter.schemaKey, parameter.valueKind, &parameter,
                                      parameter.role));
        } else {
            out.set(row, "valueKind", out.text(scripting::parameterKindName(parameter.valueKind)));
            out.set(row, "animatable", out.boolean(parameter.supportsAnimation));
        }
        out.append(parameters, row);
    }
    out.set(value, "parameters", parameters);
    return value;
}

} // namespace bloom::mcp
