// The orientation an agent reads first: initialize's instructions and the bloom://guide/agents
// resource. bloom.mcp.reference replays the guide's calls against a new project, so its ids and
// revisions stay true.

#include <bloom/scripting/operation_reference.hpp>

namespace bloom::scripting {

std::string_view agentInstructions() noexcept {
    return "Bloom is a node-based compositor; this server is its interface for agents. Call "
           "`help` with no arguments to list every tool, query kind and operation, and `help "
           "{\"name\": ...}` for arguments with units and accepted values, results, errors with "
           "remedies and examples; you never need Bloom's source. Read with `query` (start with "
           "kind=context, then compositions and layers); change the project with `transact`, "
           "passing the `revision` of your last read as expectedRevision: the operations apply "
           "atomically as one undo step and new ids come back in `outputs`. Times are integer "
           "frames or exact seconds [numerator, denominator]; ids are stable positive integers, "
           "one space per kind. Look at the result with `capture`. Resources: "
           "bloom://guide/agents (a five-step walkthrough), bloom://reference/nodes, "
           "bloom://reference/operations and the user guides.";
}

std::string agentGuideMarkdown() {
    return R"md(# Getting started with Bloom for agents

Bloom is a node-based compositor. Through MCP you work on the same project the artist sees (in
the desktop application) or on a headless session (`bloom-mcp` on stdio). Everything below uses
only `query`, `transact`, `capture` and `help`; `help {"name": X}` explains any name used here.

## 1. Find the active composition

```json
{"name":"query","arguments":{"kind":"context"}}
```

`composition` is the composition the Viewer shows (null when the project has none) and
`revision` is the revision your next edit names. The numbers below are those of a new project,
which has composition 1 at revision 0; always pass the `revision` your last result returned.
`query {kind:"compositions"}` lists every composition with its size, frame rate, duration
(`{frames, seconds}`) and work area. With no composition, create one; its id is in `outputs`:

```json
{"name":"transact","arguments":{"expectedRevision":0,"label":"New comp","operations":[
  {"op":"bloom.composition.add","args":{"name":"Scene","width":1920,"height":1080,"duration":48}}]}}
```

## 2. Add a layer

```json
{"name":"transact","arguments":{"expectedRevision":0,"label":"Add a red solid","operations":[
  {"op":"bloom.layer.add-solid","args":{"composition":1,"name":"Red","color":[1,0,0,1]}}]}}
```

The result's `outputs` names every new id: `layer`, `layerOutputNode`, `positionParameter`,
`opacityParameter` and so on. `query {kind:"layers","composition":1}` lists the layers topmost
first with the same parameter ids under `parameters`.

## 3. Animate a parameter

Make the position animated, then key it at frame 0 and frame 47 (an integer time is a frame;
`[n, d]` is exact seconds). `parameter` is the `positionParameter` id from step 2's outputs (4 in
a new project).

```json
{"name":"transact","arguments":{"expectedRevision":1,"label":"Slide","operations":[
  {"op":"bloom.animation.create-for-parameter","args":{"composition":1,"parameter":4,"time":0}},
  {"op":"bloom.animation.set-keyframe-at-time-for-parameter","args":{"composition":1,"parameter":4,"time":0,"value":[200,540]}},
  {"op":"bloom.animation.set-keyframe-at-time-for-parameter","args":{"composition":1,"parameter":4,"time":47,"value":[1720,540]}}]}}
```

Read the keys back in the same units:

```json
{"name":"query","arguments":{"kind":"keyframes","composition":1,"parameter":4}}
```

`query {kind:"parameter"}` reports a parameter's value kind, unit, range, default and, for
enumerations such as the blend mode, the accepted values with their labels.

## 4. Render a frame to check

```json
{"name":"capture","arguments":{"composition":1,"frame":24}}
```

returns the frame as a PNG image. Output keeps alpha; the composition background colour is
display-only and not rendered. `render` writes reference files with SHA-256 digests and `export`
writes movies.

To undo or redo a step, call `history` with `action` and `expectedRevision`, the current
revision from any query or edit result; a stale revision changes nothing.

## 5. Save

The in-app server edits the artist's open project; the artist saves it (File > Save), and the
edit is already in their undo history. If `help` lists a `project` tool, it saves too. On stdio,
`bloom-mcp --project scene.bloom` edits a session copy; save from Python with
`bloom.Host("scene.bloom")` and `save(path)`, or ask the person running the session.

## When something fails

A refused edit changes nothing. `diagnostics[].code` and `message` say why, and
`help {"name":"concept:errors"}` lists which codes to retry. A stale revision means someone else
edited: query again and retry with the new `revision`.
)md";
}

} // namespace bloom::scripting
