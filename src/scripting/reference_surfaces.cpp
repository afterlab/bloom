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
                  "id. Operations that create objects return the new ids in the transaction "
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
        "re-query what you depend on and retry with the new revision.");
    addRelated(revisions, {"transact", "history", "events"});
    out.push_back(std::move(revisions));

    auto context = makeEntry(
        ReferenceKind::Concept, "concept:context", "concepts",
        "Omitted contextual arguments take the active composition, playhead and selection.",
        "Arguments whose reference says `context` may be omitted. They are filled from the live "
        "editor in the desktop application (the composition the Viewer shows, the playhead, the "
        "selected layers or nodes) and from the session defaults on stdio and in bloom-cli (the "
        "first composition, time 0, no selection). Read the values with query:context. An "
        "argument you pass always wins.");
    addRelated(context, {"query:context"});
    out.push_back(std::move(context));

    auto errors = makeEntry(
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
    addArguments(query,
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
                               {"enums", "Keyframe interpolation, ease and handle-mode tokens"}}},
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
                   .unit = "node type id such as bloom.solid-source"},
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
    query.errors = {
        makeError("-32602", "an unknown kind, or an argument the kind does not take",
                  "use one of the listed kinds; help query:<kind> lists its arguments"),
        makeError("-32602", "a stale composition, parameter or curve id",
                  "re-read the ids with query:compositions, query:layers or query:nodes")};
    addExamples(query,
                {{"mcp", R"({"name":"query","arguments":{"kind":"layers","composition":1}})"}});
    addRelated(query, {"query:context", "query:compositions", "query:layers", "query:nodes",
                       "query:keyframes", "query:node-types"});
    out.push_back(std::move(query));

    auto transact = makeEntry(
        ReferenceKind::Tool, "transact", "document",
        "Apply a list of operations atomically as one undo step.",
        "Every operation in `operations` is validated first; if any is invalid, nothing is "
        "applied. They then run in order as one command with one undo entry named `label`. In "
        "the desktop application the edit lands in the artist's undo history and waits for an "
        "open drag or text edit to finish (up to 30 s). Operation ids and their arguments: "
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
                          "stdio"}});
    transact.prerequisites = {"Read the current revision (any query result)."};
    transact.result =
        "{succeeded, revision, diagnostics:[{code, severity?, operationIndex, operation, "
        "argument?, message}], outputs:[{operationIndex, name, id}]}. `revision` is the new "
        "revision; `outputs` lists every id the operations created, by the zero-based index of "
        "the operation that made it (an operation's reference names its outputs).";
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
                  "nothing to retry; the message says what was skipped")};
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
    history.result = "The transact result shape: {succeeded, revision, diagnostics, outputs}.";
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
        "Poll what changed: command events after a revision and sequence.",
        "Each event is a command result with `sequence` and `kind` (revision: an edit applied; "
        "history: an undo or redo; rejected: an edit refused). Pass the last `sequence` you saw "
        "as afterSequence. When `resetRequired` is true (events were trimmed or the project was "
        "replaced) re-query whatever you cache and start again from the returned sequence.");
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
                           .defaultValue = "0"}});
    events.result = "{revision, sequence, resetRequired, events:[{sequence, kind, succeeded, "
                    "revision, diagnostics, outputs}]}";
    addExamples(events, {{"mcp", R"({"name":"events","arguments":{"sinceRevision":0}})"}});
    addRelated(events, {"concept:revisions"});
    out.push_back(std::move(events));

    const auto stillPresets = Choices{
        {"PngRgba8SrgbV1", "PNG, 8-bit RGBA, sRGB"},
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
                      {"refuse", "Fail when a file exists"}}}});
    render.result = "{publishedFrames, preservationReport, files:[{path, "
                    "digest:\"sha256:<hex>\"}]}";
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
    const auto renderResult = render.result;
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
                            .defaultValue = "the context time (frame 0 on stdio)"}});
    capture.result = "MCP image content (image/png, or a resource_link when too large) plus "
                     "{composition, frame, path, bytes, digest, width, height, inline}.";
    capture.errors = renderErrors;
    addExamples(capture,
                {{"mcp", R"({"name":"capture","arguments":{"composition":1,"frame":12}})"}});
    addRelated(capture, {"render", "query:context"});
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
        {"export.presets",
         "The export presets available on this computer, optionally for one job's composition.",
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
                                  .range = "1..1024 ids"}});
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
