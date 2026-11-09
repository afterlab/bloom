// The reference entries of the MCP project and window tools (File menu project lifecycle, the
// active composition, selection, playhead and Viewer) and Project Settings > Colour.

#include "reference_internal.hpp"

#include <array>
#include <string>
#include <utility>

namespace bloom::scripting::detail {
namespace {

constexpr std::string_view kTimeUnit =
    "frames at the composition's frame rate (an integer), or exact seconds as "
    "[numerator, denominator]";

constexpr std::string_view kPixelUnit =
    "composition pixels: x to the right, y down, origin at the frame's top-left corner";

ReferenceError refusal(std::string code, std::string when, std::string remedy) {
    return makeError(std::move(code), std::move(when), std::move(remedy));
}

std::vector<ReferenceError> lifecycleErrors() {
    return {refusal("bloom.mcp.unsaved-changes", "the open project has unsaved changes",
                    "save it first with project.save, or pass discardChanges:true to throw the "
                    "changes away"),
            refusal("bloom.mcp.jobs-running", "export jobs are running",
                    "wait with export.wait, or pass stopJobs:true to stop them"),
            refusal("bloom.mcp.busy", "another project change is in progress", "retry shortly")};
}

std::vector<ReferenceError> windowErrors() {
    return {refusal("bloom.mcp.not-live",
                    "the call reached bloom-mcp on stdio, which has no window",
                    "use the in-app server of the desktop application"),
            refusal("bloom.mcp.ui-unbound", "the application has not connected this window tool",
                    "nothing to retry; the window cannot do this yet"),
            refusal("bloom.mcp.unknown-target", "a composition, layer or node id does not exist",
                    "re-read the ids with query:compositions, query:layers or query:nodes"),
            refusal("bloom.mcp.invalid-argument", "an argument is out of range or inconsistent",
                    "the message names the argument")};
}

void projectTools(std::vector<ReferenceEntry>& out) {
    const std::string projectResult =
        "{succeeded, event (project.saved, project.opened, project.created or project.closed), "
        "path|null, projectName, message?}; the event is also recorded for the events tool.";
    constexpr ArgumentSpec discard{.name = "discardChanges",
                                   .type = "boolean",
                                   .summary = "Throw away unsaved changes instead of refusing.",
                                   .defaultValue = "false"};
    constexpr ArgumentSpec stop{.name = "stopJobs",
                                .type = "boolean",
                                .summary = "Stop running export jobs instead of refusing.",
                                .defaultValue = "false"};

    auto save = makeEntry(ReferenceKind::Tool, "project.save", "project",
                          "Save the open project to its file (File > Save).",
                          "Not undoable. The project must have been saved before; otherwise use "
                          "project.save-as.");
    save.effects = fileEffects();
    save.result = projectResult;
    save.errors = {refusal("bloom.mcp.no-path", "the project has never been saved",
                           "use project.save-as with an absolute .bloom path"),
                   refusal("bloom.mcp.read-only", "the project is read-only",
                           "save a copy with project.save-as")};
    addExamples(save, {{"mcp", R"({"name":"project.save","arguments":{}})"}});
    addRelated(save, {"project.save-as", "query:project"});
    out.push_back(std::move(save));

    auto saveAs = makeEntry(ReferenceKind::Tool, "project.save-as", "project",
                            "Save the open project to a new .bloom file (File > Save As).",
                            "The project then lives at that path; later project.save calls write "
                            "there.");
    saveAs.effects = fileEffects();
    addArguments(saveAs, {{.name = "path",
                           .type = "string",
                           .required = true,
                           .summary = "The file to write.",
                           .unit = "an absolute path ending in .bloom whose folder exists",
                           .range = "1..4096 bytes"},
                          {.name = "replace",
                           .type = "boolean",
                           .summary = "Replace an existing file at path.",
                           .defaultValue = "false"}});
    saveAs.result = projectResult;
    saveAs.errors = {refusal("bloom.mcp.destination-exists", "a file exists at path",
                             "pass replace:true or choose another path"),
                     refusal("bloom.mcp.path-invalid", "path is relative or its folder is missing",
                             "pass an absolute path in an existing folder")};
    addExamples(
        saveAs,
        {{"mcp", R"({"name":"project.save-as","arguments":{"path":"/projects/scene.bloom"}})"}});
    addRelated(saveAs, {"project.save", "project.open"});
    out.push_back(std::move(saveAs));

    auto open = makeEntry(ReferenceKind::Tool, "project.open", "project",
                          "Open a .bloom file, replacing the open project (File > Open).",
                          "Every id and events cursor from the old project becomes stale: re-query "
                          "after it (events reports resetRequired).");
    open.effects = {};
    addArguments(open, {{.name = "path",
                         .type = "string",
                         .required = true,
                         .summary = "The project to open.",
                         .unit = "an absolute path to a .bloom file",
                         .range = "1..4096 bytes"},
                        discard,
                        stop});
    open.result = projectResult;
    open.errors = lifecycleErrors();
    addExamples(
        open, {{"mcp", R"({"name":"project.open","arguments":{"path":"/projects/scene.bloom"}})"}});
    addRelated(open, {"project.new", "query:context"});
    out.push_back(std::move(open));

    for (const auto& [name, summary] : std::array<std::pair<std::string_view, std::string_view>, 2>{
             {{"project.new", "Replace the open project with a new untitled one (File > New)."},
              {"project.close", "Close the open project (File > Close); an empty project is "
                                "shown."}}}) {
        auto value =
            makeEntry(ReferenceKind::Tool, std::string(name), "project", std::string(summary),
                      "Every id and events cursor from the old project becomes stale.");
        addArguments(value, {discard, stop});
        value.result = projectResult;
        value.errors = lifecycleErrors();
        addExamples(value, {{"mcp", R"({"name":")" + std::string(name) +
                                        R"(","arguments":{"discardChanges":false}})"}});
        addRelated(value, {"project.open", "project.save"});
        out.push_back(std::move(value));
    }

    auto color = makeEntry(
        ReferenceKind::Tool, "project.set-color-settings", "project",
        "Set the project's OCIO configuration and working colour space (Project Settings > "
        "Colour).",
        "Not undoable and not in the history: it marks the project unsaved and changes how every "
        "composition renders, so set it before authoring colour.");
    color.effects = {.dirtiesProject = true};
    addArguments(color,
                 {{.name = "config",
                   .type = "string",
                   .required = true,
                   .summary = "The OCIO configuration.",
                   .choices = {{"neutral", "Bloom Neutral v1"},
                               {"aces", "ACES 1.3 CG (built in)"},
                               {"current", "Keep the project's own (the only choice for an "
                                           "external configuration)"}}},
                  {.name = "workingSpace",
                   .type = "string",
                   .summary = "The working colour space: lin_rec709_scene for neutral; ACEScg, "
                              "ACES2065-1, Linear Rec.709 (sRGB), Linear P3-D65 or Linear "
                              "Rec.2020 for aces.",
                   .range = "at most 256 bytes",
                   .defaultValue = "the project's when the configuration stays, else the "
                                   "configuration's first"}});
    color.result = "{succeeded, code: updated|no-change, message, config, workingSpace}";
    color.errors = {
        refusal("invalid-config", "the configuration cannot be used", "choose neutral or aces"),
        refusal("invalid-settings", "the configuration does not offer workingSpace",
                "pick one of the listed working spaces"),
        refusal("bloom.mcp.read-only", "the project is read-only", "save a copy and open it")};
    addExamples(
        color,
        {{"mcp",
          R"({"name":"project.set-color-settings","arguments":{"config":"aces","workingSpace":"ACEScg"}})"}});
    addRelated(color, {"query:project"});
    out.push_back(std::move(color));
}

void windowTools(std::vector<ReferenceEntry>& out) {
    const ReferenceEffects live{.liveOnly = true};
    const std::string windowResult = "{succeeded, message?}";
    constexpr ArgumentSpec composition{.name = "composition",
                                       .type = "integer",
                                       .required = true,
                                       .summary = "The composition.",
                                       .unit = "composition id (query:compositions)",
                                       .range = ">= 1"};
    const auto time = [](const bool required) {
        return ArgumentSpec{.name = "time",
                            .type = "integer or [numerator, denominator]",
                            .required = required,
                            .summary = "Where to put the playhead.",
                            .unit = kTimeUnit};
    };

    auto reveal = makeEntry(ReferenceKind::Tool, "editor.reveal", "window",
                            "Show a composition in the Viewer, making it active, and optionally "
                            "move the playhead.",
                            "Desktop application only. Never changes the project.");
    reveal.effects = live;
    addArguments(reveal, {composition, time(false)});
    reveal.result = windowResult;
    reveal.errors = windowErrors();
    addExamples(reveal,
                {{"mcp", R"({"name":"editor.reveal","arguments":{"composition":1,"time":24}})"}});
    addRelated(reveal, {"composition.activate", "transport", "query:context"});
    out.push_back(std::move(reveal));

    auto select = makeEntry(
        ReferenceKind::Tool, "selection.set", "window",
        "Set, add to, remove from or clear the selected layers and nodes, or use the Select "
        "menu's all, invert and linked modes.",
        "Desktop application only. Operations whose layer, layers or "
        "selection argument is contextual then default to it. Never changes "
        "the project.");
    select.effects = live;
    addArguments(select, {composition,
                          {.name = "layers",
                           .type = "array",
                           .summary = "Layers to select.",
                           .unit = "layer ids (query:layers .id)",
                           .range = "at most 4096"},
                          {.name = "nodes",
                           .type = "array",
                           .summary = "Nodes to select.",
                           .unit = "node ids (query:nodes)",
                           .range = "at most 4096"},
                          {.name = "mode",
                           .type = "string",
                           .summary = "How the ids change the selection.",
                           .defaultValue = "set",
                           .choices = {{"set", "Select exactly these"},
                                       {"add", "Add them"},
                                       {"remove", "Remove them"},
                                       {"clear", "Select nothing"},
                                       {"all-layers", "Every layer of the composition"},
                                       {"all-nodes", "Every node of the composition"},
                                       {"invert-layers", "Invert the active composition's "
                                                         "layer selection"},
                                       {"invert-nodes", "Invert the active composition's "
                                                        "node selection"},
                                       {"linked-upstream", "Nodes feeding `nodes` (or the "
                                                           "selected nodes) through links and "
                                                           "driver bindings"},
                                       {"linked-downstream", "Nodes fed by `nodes` (or the "
                                                             "selected nodes) through links and "
                                                             "driver bindings"}}}});
    select.result = windowResult;
    select.errors = windowErrors();
    addExamples(
        select,
        {{"mcp", R"({"name":"selection.set","arguments":{"composition":1,"layers":[1]}})"}});
    addRelated(select, {"concept:context", "query:context"});
    out.push_back(std::move(select));

    auto activate = makeEntry(ReferenceKind::Tool, "composition.activate", "window",
                              "Make a composition the active one.",
                              "Desktop application only. The Viewer, Timeline and contextual "
                              "composition arguments follow it. Never changes the project.");
    activate.effects = live;
    addArguments(activate, {composition});
    activate.result = windowResult;
    activate.errors = windowErrors();
    addExamples(activate,
                {{"mcp", R"({"name":"composition.activate","arguments":{"composition":1}})"}});
    addRelated(activate, {"editor.reveal", "query:context"});
    out.push_back(std::move(activate));

    auto transport =
        makeEntry(ReferenceKind::Tool, "transport", "window", "Move or run the playhead.",
                  "Desktop application only. Never changes the project.");
    transport.effects = live;
    addArguments(transport, {{.name = "action",
                              .type = "string",
                              .required = true,
                              .summary = "What to do.",
                              .choices = {{"seek", "Go to time"},
                                          {"play", "Play"},
                                          {"stop", "Stop"},
                                          {"step-forward", "One frame forward"},
                                          {"step-backward", "One frame back"},
                                          {"go-to-start", "First frame"},
                                          {"go-to-end", "Last frame"},
                                          {"ram-preview", "Cache the work area, then play it "
                                                          "(the RAM Preview button)"}}},
                             {.name = "composition",
                              .type = "integer",
                              .summary = "The composition time counts in.",
                              .unit = "composition id",
                              .range = ">= 1",
                              .defaultValue = "the active composition"},
                             time(false)});
    transport.arguments.back().summary = "Where to seek; required for seek.";
    transport.result = windowResult;
    transport.errors = windowErrors();
    addExamples(transport,
                {{"mcp", R"({"name":"transport","arguments":{"action":"seek","time":[1,1]}})"}});
    addRelated(transport, {"editor.reveal", "concept:time"});
    out.push_back(std::move(transport));

    auto viewer = makeEntry(
        ReferenceKind::Tool, "viewer.set", "window",
        "Set the Viewer's display, view, exposure, gamma, channel, zoom, resolution, look, "
        "background, transparency grid and region of interest.",
        "Desktop application only. Display settings never reach a render, "
        "an export, a capture or the project. Only the given fields change.");
    viewer.effects = live;
    addArguments(
        viewer,
        {{.name = "display",
          .type = "string",
          .summary = "The OCIO display; empty returns to the project default.",
          .range = "at most 256 bytes"},
         {.name = "view",
          .type = "string",
          .summary = "The OCIO view; empty returns to the project default.",
          .range = "at most 256 bytes"},
         {.name = "exposure",
          .type = "number",
          .summary = "Viewer exposure.",
          .unit = "stops",
          .range = "-64..64"},
         {.name = "channel",
          .type = "string",
          .summary = "Which channels the Viewer shows.",
          .choices = {{"rgba", "RGBA"},
                      {"rgb", "RGB"},
                      {"red", "Red"},
                      {"green", "Green"},
                      {"blue", "Blue"},
                      {"alpha", "Alpha"}}},
         {.name = "zoom",
          .type = "string or number",
          .summary = "\"fit\", \"actual-size\", or a factor where 1.0 is 100%.",
          .range = "a factor in (0, 64]"},
         {.name = "gamma",
          .type = "number",
          .summary = "Viewer gamma; 1.0 is neutral.",
          .range = "(0, 10]"},
         {.name = "resolution",
          .type = "string",
          .summary = "The preview resolution.",
          .choices = {{"auto", "Chosen from the zoom"},
                      {"full", "Full resolution"},
                      {"half", "Half resolution"},
                      {"quarter", "Quarter resolution"}}},
         {.name = "look", .type = "boolean", .summary = "Whether the view's look is applied."},
         {.name = "background",
          .type = "string",
          .summary = "The surround behind the frame.",
          .choices = {{"solid", "The interface colour"},
                      {"gray", "Gray"},
                      {"black", "Black"},
                      {"white", "White"}}},
         {.name = "transparencyGrid",
          .type = "boolean",
          .summary = "Whether transparent areas show a checkerboard."},
         {.name = "regionOfInterest",
          .type = "object or null",
          .summary = "The region the Viewer renders, as {x, y, width, height}; "
                     "null clears it. Clipped to the frame.",
          .unit = kPixelUnit}});
    viewer.result = windowResult;
    viewer.errors = windowErrors();
    addExamples(viewer,
                {{"mcp", R"({"name":"viewer.set","arguments":{"channel":"alpha","zoom":"fit"}})"}});
    addRelated(viewer, {"capture"});
    out.push_back(std::move(viewer));
}

void parityTools(std::vector<ReferenceEntry>& out) {
    const ReferenceEffects live{.liveOnly = true};
    const std::string windowResult = "{succeeded, message?}";
    const auto notLive = windowErrors();
    const auto simple = [&](std::string name, std::string area, std::string summary,
                            std::string notes, const ReferenceEffects effects,
                            std::vector<ArgumentSpec> arguments, std::string result,
                            std::string example, std::vector<ReferenceError> errors,
                            std::vector<std::string_view> related) {
        auto value = makeEntry(ReferenceKind::Tool, name, std::move(area), std::move(summary),
                               std::move(notes));
        value.effects = effects;
        for (const auto& argument : arguments)
            addArguments(value, {argument});
        value.result = std::move(result);
        value.errors = std::move(errors);
        addExamples(value, {{"mcp", std::move(example)}});
        for (const auto& relatedName : related)
            addRelated(value, {relatedName});
        out.push_back(std::move(value));
    };
    const auto withRefusals = [&](std::vector<ReferenceError> extra) {
        extra.insert(extra.end(), notLive.begin(), notLive.end());
        return extra;
    };

    simple(
        "project.save-copy", "project",
        "Write a byte-for-byte copy of a read-only project to a new .bloom file (File > Save "
        "a Copy).",
        "For a newer or preserved document that Save cannot rewrite. The open project stays "
        "where it is.",
        fileEffects(),
        {{.name = "path",
          .type = "string",
          .required = true,
          .summary = "The file to write.",
          .unit = "an absolute path ending in .bloom whose folder exists",
          .range = "1..4096 bytes"},
         {.name = "replace",
          .type = "boolean",
          .summary = "Replace an existing file at path.",
          .defaultValue = "false"}},
        "{succeeded, event (project.copy-saved), path, projectName, message?}",
        R"({"name":"project.save-copy","arguments":{"path":"/projects/copy.bloom"}})",
        withRefusals({refusal("bloom.mcp.invalid-argument", "the project is editable",
                              "use project.save-as"),
                      refusal("bloom.mcp.destination-exists", "a file exists at path",
                              "pass replace:true or choose another path"),
                      refusal("bloom.mcp.path-invalid", "path is relative or its folder is missing",
                              "pass an absolute path in an existing folder")}),
        {"project.save-as", "project.open"});

    simple("project.cancel-open", "project",
           "Abandon the project open in progress (File > Cancel Open).",
           "The project that was open stays. Cancels an open started by the artist or an agent.",
           live, {}, windowResult, R"({"name":"project.cancel-open","arguments":{}})",
           withRefusals(
               {refusal("bloom.mcp.invalid-argument", "nothing is opening", "nothing to cancel")}),
           {"project.open"});

    simple("export.reveal", "window",
           "Bring the Export editor into view and optionally select a job (Jobs > Show in "
           "Export).",
           "Export queue edits never reveal the panel by themselves; call this when the artist "
           "should see the queue. Never changes the project or its undo history.",
           live,
           {{.name = "job",
             .type = "integer",
             .summary = "The job to select.",
             .unit = "export job id (export.status)",
             .range = ">= 1"}},
           windowResult, R"({"name":"export.reveal","arguments":{"job":1}})", windowErrors(),
           {"export.status", "editor.reveal"});

    simple("profiler.snapshot", "telemetry",
           "Bloom's runtime telemetry now: memory, GPU, CPU per executor pool, tasks, stage "
           "timings, playback and RAM Preview rates, the RAM Preview's contiguous cached run, the "
           "Viewer's displayed frame and the GPU preview route's CPU fallbacks, device admission "
           "claims, and helper processes.",
           "Absent values are null, never 0. Counters are cumulative, so rates come from two "
           "snapshots. snapshot.schemaVersion names the field set. Read-only; holds no session "
           "lease, so it answers while a project opens.",
           readOnlyEffects(), {}, "{succeeded, snapshot:{schemaVersion, ...}}",
           R"({"name":"profiler.snapshot","arguments":{}})", commonToolErrors(), {});

    simple("preferences.get", "application",
           "The application preferences (Edit > Preferences) as {key: value}.",
           "Playback, memory and media cache, timeline, node graph, Viewer defaults and guides, "
           "import defaults, sounds and notifications. Per-user settings, never project truth.",
           {.readOnly = true, .liveOnly = true}, {}, "{succeeded, preferences:{key: value}}",
           R"({"name":"preferences.get","arguments":{}})", notLive, {"preferences.set"});

    simple("preferences.set", "application",
           "Change application preferences as the Preferences window's OK does.",
           "Every entry is checked first; an unknown key or a value out of range refuses the "
           "whole call naming the key. Then all are saved and applied to the open editors "
           "together. Not undoable and not in the project. preferences.get lists the keys.",
           live,
           {{.name = "preferences",
             .type = "object",
             .required = true,
             .summary = "The preferences to change, as {key: value}.",
             .unit = "each value a boolean, number or string, in the preference's own unit "
                     "(preferences.get)",
             .range = "1..64 entries"}},
           windowResult,
           R"({"name":"preferences.set","arguments":{"preferences":{"playback.loop":true}}})",
           withRefusals({refusal("bloom.mcp.busy", "a modal dialog is open",
                                 "retry when the artist closes it, or press one of its buttons "
                                 "with dialog.answer when the artist has allowed that")}),
           {"preferences.get"});

    simple("dialog.answer", "window",
           "Press one button of the modal dialog that blocks the Bloom window (opt-in).",
           "Off unless the artist switches on Preferences > General > Agents > Answer open "
           "dialogs; until then it is refused with bloom.mcp.not-allowed. A call refused with "
           "bloom.mcp.busy because a dialog is open describes it: title, text and buttons, with "
           "the buttons that discard or overwrite data marked. The dialog's own action happens "
           "(it is not a project edit and Undo cannot restore it), so read the marks before "
           "choosing.",
           live,
           {{.name = "button",
             .type = "string",
             .required = true,
             .summary = "The text of the button to press.",
             .unit = "a visible, enabled button of the open dialog, case-insensitive, "
                     "accelerator marks ignored",
             .range = "1..256 bytes"}},
           windowResult, R"({"name":"dialog.answer","arguments":{"button":"Cancel"}})",
           withRefusals({refusal("bloom.mcp.not-allowed", "the Preferences switch is off",
                                 "ask the artist to switch it on, or ask them to answer the "
                                 "dialog"),
                         refusal("bloom.mcp.invalid-argument",
                                 "no dialog is open, or no button has that text",
                                 "the message lists the buttons")}),
           {"preferences.get"});

    simple("script.run", "application",
           "Run Python in the live session through the Script editor (opt-in).",
           "Off unless the artist switches on Preferences > General > Agents > Run Python "
           "scripts; until then it is refused with bloom.mcp.not-allowed. The script uses the "
           "bloom module against the open project: its edits are the same commands, one undo "
           "entry per transaction group, as the Script editor's, and its output appears in the "
           "editor. Python can reach anything the user account can, so the switch is access to "
           "the machine. Bounded: 65536 characters of source, timeoutSeconds (default 60, at "
           "most 600) before it is canceled, 65536 characters of output; the request's own "
           "cancellation also cancels it. One script runs at a time, the rest queue.",
           live,
           {{.name = "source",
             .type = "string",
             .required = true,
             .summary = "The Python source to run.",
             .range = "1..65536 characters"},
            {.name = "timeoutSeconds",
             .type = "integer",
             .summary = "Seconds before the script is canceled.",
             .unit = "seconds",
             .range = "1..600",
             .defaultValue = "60"}},
           "{succeeded, output} or, when the script failed, a bloom.mcp.failed diagnostic with "
           "output",
           R"json({"name":"script.run","arguments":{"source":"print(bloom.context.composition)"}})json",
           withRefusals({refusal("bloom.mcp.not-allowed", "the Preferences switch is off",
                                 "ask the artist to switch it on"),
                         refusal("bloom.mcp.busy", "the Script editor's queue is full",
                                 "retry shortly"),
                         refusal("bloom.mcp.failed", "the script raised an error or was canceled",
                                 "read output; fix the script and run it again")}),
           {"transact", "history"});

    simple("cache.purge", "application", "Clear derived caches (Edit > Purge).",
           "Derived state only: the project, its revision and undo history are untouched. Source "
           "files are never touched.",
           live,
           {{.name = "cache",
             .type = "string",
             .required = true,
             .summary = "Which cache to clear.",
             .choices = {{"preview", "The RAM preview and operation caches"},
                         {"media", "The decoded media cache and its disk store"}}}},
           windowResult, R"({"name":"cache.purge","arguments":{"cache":"preview"}})",
           withRefusals({refusal("bloom.mcp.busy", "a purge is running", "retry shortly")}),
           {"jobs.list"});

    simple("jobs.list", "application",
           "The Jobs panel's rows: imports, previews, thumbnails, purges and export runs.",
           "An export run's row names its export job.", {.readOnly = true, .liveOnly = true}, {},
           "{succeeded, jobs:[{id, name, phase, state, completed, total, canCancel, "
           "cancellationRequested, exportJob}]}",
           R"({"name":"jobs.list","arguments":{}})", notLive, {"jobs.cancel"});

    simple("jobs.cancel", "application",
           "The Jobs panel's Cancel for one job: cooperative cancellation of a queued or running "
           "job.",
           "An export run cannot be cancelled here; use export.stop.", live,
           {{.name = "job",
             .type = "integer",
             .required = true,
             .summary = "The job to cancel.",
             .unit = "Jobs panel job id (jobs.list)",
             .range = ">= 1"}},
           windowResult, R"({"name":"jobs.cancel","arguments":{"job":1}})",
           withRefusals({refusal("bloom.mcp.unknown-target", "the panel does not list the job",
                                 "re-read jobs.list"),
                         refusal("bloom.mcp.invalid-argument", "the job cannot be cancelled here",
                                 "an export run: use export.stop")}),
           {"jobs.list", "export.stop"});

    simple("asset.reveal", "window", "Reveal an asset's file in the platform's file manager.",
           "Selects the file where the file manager can, else opens its folder. Never changes "
           "the project.",
           live,
           {{.name = "asset",
             .type = "integer",
             .required = true,
             .summary = "The asset.",
             .unit = "asset id (query:assets)",
             .range = ">= 1"}},
           windowResult, R"({"name":"asset.reveal","arguments":{"asset":1}})",
           withRefusals({refusal("bloom.mcp.invalid-argument",
                                 "the asset has no file (a font or a data asset)",
                                 "only file-backed assets can be revealed")}),
           {"query:assets"});

    simple("composition.presets", "application", "The Composition Settings dialog's presets.",
           "The built-in sizes carry size and pixel aspect only; the artist's saved presets also "
           "carry frame rate, duration, start timecode, drop-frame and background. Apply one "
           "with bloom.composition.set-settings.",
           {.readOnly = true, .liveOnly = true}, {},
           "{succeeded, presets:[{name, builtIn, width, height, pixelAspect, ...}]}",
           R"({"name":"composition.presets","arguments":{}})", notLive,
           {"composition.save-preset", "bloom.composition.set-settings"});

    simple("composition.save-preset", "application",
           "Save a composition's settings as a named preset (Save as preset in Composition "
           "Settings).",
           "Saves size, pixel aspect, frame rate, duration, start timecode, drop-frame and "
           "background. A per-user setting, never project truth or undo.",
           live,
           {{.name = "composition",
             .type = "integer",
             .required = true,
             .summary = "The composition to save.",
             .unit = "composition id (query:compositions)",
             .range = ">= 1"},
            {.name = "name",
             .type = "string",
             .required = true,
             .summary = "The new preset's name.",
             .range = "1..256 bytes, not already used"}},
           windowResult,
           R"({"name":"composition.save-preset","arguments":{"composition":1,"name":"HD 24"}})",
           withRefusals({}), {"composition.presets"});

    simple("composition.rename-preset", "application",
           "Rename a saved composition preset (not a built-in one).", "", live,
           {{.name = "name",
             .type = "string",
             .required = true,
             .summary = "The saved preset to rename.",
             .range = "1..256 bytes"},
            {.name = "newName",
             .type = "string",
             .required = true,
             .summary = "Its new name.",
             .range = "1..256 bytes, not already used"}},
           windowResult,
           R"({"name":"composition.rename-preset","arguments":{"name":"HD 24","newName":"HD"}})",
           withRefusals({}), {"composition.presets"});

    simple("composition.delete-preset", "application", "Delete a saved composition preset.",
           "A per-user setting, so Undo cannot restore it; an agent call is the confirmation.",
           live,
           {{.name = "name",
             .type = "string",
             .required = true,
             .summary = "The saved preset to delete.",
             .range = "1..256 bytes"}},
           windowResult, R"({"name":"composition.delete-preset","arguments":{"name":"HD"}})",
           withRefusals({}), {"composition.presets"});
}

} // namespace

void addSessionToolEntries(std::vector<ReferenceEntry>& out) {
    projectTools(out);
    windowTools(out);
    parityTools(out);
}

} // namespace bloom::scripting::detail
