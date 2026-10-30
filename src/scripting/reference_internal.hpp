#pragma once

// Private to bloom_scripting: the pieces OperationReference is assembled from.

#include <bloom/scripting/operation_reference.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::scripting::detail {

// JSON text helpers shared by the reference renderers (compact, deterministic).
[[nodiscard]] std::string jsonQuote(std::string_view text);
[[nodiscard]] std::string valueJson(const Value& value);

// The errors every MCP tools/call can end in, before the tool's own.
[[nodiscard]] std::vector<ReferenceError> commonToolErrors();
// The errors every operation can end in inside `transact`, before its own.
[[nodiscard]] std::vector<ReferenceError> commonOperationErrors();

// Concepts, MCP tools, query kinds, bloom-cli commands and the Python entry points.
[[nodiscard]] std::vector<ReferenceEntry> surfaceEntries();

// A small builder for the surface tables.
struct ArgumentSpec final {
    std::string_view name;
    std::string_view type;
    bool required = false;
    std::string_view summary;
    std::string_view unit = {};
    std::string_view range = {};
    std::string_view defaultValue = {};
    std::initializer_list<std::pair<std::string_view, std::string_view>> choices = {};
};
[[nodiscard]] ReferenceArgument argument(const ArgumentSpec& spec);

} // namespace bloom::scripting::detail
