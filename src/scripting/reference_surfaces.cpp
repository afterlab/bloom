// The reference entries that do not come from the operation registry: the shared concepts, the MCP
// tools and their query kinds, the bloom-cli commands and the Python entry points. The MCP tool
// schemas (src/mcp/tool_schemas.cpp) are checked against these entries by bloom.mcp.reference, so
// an argument cannot be added to a tool without being described here.

#include "reference_internal.hpp"

#include <array>
#include <utility>

namespace bloom::scripting::detail {
namespace {

using Choices = std::initializer_list<std::pair<std::string_view, std::string_view>>;

ReferenceEntry entry(const ReferenceKind kind, std::string name, std::string area,
                     std::string summary, std::string description = {}) {
    ReferenceEntry value;
    value.kind = kind;
    value.name = std::move(name);
    value.area = std::move(area);
    value.summary = std::move(summary);
    value.description = std::move(description);
    return value;
}

ReferenceError error(std::string code, std::string when, std::string remedy,
                     const bool retryable = false) {
    return {.code = std::move(code),
            .when = std::move(when),
            .remedy = std::move(remedy),
            .retryable = retryable};
}

void examples(ReferenceEntry& value,
              std::initializer_list<std::pair<std::string_view, std::string_view>> list) {
    for (const auto& [surface, text] : list)
        value.examples.push_back({.surface = std::string(surface), .text = std::string(text)});
}

void related(ReferenceEntry& value, std::initializer_list<std::string_view> names) {
    for (const auto name : names)
        value.related.emplace_back(name);
}

void args(ReferenceEntry& value, std::initializer_list<ArgumentSpec> specs) {
    for (const auto& spec : specs)
        value.arguments.push_back(argument(spec));
}

ReferenceEffects readOnly() { return {.readOnly = true}; }
ReferenceEffects edits() { return {.undoable = true, .dirtiesProject = true}; }
ReferenceEffects writesFiles() { return {.writesFiles = true}; }

// ---------------------------------------------------------------------------------------------
// Concepts

void addConcepts(std::vector<ReferenceEntry>& out) {
    auto ids = entry(ReferenceKind::Concept, "concept:ids", "concepts",
                     "Every object has a stable positive integer id that never changes or is "
                     "reused.",
                     "Ids are 1-based and stable across edits, saves and undo. Each kind has its "
                     "own id space: a layer id (query:layers .id) is not the id of the layer's "
                     "Layer Output node (.layerOutputNode), and a stack slot (.slot) is a third "
                     "id. Operations that create objects return the new ids in the transaction "
                     "result's `outputs` as {operationIndex, name, id}; read them there instead of "
                     "guessing. An id from a project that was since replaced is stale.");
    related(ids, {"query:layers", "query:nodes", "concept:revisions"});
    out.push_back(std::move(ids));

    auto time = entry(ReferenceKind::Concept, "concept:time", "concepts",
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
    related(time, {"query:compositions", "query:keyframes", "bloom.layer.set-range"});
    out.push_back(std::move(time));

    auto revisions = entry(
        ReferenceKind::Concept, "concept:revisions", "concepts",
        "Every edit names the revision it expects; a stale one changes nothing.",
        "Each project state has a revision number. Read it from any query result (`revision`), "
        "pass it as `expectedRevision` to transact or history, and use the result's `revision` "
        "for the next call. When someone else (the artist, a script, another agent) edits in "
        "between, the call fails with bloom.scripting.stale-revision and nothing is applied: "
        "re-query what you depend on and retry with the new revision.");
    related(revisions, {"transact", "history", "events"});
    out.push_back(std::move(revisions));

    auto context = entry(
        ReferenceKind::Concept, "concept:context", "concepts",
        "Omitted contextual arguments take the active composition, playhead and selection.",
        "Arguments whose reference says `context` may be omitted. They are filled from the live "
        "editor in the desktop application (the composition the Viewer shows, the playhead, the "
        "selected layers or nodes) and from the session defaults on stdio and in bloom-cli (the "
        "first composition, time 0, no selection). Read the values with query:context. An "
        "argument you pass always wins.");
    related(context, {"query:context"});
    out.push_back(std::move(context));

    auto errors = entry(
        ReferenceKind::Concept, "concept:errors", "concepts",
        "How failures are reported and which ones to retry.",
        "A malformed request (unknown tool, unknown argument, wrong JSON type) is a JSON-RPC "
        "error with code -32602 and a message naming the field. A tool that ran but failed "
        "answers isError:true with structuredContent {succeeded:false, diagnostic:{code, "
        "message}} or, for transact, {succeeded:false, diagnostics:[{code, operationIndex, "
        "operation, argument, message}]}. Retry bloom.mcp.project-unavailable, "
        "bloom.mcp.commit-queue-full, bloom.mcp.gesture-open, bloom.mcp.deadline-exceeded and "
        "bloom.scripting.stale-revision (after re-reading the revision); fix the request for "
        "every other code.");
    errors.errors = commonToolErrors();
    related(errors, {"transact", "help"});
    out.push_back(std::move(errors));
}

// ---------------------------------------------------------------------------------------------
// MCP tools

void addTools(std::vector<ReferenceEntry>& out) {
    auto help = entry(ReferenceKind::Tool, "help", "discovery",
                      "List every tool, query kind, operation and command, or show one entry in "
                      "full.",
                      "Without `name`, returns the index: every entry grouped by kind and area "
                      "with a one-line summary. With `name` (a tool, an operation id such as "
                      "bloom.layer.add-solid, a query kind such as query:layers, a bloom-cli "
                      "command or a concept such as concept:time), returns its full reference: "
                      "arguments with type, unit, range, default and accepted values, side "
                      "effects, prerequisites, result shape, errors with remedies, examples and "
                      "related entries. bloom-cli help and bloom.help() print the same text.");
    help.effects = readOnly();
    args(help, {{.name = "name",
                 .type = "string",
                 .summary = "The entry to show. Omit for the index.",
                 .unit = "an entry name; operation ids also accept bloom.ops.* and "
                         "domain.verb spellings"}});
    help.result = "{found, name, text, entry|index|suggestions}: `text` is the readable "
                  "reference; `entry` the same entry as structured data; `suggestions` the "
                  "closest names when nothing matched.";
    examples(help, {{"mcp", R"({"name":"help","arguments":{"name":"bloom.layer.add-solid"}})"},
                    {"python", "bloom.help(\"bloom.layer.add-solid\")"},
                    {"cli", "bloom-cli help bloom.layer.add-solid"}});
    related(help, {"query:operations", "query:node-types"});
    out.push_back(std::move(help));

    auto query = entry(
        ReferenceKind::Tool, "query", "document",
        "Read the project: compositions, layers, nodes, parameters, keyframes, assets, node "
        "types, operations and the editor context.",
        "One read-only call per `kind`; each kind has its own entry (query:layers, "
        "query:keyframes, ...) with its arguments and result. Every result carries the "
        "`revision` it was read at. Arguments a kind does not take are refused with a message "
        "naming them.");
    query.effects = readOnly();
    args(query, {{.name = "kind",
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
                              {"context", "The editor context contextual arguments use"}}},
                 {.name = "composition",
                  .type = "integer",
                  .summary = "The composition, for layers, nodes, parameters, parameter, "
                             "parameter-value and keyframes.",
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
                  .unit = "node type id such as bloom.solid"},
                 {.name = "pageSize",
                  .type = "integer",
                  .summary = "Records per page, for parameters and keyframes.",
                  .range = "1..64",
                  .defaultValue = "64"},
                 {.name = "cursor",
                  .type = "string",
                  .summary = "The previous page's nextCursor. It is bound to the revision: any "
                             "edit in between makes it stale, so restart from the first page.",
                  .range = "at most 160 characters"},
                 {.name = "expectedRevision",
                  .type = "integer",
                  .summary = "For parameter and parameter-value: refuse when the project moved "
                             "on. Required for parameter-value.",
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
                  .defaultValue = "131072"}});
    query.result = "{revision, ...}: see the query:<kind> entries.";
    query.errors = {error("-32602", "an unknown kind, or an argument the kind does not take",
                          "use one of the listed kinds; help query:<kind> lists its arguments"),
                    error("-32602", "a stale composition, parameter or curve id",
                          "re-read the ids with query:compositions, query:layers or query:nodes")};
    examples(query, {{"mcp", R"({"name":"query","arguments":{"kind":"layers","composition":1}})"}});
    related(query, {"query:context", "query:compositions", "query:layers", "query:nodes",
                    "query:keyframes", "query:node-types"});
    out.push_back(std::move(query));

    auto transact = entry(
        ReferenceKind::Tool, "transact", "document",
        "Apply a list of operations atomically as one undo step.",
        "Every operation in `operations` is validated first; if any is invalid, nothing is "
        "applied. They then run in order as one command with one undo entry named `label`. In "
        "the desktop application the edit lands in the artist's undo history and waits for an "
        "open drag or text edit to finish (up to 30 s). Operation ids and their arguments: "
        "help with no name lists them, help <operation id> describes one, query:operations "
        "returns every schema. Omitted contextual arguments are filled from the editor context "
        "(concept:context).");
    transact.effects = edits();
    args(transact,
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
                           "stdio"}});
    transact.prerequisites = {"Read the current revision (any query result)."};
    transact.result =
        "{succeeded, revision, diagnostics:[{code, severity?, operationIndex, operation, "
        "argument?, message}], outputs:[{operationIndex, name, id}]}. `revision` is the new "
        "revision; `outputs` lists every id the operations created, by the zero-based index of "
        "the operation that made it (an operation's reference names its outputs).";
    transact.errors = {
        error("bloom.scripting.invalid-argument",
              "an argument is missing, of the wrong kind or out of range",
              "the message names the operation, the argument and a working example call"),
        error("bloom.scripting.unknown-operation", "the op id is not registered",
              "help with no name lists every operation"),
        error("bloom.scripting.stale-revision", "expectedRevision is not the current revision",
              "re-query and retry with the current revision", true),
        error("bloom.scripting.command-rejected",
              "the document refused an operation (a stale id, a locked layer, an impossible "
              "edit)",
              "read the message; re-query the ids it names"),
        error("bloom.scripting.operation-warning",
              "an operation applied but skipped part of its work (severity warning)",
              "nothing to retry; the message says what was skipped")};
    examples(
        transact,
        {{"mcp",
          R"({"name":"transact","arguments":{"expectedRevision":0,"label":"Add a red solid","operations":[{"op":"bloom.layer.add-solid","args":{"name":"Red","color":[1,0,0,1]}}]}})"},
         {"python", "with bloom.transactions.group(\"Add a red solid\"):\n"
                    "    bloom.ops.layer.add_solid(name=\"Red\", color=(1, 0, 0, 1))"},
         {"cli",
          R"(bloom-cli run edits.json   # [{"op":"bloom.layer.add-solid","args":{"name":"Red","color":[1,0,0,1]}}])"}});
    related(transact, {"concept:revisions", "concept:context", "history", "query:operations"});
    out.push_back(std::move(transact));

    auto history = entry(ReferenceKind::Tool, "history", "document",
                         "Undo or redo one step of the project's history.",
                         "Undoes or redoes the most recent step of ANY origin, including the "
                         "artist's own edits in the desktop application, so read events or "
                         "query the revision first and undo only a step you made.");
    history.effects = edits();
    args(history, {{.name = "action",
                    .type = "string",
                    .required = true,
                    .summary = "Which way to step.",
                    .choices = {{"undo", "Undo"}, {"redo", "Redo"}}},
                   {.name = "expectedRevision",
                    .type = "integer",
                    .required = true,
                    .summary = "The current revision; a stale one changes nothing.",
                    .range = ">= 0"}});
    history.result = "The transact result shape: {succeeded, revision, diagnostics, outputs}.";
    history.errors = {
        error("bloom.scripting.stale-revision", "the project moved on", "re-query and retry", true),
        error("bloom.scripting.command-rejected", "there is nothing to undo or redo",
              "nothing to do")};
    examples(history,
             {{"mcp", R"({"name":"history","arguments":{"action":"undo","expectedRevision":3}})"},
              {"python", "bloom.transactions.undo()"}});
    related(history, {"transact", "events"});
    out.push_back(std::move(history));

    auto events = entry(
        ReferenceKind::Tool, "events", "document",
        "Poll what changed: command events after a revision and sequence.",
        "Each event is a command result with `sequence` and `kind` (revision: an edit applied; "
        "history: an undo or redo; rejected: an edit refused). Pass the last `sequence` you saw "
        "as afterSequence. When `resetRequired` is true (events were trimmed or the project was "
        "replaced) re-query whatever you cache and start again from the returned sequence.");
    events.effects = readOnly();
    args(events, {{.name = "sinceRevision",
                   .type = "integer",
                   .required = true,
                   .summary = "Skip events whose revision is older.",
                   .range = ">= 0"},
                  {.name = "afterSequence",
                   .type = "integer",
                   .summary = "Skip events up to and including this sequence.",
                   .range = ">= 0",
                   .defaultValue = "0"}});
    events.result = "{revision, sequence, resetRequired, events:[{sequence, kind, succeeded, "
                    "revision, diagnostics, outputs}]}";
    examples(events, {{"mcp", R"({"name":"events","arguments":{"sinceRevision":0}})"}});
    related(events, {"concept:revisions"});
    out.push_back(std::move(events));

    const auto stillPresets = Choices{
        {"PngRgba8SrgbV1", "PNG, 8-bit RGBA, sRGB"},
        {"FlatExrRgba32fLinRec709SceneV1", "OpenEXR, 32-bit float RGBA, linear Rec.709 scene"},
        {"TiffRgba16SrgbV1", "TIFF, 16-bit RGBA, sRGB"}};
    auto render = entry(
        ReferenceKind::Tool, "render", "render",
        "Render one frame or an inclusive frame range to image files and return their digests.",
        "Runs as a transient export job with the Export panel's checks. Output keeps alpha; the "
        "composition background colour is not rendered. Frames are composition frame indices "
        "(0 is the first frame). Use capture to look at a frame, render for reference stills or "
        "sequences, export for movies and audio.");
    render.effects = writesFiles();
    args(render,
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
                       {"refuse", "Fail when a file exists"}}}});
    render.result = "{publishedFrames, preservationReport, files:[{path, "
                    "digest:\"sha256:<hex>\"}]}";
    render.errors = {
        error("bloom.mcp.export-unavailable",
              "the destination exists and overwrite is not replace (ask and refuse both stop)",
              "pass overwrite:\"replace\" or choose another destination"),
        error("bloom.mcp.export-failed", "the render or a destination check failed",
              "the message and diagnostic.detail say which frame or file"),
        error("bloom.mcp.export-unavailable", "the export runner is unavailable on this system",
              "diagnostic.detail names the reason (PlatformNotSupported on macOS and Windows)"),
        error("bloom.mcp.cancelled", "notifications/cancelled or shutdown stopped the job",
              "frames already published remain; render again for the rest", true)};
    examples(
        render,
        {{"mcp",
          R"({"name":"render","arguments":{"composition":1,"frame":0,"preset":"PngRgba8SrgbV1","destination":"frame.png","overwrite":"replace"}})"},
         {"python", "bloom.render.frame(0, out=\"frames\")"},
         {"cli", "bloom-cli render --project scene.bloom --frame 0 --preset PngRgba8SrgbV1 "
                 "--out frames"}});
    related(render, {"capture", "export", "export.confirm-replace"});
    const auto renderErrors = render.errors;
    const auto renderResult = render.result;
    out.push_back(std::move(render));

    auto exportTool = entry(
        ReferenceKind::Tool, "export", "render",
        "Export a composition to a movie, audio file or image files in one call.",
        "Runs as a transient export job. Output keeps alpha; the composition background colour "
        "is not rendered. For a job that stays in the project's export queue (and the Export "
        "panel), use bloom.export.add-job and export.start instead.");
    exportTool.effects = writesFiles();
    args(exportTool,
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
           .summary = "The output format.",
           .choices = {{"PngRgba8SrgbV1", "PNG, 8-bit RGBA, sRGB"},
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
          {.name = "overwrite",
           .type = "string",
           .summary = "What to do when a file exists. Over MCP, ask behaves as refuse; pass "
                      "replace to replace it.",
           .defaultValue = "ask",
           .choices = {{"ask", "Fail and name the existing file"},
                       {"replace", "Replace existing files"},
                       {"refuse", "Fail when a file exists"}}}});
    exportTool.result = renderResult;
    exportTool.errors = renderErrors;
    examples(
        exportTool,
        {{"mcp",
          R"({"name":"export","arguments":{"composition":1,"first":0,"last":47,"preset":"ProResMovV1","destination":"review.mov"}})"}});
    related(exportTool, {"render", "bloom.export.add-job", "export.start", "export.presets"});
    out.push_back(std::move(exportTool));

    auto capture = entry(
        ReferenceKind::Tool, "capture", "render",
        "Look at a frame: render one PNG at composition resolution and return it as an image.",
        "Uses the still export route with the Viewer's display and view in the desktop "
        "application. Output keeps alpha; the composition background colour is not rendered. "
        "It does not apply the Viewer's exposure, gamma, channel, region of interest or preview "
        "resolution. A PNG above about 2.9 MiB is returned as a resource_link with the notice "
        "bloom.mcp.capture-too-large-for-inline. Captures go to Bloom's per-user cache, which "
        "keeps the newest 16.");
    capture.effects = {.readOnly = true, .writesFiles = true};
    args(capture, {{.name = "composition",
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
                    .defaultValue = "the context time (frame 0 on stdio)"}});
    capture.result = "MCP image content (image/png, or a resource_link when too large) plus "
                     "{composition, frame, path, bytes, digest, width, height, inline}.";
    capture.errors = renderErrors;
    examples(capture, {{"mcp", R"({"name":"capture","arguments":{"composition":1,"frame":12}})"}});
    related(capture, {"render", "query:context"});
    out.push_back(std::move(capture));

    // The export queue's session calls (ADR 0033).
    struct QueueTool final {
        std::string_view name;
        std::string_view summary;
        bool readOnlyCall;
    };
    constexpr std::array<QueueTool, 14> queue{{
        {"export.start", "Start export queue jobs, as the Export panel's Start does.", false},
        {"export.start-all", "Start every enabled export queue job.", false},
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
        {"export.presets", "The export presets available on this computer.", true},
        {"export.script",
         "A script that recreates an export job and starts it (bloom-cli JSON or Python).", true},
    }};
    for (const auto& item : queue) {
        auto value = entry(ReferenceKind::Tool, std::string(item.name), "export queue",
                           std::string(item.summary),
                           "Job and output ids come from bloom.export.add-job's outputs or from "
                           "export.status. Jobs started this way belong to the Export panel and "
                           "keep running when the agent disconnects. Output keeps alpha; the "
                           "composition background colour is not rendered.");
        value.effects = item.readOnlyCall ? readOnly() : writesFiles();
        const auto name = item.name;
        if (name == "export.start")
            args(value, {{.name = "jobs",
                          .type = "array",
                          .required = true,
                          .summary = "The jobs to start, in run order.",
                          .unit = "export job ids (export.status)",
                          .range = "1..1024 ids"}});
        if (name == "export.pause" || name == "export.resume" || name == "export.stop" ||
            name == "export.retry" || name == "export.confirm-replace" || name == "export.log")
            args(value, {{.name = "job",
                          .type = "integer",
                          .required = true,
                          .summary = "The job.",
                          .unit = "export job id (export.status)",
                          .range = ">= 1"}});
        if (name == "export.confirm-replace")
            args(value, {{.name = "output",
                          .type = "integer",
                          .required = true,
                          .summary = "The output whose existing destination may be replaced.",
                          .unit = "export output id (export.status)",
                          .range = ">= 1"}});
        if (name == "export.status")
            args(value, {{.name = "sinceRevision",
                          .type = "integer",
                          .summary = "Answer unchanged:true when the queue has not changed since "
                                     "this queue revision.",
                          .range = ">= 0"}});
        if (name == "export.events" || name == "export.log")
            args(value, {{.name = "afterSequence",
                          .type = "integer",
                          .summary = "Skip entries up to and including this sequence.",
                          .range = ">= 0",
                          .defaultValue = "0"}});
        if (name == "export.wait")
            args(value, {{.name = "job",
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
        if (name == "export.script")
            args(value, {{.name = "job",
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
        value.errors = {error("bloom.mcp.export-unavailable",
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
        related(value, {"export.status", "bloom.export.add-job"});
        out.push_back(std::move(value));
    }
}

// ---------------------------------------------------------------------------------------------
// Query kinds

void addQueryKinds(std::vector<ReferenceEntry>& out) {
    const auto kind = [&out](std::string name, std::string summary, std::string description,
                             std::string result, std::initializer_list<ArgumentSpec> arguments,
                             std::string_view example) {
        auto value = entry(ReferenceKind::QueryKind, "query:" + name, "document",
                           std::move(summary), std::move(description));
        value.effects = readOnly();
        args(value, {{.name = "kind",
                      .type = "string",
                      .required = true,
                      .summary = "This kind.",
                      .choices = {}}});
        value.arguments.back().choices.push_back({.value = jsonQuote(name), .label = name});
        args(value, arguments);
        value.result = std::move(result);
        value.examples.push_back({.surface = "mcp", .text = std::string(example)});
        value.related.emplace_back("query");
        out.push_back(std::move(value));
    };
    constexpr ArgumentSpec composition{.name = "composition",
                                       .type = "integer",
                                       .required = true,
                                       .summary = "The composition.",
                                       .unit = "composition id (query:compositions)",
                                       .range = ">= 1"};
    constexpr ArgumentSpec page{.name = "pageSize",
                                .type = "integer",
                                .summary = "Records per page.",
                                .range = "1..64",
                                .defaultValue = "64"};
    constexpr ArgumentSpec cursor{.name = "cursor",
                                  .type = "string",
                                  .summary = "The previous page's nextCursor; stale after any "
                                             "edit."};
    constexpr std::string_view time = "{frames, seconds:[n, d]}";

    kind("project", "The project's id and name.", {}, "{revision, project:{id, name}}", {},
         R"({"name":"query","arguments":{"kind":"project"}})");
    kind("compositions",
         "Every composition with its size, frame rate, duration, work area and settings.",
         "Times are reported as {frames, seconds:[n, d]} (concept:time), in the composition's "
         "own frame rate.",
         "{revision, records:[{id, name, width, height, frameRate:[n, d], fps, "
         "frameRateNumerator, frameRateDenominator, pixelAspect:[n, d], duration:" +
             std::string(time) + ", start:" + std::string(time) +
             " (the timecode label of frame 0), dropFrame, workArea:{start, end}|null (end "
             "exclusive), background:[r, g, b, a] (display-only), workingColorSpace|null, "
             "safeAreas:{action, title}, layerCount, folder|null}]}",
         {}, R"({"name":"query","arguments":{"kind":"compositions"}})");
    kind("layers", "A composition's layers, topmost first, with timing, masks and parameter ids.",
         "Each layer's `parameters` maps its transform roles (position, anchor, scale, rotation, "
         "opacity, blendMode, timeRemap) to parameter ids; `sourceParameters` maps the source "
         "node's roles (a solid's color, a text layer's text, size, font, ...) the same way. "
         "Pass those ids to bloom.parameter.set-source or the animation operations.",
         "{revision, composition, records:[{id, name, index (0 is topmost), slot, merge, "
         "layerOutputNode, sourceNode|null, sourceType|null, enabled, solo, locked, "
         "labelColor:[r, g, b]|null, parent|null, start:" +
             std::string(time) + ", end:" + std::string(time) +
             " (exclusive), timing:{startTime, speed:[n, d], before, after, extent}, "
             "masks:[{id, name, mode, inverted, path, feather, expansion, opacity}], "
             "trackMatte:{source, channel, inverted}|null, parameters:{role: id}, "
             "sourceParameters:{role: id}}]}",
         {composition}, R"({"name":"query","arguments":{"kind":"layers","composition":1}})");
    kind("nodes", "A composition's node graph: nodes, ports, links, card positions and groups.",
         "Input ports list what feeds them; output ports list where they lead. Merge nodes also "
         "list their layer stack. `links` repeats every edge once.",
         "{revision, composition, records:[{id, typeId, name, category, parameters:{role: id}, "
         "position:[x, y], width, collapsed, muted, group|null, inputs:[{port, kind, required, "
         "source:{node, port}|null}], outputs:[{port, kind, targets:[{node, port}|{node, role, "
         "slot}]}], stack?:[{slot, layer, index}]}], links:[{id, source:{node, port}, "
         "target:{node, port}|{node, role, slot}}], groups:[{id, name, members}]}",
         {composition}, R"({"name":"query","arguments":{"kind":"nodes","composition":1}})");
    kind("node-types", "Every node type bloom.node.add accepts, or one type in full.",
         "Without nodeType: every type with its category, ports and parameter roles. With "
         "nodeType: the type's ports and every parameter with value kind, default, unit, range, "
         "accepted values and whether it can be animated. bloom://reference/nodes has the same "
         "content as Markdown.",
         "{revision, records:[{id, name, category, schemaVersion, cardinality, inputs:[{port, "
         "kind, required}], outputs:[{port, kind}], parameters:[{role, schemaKey, valueKind, "
         "animatable, default, unit?, minimum?, maximum?, choices?}], layerSlots?}]} or, with "
         "nodeType, {revision, record}",
         {{.name = "nodeType",
           .type = "string",
           .summary = "One type to describe in full.",
           .unit = "node type id such as bloom.solid"}},
         R"({"name":"query","arguments":{"kind":"node-types","nodeType":"bloom.solid"}})");
    kind("parameters", "A composition's parameters: full records, or summary pages.",
         "Without pageSize or cursor: every full record. With them: bounded summaries whose "
         "constant values are omitted (valueOmitted:true) and a nextCursor until it is null.",
         "{revision, records:[{id, schemaKey, source:\"constant\"|\"curve\"|\"driver\", value?, "
         "curveId?, driverNode?, driverPort?}], nextCursor?, totalRecords?}",
         {composition, page, cursor},
         R"({"name":"query","arguments":{"kind":"parameters","composition":1,"pageSize":64}})");
    kind("parameter",
         "One parameter's record and its metadata: value kind, unit, range, default, accepted "
         "values and owner.",
         "Write values in stored units, the units minimum, maximum and default are in. `unit` "
         "and `displayScale` are how the Properties panel shows the value (displayed = stored x "
         "displayScale: an opacity stored as 0.5 shows as 50 %). Enumerations report `choices` "
         "as {value, label}: write the integer value with bloom.parameter.set-source.",
         "{revision, composition, parameter, record:{id, schemaKey, source, value|curveId|...}, "
         "metadata:{valueKind, animatable, default, unit, displayScale, minimum, maximum, "
         "softMinimum, softMaximum, choices:[{value, label}], owner:{node, typeId, role}|{layer, "
         "mask, role}|null}}",
         {composition,
          {.name = "parameter",
           .type = "integer",
           .required = true,
           .summary = "The parameter.",
           .unit = "parameter id",
           .range = ">= 1"},
          {.name = "expectedRevision",
           .type = "integer",
           .summary = "Refuse when the project moved on.",
           .range = ">= 0"}},
         R"({"name":"query","arguments":{"kind":"parameter","composition":1,"parameter":5}})");
    kind(
        "parameter-value", "A long constant string parameter, in UTF-8 chunks.",
        "Start at offset 0 and pass each nextOffset until complete is true. Chunks end on a "
        "UTF-8 boundary.",
        "{revision, composition, parameter, offset, nextOffset, totalBytes, complete, chunk}",
        {composition,
         {.name = "parameter",
          .type = "integer",
          .required = true,
          .summary = "The parameter.",
          .unit = "parameter id",
          .range = ">= 1"},
         {.name = "expectedRevision",
          .type = "integer",
          .required = true,
          .summary = "The revision the chunks must come from.",
          .range = ">= 0"},
         {.name = "offset",
          .type = "integer",
          .summary = "Byte offset.",
          .unit = "UTF-8 bytes",
          .range = ">= 0",
          .defaultValue = "0"},
         {.name = "limit",
          .type = "integer",
          .summary = "Most bytes.",
          .unit = "UTF-8 bytes",
          .range = "4..131072",
          .defaultValue = "131072"}},
        R"({"name":"query","arguments":{"kind":"parameter-value","composition":1,"parameter":9,"expectedRevision":4}})");
    kind("keyframes", "An animated parameter's keys: time, value and interpolation, paged.",
         "Address the curve by `parameter` (an animated parameter) or by `curve`. Vector and "
         "colour curves list one row per component key, with `component` naming the axis or "
         "channel. Times are {frames, seconds:[n, d]} at the composition's rate (concept:time); "
         "values are in the parameter's own units.",
         "{revision, composition, parameter|null, curve, valueKind, frameRate:[n, d], "
         "records:[{keyframe, component|null, time:" +
             std::string(time) +
             ", value, interpolation:\"hold\"|\"linear\"|\"ease-in-out\", "
             "outgoingHandle:{time, value}, incomingHandle:{time, value}}], totalRecords, "
             "nextCursor|null}",
         {composition,
          {.name = "parameter",
           .type = "integer",
           .summary = "The animated parameter (or give curve).",
           .unit = "parameter id",
           .range = ">= 1"},
          {.name = "curve",
           .type = "integer",
           .summary = "The curve (or give parameter).",
           .unit = "animation curve id",
           .range = ">= 1"},
          page,
          cursor},
         R"({"name":"query","arguments":{"kind":"keyframes","composition":1,"parameter":5}})");
    kind("assets", "Every asset: kind, file, size, folder and tags.", {},
         "{revision, records:[{id, name, kind:\"image\"|\"sequence\"|\"audio\"|\"font\"|\"video\""
         "|\"lut\", path, width, height, frames, digest, folder|null, tags}], folders:[{id, "
         "name, parent|null}]}",
         {}, R"({"name":"query","arguments":{"kind":"assets"}})");
    kind("operations", "Every operation schema transact accepts.",
         "The same schemas help describes, as data.",
         "{revision, records:[{id, description, arguments:[{name, kind, required, contextual, "
         "context, summary, unit?, minimum?, maximum?, default?, choices?}], example}]}",
         {}, R"({"name":"query","arguments":{"kind":"operations"}})");
    kind("context", "The editor context omitted contextual arguments fall back to.",
         "In the desktop application: the Viewer's composition, the playhead, the selection and "
         "the Viewer's display and view. On stdio: the session defaults (concept:context).",
         "{revision, live, project:{id, name}, composition|null, selectedNodes, selectedLayers, "
         "time:[n, d] (seconds), display, view, readOnly}",
         {}, R"({"name":"query","arguments":{"kind":"context"}})");
}

// ---------------------------------------------------------------------------------------------
// bloom-cli commands

void addCliCommands(std::vector<ReferenceEntry>& out) {
    const auto command = [&out](std::string name, std::string summary, std::string description,
                                std::initializer_list<ArgumentSpec> arguments, std::string result,
                                std::string_view example, const ReferenceEffects effects) {
        auto value = entry(ReferenceKind::CliCommand, "bloom-cli " + name, "bloom-cli",
                           std::move(summary), std::move(description));
        args(value, arguments);
        value.result = std::move(result);
        value.effects = effects;
        value.examples.push_back({.surface = "cli", .text = std::string(example)});
        out.push_back(std::move(value));
    };
    command("help", "Show the reference index, or one entry in full.",
            "The same reference the MCP help tool and bloom.help() print. --json prints it as "
            "JSON.",
            {{.name = "name", .type = "string", .summary = "The entry; omit for the index."},
             {.name = "--json", .type = "flag", .summary = "Print JSON instead of text."}},
            "The reference text on stdout; exit status 1 when the name matches nothing (the "
            "closest names are printed).",
            "bloom-cli help bloom.layer.add-solid", readOnly());
    command("new", "Create an empty project file.", {},
            {{.name = "file", .type = "path", .required = true, .summary = "The .bloom file."}},
            "\"created <file>\"; exit status 2 or 3 on failure.", "bloom-cli new scene.bloom",
            writesFiles());
    command("open", "Open a project and print its revision (a validity check).", {},
            {{.name = "file", .type = "path", .required = true, .summary = "The .bloom file."}},
            "\"opened <file> revision=N\"; exit status 2 when it cannot be opened.",
            "bloom-cli open scene.bloom", readOnly());
    command(
        "run", "Run a JSON operation script or a Python script.",
        "A .json script is an array of {\"op\", \"args\"} objects; each runs as its own "
        "transaction, in a new empty project that is not saved, and export.* calls drive a "
        "headless export queue. A .py script runs in the embedded interpreter, where "
        "bloom.Host(project) opens and saves projects.",
        {{.name = "script",
          .type = "path",
          .required = true,
          .summary = "A .json operation script or a .py Python script."}},
        "\"executed N transaction(s), revision=R\" for JSON; exit status 4 (invalid "
        "operation), 5 (rejected) or 6 (export call refused) names the failing operation.",
        R"(bloom-cli run edits.json   # [{"op":"bloom.composition.add","args":{"name":"Scene"}}])",
        {});
    command("python", "Start an interactive Python prompt with the bloom package.", {}, {},
            "An interactive interpreter.", "bloom-cli python", {});
    command("render", "Render one frame or a frame range of a project's first composition.",
            "Output keeps alpha; the composition background colour is not rendered. Files are "
            "written as <out>/frame.png or frame.exr (a range numbers them).",
            {{.name = "--project",
              .type = "path",
              .summary = "The project; omit to render a new empty project."},
             {.name = "--frame",
              .type = "integer",
              .summary = "One frame (or --range).",
              .unit = "composition frame index"},
             {.name = "--range",
              .type = "A-B",
              .summary = "An inclusive frame range (or --frame).",
              .unit = "composition frame indices"},
             {.name = "--preset",
              .type = "string",
              .required = true,
              .summary = "The image format.",
              .choices = {{"PngRgba8SrgbV1", "PNG, 8-bit RGBA, sRGB"},
                          {"FlatExrRgba32fLinRec709SceneV1",
                           "OpenEXR, 32-bit float RGBA, linear Rec.709 scene"}}},
             {.name = "--out",
              .type = "path",
              .required = true,
              .summary = "The output folder; created when missing."}},
            "The preservation report and \"rendered N frame(s) to <out>\"; exit status 5 on a "
            "render failure.",
            "bloom-cli render --project scene.bloom --range 0-47 --preset PngRgba8SrgbV1 --out "
            "frames",
            writesFiles());
    command("export", "Render a saved project's export queue jobs headlessly.",
            "Exits non-zero unless every selected job succeeds and prints the verification and "
            "import results as JSON. Output keeps alpha; the composition background colour is "
            "not rendered.",
            {{.name = "--project", .type = "path", .required = true, .summary = "The project."},
             {.name = "--all", .type = "flag", .summary = "Every enabled job (or --job)."},
             {.name = "--job",
              .type = "integer",
              .summary = "One job id; repeat for several.",
              .unit = "export job id"},
             {.name = "--replace",
              .type = "flag",
              .summary = "Confirm replacing every existing destination that asks."},
             {.name = "--save",
              .type = "flag",
              .summary = "Write the imports of verified results back to the project."}},
            "Export events as JSON; exit status non-zero when a job fails.",
            "bloom-cli export --project scene.bloom --all --replace", writesFiles());
}

// ---------------------------------------------------------------------------------------------
// Python

void addPython(std::vector<ReferenceEntry>& out) {
    const auto api = [&out](std::string name, std::string summary, std::string description,
                            std::string example) {
        auto value = entry(ReferenceKind::Python, std::move(name), "python", std::move(summary),
                           std::move(description));
        value.examples.push_back({.surface = "python", .text = std::move(example)});
        out.push_back(std::move(value));
    };
    api("bloom.help", "Print the reference index, or one entry in full.",
        "bloom.help(name=None, *, json=False). With json=True it returns the entry as a dict "
        "instead of printing.",
        "bloom.help(\"query:layers\")");
    api("bloom.ops", "Every operation as a Python function: bloom.ops.<domain>.<verb>.",
        "bloom.ops.layer.add_solid is bloom.layer.add-solid. Arguments bind positionally or by "
        "keyword; contextual arguments are keyword-only and default to bloom.context. Outside a "
        "transaction group each call is its own undo step and returns the result mapping.",
        "bloom.ops.layer.add_solid(name=\"Red\", color=(1, 0, 0, 1))");
    api("bloom.transactions.group", "Group operation calls into one atomic undo step.",
        "with bloom.transactions.group(label, expected_revision=None): ... The calls inside are "
        "queued and applied together when the block exits; `result` holds the transaction "
        "result. bloom.transactions.undo() and redo() step the history.",
        "with bloom.transactions.group(\"Title\") as edit:\n"
        "    bloom.ops.layer.add_text(name=\"Title\", text=\"Hello\")");
    api("bloom.context", "The editor context: project, composition, selection, layers, time.",
        "Read-only. time is a Fraction of seconds.", "bloom.context.composition");
    api("bloom.data", "Read the project: compositions, assets, layers, nodes, parameters, curves.",
        "bloom.data.layers(composition), bloom.data.curves(composition) and the other "
        "collections return proxies whose attributes read the current snapshot.",
        "bloom.data.layers(bloom.context.composition)");
    api("bloom.render", "Render a frame or a range to image files.",
        "bloom.render.frame(number, *, out, composition=None, preset=...) and "
        "bloom.render.range(first, last, *, out, ...). Output keeps alpha; the composition "
        "background colour is not rendered.",
        "bloom.render.frame(0, out=\"frames\")");
    api("bloom.events", "Subscribe to command events.",
        "bloom.events.subscribe(callback, kinds=...) and bloom.events.pump().",
        "bloom.events.subscribe(print)");
    api("bloom.tasks", "Run cancellable background work with progress.",
        "bloom.tasks.submit(function, *args) returns a TaskHandle with progress, cancel() and "
        "result().",
        "bloom.tasks.submit(work)");
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
        error("bloom.mcp.project-unavailable", "a project is opening, closing or being replaced",
              "retry after a moment", true),
        error("bloom.mcp.project-replaced", "the project was replaced while the call ran",
              "re-query ids and the revision, then retry", true),
        error("bloom.mcp.commit-queue-full", "too many edits are waiting (16)",
              "retry after a moment", true),
        error("bloom.mcp.gesture-open",
              "the artist kept a drag or text edit open past the 30 s deadline",
              "retry when the artist is done", true),
        error("bloom.mcp.deadline-exceeded", "Bloom was busy past the request's deadline", "retry",
              true),
        error("bloom.mcp.cancelled", "notifications/cancelled, server stop or shutdown",
              "nothing was applied; repeat the call if still wanted"),
        error("bloom.mcp.read-only", "the project is read-only (a newer or preserved document)",
              "save a copy or open another project; edits cannot apply"),
    };
}

std::vector<ReferenceError> commonOperationErrors() {
    return {error("bloom.scripting.invalid-argument",
                  "an argument is missing, of the wrong kind or out of range",
                  "the message names the argument and a working example call"),
            error("bloom.scripting.command-rejected",
                  "the document refused the edit (a stale id, a locked layer, an impossible "
                  "value)",
                  "re-read the ids with the related query and fix the value"),
            error("bloom.scripting.stale-revision", "the project moved on since expectedRevision",
                  "re-query and retry with the current revision", true)};
}

std::vector<ReferenceEntry> surfaceEntries() {
    std::vector<ReferenceEntry> out;
    addConcepts(out);
    addTools(out);
    addQueryKinds(out);
    addCliCommands(out);
    addPython(out);
    return out;
}

} // namespace bloom::scripting::detail
