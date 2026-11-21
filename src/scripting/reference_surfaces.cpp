// The reference entries that do not come from the operation registry: the shared concepts, the MCP
// tools and their query kinds, the bloom-cli commands and the Python entry points. The MCP tool
// schemas (src/mcp/tool_schemas.cpp) are checked against these entries by bloom.mcp.reference, so
// an argument cannot be added to a tool without being described here.

#include "reference_internal.hpp"

#include <array>
#include <utility>

namespace bloom::scripting::detail {

ReferenceEntry makeEntry(const ReferenceKind kind, std::string name, std::string area,
                         std::string summary, std::string description) {
    ReferenceEntry value;
    value.kind = kind;
    value.name = std::move(name);
    value.area = std::move(area);
    value.summary = std::move(summary);
    value.description = std::move(description);
    return value;
}

ReferenceError makeError(std::string code, std::string when, std::string remedy,
                         const bool retryable) {
    return {.code = std::move(code),
            .when = std::move(when),
            .remedy = std::move(remedy),
            .retryable = retryable};
}

void addExamples(ReferenceEntry& value,
                 std::initializer_list<std::pair<std::string_view, std::string_view>> list) {
    for (const auto& [surface, text] : list)
        value.examples.push_back({.surface = std::string(surface), .text = std::string(text)});
}

void addRelated(ReferenceEntry& value, std::initializer_list<std::string_view> names) {
    for (const auto name : names)
        value.related.emplace_back(name);
}

void addArguments(ReferenceEntry& value, std::initializer_list<ArgumentSpec> specs) {
    for (const auto& spec : specs)
        value.arguments.push_back(argument(spec));
}

ReferenceEffects readOnlyEffects() { return {.readOnly = true}; }
ReferenceEffects editEffects() { return {.undoable = true, .dirtiesProject = true}; }
ReferenceEffects fileEffects() { return {.writesFiles = true}; }

namespace {

using Choices = std::initializer_list<std::pair<std::string_view, std::string_view>>;

// ---------------------------------------------------------------------------------------------
// Concepts

void addConcepts(std::vector<ReferenceEntry>& out) {
    auto ids =
        makeEntry(ReferenceKind::Concept, "concept:ids", "concepts",
                  "Every object has a stable positive integer id that never changes or is "
                  "reused.",
                  "Ids are 1-based and stable across edits, saves and undo. Each kind has its "
                  "own id space: a layer id (query:layers .id) is not the id of the layer's "
                  "Layer Output node (.layerOutputNode), and a stack slot (.slot) is a third "
                  "id. A Merge entry is addressed by its Merge node and slot (query:layers "
                  ".entries), because an entry whose layer is not created yet has no layer id. "
                  "Operations that create objects return the new ids in the transaction "
                  "result's `outputs` as {operationIndex, name, id}; read them there instead of "
                  "guessing. An id from a project that was since replaced is stale.");
    addRelated(ids, {"query:layers", "query:nodes", "concept:revisions"});
    out.push_back(std::move(ids));

    auto time =
        makeEntry(ReferenceKind::Concept, "concept:time", "concepts",
                  "Times are frames (an integer) or exact seconds ([numerator, "
                  "denominator]).",
                  "A Time argument accepts an integer, which is a frame count at the "
                  "composition's frame rate (48 at 24 fps is two seconds), or a [numerator, "
                  "denominator] pair, which is an exact time in seconds ([1001, 24000] is one "
                  "frame at 23.976 fps). Read-back reports every time both ways, "
                  "{frames, seconds:[n, d]}, so a value can be written back in the unit it "
                  "was read in. Frame 0 is the composition's first frame. Ranges with an "
                  "`end` (layer ranges, the work area) are end-exclusive: [0, 24) shows "
                  "frames 0..23. Export job ranges and render first/last are inclusive "
                  "frame indices.");
    addRelated(time, {"query:compositions", "query:keyframes", "bloom.layer.set-range"});
    out.push_back(std::move(time));

    auto revisions = makeEntry(
        ReferenceKind::Concept, "concept:revisions", "concepts",
        "Every edit names the revision it expects; a stale one changes nothing.",
        "Each project state has a revision number. Read it from any query result (`revision`), "
        "pass it as `expectedRevision` to transact or history, and use the result's `revision` "
        "for the next call. When someone else (the artist, a script, another agent) edits in "
        "between, the call fails with bloom.scripting.stale-revision and nothing is applied: "
        "re-query what you depend on and retry with the new revision. A revision counts the "
        "edits made to the open project since it was opened or created: opening a project "
        "file (project.open) starts it again at 0, whatever revision the file was saved at, "
        "and so does project.new. It is not kept increasing per file, so a revision read "
        "before a project was replaced says nothing about the new one: read it again after "
        "project.open, and treat an events cursor that reports resetRequired as void.");
    addRelated(revisions, {"transact", "history", "events"});
    out.push_back(std::move(revisions));

    auto context = makeEntry(
        ReferenceKind::Concept, "concept:context", "concepts",
        "Omitted contextual arguments take the active composition, playhead and selection.",
        "Arguments whose reference says `context` may be omitted. They are filled from the live "
        "editor in the desktop application (the composition the Viewer shows, the playhead, the "
        "selected layers or nodes) and from the session defaults on stdio and in bloom-cli (the "
        "first composition, time 0, no selection). Read the values with query:context. An "
        "argument you pass always wins, and a layer edit addressed by merge and slot never "
        "takes the selected layer.");
    addRelated(context, {"query:context"});
    out.push_back(std::move(context));

    auto errors = makeEntry(
        ReferenceKind::Concept, "concept:errors", "concepts",
        "How failures are reported and which ones to retry.",
        "A malformed request (unknown tool, unknown argument, wrong JSON type) is a JSON-RPC "
        "error with code -32602 and a message naming the field. A tool that ran but failed "
        "answers isError:true with structuredContent {succeeded:false, diagnostic:{code, "
        "message}} or, for transact, {succeeded:false, diagnostics:[{code, operationIndex, "
        "operation, argument, message}]}. A request over one of the advertised limits "
        "(initialize: capabilities.experimental.bloom.limits) is a JSON-RPC error with that "
        "limit's own code (-32003 to -32012, or -32000 for the pending queue) and error.data "
        "{code, limit, value, actual, unit}; the commit bounds answer bloom.mcp.commit-queue-full, "
        "bloom.mcp.deadline-exceeded and bloom.mcp.commit-too-large with diagnostic.limit. "
        "Retry bloom.mcp.project-unavailable, "
        "bloom.mcp.commit-queue-full, bloom.mcp.gesture-open, bloom.mcp.deadline-exceeded and "
        "bloom.scripting.stale-revision (after re-reading the revision); fix the request for "
        "every other code.");
    errors.errors = commonToolErrors();
    addRelated(errors, {"transact", "help"});
    out.push_back(std::move(errors));
}

// ---------------------------------------------------------------------------------------------
// MCP tools

void addTools(std::vector<ReferenceEntry>& out) {
    auto help =
        makeEntry(ReferenceKind::Tool, "help", "discovery",
                  "List every tool, query kind, operation and command, or show one entry in "
                  "full.",
                  "Without `name`, returns the index: every entry grouped by kind and area "
                  "with a one-line summary. With `name` (a tool, an operation id such as "
                  "bloom.layer.add-solid, a query kind such as query:layers, a bloom-cli "
                  "command or a concept such as concept:time), returns its full reference: "
                  "arguments with type, unit, range, default and accepted values, side "
                  "effects, prerequisites, result shape, errors with remedies, examples and "
                  "related entries. bloom-cli help and bloom.help() print the same text.");
    help.effects = readOnlyEffects();
    addArguments(help, {{.name = "name",
                         .type = "string",
                         .summary = "The entry to show. Omit for the index.",
                         .unit = "an entry name; operation ids also accept bloom.ops.* and "
                                 "domain.verb spellings"}});
    help.result = "{found, name, text, entry|index|suggestions}: `text` is the readable "
                  "reference; `entry` the same entry as structured data; `suggestions` the "
                  "closest names when nothing matched.";
    addExamples(help, {{"mcp", R"({"name":"help","arguments":{"name":"bloom.layer.add-solid"}})"},
                       {"python", "bloom.help(\"bloom.layer.add-solid\")"},
                       {"cli", "bloom-cli help bloom.layer.add-solid"}});
    addRelated(help, {"query:operations", "query:node-types"});
    out.push_back(std::move(help));

    auto query = makeEntry(
        ReferenceKind::Tool, "query", "document",
        "Read the project: compositions, layers, nodes, parameters, keyframes, assets, node "
        "types, operations and the editor context.",
        "One read-only call per `kind`; each kind has its own entry (query:layers, "
        "query:keyframes, ...) with its arguments and result. Every result carries the "
        "`revision` it was read at. Arguments a kind does not take are refused with a message "
        "naming them.");
    query.effects = readOnlyEffects();
    addArguments(
        query,
        {{.name = "kind",
          .type = "string",
          .required = true,
          .summary = "What to read.",
          .choices = {{"project", "Project id, name, revision"},
                      {"compositions", "Every composition with size, rate, duration, "
                                       "work area"},
                      {"layers", "A composition's layers, topmost first"},
                      {"nodes", "A composition's nodes with ports, links and positions"},
                      {"node-types", "Every node type, or one with nodeType"},
                      {"parameters", "A composition's parameters (pageable)"},
                      {"parameter", "One parameter with its metadata"},
                      {"parameter-value", "A long string parameter in chunks"},
                      {"keyframes", "An animated parameter's keys (pageable)"},
                      {"assets", "Every asset"},
                      {"operations", "Every operation schema"},
                      {"context", "The editor context contextual arguments use"},
                      {"enums", "Keyframe interpolation, ease and handle-mode tokens"},
                      {"text-metrics", "The advance and ink bounds of a piece of text"},
                      {"layer-bounds", "A layer's evaluated bounds at a time"},
                      {"outputs", "Every output of a node at a time, as typed values"},
                      {"subtitles", "A Subtitles node's cues (pageable), or the "
                                    "composition's Subtitles nodes"},
                      {"history", "The undo history: labels and position"},
                      {"viewer", "The Viewer's display settings (desktop only)"},
                      {"preview-cache", "RAM Preview: filled range, fill rate, budget, "
                                        "stop reason (desktop only)"},
                      {"jobs", "Background jobs that are not exports (desktop only)"},
                      {"pixels", "Pixel values of a frame, read from the production "
                                 "render"},
                      {"render-route", "Which route produced the Viewer's frame "
                                       "(desktop only)"},
                      {"scopes", "The Scopes panel's count tables, a summary of them or a "
                                 "picture of a scope, for a frame or a range"}}},
         {.name = "composition",
          .type = "integer",
          .summary = "The composition, for layers, nodes, parameters, parameter, "
                     "parameter-value, keyframes, layer-bounds, outputs, pixels and scopes.",
          .unit = "composition id (query:compositions)",
          .range = ">= 1"},
         {.name = "parameter",
          .type = "integer",
          .summary = "The parameter, for parameter, parameter-value and keyframes.",
          .unit = "parameter id",
          .range = ">= 1"},
         {.name = "curve",
          .type = "integer",
          .summary = "The animation curve, for keyframes (instead of parameter).",
          .unit = "animation curve id",
          .range = ">= 1"},
         {.name = "nodeType",
          .type = "string",
          .summary = "One node type, for node-types.",
          .unit = "node type id such as bloom.solid-source"},
         {.name = "pageSize",
          .type = "integer",
          .summary = "Records per page, for parameters, keyframes and subtitles.",
          .range = "1..64",
          .defaultValue = "64"},
         {.name = "cursor",
          .type = "string",
          .summary = "The previous page's nextCursor. It is bound to the revision: any "
                     "edit in between makes it stale, so restart from the first page.",
          .range = "at most 160 characters"},
         {.name = "expectedRevision",
          .type = "integer",
          .summary = "For parameter, parameter-value and outputs: refuse when the "
                     "project moved on. Required for parameter-value.",
          .range = ">= 0"},
         {.name = "offset",
          .type = "integer",
          .summary = "For parameter-value: the byte offset to read from (the previous "
                     "chunk's nextOffset).",
          .unit = "UTF-8 bytes",
          .range = ">= 0",
          .defaultValue = "0"},
         {.name = "limit",
          .type = "integer",
          .summary = "For parameter-value: the most bytes to return.",
          .unit = "UTF-8 bytes",
          .range = "4..131072",
          .defaultValue = "131072"},
         {.name = "idPrefix",
          .type = "string",
          .summary = "For operations: only the operations whose id starts with this "
                     "text, such as \"bloom.animation.\". Default: every operation.",
          .unit = "operation id prefix",
          .range = "at most 128 characters"},
         {.name = "layer",
          .type = "integer",
          .summary = "For layers and nodes: only this layer (its record and entries), "
                     "or only its Layer Output node (or its entry's source) and "
                     "everything upstream of it. Default: every layer.",
          .unit = "layer id (query:layers .id)",
          .range = ">= 1"},
         {.name = "node",
          .type = "integer",
          .summary = "For outputs: the node whose outputs are read. For subtitles: the "
                     "Subtitles node whose cues are read (omitted, the composition's "
                     "Subtitles nodes are listed).",
          .unit = "node id (query:nodes .id)",
          .range = ">= 1"},
         {.name = "text",
          .type = "string",
          .summary = "For text-metrics: the text to measure. A newline starts a new "
                     "line.",
          .unit = "UTF-8 text",
          .range = "at most 4096 bytes"},
         {.name = "font",
          .type = "string or integer",
          .summary = "For text-metrics: a built-in face, by label (\"DejaVu Sans\", "
                     "\"Inter Regular\", \"Inter Medium\", \"Inter SemiBold\"), by "
                     "stored value 0 to 3, or by its family|style|digest font "
                     "reference.",
          .unit = "built-in face",
          .defaultValue = "DejaVu Sans (a new Text layer's font)"},
         {.name = "size",
          .type = "number",
          .summary = "For text-metrics: the text size.",
          .unit = "pixels per em",
          .range = "above 0 to 4096"},
         {.name = "time",
          .type = "integer or [numerator, denominator]",
          .summary =
              "For layer-bounds, outputs, pixels and scopes: the time to evaluate at. An integer is a whole "
              "frame of the composition at its frame rate; [numerator, "
              "denominator] is exact seconds (concept:time).",
          .unit = "composition time",
          .defaultValue = "the playhead (frame 0 headless)"},
         {.name = "at",
          .type = "integer, [numerator, denominator] or \"HH:MM:SS.mmm\"",
          .summary = "For subtitles: only the cues showing at this time.",
          .unit = "composition time"},
         {.name = "from",
          .type = "integer, [numerator, denominator] or \"HH:MM:SS.mmm\"",
          .summary = "For subtitles: only the cues that end after this time.",
          .unit = "composition time"},
         {.name = "to",
          .type = "integer, [numerator, denominator] or \"HH:MM:SS.mmm\"",
          .summary = "For subtitles: only the cues that start before this time.",
          .unit = "composition time"},
         {.name = "format",
          .type = "string",
          .summary = "For subtitles: json (cue records), or srt or vtt (the whole list as "
                     "that file's text).",
          .defaultValue = "json",
          .choices = {{"json", "Cue records"},
                      {"srt", "The list as SubRip text"},
                      {"vtt", "The list as WebVTT text"}}},
         {.name = "region",
          .type = "array of 4 integers",
          .summary = "For pixels: [x, y, width, height], the pixels to read. A point is "
                     "[x, y, 1, 1]. For scopes: the part of the frame to measure.",
          .unit = "composition pixels from the frame's top-left corner",
          .range = "pixels: inside the frame, width * height at most 256; scopes: width "
                   "and height at least 1, the part outside the frame is cut"},
         {.name = "frame",
          .type = "integer",
          .summary = "For scopes: the first (or only) frame to read, instead of time.",
          .unit = "whole frames of the composition",
          .range = ">= 0"},
         {.name = "lastFrame",
          .type = "integer",
          .summary = "For scopes: the last frame of a range, inclusive.",
          .unit = "whole frames",
          .range = ">= 0"},
         {.name = "scopes",
          .type = "array of strings",
          .summary = "For scopes: waveform, parade, vectorscope, histogram or chromaticity. "
                     "Default: all five.",
          .unit = "scope names"},
         {.name = "signal",
          .type = "string",
          .summary = "For scopes: \"display\" (default) or \"working\".",
          .range = "\"display\" or \"working\"",
          .choices = {{"display", "The configuration's display and view"},
                      {"working", "The working signal"}}},
         {.name = "axisMaximum",
          .type = "integer",
          .summary = "For scopes with the working signal: the axis maximum.",
          .unit = "linear value",
          .range = "1, 2, 4, 8 or 16",
          .defaultValue = "1"},
         {.name = "ignoreTransparent",
          .type = "boolean",
          .summary = "For scopes: leave out pixels whose alpha is 0.",
          .defaultValue = "false"},
         {.name = "resolution",
          .type = "string",
          .summary = "For scopes: \"full\" (default), \"half\" or \"quarter\".",
          .range = "\"full\", \"half\" or \"quarter\"",
          .choices = {{"full", "Every pixel"},
                      {"half", "Half the size"},
                      {"quarter", "A quarter of the size"}}},
         {.name = "display",
          .type = "string",
          .summary = "For scopes: the OCIO display of the display signal.",
          .unit = "display name of the color configuration"},
         {.name = "view",
          .type = "string",
          .summary = "For scopes: the OCIO view of the display signal.",
          .unit = "view name of the display"},
         {.name = "result",
          .type = "string",
          .summary = "For scopes: \"tables\" (default), \"summary\" or \"image\".",
          .range = "\"tables\", \"summary\" or \"image\"",
          .choices = {{"tables", "The count tables"},
                      {"summary", "Statistics computed from the tables"},
                      {"image", "A PNG of the scope"}}},
         {.name = "encoding",
          .type = "string",
          .summary = "For scopes tables: \"dense\" (default) or \"sparse\".",
          .range = "\"dense\" or \"sparse\"",
          .choices = {{"dense", "Every cell"},
                      {"sparse", "Index and count pairs of the non-zero cells"}}},
         {.name = "image",
          .type = "string",
          .summary = "For a scopes image: which scope, or \"all\" (default).",
          .range = "\"waveform\", \"parade\", \"vectorscope\", \"histogram\", "
                   "\"chromaticity\" or \"all\"",
          .choices = {{"waveform", "Waveform"},
                      {"parade", "RGB parade"},
                      {"vectorscope", "Vectorscope"},
                      {"histogram", "Histogram"},
                      {"chromaticity", "CIE chromaticity"},
                      {"all", "All five stacked in the panel's order"}}},
         {.name = "width",
          .type = "integer",
          .summary = "For a scopes image: the picture's width.",
          .unit = "pixels",
          .range = "128..2048",
          .defaultValue = "640"},
         {.name = "height",
          .type = "integer",
          .summary = "For a scopes image of one scope: the picture's height.",
          .unit = "pixels",
          .range = "128..2048"},
         {.name = "waveformChannels",
          .type = "string",
          .summary = "For a scopes image: \"rgb\" (default) or \"luma\".",
          .range = "\"rgb\" or \"luma\"",
          .choices = {{"rgb", "Red, green and blue"}, {"luma", "Luma"}}},
         {.name = "histogramChannels",
          .type = "string",
          .summary = "For a scopes image: \"rgb\" (default), \"luma\" or \"rgb-luma\".",
          .range = "\"rgb\", \"luma\" or \"rgb-luma\"",
          .choices = {{"rgb", "Red, green and blue"}, {"luma", "Luma"}, {"rgb-luma", "All four"}}},
         {.name = "histogramScale",
          .type = "string",
          .summary = "For a scopes image: \"linear\" (default) or \"logarithmic\".",
          .range = "\"linear\" or \"logarithmic\"",
          .choices = {{"linear", "Linear heights"}, {"logarithmic", "Logarithmic heights"}}},
         {.name = "fields",
          .type = "array of strings",
          .summary = "For the kinds that return records: keep only these members of "
                     "every record, such as [\"id\", \"name\"]. A name the records "
                     "do not have is refused, listing the ones they do. Default: every "
                     "member.",
          .unit = "member names of the records",
          .range = "1 to 64 names"}});
    query.result = "{revision, ...}: see the query:<kind> entries.";
    query.errors = {
        makeError("-32602", "an unknown kind, or an argument the kind does not take",
                  "use one of the listed kinds; help query:<kind> lists its arguments"),
        makeError("-32602", "a stale composition, parameter or curve id",
                  "re-read the ids with query:compositions, query:layers or query:nodes")};
    addExamples(query,
                {{"mcp", R"({"name":"query","arguments":{"kind":"layers","composition":1}})"}});
    addRelated(query, {"query:context", "query:compositions", "query:layers", "query:nodes",
                       "query:keyframes", "query:node-types", "query:text-metrics",
                       "query:layer-bounds", "query:history", "query:viewer", "query:preview-cache",
                       "query:jobs", "query:pixels", "query:render-route", "query:scopes"});
    out.push_back(std::move(query));

    auto transact = makeEntry(
        ReferenceKind::Tool, "transact", "document",
        "Apply a list of operations atomically as one undo step.",
        "Every operation in `operations` is validated first; if any is invalid, nothing is "
        "applied. They then run in order as one command with one undo entry named `label`. In "
        "the desktop application the edit lands in the artist's undo history and waits for an "
        "open drag or text edit to finish (up to 30 s); it is built and checked off Bloom's main "
        "thread, and one transaction carries at most 1,000,000 units of work (one per "
        "operation, or one per key, keyframe or edit an operation lists). Operation ids and their "
        "arguments: "
        "help with no name lists them, help <operation id> describes one, query:operations "
        "returns every schema. Omitted contextual arguments are filled from the editor context "
        "(concept:context).");
    transact.effects = editEffects();
    addArguments(
        transact,
        {{.name = "expectedRevision",
          .type = "integer",
          .required = true,
          .summary = "The revision the edit is based on (any query result's revision).",
          .range = ">= 0"},
         {.name = "operations",
          .type = "array",
          .required = true,
          .summary = "The operations, applied in order: [{\"op\": operation id, \"args\": "
                     "{argument: value}}]. Unknown argument names are refused.",
          .range = "at least 1"},
         {.name = "label",
          .type = "string",
          .summary = "The undo step's name (Edit > Undo <label>).",
          .range = "at most 4096 bytes",
          .defaultValue = "\"Agent edit\" in the desktop application, \"MCP transaction\" on "
                          "stdio"},
         {.name = "response",
          .type = "string",
          .summary = "\"full\" lists every id the operations created in `outputs`; \"summary\" "
                     "replaces that list with `outputSummary` {count, byName, digest}: how many "
                     "ids each output name made and one SHA-256 over the full list (MCP only). "
                     "Use it for edits that create hundreds of keys. A transaction that creates "
                     "more than 10,000 ids (limit transactOutputs) answers in the summary form "
                     "even when \"full\" was asked, with `outputsSummarized` {name, value, "
                     "actual}.",
          .defaultValue = "\"full\"",
          .choices = {{"full", "Every created id in `outputs`"},
                      {"summary", "`outputSummary`: counts by output name and one digest"}}},
         {.name = "compileCheck",
          .type = "boolean",
          .summary = "Whether to compile, after the edit, every composition whose evaluation "
                     "inputs it changed and report what no longer compiles (`compileFailures`, "
                     "`compileWarnings`). false skips it for bulk key writes that cannot break a "
                     "composition: the result then says `compileChecked: false` and both lists "
                     "are null (MCP only).",
          .defaultValue = "true"},
         {.name = "async",
          .type = "boolean",
          .summary = "true answers at once with a job id ({succeeded, async, job: "
                     "\"transact-<n>\", state, phase, queuedAhead, baseRevision}) and runs the "
                     "edit as a job: jobs.wait waits for it (with progress over a progress token) "
                     "and returns this tool's answer as `result`, jobs.list lists it and "
                     "jobs.cancel stops it until its edit starts publishing. At most 16 jobs are "
                     "queued or running (limit asyncJobs). A job applies only to the project it "
                     "was sent for: one replaced first fails with bloom.mcp.project-replaced. "
                     "Desktop application only (MCP only).",
          .defaultValue = "false"},
         {.name = "idempotencyKey",
          .type = "string",
          .summary = "A key of your own (a UUID) that makes a retry safe: for 10 minutes the "
                     "same key with the same arguments answers what the first request answered "
                     "(with `idempotentReplay: true`; for async, the same job) instead of "
                     "applying again, and while the first is still running the retry waits for "
                     "it. Only an edit that reached the document is remembered; a busy, "
                     "cancelled or replaced one is forgotten, so its retry runs. The same key "
                     "with other arguments is refused with bloom.mcp.idempotency-conflict. Keys "
                     "are shared by every client (MCP only).",
          .range = "1..128 printable ASCII characters"}});
    transact.prerequisites = {"Read the current revision (any query result)."};
    transact.result =
        "{succeeded, revision, origin (\"agent\"), diagnostics:[{code, severity?, operationIndex, "
        "operation, argument?, message}], outputs:[{operationIndex, name, id}], compileChecked, "
        "compileFailures, compileWarnings}. `revision` is "
        "the new "
        "revision; `outputs` lists every id the operations created, by the zero-based index of "
        "the operation that made it (an operation's reference names its outputs). At most 256 "
        "diagnostics are listed (the rest are counted in `diagnosticsOmitted`). Over MCP any "
        "operation argument may be {\"fromFile\": \"<absolute .json path>\"}: the server reads "
        "that JSON file (regular file, at most 16 MiB, 2,000,000 values and 16 levels deep; at "
        "most 16 per call) as the argument's value, so a large column of keys never passes "
        "through the conversation. One request is at most 1 MiB and 65,536 JSON values: write "
        "many keys with bloom.animation.set-key-columns, and many parameters' keys with "
        "bloom.animation.set-key-tracks. A transaction that applied is always "
        "answered: an answer that would exceed the response bound comes back compact "
        "(`responseCompacted`).";
    transact.errors = {
        makeError("bloom.scripting.invalid-argument",
                  "an argument is missing, of the wrong kind or out of range",
                  "the message names the operation, the argument and a working example call"),
        makeError("bloom.scripting.unknown-operation", "the op id is not registered",
                  "help with no name lists every operation"),
        makeError("bloom.scripting.stale-revision", "expectedRevision is not the current revision",
                  "re-query and retry with the current revision", true),
        makeError("bloom.scripting.command-rejected",
                  "the document refused an operation (a stale id, a locked layer, an impossible "
                  "edit)",
                  "read the message; re-query the ids it names"),
        makeError("bloom.scripting.operation-warning",
                  "an operation applied but skipped part of its work (severity warning)",
                  "nothing to retry; the message says what was skipped"),
        makeError("bloom.mcp.commit-queue-full",
                  "in the desktop application, 16 edits are waiting already (limit commitQueue); "
                  "diagnostic.retryAfterMs says when to try again",
                  "wait retryAfterMs, then send it again; or send long edits with async:true",
                  true),
        makeError("bloom.mcp.jobs-full",
                  "async:true while 16 asynchronous jobs are queued or running (limit "
                  "asyncJobs); diagnostic.retryAfterMs says when to try again",
                  "wait for one with jobs.wait, or retry after retryAfterMs", true),
        makeError("bloom.mcp.idempotency-conflict",
                  "the idempotencyKey was used in the last 10 minutes for other arguments",
                  "use a new key for a new edit"),
        makeError("bloom.mcp.not-live", "async:true on the stdio server",
                  "send it without async; stdio answers when the edit applied"),
        makeError("bloom.mcp.commit-too-large",
                  "in the desktop application, the transaction carries more than 1,000,000 units "
                  "of work (one per operation, or one per key, keyframe or edit an operation "
                  "lists); diagnostic.limit names the bound and the actual count",
                  "split it into several transactions"),
        makeError("bloom.mcp.limit.json-values",
                  "the request holds more than 65,536 JSON values (JSON-RPC error -32004; "
                  "error.data names the limit, its value and the actual count)",
                  "write keys with bloom.animation.set-key-columns and pass long columns as "
                  "{\"fromFile\": ...}"),
        makeError("bloom.mcp.limit.request-bytes",
                  "the request is larger than 1 MiB (JSON-RPC error -32003)",
                  "pass long columns as {\"fromFile\": ...}, or split the edit"),
        makeError("bloom.mcp.limit.data-file-values",
                  "a data file holds more than 2,000,000 values (-32011; also -32009 more than 16 "
                  "files, -32010 a file over 16 MiB, -32012 deeper than 16 levels)",
                  "split the data over several files or calls")};
    addExamples(
        transact,
        {{"mcp",
          R"({"name":"transact","arguments":{"expectedRevision":0,"label":"Add a red solid","operations":[{"op":"bloom.layer.add-solid","args":{"name":"Red","color":[1,0,0,1]}}]}})"},
         {"python", "with bloom.transactions.group(\"Add a red solid\"):\n"
                    "    bloom.ops.layer.add_solid(name=\"Red\", color=(1, 0, 0, 1))"},
         {"cli",
          R"(bloom-cli run edits.json   # [{"op":"bloom.layer.add-solid","args":{"name":"Red","color":[1,0,0,1]}}])"}});
    addRelated(transact, {"concept:revisions", "concept:context", "history", "query:operations"});
    out.push_back(std::move(transact));

    auto history = makeEntry(ReferenceKind::Tool, "history", "document",
                             "Undo or redo one step of the project's history.",
                             "Undoes or redoes the most recent step of ANY origin, including the "
                             "artist's own edits in the desktop application, so read events or "
                             "query the revision first and undo only a step you made.");
    history.effects = editEffects();
    addArguments(history, {{.name = "action",
                            .type = "string",
                            .required = true,
                            .summary = "Which way to step.",
                            .choices = {{"undo", "Undo"}, {"redo", "Redo"}}},
                           {.name = "expectedRevision",
                            .type = "integer",
                            .required = true,
                            .summary = "The current revision; a stale one changes nothing.",
                            .range = ">= 0"}});
    history.result = "The transact result shape: {succeeded, revision, origin, diagnostics, outputs}.";
    history.errors = {makeError("bloom.scripting.stale-revision", "the project moved on",
                                "re-query and retry", true),
                      makeError("bloom.scripting.command-rejected",
                                "there is nothing to undo or redo", "nothing to do")};
    addExamples(
        history,
        {{"mcp", R"({"name":"history","arguments":{"action":"undo","expectedRevision":3}})"},
         {"python", "bloom.transactions.undo()"}});
    addRelated(history, {"transact", "events"});
    out.push_back(std::move(history));

    auto events = makeEntry(
        ReferenceKind::Tool, "events", "document",
        "Poll what changed: command events after a revision and sequence, a page or a summary.",
        "Each event is a command result with `sequence` and `kind` (revision: an edit applied; "
        "history: an undo or redo; rejected: an edit refused), or a notice (composition."
        "compile-failed, project.saved, project.opened, project.created, project.closed, "
        "project.copy-saved). Pass the last `sequence` you saw as afterSequence. Every event "
        "names who made the edit in `origin`: user (the artist, in the application's panels), "
        "agent (an MCP client: transact and history) or script (a Python or JSON script, the "
        "Script editor included), so you can tell your own edits from the artist's. One answer "
        "lists at most `limit` events (1,024); `kinds` and `compositions` select events (an event "
        "that names no composition, a project-wide change, matches every composition). "
        "response \"summary\" answers what changed after the cursor in one object instead: the "
        "sequence and revision ranges, counts by kind and origin, and per composition, the "
        "shape of an events.subscribe notification. When `resetRequired` is true (events were "
        "trimmed or the project was replaced) re-query whatever you cache and start again from "
        "the returned sequence. To be told instead of polling, use events.subscribe.");
    events.effects = readOnlyEffects();
    addArguments(events, {{.name = "sinceRevision",
                           .type = "integer",
                           .required = true,
                           .summary = "Skip events whose revision is older.",
                           .range = ">= 0"},
                          {.name = "afterSequence",
                           .type = "integer",
                           .summary = "Skip events up to and including this sequence.",
                           .range = ">= 0",
                           .defaultValue = "0"},
                          {.name = "limit",
                           .type = "integer",
                           .summary = "The most events one answer lists; when more follow, "
                                      "`truncated` is true and `sequence` is the last one listed.",
                           .range = "1..1024",
                           .defaultValue = "1024"},
                          {.name = "kinds",
                           .type = "array of strings",
                           .summary = "Only these kinds (revision, history, rejected, "
                                      "composition.compile-failed, project.saved, "
                                      "project.opened, project.created, project.closed, "
                                      "project.copy-saved).",
                           .defaultValue = "every kind"},
                          {.name = "compositions",
                           .type = "array of composition ids",
                           .summary = "Only events that change one of these compositions, or "
                                      "name none (project-wide).",
                           .defaultValue = "every composition"},
                          {.name = "response",
                           .type = "string",
                           .summary = "The answer's form.",
                           .defaultValue = "events",
                           .choices = {{"events", "The events themselves"},
                                       {"summary", "One summary: ranges and counts, no events"}}}});
    events.result =
        "{revision, sequence, resetRequired, truncated, events:[{sequence, kind, succeeded, "
        "revision, origin (user|agent|script), diagnostics, outputs, outputCount, "
        "outputsOmitted?}]}. An event keeps its created ids when it made at most 1,024 "
        "(otherwise `outputsOmitted` and only `outputCount`); one answer lists at most `limit` "
        "events and 8,192 ids, and when it stops early `truncated` is true and `sequence` is the "
        "last event listed: pass it as afterSequence to continue. response \"summary\": "
        "{revision, sequence, truncated:false, fromSequence, toSequence, resetRequired, reason?, "
        "count, fromRevision?, toRevision?, kinds:{<kind>: n}, origins:{user|agent|script: n}, "
        "compositions:[{composition, count, fromRevision, toRevision}] (at most 64), "
        "compositionsOmitted, project?:{count, fromRevision, toRevision}}.";
    addExamples(
        events,
        {{"mcp", R"({"name":"events","arguments":{"sinceRevision":0}})"},
         {"mcp",
          R"({"name":"events","arguments":{"sinceRevision":0,"afterSequence":120,"limit":100,"kinds":["revision","history"]}})"},
         {"mcp",
          R"({"name":"events","arguments":{"sinceRevision":0,"afterSequence":120,"response":"summary","compositions":[1]}})"}});
    addRelated(events, {"concept:revisions", "events.subscribe"});
    out.push_back(std::move(events));

    auto subscribe = makeEntry(
        ReferenceKind::Tool, "events.subscribe", "document",
        "Be told what changed instead of polling: coalesced change notifications.",
        "Subscribes this MCP session. Bloom then sends notifications/bloom/events: at most one "
        "per intervalMs, each saying what changed since the previous one as revision ranges "
        "with counts (by kind, by origin and per composition, the shape of events with response "
        "\"summary\") and the newest step of each asynchronous transact job that moved (kind "
        "job). Over Streamable HTTP, open the stream with GET /mcp (Accept: text/event-stream, "
        "your Mcp-Session-Id); each notification's SSE id is its `sequence`, so a reconnect with "
        "Last-Event-ID resumes after it, and one without resumes after the last one sent. A "
        "session has one stream: a newer GET ends the older. On stdio the notifications arrive "
        "on stdout between answers. Bloom keeps no backlog per subscriber: when you fall further "
        "behind than the retained events (initialize's limits.retainedEvents), or the project is "
        "replaced, the next notification says resetRequired with reason overflow or "
        "project-replaced; re-query what you cache and go on. Read the events themselves with "
        "events from the previous `sequence`. Calling it again changes the filter and interval "
        "in place.");
    subscribe.effects = readOnlyEffects();
    addArguments(subscribe,
                 {{.name = "kinds",
                   .type = "array of strings",
                   .summary = "Only these kinds: revision, history, rejected, "
                              "composition.compile-failed, project.saved, project.opened, "
                              "project.created, project.closed, project.copy-saved, job.",
                   .defaultValue = "every kind"},
                  {.name = "compositions",
                   .type = "array of composition ids",
                   .summary = "Only changes to these compositions, and project-wide changes "
                              "(which name none).",
                   .defaultValue = "every composition"},
                  {.name = "intervalMs",
                   .type = "integer",
                   .summary = "The least time between two notifications; changes inside it are "
                              "coalesced into one.",
                   .unit = "milliseconds",
                   .range = "50..60000",
                   .defaultValue = "250"},
                  {.name = "afterSequence",
                   .type = "integer",
                   .summary = "Report changes after this event sequence (an events `sequence`).",
                   .range = ">= 0",
                   .defaultValue = "now (the first call), else unchanged"}});
    subscribe.result =
        "{succeeded, subscription, created, method:\"notifications/bloom/events\", sequence, "
        "intervalMs, kinds, compositions}. Each notification's params: {subscription, "
        "sequence, fromSequence, toSequence, resetRequired, reason?, count, fromRevision?, "
        "toRevision?, kinds, origins, compositions:[{composition, count, fromRevision, "
        "toRevision}], compositionsOmitted, project?, jobs?:[{job, mine, state, phase, "
        "completed, total?, message?, revision?, code?, finished, cancellationRequested?}]}.";
    subscribe.errors = {makeError("bloom.mcp.invalid-argument",
                                  "an unknown kind, an interval out of range, or an "
                                  "afterSequence in the future",
                                  "fix the argument")};
    addExamples(
        subscribe,
        {{"mcp",
          R"({"name":"events.subscribe","arguments":{"kinds":["revision","history","job"],"compositions":[1],"intervalMs":500}})"}});
    addRelated(subscribe, {"events", "events.unsubscribe", "jobs.list"});
    out.push_back(std::move(subscribe));

    auto unsubscribe = makeEntry(
        ReferenceKind::Tool, "events.unsubscribe", "document",
        "End this session's change notifications.",
        "Ends the subscription events.subscribe made, and its stream (the GET stream closes). "
        "Ending the MCP session ends it too.");
    unsubscribe.effects = readOnlyEffects();
    unsubscribe.result = "{succeeded, ended, subscription?, sequence?}";
    addExamples(unsubscribe, {{"mcp", R"({"name":"events.unsubscribe","arguments":{}})"}});
    addRelated(unsubscribe, {"events.subscribe"});
    out.push_back(std::move(unsubscribe));

    // The export queue's built-in ids (the vocabulary of export.presets) lead; the older names
    // name the same presets and are still accepted.
    const auto stillPresets = Choices{
        {"builtin:png-sequence", "PNG, 8-bit RGBA, sRGB"},
        {"builtin:exr-sequence", "OpenEXR, 32-bit float RGBA, linear Rec.709 scene"},
        {"builtin:tiff-sequence", "TIFF, 16-bit RGBA, sRGB"},
        {"PngRgba8SrgbV1", "PNG, 8-bit RGBA, sRGB (same as builtin:png-sequence)"},
        {"FlatExrRgba32fLinRec709SceneV1", "OpenEXR, 32-bit float RGBA, linear Rec.709 scene"},
        {"TiffRgba16SrgbV1", "TIFF, 16-bit RGBA, sRGB"}};
    auto render = makeEntry(
        ReferenceKind::Tool, "render", "render",
        "Render one frame or an inclusive frame range to image files and return their digests.",
        "Runs as a transient export job with the Export panel's checks. Output keeps alpha; the "
        "composition background colour is not rendered. Frames are composition frame indices "
        "(0 is the first frame). Use capture to look at a frame, render for reference stills or "
        "sequences, export for movies and audio.");
    render.effects = fileEffects();
    addArguments(
        render,
        {{.name = "composition",
          .type = "integer",
          .required = true,
          .summary = "The composition to render.",
          .unit = "composition id",
          .range = ">= 1"},
         {.name = "frame",
          .type = "integer",
          .summary = "One frame. Give frame, or first and last.",
          .unit = "composition frame index",
          .range = ">= 0"},
         {.name = "first",
          .type = "integer",
          .summary = "First frame of an inclusive range.",
          .unit = "composition frame index",
          .range = ">= 0"},
         {.name = "last",
          .type = "integer",
          .summary = "Last frame of an inclusive range (at most 10000 frames per call).",
          .unit = "composition frame index",
          .range = ">= 0"},
         {.name = "preset",
          .type = "string",
          .required = true,
          .summary = "The file format.",
          .choices = stillPresets},
         {.name = "destination",
          .type = "string",
          .required = true,
          .summary = "The file to write; a range numbers its files. A relative path is "
                     "relative to the project folder (the working directory on stdio); an "
                     "absolute path is used as is. The folder must exist.",
          .range = "1..4096 bytes"},
         {.name = "overwrite",
          .type = "string",
          .summary = "What to do when a file exists. Over MCP, ask behaves as refuse: nothing "
                     "can answer the question, so the call fails and names the file; pass "
                     "replace to replace it.",
          .defaultValue = "ask",
          .choices = {{"ask", "Fail and name the existing file"},
                      {"replace", "Replace existing files"},
                      {"refuse", "Fail when a file exists"}}},
         {.name = "subtitles",
          .type = "object",
          .summary =
              "Also write the composition's subtitles as sidecar files "
              "<name>.<language>[.<n>].<format> beside the images, frame-exact to the "
              "range: {sidecar:[\"srt\"|\"vtt\"|\"ttml\" (each at most once)], nodes:[Subtitles "
              "node ids] (1..256; absent means every Subtitles node with Export as track "
              "on)}. Absent writes none.",
          .range = "sidecar needs at least one format"}});
    render.result =
        "{publishedFrames, preservationReport, gpuFrames|null, "
        "cpuReferenceFrames|null, provenance (gpu|cpu-reference)|null, routeNote?, "
        "files:[{path, digest:\"sha256:<hex>\"}] (with the sidecars), subtitles?:[{node, "
        "language, title, format:\"srt\"|\"vtt\"|\"ttml\", path, cues, bytes, sha256}] "
        "(with subtitles). The route fields say which route "
        "evaluated the frames and read null when none was recorded; query:render-route "
        "reads the Viewer's frame.";
    render.errors = {
        makeError("bloom.mcp.export-unavailable",
                  "the destination exists and overwrite is not replace (ask and refuse both stop)",
                  "pass overwrite:\"replace\" or choose another destination"),
        makeError("bloom.mcp.export-failed", "the render or a destination check failed",
                  "the message and diagnostic.detail say which frame or file"),
        makeError("bloom.mcp.export-unavailable", "the export runner is unavailable on this system",
                  "diagnostic.detail names the reason (PlatformNotSupported on macOS and Windows)"),
        makeError("bloom.mcp.cancelled", "notifications/cancelled or shutdown stopped the job",
                  "frames already published remain; render again for the rest", true)};
    addExamples(
        render,
        {{"mcp",
          R"({"name":"render","arguments":{"composition":1,"frame":0,"preset":"PngRgba8SrgbV1","destination":"frame.png","overwrite":"replace"}})"},
         {"python", "bloom.render.frame(0, out=\"frames\")"},
         {"cli", "bloom-cli render --project scene.bloom --frame 0 --preset PngRgba8SrgbV1 "
                 "--out frames"}});
    addRelated(render, {"capture", "export", "export.confirm-replace"});
    const auto renderErrors = render.errors;
    out.push_back(std::move(render));

    auto exportTool = makeEntry(
        ReferenceKind::Tool, "export", "render",
        "Export a composition to a movie, audio file or image files in one call.",
        "Runs as a transient export job. Output keeps alpha; the composition background colour "
        "is not rendered. For a job that stays in the project's export queue (and the Export "
        "panel), use bloom.export.add-job and export.start instead.");
    exportTool.effects = fileEffects();
    addArguments(
        exportTool,
        {{.name = "composition",
          .type = "integer",
          .required = true,
          .summary = "The composition to export.",
          .unit = "composition id",
          .range = ">= 1"},
         {.name = "frame",
          .type = "integer",
          .summary = "One frame. Give frame, or first and last.",
          .unit = "composition frame index",
          .range = ">= 0"},
         {.name = "first",
          .type = "integer",
          .summary = "First frame of an inclusive range.",
          .unit = "composition frame index",
          .range = ">= 0"},
         {.name = "last",
          .type = "integer",
          .summary = "Last frame of an inclusive range.",
          .unit = "composition frame index",
          .range = ">= 0"},
         {.name = "preset",
          .type = "string",
          .required = true,
          .summary = "The output format: any id export.presets lists as available on this "
                     "computer (built-in or user preset), for example builtin:h264-mp4. A "
                     "preset that is not available is refused with its reason. The values "
                     "below are the presets that have an older name; export.presets lists "
                     "every other one (H.264, HEVC, VP9, AV1, DPX and user presets).",
          .choices = {{"builtin:png-sequence", "PNG, 8-bit RGBA, sRGB"},
                      {"builtin:exr-sequence", "OpenEXR, 32-bit float RGBA, linear Rec.709 scene"},
                      {"builtin:tiff-sequence", "TIFF, 16-bit RGBA, sRGB"},
                      {"builtin:prores-422hq-mov", "ProRes in QuickTime (preview)"},
                      {"builtin:dnxhr-hq-mxf", "DNxHR in MXF"},
                      {"builtin:wav", "PCM WAV audio"},
                      {"PngRgba8SrgbV1", "PNG, 8-bit RGBA, sRGB"},
                      {"FlatExrRgba32fLinRec709SceneV1",
                       "OpenEXR, 32-bit float RGBA, linear Rec.709 scene"},
                      {"TiffRgba16SrgbV1", "TIFF, 16-bit RGBA, sRGB"},
                      {"ProResMovV1", "ProRes in QuickTime (preview)"},
                      {"DnxhrMxfV1", "DNxHR in MXF"},
                      {"PcmWavV1", "PCM WAV audio"}}},
         {.name = "destination",
          .type = "string",
          .required = true,
          .summary = "The file to write; relative paths are relative to the project folder "
                     "(the working directory on stdio). The folder must exist.",
          .range = "1..4096 bytes"},
         {.name = "profile",
          .type = "string",
          .summary = "The codec profile for ProRes or DNxHR (see Exporting video).",
          .range = "at most 32 characters"},
         {.name = "audio",
          .type = "boolean",
          .summary = "Include the composition's audio in a movie."},
         {.name = "sampleRate",
          .type = "integer",
          .summary = "Audio sample rate.",
          .unit = "Hz",
          .range = "8000..192000"},
         {.name = "loudnessLufs",
          .type = "number",
          .summary = "Normalize the audio to this integrated loudness (EBU R128) with a true-peak "
                     "limiter. Absent leaves the audio as mixed.",
          .unit = "LUFS",
          .range = "-40..-5"},
         {.name = "truePeakDbtp",
          .type = "number",
          .summary = "The true-peak ceiling of a loudness-normalized export. Needs loudnessLufs.",
          .unit = "dBTP",
          .range = "-12..0",
          .defaultValue = "-1"},
         {.name = "encoder",
          .type = "string",
          .summary = "Who encodes a movie. On macOS, automatic makes ProRes with the system's "
                     "encoder and fails if it cannot; software uses Bloom's FFmpeg encoder; "
                     "hardware fails without a system or GPU encoder.",
          .defaultValue = "automatic",
          .choices = {{"automatic",
                       "The system's encoder for ProRes, the best available otherwise"},
                      {"software", "Bloom's FFmpeg encoder"},
                      {"hardware", "A system or GPU encoder, or the export fails"}}},
         {.name = "overwrite",
          .type = "string",
          .summary = "What to do when a file exists. Over MCP, ask behaves as refuse; pass "
                     "replace to replace it.",
          .defaultValue = "ask",
          .choices = {{"ask", "Fail and name the existing file"},
                      {"replace", "Replace existing files"},
                      {"refuse", "Fail when a file exists"}}},
         {.name = "workerMemoryCapBytes",
          .type = "integer",
          .summary =
              "Opt-in cap on the encode worker's memory: at the cap the export waits for the "
              "worker, and fails with a message if it cannot go on. 0 is no cap. Not given "
              "uses the saved Preferences value.",
          .unit = "bytes",
          .range = "0..1099511627776",
          .defaultValue = "the Preferences value (no cap)"},
         {.name = "workerThreads",
          .type = "integer",
          .summary = "Encoder threads in the worker. 0 is automatic. Not given uses the saved "
                     "Preferences value.",
          .range = "0..64",
          .defaultValue = "the Preferences value (automatic)"},
         {.name = "subtitles",
          .type = "object",
          .summary =
              "Deliver the composition's subtitles with the export: {sidecar:[\"srt\"|\"vtt\"|"
              "\"ttml\" (each at most once)], embed:boolean (default false), nodes:[Subtitles "
              "node ids] (1..256; absent means every Subtitles node with Export as track "
              "on)}. Sidecar files are <name>.<language>[.<n>].<format> beside the "
              "output; embed puts the tracks inside a MOV, MP4 or WebM movie (MXF, image "
              "sequences and WAV carry none and refuse embed: use sidecars). Needs a "
              "sidecar format or embed. Absent delivers none.",
          .range = "sidecar or embed"}});
    exportTool.result = "{publishedFrames, preservationReport, workerMemory:{measured, peakBytes, "
                        "capBytes, waits, waitedMs} (a movie or WAV), loudness:{targetLufs, "
                        "truePeakLimitDbtp, measured, measuredLufs, measuredTruePeakDbtp, gainDb, "
                        "finalMeasured, finalLufs, finalTruePeakDbtp, limiterReductionDb} (with "
                        "loudnessLufs), gpuFrames|null, cpuReferenceFrames|null, provenance "
                        "(gpu|cpu-reference)|null, routeNote? (which route evaluated the frames; "
                        "null when none was recorded), files:[{path, "
                        "digest:\"sha256:<hex>\"}] (with the sidecars), subtitles?:[{node, "
                        "language, title, format:\"srt\"|\"vtt\"|\"ttml\"|\"embedded\", path, "
                        "cues, bytes, sha256}] (with subtitles; bytes and sha256 describe a "
                        "sidecar)}";
    exportTool.errors = renderErrors;
    addExamples(
        exportTool,
        {{"mcp",
          R"({"name":"export","arguments":{"composition":1,"first":0,"last":47,"preset":"ProResMovV1","destination":"review.mov"}})"}});
    addRelated(exportTool, {"render", "bloom.export.add-job", "export.start", "export.presets"});
    out.push_back(std::move(exportTool));

    auto capture = makeEntry(
        ReferenceKind::Tool, "capture", "render",
        "Look at a frame: render one PNG at composition resolution and return it as an image.",
        "Uses the still export route with the Viewer's display and view in the desktop "
        "application. Output keeps alpha; the composition background colour is not rendered. "
        "It does not apply the Viewer's exposure, gamma, channel, region of interest or preview "
        "resolution. A PNG above about 2.9 MiB is returned as a resource_link with the notice "
        "bloom.mcp.capture-too-large-for-inline. Captures go to Bloom's per-user cache, which "
        "keeps the newest 16.");
    capture.effects = {.readOnly = true, .writesFiles = true};
    addArguments(capture, {{.name = "composition",
                            .type = "integer",
                            .summary = "The composition to capture.",
                            .unit = "composition id",
                            .range = ">= 1",
                            .defaultValue = "the context composition"},
                           {.name = "frame",
                            .type = "integer",
                            .summary = "The frame to capture.",
                            .unit = "composition frame index",
                            .range = ">= 0",
                            .defaultValue = "the context time (frame 0 on stdio)"},
                           {.name = "statistics",
                            .type = "boolean",
                            .summary = "Return statistics instead of a picture: luma (Rec.709 "
                                       "weights) and red, green and blue mean, min, max and "
                                       "histogram, measured on the frame as the production still "
                                       "export route renders it, in the project's working colour "
                                       "space (named in the result), before any display or view "
                                       "transform.",
                            .defaultValue = "false"},
                           {.name = "last",
                            .type = "integer",
                            .summary = "With statistics: the last frame of an inclusive range "
                                       "starting at frame. The result adds one row of means and "
                                       "luma extremes per frame.",
                            .unit = "composition frame index",
                            .range = ">= frame"},
                           {.name = "region",
                            .type = "array",
                            .summary = "With statistics: [x, y, width, height] in pixels from the "
                                       "top-left corner, inside the composition.",
                            .defaultValue = "the whole frame"},
                           {.name = "bins",
                            .type = "integer",
                            .summary = "With statistics: histogram bins over 0..1. Values below 0 "
                                       "and above 1 are counted in below and above.",
                            .range = "2..256",
                            .defaultValue = "32"}});
    capture.result = "MCP image content (image/png, or a resource_link when too large) plus "
                     "{composition, frame, path, bytes, digest, gpuFrames|null, cpuReferenceFrames|null, "
                     "provenance|null, routeNote?, width, height, inline}. With "
                     "statistics: {composition, first, last, region, colorSpace, luma, route, "
                     "provenance, statistics {pixels, luma, red, green, blue, alphaMean}, frames}.";
    capture.errors = renderErrors;
    addExamples(capture,
                {{"mcp", R"({"name":"capture","arguments":{"composition":1,"frame":12}})"},
                 {"mcp", R"({"name":"capture","arguments":{"composition":1,"frame":0,"last":23,"statistics":true,"region":[0,0,960,540]}})"}});
    addRelated(capture, {"render", "query:context"});
    out.push_back(std::move(capture));

    // The export queue's session calls (ADR 0033).
    struct QueueTool final {
        std::string_view name;
        std::string_view summary;
        bool readOnlyCall;
    };
    constexpr std::array<QueueTool, 14> queue{{
        {"export.start",
         "Start export queue jobs, as the Export panel's Start does. If a destination is still "
         "being checked, the call waits for the check (up to 10 seconds) before it answers; "
         "DestinationCheckPending means the check took longer, so call again.",
         false},
        {"export.start-all",
         "Start every enabled export queue job. Waits for destination checks as export.start "
         "does.",
         false},
        {"export.pause", "Pause a rendering export job at the next frame boundary.", false},
        {"export.resume", "Resume a paused export job.", false},
        {"export.stop", "Stop an export job.", false},
        {"export.stop-all", "Stop every active export job.", false},
        {"export.retry", "Run a finished export job again.", false},
        {"export.confirm-replace",
         "Confirm replacing an output's existing destination exactly as it was last checked.",
         false},
        {"export.status",
         "The export queue snapshot: jobs, outputs, states, progress, failures and "
         "verification.",
         true},
        {"export.events", "Terminal export events (done, failed, stopped) after a sequence.", true},
        {"export.log", "An export job's run log after a sequence.", true},
        {"export.wait",
         "Wait until an export job (or every active job) finishes, with progress when the "
         "request carries a progress token.",
         true},
        {"export.presets",
         "The export presets available on this computer and for the open project, optionally "
         "for one job's composition.",
         true},
        {"export.script",
         "A script that recreates an export job and starts it (bloom-cli JSON or Python).", true},
    }};
    for (const auto& item : queue) {
        auto value = makeEntry(
            ReferenceKind::Tool, std::string(item.name), "export queue", std::string(item.summary),
            "Job and output ids come from bloom.export.add-job's outputs or from "
            "export.status. Jobs started this way belong to the Export panel and "
            "keep running when the agent disconnects. Output keeps alpha; the "
            "composition background colour is not rendered.");
        value.effects = item.readOnlyCall ? readOnlyEffects() : fileEffects();
        const auto name = item.name;
        if (name == "export.start")
            addArguments(value, {{.name = "jobs",
                                  .type = "array",
                                  .required = true,
                                  .summary = "The jobs to start, in run order.",
                                  .unit = "export job ids (export.status)",
                                  .range = "1..1024 ids"},
                                 {.name = "workerMemoryCapBytes",
          .type = "integer",
          .summary = "Opt-in cap on the encode worker's memory: at the cap the export waits for the "
                     "worker, and fails with a message if it cannot go on. 0 is no cap. Not given "
                     "uses the saved Preferences value.",
          .unit = "bytes",
          .range = "0..1099511627776",
          .defaultValue = "the Preferences value (no cap)"},
         {.name = "workerThreads",
          .type = "integer",
          .summary = "Encoder threads in the worker. 0 is automatic. Not given uses the saved "
                     "Preferences value.",
          .range = "0..64",
          .defaultValue = "the Preferences value (automatic)"}});
        if (name == "export.start-all")
            addArguments(value, {{.name = "workerMemoryCapBytes",
          .type = "integer",
          .summary = "Opt-in cap on the encode worker's memory: at the cap the export waits for the "
                     "worker, and fails with a message if it cannot go on. 0 is no cap. Not given "
                     "uses the saved Preferences value.",
          .unit = "bytes",
          .range = "0..1099511627776",
          .defaultValue = "the Preferences value (no cap)"},
         {.name = "workerThreads",
          .type = "integer",
          .summary = "Encoder threads in the worker. 0 is automatic. Not given uses the saved "
                     "Preferences value.",
          .range = "0..64",
          .defaultValue = "the Preferences value (automatic)"}});
        if (name == "export.pause" || name == "export.resume" || name == "export.stop" ||
            name == "export.retry" || name == "export.confirm-replace" || name == "export.log")
            addArguments(value, {{.name = "job",
                                  .type = "integer",
                                  .required = true,
                                  .summary = "The job.",
                                  .unit = "export job id (export.status)",
                                  .range = ">= 1"}});
        if (name == "export.confirm-replace")
            addArguments(value,
                         {{.name = "output",
                           .type = "integer",
                           .required = true,
                           .summary = "The output whose existing destination may be replaced.",
                           .unit = "export output id (export.status)",
                           .range = ">= 1"}});
        if (name == "export.status")
            addArguments(value,
                         {{.name = "sinceRevision",
                           .type = "integer",
                           .summary = "Answer unchanged:true when the queue has not changed since "
                                      "this queue revision.",
                           .range = ">= 0"}});
        if (name == "export.events" || name == "export.log")
            addArguments(value, {{.name = "afterSequence",
                                  .type = "integer",
                                  .summary = "Skip entries up to and including this sequence.",
                                  .range = ">= 0",
                                  .defaultValue = "0"}});
        if (name == "export.wait")
            addArguments(value, {{.name = "job",
                                  .type = "integer",
                                  .summary = "The job to wait for.",
                                  .unit = "export job id",
                                  .range = ">= 1",
                                  .defaultValue = "every active job"},
                                 {.name = "timeoutMs",
                                  .type = "integer",
                                  .summary = "The longest wait.",
                                  .unit = "milliseconds",
                                  .range = "1..86400000"}});
        if (name == "export.presets")
            addArguments(value,
                         {{.name = "job",
                           .type = "integer",
                           .summary = "Make availability reflect this job's composition (for "
                                      "example a source whose audio the export cannot decode "
                                      "disables every preset that carries audio).",
                           .unit = "export job id",
                           .range = ">= 1"}});
        if (name == "export.script")
            addArguments(value, {{.name = "job",
                                  .type = "integer",
                                  .required = true,
                                  .summary = "The job to recreate.",
                                  .unit = "export job id",
                                  .range = ">= 1"},
                                 {.name = "format",
                                  .type = "string",
                                  .summary = "The script language.",
                                  .defaultValue = "cli-json",
                                  .choices = {{"cli-json", "A bloom-cli run script"},
                                              {"python", "A Python script"}}}});
        value.result = item.readOnlyCall
                           ? "{succeeded, ...}: the queue snapshot, events, log lines, presets or "
                             "script text."
                           : "{accepted, revision, availability?:{code, reason}}: accepted is "
                             "false with the queue's reason when the call is refused.";
        if (name == "export.script")
            value.result =
                "{succeeded, format, script}. `script` is text. cli-json: a JSON array for "
                "bloom-cli run, two steps: {\"op\":\"bloom.export.add-job\",\"args\":{composition, "
                "name?, range, first?, last?, enabled, labelColor?, outputs:[...]}} and "
                "{\"op\":\"export.start\",\"args\":{}}; each element of `outputs` is a string "
                "holding one output as JSON ({preset, settings, destination:{portability, path, "
                "overwrite}, enabled, importIntoAssets}), the form bloom.export.add-job takes. "
                "python: a script that imports bloom, calls bloom.ops.export.add_job(...) with the "
                "same arguments and ends with bloom.export.start().";
        value.errors = {makeError("bloom.mcp.export-unavailable",
                                  "the queue refused (unknown job, wrong state, platform)",
                                  "diagnostic.detail names the queue's availability code")};
        std::string call = R"({"name":")" + std::string(name) + R"(","arguments":{)";
        bool firstArgument = true;
        for (const auto& argument : value.arguments) {
            if (!argument.required)
                continue;
            call += (firstArgument ? "" : ",") + jsonQuote(argument.name) + ":" +
                    (argument.type == "array" ? "[1]" : "1");
            firstArgument = false;
        }
        value.examples.push_back({.surface = "mcp", .text = call + "}}"});
        addRelated(value, {"export.status", "bloom.export.add-job"});
        out.push_back(std::move(value));
    }
}

} // namespace

ReferenceArgument argument(const ArgumentSpec& spec) {
    ReferenceArgument value;
    value.name = std::string(spec.name);
    value.type = std::string(spec.type);
    value.required = spec.required;
    value.summary = std::string(spec.summary);
    value.unit = std::string(spec.unit);
    value.range = std::string(spec.range);
    value.defaultValue = std::string(spec.defaultValue);
    for (const auto& [choice, label] : spec.choices)
        value.choices.push_back({.value = jsonQuote(choice), .label = std::string(label)});
    return value;
}

std::vector<ReferenceError> commonToolErrors() {
    return {
        makeError("bloom.mcp.project-unavailable",
                  "a project is opening, closing or being replaced", "retry after a moment", true),
        makeError("bloom.mcp.project-replaced", "the project was replaced while the call ran",
                  "re-query ids and the revision, then retry", true),
        makeError("bloom.mcp.commit-queue-full", "too many edits are waiting (16)",
                  "retry after a moment", true),
        makeError("bloom.mcp.gesture-open",
                  "the artist kept a drag or text edit open past the 30 s deadline",
                  "retry when the artist is done", true),
        makeError("bloom.mcp.deadline-exceeded", "Bloom was busy past the request's deadline",
                  "retry", true),
        makeError("bloom.mcp.cancelled", "notifications/cancelled, server stop or shutdown",
                  "nothing was applied; repeat the call if still wanted"),
        makeError("bloom.mcp.read-only", "the project is read-only (a newer or preserved document)",
                  "save a copy or open another project; edits cannot apply"),
    };
}

std::vector<ReferenceError> commonOperationErrors() {
    return {makeError("bloom.scripting.invalid-argument",
                      "an argument is missing, of the wrong kind or out of range",
                      "the message names the argument and a working example call"),
            makeError("bloom.scripting.command-rejected",
                      "the document refused the edit (a stale id, a locked layer, an impossible "
                      "value)",
                      "re-read the ids with the related query and fix the value"),
            makeError("bloom.scripting.stale-revision",
                      "the project moved on since expectedRevision",
                      "re-query and retry with the current revision", true)};
}

std::vector<ReferenceEntry> surfaceEntries() {
    std::vector<ReferenceEntry> out;
    addConcepts(out);
    addTools(out);
    addSessionToolEntries(out);
    addQueryKindEntries(out);
    addCliCommandEntries(out);
    addPythonEntries(out);
    return out;
}

} // namespace bloom::scripting::detail
