// The node and parameter vocabulary of the agent surfaces (node_reference.hpp). The closed values
// are document::parameterEnumOptions(), the one table set-source and read-back use;
// bloom.scripting.operation-reference checks every Choice schema of the built-in registry has
// them.

#include "reference_internal.hpp"

#include <bloom/document/node_groups.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/parameter_presentation.hpp>
#include <bloom/scripting/node_reference.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>

namespace bloom::scripting {
namespace {

std::string number(const double value) {
    if (!std::isfinite(value))
        return "null";
    if (std::floor(value) == value && std::fabs(value) < 1e15)
        return std::to_string(static_cast<std::int64_t>(value));
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), converted.ptr);
}

std::string vector(const std::initializer_list<double> values) {
    std::string text = "[";
    for (const auto value : values)
        text += (text.size() > 1 ? "," : "") + number(value);
    return text + "]";
}

std::string pathJson(const document::PathValue& path) {
    std::string text =
        std::string("{\"closed\":") + (path.closed ? "true" : "false") + ",\"anchors\":[";
    for (std::size_t index = 0; index < path.anchors.size(); ++index) {
        const auto& anchor = path.anchors[index];
        const auto handle = [](const std::optional<document::Vec2d>& point) {
            return point ? vector({point->x, point->y}) : std::string("null");
        };
        text += (index == 0 ? "" : ",") + std::string("{\"point\":") +
                vector({anchor.point.x, anchor.point.y}) +
                ",\"inHandle\":" + handle(anchor.inHandle) +
                ",\"outHandle\":" + handle(anchor.outHandle) +
                ",\"mode\":" + detail::jsonQuote(document::pathAnchorModeName(anchor.mode)) + "}";
    }
    return text + "]}";
}

// #546: the gradient's modes that are not their defaults, as JSON members after its stops.
[[nodiscard]] std::string gradientModesJson(const document::GradientModes& modes) {
    std::string text;
    if (modes.interpolation != document::GradientInterpolation::Linear)
        text += ",\"interpolation\":\"" +
                std::string(document::gradientInterpolationName(modes.interpolation)) + "\"";
    if (modes.colorMode != document::GradientColorMode::Rgb)
        text += ",\"colorMode\":\"" +
                std::string(document::gradientColorModeName(modes.colorMode)) + "\"";
    if (modes.hueDirection != document::GradientHueDirection::Near)
        text += ",\"hueDirection\":\"" +
                std::string(document::gradientHueDirectionName(modes.hueDirection)) + "\"";
    return text;
}

// #541: a gradient as {"stops": [{"position": p, "color": [r, g, b, a]}]}, stops in stored order.
std::string gradientJson(const document::GradientValue& gradient) {
    std::string text = "{\"stops\":[";
    for (std::size_t index = 0; index < gradient.stops.size(); ++index) {
        const auto& stop = gradient.stops[index];
        text += (index == 0 ? "" : ",") + std::string("{\"position\":") + number(stop.position) +
                ",\"color\":" +
                vector({stop.color.red, stop.color.green, stop.color.blue, stop.color.alpha}) + "}";
    }
    return text + "]" + gradientModesJson(gradient.modes) + "}";
}

// A multiplication that reports overflow instead of wrapping, portable to every compiler.
std::optional<std::int64_t> multiplied(const std::int64_t left, const std::int64_t right) {
    if (left == 0 || right == 0)
        return 0;
    if (std::llabs(left) > std::numeric_limits<std::int64_t>::max() / std::llabs(right))
        return std::nullopt;
    return left * right;
}

} // namespace

std::vector<ParameterChoice> parameterChoices(const std::string_view key) {
    // The document's table is the one vocabulary (set-source accepts its labels, read-back names
    // them); this is the same list in the reference's shape.
    std::vector<ParameterChoice> result;
    for (auto& option : document::parameterEnumOptions(key))
        result.push_back({.value = option.value, .label = std::move(option.label)});
    return result;
}

std::string nodeTypeDisplayName(const std::string_view typeId) {
    auto name = typeId;
    if (name.starts_with("bloom."))
        name.remove_prefix(6);
    std::string result;
    bool start = true;
    for (const char c : name) {
        if (c == '-' || c == '_' || c == '.') {
            result.push_back(' ');
            start = true;
            continue;
        }
        result.push_back(start ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                               : c);
        start = false;
    }
    return result;
}

std::string_view nodeCategoryName(const document::NodeDefinition& definition) noexcept {
    const auto group = document::nodeGroupOf(definition.key.typeId);
    return group ? document::nodeGroupWireName(*group) : std::string_view{};
}

std::string_view socketKindName(const document::SocketValueKind kind) noexcept {
    return document::socketValueKindName(kind);
}

std::string_view parameterKindName(const document::ParameterValueKind kind) noexcept {
    return document::parameterValueKindName(kind);
}

std::string_view assetKindName(const std::uint8_t kind) noexcept {
    constexpr std::array<std::string_view, 6> names{"image", "sequence", "audio",
                                                    "font",  "video",    "lut"};
    return kind < names.size() ? names[kind] : "unknown";
}

std::string parameterValueJson(const document::ParameterValue& value) {
    return std::visit(
        [](const auto& held) -> std::string {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>)
                return held ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return std::to_string(held);
            else if constexpr (std::is_same_v<T, double>)
                return number(held);
            else if constexpr (std::is_same_v<T, std::string>)
                return detail::jsonQuote(held);
            else if constexpr (std::is_same_v<T, document::Vec2d>)
                return vector({held.x, held.y});
            else if constexpr (std::is_same_v<T, document::Vec3d>)
                return vector({held.x, held.y, held.z});
            else if constexpr (std::is_same_v<T, core::Color4d>)
                return vector({held.red, held.green, held.blue, held.alpha});
            else if constexpr (std::is_same_v<T, core::RationalTime>)
                return "[" + std::to_string(held.numerator()) + "," +
                       std::to_string(held.denominator()) + "]";
            else if constexpr (std::is_same_v<T, document::GradientValue>)
                return gradientJson(held);
            else
                return pathJson(held);
        },
        value);
}

FrameCount framesAt(const core::RationalTime time, const document::FrameRate& rate) {
    // frames = (n / d) * (rateN / rateD), reduced before multiplying so exact values stay exact.
    auto n = time.numerator();
    auto d = time.denominator();
    auto rateNumerator = static_cast<std::int64_t>(rate.numerator());
    auto rateDenominator = static_cast<std::int64_t>(rate.denominator());
    const auto first = std::gcd(n, rateDenominator);
    if (first != 0) {
        n /= first;
        rateDenominator /= first;
    }
    const auto second = std::gcd(rateNumerator, d);
    if (second != 0) {
        rateNumerator /= second;
        d /= second;
    }
    const auto numerator = multiplied(n, rateNumerator);
    const auto denominator = multiplied(d, rateDenominator);
    FrameCount count;
    count.approximate = time.toSeconds() * static_cast<double>(rate.numerator()) /
                        static_cast<double>(rate.denominator());
    if (numerator && denominator && *denominator != 0 && *numerator % *denominator == 0) {
        count.exact = true;
        count.frames = *numerator / *denominator;
    }
    return count;
}

std::string timeJson(const core::RationalTime time, const document::FrameRate& rate) {
    const auto count = framesAt(time, rate);
    return "{\"frames\":" +
           (count.exact ? std::to_string(count.frames) : number(count.approximate)) +
           ",\"seconds\":[" + std::to_string(time.numerator()) + "," +
           std::to_string(time.denominator()) + "]}";
}

std::string parameterMetadataJson(const std::string_view schemaKey,
                                  const document::ParameterValueKind kind,
                                  const document::ParameterDefinition* definition,
                                  const std::string_view role) {
    const auto presentation = document::parameterPresentation(schemaKey, kind, role);
    std::string text = "{\"valueKind\":" + detail::jsonQuote(parameterKindName(kind));
    text += ",\"animatable\":";
    text += definition != nullptr && definition->supportsAnimation ? "true" : "false";
    text += ",\"default\":" +
            (definition != nullptr ? parameterValueJson(definition->defaultValue) : "null");
    const bool numeric = presentation.kind != document::ParameterPresentationKind::Other;
    text += ",\"unit\":" + detail::jsonQuote(presentation.unit);
    if (numeric) {
        text += ",\"displayScale\":" + number(presentation.displayScale);
        const auto bound = [](const double value) {
            return std::fabs(value) >= std::numeric_limits<double>::max() / 2 ? std::string("null")
                                                                              : number(value);
        };
        text += ",\"minimum\":" + bound(presentation.minimum);
        text += ",\"maximum\":" + bound(presentation.maximum);
        text += ",\"softMinimum\":" + number(presentation.softMinimum);
        text += ",\"softMaximum\":" + number(presentation.softMaximum);
    }
    text += ",\"choices\":[";
    const auto choices = parameterChoices(schemaKey);
    for (std::size_t index = 0; index < choices.size(); ++index)
        text += (index == 0 ? "" : ",") + std::string("{\"value\":") +
                std::to_string(choices[index].value) +
                ",\"label\":" + detail::jsonQuote(choices[index].label) + "}";
    return text + "]}";
}

const document::ParameterDefinition*
findParameterDefinition(const document::NodeDefinition& definition, const std::string_view role) {
    const auto found =
        std::ranges::find(definition.parameters, role, &document::ParameterDefinition::role);
    return found == definition.parameters.end() ? nullptr : &*found;
}

std::string nodeTypeJson(const document::NodeDefinition& definition, const bool full) {
    std::string text =
        "{\"id\":" + detail::jsonQuote(definition.key.typeId) +
        ",\"name\":" + detail::jsonQuote(nodeTypeDisplayName(definition.key.typeId)) +
        ",\"category\":" + detail::jsonQuote(nodeCategoryName(definition)) +
        ",\"schemaVersion\":" + std::to_string(definition.key.schemaVersion) + ",\"cardinality\":" +
        (definition.cardinality == document::NodeCardinality::OnePerComposition
             ? std::string("\"one-per-composition\"")
             : std::string("\"many\""));
    text += ",\"inputs\":[";
    for (std::size_t index = 0; index < definition.inputs.size(); ++index) {
        const auto& port = definition.inputs[index];
        text += (index == 0 ? "" : ",") + std::string("{\"port\":") + detail::jsonQuote(port.name) +
                ",\"kind\":" + detail::jsonQuote(socketKindName(port.valueKind)) +
                ",\"required\":" + (port.required ? "true" : "false");
        // #490: an inspection input (the Image Preview's) also takes these kinds.
        if (port.acceptsPixelKinds)
            text += ",\"alsoAccepts\":[" +
                    detail::jsonQuote(socketKindName(document::SocketValueKind::Mask)) + "," +
                    detail::jsonQuote(socketKindName(document::SocketValueKind::ChannelSet)) + "]";
        text += "}";
    }
    text += "],\"outputs\":[";
    for (std::size_t index = 0; index < definition.outputs.size(); ++index) {
        const auto& port = definition.outputs[index];
        text += (index == 0 ? "" : ",") + std::string("{\"port\":") + detail::jsonQuote(port.name) +
                ",\"kind\":" + detail::jsonQuote(socketKindName(port.valueKind)) + "}";
    }
    text += "],\"layerSlots\":[";
    bool firstSlot = true;
    for (const auto* slot :
         {definition.layerSlotInput ? &*definition.layerSlotInput : nullptr,
          definition.audioLayerSlotInput ? &*definition.audioLayerSlotInput : nullptr}) {
        if (slot == nullptr)
            continue;
        text += (firstSlot ? "" : ",") + std::string("{\"role\":") + detail::jsonQuote(slot->role) +
                ",\"kind\":" + detail::jsonQuote(socketKindName(slot->valueKind)) + "}";
        firstSlot = false;
    }
    text += "],\"parameters\":[";
    for (std::size_t index = 0; index < definition.parameters.size(); ++index) {
        const auto& parameter = definition.parameters[index];
        text += (index == 0 ? "" : ",") + std::string("{\"role\":") +
                detail::jsonQuote(parameter.role) +
                ",\"schemaKey\":" + detail::jsonQuote(parameter.schemaKey);
        if (full) {
            text +=
                ",\"metadata\":" + parameterMetadataJson(parameter.schemaKey, parameter.valueKind,
                                                         &parameter, parameter.role);
        } else {
            text += ",\"valueKind\":" + detail::jsonQuote(parameterKindName(parameter.valueKind)) +
                    ",\"animatable\":" + (parameter.supportsAnimation ? "true" : "false");
        }
        text += "}";
    }
    return text + "]}";
}

std::string nodeReferenceMarkdown() {
    const auto& registry = document::builtInNodeDefinitions();
    std::vector<const document::NodeDefinition*> definitions;
    for (const auto& definition : registry.definitions())
        definitions.push_back(&definition);
    std::ranges::stable_sort(definitions, {}, [](const auto* definition) {
        const auto group = document::nodeGroupOf(definition->key.typeId);
        return std::pair(group ? static_cast<int>(*group)
                               : static_cast<int>(document::kNodeGroups.size()),
                         definition->key.typeId);
    });
    std::string text =
        "# Bloom node and parameter reference\n\n"
        "Generated from the running build's node registry. Add a node with "
        "`bloom.node.add {nodeType}`, connect ports with `bloom.node.connect`, and set a "
        "parameter with `bloom.parameter.set-source {parameter, value}` (the parameter ids are "
        "in `query {kind:\"nodes\"}` and `query {kind:\"layers\"}`). Enumerations take "
        "either the stored value or its label (case-insensitive). `query {kind:\"node-types\", "
        "nodeType}` returns the same data as JSON.\n\n"
        "Port kinds connect when they are equal, or when the source promotes: integer to "
        "scalar, boolean to integer or scalar, scalar to vector2 or vector3. An Image "
        "Preview's input also takes a mask or a channel set.\n\n"
        "## Blend modes\n\n";
    for (const auto& choice : parameterChoices(document::kBlendModeParameterSchemaKey))
        text += "- " + std::to_string(choice.value) + " " + choice.label + "\n";
    std::string_view category;
    for (const auto* definition : definitions) {
        const auto group = document::nodeGroupOf(definition->key.typeId);
        const auto name = group ? document::nodeGroupName(*group) : std::string_view{};
        if (name != category) {
            category = name;
            text += "\n## " + std::string(name) + "\n";
        }
        text += "\n### " + nodeTypeDisplayName(definition->key.typeId) + " (`" +
                definition->key.typeId + "`)\n\n";
        if (definition->cardinality == document::NodeCardinality::OnePerComposition)
            text += "One per composition.\n\n";
        const auto ports = [&text](std::string_view label, const auto& list) {
            if (list.empty())
                return;
            text += std::string(label) + ": ";
            for (std::size_t index = 0; index < list.size(); ++index)
                text += (index == 0 ? "`" : ", `") + list[index].name + "` (" +
                        std::string(socketKindName(list[index].valueKind)) + ")";
            text += "\n\n";
        };
        ports("Inputs", definition->inputs);
        ports("Outputs", definition->outputs);
        if (definition->lowering == document::NodeLoweringKind::ImagePreview)
            text += "An inspection sink: its input (an image, a mask or a channel set) is drawn on "
                    "the node and, optionally, behind the node editor's graph. It has no output "
                    "and never changes the composition's output, RAM Preview, exports or "
                    "renders.\n\n";
        if (definition->layerSlotInput)
            text += "Layer stack slots: role `" + definition->layerSlotInput->role + "`" +
                    (definition->audioLayerSlotInput
                         ? " and `" + definition->audioLayerSlotInput->role + "`"
                         : std::string()) +
                    "\n\n";
        if (definition->parameters.empty())
            continue;
        text += "| Role | Kind | Default | Unit and range | Values | Animatable |\n"
                "| --- | --- | --- | --- | --- | --- |\n";
        for (const auto& parameter : definition->parameters) {
            const auto presentation = document::parameterPresentation(parameter);
            std::string range = presentation.unit;
            if (presentation.kind == document::ParameterPresentationKind::Number ||
                presentation.kind == document::ParameterPresentationKind::Integer) {
                const bool bounded =
                    presentation.minimum > std::numeric_limits<double>::lowest() / 2 ||
                    presentation.maximum < std::numeric_limits<double>::max() / 2;
                if (bounded)
                    range += (range.empty() ? "" : " ") + std::string("[") +
                             (presentation.minimum > std::numeric_limits<double>::lowest() / 2
                                  ? number(presentation.minimum)
                                  : std::string("-inf")) +
                             ", " +
                             (presentation.maximum < std::numeric_limits<double>::max() / 2
                                  ? number(presentation.maximum)
                                  : std::string("inf")) +
                             "]";
            }
            std::string values;
            for (const auto& choice : parameterChoices(parameter.schemaKey))
                values += (values.empty() ? "" : ", ") + std::to_string(choice.value) + " " +
                          choice.label;
            auto value = parameterValueJson(parameter.defaultValue);
            if (value.size() > 48)
                value = value.substr(0, 45) + "...";
            text += "| `" + parameter.role + "` | " +
                    std::string(parameterKindName(parameter.valueKind)) + " | `" + value + "` | " +
                    range + " | " + values + " | " + (parameter.supportsAnimation ? "yes" : "no") +
                    " |\n";
        }
    }
    return text;
}

} // namespace bloom::scripting
