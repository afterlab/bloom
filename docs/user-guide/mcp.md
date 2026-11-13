# Using Bloom from an agent (MCP)

Bloom offers the Model Context Protocol (MCP 2025-11-25) in two ways. Both serve the same tools
with the same schemas from one server core (ADR 0034):

- **The in-app server** runs inside the desktop application and works on the project you have
  open. An agent's edits land in your undo history, show in every panel at once, and can be
  undone with Edit ▸ Undo. Start it from the **MCP Server** editor.
- **`bloom-mcp`** is a separate process on stdin/stdout with its own headless session, for
  scripts, CI and agents that should not touch an open project.

## The in-app server

Open an **MCP Server** editor (any editor area's editor menu) and press **Start**. The server is
off until you start it. It listens on `http://127.0.0.1:6767/mcp`, on this computer only: it binds
the loopback address `127.0.0.1` and nothing else, so other computers cannot reach it, and it
trusts no web page. Change the **Port** (1024–65535) while the server is stopped. If another
program already uses the port, Start says so ("Port 6767 is in use. Choose another port or stop
the other program."); Bloom never moves to another port by itself, because a client configured
for one port must never reach a server that silently moved. **Start when Bloom opens** starts the
server at launch. The port and this choice are preferences, kept outside your projects; nothing
the MCP Server editor does changes a project or its undo history.

### Connecting a client

Every request needs the access token, sent as `Authorization: Bearer <token>`. The token is
created on first use and kept in an owner-only file in Bloom's user-data folder (readable only by
you; a file other users could read is replaced). **Copy client config** puts ready-to-paste text
on the clipboard for:

- **Claude Code**: `claude mcp add --transport http bloom http://127.0.0.1:6767/mcp --header "Authorization: Bearer <token>"`
- **MCP JSON** (a project's `.mcp.json` and clients that read it):
  `{"mcpServers":{"bloom":{"type":"http","url":"http://127.0.0.1:6767/mcp","headers":{"Authorization":"Bearer <token>"}}}}`
- **VS Code** (`.vscode/mcp.json`):
  `{"servers":{"bloom":{"type":"http","url":"http://127.0.0.1:6767/mcp","headers":{"Authorization":"Bearer <token>"}}}}`
- **Other client**: the address and the header as two lines.

Each kind can also be copied with `${BLOOM_MCP_TOKEN}` in place of the token, for configuration
that is shared or committed; set that environment variable in the client's environment. The
editor shows the token masked; **Reveal** and **Copy** show or copy it, and **Regenerate…**
replaces it and disconnects every client (they must use the new token). The token never appears
in the activity list or in diagnostics.

### What the editor shows

The status line (Stopped, Running on 127.0.0.1:6767 with the number of clients, Stopping, or the
reason the server could not start), the connected clients (name and version from the client,
connected since, last activity, requests in flight, the last tool), and the activity list: every
tool call with its client, outcome, error and duration, and every refused request (a wrong token,
a browser origin, too many clients). Identical refusals within a second are counted on one line.
Up to 512 lines are kept; "Earlier activity was trimmed." marks a gap.

### Limits

Up to 8 clients at once. When a ninth connects, the least recently used client that has been idle
for 5 minutes with nothing in flight is disconnected; otherwise the new client is refused. A client
idle for 60 minutes is ended and reconnects on its next request. At most 16 connections, 4 requests
in progress per client, 4 executing at once and 16 waiting. Every request is at most 1 MiB and every
response at most 4 MiB, as on stdio. A client that has 4 requests in progress and sends a fifth gets
error `-32000` "Too many requests from this client" (HTTP 503) naming that limit; nothing was run, so
send it again once one of the four has answered. The limit protects the main thread, so batch many
edits into one `transact` instead of sending them in parallel.

### Edits, undo and project changes

A `transact` call is one atomic transaction and one undo step, labelled with its `label` or
"Agent edit". It applies on Bloom's main thread between your own edits: never while you are in the
middle of a drag, a scrub or a text edit (it waits until you finish, up to 30 seconds), and a
revision conflict leaves the project unchanged. A queued edit that is cancelled, times out or
outlives its project is never applied, and the result names why with a stable code:
`bloom.mcp.cancelled`, `bloom.mcp.deadline-exceeded`, `bloom.mcp.gesture-open`,
`bloom.mcp.project-replaced`, `bloom.mcp.commit-queue-full`. The Script editor and MCP share one
queue of 16 edits.

Clients stay connected when you open or create a project. While a project is opening, calls answer
`bloom.mcp.project-unavailable`; an `events` cursor from the old project reports `resetRequired`.

### Projects and the application window

An agent can save, open, create and close projects as you can from the File menu, and can act on
the window the way you do: what is selected, which composition is active, the playhead, the
Viewer, the Export panel, the Jobs panel, Preferences, Edit > Purge and the composition presets.
None of these is an undo step. The project tools never throw work away silently: Open,
New and Close over unsaved changes answer `bloom.mcp.unsaved-changes` (naming the project) unless
the call passes `discardChanges:true`, and while export jobs run they answer
`bloom.mcp.jobs-running` unless it passes `stopJobs:true`.

| Tool | Arguments | Notes |
| --- | --- | --- |
| `project.save` | none | `bloom.mcp.no-path` when the project was never saved; `bloom.mcp.read-only` for a read-only project. |
| `project.save-as` | `path` (absolute, `.bloom`), optional `replace` | `bloom.mcp.destination-exists` unless `replace` is true; `bloom.mcp.path-invalid` for a relative path or a missing folder. |
| `project.open` | `path`, optional `discardChanges`, `stopJobs` | Replaces the open project. |
| `project.new` | optional `discardChanges`, `stopJobs` | An untitled project. |
| `project.close` | optional `discardChanges`, `stopJobs` | The window shows an empty project. |
| `project.save-copy` | `path` (absolute, `.bloom`), optional `replace` | File > Save a Copy: a byte-for-byte copy of a read-only (newer or preserved) project; the open project stays where it is. An editable project answers `bloom.mcp.invalid-argument` (use `project.save-as`). |
| `project.cancel-open` | none | File > Cancel Open: abandons the open in progress, yours or an agent's. `bloom.mcp.invalid-argument` when nothing is opening. |
| `editor.reveal` | `composition`, optional `time` | Shows the composition in the Viewer and moves the playhead. |
| `selection.set` | `composition`, optional `layers`, `nodes`, `mode` (`set`, `add`, `remove`, `clear`, or the Select menus' `all-layers`, `all-nodes`, `invert-layers`, `invert-nodes`, `linked-upstream`, `linked-downstream`) | Contextual `layer`, `layers` and `selection` arguments then default to it. Invert works on the active composition's selection; the linked modes extend from `nodes`, or from the selected nodes, through links and driver bindings. |
| `composition.activate` | `composition` | The Viewer, Timeline and contextual `composition` follow it. |
| `transport` | `action` (`seek`, `play`, `stop`, `step-forward`, `step-backward`, `go-to-start`, `go-to-end`, `ram-preview`), `time` for `seek`, optional `composition` | `time` counts in the given composition, or the active one. `ram-preview` caches the work area and then plays it, as the RAM Preview button and Numpad 0 do; `stop` also ends a RAM preview that is still caching. |
| `viewer.set` | any of `display`, `view`, `exposure` (stops), `gamma` (1.0 is neutral), `channel` (`rgba`, `rgb`, `red`, `green`, `blue`, `alpha`), `zoom` (`"fit"`, `"actual-size"` or a factor, 1.0 is 100%), `resolution` (`auto`, `full`, `half`, `quarter`), `look` (true or false), `background` (`solid`, `gray`, `black`, `white`), `transparencyGrid` (true or false), `regionOfInterest` (`{x, y, width, height}` in composition pixels, or `null` to clear it), `level` (group layer ids from the top, outermost first; `[]` is the top level), `points` (a shape layer of that level whose points are edited, or `null`) | Display settings only: they never reach a render or the project. `level` and `points` are the Viewer's drill level, session state like a double-click. |
| `export.reveal` | optional `job` | Brings the Export editor into view and, with `job`, selects it, as the Jobs panel's **Show in Export** does. `bloom.mcp.unknown-target` for a job the queue does not hold; `bloom.mcp.failed` when the panel could not be shown. |
| `jobs.list` | none | The Jobs panel's rows: `{id, name, phase, state, completed, total, canCancel, cancellationRequested, exportJob}`. An export run's row names its `exportJob`. |
| `jobs.cancel` | `job` | The Jobs panel's **Cancel**. `bloom.mcp.unknown-target` for a job it does not list; `bloom.mcp.invalid-argument` for a job it cannot cancel (an export run: use `export.stop`). |
| `preferences.get` | none | Every preference below with its current value, as `{key: value}`. |
| `preferences.set` | `preferences`: `{key: value, ...}` (1 to 64) | Edit > Preferences' **OK**: every entry is checked first, then all are saved and applied together. An unknown key or a value out of range answers `bloom.mcp.invalid-argument` naming the key and changes nothing; `bloom.mcp.busy` while a dialog is open. |
| `dialog.answer` | `button` | Presses one button of the modal dialog that blocks the window (see **Dialogs** below). Off unless the artist switches on Edit > Preferences > General > **Let agents answer open dialogs**: until then `bloom.mcp.not-allowed`. `bloom.mcp.invalid-argument` when no dialog is open or no visible, enabled button has that text, listing the buttons. |
| `script.run` | `source`, optional `timeoutSeconds` (1 to 600, default 60) | Runs Python in the open project through the Script editor (see **Python in the running session** below). Off unless the artist switches on **Let agents run Python scripts**: until then `bloom.mcp.not-allowed`. |
| `cache.purge` | `cache` (`preview` or `media`) | Edit > Purge: derived caches only; the project, its revision and undo are untouched. `bloom.mcp.busy` while a purge runs. |
| `asset.reveal` | `asset` | Reveal in File Manager. `bloom.mcp.invalid-argument` for an asset without a file. |
| `composition.presets` | none | The Composition Settings presets: the built-in sizes, then your saved presets with `frameRate`, `duration`, `startTimecode`, `dropFrame` and `background`. Apply one with `bloom.composition.set-settings`. |
| `composition.save-preset` | `composition`, `name` | **Save as preset**: the composition's size, pixel aspect, frame rate, duration, start timecode, drop-frame and background. `bloom.mcp.invalid-argument` for an empty or used name. |
| `composition.rename-preset` | `name`, `newName` | **Rename preset**, for a saved preset. `bloom.mcp.unknown-target` when no saved preset has that name. |
| `composition.delete-preset` | `name` | **Delete preset**. Presets are saved for you, not in the project, so Undo cannot restore one. |

A successful project tool answers `{succeeded, event, path, projectName}` and records the event
(`project.saved`, `project.opened`, `project.created`, `project.closed` or `project.copy-saved`)
for the `events` tool and the MCP Server editor's activity list. The window tools work only with
the in-app server: `bloom-mcp` implements `project.save`, `project.save-as`, `project.open`,
`project.new`, `project.close` and `project.save-copy` on its own session and answers
`bloom.mcp.not-live` for the others. A window tool the application has not connected answers `bloom.mcp.ui-unbound`.
Other refusals are `bloom.mcp.unknown-target`, `bloom.mcp.invalid-argument`, `bloom.mcp.busy` and
`bloom.mcp.failed`, each with a message that says what to do.

Some of what you do in the window has no tool, on purpose: arranging panels (split, join,
close, maximize, Reset Workspace, full screen, the toolbar), zooming and scrolling editors other
than the Viewer, and the Viewer's guide overlays except through `preferences.set`; quitting Bloom;
starting, stopping or configuring the MCP server and its token; running Python in the Script
editor (use `transact`); the Help menu's links; and the recovery question Bloom asks at launch
after a crash.

#### Acting in the open app

In the desktop application these tools are the artist's own actions, not a second path: an agent's
call goes through the same controllers the window uses, and the window shows it at once.

- **Project tools** use the File menu's open, save, save-as, new and close flows without any dialog.
  A refusal never changes anything: `project.save` of a never-saved project answers
  `bloom.mcp.no-path` ("use `project.save-as`"), `project.save-as` onto an existing file answers
  `bloom.mcp.destination-exists` unless `replace` is true, and a dirty project or a running export
  answers `bloom.mcp.unsaved-changes` or `bloom.mcp.jobs-running` unless `discardChanges` or
  `stopJobs` is passed. `stopJobs` stops the jobs through the export queue and the call goes on once
  they have. While another project operation or a dialog is open the call answers `bloom.mcp.busy`.
  `project.close` leaves the same empty project File > New does.
- **`selection.set`** selects as clicks in the Timeline and Nodes do: the Timeline, Nodes and
  Properties show it, and a `layers`-only `set` or `add` extends like Shift-click. The composition is
  made active first when it is not. A layer inside a group (an imported SVG's parts) is selected
  with `layers` alone, `set` or `add`, and the Viewer's level follows it.
- **`transport`** drives the Viewer footer's transport: `seek` and the step and go-to actions stop
  playback first, as the footer does; `time` outside the composition answers
  `bloom.mcp.invalid-argument`.
- **`viewer.set`** moves the displayed Viewer's own controls, so the footer and the View menu show
  the new channel, exposure, zoom, display and view. An empty `display` or `view` returns to the
  project's default. A value the project does not offer answers `bloom.mcp.invalid-argument` and
  lists the choices. Nothing here reaches a render. `level` enters groups as double-clicking in the
  Viewer does: each id must be a group child of the one before it (the first, a top-level layer);
  a track matte source is never a level. `points` edits a shape's points, selecting the layer whose
  points are shown. A chain that does not resolve answers `bloom.mcp.invalid-argument` (an unknown
  id `bloom.mcp.unknown-target`) and changes nothing. The level applies without a Viewer, is never
  saved or undone, and selecting a layer anywhere sets it to that layer's groups.
- **`editor.reveal`** brings the Viewer and Timeline forward on the composition.
- **`export.reveal`** follows the Export panel's own reveal rule: an open Export editor is focused,
  otherwise the Timeline's panel becomes Export, and with no Timeline a new panel opens along the
  bottom. Export queue edits (adding jobs, rendering) never reveal the panel by themselves; an agent
  calls `export.reveal` when you should see the queue.
- **`project.set-color-settings`** is Project Settings > Color: one undo step, marks the project
  dirty, `bloom.mcp.busy` while a project opens.
- **`preferences.set`** is the Preferences window without the window: the open editors take the new
  values at once and the settings are saved for you, never in the project. The keys are
  `audioPlayback`, `loopPlayback`, `crashRecovery` (off by default), `ramHeadroomBytes` and `vramHeadroomBytes` (0 to 256 GiB), `exportWorkerMemoryCapBytes` (0 for no
  cap, to 1 TiB) and `exportWorkerThreads` (0 for automatic, to 64),
  `mediaDiskCache`, `mediaDiskCacheDirectory` (absolute, or empty for the default),
  `mediaDiskCacheBudgetBytes` (0 for the default), `mediaLocalCopies` (`automatic`, `always` or
  `never`), `mediaLocalCopyFolders` (one line per folder rule, `keep:<absolute folder>` or
  `skip:<absolute folder>`; empty for none), `timelineTimeFormat` (`frames` or `timecode`),
  `timelineSnapping`, `timelineKeyframesVisible`, `timelineGraphEditor`, `nodeLinkStyle` (`spline`,
  `straight`, `angled`), `nodeSnap`, `nodeGridSize` (pixels), `nodeShowZoomControls`,
  `nodeShowMinimap`, `viewerResolution` (`auto`, `full`,
  `half`, `quarter`), `viewerBackground` (`solid`, `gray`, `black`, `white`),
  `viewerTransparencyGrid`, `viewerSafeAreas`, `viewerCenterCross`, `viewerThirds`, `viewerRulers`,
  `viewerPixelGrid`, `importSequenceFrameRate` (`"24"`, `"24000/1001"`, or empty for one image per
  composition frame), `importStillDuration` (seconds, `"5"` or `"1001/240"`, or empty for the
  composition's length), `sounds`, `soundVolume` (0 to 100), `eventSounds`, `interactionSounds`
  and `renderNotifications`.
- **`jobs.list`** and **`jobs.cancel`** read and cancel what the Jobs panel shows: imports, previews,
  thumbnails, purges and export runs.

Selection, the active composition, the playhead, the Viewer's settings, preferences, cache purges,
jobs and composition presets are session or per-user state: they never mark the project as changed
and never enter the undo history. An edit that leaves a
composition unable to render also appears in the MCP Server editor's activity list as a red line
("left 'Main' unable to render: ...") and in `transact`'s `compileFailures`.

`render`, `export` and `capture` run as transient jobs of the export queue, with the same checks,
verification and Viewer priority as the Export editor; they are never saved. Their output keeps
alpha: the composition background colour is display-only and is not rendered. Nothing replaces an
existing file silently: for `render` and `export`, `"overwrite":"ask"` (the default) and `"refuse"`
both fail with `bloom.mcp.export-unavailable` when the destination exists, because nothing can
answer the question over MCP; pass `"overwrite":"replace"`. Queue jobs that ask wait for
`export.confirm-replace`. Right after Bloom starts, the encoders are still being checked: an `export`
of a movie preset in that moment waits for the check (up to 30 seconds, and it ends when the request
is cancelled) and then answers with the real availability, so a client never has to retry. A check
that takes longer still fails with `bloom.mcp.export-unavailable` and "Checking encoders…".
### Export worker limits

`export` takes optional `workerMemoryCapBytes` (0 = no cap, up to 1 TiB) and `workerThreads` (0 =
automatic, 1 to 64), and so do `export.start` and `export.start-all`. They limit the media worker
that encodes a movie or WAV for that call; an option that is not given uses the saved Preferences
value (`exportWorkerMemoryCapBytes`, `exportWorkerThreads`; off by default, in which case nothing
changes). At the cap the export waits for the worker to finish what it holds, and fails with a
message naming the worker's memory and the cap if it still cannot go on. A still `render` has no
worker and refuses the options. On stdio `bloom-mcp` has no saved Preferences, so only the call's
options apply.

The `export` result and each output's `verification` in `export.status` carry `workerMemory`:
`measured` (false when the platform cannot report a worker's memory; then no cap can be enforced),
`peakBytes` (the most resident memory the worker reached), `capBytes` (the cap the run used, 0 for
none), `waits` (times the export held back a frame at the cap) and `waitedMs`. It is absent for an
image output, which has no worker.

Stopping the server or quitting stops the jobs an agent started this way, never jobs started with
`export.start`, which belong to the Export editor. Every tool, including render, export and
capture, works on Linux, macOS and Windows.

### Streamable HTTP

The endpoint is one path, `/mcp`. `POST` carries one JSON-RPC message (batches are refused with 400)
and needs `Content-Type: application/json` and an `Accept` that allows both `application/json` and
`text/event-stream`. `initialize` returns an `Mcp-Session-Id` that every later request carries;
`DELETE /mcp` ends the session; an ended or expired session answers 404 and the client initializes
again. A task tool (`render`, `export`, `capture`, `export.wait`) that carries
`_meta.progressToken` is answered as `text/event-stream`: `notifications/progress` events, then the
result. `GET` is 405: Bloom sends no server-initiated messages. Requests from a browser `Origin`, a
`Host` other than `127.0.0.1:<port>` or `localhost:<port>`, or without the token are refused (403
or 401) before anything else is read. Disconnecting does not cancel a request; send
`notifications/cancelled`.

## `bloom-mcp` on stdio

Start `bloom-mcp`, optionally with `--project project.bloom`. The server is Qt-free and uses
JSON-RPC 2.0 over newline-delimited UTF-8 stdin/stdout. It implements the MCP 2025-11-25 handshake
and tool protocol. Logs go to stderr.

A client configuration can launch it as:

```json
{
  "mcpServers": {
    "bloom": {
      "command": "bloom-mcp",
      "args": ["--project", "project.bloom"]
    }
  }
}
```

The client's working directory resolves relative project and output paths. The server opens one
process-local session at startup. Transactions change that session's command history; they do not
automatically save the `.bloom` document. Render/export publication writes the requested outputs.

The running application can also host the server itself; start it and manage its port and token
from the [MCP Server panel](mcp-server-panel.md).

## Connect and discover

Send `initialize` with `protocolVersion`, `capabilities` and `clientInfo`, then send the
`notifications/initialized` notification. `initialize` returns Bloom's version, facade version,
headless status, tool and resource support, the request limit and short `instructions` that point
an agent at `help` and the getting-started guide.

An agent never needs Bloom's source to use it:

- **`help`** with no arguments lists every tool, query kind, operation, `bloom-cli` command and
  Python entry point, grouped by area, each with a one-line summary. `help {"name": X}` shows one
  entry in full: what it does; every argument with its type, unit (frames, pixels, a 0..1 fraction,
  which id), range, default, the editor context that fills it when omitted, and the accepted values
  with their labels; side effects (read-only, one undo step, marks the project unsaved, writes
  files); prerequisites; the result shape; typed errors with what to do about each; examples for
  MCP, Python and `bloom-cli`; and related entries. `X` may be an operation id
  (`bloom.layer.add-solid`, or its Python spelling `bloom.ops.layer.add_solid`), a tool
  (`transact`), a query kind (`query:layers`), a command (`bloom-cli render`) or a concept
  (`concept:time`, `concept:ids`, `concept:revisions`, `concept:context`, `concept:errors`). An
  unknown name answers `found: false` with the closest names. `bloom-cli help` and `bloom.help()`
  print exactly the same text.
- **`tools/list`** descriptions and every input property's description come from the same
  reference, with units, ranges, defaults and accepted values.
- **Resources** (`resources/list`, `resources/read`): `bloom://guide/agents` (find the active
  composition, add a layer, animate it, render a frame to check, save), `bloom://reference/nodes`
  (every node type with its ports and parameters: value kinds, defaults, units, ranges, blend modes
  and other enumerations), `bloom://reference/operations` (the whole reference as one document),
  `bloom://reference/index.json`, and this guide, the scripting guide and the Python guide.

Times are integers or exact pairs everywhere: an integer is a frame count at the composition's
frame rate, and `[numerator, denominator]` is an exact time in seconds. Read-back reports every
time as `{frames, seconds: [n, d]}`, so a value can be written back in the unit it was read in.
Ranges with an `end` (a layer's range, the work area) are end-exclusive; render frames and export
job ranges are inclusive frame indices. Ids are stable positive integers, one space per kind: a
layer id is not the id of the layer's Layer Output node.

Use `tools/call` with `name` and `arguments`:

| Tool | Arguments and result |
| --- | --- |
| `help` | Optional `name`. Returns `found`, `text` (the readable entry or index), and `entry`, `index` or `suggestions` as data. |
| `query` | `kind`: project, compositions, layers, nodes, node-types, parameters, parameter, parameter-value, keyframes, assets, operations, context or enums (the interpolation, ease and Easy Ease vocabularies). layers, nodes, parameters, parameter, parameter-value and keyframes also need `composition`. `compositions` records carry size, `frameRate`, `fps`, `pixelAspect`, `duration` and `start` as `{frames, seconds}`, `dropFrame`, `workArea` (end exclusive, or null), `backgroundColor` (display-only), `workingColorSpace`, `safeAreas`, `layerCount` and `folder`. `layers` lists a composition's layers topmost first: `index`, `slot`, `merge`, `layerOutputNode`, `sourceNode`, `sourceType`, enabled, solo, locked, label colour, parent, `start` and exclusive `end`, `timing` (start time, speed, before and after modes, extent), masks, track matte, and the parameter ids of the layer's transform (`parameters`) and its source (`sourceParameters`). `nodes` records carry `name`, `category`, `parameters`, `position`, `width`, `collapsed`, `muted`, `group`, `inputs` (port, kind, required and the connected `source`) and `outputs` (port, kind and `targets`); Merge nodes add their `stack`; the result adds `links` and `groups`. `node-types` lists every type `bloom.node.add` accepts; with `nodeType` it returns one type with each parameter's metadata. A node's `category` (in `nodes` and `node-types`) is its group in the Add menu ([UI grammar](../ux/ui-grammar.md)): one of `sources`, `compositing`, `channels`, `keying-mattes`, `transform`, `distort`, `blur-sharpen`, `light-shadow`, `stylize`, `color-correction`, `color-management`, `time`, `text-paths`, `instances-particles`, `tracking-analysis`, `fields`, `values`, `math`, `convert`, `string`, `logic`, `time-values`, `color-values`, `vector`, `utilities` or `output`. Earlier builds reported `filters`, `grade`, `color` and `layers`, and reported the value nodes as `values`, `math` or `utilities` only. `parameter` with `parameter` returns the record and `metadata`: `valueKind`, `animatable`, `default`, `unit`, `displayScale`, `minimum`, `maximum`, `softMinimum`, `softMaximum`, `choices` (`{value, label}`; `bloom.parameter.set-source` takes either the value or the label, matched case-insensitively, and refuses anything else naming the valid choices) and `owner`; write values in stored units (the units of `minimum`, `maximum` and `default`), while `unit` and `displayScale` are how Properties shows them (an opacity stored as 0.5 shows as 50 %). `keyframes` with `parameter` (an animated one) or `curve` returns the keys sorted by time: `keyframe`, `component` (x, y, z, red, green, blue, alpha, or null), `time` as `{frames, seconds}`, `value`, `interpolation` (hold, linear, ease-in-out) and the ease handles, paged like parameters (`pageSize` 1–64, `nextCursor`). `parameters` without paging returns full records; with `pageSize` (1–64) and optional `cursor` it returns bounded summaries and `nextCursor`; constant values are explicitly marked `valueOmitted`. `parameter-value` with `parameter` ID and `expectedRevision` reads UTF-8 string chunks using optional byte `offset` and `limit` (4–131072). `assets` records carry `kind` as a name (image, sequence, audio, video, lut), `path`, size, `frames`, digest, `folder` and `tags`, and the result lists the Assets `folders` (`id`, `name`, `parent`); an image, sequence or video also carries its `interpretation` (the [Interpret Footage](interpret-footage.md) members `bloom.asset.set-interpretation` takes, plus the resolved `playbackFrameRate` and `filePixelAspect`). `layers` records also carry `shy`, `nameFollowsSource` and `sourceName`, and (every Merge entry is a layer, see [Merge entries and adjustment layers](#merge-entries-and-adjustment-layers)) `layerKind` (`node` for a Layer node, `entry` for a Merge entry's own layer, whose `layerOutputNode` is null and whose `merge` and `slot` own it), `entryLayer`, `pivot` (`content` or `frame`), `entries` (`[{merge, slot}]` of every entry it is the layer of), `shared`, `adjustment` (the Adjustment node serving its entry, or null) and `values` (`{role: value}`, null for an animated or driven control); the `layers` result adds `entries`, every Merge entry (output Merge first, each top to bottom) whether or not it has a layer yet: `merge`, `slot`, `index`, `outputMerge`, `resolvedLayer` (the entry's layer id, or null), `layerKind` (`node`, `entry` or `none`), `entryLayer`, `layerNode`, `sourceNode`, `sourceType`, `name`, `enabled`, `solo`, `locked`, `shy`, `pivot`, `adjustment`, `shared`, `nestedMerge` (the Merge it shows, for a nested Merge), `parameters` and `values` (an entry without a layer reports the defaults it renders with). A Merge node's `stack` rows (`nodes`) carry the same entry fields after `slot`, `layer` and `index`. A `parameter` of an entry layer reports `owner` `{layer, merge, slot, role}` and the Layer node's metadata. An operations record carries a `description`, every argument's `kind`, `required`, `contextual`, `context`, `summary`, `unit`, `minimum`, `maximum`, `default` and `choices`, plus a pasteable `example` call. `context` returns the editor context contextual arguments fall back to: project, composition, selected nodes and layers, time (seconds as `[n, d]`), the Viewer's display and view, the Viewer's drill `level` (entered group layers, outermost first) and `points` (the shape in point editing, or null), read-only state and `live` (false on stdio, which reports the session defaults, an empty level). |
| `transact` | `expectedRevision`, an `operations` array of `{op,args}`, and optional `label`. Returns one atomic command result, created IDs, structured diagnostics and `compileFailures` and `compileWarnings` (see [Edits and diagnostics](#edits-and-diagnostics)). Arguments the descriptor marks `contextual` may be omitted and are filled from the session (the live editor context in the desktop application). Coded failures carry `structuredContent.diagnostic` `{code, message}` with a `bloom.mcp.*` code. |
| `render` | `composition`, `frame` or inclusive `first`/`last`, `preset`, `destination`, optional `overwrite` (`ask`, the default, `replace` or `refuse`). Returns `publishedFrames`, `preservationReport` and `files` with SHA-256 digests. Output keeps alpha; the composition background colour is not rendered. With `loudnessLufs` the audio is normalized to that EBU R128 integrated loudness under the true-peak ceiling, and the result carries `loudness` (measured and delivered level and true peak, the gain, and the limiter's deepest reduction; a level that could not be measured, as in silence, is `null`). |
| `export` | Render arguments plus optional `profile`, `audio`, `sampleRate`, `loudnessLufs` (-40 to -5) with `truePeakDbtp` (-12 to 0, default -1), `encoder` (`automatic`, the default, `software` or `hardware`; on macOS `automatic` makes ProRes with the system's encoder and fails if it cannot, and `software` uses Bloom's FFmpeg encoder), `workerMemoryCapBytes` and `workerThreads` (see [Export worker limits](#export-worker-limits)). Uses the existing MEDIA-4 composition runner for ProRes preview, DNxHR and PCM output. Output keeps alpha; the composition background colour is not rendered. |
| `events` | `sinceRevision`, optionally `afterSequence`. Returns revision-inclusive events, a sequence cursor and `resetRequired` when retained history cannot cover the cursor. |
| `history` | `action` (`undo` or `redo`) and `expectedRevision`. One step of the project's history, refused with a stale-revision result when the revision moved. In the desktop application it is queued like a `transact`, and it undoes the most recent step of any origin, including the artist's. |
| `capture` | Optional `composition` and `frame` (defaults: the context composition at the context time; on stdio the session's composition at frame 0). Renders one PNG at composition resolution through the still export route, with the Viewer's display and view in the desktop application (not its exposure, gamma, channel, region of interest or preview resolution), and returns it as MCP `image` content. Output keeps alpha; the composition background colour is not rendered. A PNG larger than about 2.9 MiB is returned as a `resource_link` to the file with the notice `bloom.mcp.capture-too-large-for-inline`. Captures go to an `mcp-captures` folder in Bloom's per-user cache, which keeps the newest 16. |
| `export.start`, `export.start-all`, `export.pause`, `export.resume`, `export.stop`, `export.stop-all`, `export.retry`, `export.confirm-replace` | The export queue's session commands (`jobs`, `job`, `output`). Each returns `accepted`, the queue revision and, when refused, the availability code and reason. |
| `export.status`, `export.events`, `export.log`, `export.presets`, `export.script` | The queue snapshot (optionally `sinceRevision`), terminal events after `afterSequence`, a job's run log, the presets available on this computer and for the open project's color configuration (with `job`, also for that job's composition: when a source's audio cannot be decoded, presets that carry audio report `available: false` with the reason, and the same presets without audio stay available), and a script that recreates a job (`format` `cli-json` or `python`). In the snapshot, a published movie's output `verification` carries `reopenQuality` (`toleranceProfile`, `toleranceDigest`, `maximumError`, `percentileMaximumError`, `meanError`): the reopen check's tolerance profile and digest, the raw maximum (reported, not checked), the maximum after the top 0.01% of each frame's color samples are set aside, and the mean, in 16-bit units. Outputs without a tolerance profile omit it. A job's `progress` carries `stalled`; while a running job makes no progress (no frame, output step or destination check finishes) for 30 seconds, `stalled` is `true` and `progress.stall` gives `waitingOn` (`destination-check`, `viewer`, `gpu-capacity`, `gpu-recovery`, `gpu-preparation` or `worker`) and `idleMs`. `progress.elapsedMs` counts all running time except pauses, also while no frame completes. |
| `project.set-color-settings` | `config` (`neutral` Bloom Neutral v1, `aces` ACES 1.3 CG, or `current` to keep the project's own, the only choice for an external configuration) and optional `file` (an external OCIO config path; `projectRelative` makes it relative to the project folder; `context` sets its variables; `config` `clear` returns to Bloom Neutral) and optional `workingSpace` (neutral: `lin_rec709_scene`, shown as Linear Rec.709 (sRGB); aces: `ACEScg`, `ACES2065-1`, `Linear Rec.709 (sRGB)`, `Linear P3-D65`, `Linear Rec.2020`; omitted keeps the project's when the configuration stays, else the first). Project Settings > Color as the dialog applies it. One undo step in the history: it marks the project dirty and changes how every composition renders. Returns `code` `updated` or `no-change` with the resulting `config`, `workingSpace` (the stored id) and `workingSpaceLabel` (the name Project Settings shows); `invalid-config` and `invalid-settings` (a working space the configuration does not offer) return `succeeded: false`; a read-only project answers `bloom.mcp.read-only`. In the desktop application the window applies it (`bloom.mcp.ui-unbound` if it does not offer it). |
| `export.wait` | Optional `job` and `timeoutMs`: waits until the job (or the whole queue) is finished, with progress when the request carries a progress token. |

For example:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"query","arguments":{"kind":"operations"}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"transact","arguments":{"expectedRevision":0,"label":"Name study","operations":[{"op":"bloom.project.set-name","args":{"name":"Study"}}]}}}
```

Operation IDs, their argument schemas and their reference entries come directly from the
operation registry, so `help`, `query {kind:"operations"}`, Python and `bloom-cli` cannot disagree
about them. A revision conflict leaves the document unchanged. Tool results include both `structuredContent` and a JSON text content item;
`isError` distinguishes failed operations from successful tools. Invalid JSON-RPC or tool arguments
produce protocol errors with the request ID where valid.

`bloom.layer.align-layers` and `bloom.layer.distribute-layers` measure the layers' evaluated bounds
on the server before the transaction runs (the CPU reference evaluation of one frame), so a
`transact` call with them can take longer than other edits. Their `layers` argument is contextual
(`context` `"layers"`: the selected layers) and `time` defaults to the session time. Skipped layers
arrive as `bloom.scripting.operation-warning` diagnostics; when no layer can move, the transaction
fails with the reasons. See [Scripting Bloom with Python](python.md#align-and-distribute) for the
arguments.

Three more edits read the project when the operation is created, on the server and never on the
window's thread, and stop when the request is cancelled (`bloom.scripting.cancelled`):

- `bloom.asset.reload` (`asset`) is the Assets panel's **Reload Footage**: it probes the asset's own
  file again the way it was imported and updates its size, duration and digest in one undo step.
  A missing file is refused with the
  probe's reason (`bloom.asset.relink` points the asset at another file).
- `bloom.asset.relink` (`asset`, `path`, `projectDirectory`) accepts an absolute path or one
  relative to the project file's folder, even when the old file is gone. A saved project's own
  folder is the base for both the relative path and the stored locator (`projectDirectory` only
  stands in for an unsaved project), so the locator is stored project-relative like an import's. An
  unreadable path is refused with the reason and the absolute path that was tried.
- `bloom.layer.add-svg` (`composition`, `path`, optional `name`, `fit`, `projectDirectory` and
  `mode`) imports an SVG as one Vector Artwork layer (`mode` `"layer"`, the default) or as editable
  shapes (`"shapes"`). The file is read and parsed when the call is made, on the MCP thread, never on
  the application's interface thread. `bloom.layer.break-apart-svg` (`composition`, `layer`) turns a
  one-layer SVG into editable shapes in place, in one undo step, from the copy the project holds; a
  layer that holds no Vector Artwork is refused by name.
- `bloom.layer.set-anchor-preset` (`layer` contextual, `anchor` one of `top-left`, `top`,
  `top-right`, `left`, `center`, `right`, `bottom-left`, `bottom`, `bottom-right`, `time`
  contextual) is the anchor grid in Properties > Transform: it measures the layer's evaluated local
  bounds at `time` and moves the anchor point to that point of them. An animated anchor gets a key
  at the layer's own key time. A locked layer, a driven anchor or a layer with no visible bounds at
  the time is refused.
- `bloom.cryptomatte.pick` (`node`, `point` `[x, y]` in composition pixels as `capture` shows them,
  optional `mode` `add` or `remove`, `time` contextual) is the Viewer's Cryptomatte eyedropper: it
  maps the point through the layer that shows the node, reads the object with the most coverage
  there and adds it to (or removes it from) the node's matte list by its manifest name, or its raw
  ID without a manifest. A point outside the layer or over no object answers "No object under the
  point". To choose mattes by name, set the node's `mattes` text with
  `bloom.node.set-parameter-value`.
- `bloom.node.delete-unused` (`composition`) is the Node editor's Delete Unused button: it deletes
  every node with no path to the composition's Output (links, Merge slots, a layer's parent and
  matte sources, and drivers all count as a path) in one undo step, with Delete's rules: the Output,
  the Merge connected to it and the nodes of locked layers stay, and nothing unused is a no-change.
  In Python it is `bloom.ops.node.delete_unused()`.
- Any node's Mask output (a Cryptomatte, a Mask Combine, an Image to Mask) can shape a layer:
  `bloom.layer.add-matte-mask` (`composition`, `layer`, `node`, optional `port` (default `mask`),
  `mode` and `name`) adds a mask shaped by it, `bloom.layer.set-mask-matte` (`composition`, `layer`,
  `mask`, `node`, optional `port`) reshapes an existing mask by it and
  `bloom.layer.clear-mask-matte` returns the mask to its path. `bloom.layer.set-node-track-matte`
  (`composition`, `layer`, `node`, optional `port`, `channel` and `inverted`) mattes the layer by the
  node's Mask; `bloom.layer.clear-track-matte` clears it. The `layers` query reports a mask's
  `matte` as `{node, port}` (null for a path mask) and a track matte's `node` the same way.

A client that cannot read the project (no session) gets `bloom.scripting.source-unavailable` from
these three.

### Merge entries and adjustment layers

Every image fed into a Merge is a layer with the full layer controls
([Layer Graph Model](../architecture/layer-graph-model.md#merge)): its layer is the Layer node
nearest the Merge on its chain, or the entry's own entry layer, or none yet, in which case it
renders as an entry layer at its defaults (position the composition centre, anchor `[0, 0]`, scale
`[1, 1]`, rotation 0, opacity 1, Normal, pivot `frame`). `query` `kind` `layers` lists every entry
in `entries`, with its `merge` and `slot`.

- Every layer operation that takes `layer` also takes `merge` and `slot` instead (both, or
  neither). When the entry has no layer yet, its entry layer is created in the same undo step as
  the edit and the result also outputs `layer` and `parameter.<role>`. `layer` with `merge` and
  `slot` is refused, and the selected layer never fills `layer` when `merge` is given:
  `{"op":"bloom.layer.set-solo","args":{"merge":1,"slot":2,"solo":true}}`.
- `bloom.merge.set-entry-parameter` (`merge`, `slot`, `role`, `value`, optional `time`) sets one
  control of the entry's layer by role -- `position`, `anchor`, `scale`, `rotation`, `opacity`,
  `blendMode` (a value or a label such as `"screen"`) or `timeRemap` (only while Time Remap is on)
  -- whatever that layer is. Without `time` the parameter becomes the constant; with `time` it is
  keyed there. An entry layer's parameters are constants or curves; insert a Layer node to drive
  one from the value graph.
- `bloom.merge.ensure-entry-layer` (`merge`, `slot`) outputs the entry's `layer`, creating its
  entry layer (with `parameter.<role>` outputs) when it has none.
- `bloom.merge.insert-layer-node` (`merge`, `slot`) splices a Layer node into the entry; it adopts
  the entry layer's values in the same undo step, so the picture does not change. Outputs `node`,
  `layer`, `edge` and `parameter.<role>`.
- `bloom.merge.add-adjustment-layer` (`merge`, optional `above` slot) is the Timeline's Add >
  Adjustment Layer: an Adjustment node feeding a new entry directly above `above` (default: the
  top) with an entry layer named "Adjustment Layer N". Outputs `node`, `layerSlot` and `layer`.
  Connect filters to the Adjustment node's `image` output (`bloom.node.add-fed-by`); the entry
  replaces the stack below it, mixed by its opacity.

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"transact","arguments":{"expectedRevision":3,"operations":[{"op":"bloom.merge.add-adjustment-layer","args":{"merge":1}},{"op":"bloom.merge.set-entry-parameter","args":{"merge":1,"slot":2,"role":"opacity","value":0.5}}]}}}
```

### Parents and track mattes

A parented layer's `position`, `anchor` and `rotation` are measured in its parent's space: the origin
is the top-left corner of the parent's content (the frame, for a nested composition), and the child
moves, scales and rotates with the parent. A layer used as a track matte stops displaying in the
composite and only shapes its target; that is intended, and clearing the matte brings the layer
back. A matte layer may be a child of the layer it mattes.

## Edits and diagnostics

**Omitted arguments.** An operation argument set to `null` is the same as leaving it out, in
`transact` and JSON scripts as with `None` in Python: an optional argument takes its default
(`bloom.layer.set-parent` with `"parent": null` clears the parent) and a required one is reported
missing.

**Times.** Every time argument follows one contract, in `transact`, `editor.reveal`, `transport`,
JSON scripts and Python alike: an integer is a whole frame of the composition the call works on, at
its frame rate, and `[numerator, denominator]` is an exact time in seconds. At 24 fps,
`{"duration": 48}` is two seconds and `{"time": [1, 2]}` is frame 12. When a transaction changes a
composition's frame rate, later frame counts in it use the new rate.

**Parameter values.** `bloom.parameter.set-source` takes `value` in the parameter's own kind: a
number, `true`/`false` for a Boolean, text for a String, `[x, y]`, `[x, y, z]` or `[r, g, b, a]`,
and for an Integer or enumeration a whole number or the member's name (`"screen"`, `"Ping-pong"`;
case, spaces, hyphens and underscores do not matter, so `"ping_pong"` names Ping-pong). Reading an enumeration parameter back
(`query` `kind` `parameter`) returns both its stored `value` and its `label`. A Path parameter (a shape's outline) takes a path object, the same one
`bloom.layer.add-shape` (`kind` `"path"`) and the mask operations take: `{"closed": bool,
"anchors": [{"point": [x, y], "inHandle": [x, y] or null, "outHandle": [x, y] or null, "mode":
"corner" | "mirrored" | "aligned" | "free" | "auto"}]}`, where `mode` is optional and inferred from the
handles. A shape's outline and a Path node's value may also be compound (`docs/architecture/path-value.md`):
`{"contours": [{"closed": bool, "anchors": [...]}, ...], "fillRule": "nonZero" | "evenOdd", "groups":
[{"key": text, "firstContour": n, "contourCount": n, "color": [r, g, b, a]}]}`, with `fillRule`, `groups`
and each group's `color` optional; Python takes the same mapping with `fill_rule`, `first_contour` and
`contour_count`. A mask holds one contour. A value one contour can express is always read back in the
first form. A Gradient parameter takes `{"stops": [{"position": p, "color": [r, g, b, a]}]}` with
optional `"interpolation"` (`"constant"`, `"linear"`, `"ease"`, `"cardinal"`, `"b-spline"` or
`"exponential"`), `"colorMode"` (`"rgb"`, `"hsv"` or `"hsl"`) and `"hueDirection"` (`"near"`,
`"far"`, `"clockwise"` or `"counter-clockwise"`); one left out is Linear, RGB or Near. Python
takes the same mapping with `color_mode` and `hue_direction`, and reading a gradient back always
lists all three. A value out of range is refused with what the parameter
accepts, for example "Parameter 31 (points of node 7, bloom.shape) cannot take 2; it takes a whole
number in 3..64." A Boolean parameter such as a shape's `fillEnabled` or `strokeEnabled` can be
driven by a Boolean value node: `{"driverNode": <id>, "driverPort": "result"}` with
`bloom.scalar-to-boolean`, for example. A driver naming an output the node does not have, or one of
the wrong kind, is refused with the node's outputs. A text layer's `font` takes a font ID as a string, for example
`"source=google;family=Roboto;style=Bold;weight=700;italic=false"`: `source` is `system`, `google` or
`builtin`; `family` is required; `style`, `weight`, `italic`, `axes` (`tag:value` pairs separated by
`|`), `ps` (a system font's PostScript name) and `version` (a Google family version) are optional.
Anything that does not decode is refused with that form. `query` `kind` `parameters` and `parameter`
report, on a font parameter, `font` (the decoded members) and `fontStatus` (`available`, `downloading`,
`checking` or `missing`) and, for a missing font, the reason. There are no font assets:
`bloom.asset.ensure-font` and `bloom.asset.relink-font` no longer exist. `bloom.asset.import` of a file the project
already holds adds nothing and returns the existing asset's id as its `asset` output, one id per
imported file. `bloom.animation.set-keyframe-at-time-for-parameter``
on a constant parameter makes it animated in the same undo step, its constant becoming the first
key. The reverse is one step too: `bloom.parameter.set-source` with a `value` (or a driver) on an
animated parameter replaces the animation and removes its curve, as
`bloom.animation.convert-to-constant` does.

**Diagnostics.** A refused transaction applies nothing and lists why. Each diagnostic is
`{code, operationIndex, operation?, path?, message}`: `operationIndex` is the 0-based place in
`operations` of the operation that caused it (`null` for a stale revision or a history limit,
which belong to the whole transaction). Codes are `bloom.scripting.command-rejected` (an operation
refused), `bloom.scripting.validation-failed` (the edit would leave the project invalid; `path`
names where), `bloom.scripting.stale-revision`, `bloom.scripting.history-limit` and
`bloom.scripting.invalid-argument` (the registry refused an argument). An argument of the wrong
JSON type is a protocol error whose message starts with `operations[<index>] (<op>)` and gives a
working example.

**Compile failures.** After a successful edit, every composition it touched is compiled (never
rendered). `compileFailures` lists those that cannot render now:
`[{composition, code, detail, subject: {composition, node, parameter}}]`, where `detail` is one
plain sentence and `node`/`parameter` are `null` when the failure has no narrower subject. Each one
is also recorded as a `composition.compile-failed` event. An empty list means every touched
composition compiles.

`compileWarnings` lists what still compiles but degrades, in the same record shape: for example a
text layer whose font is not installed and cannot be fetched, which is
drawn in a fallback face chosen by category (`bloom.runtime.compile.font-missing`). A Google font
that is not installed or cached is downloaded into the per-user cache like the desktop application
does, and an export waits for it; offline, the font is reported missing and drawn in the fallback.

A transact that leaves an Apply LUT's Process Space, or a Color Space Transform's From or To, on a
color space the project's color configuration does not have still succeeds (a project moved to
another configuration must stay editable), and says so in `compileWarnings`: code
`bloom.runtime.evaluation.color-space-missing`, a `detail` naming the space, and the node and
parameter as the subject. The node leaves the image unchanged until a listed space is chosen.

**Events.** Besides command events (`kind` `revision`, `history` or `rejected`), `events` reports
`{kind, revision, message, composition?, subject?, path?, projectName?, sequence}` records for
`composition.compile-failed`, `project.saved`, `project.opened`, `project.created`,
`project.closed` and `project.copy-saved`.

Command events and every `transact` and `history` result also carry `origin`: `user` (the artist),
`agent` (an MCP call) or `script` (the Script editor or `bloom-cli`), so a client can tell its own
edits from the artist's.

For a large parameter set, start with `{"kind":"parameters","composition":1,"pageSize":64}`.
Repeat with its `nextCursor` until it is `null`. Each page includes `revision` and `totalRecords`;
the cursor binds the server session, project, composition, revision and next record index. A
malformed cursor or any intervening edit is an error, so restart at the first page after an edit.
Summaries identify omitted constants with `valueOmitted`; string summaries also report
`valueBytes`. Read one parameter with `{"kind":"parameter","composition":1,"parameter":42,
"expectedRevision":3}`. For an oversized constant string, use `kind:"parameter-value"` with
the same IDs and revision, starting at `offset:0` and advancing to each `nextOffset` until
`complete:true`. Chunks report `totalBytes` and always end on a UTF-8 boundary; no value is
silently shortened. An `expectedRevision` mismatch is an error.

**Revisions.** A project's revision counts the edits made since it was opened or created.
`project.open` and `project.new` start it again at 0, whatever revision the file was saved at; it
is not kept increasing per file. Read the revision again after replacing the project, and treat an
events cursor that reports `resetRequired` as void.

**Ids.** Every id argument takes a positive integer or the same value as decimal text (`16` or
`"16"`). A Font, video, image or audio asset parameter holds the asset's id as text, and
`bloom.parameter.set-source` accepts the integer `query` reports for the asset as well.
`bloom.asset.import` needs `projectDirectory` only when a path is relative; with absolute paths it
uses the saved project's folder. A project that has never been saved is supported: an absolute path
imports, the asset keeps an external locator (the file's absolute location, not copied), and it
stays valid after Save As. A relative path in that project is refused, with nothing imported, unless
`projectDirectory` is given.

**Export destinations.** An output's destination is a path below the project folder
(`portability` `project-relative`) or an absolute local path such as `/exports/shot.mov`, which is
stored as `portability` `external` with its `file://` URI. `bloom.export.set-output-destination`
and the `destination` of an `outputs` entry of `bloom.export.add-job` take either; a file URI with
`portability` `external` is still accepted.

**Names and roles.** Where an operation takes a `layer` (or `layers`, or a layer operation's
`parent`), pass `{"name": "Shot 2"}` instead of the id. Where it takes a `parameter`, pass
`{"layer": "Shot 2", "role": "opacity"}` (`layer` is a name or an id); the layer's own parameters
are searched first, then its source's. An optional `composition` in the reference scopes the
lookup; the default is the operation's composition. A name that matches no layer or several lists
the candidates with their ids, and a role the layer lacks lists the roles it has. Ids still work
everywhere.

**Readings.** Beyond the project records, `query` answers measurements and window state, all
through the same route (`help` lists them with their arguments):

- `kind:"text-metrics"` with `text`, optional `font` (a font ID or a built-in face's name) and `size`
  (pixels per em) returns the advance and the ink bounds per line. A font ID is measured in the face a
  render would use now, and `font` in the answer says when that is a fallback or still downloading.
- `kind:"layer-bounds"` with `composition`, `layer` and optional `time` returns the evaluated
  bounds: `local` (the content rectangle before the layer transform), `corners` (four corners after
  it) and `output` (their axis-aligned box), with the space stated in the answer; a time outside the
  layer reads `visible:false` with a reason.
- `kind:"history"` lists the undo history (labels, the current position, `canUndo`, `canRedo`).
- `kind:"viewer"` and `kind:"preview-cache"` read the Viewer state and the RAM preview (filled
  range, fill rate, budget, stop reason; what is not known reads `null`). The viewer state always
  carries `level` and `points`, the drill level, even with no Viewer showing. They answer
  `bloom.mcp.not-live` on stdio.
- `kind:"jobs"` lists the background jobs that are not exports (imports, previews, thumbnails,
  cache purges): `id`, `name`, `kind` (the part of Bloom that owns the job), `state` (`queued`,
  `running`, `succeeded`, `cancelled` or `failed`), `progress` (`phase`, `completed`, `total`; the
  counts read `null` for a job that reports none) and `failure`, the reason a failed job recorded
  (`null` otherwise). Export runs are read with `export.status`. It answers `bloom.mcp.not-live`
  on stdio.
- `kind:"pixels"` with `region` `[x, y, width, height]` (a point is `[x, y, 1, 1]`, at most 256
  pixels), and optional `composition` and `time`, returns pixel values at a time without exporting
  a file. The frame is rendered as a still export renders it, to a scratch OpenEXR that is deleted
  before the answer returns, and the numbers are read from it, so they match what an export
  writes. It evaluates the whole frame, off the application's interface, and stops when the call
  is cancelled. `colorSpace` names the space of the values: they are scene-linear in the project's
  working space, with no display transform and no clamping, and red, green and blue are
  premultiplied by alpha. The answer also reports the route that rendered the frame (see below).
- `kind:"render-route"` says which route produced the Viewer's displayed frame: `scene` and
  `display` are each `gpu` or `cpu` as the frame itself recorded, `provider` names the display
  arm, `why` is the reason the frame recorded for running part of it on the CPU, and
  `cpuFallbacks` is the running count of preview requests that took the CPU path, by the reason
  the GPU preview service recorded. What was not recorded reads `null`; a CPU frame is never
  reported as GPU. It answers `bloom.mcp.not-live` on stdio.

An operations record also lists `outputs` (the ids the result carries, by name and kind) and
`sideEffects` (for example that connecting a node replaces the existing edge, or that repeated ids
in `bloom.node.duplicate` collapse). Data-block `typeId` and `mediaType` rules are in the
`bloom.data-block.create` entry. `script.run` runs its source as a whole program, so multi-line
code works.

**Filters and projection.** `query` accepts `idPrefix` on `kind:"operations"` (for example
`"bloom.layer."`), `layer` on `kind:"nodes"` and `kind:"layers"` (a layer id: its nodes and
everything upstream of it, with `links` and `groups` narrowed to them), and `fields` on any query
that returns `records`: a list of the member names to keep. A name no record carries is refused,
listing the members the records have.

**Bulk keys.** For hundreds of keys, `bloom.animation.set-key-columns` takes columns instead of rows:
`times`, `values` and optionally `interpolations` (one entry per time, missing ones linear), plus
`replace`, with the same units and refusals as `set-keys`. A column, or any other operation
argument, can be `{"fromFile": "/absolute/path/data.json"}`: the server reads that JSON file (a
regular `.json` file of at most 16 MiB and 2 million values, at most 16 per call), so the numbers
never pass through the conversation. Add `"response": "summary"` to `transact` and the answer
carries `outputSummary` (`count`, `byName` and a SHA-256 `digest` of the full id list) instead of
listing every created id. Python and `bloom-cli` have the same operation.

**Keys.** `bloom.animation.set-keys` replaces or edits a parameter's keys in one undo step: `keys`
is a list of `[time, value]` or `[time, value, interpolation]` entries in any order, and `replace`
clears the existing keys first (an empty list with `replace` returns the parameter to a
constant). The time of every key operation counts in the unit of the parameter's owner: a layer's
own parameters (position, opacity and the other Layer Output parameters) in layer time, which
is the composition frame minus the layer's start; every other parameter in composition time. A
key that lands outside the layer's start and end is applied and answers a
`bloom.scripting.operation-warning` naming the layer and its range.

**Duplicating a composition.** `bloom.composition.duplicate` returns the new composition as
`composition` and, for every copied object, the new id under `<kind>.<original id>`
(`layer.30`, `node.21`, `edge.50`, `parameter.60`, `slot.40`, `mask.7`, `curve.5`,
`keyframe.9`, `group.2`), so edits to the copy can address the same layers and nodes.

**Frame statistics.** `capture` with `statistics:true` returns measurements instead of a picture:
luma (Rec.709 weights of the working-space values) and red, green and blue mean, min, max and a
histogram (`bins`, 2 to 256, over 0 to 1, with `below` and `above` counting values outside),
the mean alpha, and the pixel count. The frame is rendered by the same still-export route as
`render`, written to a scratch OpenEXR in the project's working colour space without a display
transform, and read back; the result names that space in `colorSpace`. `last` makes a range
(`frame` to `last`, with a row of means and luma extremes per frame in `frames`) and `region` is
`[x, y, width, height]` in pixels from the top-left corner. Values are premultiplied, so a
transparent pixel counts as black.

**Other project files.** `query`, `transact`, `history`, `render`, `export`, `capture`,
`project.save` and `project.close` take `projectFile`, the absolute path of a project file other
than the open one. The call runs in a headless session of that file, with the same operations;
the session stays open between calls (edits, undo and unsaved changes persist), `project.save`
with `projectFile` writes it and `project.close` releases it (`discardChanges:true` to close with
unsaved edits). Asking for a file that is open in Bloom, or is the server's own project, is
refused with `bloom.mcp.preflight-blocked`, because two sessions on one file would overwrite each
other's saves. The question is asked again on every call, `project.save` included, so a file the
artist opens in Bloom after its session opened is refused from then on. When Bloom is busy and
cannot say within two seconds whether the file is open there, the call is refused
(`bloom.mcp.project-unavailable`, "Bloom is busy ...") instead of assuming it is free. While a
session holds a file, `project.open`, `project.save-as` and `project.save-copy` onto that path are
refused with `bloom.mcp.busy`; close the session first. At most four files are open this way at
once. In the desktop application their renders use the application's own final-render provider,
only for these calls; the open project keeps rendering through the export queue. A render, export
or capture reports `gpuFrames`, `cpuReferenceFrames` and `provenance` (`gpu` or `cpu-reference`),
for a project file and for the project a `bloom-mcp` process has open alike; when frames ran on
the CPU reference path, because no GPU device was available or the scene could not run on it,
`routeNote` says so. In the desktop application the render, export, capture and `pixels` of the
open project report the same fields from what the export queue recorded for the job, and read
`null` when it recorded none.

**Dialogs.** While a modal dialog is open in the application, the calls that need the window
answer `bloom.mcp.busy` and name the dialog: its title, its text and its buttons, with the
buttons that discard or overwrite data marked as such. `dialog.answer` presses one of those buttons
by its text (`{"button": "Cancel"}`, case-insensitive). It is off by default: the artist turns on
**Let agents answer open dialogs** in Edit > Preferences > General > Agents, and `preferences.set`
cannot do it for them (it refuses the key, which `preferences.get` lists as `agentsAnswerDialogs`).
With the switch off nothing is pressed and the call answers `bloom.mcp.not-allowed`. The dialog's
own action happens when the button is pressed, and Undo cannot restore what a discarding button
throws away, so read the marks first; the answer repeats the warning when the button you pressed
discards or overwrites data.

**Python in the running session.** `script.run` runs Python source through the same interpreter,
queue and live session as the Script editor, so the script's `bloom` edits are the same commands
with the same undo history as every other edit, and the source and output also appear in the
Script editor. It is a separate, opt-in capability, off by default, because Python can do
anything the user account can: turning it on is giving whoever holds the MCP token access to the
computer. The artist switches **Let agents run Python scripts** on in Edit > Preferences > General >
Agents (`preferences.set` cannot; `preferences.get` shows `agentsRunScripts`); with it off the call
answers `bloom.mcp.not-allowed` and nothing runs. It is bounded: at most 65536 characters of source,
`timeoutSeconds` (default 60, at most 600) after which the script is canceled as the editor's
**Cancel** does, 65536 characters of output, and the request's own cancellation also cancels the
script. While it runs the request reports progress when the client sent a progress token. One
script runs at a time; others queue (up to 16). On stdio there is no application window and the
call answers `bloom.mcp.not-live`. A script error answers `bloom.mcp.failed` with the error text in
`output`.

## Output and cancellation

`render` and `export` write to `destination`: a file path with the preset's extension (`.png`,
`.exr`, `.tiff` or `.tif`, `.mov`, `.mxf`, `.wav`), a path without an extension (the preset's is
added), or an existing folder (the file is then `frame.<ext>`, or `render.<ext>` for media). A
range writes `<base>.NNNN.<ext>`; a still destination may instead carry one `####` or `{frame}`
where the frame number goes (`seq.####.png` writes `seq.0000.png`, `seq.0001.png`, ...). A wrong
extension is an argument error saying which one the preset needs. An existing file is replaced
only with `overwrite:"replace"`; every file a range would write is checked, frame by frame. On
stdio, which has no confirmation, `ask` refuses like `refuse`. A request the export checks
refuse (an existing file without `overwrite:"replace"`, for example) answers
`bloom.mcp.preflight-blocked` with the reason, and a file that appears or changes after the check
is not replaced either; `bloom.mcp.export-unavailable` means export itself
cannot run here, and "Export isn't available in this window yet." means the window has no export
queue.

`render`, `export` and `capture` run as transient export jobs in the desktop application. Their
results, on success and on failure, carry `job` (the transient job id) and `log` (the run's newest
64 log entries, each `{sequence, sinceStartMs, severity, code, frame, message}` as `export.log`
gives them), so the detail a failure points to is in the answer itself. A capture carries them
too; a capture on `bloom-mcp` renders without a queue job and has neither. While such a job runs,
`export.status` lists it in `jobs` with `transient: true`. The queue retires the job when the call
returns, but Bloom keeps the newest 32 finished ones: `export.status` lists them in
`recentTransientJobs`, and `export.log` and `export.wait` still read each one by its id. Opening or
creating another project releases them.

Export job and output ids (`export.status`, `export.events`, the `job` of a failure, `jobs.list`'s
`exportJob`) are reported as decimal strings, such as `"4611686018427387905"`: a transient job's id
is above 2^53, which a JSON client that parses numbers as doubles cannot hold exactly. Pass the
string back as `job` or `output` (or in the `jobs` list of `export.start`) to `export.start`,
`export.pause`, `export.resume`, `export.stop`, `export.retry`, `export.confirm-replace`,
`export.log`, `export.wait`, `export.presets`, `export.script` and `export.reveal`. A plain integer
is still accepted where it is exact (a client that sends the literal digits unchanged).

Output keeps alpha: a composition's `backgroundColor`
(`bloom.composition.set-background-color`; `query` with `kind` compositions reads it back as
straight `[red, green, blue, alpha]` display values) is the Viewer's display backdrop only, so `render`,
`export` and `capture` never composite it. Where nothing covers the frame, a still stays
transparent exactly as a sequence or a video format that carries alpha does, on the GPU route and
the CPU reference alike. For an opaque backdrop in the output, add a full-frame solid layer at the
bottom of the composition.

`preset` takes the ids that `export.presets` and the queue operations use. `render` takes the
still presets `builtin:png-sequence`, `builtin:exr-sequence` and `builtin:tiff-sequence`. `export`
takes every preset `export.presets` lists as available on this computer, built-in or your own:
`builtin:h264-mp4`, `builtin:prores-422hq-mov`, `builtin:dnxhr-hq-mxf`, `builtin:wav` and the
rest. Each row of `export.presets` carries the preset's `extension`, which the destination must
end in. Availability also follows the open project: a preset that uses ACES names is listed as
unavailable, with the reason, until the project uses the built-in ACES configuration. A preset
that is listed but not available here is refused with the reason the list gives;
an id that is not listed is refused as unknown. The older names `PngRgba8SrgbV1`,
`FlatExrRgba32fLinRec709SceneV1`, `TiffRgba16SrgbV1`, `ProResMovV1`, `DnxhrMxfV1` and `PcmWavV1`
name six of the same presets and are still accepted. In `destination`,
`{name}` (the composition name), `{job}`, `{format}` and `{date}` expand as they do for a queued
output, and a name with any other `{...}` token is refused instead of written. A frame, or a range
whose first and last frames are the same, writes one file named as given; a longer range of a still
preset needs one `####` or `{frame}`. ProRes made by Bloom's FFmpeg
encoder retains its non-authorized preview status; on macOS the system's encoder makes ProRes
unless `encoder` is `software`. Worker exports run on Linux, macOS and Windows. See [Exporting video](exporting-video.md)
for profiles and preservation limits. The destination directory must exist.

Send `notifications/cancelled` with `params.requestId` matching the active tool request to cancel
that request's own work; other work keeps running. The bounded input reader continues receiving notifications during output work.
Completed image-sequence frames remain published; media publication retains the runner's staged
verification and cancellation behavior. Export uses the requested preset and approves the resulting
host analysis digest; the returned report preserves implementation notes.

On both transports each request is at most 1 MiB, each response at most 4 MiB, at most 65,536 JSON
values, and JSON depth at most 32. Arrays have no separate item cap. Parsing and writing use fixed 32 MiB allocation arenas. The pending request queue holds
at most 16 messages. Duplicate decoded keys, malformed Unicode, non-finite JSON numbers, invalid ID
types and unknown schema members are rejected. A render request is limited to 10,000 frames; event
history retains 1024 records. Clients should advance `afterSequence` and re-query the snapshot when
`resetRequired` is true. This local process uses its launcher's filesystem permissions.

Protocol references: [transports (stdio and Streamable HTTP)](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and [tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).
