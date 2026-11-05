// OperationReference: the registry, the operation notes and the surface tables assembled into one
// reference, and the text, JSON and Markdown every surface prints from it.

#include "operation_docs.hpp"
#include "reference_internal.hpp"

#include <bloom/scripting/operation_reference.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <utility>

namespace bloom::scripting {
namespace {

constexpr std::array<std::string_view, 7> kContextNames{"",      "project",   "composition", "time",
                                                        "layer", "selection", "layers"};
constexpr std::array<std::string_view, 7> kContextDefaults{
    "",
    "the current project",
    "the current composition (the Viewer's in the desktop application, the first one "
    "headless)",
    "the playhead (frame 0 headless)",
    "the first selected layer",
    "the selected nodes",
    "the selected layers"};

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string firstSentence(std::string_view text) {
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        if (text[index] == '.' && text[index + 1] == ' ')
            return std::string(text.substr(0, index + 1));
    }
    return std::string(text);
}

std::string numberText(const double value) {
    if (std::floor(value) == value && std::fabs(value) < 1e15)
        return std::to_string(static_cast<std::int64_t>(value));
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), converted.ptr);
}

std::string rangeText(const ArgumentSchema& argument) {
    if (argument.minimum && argument.maximum)
        return numberText(*argument.minimum) + ".." + numberText(*argument.maximum);
    if (argument.minimum)
        return ">= " + numberText(*argument.minimum);
    if (argument.maximum)
        return "<= " + numberText(*argument.maximum);
    return {};
}

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> result;
    while (!text.empty()) {
        const auto space = text.find(' ');
        if (space != 0)
            result.emplace_back(text.substr(0, space));
        if (space == std::string_view::npos)
            break;
        text.remove_prefix(space + 1);
    }
    return result;
}

std::string argumentsJson(const OperationSchema& schema) {
    std::string text = "{";
    bool first = true;
    for (const auto& argument : schema.arguments) {
        if (!argument.inExample || argument.contextual() || !argument.example.has_value())
            continue;
        text += (first ? "" : ",") + detail::jsonQuote(argument.name) + ":" +
                detail::valueJson(*argument.example);
        first = false;
    }
    return text + "}";
}

ReferenceEntry operationEntry(const OperationDescriptor& descriptor) {
    const auto& schema = descriptor.schema;
    const auto* notes = detail::operationNotes(schema.typeId);
    ReferenceEntry entry;
    entry.name = schema.typeId;
    entry.kind = ReferenceKind::Operation;
    const auto first = schema.typeId.find('.');
    const auto second = schema.typeId.find('.', first + 1);
    entry.area = first == std::string::npos || second == std::string::npos
                     ? std::string("operations")
                     : schema.typeId.substr(first + 1, second - first - 1);
    entry.description = schema.description;
    entry.summary = entry.description.empty() ? "Runs " + schema.typeId + "."
                                              : firstSentence(entry.description);
    for (const auto& argument : schema.arguments) {
        ReferenceArgument value;
        value.name = argument.name;
        value.type = std::string(valueKindName(argument.kind));
        value.required = argument.required && !argument.contextual();
        value.summary = argument.summary;
        value.unit = argument.unit;
        value.range = rangeText(argument);
        const auto context = static_cast<std::size_t>(argument.context);
        if (argument.contextual()) {
            value.context = std::string(kContextNames.at(context));
            value.defaultValue = std::string(kContextDefaults.at(context));
        } else {
            value.defaultValue = argument.defaultText;
        }
        for (const auto& choice : argument.choices)
            value.choices.push_back(
                {.value = detail::valueJson(choice.value), .label = choice.label});
        entry.arguments.push_back(std::move(value));
    }
    entry.effects = {.undoable = true, .dirtiesProject = true};
    entry.prerequisites = {"The current revision, from any query result, as transact's "
                           "expectedRevision."};
    for (const auto& argument : schema.arguments) {
        if (argument.kind == ValueKind::Id && !argument.contextual() && argument.required) {
            entry.prerequisites.push_back(
                "Existing ids for the id arguments; each argument's unit names the query that "
                "lists them.");
            break;
        }
    }
    const auto outputs = notes != nullptr ? notes->outputs : std::string_view{};
    entry.result = "The transact result {succeeded, revision, diagnostics, outputs}. ";
    entry.result +=
        outputs.empty()
            ? std::string("This operation creates no ids, so outputs is empty.")
            : "outputs names: " + std::string(outputs) + " (each {operationIndex, name, id}).";
    if (notes != nullptr && !notes->errors.empty()) {
        auto rest = notes->errors;
        while (!rest.empty()) {
            const auto end = rest.find('\n');
            auto line = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            std::string code = "bloom.scripting.command-rejected";
            for (const std::string_view tag : {"invalid-argument", "operation-warning"}) {
                const auto prefix = "[" + std::string(tag) + "] ";
                if (line.starts_with(prefix)) {
                    code = "bloom.scripting." + std::string(tag);
                    line.remove_prefix(prefix.size());
                }
            }
            const auto arrow = line.find(" => ");
            entry.errors.push_back({.code = code,
                                    .when = std::string(line.substr(0, arrow)),
                                    .remedy = arrow == std::string_view::npos
                                                  ? std::string()
                                                  : std::string(line.substr(arrow + 4)),
                                    .retryable = false});
        }
    }
    for (auto& common : detail::commonOperationErrors())
        entry.errors.push_back(std::move(common));
    const auto args = argumentsJson(schema);
    entry.examples.push_back(
        {.surface = "mcp",
         .text = R"({"name":"transact","arguments":{"expectedRevision":0,"operations":[{"op":)" +
                 detail::jsonQuote(schema.typeId) + R"(,"args":)" + args + "}]}}"});
    entry.examples.push_back({.surface = "python", .text = exampleCall(schema)});
    entry.examples.push_back({.surface = "cli",
                              .text = R"(bloom-cli run edits.json   # [{"op":)" +
                                      detail::jsonQuote(schema.typeId) + R"(,"args":)" + args +
                                      "}]"});
    if (notes != nullptr)
        entry.related = words(notes->related);
    if (std::ranges::find(entry.related, "transact") == entry.related.end())
        entry.related.emplace_back("transact");
    return entry;
}

int kindOrder(const ReferenceKind kind) {
    switch (kind) {
    case ReferenceKind::Concept:
        return 0;
    case ReferenceKind::Tool:
        return 1;
    case ReferenceKind::QueryKind:
        return 2;
    case ReferenceKind::Operation:
        return 3;
    case ReferenceKind::CliCommand:
        return 4;
    case ReferenceKind::Python:
        return 5;
    }
    return 6;
}

std::string_view kindHeading(const ReferenceKind kind) {
    switch (kind) {
    case ReferenceKind::Concept:
        return "Concepts (read these first)";
    case ReferenceKind::Tool:
        return "MCP tools";
    case ReferenceKind::QueryKind:
        return "Query kinds: query {kind, ...}";
    case ReferenceKind::Operation:
        return "Operations: transact {op, args}, bloom.ops.<domain>.<verb>(...), bloom-cli run";
    case ReferenceKind::CliCommand:
        return "bloom-cli commands";
    case ReferenceKind::Python:
        return "Python";
    }
    return "Other";
}

std::size_t editDistance(std::string_view left, std::string_view right) {
    std::vector<std::size_t> row(right.size() + 1);
    for (std::size_t j = 0; j <= right.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1; i <= left.size(); ++i) {
        auto previous = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= right.size(); ++j) {
            const auto saved = row[j];
            row[j] = std::min(
                {row[j] + 1, row[j - 1] + 1, previous + (left[i - 1] == right[j - 1] ? 0U : 1U)});
            previous = saved;
        }
    }
    return row[right.size()];
}

void appendWrapped(std::string& text, std::string_view indent, std::string_view body) {
    if (body.empty())
        return;
    constexpr std::size_t width = 96;
    std::string line(indent);
    for (const auto& word : words(body)) {
        if (line.size() > indent.size() && line.size() + 1 + word.size() > width) {
            text += line + "\n";
            line = std::string(indent);
        }
        line += (line.size() > indent.size() ? " " : "") + word;
    }
    text += line + "\n";
}

} // namespace

namespace detail {

std::string jsonQuote(const std::string_view text) {
    std::string result = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                constexpr std::string_view hex = "0123456789abcdef";
                result += "\\u00";
                result.push_back(hex[byte >> 4U]);
                result.push_back(hex[byte & 0xFU]);
            } else {
                result.push_back(c);
            }
        }
    }
    return result + "\"";
}

std::string valueJson(const Value& value) {
    return std::visit(
        [](const auto& held) -> std::string {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, bool>) {
                return held ? "true" : "false";
            } else if constexpr (std::is_same_v<Held, std::int64_t>) {
                return std::to_string(held);
            } else if constexpr (std::is_same_v<Held, double>) {
                return numberText(held);
            } else if constexpr (std::is_same_v<Held, std::string>) {
                return jsonQuote(held);
            } else if constexpr (std::is_same_v<Held, StableId>) {
                return std::to_string(held.value);
            } else if constexpr (std::is_same_v<Held, document::Vec2d>) {
                return "[" + numberText(held.x) + "," + numberText(held.y) + "]";
            } else if constexpr (std::is_same_v<Held, document::Vec3d>) {
                return "[" + numberText(held.x) + "," + numberText(held.y) + "," +
                       numberText(held.z) + "]";
            } else if constexpr (std::is_same_v<Held, core::Color4d>) {
                return "[" + numberText(held.red) + "," + numberText(held.green) + "," +
                       numberText(held.blue) + "," + numberText(held.alpha) + "]";
            } else if constexpr (std::is_same_v<Held, document::PathValue>) {
                const auto point = [](const document::Vec2d& p) {
                    return "[" + numberText(p.x) + "," + numberText(p.y) + "]";
                };
                std::string text = std::string("{\"closed\":") + (held.closed ? "true" : "false") +
                                   ",\"anchors\":[";
                for (std::size_t index = 0; index < held.anchors.size(); ++index) {
                    const auto& anchor = held.anchors[index];
                    text += (index == 0 ? "" : ",") + std::string("{\"point\":") +
                            point(anchor.point) + ",\"inHandle\":" +
                            (anchor.inHandle ? point(*anchor.inHandle) : std::string("null")) +
                            ",\"outHandle\":" +
                            (anchor.outHandle ? point(*anchor.outHandle) : std::string("null")) +
                            "}";
                }
                return text + "]}";
            } else if constexpr (std::is_same_v<Held, document::GradientValue>) {
                std::string text = "{\"stops\":[";
                for (std::size_t index = 0; index < held.stops.size(); ++index) {
                    const auto& stop = held.stops[index];
                    text += (index == 0 ? "" : ",") + std::string("{\"position\":") +
                            numberText(stop.position) + ",\"color\":[" +
                            numberText(stop.color.red) + "," + numberText(stop.color.green) + "," +
                            numberText(stop.color.blue) + "," + numberText(stop.color.alpha) +
                            "]}";
                }
                // #546: each mode that is not its default.
                const auto& modes = held.modes;
                text += "]";
                if (modes.interpolation != document::GradientInterpolation::Linear)
                    text += ",\"interpolation\":\"" +
                            std::string(document::gradientInterpolationName(modes.interpolation)) +
                            "\"";
                if (modes.colorMode != document::GradientColorMode::Rgb)
                    text += ",\"colorMode\":\"" +
                            std::string(document::gradientColorModeName(modes.colorMode)) + "\"";
                if (modes.hueDirection != document::GradientHueDirection::Near)
                    text += ",\"hueDirection\":\"" +
                            std::string(document::gradientHueDirectionName(modes.hueDirection)) +
                            "\"";
                return text + "}";
            } else {
                std::string text = "[";
                for (std::size_t index = 0; index < held.size(); ++index)
                    text += (index == 0 ? "" : ",") + valueJson(held[index]);
                return text + "]";
            }
        },
        value.storage);
}

} // namespace detail

std::string_view referenceKindName(const ReferenceKind kind) noexcept {
    switch (kind) {
    case ReferenceKind::Concept:
        return "concept";
    case ReferenceKind::Tool:
        return "tool";
    case ReferenceKind::QueryKind:
        return "query-kind";
    case ReferenceKind::Operation:
        return "operation";
    case ReferenceKind::CliCommand:
        return "cli-command";
    case ReferenceKind::Python:
        return "python";
    }
    return "concept";
}

OperationReference OperationReference::build(const OperationRegistry& registry) {
    OperationReference reference;
    reference.entries_ = detail::surfaceEntries();
    // Every tool call can also end in the server's own refusals (concept:errors).
    for (auto& entry : reference.entries_) {
        if (entry.kind != ReferenceKind::Tool || entry.name == "help")
            continue;
        for (auto& common : detail::commonToolErrors()) {
            if (entry.effects.readOnly && common.code == "bloom.mcp.read-only")
                continue;
            entry.errors.push_back(std::move(common));
        }
    }
    for (const auto& descriptor : registry.descriptors())
        reference.entries_.push_back(operationEntry(descriptor));
    std::ranges::stable_sort(reference.entries_, {},
                             [](const ReferenceEntry& entry) { return kindOrder(entry.kind); });
    return reference;
}

const OperationReference& OperationReference::builtIn() {
    static const OperationReference reference = build(OperationRegistry::builtIn());
    return reference;
}

const ReferenceEntry* OperationReference::find(const std::string_view raw) const {
    std::string name = lower(raw);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.pop_back();
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
        name.erase(name.begin());
    if (name.empty())
        return nullptr;
    const auto exact = [this](std::string_view candidate) -> const ReferenceEntry* {
        const auto found = std::ranges::find_if(
            entries_, [candidate](const auto& entry) { return lower(entry.name) == candidate; });
        return found == entries_.end() ? nullptr : &*found;
    };
    if (const auto* entry = exact(name))
        return entry;
    std::vector<std::string> candidates;
    // Python spellings: bloom.ops.layer.add_solid, layer.add_solid, import_.
    auto dotted = name;
    if (dotted.starts_with("bloom.ops."))
        dotted = "bloom." + dotted.substr(10);
    if (dotted.ends_with("_"))
        dotted.pop_back();
    std::ranges::replace(dotted, '_', '-');
    candidates.push_back(dotted);
    candidates.push_back("bloom." + dotted);
    // Query kinds: "query kind=layers", "query layers", "query.layers", "kind=layers".
    auto query = name;
    for (const std::string_view prefix : {"query kind=", "query:", "query ", "query.", "kind="}) {
        if (query.starts_with(prefix)) {
            query = query.substr(prefix.size());
            break;
        }
    }
    candidates.push_back("query:" + query);
    // bloom-cli commands: "bloom-cli render", "cli render", "cli:render".
    auto command = name;
    for (const std::string_view prefix : {"bloom-cli ", "cli:", "cli "}) {
        if (command.starts_with(prefix)) {
            command = command.substr(prefix.size());
            break;
        }
    }
    candidates.push_back("bloom-cli " + command);
    candidates.push_back("concept:" + (name.starts_with("concept ") ? name.substr(8) : name));
    for (const auto& candidate : candidates) {
        if (const auto* entry = exact(candidate))
            return entry;
    }
    return nullptr;
}

std::vector<std::string> OperationReference::suggest(const std::string_view raw,
                                                     const std::size_t limit) const {
    const auto name = lower(raw);
    std::vector<std::pair<std::size_t, std::string>> scored;
    for (const auto& entry : entries_) {
        const auto candidate = lower(entry.name);
        auto score = editDistance(name, candidate);
        // The verb alone ("add-solid") should find its operation.
        const auto dot = candidate.rfind('.');
        if (dot != std::string::npos)
            score = std::min(score, editDistance(name, candidate.substr(dot + 1)) + 1);
        if (!name.empty() && candidate.find(name) != std::string::npos)
            score = std::min<std::size_t>(score, 1);
        scored.emplace_back(score, entry.name);
    }
    std::ranges::stable_sort(scored, {}, &std::pair<std::size_t, std::string>::first);
    std::vector<std::string> result;
    for (const auto& [score, entryName] : scored) {
        if (result.size() == limit || score > std::max<std::size_t>(2, name.size() / 3))
            break;
        result.push_back(entryName);
    }
    return result;
}

std::string OperationReference::indexText() const {
    std::string text =
        "Bloom reference: " + std::to_string(entries_.size()) +
        " entries. Show one in full with help <name> (MCP: help {\"name\": ...}; Python: "
        "bloom.help(name); CLI: bloom-cli help <name>).\n";
    std::optional<ReferenceKind> kind;
    std::string area;
    std::size_t width = 0;
    for (const auto& entry : entries_)
        width = std::max(width, entry.name.size());
    width = std::min<std::size_t>(width, 56);
    for (const auto& entry : entries_) {
        if (entry.kind != kind) {
            kind = entry.kind;
            area.clear();
            text += "\n" + std::string(kindHeading(entry.kind)) + "\n";
        }
        if (entry.kind == ReferenceKind::Operation && entry.area != area) {
            area = entry.area;
            text += "  " + area + "\n";
        }
        const std::string indent = entry.kind == ReferenceKind::Operation ? "    " : "  ";
        auto name = entry.name;
        if (name.size() < width)
            name.append(width - name.size(), ' ');
        text += indent + name + "  " + entry.summary + "\n";
    }
    return text;
}

std::string OperationReference::indexJson() const {
    std::string text = "{\"count\":" + std::to_string(entries_.size()) + ",\"entries\":[";
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto& entry = entries_[index];
        text += (index == 0 ? "" : ",") + std::string("{\"name\":") +
                detail::jsonQuote(entry.name) +
                ",\"kind\":" + detail::jsonQuote(referenceKindName(entry.kind)) +
                ",\"area\":" + detail::jsonQuote(entry.area) +
                ",\"summary\":" + detail::jsonQuote(entry.summary) + "}";
    }
    return text + "]}";
}

std::string OperationReference::markdown() const {
    std::string text = "# Bloom agent reference\n\nEvery MCP tool, query kind, operation, "
                       "bloom-cli command and Python entry point, generated from the running "
                       "build. The MCP `help` tool, `bloom-cli help` and `bloom.help()` print "
                       "the same entries.\n";
    std::optional<ReferenceKind> kind;
    for (const auto& entry : entries_) {
        if (entry.kind != kind) {
            kind = entry.kind;
            text += "\n## " + std::string(kindHeading(entry.kind)) + "\n";
        }
        text += "\n### `" + entry.name + "`\n\n```text\n" + referenceText(entry) + "```\n";
    }
    return text;
}

std::string referenceText(const ReferenceEntry& entry) {
    std::string text =
        entry.name + "  (" + std::string(referenceKindName(entry.kind)) + ", " + entry.area + ")\n";
    appendWrapped(text, "", entry.description.empty() ? entry.summary : entry.description);
    if (!entry.arguments.empty()) {
        text += "\nArguments:\n";
        for (const auto& argument : entry.arguments) {
            std::string head = "  " + argument.name + "  " + argument.type + "  ";
            if (!argument.context.empty())
                head += "optional, defaults to " + argument.defaultValue + " (context " +
                        argument.context + ")";
            else if (argument.required)
                head += "required";
            else if (!argument.defaultValue.empty())
                head += "optional, default " + argument.defaultValue;
            else
                head += "optional";
            text += head + "\n";
            appendWrapped(text, "      ", argument.summary);
            if (!argument.unit.empty())
                appendWrapped(text, "      ", "Unit: " + argument.unit);
            if (!argument.range.empty())
                appendWrapped(text, "      ", "Range: " + argument.range);
            if (!argument.choices.empty()) {
                text += "      Values:\n";
                for (const auto& choice : argument.choices)
                    text += "        " + choice.value + "  " + choice.label + "\n";
            }
        }
    }
    std::string effects;
    const auto add = [&effects](const bool flag, std::string_view label) {
        if (flag)
            effects += (effects.empty() ? "" : "; ") + std::string(label);
    };
    add(entry.effects.readOnly, "read-only: changes nothing in the project");
    add(entry.effects.undoable, "undoable: one undo step per transact call");
    add(entry.effects.dirtiesProject, "marks the project unsaved");
    add(entry.effects.writesFiles, "writes files outside the project");
    add(entry.effects.liveOnly, "desktop application only");
    if (!effects.empty())
        appendWrapped(text, "", "\nEffects: " + effects + ".");
    if (!entry.prerequisites.empty()) {
        text += "\nPrerequisites:\n";
        for (const auto& prerequisite : entry.prerequisites)
            appendWrapped(text, "  ", "- " + prerequisite);
    }
    if (!entry.result.empty()) {
        text += "\nResult:\n";
        appendWrapped(text, "  ", entry.result);
    }
    if (!entry.errors.empty()) {
        text += "\nErrors:\n";
        for (const auto& error : entry.errors) {
            text += "  " + error.code + (error.retryable ? "  (retry)" : "") + "\n";
            appendWrapped(text, "      ", "When: " + error.when);
            if (!error.remedy.empty())
                appendWrapped(text, "      ", "Do: " + error.remedy);
        }
    }
    if (!entry.examples.empty()) {
        text += "\nExamples:\n";
        for (const auto& example : entry.examples) {
            const auto label = example.surface == "mcp"      ? std::string("MCP")
                               : example.surface == "python" ? std::string("Python")
                                                             : std::string("CLI");
            text += "  " + label + ":\n";
            auto rest = std::string_view(example.text);
            while (!rest.empty()) {
                const auto end = rest.find('\n');
                text += "    " + std::string(rest.substr(0, end)) + "\n";
                rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            }
        }
    }
    if (!entry.related.empty()) {
        std::string list;
        for (const auto& name : entry.related)
            list += (list.empty() ? "" : ", ") + name;
        appendWrapped(text, "", "\nRelated: " + list);
    }
    return text;
}

std::string referenceJson(const ReferenceEntry& entry) {
    using detail::jsonQuote;
    const auto strings = [](const std::vector<std::string>& list) {
        std::string text = "[";
        for (std::size_t index = 0; index < list.size(); ++index)
            text += (index == 0 ? "" : ",") + jsonQuote(list[index]);
        return text + "]";
    };
    const auto boolean = [](const bool flag) { return flag ? "true" : "false"; };
    std::string text = "{\"name\":" + jsonQuote(entry.name) +
                       ",\"kind\":" + jsonQuote(referenceKindName(entry.kind)) +
                       ",\"area\":" + jsonQuote(entry.area) +
                       ",\"summary\":" + jsonQuote(entry.summary) +
                       ",\"description\":" + jsonQuote(entry.description) + ",\"arguments\":[";
    for (std::size_t index = 0; index < entry.arguments.size(); ++index) {
        const auto& argument = entry.arguments[index];
        text += (index == 0 ? "" : ",") + std::string("{\"name\":") + jsonQuote(argument.name) +
                ",\"type\":" + jsonQuote(argument.type) +
                ",\"required\":" + boolean(argument.required) +
                ",\"unit\":" + jsonQuote(argument.unit) +
                ",\"range\":" + jsonQuote(argument.range) +
                ",\"default\":" + jsonQuote(argument.defaultValue) +
                ",\"context\":" + jsonQuote(argument.context) +
                ",\"summary\":" + jsonQuote(argument.summary) + ",\"choices\":[";
        for (std::size_t choice = 0; choice < argument.choices.size(); ++choice)
            text += (choice == 0 ? "" : ",") + std::string("{\"value\":") +
                    argument.choices[choice].value +
                    ",\"label\":" + jsonQuote(argument.choices[choice].label) + "}";
        text += "]}";
    }
    text += "],\"effects\":{\"readOnly\":" + std::string(boolean(entry.effects.readOnly)) +
            ",\"undoable\":" + boolean(entry.effects.undoable) +
            ",\"dirtiesProject\":" + boolean(entry.effects.dirtiesProject) +
            ",\"writesFiles\":" + boolean(entry.effects.writesFiles) +
            ",\"liveOnly\":" + boolean(entry.effects.liveOnly) + "}";
    text += ",\"prerequisites\":" + strings(entry.prerequisites);
    text += ",\"result\":" + jsonQuote(entry.result) + ",\"errors\":[";
    for (std::size_t index = 0; index < entry.errors.size(); ++index) {
        const auto& error = entry.errors[index];
        text += (index == 0 ? "" : ",") + std::string("{\"code\":") + jsonQuote(error.code) +
                ",\"when\":" + jsonQuote(error.when) + ",\"remedy\":" + jsonQuote(error.remedy) +
                ",\"retryable\":" + boolean(error.retryable) + "}";
    }
    text += "],\"examples\":[";
    for (std::size_t index = 0; index < entry.examples.size(); ++index)
        text += (index == 0 ? "" : ",") + std::string("{\"surface\":") +
                jsonQuote(entry.examples[index].surface) +
                ",\"text\":" + jsonQuote(entry.examples[index].text) + "}";
    return text + "],\"related\":" + strings(entry.related) + "}";
}

ReferenceAnswer referenceHelp(const std::string_view name) {
    const auto& reference = OperationReference::builtIn();
    ReferenceAnswer answer;
    if (name.empty()) {
        answer.found = true;
        answer.text = reference.indexText();
        answer.json = "{\"found\":true,\"name\":\"\",\"index\":" + reference.indexJson() +
                      ",\"text\":" + detail::jsonQuote(answer.text) + "}";
        return answer;
    }
    if (const auto* entry = reference.find(name)) {
        answer.found = true;
        answer.text = referenceText(*entry);
        answer.json = "{\"found\":true,\"name\":" + detail::jsonQuote(entry->name) +
                      ",\"entry\":" + referenceJson(*entry) +
                      ",\"text\":" + detail::jsonQuote(answer.text) + "}";
        return answer;
    }
    const auto suggestions = reference.suggest(name);
    answer.text = "No reference entry is named \"" + std::string(name) + "\".";
    if (!suggestions.empty()) {
        answer.text += " Closest:";
        for (const auto& suggestion : suggestions)
            answer.text += " " + suggestion;
        answer.text += ".";
    }
    answer.text += " help with no name lists every entry.\n";
    std::string list = "[";
    for (std::size_t index = 0; index < suggestions.size(); ++index)
        list += (index == 0 ? "" : ",") + detail::jsonQuote(suggestions[index]);
    answer.json = "{\"found\":false,\"name\":" + detail::jsonQuote(name) +
                  ",\"suggestions\":" + list + "],\"text\":" + detail::jsonQuote(answer.text) + "}";
    return answer;
}

} // namespace bloom::scripting
