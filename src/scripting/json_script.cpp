#include <bloom/scripting/json_script.hpp>

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

namespace bloom::scripting {
namespace {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue final {
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, JsonArray,
                                 JsonObject>;
    Storage storage = nullptr;
};

class Parser final {
  public:
    explicit Parser(std::string_view input) : input_(input) {}

    [[nodiscard]] std::optional<JsonValue> parse(ScriptDiagnostic& diagnostic) {
        skipWhitespace();
        auto value = parseValue(diagnostic);
        if (!value.has_value()) {
            return std::nullopt;
        }
        skipWhitespace();
        if (offset_ != input_.size()) {
            fail(diagnostic, "trailing-data", "Unexpected data after the JSON value");
            return std::nullopt;
        }
        return value;
    }

  private:
    void skipWhitespace() noexcept {
        while (offset_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[offset_])) != 0) {
            ++offset_;
        }
    }

    void fail(ScriptDiagnostic& diagnostic, std::string code, std::string message) const {
        diagnostic = {
            .code = std::move(code), .byteOffset = offset_, .message = std::move(message)};
    }

    [[nodiscard]] bool consume(const char expected) noexcept {
        if (offset_ >= input_.size() || input_[offset_] != expected) {
            return false;
        }
        ++offset_;
        return true;
    }

    [[nodiscard]] std::optional<JsonValue> parseValue(ScriptDiagnostic& diagnostic) {
        skipWhitespace();
        if (offset_ >= input_.size()) {
            fail(diagnostic, "unexpected-end", "Expected a JSON value");
            return std::nullopt;
        }
        switch (input_[offset_]) {
        case '{':
            return parseObject(diagnostic);
        case '[':
            return parseArray(diagnostic);
        case '"': {
            auto value = parseString(diagnostic);
            return value.has_value() ? std::optional<JsonValue>(JsonValue{std::move(*value)})
                                     : std::nullopt;
        }
        case 't':
            return parseLiteral("true", JsonValue{true}, diagnostic);
        case 'f':
            return parseLiteral("false", JsonValue{false}, diagnostic);
        case 'n':
            return parseLiteral("null", JsonValue{nullptr}, diagnostic);
        default:
            return parseNumber(diagnostic);
        }
    }

    [[nodiscard]] std::optional<JsonValue> parseLiteral(std::string_view literal, JsonValue value,
                                                        ScriptDiagnostic& diagnostic) {
        if (input_.substr(offset_, literal.size()) != literal) {
            fail(diagnostic, "invalid-literal", "Invalid JSON literal");
            return std::nullopt;
        }
        offset_ += literal.size();
        return value;
    }

    [[nodiscard]] std::optional<std::string> parseString(ScriptDiagnostic& diagnostic) {
        if (!consume('"')) {
            fail(diagnostic, "invalid-string", "Expected a string");
            return std::nullopt;
        }
        std::string result;
        while (offset_ < input_.size()) {
            const char character = input_[offset_++];
            if (character == '"') {
                return result;
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                fail(diagnostic, "invalid-string", "Control characters are not allowed in strings");
                return std::nullopt;
            }
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            if (offset_ >= input_.size()) {
                fail(diagnostic, "invalid-escape", "Incomplete string escape");
                return std::nullopt;
            }
            const char escaped = input_[offset_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                result.push_back(escaped);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                if (offset_ + 4 > input_.size()) {
                    fail(diagnostic, "invalid-unicode", "Incomplete Unicode escape");
                    return std::nullopt;
                }
                unsigned value = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = input_[offset_++];
                    value <<= 4U;
                    if (digit >= '0' && digit <= '9') {
                        value += static_cast<unsigned>(digit - '0');
                    } else if (digit >= 'a' && digit <= 'f') {
                        value += static_cast<unsigned>(digit - 'a' + 10);
                    } else if (digit >= 'A' && digit <= 'F') {
                        value += static_cast<unsigned>(digit - 'A' + 10);
                    } else {
                        fail(diagnostic, "invalid-unicode", "Invalid Unicode escape");
                        return std::nullopt;
                    }
                }
                if (value > 0x7FU) {
                    if (value <= 0x7FFU) {
                        result.push_back(static_cast<char>(0xC0U | (value >> 6U)));
                        result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
                    } else {
                        result.push_back(static_cast<char>(0xE0U | (value >> 12U)));
                        result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
                        result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
                    }
                } else {
                    result.push_back(static_cast<char>(value));
                }
                break;
            }
            default:
                fail(diagnostic, "invalid-escape", "Unknown string escape");
                return std::nullopt;
            }
        }
        fail(diagnostic, "unexpected-end", "Unterminated string");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonValue> parseObject(ScriptDiagnostic& diagnostic) {
        static_cast<void>(consume('{'));
        JsonObject result;
        skipWhitespace();
        if (consume('}')) {
            return JsonValue{std::move(result)};
        }
        while (true) {
            skipWhitespace();
            auto key = parseString(diagnostic);
            if (!key.has_value()) {
                return std::nullopt;
            }
            skipWhitespace();
            if (!consume(':')) {
                fail(diagnostic, "invalid-object", "Expected ':' after object key");
                return std::nullopt;
            }
            auto value = parseValue(diagnostic);
            if (!value.has_value()) {
                return std::nullopt;
            }
            if (!result.emplace(std::move(*key), std::move(*value)).second) {
                fail(diagnostic, "duplicate-key", "Duplicate object key");
                return std::nullopt;
            }
            skipWhitespace();
            if (consume('}')) {
                return JsonValue{std::move(result)};
            }
            if (!consume(',')) {
                fail(diagnostic, "invalid-object", "Expected ',' or '}' in object");
                return std::nullopt;
            }
        }
    }

    [[nodiscard]] std::optional<JsonValue> parseArray(ScriptDiagnostic& diagnostic) {
        static_cast<void>(consume('['));
        JsonArray result;
        skipWhitespace();
        if (consume(']')) {
            return JsonValue{std::move(result)};
        }
        while (true) {
            auto value = parseValue(diagnostic);
            if (!value.has_value()) {
                return std::nullopt;
            }
            result.push_back(std::move(*value));
            skipWhitespace();
            if (consume(']')) {
                return JsonValue{std::move(result)};
            }
            if (!consume(',')) {
                fail(diagnostic, "invalid-array", "Expected ',' or ']' in array");
                return std::nullopt;
            }
        }
    }

    [[nodiscard]] std::optional<JsonValue> parseNumber(ScriptDiagnostic& diagnostic) {
        const auto start = offset_;
        if (offset_ < input_.size() && (input_[offset_] == '-' || input_[offset_] == '+')) {
            ++offset_;
        }
        while (offset_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[offset_])) != 0) {
            ++offset_;
        }
        bool floating = false;
        if (offset_ < input_.size() && input_[offset_] == '.') {
            floating = true;
            ++offset_;
            while (offset_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[offset_])) != 0) {
                ++offset_;
            }
        }
        if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            floating = true;
            ++offset_;
            if (offset_ < input_.size() && (input_[offset_] == '-' || input_[offset_] == '+')) {
                ++offset_;
            }
            while (offset_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[offset_])) != 0) {
                ++offset_;
            }
        }
        if (start == offset_) {
            fail(diagnostic, "invalid-number", "Expected a JSON number");
            return std::nullopt;
        }
        const auto token = input_.substr(start, offset_ - start);
        if (floating) {
            char* end = nullptr;
            const auto value = std::strtod(std::string(token).c_str(), &end);
            if (end == nullptr) {
                fail(diagnostic, "invalid-number", "Invalid JSON number");
                return std::nullopt;
            }
            return JsonValue{value};
        }
        std::int64_t value = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            fail(diagnostic, "invalid-number", "Integer is outside the supported range");
            return std::nullopt;
        }
        return JsonValue{value};
    }

    std::string_view input_;
    std::size_t offset_ = 0;
};

[[nodiscard]] std::optional<document::Vec2d> pointOf(const JsonValue& json) {
    const auto* array = std::get_if<JsonArray>(&json.storage);
    if (array == nullptr || array->size() != 2)
        return std::nullopt;
    std::array<double, 2> components{};
    for (std::size_t index = 0; index < 2; ++index) {
        const auto& item = (*array)[index].storage;
        if (const auto* real = std::get_if<double>(&item))
            components[index] = *real;
        else if (const auto* whole = std::get_if<std::int64_t>(&item))
            components[index] = static_cast<double>(*whole);
        else
            return std::nullopt;
        if (!std::isfinite(components[index]))
            return std::nullopt;
    }
    return document::Vec2d{components[0], components[1]};
}

// ADR 0028: a path object, the same shape the MCP projection publishes: {"closed": bool,
// "anchors": [{"point": [x, y], "inHandle": [x, y] | null, "outHandle": [x, y] | null,
// "mode": "corner" | "mirrored" | "aligned" | "free" | "auto"}]}; an omitted mode is inferred from
// the handles. The Python spelling (in_handle / out_handle) is accepted too.
[[nodiscard]] std::optional<document::PathContour> contourOf(const JsonObject& object) {
    const auto closed = object.find("closed");
    const auto anchors = object.find("anchors");
    if (object.size() != 2 || closed == object.end() || anchors == object.end())
        return std::nullopt;
    const auto* isClosed = std::get_if<bool>(&closed->second.storage);
    const auto* rows = std::get_if<JsonArray>(&anchors->second.storage);
    if (isClosed == nullptr || rows == nullptr || rows->size() > document::kMaximumPathAnchors)
        return std::nullopt;
    document::PathContour path;
    path.closed = *isClosed;
    for (const auto& row : *rows) {
        const auto* fields = std::get_if<JsonObject>(&row.storage);
        if (fields == nullptr)
            return std::nullopt;
        document::PathAnchor anchor;
        bool hasPoint = false;
        std::optional<document::PathAnchorMode> mode;
        for (const auto& [key, value] : *fields) {
            const bool none = std::holds_alternative<std::nullptr_t>(value.storage);
            if (key == "point") {
                const auto point = pointOf(value);
                if (!point)
                    return std::nullopt;
                anchor.point = *point;
                hasPoint = true;
            } else if (key == "inHandle" || key == "in_handle" || key == "outHandle" ||
                       key == "out_handle") {
                auto& handle = key.front() == 'i' ? anchor.inHandle : anchor.outHandle;
                if (none)
                    continue;
                const auto point = pointOf(value);
                if (!point)
                    return std::nullopt;
                handle = *point;
            } else if (key == "mode") {
                const auto* text = std::get_if<std::string>(&value.storage);
                mode = text ? document::pathAnchorModeFromName(*text) : std::nullopt;
                if (!mode)
                    return std::nullopt;
            } else {
                return std::nullopt;
            }
        }
        if (!hasPoint)
            return std::nullopt;
        anchor.mode = mode.value_or(document::inferPathAnchorMode(anchor));
        path.anchors.push_back(anchor);
    }
    return path;
}

[[nodiscard]] std::optional<double> pathNumberOf(const JsonValue& value) {
    if (const auto* number = std::get_if<double>(&value.storage))
        return *number;
    if (const auto* whole = std::get_if<std::int64_t>(&value.storage))
        return static_cast<double>(*whole);
    return std::nullopt;
}

// #589: one group of a compound path: {"key": text, "firstContour": n, "contourCount": n, "color":
// [r, g, b, a] (optional)}.
[[nodiscard]] std::optional<document::PathGroup> pathGroupOf(const JsonObject& object) {
    const auto key = object.find("key");
    const auto first = object.find("firstContour");
    const auto count = object.find("contourCount");
    const auto color = object.find("color");
    if (key == object.end() || first == object.end() || count == object.end() ||
        object.size() != (color == object.end() ? 3U : 4U))
        return std::nullopt;
    const auto index = [](const JsonValue& json) -> std::optional<std::uint32_t> {
        const auto number = pathNumberOf(json);
        if (!number || !(*number >= 0.0) || *number > 4294967295.0 ||
            *number != std::floor(*number))
            return std::nullopt;
        return static_cast<std::uint32_t>(*number);
    };
    const auto* text = std::get_if<std::string>(&key->second.storage);
    const auto firstContour = index(first->second);
    const auto contourCount = index(count->second);
    if (text == nullptr || !firstContour || !contourCount)
        return std::nullopt;
    document::PathGroup group{*text, *firstContour, *contourCount, std::nullopt};
    if (color != object.end()) {
        const auto* channels = std::get_if<JsonArray>(&color->second.storage);
        if (channels == nullptr || channels->size() != 4)
            return std::nullopt;
        std::array<double, 4> values{};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const auto number = pathNumberOf((*channels)[channel]);
            if (!number)
                return std::nullopt;
            values[channel] = *number;
        }
        group.color = core::Color4d{values[0], values[1], values[2], values[3]};
    }
    return group;
}

// #589: a path argument is one contour, or {"contours": [contour...], "fillRule": "nonZero" or
// "evenOdd" (optional), "groups": [group...] (optional)}.
[[nodiscard]] std::optional<Value> pathOf(const JsonObject& object) {
    const auto contours = object.find("contours");
    if (contours == object.end()) {
        auto contour = contourOf(object);
        return contour ? std::optional<Value>(Value{document::PathValue::fromContour(*contour)})
                       : std::nullopt;
    }
    const auto fillRule = object.find("fillRule");
    const auto groups = object.find("groups");
    const auto* rows = std::get_if<JsonArray>(&contours->second.storage);
    if (rows == nullptr || rows->size() > document::kMaximumPathContours ||
        object.size() != 1U + static_cast<std::size_t>(fillRule != object.end()) +
                             static_cast<std::size_t>(groups != object.end()))
        return std::nullopt;
    document::PathValue path;
    for (const auto& row : *rows) {
        const auto* fields = std::get_if<JsonObject>(&row.storage);
        auto contour = fields ? contourOf(*fields) : std::nullopt;
        if (!contour)
            return std::nullopt;
        path.contours.push_back(std::move(*contour));
    }
    if (fillRule != object.end()) {
        const auto* text = std::get_if<std::string>(&fillRule->second.storage);
        const auto rule = text ? document::pathFillRuleFromName(*text) : std::nullopt;
        if (!rule)
            return std::nullopt;
        path.fillRule = *rule;
    }
    if (groups != object.end()) {
        const auto* members = std::get_if<JsonArray>(&groups->second.storage);
        if (members == nullptr)
            return std::nullopt;
        for (const auto& member : *members) {
            const auto* fields = std::get_if<JsonObject>(&member.storage);
            auto group = fields ? pathGroupOf(*fields) : std::nullopt;
            if (!group)
                return std::nullopt;
            path.groups.push_back(std::move(*group));
        }
    }
    if (!path.isValid())
        return std::nullopt;
    return Value{std::move(path)};
}

// #541: a gradient object, {"stops": [{"position": p, "color": [r, g, b, a]}]}, the shape the
// parameter projection publishes, with (#546) an optional "interpolation", "colorMode" and
// "hueDirection" by name (Linear, RGB and Near when left out).
[[nodiscard]] std::optional<Value> gradientOf(const JsonObject& object) {
    const auto stops = object.find("stops");
    if (stops == object.end())
        return std::nullopt;
    document::GradientModes modes;
    std::size_t members = 1;
    const auto named = [&](const char* key, auto parse, auto& out) {
        const auto found = object.find(key);
        if (found == object.end())
            return true;
        ++members;
        const auto* text = std::get_if<std::string>(&found->second.storage);
        const auto value = text ? parse(*text) : std::nullopt;
        if (!value)
            return false;
        out = *value;
        return true;
    };
    if (!named("interpolation", document::gradientInterpolationFromName, modes.interpolation) ||
        !named("colorMode", document::gradientColorModeFromName, modes.colorMode) ||
        !named("hueDirection", document::gradientHueDirectionFromName, modes.hueDirection) ||
        object.size() != members)
        return std::nullopt;
    const auto* rows = std::get_if<JsonArray>(&stops->second.storage);
    if (rows == nullptr || rows->size() > document::kMaximumGradientStops)
        return std::nullopt;
    const auto numberOf = [](const JsonValue& value) -> std::optional<double> {
        if (const auto* real = std::get_if<double>(&value.storage))
            return *real;
        if (const auto* whole = std::get_if<std::int64_t>(&value.storage))
            return static_cast<double>(*whole);
        return std::nullopt;
    };
    document::GradientValue gradient;
    gradient.modes = modes;
    for (const auto& row : *rows) {
        const auto* fields = std::get_if<JsonObject>(&row.storage);
        if (fields == nullptr || fields->size() != 2)
            return std::nullopt;
        const auto position = fields->find("position");
        const auto color = fields->find("color");
        if (position == fields->end() || color == fields->end())
            return std::nullopt;
        const auto at = numberOf(position->second);
        const auto* channels = std::get_if<JsonArray>(&color->second.storage);
        if (!at || channels == nullptr || channels->size() != 4)
            return std::nullopt;
        std::array<double, 4> rgba{};
        for (std::size_t index = 0; index < 4; ++index) {
            const auto channel = numberOf((*channels)[index]);
            if (!channel)
                return std::nullopt;
            rgba[index] = *channel;
        }
        gradient.stops.push_back({*at, core::Color4d{rgba[0], rgba[1], rgba[2], rgba[3]}});
    }
    if (!gradient.isValid())
        return std::nullopt;
    return Value{std::move(gradient)};
}

// #574: {"name": "Shot 2"} (a layer) or {"layer": "Shot 2" or an id, "role": "opacity"} (a
// parameter), with an optional "composition". nullopt for any object with another member.
[[nodiscard]] std::optional<Value> referenceOf(const JsonObject& object) {
    if (object.empty())
        return std::nullopt;
    ObjectReference reference;
    for (const auto& [key, field] : object) {
        const auto* text = std::get_if<std::string>(&field.storage);
        const auto* whole = std::get_if<std::int64_t>(&field.storage);
        if (key == "name" && text != nullptr)
            reference.name = *text;
        else if (key == "role" && text != nullptr)
            reference.role = *text;
        else if (key == "layer" && text != nullptr)
            reference.layerName = *text;
        else if (key == "layer" && whole != nullptr && *whole > 0)
            reference.layerId = static_cast<std::uint64_t>(*whole);
        else if (key == "composition" && whole != nullptr && *whole > 0)
            reference.composition = static_cast<std::uint64_t>(*whole);
        else
            return std::nullopt;
    }
    return Value{std::move(reference)};
}

[[nodiscard]] std::optional<Value> convert(const JsonValue& json) {
    return std::visit(
        [](const auto& value) -> std::optional<Value> {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, JsonArray>) {
                ValueArray result;
                result.reserve(value.size());
                for (const auto& element : value) {
                    const auto converted = convert(element);
                    if (!converted.has_value()) {
                        return std::nullopt;
                    }
                    result.push_back(*converted);
                }
                return Value{std::move(result)};
            } else if constexpr (std::is_same_v<Type, JsonObject>) {
                if (value.contains("stops"))
                    return gradientOf(value);
                if (auto reference = referenceOf(value))
                    return reference;
                return pathOf(value);
            } else if constexpr (std::is_same_v<Type, std::nullptr_t>) {
                return std::nullopt;
            } else {
                return Value{value};
            }
        },
        json.storage);
}

} // namespace

ScriptParseResult parseScriptJson(const std::string_view json) {
    Parser parser(json);
    ScriptDiagnostic diagnostic;
    auto root = parser.parse(diagnostic);
    if (!root.has_value()) {
        ScriptParseResult result;
        result.diagnostic_ = std::move(diagnostic);
        return result;
    }
    const auto* array = std::get_if<JsonArray>(&root->storage);
    if (array == nullptr) {
        ScriptParseResult result;
        result.diagnostic_ = ScriptDiagnostic{.code = "root-not-array",
                                              .byteOffset = 0,
                                              .message = "Script root must be a JSON array"};
        return result;
    }

    ScriptParseResult result;
    for (std::size_t index = 0; index < array->size(); ++index) {
        const auto* object = std::get_if<JsonObject>(&(*array)[index].storage);
        if (object == nullptr) {
            result.diagnostic_ = ScriptDiagnostic{.code = "transaction-not-object",
                                                  .byteOffset = index,
                                                  .message = "Each script item "
                                                             "must be an object"};
            return result;
        }
        const auto operation = object->find("op");
        const auto args = object->find("args");
        if (operation == object->end() || args == object->end()) {
            result.diagnostic_ = ScriptDiagnostic{.code = "transaction-shape",
                                                  .byteOffset = index,
                                                  .message = "Each item requires 'op' and 'args'"};
            return result;
        }
        const auto* operationText = std::get_if<std::string>(&operation->second.storage);
        const auto* argsObject = std::get_if<JsonObject>(&args->second.storage);
        if (operationText == nullptr || argsObject == nullptr) {
            result.diagnostic_ =
                ScriptDiagnostic{.code = "transaction-types",
                                 .byteOffset = index,
                                 .message = "'op' must be a string and 'args' an object"};
            return result;
        }
        ScriptTransaction transaction{.operation = *operationText, .arguments = {}};
        for (const auto& [name, value] : *argsObject) {
            // null is the same as leaving the argument out, as in MCP and Python.
            if (std::holds_alternative<std::nullptr_t>(value.storage))
                continue;
            const auto converted = convert(value);
            if (!converted.has_value()) {
                result.diagnostic_ =
                    ScriptDiagnostic{.code = "unsupported-value",
                                     .byteOffset = index,
                                     .message = "Argument '" + name + "' is not a supported value"};
                return result;
            }
            transaction.arguments.emplace(name, *converted);
        }
        result.transactions_.push_back(std::move(transaction));
    }
    return result;
}

} // namespace bloom::scripting
