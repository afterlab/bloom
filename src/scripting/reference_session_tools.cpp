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

    auto select = makeEntry(ReferenceKind::Tool, "selection.set", "window",
                            "Set, add to, remove from or clear the selected layers and nodes.",
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
                                       {"clear", "Select nothing"}}}});
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
                                          {"go-to-end", "Last frame"}}},
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

    auto viewer = makeEntry(ReferenceKind::Tool, "viewer.set", "window",
                            "Set the Viewer's display, view, exposure, channel and zoom.",
                            "Desktop application only. Display settings never reach a render, "
                            "an export, a capture or the project. Only the given fields change.");
    viewer.effects = live;
    addArguments(viewer, {{.name = "display",
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
                           .range = "a factor in (0, 64]"}});
    viewer.result = windowResult;
    viewer.errors = windowErrors();
    addExamples(viewer,
                {{"mcp", R"({"name":"viewer.set","arguments":{"channel":"alpha","zoom":"fit"}})"}});
    addRelated(viewer, {"capture"});
    out.push_back(std::move(viewer));
}

} // namespace

void addSessionToolEntries(std::vector<ReferenceEntry>& out) {
    projectTools(out);
    windowTools(out);
}

} // namespace bloom::scripting::detail
