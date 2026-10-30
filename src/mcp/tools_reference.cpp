// The self-describing half of the MCP surface: the `help` tool, tools/list descriptions generated
// from the reference, the server instructions, and the resources (MCP 2025-11-25 resources/list
// and resources/read). Every text comes from scripting::OperationReference, so MCP, bloom-cli
// and Python print the same entries.
#include "core_internal.hpp"
#include "embedded_guides.hpp"

#include <bloom/scripting/node_reference.hpp>
#include <bloom/scripting/operation_reference.hpp>

#include <array>
#include <functional>

namespace bloom::mcp {
namespace {

struct Resource final {
    std::string_view uri;
    std::string_view name;
    std::string_view title;
    std::string_view description;
    std::string_view mimeType;
    std::function<std::string()> text;
};

std::string embeddedGuide(const std::string_view name) {
    for (const auto& guide : embeddedGuides())
        if (guide.name == name)
            return std::string(guide.text);
    return {};
}

const std::vector<Resource>& resources() {
    static const std::vector<Resource> list{
        {"bloom://guide/agents", "agents", "Getting started for agents",
         "Five steps with exact calls: find the active composition, add a layer, animate it, "
         "render a frame to check, save.",
         "text/markdown", [] { return scripting::agentGuideMarkdown(); }},
        {"bloom://reference/operations", "operations", "Tool, query and operation reference",
         "Every MCP tool, query kind, operation, bloom-cli command and Python entry point with "
         "arguments, units, accepted values, results, errors and examples (the help tool's "
         "entries in one document).",
         "text/markdown", [] { return scripting::OperationReference::builtIn().markdown(); }},
        {"bloom://reference/nodes", "nodes", "Node and parameter reference",
         "Every node type with its ports and parameters: value kinds, defaults, units, ranges, "
         "blend modes and other enumerations.",
         "text/markdown", [] { return scripting::nodeReferenceMarkdown(); }},
        {"bloom://reference/index.json", "index", "Reference index",
         "Every reference entry's name, kind, area and one-line summary, as JSON.",
         "application/json", [] { return scripting::OperationReference::builtIn().indexJson(); }},
        {"bloom://guide/mcp", "mcp-guide", "Using Bloom from an agent (MCP)",
         "The user guide: the in-app server, bloom-mcp on stdio, tools, limits and errors.",
         "text/markdown", [] { return embeddedGuide("mcp"); }},
        {"bloom://guide/scripting", "scripting-guide", "Scripting Bloom",
         "The user guide for bloom-cli and JSON operation scripts.", "text/markdown",
         [] { return embeddedGuide("scripting"); }},
        {"bloom://guide/python", "python-guide", "Scripting Bloom with Python",
         "The user guide for the bloom Python package.", "text/markdown",
         [] { return embeddedGuide("python"); }},
    };
    return list;
}

std::string toolDescription(const scripting::ReferenceEntry& entry) {
    std::string text = entry.summary;
    if (!entry.description.empty() && entry.description != entry.summary)
        text += " " + entry.description;
    return text + " help {\"name\":\"" + entry.name +
           "\"} gives the full reference: arguments, result, errors and examples.";
}

std::string argumentDescription(const scripting::ReferenceArgument& argument) {
    std::string text = argument.summary;
    if (!argument.unit.empty())
        text += " Unit: " + argument.unit + ".";
    if (!argument.range.empty())
        text += " Range: " + argument.range + ".";
    if (!argument.defaultValue.empty())
        text += " Default: " + argument.defaultValue + ".";
    if (!argument.choices.empty()) {
        text += " Values:";
        for (std::size_t index = 0; index < argument.choices.size(); ++index)
            text += (index == 0 ? " " : ", ") + argument.choices[index].value + " (" +
                    argument.choices[index].label + ")";
        text += ".";
    }
    return text;
}

void put(Json& out, yyjson_mut_val* object, const std::string_view key, yyjson_mut_val* value) {
    if (!yyjson_mut_obj_put(object, out.text(key), value))
        throw std::bad_alloc();
}

} // namespace

yyjson_mut_val* describedToolList(Json& out, yyjson_val* schemas) {
    const auto& reference = scripting::OperationReference::builtIn();
    auto* result = out.copy(schemas);
    auto* tools = yyjson_mut_obj_get(result, "tools");
    yyjson_mut_arr_iter iterator = yyjson_mut_arr_iter_with(tools);
    while (auto* tool = yyjson_mut_arr_iter_next(&iterator)) {
        auto* name = yyjson_mut_obj_get(tool, "name");
        const std::string_view toolName(yyjson_mut_get_str(name), yyjson_mut_get_len(name));
        const auto* entry = reference.find(toolName);
        if (entry == nullptr || entry->kind != scripting::ReferenceKind::Tool)
            continue;
        put(out, tool, "description", out.text(toolDescription(*entry)));
        auto* properties =
            yyjson_mut_obj_get(yyjson_mut_obj_get(tool, "inputSchema"), "properties");
        yyjson_mut_obj_iter fields = yyjson_mut_obj_iter_with(properties);
        while (auto* key = yyjson_mut_obj_iter_next(&fields)) {
            const std::string_view property(yyjson_mut_get_str(key), yyjson_mut_get_len(key));
            const auto argument =
                std::ranges::find(entry->arguments, property, &scripting::ReferenceArgument::name);
            if (argument != entry->arguments.end())
                put(out, yyjson_mut_obj_iter_get_val(key), "description",
                    out.text(argumentDescription(*argument)));
        }
    }
    return result;
}

std::string unknownOperation(const std::string_view id) {
    std::string text = "Unknown operation ID " + std::string(id) + ".";
    const auto suggestions = scripting::OperationReference::builtIn().suggest(id, 3);
    if (!suggestions.empty()) {
        text += " Closest:";
        for (const auto& name : suggestions)
            text += " " + name;
        text += ".";
    }
    return text + " help with no name lists every operation.";
}

std::string argumentHelp(const std::string_view operation) {
    return ". help {\"name\":\"" + std::string(operation) + "\"} shows every argument.";
}

std::string_view serverInstructions() noexcept { return scripting::agentInstructions(); }

yyjson_mut_val* helpTool(McpServerCore::State&, Json& out, McpCall& call, yyjson_val* arguments) {
    members(arguments, {"name"});
    const auto name =
        member(arguments, "name") ? string(member(arguments, "name"), 256) : std::string();
    call.summary = name;
    const auto answer = scripting::referenceHelp(name);
    Parsed parsed(answer.json);
    return out.copy(parsed.root());
}

yyjson_mut_val* resourceList(Json& out) {
    auto* result = out.object();
    auto* list = out.array();
    for (const auto& resource : resources()) {
        auto* row = out.object();
        out.set(row, "uri", out.text(resource.uri));
        out.set(row, "name", out.text(resource.name));
        out.set(row, "title", out.text(resource.title));
        out.set(row, "description", out.text(resource.description));
        out.set(row, "mimeType", out.text(resource.mimeType));
        out.append(list, row);
    }
    out.set(result, "resources", list);
    return result;
}

yyjson_mut_val* resourceRead(Json& out, yyjson_val* params) {
    members(params, {"uri", "_meta"}, {"uri"});
    const auto uri = string(member(params, "uri"), 256);
    const auto& list = resources();
    const auto found = std::ranges::find(list, uri, &Resource::uri);
    if (found == list.end()) {
        std::string known;
        for (const auto& resource : list)
            known += (known.empty() ? "" : ", ") + std::string(resource.uri);
        throw InvalidInput("Unknown resource " + uri + ". Resources: " + known);
    }
    auto* result = out.object();
    auto* contents = out.array();
    auto* item = out.object();
    out.set(item, "uri", out.text(found->uri));
    out.set(item, "mimeType", out.text(found->mimeType));
    out.set(item, "text", out.text(found->text()));
    out.append(contents, item);
    out.set(result, "contents", contents);
    return result;
}

} // namespace bloom::mcp
