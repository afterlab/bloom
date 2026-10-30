#pragma once

// The one reference of Bloom's agent surfaces: every MCP tool, every `query` kind, every
// registered operation, every bloom-cli command and the Python entry points, with their arguments
// (type, unit, range, default, context and closed values), side effects, prerequisites, result
// shape, typed errors with remedies, pasteable examples and related entries. It is generated from
// the operation registry and the surface tables beside it, so the MCP `help` tool, `bloom-cli help`
// and `bloom.help()` print the same entry. Qt-free.

#include <bloom/scripting/operation_registry.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::scripting {

enum class ReferenceKind : std::uint8_t {
    // A rule every surface shares: ids, time, revisions, context, errors.
    Concept,
    Tool,
    QueryKind,
    Operation,
    CliCommand,
    Python,
};

[[nodiscard]] std::string_view referenceKindName(ReferenceKind kind) noexcept;

struct ReferenceChoice final {
    // The value as JSON text: "\"ease-in-out\"" or "2".
    std::string value;
    std::string label;
};

struct ReferenceArgument final {
    std::string name;
    // The kind name (valueKindName for operations; integer, string, boolean, array for tools).
    std::string type;
    std::string unit;
    // "1..64", ">= 0" or empty.
    std::string range;
    // What an omitted optional argument means; empty for a required one.
    std::string defaultValue;
    // The live-session value that fills it when omitted ("composition", "time", ...), or empty.
    std::string context;
    std::string summary;
    bool required = false;
    std::vector<ReferenceChoice> choices;
};

struct ReferenceEffects final {
    bool readOnly = false;
    // A successful call adds one step to the project's undo history.
    bool undoable = false;
    // A successful call changes the project (it becomes unsaved).
    bool dirtiesProject = false;
    // Only the desktop application's in-app server can honour it.
    bool liveOnly = false;
    // A successful call writes files outside the project document.
    bool writesFiles = false;
};

struct ReferenceError final {
    std::string code;
    std::string when;
    std::string remedy;
    bool retryable = false;
};

struct ReferenceExample final {
    // "mcp", "python" or "cli".
    std::string surface;
    std::string text;
};

struct ReferenceEntry final {
    std::string name;
    ReferenceKind kind = ReferenceKind::Concept;
    std::string area;
    std::string summary;
    std::string description;
    std::vector<ReferenceArgument> arguments;
    ReferenceEffects effects;
    std::vector<std::string> prerequisites;
    std::string result;
    std::vector<ReferenceError> errors;
    std::vector<ReferenceExample> examples;
    std::vector<std::string> related;
};

class OperationReference final {
  public:
    // The reference of the built-in registry, built once.
    [[nodiscard]] static const OperationReference& builtIn();
    [[nodiscard]] static OperationReference build(const OperationRegistry& registry);

    [[nodiscard]] std::span<const ReferenceEntry> entries() const noexcept { return entries_; }
    // Finds an entry by its name or a spelling of it: "bloom.layer.add-solid",
    // "bloom.ops.layer.add_solid", "layer.add-solid", "query:layers", "query layers",
    // "bloom-cli render". Null when nothing matches.
    [[nodiscard]] const ReferenceEntry* find(std::string_view name) const;
    // Entry names that resemble `name`, closest first.
    [[nodiscard]] std::vector<std::string> suggest(std::string_view name,
                                                   std::size_t limit = 5) const;

    // Every entry grouped by kind and area with its one-line summary.
    [[nodiscard]] std::string indexText() const;
    [[nodiscard]] std::string indexJson() const;
    // The whole reference as one Markdown document (the bloom://reference/operations resource).
    [[nodiscard]] std::string markdown() const;

  private:
    std::vector<ReferenceEntry> entries_;
};

[[nodiscard]] std::string referenceText(const ReferenceEntry& entry);
[[nodiscard]] std::string referenceJson(const ReferenceEntry& entry);

// The help answer every surface prints: the index for an empty name, the entry for a known one,
// and for an unknown one a message naming the closest entries.
struct ReferenceAnswer final {
    bool found = false;
    std::string text;
    // {"found":..., "name":..., "entry"|"index"|"suggestions":...}
    std::string json;
};
[[nodiscard]] ReferenceAnswer referenceHelp(std::string_view name);

// The Markdown resources served beside the reference: the node and parameter reference and the
// getting-started guide for agents.
[[nodiscard]] std::string nodeReferenceMarkdown();
[[nodiscard]] std::string agentGuideMarkdown();
// Short orientation for an MCP client (initialize's `instructions`).
[[nodiscard]] std::string_view agentInstructions() noexcept;

} // namespace bloom::scripting
